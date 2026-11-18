#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; SKIPPED=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

skip_live() { echo -e "${YELLOW}[SKIP]${NC} live only — $*"; SKIPPED=$((SKIPPED+1)); }
skip_all() { echo -e "${YELLOW}[SKIP]${NC} VPC 관리 대역 보호 E2E: $*"; exit 0; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_VPC_MGMT_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

LIVE=0
[ "${PCV_VPC_MGMT_LIVE:-0}" = "1" ] && LIVE=1

RESERVED_CIDR='10.253.98.0/24'
FREE_CIDR='10.253.97.0/24'
CONFIRM_SEC=5
ALICE='pcvg-alice'
ALICE_PW='PcvgAlicePw-not-for-prod-1'
ADMIN_PW='PcvgAdminPw-not-for-prod-1'
TENANT='ta'
PORT=0

warning_expected() { printf '관리 대역 겹침(%s) — Job 완료 뒤 %s초 안에 vpc.subnet.confirm을 보내지 않으면 자동 원복합니다' "$RESERVED_CIDR" "$CONFIRM_SEC"; }

for tool in jq sqlite3 curl python3; do
    command -v "$tool" >/dev/null 2>&1 || skip_all "$tool 미설치"
done

STATE=""
SOCK=""; BASE=""; AUDIT_DB=""; FLAG=""
FLAG_MADE=0; TIMEOUT_SET=0

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"
if [ "$LIVE" = "1" ]; then
    CONFIRM_SEC="${PCV_VPC_MGMT_LIVE_CONFIRM_SEC:-5}"
    SOCK="${PCV_VPC_MGMT_LIVE_SOCK:-/var/run/purecvisor/daemon.sock}"
    BASE="${PCV_VPC_MGMT_LIVE_REST:-http://127.0.0.1:8080/api/v1}"
    AUDIT_DB="${PCV_VPC_MGMT_LIVE_AUDIT_DB:-/var/lib/purecvisor/pcv_audit.db}"
    FLAG="${PCV_VPC_MGMT_LIVE_FLAG:-/etc/purecvisor/vpc-reconcile.skip}"
    [ -S "$SOCK" ] || skip_all "라이브 UDS 없음($SOCK)"

    if [ -e "$FLAG" ]; then
        echo -e "${RED}[FAIL]${NC} 라이브 reconcile 건너뛰기 플래그가 이미 있다($FLAG) — 운영자 플래그 보존을 위해 시험을 거부한다"; exit 1
    fi

    if [ -n "${PCV_VPC_MGMT_LIVE_TIMEOUT_CMD:-}" ] && { [ -z "${PCV_VPC_MGMT_LIVE_RESTORE_CMD:-}" ] || [ -z "${PCV_VPC_MGMT_LIVE_RESTART_CMD:-}" ]; }; then
        echo -e "${RED}[FAIL]${NC} PCV_VPC_MGMT_LIVE_TIMEOUT_CMD 에는 PCV_VPC_MGMT_LIVE_RESTORE_CMD 와 RESTART_CMD 가 함께 필요하다 — 시험을 거부한다"; exit 1
    fi
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
        for p in 28087 29087 31087 33087 37087 41087 43087; do
            if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then echo "$p"; return 0; fi
        done
        return 1
    }
    PORT="$(pick_free_port)" || skip_all "빈 포트 확보 실패"
    BASE="http://127.0.0.1:$PORT/api/v1"
    STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-vpcg.XXXXXX")"
    pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
    mkdir -p "$STATE/var-lib" "$STATE/etc"
    AUDIT_DB="$STATE/var-lib/pcv_audit.db"
    SOCK="$STATE/var-lib/daemon.sock"
    FLAG="$STATE/etc/vpc-reconcile.skip"
fi

kill_daemon() {
    [ -n "$STATE" ] && [ -f "$STATE/bwrap.pid" ] || return 0
    local bp; bp="$(cat "$STATE/bwrap.pid" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    local i
    for i in $(seq 1 12); do kill -0 "$bp" 2>/dev/null || break; sleep 0.25; done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$STATE/bwrap.pid"
}

CLEANUP_RESIDUE=0
EFF_SEC=$CONFIRM_SEC

pcvg_vpc_ids() {
    local r; r="$(uds vpc.list "{\"tenant\":\"$TENANT\"}" 10)"
    if [ "$(printf '%s' "$r" | jq -r '.result | type' 2>/dev/null)" != "array" ]; then echo ERR; return 1; fi
    printf '%s' "$r" | jq -r '.result[] | select(.name|startswith("pcvg-")) | .id'
}
pcvg_child_rows() {
    local ids vid r rows="[]"
    ids="$(pcvg_vpc_ids)" || { echo ERR; return 1; }
    for vid in $ids; do
        r="$(uds "$1" "{\"tenant\":\"$TENANT\",\"vpc_id\":\"$vid\"}" 10)"
        if [ "$(printf '%s' "$r" | jq -r '.result | type' 2>/dev/null)" != "array" ]; then echo ERR; return 1; fi
        rows="$(jq -c -n --argjson a "$rows" --argjson b "$(printf '%s' "$r" | jq -c '.result')" '$a + $b')"
    done
    printf '%s' "$rows"
}
subnet_rows() { pcvg_child_rows vpc.subnet.list; }

rpc_job_done() {
    local r jid
    r="$(uds "$1" "$2" 15)"; jid="$(job_id_of "$r")"
    [ -n "$jid" ] && wait_job "$jid" 60 >/dev/null 2>&1
    return 0
}
cleanup_resources() {
    local left vid sid aid pend i n=0
    for vid in $(pcvg_vpc_ids | grep -v '^ERR$'); do
        for aid in $(uds vpc.attachment.list "{\"tenant\":\"$TENANT\",\"vpc_id\":\"$vid\"}" 10 | jq -r '(.result // [])[]? | .id' 2>/dev/null); do
            rpc_job_done vpc.attachment.delete "{\"tenant\":\"$TENANT\",\"attachment_id\":\"$aid\"}"
        done
        for sid in $(uds vpc.subnet.list "{\"tenant\":\"$TENANT\",\"vpc_id\":\"$vid\"}" 10 | jq -r '(.result // [])[]? | .id' 2>/dev/null); do
            rpc_job_done vpc.subnet.delete "{\"tenant\":\"$TENANT\",\"subnet_id\":\"$sid\"}"
        done
        rpc_job_done vpc.delete "{\"tenant\":\"$TENANT\",\"vpc_id\":\"$vid\"}"
        n=$((n+1))
    done
    echo -e "${CYAN}[INFO]${NC} cleanup: pcvg-* VPC ${n}개 정리 시도"
    left="$(pcvg_vpc_ids | grep -c . )"; [ "$(pcvg_vpc_ids | head -1)" = "ERR" ] && left="ERR"

    pend=""
    for i in $(seq 1 $((EFF_SEC + 10))); do
        pend="$(uds vpc.status "{\"tenant\":\"$TENANT\"}" 10 | jq -r '.result.pending_confirmations // empty' 2>/dev/null)"
        [ "$pend" = "0" ] && break
        sleep 1
    done
    if [ "$left" = "0" ] && [ "$pend" = "0" ]; then
        echo -e "${GREEN}[PASS]${NC} cleanup: pcvg-* VPC 잔재 0, 확인 대기 0"
    else
        echo -e "${RED}[FAIL]${NC} cleanup: pcvg-* VPC 잔재 '${left:-?}', 확인 대기 '${pend:-?}' — 설치 데몬에 시험 자원이 남았다"
        CLEANUP_RESIDUE=1
    fi
}
cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    if [ -n "$(uds vpc.status '{}' 5)" ]; then
        cleanup_resources
        uds auth.user.delete "{\"username\":\"$ALICE\"}" 10 >/dev/null 2>&1 || true
    fi
    if [ "$LIVE" = "1" ]; then
        [ "$FLAG_MADE" = "1" ] && rm -f "$FLAG" 2>/dev/null

        if [ "$TIMEOUT_SET" = "1" ]; then
            if bash -c "$PCV_VPC_MGMT_LIVE_RESTORE_CMD" >/dev/null 2>&1 && bash -c "$PCV_VPC_MGMT_LIVE_RESTART_CMD" >/dev/null 2>&1; then
                echo -e "${GREEN}[PASS]${NC} cleanup: confirm_timeout_sec 복원 + 재시작"
            else
                echo -e "${RED}[FAIL]${NC} cleanup: confirm_timeout_sec 복원 실패 — 수동 복원 필요"; CLEANUP_RESIDUE=1
            fi
        fi
    else
        kill_daemon
        [ -n "$STATE" ] && rm -rf "$STATE" 2>/dev/null
    fi
    if [ "$CLEANUP_RESIDUE" = "1" ] && [ "$rc" = "0" ]; then rc=1; fi
    exit "$rc"
}
trap cleanup EXIT INT TERM

uds() {
    printf '%s' "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"vpcg\"}" \
        | pcv_iso_uds_nc "$SOCK" "${3:-30}" 2>/dev/null || true
}
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
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null | jq -r '.access_token // empty' 2>/dev/null || true
}

write_conf() {
    cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $ADMIN_PW
jwt_secret = vpcg-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1

[vpc]
reserved_cidrs = $RESERVED_CIDR
confirm_timeout_sec = $1
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
    local i resp
    for i in $(seq 1 60); do
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

restart_daemon() {
    if [ "$LIVE" = "1" ]; then
        [ -n "${PCV_VPC_MGMT_LIVE_RESTART_CMD:-}" ] || return 2
        if [ -n "${1:-}" ]; then
            [ -n "${PCV_VPC_MGMT_LIVE_TIMEOUT_CMD:-}" ] || return 2
            TIMEOUT_SET=1; EFF_SEC="$1"
            bash -c "$PCV_VPC_MGMT_LIVE_TIMEOUT_CMD $1" >/dev/null 2>&1 || return 1
        fi
        bash -c "$PCV_VPC_MGMT_LIVE_RESTART_CMD" >/dev/null 2>&1 || return 1
        local i
        for i in $(seq 1 60); do
            [ -S "$SOCK" ] && [ -n "$(uds vpc.status '{}' 5)" ] && return 0
            sleep 1
        done
        return 1
    fi
    kill_daemon
    EFF_SEC="${1:-$CONFIRM_SEC}"
    write_conf "$EFF_SEC"
    boot
}

bridge_visible() {
    local out
    if [ "$LIVE" = "1" ]; then out="$(ip -br link 2>/dev/null)"
    else out="$(pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" ip -br link 2>/dev/null)"; fi
    printf '%s\n' "$out" | awk '{print $1}' | sed 's/@.*//' | grep -qx "$1"
}

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

jobs_count() {
    local b; b="$(rest "$1" GET 'jobs?limit=500')"
    printf '%s' "$b" | jq -r '.data.items | length' 2>/dev/null || echo "?"
}
wait_job() {
    local r st _ lim="${2:-30}"
    for _ in $(seq 1 $((lim * 4))); do
        r="$(uds jobs.get "{\"job_id\":\"$1\"}" 10)"
        st="$(printf '%s' "$r" | jq -r '.result.status // .result.job.status // empty' 2>/dev/null)"
        case "$st" in completed|failed) printf '%s' "$r"; [ "$st" = completed ]; return ;; esac
        sleep 0.25
    done
    return 1
}
job_result() {
    printf '%s' "$1" | jq -c '(.result.result // .result.job.result // empty) | if type=="string" then (fromjson? // .) else . end' 2>/dev/null
}
is_accepted() { [ "$(printf '%s' "$1" | jq -r '.result.status // empty' 2>/dev/null)" = "accepted" ]; }
job_id_of() { printf '%s' "$1" | jq -r '.result.job_id // empty' 2>/dev/null; }

TOK_ALICE=""
BASE_ID=0
BASE_VPC_ID=""; BASE_VPC_REV=1
FREE_SUBNET_ID=""; FREE_BRIDGE=""
FAKE_UUID='00000000-0000-4000-8000-000000000001'

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  VPC 관리 대역 보호·확인 대기 자동 원복 E2E ($([ "$LIVE" = 1 ] && echo 라이브 || echo 격리) 모드)${NC}"
echo -e "${CYAN}  base=$BASE state=${STATE:-live}${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if [ "$LIVE" = "0" ]; then
    write_conf "$CONFIRM_SEC"
    if ! boot; then
        fail "S1: 격리 데몬 기동 실패"; tail -30 "$STATE/daemon.log" 2>/dev/null; exit 1
    fi
    pass "S1: 격리 데몬 기동 + UDS 응답 + REST /health 준비"
else
    if [ -n "$(uds vpc.status '{}' 10)" ]; then pass "S1: 라이브 데몬 UDS 응답"; else fail "S1: 라이브 UDS 무응답"; exit 1; fi
fi
r="$(uds auth.user.create "{\"username\":\"$ALICE\",\"password\":\"$ALICE_PW\",\"role\":\"operator\",\"tenant\":\"$TENANT\"}")"
if [ "$(printf '%s' "$r" | jq -r 'has("result") and (.error == null)' 2>/dev/null)" = "true" ]; then
    pass "S2: UDS(ADMIN) auth.user.create $ALICE(operator, tenant $TENANT)"
else
    fail "S2: alice 생성 실패 — ${r:-<empty>}"; exit 1
fi
TOK_ALICE="$(login_token "$ALICE" "$ALICE_PW")"
if [ -n "$TOK_ALICE" ]; then pass "S3: REST 로그인 $ALICE → access token"
else fail "S3: REST 로그인 실패"; [ -n "$STATE" ] && tail -20 "$STATE/daemon.log" 2>/dev/null; exit 1; fi

BASE_ID="$(max_id)"
info "BASE(audit_log MAX(id)) = $BASE_ID"

r="$(rest "$TOK_ALICE" POST vpcs '{"name":"pcvg-base","egress_mode":"isolated"}')"
info "기준 VPC 생성 응답: $(printf '%s' "$r" | head -c 300)"
bj="$(printf '%s' "$r" | jq -r '.data.job_id // .job_id // empty' 2>/dev/null)"
if [ -n "$bj" ] && jr="$(wait_job "$bj" 20)"; then
    info "기준 VPC Job 결과: $(job_result "$jr" | head -c 300)"
    BASE_VPC_ID="$(job_result "$jr" | jq -r '.id // empty' 2>/dev/null)"
    BASE_VPC_REV="$(job_result "$jr" | jq -r '.revision // 1' 2>/dev/null)"
fi
[ -n "$BASE_VPC_ID" ] || BASE_VPC_ID="$FAKE_UUID"
info "기준 VPC id=$BASE_VPC_ID revision=$BASE_VPC_REV"

check_1() {
    local since jobs_before jobs_after r1 r2 c1 c2 m1 m2 dc ds
    since="$(max_id)"; jobs_before="$(jobs_count "$TOK_ALICE")"
    r1="$(rest "$TOK_ALICE" POST vpcs "{\"name\":\"pcvg-a1\",\"egress_mode\":\"isolated\",\"subnet_name\":\"s\",\"subnet_cidr\":\"$RESERVED_CIDR\"}")"
    r2="$(rest "$TOK_ALICE" POST "vpcs/$BASE_VPC_ID/subnets" "{\"name\":\"s\",\"cidr\":\"$RESERVED_CIDR\",\"expected_revision\":$BASE_VPC_REV}")"
    c1="$(printf '%s' "$r1" | jq -r '.error.code // empty' 2>/dev/null)"; m1="$(printf '%s' "$r1" | jq -r '.error.message // empty' 2>/dev/null)"
    c2="$(printf '%s' "$r2" | jq -r '.error.code // empty' 2>/dev/null)"; m2="$(printf '%s' "$r2" | jq -r '.error.message // empty' 2>/dev/null)"
    if [ "$c1" = "-32006" ] && [ "$c2" = "-32006" ]; then
        pass "1: OPERATOR vpc.create·vpc.subnet.create 예약 대역 → 둘 다 -32006"
    else
        fail "1: 거절 코드 create=${c1:-<none>} subnet.create=${c2:-<none>} (기대 -32006) — $r1 / $r2"
    fi
    if [ -n "$m1" ] && [ -n "$m2" ] && ! printf '%s%s' "$m1" "$m2" | grep -q '10\.253\.98'; then
        pass "1: 거절 메시지가 있고 겹친 관리 대역 값(10.253.98)이 없음"
    else
        fail "1: 거절 메시지가 비었거나 겹친 값(10.253.98)을 노출 — '$m1' / '$m2'"
    fi
    jobs_after="$(jobs_count "$TOK_ALICE")"
    if [ "$jobs_before" = "$jobs_after" ] && [ "$jobs_before" != "?" ]; then
        pass "1: 거절 사이 jobs 수 불변($jobs_before)"
    else
        fail "1: jobs 수 변함 $jobs_before → $jobs_after"
    fi

    if is_uuid "$BASE_VPC_ID"; then
        dc="$(audit_exact "$since" "username='$ALICE' AND method='vpc.create' AND result='denied' AND target LIKE '$TENANT/pcvg-a1[%'" 1)"
        ds="$(audit_exact "$since" "username='$ALICE' AND method='vpc.subnet.create' AND result='denied' AND target LIKE '$TENANT/$BASE_VPC_ID/s[%'" 1)"
    else
        dc="?"; ds="?(VPC id 형식 아님: '$BASE_VPC_ID')"
    fi
    if [ "$dc" = "1" ] && [ "$ds" = "1" ]; then pass "1: 감사 denied 2행(vpc.create·vpc.subnet.create 각 1행, 대상 일치)"
    else fail "1: 감사 denied vpc.create $dc 행 / vpc.subnet.create $ds 행 (기대 각 1)"; dump_audit "$since"; fi
}

check_2() {
    if [ "$LIVE" = "0" ]; then skip_live "#2 OPERATOR 비예약 vpc.create Job completed (격리 데몬 dnsmasq 실패)"; return; fi
    local r jid jr
    r="$(rest "$TOK_ALICE" POST vpcs "{\"name\":\"pcvg-a2\",\"egress_mode\":\"isolated\",\"subnet_name\":\"s\",\"subnet_cidr\":\"$FREE_CIDR\"}")"
    jid="$(printf '%s' "$r" | jq -r '.data.job_id // empty' 2>/dev/null)"
    if [ -n "$jid" ] && [ "$(printf '%s' "$r" | jq -r '.data.warning // empty' 2>/dev/null)" = "" ]; then
        pass "2: OPERATOR 비예약 vpc.create accepted, warning 없음"
    else
        fail "2: 응답 이상 — $r"
    fi
    if [ -n "$jid" ] && jr="$(wait_job "$jid" 60)"; then
        pass "2: Job completed"
        FREE_SUBNET_ID="$(job_result "$jr" | jq -r '.subnet_id // .subnet.id // empty' 2>/dev/null)"
        if [ -z "$FREE_SUBNET_ID" ]; then
            local rows; rows="$(subnet_rows)" || rows=""
            FREE_SUBNET_ID="$(printf '%s' "$rows" | jq -r --arg c "$FREE_CIDR" '.[]? | select(.cidr==$c) | .id' 2>/dev/null | head -1)"
            [ -n "$rows" ] && [ "$rows" != ERR ] || fail "2: vpc.subnet.list 오류 — subnet id를 찾지 못함"
        fi
    else
        fail "2: Job 미완료 — ${jr:-<no result>}"
    fi
}

SUB_A3='10.253.98.0/27'; SUB_A4A='10.253.98.32/27'; SUB_A4B='10.253.98.64/27'; SUB_A5='10.253.98.96/27'; SUB_A6='10.253.98.128/27'
admin_reserved_create() {
    uds vpc.create "{\"name\":\"$1\",\"tenant\":\"$TENANT\",\"egress_mode\":\"isolated\",\"subnet_name\":\"s\",\"subnet_cidr\":\"${2:-$RESERVED_CIDR}\"}" 15
}
check_3() {
    local since r want got ct jid jr res listed pend n psid
    since="$(max_id)"
    r="$(admin_reserved_create pcvg-a3 "$SUB_A3")"
    want="$(warning_expected)"
    got="$(printf '%s' "$r" | jq -r '.result.warning // empty' 2>/dev/null)"
    ct="$(printf '%s' "$r" | jq -r '.result.confirm_timeout_sec // empty' 2>/dev/null)"
    jid="$(job_id_of "$r")"
    if is_accepted "$r" && [ "$got" = "$want" ]; then pass "3: ADMIN accepted + warning 문장 바이트 일치"
    else fail "3: accepted/warning 불일치 — 기대 '$want' / 실제 '$got' ($r)"; fi
    if [ "$ct" = "$CONFIRM_SEC" ]; then pass "3: confirm_timeout_sec == $CONFIRM_SEC"
    else fail "3: confirm_timeout_sec '$ct' (기대 $CONFIRM_SEC)"; fi

    n="$(audit_exact "$since" "method='vpc.reserved.warning' AND target LIKE '$TENANT/pcvg-a3[%'" 1)"
    if [ "$n" = "1" ]; then pass "3: 감사 vpc.reserved.warning 1행"
    else fail "3: 감사 vpc.reserved.warning $n 행 (기대 1)"; dump_audit "$since"; fi
    if [ "$LIVE" = "0" ]; then
        skip_live "#3 Job 결과 warning·confirm_pending·subnet.list·status.pending_confirmations==1 (Job 완료 필요)"
        return
    fi
    if [ -n "$jid" ] && jr="$(wait_job "$jid" 60)"; then
        res="$(job_result "$jr")"
        psid="$(printf '%s' "$res" | jq -r '.subnet_id // empty')"
        if [ "$(printf '%s' "$res" | jq -r '.confirm_pending')" = "true" ] &&
           [ "$(printf '%s' "$res" | jq -r '.warning')" = "관리 대역 겹침($RESERVED_CIDR) — ${CONFIRM_SEC}초 안에 vpc.subnet.confirm(pcvctl vpc subnet-confirm $psid)을 보내지 않으면 자동 원복합니다" ]; then
            pass "3: Job 결과 warning(RB3 문장)·confirm_pending:true"
        else
            fail "3: Job 결과 이상 — $res"
        fi
        listed="$(subnet_rows | jq -r --arg id "$psid" '.[]? | select(.id==$id) | .confirm_pending' 2>/dev/null)"
        pend="$(uds vpc.status "{\"tenant\":\"$TENANT\"}" 10 | jq -r '.result.pending_confirmations // empty')"
        if [ "$listed" = "true" ] && [ "$pend" = "1" ]; then pass "3: subnet.list confirm_pending:true, status.pending_confirmations==1"
        else fail "3: subnet.list confirm_pending='$listed' pending_confirmations='$pend'"; fi
    else
        fail "3: Job 미완료 — ${jr:-<no result>}"
    fi
}

admin_pending_subnet() {
    local r jid jr
    r="$(admin_reserved_create "$1" "$2")"; jid="$(job_id_of "$r")"
    jr="$(wait_job "$jid" 60)" || true
    job_result "$jr" | jq -r '.subnet_id // empty'
}
subnet_count() {
    local rows; rows="$(subnet_rows)" || { echo ERR; return; }
    printf '%s' "$rows" | jq -r --arg id "$1" '[.[]? | select(.id==$id)] | length'
}

check_4() {
    if [ "$LIVE" = "0" ]; then skip_live "#4 자동 원복·confirm 유지·재confirm -32005 (Job 완료 필요)"; return; fi
    local since r sid left c
    since="$(max_id)"
    sid="$(admin_pending_subnet pcvg-a4a "$SUB_A4A")"
    sleep $((CONFIRM_SEC + 3))
    left="$(subnet_count "$sid")"

    if is_uuid "$sid"; then
        c="$(audit_exact "$since" "username='system' AND method='vpc.subnet.auto_revert' AND result='ok' AND target LIKE '%/$sid[%' AND target NOT LIKE '% boot'" 1)"
    else
        c="?(subnet id 형식 아님)"
    fi
    if is_uuid "$sid" && [ "$left" = "0" ] && [ "$c" = "1" ]; then pass "4a: 확인 없이 대기 → subnet 없음 + 그 subnet의 감사 system·vpc.subnet.auto_revert·ok 1행"
    else fail "4a: sid='$sid' 남은 subnet=$left auto_revert 행=$c"; dump_audit "$since"; fi
    sid="$(admin_pending_subnet pcvg-a4b "$SUB_A4B")"
    r="$(uds vpc.subnet.confirm "{\"subnet_id\":\"$sid\",\"tenant\":\"$TENANT\"}" 10)"
    sleep $((CONFIRM_SEC + 3))
    left="$(subnet_count "$sid")"
    if [ -n "$sid" ] && [ "$(printf '%s' "$r" | jq -r 'has("result") and (.error==null)')" = "true" ] && [ "$left" = "1" ]; then
        pass "4b: 제때 confirm ok → 대기 시간 뒤에도 subnet 있음"
    else fail "4b: sid='$sid' confirm=$r 남은 subnet=$left"; fi
    r="$(uds vpc.subnet.confirm "{\"subnet_id\":\"$sid\",\"tenant\":\"$TENANT\"}" 10)"
    c="$(printf '%s' "$r" | jq -r '.error.code // empty')"
    if [ "$c" = "-32005" ]; then pass "4c: 이미 확인한 subnet 재confirm → -32005"
    else fail "4c: 재confirm 코드 '$c' (기대 -32005) — $r"; fi
}

check_5() {
    if [ "$LIVE" = "0" ]; then skip_live "#5 재시작 기동 원복 (Job 완료 필요)"; return; fi
    local since sid rc left c skipped
    since="$(max_id)"
    restart_daemon 120; rc=$?
    if [ "$rc" != 0 ]; then fail "5: confirm_timeout_sec=120 재시작 실패(rc=$rc — PCV_VPC_MGMT_LIVE_RESTART_CMD/TIMEOUT_CMD 필요)"; return; fi
    sid="$(admin_pending_subnet pcvg-a5 "$SUB_A5")"
    restart_daemon; rc=$?
    if [ "$rc" != 0 ]; then fail "5: 재시작 실패(rc=$rc)"; return; fi
    left="$(subnet_count "$sid")"

    if is_uuid "$sid"; then
        c="$(audit_exact "$since" "username='system' AND method='vpc.subnet.auto_revert' AND result='ok' AND target LIKE '%/$sid[%] boot'" 1)"
    else
        c="?(subnet id 형식 아님)"
    fi
    skipped="$(uds vpc.status '{}' 10 | jq -r '.result.boot_reconcile_skipped | tostring')"
    if is_uuid "$sid" && [ "$left" = "0" ] && [ "$c" = "1" ] && [ "$skipped" = "false" ]; then
        pass "5: 재시작 뒤 확인 대기 subnet 없음 + 그 subnet의 감사 auto_revert 대상 끝 ' boot' 1행 + boot_reconcile_skipped==false"
    else
        fail "5: sid='$sid' 남은 subnet=$left boot 감사=$c boot_reconcile_skipped='$skipped'"; dump_audit "$since"
    fi
}

check_6() {
    if [ "$LIVE" = "0" ]; then skip_live "#6 대기 subnet에 vpc.attachment.create -32002 (Job 완료 필요)"; return; fi
    local r sid c m
    sid="$(admin_pending_subnet pcvg-a6 "$SUB_A6")"
    r="$(uds vpc.attachment.create "{\"subnet_id\":\"$sid\",\"tenant\":\"$TENANT\",\"vm\":\"pcvg-vm\"}" 10)"
    c="$(printf '%s' "$r" | jq -r '.error.code // empty')"; m="$(printf '%s' "$r" | jq -r '.error.message // empty')"
    if [ -n "$sid" ] && [ "$c" = "-32002" ] && printf '%s' "$m" | grep -q 'awaiting confirmation'; then
        pass "6: 대기 subnet에 vpc.attachment.create → -32002 awaiting confirmation"
    else
        fail "6: sid='$sid' code='$c' message='$m'"
    fi
    uds vpc.subnet.confirm "{\"subnet_id\":\"$sid\",\"tenant\":\"$TENANT\"}" 10 >/dev/null
}

check_7() {
    if [ "$LIVE" = "0" ]; then skip_live "#7 vpc-reconcile.skip 플래그 동작 (Job 완료 필요)"; return; fi
    local since rc skipped inlist c
    if [ -z "$FREE_SUBNET_ID" ]; then fail "7: 전제 실패 — #2의 비예약 subnet 없음"; return; fi
    FREE_BRIDGE="$(subnet_rows | jq -r --arg id "$FREE_SUBNET_ID" '.[]? | select(.id==$id) | .bridge_name // empty' 2>/dev/null)"
    if [ -z "$FREE_BRIDGE" ]; then fail "7: 비대기 subnet의 bridge_name을 vpc.subnet.list에서 얻지 못함(오류 또는 null)"; return; fi

    if [ -z "${PCV_VPC_MGMT_LIVE_BRIDGE_DEL_CMD:-}" ]; then
        fail "7: PCV_VPC_MGMT_LIVE_BRIDGE_DEL_CMD(브리지 이름을 인자로 받아 ip link del 하는 명령)가 필요하다"; return
    fi
    bash -c "$PCV_VPC_MGMT_LIVE_BRIDGE_DEL_CMD \"\$1\"" _ "$FREE_BRIDGE" >/dev/null 2>&1
    if bridge_visible "$FREE_BRIDGE"; then fail "7: 재시작 전 브리지 삭제 실패($FREE_BRIDGE)"; return; fi
    since="$(max_id)"
    : > "$FLAG" && FLAG_MADE=1 && chmod 644 "$FLAG"
    restart_daemon; rc=$?
    if [ "$rc" != 0 ]; then fail "7: 플래그와 함께 재시작 실패(rc=$rc)"; return; fi
    skipped="$(uds vpc.status '{}' 10 | jq -r '.result.boot_reconcile_skipped | tostring')"
    inlist="$(subnet_count "$FREE_SUBNET_ID")"

    c="$(audit_exact "$since" "username='system' AND method='vpc.reconcile.skip' AND result='ok'" 1)"
    if [ "$skipped" = "true" ] && [ "$inlist" = "1" ] && [ "$c" = "1" ] &&
       { [ -z "$FREE_BRIDGE" ] || ! bridge_visible "$FREE_BRIDGE"; }; then
        pass "7: 플래그 → boot_reconcile_skipped==true, subnet 행 유지, 브리지 netns에 없음, 감사 vpc.reconcile.skip ok 1행"
    else
        fail "7: skipped='$skipped' list=$inlist bridge='$FREE_BRIDGE' 감사=$c"
    fi
    rm -f "$FLAG"; FLAG_MADE=0
    restart_daemon; rc=$?
    if [ "$rc" != 0 ]; then fail "7: 플래그 제거 뒤 재시작 실패(rc=$rc)"; return; fi
    skipped="$(uds vpc.status '{}' 10 | jq -r '.result.boot_reconcile_skipped | tostring')"
    if [ "$skipped" = "false" ] && { [ -z "$FREE_BRIDGE" ] || bridge_visible "$FREE_BRIDGE"; }; then
        pass "7: 플래그 제거 후 재시작 → 브리지 있음, boot_reconcile_skipped==false"
    else
        fail "7: 플래그 제거 후 skipped='$skipped' bridge='$FREE_BRIDGE'"
    fi
}

check_8() {
    local n
    sleep 1
    n="$(audit_count "$BASE_ID" "username='-'")"
    if [ "$n" = "0" ]; then pass "8: 시험 전체에서 audit_log username='-' 0행"
    else fail "8: username='-' $n 행"; aq "SELECT id||'|'||method||'|'||IFNULL(target,'') FROM audit_log WHERE id > $BASE_ID AND username='-';" | sed 's/^/        /'; fi
}

check_1
check_2
check_3
check_4
check_5
check_6
check_7
check_8

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC} / ${YELLOW}live-only skip %d${NC} (모드: %s)\n" \
    "$PASS" "$FAIL" "$SKIPPED" "$([ "$LIVE" = 1 ] && echo 라이브 || echo 격리)"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if [ "$PASS" -lt 8 ]; then fail "단언이 거의 실행되지 않음(PASS $PASS) — 준비 단계 점검"; fi
[ "$FAIL" -eq 0 ]
