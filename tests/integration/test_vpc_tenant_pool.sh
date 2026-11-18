#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; SKIPPED=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

skip_live() { echo -e "${YELLOW}[SKIP]${NC} live only — $*"; SKIPPED=$((SKIPPED+1)); }
skip_all() { echo -e "${YELLOW}[SKIP]${NC} VPC 테넌트 주소 풀 E2E: $*"; exit 0; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_VPC_POOL_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

LIVE=0
[ "${PCV_VPC_POOL_LIVE:-0}" = "1" ] && LIVE=1

TA='ta'; TB='tb'; PROBE_T='pcvp-probe'
POOL_TA='10.253.96.0/23'
A2A='10.253.96.0/24'
RESERVED_CIDR='10.253.98.0/24'
A2C='10.253.97.128/25'
BOB_CIDR='10.253.96.0/25'
POOL_TB='10.253.96.0/24'
SHRINK='10.253.97.0/24'
ADMIN_OUT='10.253.99.0/24'
ALICE='pcvp-alice'; BOB='pcvp-bob'; NOB='pcvp-nob'
USER_PW='PcvpUserPw-not-for-prod-1'
ADMIN_PW='PcvpAdminPw-not-for-prod-1'
POOL_MSG="Permission denied: subnet CIDR is outside this tenant's address pool"
RESERVED_MSG='Permission denied: subnet CIDR overlaps a reserved host management range'
TENANT_MSG='이 작업에는 tenant가 필요합니다'
PORT=0

for tool in jq sqlite3 curl python3; do
    command -v "$tool" >/dev/null 2>&1 || skip_all "$tool 미설치"
done

STATE=""
SOCK=""; BASE=""; AUDIT_DB=""

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

uds() {
    local req
    req="$(jq -nc --arg m "$1" --argjson p "$2" '{jsonrpc:"2.0",method:$m,params:$p,id:"vpcp"}' 2>/dev/null)" || return 0
    printf '%s' "$req" | pcv_iso_uds_nc "$SOCK" "${3:-30}" 2>/dev/null || true
}
if [ "$LIVE" = "1" ]; then
    SOCK="${PCV_VPC_POOL_LIVE_SOCK:-/var/run/purecvisor/daemon.sock}"
    BASE="${PCV_VPC_POOL_LIVE_REST:-http://127.0.0.1:8080/api/v1}"
    AUDIT_DB="${PCV_VPC_POOL_LIVE_AUDIT_DB:-/var/lib/purecvisor/pcv_audit.db}"
    [ -S "$SOCK" ] || skip_all "라이브 UDS 없음($SOCK)"

    for t in "$TA" "$TB" "$PROBE_T"; do
        nv="$(uds vpc.list "$(jq -nc --arg t "$t" '{tenant:$t}')" 10 | jq -r '.result | if type=="array" then length else "ERR" end' 2>/dev/null)"
        np="$(uds vpc.pool.list "$(jq -nc --arg t "$t" '{tenant:$t}')" 10 | jq -r '.result | if type=="array" then ([.[].cidrs[]] | length) else "ERR" end' 2>/dev/null)"
        if [ "$nv" != "0" ] || [ "$np" != "0" ]; then
            echo -e "${RED}[FAIL]${NC} 라이브 시작 전제: 테넌트 $t 의 VPC '${nv:-?}'개·풀 CIDR '${np:-?}'개(둘 다 0이어야 한다) — 시험을 거부한다"; exit 1
        fi
    done
else
    command -v bwrap  >/dev/null 2>&1 || skip_all "bwrap(bubblewrap) 미설치"
    command -v setsid >/dev/null 2>&1 || skip_all "setsid 미설치"
    pcv_iso_capable || skip_all "$PCV_ISO_SKIP_REASON"
    if [ ! -x "$DAEMON_BIN" ]; then
        info "데몬 바이너리 없음 — make daemon 시도"
        make -C "$REPO" daemon >/dev/null 2>&1 || true
    fi
    [ -x "$DAEMON_BIN" ] || skip_all "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
    pcv_iso_check_bin "$DAEMON_BIN" || skip_all "$PCV_ISO_SKIP_REASON"

    pick_free_port() {
        local p
        for p in 28097 29097 31097 33097 37097 41097 43097; do
            if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then echo "$p"; return 0; fi
        done
        return 1
    }
    PORT="$(pick_free_port)" || skip_all "빈 포트 확보 실패"
    BASE="http://127.0.0.1:$PORT/api/v1"
    STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-vpcp.XXXXXX")"
    pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
    mkdir -p "$STATE/var-lib" "$STATE/etc"
    AUDIT_DB="$STATE/var-lib/pcv_audit.db"
    SOCK="$STATE/var-lib/daemon.sock"
fi
if [ "$LIVE" = "1" ]; then
    xcurl() { curl "$@"; }
else
    xcurl() { pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" curl "$@"; }
fi
rest() {
    if [ -n "${4:-}" ]; then
        xcurl -s --max-time 30 -X "$2" -H "Authorization: Bearer $1" \
            -H 'Content-Type: application/json' -d "$4" "$BASE/$3" 2>/dev/null || true
    else
        xcurl -s --max-time 30 -X "$2" -H "Authorization: Bearer $1" "$BASE/$3" 2>/dev/null || true
    fi
}
login_token() {
    xcurl -s --max-time 6 -X POST "$BASE/auth/token" -H 'Content-Type: application/json' \
        -d "$(jq -nc --arg u "$1" --arg p "$2" '{username:$u,password:$p}')" 2>/dev/null \
        | jq -r '.access_token // empty' 2>/dev/null || true
}

err_code() { printf '%s' "$1" | jq -r '.error.code // empty | tostring' 2>/dev/null; }
err_msg()  { printf '%s' "$1" | jq -r '.error.message // empty' 2>/dev/null; }
res_q()    { printf '%s' "$1" | jq -r "(if has(\"result\") then .result else .data end) | $2" 2>/dev/null; }
job_id_of() { printf '%s' "$1" | jq -r '(.result.job_id // .data.job_id // empty)' 2>/dev/null; }
is_accepted() {
    [ -z "$(err_code "$1")" ] && [ -n "$(job_id_of "$1")" ]
}
brief() { printf '%s' "$1" | head -c 300; }
wait_job() {
    local r st _ lim="${2:-30}"
    for _ in $(seq 1 $((lim * 4))); do
        r="$(uds jobs.get "$(jq -nc --arg j "$1" '{job_id:$j}')" 10)"
        st="$(printf '%s' "$r" | jq -r '.result.status // .result.job.status // empty' 2>/dev/null)"
        case "$st" in completed|failed) printf '%s' "$r"; [ "$st" = completed ]; return ;; esac
        sleep 0.25
    done
    return 1
}
job_result() {
    printf '%s' "$1" | jq -c '(.result.result // .result.job.result // empty) | if type=="string" then (fromjson? // .) else . end' 2>/dev/null
}

vpc_job_ids() {
    uds jobs.list '{"limit":500}' 15 | jq -c '(.result | if type=="array" then . elif type=="object" then (.items // .data // .jobs // null) else null end)
        | if type=="array" then [.[] | .job_id] else "ERR" end' 2>/dev/null || echo '"ERR"'
}
new_vpc_jobs() {
    local before="${1:-}" now
    now="$(uds jobs.list '{"limit":500}' 15)"
    [ -n "$before" ] && [ "$before" != '"ERR"' ] || { echo "?"; return; }
    printf '%s' "$now" | jq -r --argjson b "$before" '(.result | if type=="array" then . elif type=="object" then (.items // .data // .jobs // null) else null end)
        | if type=="array" then [.[] | select((.job_id as $j | $b | index($j)) | not)
                                   | select(((.method // .type // "") | tostring | startswith("vpc.")))] | length
          else "?" end' 2>/dev/null || echo "?"
}

pcvp_vpc_ids() {
    local r; r="$(uds vpc.list "$(jq -nc --arg t "$1" '{tenant:$t}')" 10)"
    if [ "$(printf '%s' "$r" | jq -r '.result | type' 2>/dev/null)" != "array" ]; then echo ERR; return 1; fi
    printf '%s' "$r" | jq -r '.result[] | select((.name // "") | startswith("pcvp-")) | .id'
}
vpc_rev() {
    uds vpc.list "$(jq -nc --arg t "$1" '{tenant:$t}')" 10 \
        | jq -r --arg id "$2" '[.result[]? | select(.id==$id) | .revision][0] // 1' 2>/dev/null || echo 1
}
subnet_rows() {
    local r; r="$(uds vpc.subnet.list "$(jq -nc --arg t "$1" --arg v "$2" '{tenant:$t,vpc_id:$v}')" 10)"
    if [ "$(printf '%s' "$r" | jq -r '.result | type' 2>/dev/null)" != "array" ]; then echo ERR; return 1; fi
    printf '%s' "$r" | jq -c '.result'
}
pool_cidrs() {
    uds vpc.pool.list "$(jq -nc --arg t "$1" '{tenant:$t}')" 10 \
        | jq -c '.result | if type=="array" then ([.[].cidrs[]]) else "ERR" end' 2>/dev/null || echo '"ERR"'
}

POOL_SET_CALLS=0
POOL_R=""
pool_set() {
    local t="$1"; shift
    POOL_SET_CALLS=$((POOL_SET_CALLS+1))
    POOL_R="$(uds vpc.pool.set "$(jq -nc --arg t "$t" '{tenant:$t, cidrs:$ARGS.positional}' --args "$@")" 15)"
}

POOL_TENANTS="$TA $TB $PROBE_T"

aq() { sqlite3 -cmd ".timeout 5000" "$AUDIT_DB" "$1" 2>/dev/null; }
max_id() { local v; v="$(aq 'SELECT COALESCE(MAX(id),0) FROM audit_log;')"; echo "${v:-0}"; }
audit_count() {
    local c; c="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $1 AND $2;")"; echo "${c:-0}"
}
wait_audit() {
    local c=0 _
    for _ in $(seq 1 60); do
        c="$(audit_count "$1" "$2")"
        [ "$c" -ge "$3" ] && break
        sleep 0.25
    done
    echo "$c"
}

audit_exact() {
    wait_audit "$1" "$2" "$3" >/dev/null
    sleep 1
    audit_count "$1" "$2"
}

is_uuid() { local re='^[0-9a-f-]{36}$'; [[ "${1:-}" =~ $re ]]; }
dump_audit() { aq "SELECT id||'|'||IFNULL(username,'NULL')||'|'||method||'|'||IFNULL(target,'')||'|'||result||'|'||IFNULL(error_code,'') FROM audit_log WHERE id > $1 ORDER BY id;" | sed 's/^/        /'; }

kill_daemon() {
    [ -n "$STATE" ] && [ -f "$STATE/bwrap.pid" ] || return 0
    local bp; bp="$(cat "$STATE/bwrap.pid" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    local _
    for _ in $(seq 1 12); do kill -0 "$bp" 2>/dev/null || break; sleep 0.25; done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$STATE/bwrap.pid"
}
rpc_job_done() {
    local r jid
    r="$(uds "$1" "$2" 15)"; jid="$(job_id_of "$r")"
    [ -n "$jid" ] && wait_job "$jid" 60 >/dev/null 2>&1
    return 0
}
delete_vpc_tree() {
    local t="$1" vid="$2" aid sid
    for aid in $(uds vpc.attachment.list "$(jq -nc --arg t "$t" --arg v "$vid" '{tenant:$t,vpc_id:$v}')" 10 | jq -r '(.result // [])[]? | .id' 2>/dev/null); do
        rpc_job_done vpc.attachment.delete "$(jq -nc --arg t "$t" --arg a "$aid" '{tenant:$t,attachment_id:$a}')"
    done
    for sid in $(uds vpc.subnet.list "$(jq -nc --arg t "$t" --arg v "$vid" '{tenant:$t,vpc_id:$v}')" 10 | jq -r '(.result // [])[]? | .id' 2>/dev/null); do
        rpc_job_done vpc.subnet.delete "$(jq -nc --arg t "$t" --arg s "$sid" '{tenant:$t,subnet_id:$s}')"
    done
    rpc_job_done vpc.delete "$(jq -nc --arg t "$t" --arg v "$vid" '{tenant:$t,vpc_id:$v}')"
}
USERS_MADE=""
CLEANUP_RESIDUE=0

cleanup_resources() {
    local t vid n=0 left="" lt p bad=""
    for t in "$TA" "$TB"; do
        for vid in $(pcvp_vpc_ids "$t" | grep -v '^ERR$'); do delete_vpc_tree "$t" "$vid"; n=$((n+1)); done
    done
    for t in $POOL_TENANTS; do
        uds vpc.pool.set "$(jq -nc --arg t "$t" '{tenant:$t,cidrs:[]}')" 15 >/dev/null
    done
    info "cleanup: pcvp-* VPC ${n}개 정리 시도, 풀 비우기: $POOL_TENANTS"
    for t in "$TA" "$TB"; do
        lt="$(pcvp_vpc_ids "$t")" || lt="ERR"
        [ -n "$lt" ] && left="$left $t:$(printf '%s\n' "$lt" | grep -c .)"
        [ "$lt" = "ERR" ] && left="$left $t:ERR"
    done
    for t in $POOL_TENANTS; do
        p="$(pool_cidrs "$t")"; [ "$p" = "[]" ] || bad="$bad $t:$p"
    done
    if [ -z "$left" ] && [ -z "$bad" ]; then
        echo -e "${GREEN}[PASS]${NC} cleanup: pcvp-* VPC 잔재 0, 시험 풀($POOL_TENANTS) 빈 목록"
    else
        echo -e "${RED}[FAIL]${NC} cleanup: pcvp-* VPC 잔재 '${left:- 0}', 비우지 못한 풀 '${bad:- 없음}' — 데몬에 시험 자원이 남았다"
        CLEANUP_RESIDUE=1
    fi
}
cleanup() {
    local rc=$? u
    trap - EXIT INT TERM
    if [ -n "$(uds vpc.status '{}' 5)" ]; then
        cleanup_resources
        for u in $USERS_MADE; do
            uds auth.user.delete "$(jq -nc --arg u "$u" '{username:$u}')" 10 >/dev/null 2>&1 || true
        done
    fi
    if [ "$LIVE" = "0" ]; then
        kill_daemon
        [ -n "$STATE" ] && rm -rf "$STATE" 2>/dev/null
    fi
    if [ "$CLEANUP_RESIDUE" = "1" ] && [ "$rc" = "0" ]; then rc=1; fi
    exit "$rc"
}
trap cleanup EXIT INT TERM

write_conf() {
    cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $ADMIN_PW
jwt_secret = vpcp-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1

[vpc]
reserved_cidrs = $RESERVED_CIDR
EOF
}
boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PCV_LIBVIRT_URI test:///default \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local _ resp
    for _ in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            resp="$(uds vpc.status '{}' 10)"
            case "$resp" in
                *'"jsonrpc"'*|*'"result"'*|*'"error"'*)
                    pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE/health" >/dev/null && return 0 ;;
            esac
        fi
        sleep 0.5
    done
    return 1
}

TOK_ALICE=""; TOK_BOB=""; TOK_NOB=""
BASE_ID=0
BASE_VPC_ID=""
A2A_DONE=0; A2C_DONE=0; A5_LISTED=0

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  VPC 테넌트 주소 풀 E2E ($([ "$LIVE" = 1 ] && echo 라이브 || echo 격리) 모드)${NC}"
echo -e "${CYAN}  base=$BASE state=${STATE:-live}${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if [ "$LIVE" = "0" ]; then
    write_conf
    if ! boot; then
        fail "S1: 격리 데몬 기동 실패"; tail -30 "$STATE/daemon.log" 2>/dev/null; exit 1
    fi
    pass "S1: 격리 데몬 기동 + UDS 응답 + REST /health 준비"
else
    if [ -n "$(uds vpc.status '{}' 10)" ]; then pass "S1: 라이브 데몬 UDS 응답"; else fail "S1: 라이브 UDS 무응답"; exit 1; fi
fi
make_user() {
    local p r
    if [ -n "${2:-}" ]; then
        p="$(jq -nc --arg u "$1" --arg pw "$USER_PW" --arg t "$2" '{username:$u,password:$pw,role:"operator",tenant:$t}')"
    else
        p="$(jq -nc --arg u "$1" --arg pw "$USER_PW" '{username:$u,password:$pw,role:"operator"}')"
    fi
    r="$(uds auth.user.create "$p")"
    if [ "$(printf '%s' "$r" | jq -r 'has("result") and (.error == null)' 2>/dev/null)" = "true" ]; then
        USERS_MADE="$USERS_MADE $1"; return 0
    fi
    fail "S2: $1 생성 실패 — $(brief "$r")"; return 1
}
make_user "$ALICE" "$TA" && make_user "$BOB" "$TB" && make_user "$NOB" || exit 1
pass "S2: UDS(ADMIN) auth.user.create $ALICE(operator, $TA)·$BOB(operator, $TB)·$NOB(operator, 테넌트 없음)"
TOK_ALICE="$(login_token "$ALICE" "$USER_PW")"; TOK_BOB="$(login_token "$BOB" "$USER_PW")"; TOK_NOB="$(login_token "$NOB" "$USER_PW")"
if [ -n "$TOK_ALICE" ] && [ -n "$TOK_BOB" ] && [ -n "$TOK_NOB" ]; then pass "S3: REST 로그인 세 사용자 → access token"
else fail "S3: REST 로그인 실패(alice='${TOK_ALICE:0:8}' bob='${TOK_BOB:0:8}' nob='${TOK_NOB:0:8}')"; [ -n "$STATE" ] && tail -20 "$STATE/daemon.log" 2>/dev/null; exit 1; fi

BASE_ID="$(max_id)"
info "BASE(audit_log MAX(id)) = $BASE_ID"
r="$(uds vpc.create "$(jq -nc --arg t "$TA" '{name:"pcvp-base",tenant:$t,egress_mode:"isolated"}')" 15)"
bj="$(job_id_of "$r")"
if [ -n "$bj" ] && jr="$(wait_job "$bj" 30)"; then
    BASE_VPC_ID="$(job_result "$jr" | jq -r '.id // empty' 2>/dev/null)"
fi
if is_uuid "$BASE_VPC_ID"; then pass "S4: ta 기준 VPC pcvp-base(subnet 없음) completed — id=$BASE_VPC_ID"
else fail "S4: ta 기준 VPC 생성 실패 — $(brief "$r") / ${jr:-<no job>}"; exit 1; fi

check_1() {
    local since before r c m nj d p
    p="$(pool_cidrs "$TA")"
    if [ "$p" = "[]" ]; then pass "1: 시작 때 ta 풀이 비어 있음"; else fail "1: 시작 때 ta 풀 '$p'(기대 [])"; fi
    since="$(max_id)"; before="$(vpc_job_ids)"
    r="$(rest "$TOK_ALICE" POST vpcs "$(jq -nc --arg c "$A2A" '{name:"pcvp-a1",egress_mode:"isolated",subnet_name:"s",subnet_cidr:$c}')")"
    c="$(err_code "$r")"; m="$(err_msg "$r")"
    if [ "$c" = "-32006" ] && [ "$m" = "$POOL_MSG" ]; then pass "1: 풀 없는 ta의 OPERATOR vpc.create → -32006 풀 밖 문구(바이트 같음)"
    else fail "1: 코드 '${c:-<none>}' 문구 '$m' (기대 -32006 '$POOL_MSG') — $(brief "$r")"; fi
    nj="$(new_vpc_jobs "$before")"
    if [ "$nj" = "0" ]; then pass "1: 거절 사이 새 vpc Job 0"; else fail "1: 새 vpc Job '$nj'"; fi
    d="$(audit_exact "$since" "username='$ALICE' AND method='vpc.create' AND result='denied' AND error_code=-32006 AND target LIKE '$TA/pcvp-a1[%'" 1)"
    if [ "$d" = "1" ]; then pass "1: 감사 denied 1행(username=$ALICE, 대상 $TA/pcvp-a1[…])"
    else fail "1: 감사 denied $d 행 (기대 1)"; dump_audit "$since"; fi
}

check_2() {
    local since r c m w jid jr before nj d rev
    since="$(max_id)"
    pool_set "$TA" "$POOL_TA"; r="$POOL_R"
    if [ "$(printf '%s' "$r" | jq -r --arg c "$POOL_TA" '(.result.tenant == "ta") and (.result.cidrs == [$c]) | tostring' 2>/dev/null)" = "true" ]; then
        pass "2: UDS(ADMIN) vpc.pool.set ta [$POOL_TA] → ok, cidrs 같음"
    else fail "2: vpc.pool.set ta 응답 이상 — $(brief "$r")"; fi
    d="$(audit_exact "$since" "method='vpc.pool.set' AND target='pool:$TA' AND result='ok'" 1)"
    if [ "$d" = "1" ]; then pass "2: 감사 vpc.pool.set pool:ta ok 1행"; else fail "2: 감사 vpc.pool.set pool:ta ok $d 행 (기대 1)"; dump_audit "$since"; fi

    pool_set "$PROBE_T" "$RESERVED_CIDR"; r="$POOL_R"
    w="overlaps reserved management range: $RESERVED_CIDR"
    if [ "$(printf '%s' "$r" | jq -r --arg w "$w" '(.result.warnings // []) | index($w) != null | tostring' 2>/dev/null)" = "true" ]; then
        pass "2: 전제 — $RESERVED_CIDR 는 예약 대역(vpc.pool.set 경고 '$w')"
    else fail "2: 전제 실패 — $RESERVED_CIDR 가 예약 대역이 아니다(라이브: [vpc] reserved_cidrs 호출자 책임) — $(brief "$r")"; fi
    pool_set "$PROBE_T"; r="$POOL_R"
    if [ "$(printf '%s' "$r" | jq -r '.result.cidrs == [] | tostring' 2>/dev/null)" = "true" ]; then pass "2: 전제 확인용 pcvp-probe 풀 비움"
    else fail "2: pcvp-probe 풀 비우기 실패 — $(brief "$r")"; fi

    r="$(rest "$TOK_ALICE" POST vpcs "$(jq -nc --arg c "$A2A" '{name:"pcvp-a2a",egress_mode:"isolated",subnet_name:"s",subnet_cidr:$c}')")"
    jid="$(job_id_of "$r")"
    if is_accepted "$r" && [ -z "$(res_q "$r" '.warning // empty')" ]; then pass "2a: alice vpc.create $A2A(풀 안) → accepted, warning 없음"
    else fail "2a: 응답 이상 — $(brief "$r")"; fi
    if [ "$LIVE" = "0" ]; then skip_live "#2a Job completed (격리 데몬 dnsmasq 실패)"
    elif [ -n "$jid" ] && jr="$(wait_job "$jid" 60)"; then pass "2a: Job completed"; A2A_DONE=1
    else fail "2a: Job 미완료 — $(brief "${jr:-<no result>}")"; fi

    since="$(max_id)"; before="$(vpc_job_ids)"
    rev="$(vpc_rev "$TA" "$BASE_VPC_ID")"
    r="$(rest "$TOK_ALICE" POST "vpcs/$BASE_VPC_ID/subnets" "$(jq -nc --arg c "$RESERVED_CIDR" --argjson rv "$rev" '{name:"s2b",cidr:$c,expected_revision:$rv}')")"
    c="$(err_code "$r")"; m="$(err_msg "$r")"
    if [ "$c" = "-32006" ] && [ "$m" = "$POOL_MSG" ]; then pass "2b: alice vpc.subnet.create $RESERVED_CIDR(풀 밖·예약) → -32006 풀 문구(예약 문구보다 먼저)"
    elif [ "$m" = "$RESERVED_MSG" ]; then fail "2b: 예약 문구가 먼저 나왔다 — 풀 판정이 예약 판정 뒤에 있다(RB13 순서) — $(brief "$r")"
    else fail "2b: 코드 '${c:-<none>}' 문구 '$m' (기대 -32006 풀 문구) — $(brief "$r")"; fi
    nj="$(new_vpc_jobs "$before")"
    if [ "$nj" = "0" ]; then pass "2b: 새 vpc Job 0"; else fail "2b: 새 vpc Job '$nj'"; fi
    d="$(audit_exact "$since" "username='$ALICE' AND method='vpc.subnet.create' AND result='denied' AND error_code=-32006 AND target LIKE '$TA/$BASE_VPC_ID/s2b[%'" 1)"
    if [ "$d" = "1" ]; then pass "2b: 감사 denied 1행(대상 $TA/<기준 VPC>/s2b[…])"; else fail "2b: 감사 denied $d 행 (기대 1)"; dump_audit "$since"; fi

    rev="$(vpc_rev "$TA" "$BASE_VPC_ID")"
    r="$(rest "$TOK_ALICE" POST "vpcs/$BASE_VPC_ID/subnets" "$(jq -nc --arg c "$A2C" --argjson rv "$rev" '{name:"s2c",cidr:$c,expected_revision:$rv}')")"
    jid="$(job_id_of "$r")"
    if is_accepted "$r"; then pass "2c: alice vpc.subnet.create $A2C(풀 안) → accepted"; else fail "2c: 응답 이상 — $(brief "$r")"; fi
    if [ "$LIVE" = "0" ]; then skip_live "#2c Job completed (격리 데몬 dnsmasq 실패)"
    elif [ -n "$jid" ] && jr="$(wait_job "$jid" 60)"; then pass "2c: Job completed"; A2C_DONE=1
    else fail "2c: Job 미완료 — $(brief "${jr:-<no result>}")"; fi

    since="$(max_id)"; before="$(vpc_job_ids)"
    r="$(rest "$TOK_BOB" POST vpcs "$(jq -nc --arg c "$BOB_CIDR" '{name:"pcvp-b2d",egress_mode:"isolated",subnet_name:"s",subnet_cidr:$c}')")"
    c="$(err_code "$r")"; m="$(err_msg "$r")"
    if [ "$c" = "-32006" ] && [ "$m" = "$POOL_MSG" ]; then pass "2d: bob(tb 풀 없음) vpc.create $BOB_CIDR → -32006 풀 문구"
    else fail "2d: 코드 '${c:-<none>}' 문구 '$m' (기대 -32006 풀 문구) — $(brief "$r")"; fi
    nj="$(new_vpc_jobs "$before")"
    if [ "$nj" = "0" ]; then pass "2d: 새 vpc Job 0"; else fail "2d: 새 vpc Job '$nj'"; fi
    d="$(audit_exact "$since" "username='$BOB' AND method='vpc.create' AND result='denied' AND error_code=-32006 AND target LIKE '$TB/pcvp-b2d[%'" 1)"
    if [ "$d" = "1" ]; then pass "2d: 감사 denied 1행(username=$BOB, 대상 $TB/pcvp-b2d[…])"; else fail "2d: 감사 denied $d 행 (기대 1)"; dump_audit "$since"; fi
}

check_3() {
    local since r w d
    since="$(max_id)"
    pool_set "$TB" "$POOL_TB"; r="$POOL_R"
    w="overlaps pool of tenant $TA: $POOL_TB"
    if [ "$(printf '%s' "$r" | jq -r --arg c "$POOL_TB" --arg w "$w" '(.result.cidrs == [$c]) and ((.result.warnings // []) | index($w) != null) | tostring' 2>/dev/null)" = "true" ]; then
        pass "3: vpc.pool.set tb [$POOL_TB] → ok + 경고 '$w'"
    else fail "3: 응답 이상(기대 cidrs [$POOL_TB]·경고 '$w') — $(brief "$r")"; fi
    if [ "$(pool_cidrs "$TB")" = "$(jq -nc --arg c "$POOL_TB" '[$c]')" ]; then pass "3: vpc.pool.list tb = [$POOL_TB]"
    else fail "3: vpc.pool.list tb '$(pool_cidrs "$TB")'"; fi
    d="$(audit_exact "$since" "method='vpc.pool.set' AND target='pool:$TB' AND result='ok'" 1)"
    if [ "$d" = "1" ]; then pass "3: 감사 vpc.pool.set pool:tb ok 1행"; else fail "3: 감사 $d 행 (기대 1)"; dump_audit "$since"; fi
}

check_4() {
    if [ "$LIVE" = "0" ]; then
        skip_live "#4 축소 거부 -32002·풀 그대로·감사 fail 1행 (격리에서는 쓰는 subnet이 store에 남지 않아 축소가 성공한다 — 보내지 않음)"
        return
    fi
    local since r c m d want

    if [ "$A2A_DONE" != "1" ]; then fail "4: 전제 실패 — 2a subnet $A2A 가 completed가 아니다(축소 요청을 보내지 않음)"; return; fi
    since="$(max_id)"
    pool_set "$TA" "$SHRINK"; r="$POOL_R"
    c="$(err_code "$r")"; m="$(err_msg "$r")"
    want="pool change would leave subnets outside the pool: $A2A"
    if [ "$c" = "-32002" ] && [ "$m" = "$want" ]; then pass "4: 축소 vpc.pool.set ta [$SHRINK] → -32002 '$want'"
    else fail "4: 코드 '${c:-<none>}' 문구 '$m' (기대 -32002 '$want') — $(brief "$r")"; fi
    if [ "$(pool_cidrs "$TA")" = "$(jq -nc --arg c "$POOL_TA" '[$c]')" ]; then pass "4: 거부 뒤 ta 풀 그대로 [$POOL_TA]"
    else fail "4: 거부 뒤 ta 풀 '$(pool_cidrs "$TA")' (기대 [$POOL_TA])"; fi
    d="$(audit_exact "$since" "method='vpc.pool.set' AND target='pool:$TA' AND result='fail' AND error_code=-32002" 1)"
    if [ "$d" = "1" ]; then pass "4: 감사 vpc.pool.set pool:ta fail -32002 1행"; else fail "4: 감사 $d 행 (기대 1)"; dump_audit "$since"; fi
}

check_5() {
    local since r jid jr ul items n want p d c m
    r="$(uds vpc.create "$(jq -nc --arg t "$TA" --arg c "$ADMIN_OUT" '{name:"pcvp-a5",tenant:$t,egress_mode:"isolated",subnet_name:"s",subnet_cidr:$c}')" 15)"
    jid="$(job_id_of "$r")"
    if is_accepted "$r" && [ -z "$(res_q "$r" '.warning // empty')" ]; then pass "5: UDS(ADMIN) ta 풀 밖 $ADMIN_OUT vpc.create → accepted(ADMIN은 풀을 보지 않음), warning 없음"
    else fail "5: ADMIN 응답 이상 — $(brief "$r")"; fi
    if [ "$LIVE" = "1" ]; then
        if [ -n "$jid" ] && jr="$(wait_job "$jid" 60)"; then pass "5: ADMIN Job completed"; else fail "5: ADMIN Job 미완료 — $(brief "${jr:-<no result>}")"; fi
    fi
    ul="$(uds resource.unregistered.list "$(jq -nc --arg t "$TA" '{tenant:$t}')" 30)"
    if [ "$(printf '%s' "$ul" | jq -r '.result.vpc_subnets | type' 2>/dev/null)" = "array" ]; then pass "5: resource.unregistered.list 결과에 vpc_subnets 배열"
    else fail "5: vpc_subnets 없음 — $(brief "$ul")"; fi

    items="$(printf '%s' "$ul" | jq -c --arg t "$TA" '[(.result.vpc_subnets // [])[] | select(.tenant == $t and ((.vpc_name // "") | startswith("pcvp-")))
                                                       | {kind:"vpc_pool", tenant:.tenant, cidr:.cidr}]' 2>/dev/null)"
    [ -n "$items" ] || items='[]'
    want="$(jq -nc --arg t "$TA" --arg c "$ADMIN_OUT" '[{kind:"vpc_pool",tenant:$t,cidr:$c}]')"
    if [ "$LIVE" = "1" ]; then

        if [ "$items" = "$want" ] &&
           [ "$(printf '%s' "$ul" | jq -r --arg c "$ADMIN_OUT" '[.result.vpc_subnets[] | select(.cidr == $c and .vpc_name == "pcvp-a5")] | length | tostring')" = "1" ]; then
            pass "5: vpc_subnets 에 ta/pcvp-a5 $ADMIN_OUT 만(풀 안 subnet 제외)"; A5_LISTED=1
        else fail "5: vpc_subnets 변환 '$items' (기대 '$want') — $(brief "$ul")"; fi
    else
        skip_live "#5 vpc_subnets 에 ADMIN이 만든 풀 밖 subnet (격리에서는 subnet이 store에 남지 않음 — 변환 결과 '$items')"

        items="$want"
    fi
    since="$(max_id)"
    n="$(printf '%s' "$items" | jq -r 'length')"
    r="$(uds resource.import "$(jq -nc --argjson i "$items" '{items:$i}')" 30)"

    if [ "$n" != "0" ] && [ "$(res_q "$r" '(.pool_items | tostring) + "/" + (.registered | tostring)')" = "$n/0" ]; then pass "5: resource.import vpc_pool 항목 → pool_items $n, registered 0"
    else fail "5: resource.import 응답 이상(넣은 항목 $n, 기대 pool_items $n ≥ 1) — $(brief "$r")"; fi
    p="$(jq -nc --arg a "$POOL_TA" --arg b "$ADMIN_OUT" '[$a,$b]')"
    if [ "$(pool_cidrs "$TA")" = "$p" ]; then pass "5: import 뒤 vpc.pool.list ta = $p"
    else fail "5: import 뒤 ta 풀 '$(pool_cidrs "$TA")' (기대 $p)"; fi
    d="$(audit_exact "$since" "method='resource.import' AND target='vpc_pool:$TA/$ADMIN_OUT' AND result='ok'" 1)"
    if [ "$d" = "1" ]; then pass "5: 감사 resource.import vpc_pool:ta/$ADMIN_OUT ok 1행"; else fail "5: 감사 $d 행 (기대 1)"; dump_audit "$since"; fi

    r="$(uds resource.import "$(jq -nc --argjson i "$items" '{items:$i}')" 30)"
    if [ -z "$(err_code "$r")" ] && [ "$(pool_cidrs "$TA")" = "$p" ]; then pass "5: 같은 import 재실행 → 오류 없음, ta 풀 그대로 $p(멱등)"
    else fail "5: 재실행 — $(brief "$r") / 풀 '$(pool_cidrs "$TA")'"; fi

    r="$(uds resource.import "$(jq -nc --arg t "$TA" --arg c "$A2A" '{items:[{kind:"vpc_pool",tenant:$t,cidr:$c}]}')" 30)"
    c="$(err_code "$r")"; m="$(err_msg "$r")"
    if [ "$c" = "-32602" ] && [ "$m" = "item 1: vpc_pool cidr $A2A overlaps existing pool cidr $POOL_TA" ] && [ "$(pool_cidrs "$TA")" = "$p" ]; then
        pass "5: ta 풀 원소 안쪽 $A2A import → -32602 '…overlaps existing pool cidr $POOL_TA', ta 풀 그대로"
    else fail "5: 겹침 import 코드 '${c:-<none>}' 문구 '$m' / 풀 '$(pool_cidrs "$TA")' (기대 -32602, 풀 $p)"; fi
    r="$(uds resource.import "$(jq -nc --arg t "$PROBE_T" --arg a "$A2A" --arg b "$BOB_CIDR" \
        '{items:[{kind:"vpc_pool",tenant:$t,cidr:$a},{kind:"vpc_pool",tenant:$t,cidr:$b}]}')" 30)"
    c="$(err_code "$r")"; m="$(err_msg "$r")"
    if [ "$c" = "-32602" ] && [ "$m" = "item 2: vpc_pool cidr $BOB_CIDR overlaps item 1 cidr $A2A" ] && [ "$(pool_cidrs "$PROBE_T")" = "[]" ]; then
        pass "5: 같은 배치 안 겹침(item 2 ⊂ item 1) → -32602, 앞 항목도 반영 안 됨(pcvp-probe 풀 [])"
    else fail "5: 배치 겹침 코드 '${c:-<none>}' 문구 '$m' / probe 풀 '$(pool_cidrs "$PROBE_T")' (기대 -32602, [])"; fi

    r="$(uds resource.import "$(jq -nc --arg t "$TA" --arg c "$ADMIN_OUT" \
        '{items:[{kind:"vpc_pool",tenant:$t,cidr:$c},{kind:"iso",name:"bad name!",ref:"/nonexistent/pcvp.iso"}]}')" 30)"
    c="$(err_code "$r")"; m="$(err_msg "$r")"
    if [ "$c" = "-32602" ] && [ "${m#item 2: }" != "$m" ] && [ "$(pool_cidrs "$TA")" = "$p" ]; then
        pass "5: 일반 항목 저장 오류 → -32602 'item 2: …'(요청 순번), ta 풀 그대로"
    else fail "5: 순번 코드 '${c:-<none>}' 문구 '$m' (기대 -32602 'item 2: …')"; fi
    if [ "$LIVE" = "1" ]; then
        ul="$(uds resource.unregistered.list "$(jq -nc --arg t "$TA" '{tenant:$t}')" 30)"
        if [ "$A5_LISTED" != "1" ]; then
            fail "5: 재조회 전제 실패 — 첫 조회에 그 subnet이 없어 '빠짐'을 볼 수 없다"
        elif [ "$(printf '%s' "$ul" | jq -r --arg t "$TA" --arg c "$ADMIN_OUT" '[(.result.vpc_subnets // "ERR")[] | select(.tenant == $t and .cidr == $c)] | length | tostring' 2>/dev/null)" = "0" ]; then
            pass "5: 재조회한 vpc_subnets 에 $ADMIN_OUT 없음(이제 풀 안)"
        else fail "5: 재조회 vpc_subnets 에 $ADMIN_OUT 남음 — $(brief "$ul")"; fi
    else
        skip_live "#5 재조회 vpc_subnets 에서 그 subnet이 빠짐 (격리에서는 subnet 없음)"
    fi
}

check_6() {
    local since before r c m nj d sid rev jid jr rows
    since="$(max_id)"; before="$(vpc_job_ids)"
    r="$(rest "$TOK_NOB" POST vpcs "$(jq -nc --arg c "$A2A" '{name:"pcvp-n6",egress_mode:"isolated",subnet_name:"s",subnet_cidr:$c}')")"
    c="$(err_code "$r")"; m="$(err_msg "$r")"
    if [ "$c" = "-32002" ] && [ "$m" = "$TENANT_MSG" ]; then pass "6a: nob(테넌트 없음) vpc.create → -32002 '$TENANT_MSG'(기존 계약)"
    else fail "6a: 코드 '${c:-<none>}' 문구 '$m' (기대 -32002 '$TENANT_MSG') — $(brief "$r")"; fi
    nj="$(new_vpc_jobs "$before")"
    if [ "$nj" = "0" ]; then pass "6a: 새 vpc Job 0"; else fail "6a: 새 vpc Job '$nj'"; fi
    d="$(audit_exact "$since" "username='$NOB' AND method='vpc.create' AND error_code=-32002" 1)"
    if [ "$d" = "1" ]; then pass "6a: 감사 1행(username=$NOB, -32002)"; else fail "6a: 감사 $d 행 (기대 1)"; dump_audit "$since"; fi

    if [ "$LIVE" = "1" ] && [ "$A2C_DONE" = "1" ]; then
        rows="$(subnet_rows "$TA" "$BASE_VPC_ID")" || rows="[]"
        sid="$(printf '%s' "$rows" | jq -r --arg c "$A2C" '.[]? | select(.cidr == $c) | .id' | head -1)"
        if is_uuid "$sid"; then rpc_job_done vpc.subnet.delete "$(jq -nc --arg t "$TA" --arg s "$sid" '{tenant:$t,subnet_id:$s}')"; fi
        if [ "$(subnet_rows "$TA" "$BASE_VPC_ID" | jq -r --arg c "$A2C" '[.[]? | select(.cidr == $c)] | length | tostring' 2>/dev/null)" = "0" ]; then
            pass "6: (라이브 준비) 2c subnet $A2C 삭제 — $SHRINK 대역 비움"
        else fail "6: (라이브 준비) 2c subnet 삭제 실패(sid='$sid')"; fi
    fi
    since="$(max_id)"
    rev="$(vpc_rev "$TA" "$BASE_VPC_ID")"
    r="$(uds vpc.subnet.create "$(jq -nc --arg v "$BASE_VPC_ID" --arg c "$SHRINK" --argjson rv "$rev" \
        '{vpc_id:$v,name:"g6",cidr:$c,expected_revision:$rv,_pcv_caller_role:1,_pcv_caller_sub:"grpc",_pcv_caller_tenant:"ta"}')" 15)"
    jid="$(job_id_of "$r")"
    if is_accepted "$r"; then pass "6b: UDS gRPC 주입 흉내(role 1·sub grpc·tenant ta) vpc.subnet.create $SHRINK(풀 안) → accepted"
    else fail "6b: 응답 이상(기대 accepted) — $(brief "$r")"; fi

    d="$(audit_exact "$since" "username='grpc' AND method='vpc.subnet.create' AND target LIKE '$TA/$BASE_VPC_ID/g6[%'" 1)"
    if [ "$d" = "1" ]; then pass "6b: 감사 1행 username=grpc, 대상 $TA/<기준 VPC>/g6[…](주입 테넌트 ta로 판정)"
    else fail "6b: 감사 $d 행 (기대 1)"; dump_audit "$since"; fi
    if [ "$LIVE" = "0" ]; then skip_live "#6b Job completed·subnet 목록 (격리 데몬 dnsmasq 실패)"
    elif [ -n "$jid" ] && jr="$(wait_job "$jid" 60)" &&
         [ "$(subnet_rows "$TA" "$BASE_VPC_ID" | jq -r --arg c "$SHRINK" '[.[]? | select(.cidr == $c and .name == "g6")] | length | tostring' 2>/dev/null)" = "1" ]; then
        pass "6b: Job completed, ta 기준 VPC subnet 목록에 g6 $SHRINK"
    else fail "6b: Job/목록 이상 — $(brief "${jr:-<no result>}")"; fi

    since="$(max_id)"
    r="$(uds vpc.subnet.create "$(jq -nc --arg v "$BASE_VPC_ID" --arg c "$SHRINK" --argjson rv "$rev" \
        '{vpc_id:$v,name:"g6n",cidr:$c,expected_revision:$rv,_pcv_caller_role:1,_pcv_caller_sub:"grpc",_pcv_caller_tenant:null}')" 15)"
    c="$(err_code "$r")"; m="$(err_msg "$r")"
    if [ "$c" = "-32002" ] && [ "$m" = "$TENANT_MSG" ]; then pass "6c: 같은 흉내에 _pcv_caller_tenant:null → -32002 '$TENANT_MSG'(테넌트는 주입값에서만)"
    else fail "6c: 코드 '${c:-<none>}' 문구 '$m' (기대 -32002) — $(brief "$r")"; fi
    d="$(audit_exact "$since" "username='grpc' AND method='vpc.subnet.create' AND error_code=-32002" 1)"
    if [ "$d" = "1" ]; then pass "6c: 감사 1행(username=grpc, -32002)"; else fail "6c: 감사 $d 행 (기대 1)"; dump_audit "$since"; fi
}

check_7() {
    local n dn pn
    sleep 1
    dn="$(audit_count "$BASE_ID" "username IN ('$ALICE','$BOB','$NOB') AND result='denied'")"
    if [ "$dn" = "3" ]; then pass "7: 시험 사용자 denied 합계 3행(#1·#2b·#2d — 거절마다 1행, 겹친 행 없음)"
    else fail "7: 시험 사용자 denied $dn 행 (기대 3)"; dump_audit "$BASE_ID"; fi
    pn="$(audit_count "$BASE_ID" "method='vpc.pool.set' AND target IN ('pool:$TA','pool:$TB','pool:$PROBE_T')")"
    if [ "$pn" = "$POOL_SET_CALLS" ]; then pass "7: vpc.pool.set 감사 $pn 행 = 호출 $POOL_SET_CALLS 회(성공·실패 각 1행)"
    else fail "7: vpc.pool.set 감사 $pn 행 ≠ 호출 $POOL_SET_CALLS 회"; dump_audit "$BASE_ID"; fi
    n="$(audit_count "$BASE_ID" "username='-'")"
    if [ "$n" = "0" ]; then pass "7: 시험 전체에서 audit_log username='-' 0행"
    else fail "7: username='-' $n 행"; aq "SELECT id||'|'||method||'|'||IFNULL(target,'') FROM audit_log WHERE id > $BASE_ID AND username='-';" | sed 's/^/        /'; fi
}

check_1
check_2
check_3
check_4
check_5
check_6
check_7

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC} / ${YELLOW}live-only skip %d${NC} (모드: %s)\n" \
    "$PASS" "$FAIL" "$SKIPPED" "$([ "$LIVE" = 1 ] && echo 라이브 || echo 격리)"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if [ "$PASS" -lt 20 ]; then fail "단언이 거의 실행되지 않음(PASS $PASS) — 준비 단계 점검"; fi
[ "$FAIL" -eq 0 ]
