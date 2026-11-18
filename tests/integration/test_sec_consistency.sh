#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_SEC_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} 보안 일관성 E2E: $*"; exit 0; }

command -v bwrap   >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3 >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid  >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq      >/dev/null 2>&1 || skip "jq 미설치"
command -v sqlite3 >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v curl    >/dev/null 2>&1 || skip "curl 미설치"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in "$@"; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port 28241 29241 31241 33241 37241 41241)" || skip "빈 포트 확보 실패"
BASE_URL="http://127.0.0.1:$PORT/api/v1"

ADMIN_PW='SecCons-E2e-Admin-not-for-prod-1'
USER_PW='SecCons-E2e-User-not-for-prod-1'

FAKE_MAIN='SECFAKEMAINTOKEN0000'
FAKE_CRIT='SECFAKECRITTOKEN0000'
FAKE_BODY='SECFAKEBODYVALUE0000'

FAKE_BODY_HEAD="${FAKE_BODY:0:8}"
PW_HEAD='SecCons-E2e-'
WEBHOOK_MAIN="https://hooks.example.invalid/services/$FAKE_MAIN/sendMessage?key=$FAKE_MAIN"
WEBHOOK_CRIT="http://127.0.0.1:9/bot$FAKE_CRIT/sendMessage"
ORIGIN_MAIN='https://hooks.example.invalid'
ORIGIN_CRIT='http://127.0.0.1'
VM_RN='secrn-vm'; VM_RN_NEW='secrn-new'; SG='secrn-sg'

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-seccons.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
chmod 0755 "$STATE"
mkdir -p "$STATE/var-lib/lxc" "$STATE/etc" "$STATE/images" "$STATE/var-log"
SOCK="$STATE/var-lib/daemon.sock"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
RBAC_DB="$STATE/var-lib/rbac.db"
VPC_DB="$STATE/var-lib/vpc.db"
SEC_DB="$STATE/var-lib/pcv_security.db"
DLOG="$STATE/daemon.log"

: > "$STATE/images/$VM_RN.qcow2"
cat > "$STATE/etc/topo.xml" <<XML
<node>
  <domain type="test" xmlns:test="http://libvirt.org/schemas/domain/test/1.0">
    <name>${VM_RN}</name><memory>65536</memory><os><type>hvm</type></os>
    <devices><disk type="file" device="disk"><source file="$STATE/images/$VM_RN.qcow2"/><target dev="vda" bus="virtio"/></disk></devices>
    <test:runstate>5</test:runstate>
  </domain>
</node>
XML
LIBVIRT_URI='test:///etc/purecvisor/topo.xml'

cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $ADMIN_PW
jwt_secret = sec-consistency-e2e-fixed-secret-not-for-prod
libvirt_uri = $LIBVIRT_URI
log_level = info
drain_timeout = 1
pool_max_conn = 2

[auth]
allow_self_register = true

[alert]
enabled = true
cpu_warn = 0
cpu_crit = 99
mem_warn = 0
mem_crit = 99
disk_warn = 80
disk_crit = 95
data_pool_warn = 80
data_pool_crit = 95
eval_period = 5
dedup_window = 300
webhook_url = $WEBHOOK_MAIN
webhook_crit_url = $WEBHOOK_CRIT
webhook_format = generic
telegram_chat_id =
CONF

kill_daemon() {
    local f="$STATE/bwrap.pid" bp
    [ -f "$f" ] || return 0
    bp="$(cat "$f" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    for _ in $(seq 1 12); do
        kill -0 "$bp" 2>/dev/null || break
        sleep 0.25
    done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$f"
}
cleanup() {
    kill_daemon
    rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

iso_curl() { pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" curl "$@"; }
uds() {
    printf '%s' "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"sc\"}" \
        | pcv_iso_uds_nc "$SOCK" "${3:-30}" 2>/dev/null || true
}
rpc() {
    iso_curl -s --max-time 30 -X POST -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"sc\",\"method\":\"$2\",\"params\":$3}" "$BASE_URL/rpc" 2>/dev/null || true
}
rpc_status() {
    iso_curl -s -o /dev/null -w '%{http_code}' --max-time 30 -X POST -H "Authorization: Bearer $1" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"sc\",\"method\":\"$2\",\"params\":$3}" "$BASE_URL/rpc" 2>/dev/null || true
}
get_status() {
    iso_curl -s -o /dev/null -w '%{http_code}' --max-time 30 -H "Authorization: Bearer $1" "$BASE_URL/$2" 2>/dev/null || true
}
get_body() {
    iso_curl -s --max-time 30 -H "Authorization: Bearer $1" "$BASE_URL/$2" 2>/dev/null || true
}
login_token() {
    iso_curl -s --max-time 10 -X POST "$BASE_URL/auth/token" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null \
        | jq -r '.access_token // empty' 2>/dev/null || true
}
aq() { sqlite3 -cmd ".timeout 5000" "$AUDIT_DB" "$1" 2>/dev/null; }
rq() { sqlite3 -cmd ".timeout 5000" "$RBAC_DB" "$1" 2>/dev/null; }
sq() { sqlite3 -cmd ".timeout 5000" "$SEC_DB" "$1" 2>/dev/null; }
max_id() { local v; v="$(aq 'SELECT COALESCE(MAX(id),0) FROM audit_log;')"; echo "${v:-0}"; }
dump_rows() {
    aq "SELECT id||'|'||IFNULL(username,'NULL')||'|'||method||'|'||IFNULL(target,'')||'|'||result||'|'||IFNULL(src_ip,'') FROM audit_log WHERE $1 ORDER BY id;" \
        | sed 's/^/        /'
}

settle() {
    local before after
    uds vm.list '{}' 10 >/dev/null
    before="$(max_id)"
    for _ in $(seq 1 12); do
        sleep 0.25
        after="$(max_id)"
        [ "$after" != "$before" ] && before="$after" && continue
        [ "$after" = "$before" ] && break
    done
}

rows() {
    local n
    settle
    n="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $2 AND $3;")"
    if [ "${n:-x}" = "$4" ]; then
        pass "$1: 행 $n (조건: $3)"
    else
        fail "$1: 행 ${n:-?}(기대 $4) (조건: $3)"
        dump_rows "id > $2 AND method LIKE 'auth.%'"
    fi
}

boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --bind "$STATE/var-log" /var/log/purecvisor \
        --bind "$STATE" "$STATE" \
        --setenv PCV_LIBVIRT_URI "$LIBVIRT_URI" \
        --setenv PURECVISOR_LIBVIRT_URI "$LIBVIRT_URI" \
        --setenv G_MESSAGES_DEBUG all \
        --chdir / \
        "$DAEMON_BIN" > "$DLOG" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local resp
    for _ in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            resp="$(uds vm.list '{}' 10)"
            case "$resp" in
                *'"jsonrpc"'*|*'"result"'*|*'"error"'*)
                    pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE_URL/health" >/dev/null \
                        && return 0 ;;
            esac
        fi
        sleep 0.5
    done
    return 1
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  보안 일관성(행위자·역할·테넌트·웹훅) 격리 E2E  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if ! boot; then
    fail "S1: 격리 데몬 기동 실패"
    tail -30 "$DLOG" 2>/dev/null
    exit 1
fi
pass "S1: 격리 데몬 기동"

TOK_ADMIN="$(login_token admin "$ADMIN_PW")"
[ -n "$TOK_ADMIN" ] && pass "S2: admin 로그인" || { fail "S2: admin 로그인 실패"; exit 1; }

rows "부트스트랩" 0 "method='auth.user.create' AND target='admin' AND username='-' AND result='ok' AND src_ip='bootstrap'" 1
rows "부트스트랩 총합" 0 "method='auth.user.create' AND target='admin'" 1

M="$(max_id)"
r="$(rpc "$TOK_ADMIN" auth.user.create "{\"username\":\"scviewer\",\"password\":\"$USER_PW\",\"role\":\"viewer\",\"tenant\":\"ta\"}")"
rows "생성(REST admin)" "$M" "method='auth.user.create' AND target='scviewer' AND username='admin' AND result='ok'" 1
rows "생성 총합" "$M" "method='auth.user.create'" 1

M="$(max_id)"
r="$(rpc "$TOK_ADMIN" auth.user.create "{\"username\":\"scviewer\",\"password\":\"$USER_PW\",\"role\":\"viewer\"}")"
rows "중복 생성 실패" "$M" "method='auth.user.create' AND username='admin' AND result='fail'" 1
rows "중복 생성 총합" "$M" "method='auth.user.create'" 1

for spec in scoper:operator:ta scoperb:operator:tb scviewernt:viewer: scopernt:operator: scdel:viewer: ; do
    u="${spec%%:*}"; rest="${spec#*:}"; role="${rest%%:*}"; ten="${rest#*:}"
    tjson=""; [ -n "$ten" ] && tjson=",\"tenant\":\"$ten\""
    rpc "$TOK_ADMIN" auth.user.create "{\"username\":\"$u\",\"password\":\"$USER_PW\",\"role\":\"$role\"$tjson}" >/dev/null
done

M="$(max_id)"
rpc "$TOK_ADMIN" auth.user.delete '{"username":"scdel"}' >/dev/null
rows "삭제" "$M" "method='auth.user.delete' AND target='scdel' AND username='admin' AND result='ok'" 1
rows "삭제 총합" "$M" "method='auth.user.delete'" 1

M="$(max_id)"
rpc "$TOK_ADMIN" auth.user.delete '{"username":"nobody"}' >/dev/null
rows "없는 사용자 삭제 실패" "$M" "method='auth.user.delete' AND target='nobody' AND username='admin' AND result='fail'" 1
rows "없는 사용자 삭제 총합" "$M" "method='auth.user.delete'" 1

rpc "$TOK_ADMIN" auth.user.create "{\"username\":\"scrole\",\"password\":\"$USER_PW\",\"role\":\"viewer\"}" >/dev/null
M="$(max_id)"
rpc "$TOK_ADMIN" auth.role.set '{"username":"scrole","role":"operator"}' >/dev/null
rows "역할 변경" "$M" "method='auth.role.set' AND target='scrole' AND username='admin' AND result='ok' AND src_ip='viewer->operator'" 1
rows "역할 변경 총합" "$M" "method='auth.role.set'" 1
rows "auth.role.change 없음" "$M" "method='auth.role.change'" 0

M="$(max_id)"
r="$(rpc "$TOK_ADMIN" auth.role.set '{"username":"admin","role":"viewer"}')"
rows "자기 역할 변경 거부" "$M" "method='auth.role.set' AND target='admin' AND username='admin' AND result='denied'" 1
rows "자기 역할 변경 총합" "$M" "method='auth.role.set'" 1
if [ "$(rq "SELECT role FROM users WHERE username='admin';")" = "2" ]; then
    pass "자기 역할 변경 뒤 admin 역할 그대로"
else
    fail "자기 역할 변경이 역할을 바꿨다: $(rq "SELECT role FROM users WHERE username='admin';") — ${r:-<empty>}"
fi
[ "$(printf '%s' "$r" | jq -r '.error.code // empty' 2>/dev/null)" = "-32602" ] && pass "자기 역할 변경 -32602" || fail "자기 역할 변경 응답 — $(printf '%s' "$r" | cut -c1-160)"
s="$(rpc_status "$TOK_ADMIN" auth.role.set '{"username":"admin","role":"viewer"}')"
[ "$s" = "400" ] && pass "자기 역할 변경 HTTP 400" || fail "자기 역할 변경 HTTP $s"

TOK_VIEWER="$(login_token scviewer "$USER_PW")"
TOK_OPER="$(login_token scoper "$USER_PW")"
TOK_OPERB="$(login_token scoperb "$USER_PW")"
TOK_VIEWERNT="$(login_token scviewernt "$USER_PW")"
TOK_OPERNT="$(login_token scopernt "$USER_PW")"
if [ -n "$TOK_VIEWER" ] && [ -n "$TOK_OPER" ] && [ -n "$TOK_OPERB" ] && [ -n "$TOK_VIEWERNT" ] && [ -n "$TOK_OPERNT" ]; then
    pass "S3: 역할별 사용자 로그인"
else
    fail "S3: 역할별 사용자 로그인 실패"
fi

M="$(max_id)"
s="$(rpc_status "$TOK_VIEWER" auth.user.create "{\"username\":\"scnope\",\"password\":\"$USER_PW\",\"role\":\"viewer\"}")"
[ "$s" = "403" ] && pass "VIEWER 생성 시도 403" || fail "VIEWER 생성 시도 HTTP $s"
rows "VIEWER 거부 1행" "$M" "method='auth.user.create' AND username='scviewer' AND result='denied'" 1
rows "VIEWER 거부 총합(대상 원문 없음)" "$M" "method='auth.user.create' AND target=''" 1

M="$(max_id)"
uds auth.user.create "{\"username\":\"scuds\",\"password\":\"$USER_PW\",\"role\":\"viewer\"}" >/dev/null
rows "UDS 생성 행위자" "$M" "method='auth.user.create' AND target='scuds' AND username='local-admin' AND result='ok'" 1
rows "UDS 생성 총합" "$M" "method='auth.user.create'" 1

M="$(max_id)"
s="$(iso_curl -s -o /dev/null -w '%{http_code}' --max-time 15 -X POST "$BASE_URL/auth/register" -H 'Content-Type: application/json' \
    -d "{\"username\":\"scself\",\"password\":\"$USER_PW\"}" 2>/dev/null || true)"
info "자기 가입 HTTP $s"
rows "자기 가입 감사 유지" "$M" "method='auth.register' AND result='ok' AND username='scself'" 1
rows "자기 가입 중복 없음" "$M" "method='auth.user.create'" 0

uds auth.apikey.create '{"name":"sckey","role":0}' >/dev/null
for n in grpc sckey; do
    M="$(max_id)"
    s="$(iso_curl -s -o /dev/null -w '%{http_code}' --max-time 15 -X POST "$BASE_URL/auth/register" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$n\",\"password\":\"$USER_PW\"}" 2>/dev/null || true)"
    [ "$s" = "409" ] && pass "자기 가입 '$n' 409" || fail "자기 가입 '$n' HTTP $s (기대 409)"
    rows "자기 가입 '$n' 실패 감사" "$M" "method='auth.register' AND username='$n' AND result='fail' AND error_code=409" 1
    rows "자기 가입 '$n' 생성 없음" "$M" "method='auth.user.create'" 0
done

M="$(max_id)"
iso_curl -s -o /dev/null --max-time 15 -X POST "$BASE_URL/auth/refresh" -H 'Content-Type: application/json' \
    -d '{"refresh_token":"not-a-real-refresh-token"}' 2>/dev/null || true
rows "무효 refresh 행위자 -" "$M" "method='auth.token.refresh' AND username='-' AND result='invalid token'" 1

r="$(uds auth.apikey.create '{"name":"sc-bot","role":1}')"
KEY="$(printf '%s' "$r" | jq -r '.result.api_key // .result.key // empty' 2>/dev/null)"
if [ -z "$KEY" ]; then
    fail "전제: API 키 발급 실패(응답 본문은 출력하지 않는다)"
else
    v="$(rq "SELECT IFNULL(last_used_at,'') FROM api_keys WHERE client_name='sc-bot';")"
    [ -z "$v" ] && pass "발급 직후 last_used_at 비어 있음" || fail "발급 직후 값: $v"
    key_call() { iso_curl -s -o /dev/null -w '%{http_code}' --max-time 15 -H "X-API-Key: $KEY" "$BASE_URL/vms" 2>/dev/null || true; }
    s="$(key_call)"
    v="$(rq "SELECT IFNULL(last_used_at,'') FROM api_keys WHERE client_name='sc-bot';")"
    [ -n "$v" ] && pass "API 키 호출(HTTP $s) 뒤 last_used_at=$v" || fail "API 키 호출(HTTP $s) 뒤 last_used_at 비어 있음"
    rq "UPDATE api_keys SET last_used_at=datetime('now','-30 seconds') WHERE client_name='sc-bot';"
    v1="$(rq "SELECT last_used_at FROM api_keys WHERE client_name='sc-bot';")"
    key_call >/dev/null
    v2="$(rq "SELECT last_used_at FROM api_keys WHERE client_name='sc-bot';")"
    [ "$v1" = "$v2" ] && pass "60초 창 안 재호출은 쓰지 않음" || fail "60초 창 안 재호출이 썼다: $v1 → $v2"
    rq "UPDATE api_keys SET last_used_at=datetime('now','-61 seconds') WHERE client_name='sc-bot';"
    v1="$(rq "SELECT last_used_at FROM api_keys WHERE client_name='sc-bot';")"
    key_call >/dev/null
    v2="$(rq "SELECT last_used_at FROM api_keys WHERE client_name='sc-bot';")"
    [ "$v1" != "$v2" ] && pass "창 밖 재호출은 갱신" || fail "창 밖 재호출이 갱신하지 않음: $v1"
    l="$(uds auth.apikey.list '{}' | jq -r '[.result[]? | select(.client_name=="sc-bot") | .last_used_at] | first // empty' 2>/dev/null)"
    [ -n "$l" ] && pass "auth.apikey.list에 last_used_at 표시" || fail "auth.apikey.list에 last_used_at 없음"
fi

M="$(max_id)"
r="$(rpc "$TOK_ADMIN" security.config.set '{"enabled":true,"_pcv_username":"mallory"}')"
info "security.config.set 응답 키: $(printf '%s' "$r" | jq -c 'keys' 2>/dev/null)"
rows "security.config.set 행위자는 인증 주체" "$M" "method='security.config.set' AND username='admin'" 1
rows "위조 행위자 행 없음" "$M" "username='mallory'" 0

SEC_EVENT='sc-dismiss-event'
now="$(date +%s)"
sq "INSERT INTO security_actions(event_id,action,target_kind,target,status,ttl_sec,expires_at,requested_at) VALUES('$SEC_EVENT','block_ip','ip','192.0.2.1','pending',3600,$((now + 3600)),$now);"
rpc "$TOK_OPER" security.action.dismiss "{\"event_id\":\"$SEC_EVENT\",\"reason\":\"sc\",\"_pcv_username\":\"mallory\"}" >/dev/null
v="$(sq "SELECT status||'|'||decided_by FROM security_actions WHERE event_id='$SEC_EVENT';")"
[ "$v" = "dismissed|scoper" ] && pass "OPERATOR 기각 행위자는 인증 주체(위조 무효)" || fail "OPERATOR 기각 행(status|decided_by) '$v', 기대 dismissed|scoper"

for c in sc-mine:scoper sc-theirs:scoperb; do
    mkdir -p "$STATE/var-lib/lxc/${c%%:*}"
    printf 'lxc.uts.name = %s\n' "${c%%:*}" > "$STATE/var-lib/lxc/${c%%:*}/config"
    printf '%s' "${c#*:}" > "$STATE/var-lib/lxc/${c%%:*}/purecvisor.owner"
done
DENY_MSG='Permission denied: operators can access only containers they created'
for meth in container.memory.stats container.health.check; do
    M="$(max_id)"
    r="$(rpc "$TOK_OPER" "$meth" '{"name":"sc-theirs"}')"
    if printf '%s' "$r" | grep -qF "$DENY_MSG"; then
        pass "OPERATOR $meth 남의 컨테이너 거부"
    else
        fail "OPERATOR $meth 남의 컨테이너 — $(printf '%s' "$r" | cut -c1-200)"
    fi
    rows "$meth 거부 1행" "$M" "method='$meth' AND target='sc-theirs' AND username='scoper' AND result='denied'" 1
    r="$(rpc "$TOK_OPER" "$meth" '{"name":"sc-mine"}')"
    if printf '%s' "$r" | grep -qF "$DENY_MSG"; then
        fail "OPERATOR $meth 자기 컨테이너가 거부됨"
    else
        pass "OPERATOR $meth 자기 컨테이너 통과"
    fi
done
r="$(rpc "$TOK_VIEWER" container.metrics '{"name":"sc-theirs"}')"
if printf '%s' "$r" | grep -qF 'Permission denied'; then
    fail "container.metrics가 VIEWER에 막혔다(container.metrics는 vm.metrics처럼 VIEWER 전체 조회) — $(printf '%s' "$r" | cut -c1-160)"
else
    pass "container.metrics는 VIEWER 전체 조회 유지"
fi

VPC_ID='11111111-2222-4333-8444-555555555555'
sqlite3 -cmd ".timeout 5000" "$VPC_DB" \
    "INSERT INTO vpcs(id,name,tenant,egress_mode,backend,state) VALUES('$VPC_ID','sc-vpc','ta','isolated','linux','ACTIVE');" 2>/dev/null
vpc_list_has() { printf '%s' "$1" | grep -qF "$VPC_ID"; }
for who in VIEWERNT OPERNT; do
    tok_var="TOK_$who"; tok="${!tok_var}"
    s="$(get_status "$tok" vpcs)"
    b="$(get_body "$tok" vpcs)"
    if [ "$s" = "409" ] && ! vpc_list_has "$b" && printf '%s' "$b" | grep -qF 'tenant가 필요합니다'; then
        pass "tenant 없는 $who vpc.list 409 + tenant 오류"
    else
        fail "tenant 없는 $who vpc.list HTTP $s — $(printf '%s' "$b" | cut -c1-200)"
    fi
    s="$(get_status "$tok" vpcs/status)"
    [ "$s" = "409" ] && pass "tenant 없는 $who vpc.status 409" || fail "tenant 없는 $who vpc.status HTTP $s"
    s="$(get_status "$tok" "vpcs/$VPC_ID")"
    [ "$s" = "409" ] && pass "tenant 없는 $who vpc.get 409" || fail "tenant 없는 $who vpc.get HTTP $s"
done
b="$(get_body "$TOK_VIEWER" vpcs)"
vpc_list_has "$b" && pass "tenant ta VIEWER는 자기 VPC를 본다" || fail "tenant ta VIEWER 목록 — $(printf '%s' "$b" | cut -c1-200)"
b="$(get_body "$TOK_OPERB" vpcs)"
if ! vpc_list_has "$b" && [ "$(get_status "$TOK_OPERB" vpcs)" = "200" ]; then
    pass "tenant tb OPERATOR는 ta VPC를 보지 못하고 빈 목록"
else
    fail "tenant tb OPERATOR 목록 — $(printf '%s' "$b" | cut -c1-200)"
fi
b="$(get_body "$TOK_ADMIN" vpcs)"
vpc_list_has "$b" && pass "admin 전역 조회 유지" || fail "admin 목록 — $(printf '%s' "$b" | cut -c1-200)"

if [ -n "${KEY:-}" ]; then
    key_vpcs() { iso_curl -s -w '\n%{http_code}' --max-time 15 -H "X-API-Key: $KEY" "$BASE_URL/vpcs" 2>/dev/null || true; }
    b="$(key_vpcs)"
    if [ "$(printf '%s' "$b" | tail -1)" = "409" ] && printf '%s' "$b" | grep -qF 'tenant가 필요합니다'; then
        pass "사용자 없는 OPERATOR 키 vpc.list 409 + tenant 오류"
    else
        fail "사용자 없는 OPERATOR 키 vpc.list — $(printf '%s' "$b" | cut -c1-200)"
    fi
    r="$(uds auth.user.tenant.set '{"username":"sc-bot","tenant":"ta"}')"
    if [ "$(printf '%s' "$r" | jq -r '.error.code // empty' 2>/dev/null)" = "-32005" ] && printf '%s' "$r" | grep -qF "User 'sc-bot' not found"; then
        pass "사용자 없는 키에 auth.user.tenant.set은 -32005"
    else
        fail "사용자 없는 키에 auth.user.tenant.set — $(printf '%s' "$r" | cut -c1-200)"
    fi
    s="$(rpc_status "$TOK_ADMIN" auth.user.tenant.set '{"username":"sc-bot","tenant":"ta"}')"
    [ "$s" = "404" ] && pass "같은 요청 REST는 404" || fail "같은 요청 REST HTTP $s"

    r="$(uds auth.user.create "{\"username\":\"sc-bot\",\"password\":\"$USER_PW\",\"role\":\"viewer\",\"tenant\":\"ta\"}")"
    if [ "$(printf '%s' "$r" | jq -r '.error.code // empty' 2>/dev/null)" = "-32002" ] && printf '%s' "$r" | grep -qF 'username is already used by an API key'; then
        pass "키 이름과 같은 사용자 생성은 −32002"
    else
        fail "키 이름과 같은 사용자 생성 — $(printf '%s' "$r" | cut -c1-200)"
    fi
    b="$(key_vpcs)"
    if [ "$(printf '%s' "$b" | tail -1)" = "409" ] && printf '%s' "$b" | grep -qF 'tenant가 필요합니다'; then
        pass "키는 여전히 tenant 없음(같은 이름 사용자 테넌트를 빌리지 않는다)"
    else
        fail "같은 이름 사용자 시도 뒤 키 vpc.list — $(printf '%s' "$b" | cut -c1-200)"
    fi
fi

r="$(uds security_group.create "{\"name\":\"$SG\",\"description\":\"sc\"}")"
info "security_group.create: $(printf '%s' "$r" | cut -c1-120)"
r="$(uds vm.security_group.set "{\"vm\":\"$VM_RN\",\"security_group\":\"$SG\"}")"
if [ "$(printf '%s' "$r" | jq -r '.result' 2>/dev/null)" != "true" ]; then
    fail "전제: SG 부착 실패 — $(printf '%s' "$r" | cut -c1-200)"
else
    pass "전제: SG 부착"
    s="$(rpc_status "$TOK_ADMIN" vm.rename "{\"name\":\"$VM_RN\",\"new_name\":\"$VM_RN_NEW\"}")"
    [ "$s" = "409" ] && pass "SG 바인딩 VM rename 409" || fail "SG 바인딩 VM rename HTTP $s"
    r="$(uds vm.rename "{\"name\":\"$VM_RN\",\"new_name\":\"$VM_RN_NEW\"}")"
    if printf '%s' "$r" | grep -qF 'security group binding'; then
        pass "거부 메시지가 해제를 안내"
    else
        fail "거부 메시지 — $(printf '%s' "$r" | cut -c1-200)"
    fi

    VM_UUID="$(uds vm.list '{}' | jq -r --arg n "$VM_RN" '[(.result | if type=="object" then (.vms // .data // []) else . end)[]? | select(.name==$n) | .uuid] | first // empty' 2>/dev/null)"
    if [ -n "$VM_UUID" ]; then
        r="$(uds vm.rename "{\"name\":\"$VM_UUID\",\"new_name\":\"$VM_RN_NEW\"}")"
        if printf '%s' "$r" | grep -qF 'security group binding'; then
            pass "UUID 지정 rename도 바인딩으로 거부"
        else
            fail "UUID 지정 rename — $(printf '%s' "$r" | cut -c1-200)"
        fi
    else
        fail "전제: vm.list에서 UUID를 못 읽음"
    fi
    if [ -e "$STATE/images/$VM_RN.qcow2" ] && [ ! -e "$STATE/images/$VM_RN_NEW.qcow2" ]; then
        pass "거부 뒤 디스크 그대로"
    else
        fail "거부 뒤 디스크가 옮겨졌다"
    fi
    r="$(uds security_group.detach "{\"vm\":\"$VM_RN\",\"name\":\"$SG\"}")"
    r="$(uds vm.rename "{\"name\":\"$VM_RN\",\"new_name\":\"$VM_RN_NEW\"}")"
    if [ "$(printf '%s' "$r" | jq -r 'has("result") and (.error == null)' 2>/dev/null)" = "true" ] \
        && [ -e "$STATE/images/$VM_RN_NEW.qcow2" ]; then
        pass "해제 뒤 rename 성공 + 디스크 이동"
    else
        fail "해제 뒤 rename — $(printf '%s' "$r" | cut -c1-240)"
    fi
fi

assert_origin() {
    if [ "$2" = "$3" ]; then pass "$1: $2"; else fail "$1: '$(printf '%s' "$2" | cut -c1-80)' (기대 $3)"; fi
}
for who in VIEWER OPER; do
    tok_var="TOK_$who"; tok="${!tok_var}"
    b="$(get_body "$tok" alerts/config)"
    assert_origin "$who alert.config.get webhook_url" "$(printf '%s' "$b" | jq -r '.data.webhook_url // .webhook_url // empty')" "$ORIGIN_MAIN"
    assert_origin "$who alert.config.get webhook_crit_url" "$(printf '%s' "$b" | jq -r '.data.webhook_crit_url // .webhook_crit_url // empty')" "$ORIGIN_CRIT"
    if printf '%s' "$b" | grep -qE "$FAKE_MAIN|$FAKE_CRIT"; then fail "$who 응답에 토큰"; else pass "$who 응답에 토큰 없음"; fi
    b="$(rpc "$tok" alert.config.get '{"_pcv_caller_role":2}')"
    if printf '%s' "$b" | grep -qE "$FAKE_MAIN|$FAKE_CRIT"; then fail "$who 역할 위조로 원문 수신"; else pass "$who 본문 역할 위조 무효"; fi
    b="$(rpc "$tok" alert.action.list '{}')"
    if printf '%s' "$b" | grep -qE "$FAKE_MAIN|$FAKE_CRIT"; then fail "$who alert.action.list에 토큰"; else pass "$who alert.action.list 토큰 없음"; fi
    assert_origin "$who alert.action.list url" "$(printf '%s' "$b" | jq -r '[.data.actions[]? | select(.type=="webhook") | .url] | first // empty')" "$ORIGIN_MAIN"
    b="$(rpc "$tok" vm.event.webhook.list '{}')"
    if printf '%s' "$b" | grep -qE "$FAKE_MAIN|$FAKE_CRIT"; then fail "$who vm.event.webhook.list에 토큰"; else pass "$who vm.event.webhook.list 토큰 없음"; fi
    s="$(get_status "$tok" webhook/dlq)"
    [ "$s" = "403" ] && pass "$who webhook.dlq.list 403" || fail "$who webhook.dlq.list HTTP $s"
    s="$(rpc_status "$tok" alert.dlq.list '{}')"
    [ "$s" = "403" ] && pass "$who alert.dlq.list 403" || fail "$who alert.dlq.list HTTP $s"
done
b="$(get_body "$TOK_ADMIN" alerts/config)"
assert_origin "ADMIN alert.config.get 원문" "$(printf '%s' "$b" | jq -r '.data.webhook_url // .webhook_url // empty')" "$WEBHOOK_MAIN"
assert_origin "ADMIN crit 원문" "$(printf '%s' "$b" | jq -r '.data.webhook_crit_url // .webhook_crit_url // empty')" "$WEBHOOK_CRIT"
REV="$(printf '%s' "$b" | jq -r '.data.config_revision // .config_revision // empty')"
s="$(get_status "$TOK_ADMIN" webhook/dlq)"
[ "$s" = "200" ] && pass "ADMIN webhook.dlq.list 200" || fail "ADMIN webhook.dlq.list HTTP $s"
b="$(uds alert.config.get '{}')"
assert_origin "UDS(ADMIN) 원문" "$(printf '%s' "$b" | jq -r '.result.webhook_url // empty')" "$WEBHOOK_MAIN"

put_alert() {
    iso_curl -s -w '\n%{http_code}' --max-time 15 -X PUT -H "Authorization: Bearer $1" \
        -H 'Content-Type: application/json' -d "$2" "$BASE_URL/alerts/config" 2>/dev/null || true
}
with_rev() {
    printf '%s' "$1" | jq -c --argjson r "$2" '.expected_revision=$r'
}
alert_rev() { get_body "$TOK_ADMIN" alerts/config | jq -r '.data.config_revision // .config_revision // empty'; }
SAVE_URL="http://127.0.0.1:9/$FAKE_BODY"
SAVE_SECRET="${FAKE_BODY}hmac"
get_body "$TOK_ADMIN" alerts/config | jq -c '.data // .' > "$STATE/alert-config.json"
BODIES=""
if command -v node >/dev/null 2>&1 && [ -e "$REPO/node_modules/puppeteer" ]; then

    if [ "$(id -u)" = 0 ] && [ -n "${SUDO_USER:-}" ] && [ -z "${PUPPETEER_CACHE_DIR:-}" ]; then
        export PUPPETEER_CACHE_DIR="$(getent passwd "$SUDO_USER" | cut -d: -f6)/.cache/puppeteer"
    fi
    BODIES="$(cd "$REPO" && timeout 120 node tests/ui/alert-config-bodies.mjs "$STATE/alert-config.json" \
        "$SAVE_URL" "$WEBHOOK_CRIT" "$SAVE_SECRET" 2>/dev/null || true)"
    [ -n "$BODIES" ] && pass "D10 monitor.js 저장 경로에서 실제 본문 추출" || fail "D10 monitor.js 저장 경로 본문 추출 실패"
else
    info "D10 puppeteer 없음 — monitor-alerts.test.mjs가 고정한 키로 본문을 조립한다"
fi
if [ -z "$BODIES" ]; then
    BODIES="$(jq -nc --arg u "$SAVE_URL" --arg c "$WEBHOOK_CRIT" --arg s "$SAVE_SECRET" --slurpfile cfg "$STATE/alert-config.json" '
        { routing: { webhook_url: $u, webhook_crit_url: $c, webhook_secret: $s },
          form: ($cfg[0] | { enabled, cpu_warn, cpu_crit, mem_warn, mem_crit, disk_warn: (.disk_warn - 1), disk_crit,
                             eval_period, webhook_url, webhook_format, telegram_chat_id }) }')"
fi
ROUTING_BODY="$(printf '%s' "$BODIES" | jq -c '.routing')"
FORM_BODY="$(printf '%s' "$BODIES" | jq -c '.form')"

r="$(put_alert "$TOK_ADMIN" "$(with_rev "$ROUTING_BODY" "$REV")")"
[ "$(printf '%s' "$r" | tail -1)" = "200" ] && pass "D10 ADMIN 라우팅 본문 PUT 200" || fail "D10 ADMIN 라우팅 본문 PUT HTTP $(printf '%s' "$r" | tail -1)"
b="$(get_body "$TOK_ADMIN" alerts/config)"
assert_origin "D10 라우팅 저장 값 반영" "$(printf '%s' "$b" | jq -r '.data.webhook_url // .webhook_url // empty')" "$SAVE_URL"
if printf '%s' "$b" | grep -qF "$SAVE_SECRET"; then fail "D10 응답에 HMAC 비밀"; else pass "D10 응답에 HMAC 비밀 없음"; fi
REV2="$(printf '%s' "$b" | jq -r '.data.config_revision // .config_revision // empty')"
[ "$REV2" = "$((REV + 1))" ] && pass "D10 저장 뒤 revision +1" || fail "D10 저장 뒤 revision $REV2 (이전 $REV)"

FORM_DISK="$(printf '%s' "$FORM_BODY" | jq -r '.disk_warn')"
r="$(put_alert "$TOK_ADMIN" "$(with_rev "$FORM_BODY" "$REV2")")"
[ "$(printf '%s' "$r" | tail -1)" = "200" ] && pass "D10 ADMIN 설정 폼(#al-*) 본문 PUT 200" || fail "D10 ADMIN 설정 폼 본문 PUT HTTP $(printf '%s' "$r" | tail -1)"
assert_origin "D10 설정 폼 저장 값 반영" "$(get_body "$TOK_ADMIN" alerts/config | jq -r '(.data // .).disk_warn')" "$FORM_DISK"

REV="$(alert_rev)"
s="$(printf '%s' "$(put_alert "$TOK_ADMIN" "{\"cpu_wran\":1,\"expected_revision\":$REV}")" | tail -1)"
[ "$s" = "400" ] && pass "D10 모르는 키 PUT 400" || fail "D10 모르는 키 PUT HTTP $s"
[ "$(alert_rev)" = "$REV" ] && pass "D10 거부 뒤 revision 그대로" || fail "D10 거부 뒤 revision 변경"

s="$(printf '%s' "$(put_alert "$TOK_ADMIN" "$(with_rev "$ROUTING_BODY" $((REV - 1)))")" | tail -1)"
[ "$s" = "409" ] && pass "D10 낡은 revision PUT 409" || fail "D10 낡은 revision PUT HTTP $s"

for who in VIEWER OPER; do
    tok_var="TOK_$who"
    s="$(printf '%s' "$(put_alert "${!tok_var}" "$(with_rev "$ROUTING_BODY" "$REV")")" | tail -1)"
    [ "$s" = "403" ] && pass "D10 $who 라우팅 본문 PUT 403" || fail "D10 $who 라우팅 본문 PUT HTTP $s"
done
[ "$(alert_rev)" = "$REV" ] && pass "D10 비ADMIN 거부 뒤 revision 그대로" || fail "D10 비ADMIN 거부 뒤 revision 변경"
b="$(uds alert.config.set "{\"expected_revision\":$REV,\"webhook_url\":\"http://127.0.0.1:9/$FAKE_BODY\"}")"
if [ "$(printf '%s' "$b" | jq -r '.result.webhook_url // empty')" = "http://127.0.0.1:9/$FAKE_BODY" ]; then
    pass "UDS(ADMIN) 저장은 원문을 저장·반환"
else
    fail "UDS(ADMIN) 저장 — $(printf '%s' "$b" | cut -c1-120)"
fi
assert_origin "저장 뒤 VIEWER 조회" "$(get_body "$TOK_VIEWER" alerts/config | jq -r '.data.webhook_url // .webhook_url // empty')" "$ORIGIN_CRIT"
s="$(rpc_status "$TOK_VIEWER" alert.config.set "{\"expected_revision\":1,\"webhook_url\":\"$ORIGIN_MAIN\"}")"
[ "$s" = "403" ] && pass "VIEWER 설정 저장 403(가린 URL로 덮어쓰기 불가)" || fail "VIEWER 설정 저장 HTTP $s"
s="$(rpc_status "$TOK_OPER" alert.config.set "{\"expected_revision\":1,\"webhook_url\":\"$ORIGIN_MAIN\"}")"
[ "$s" = "403" ] && pass "OPERATOR 설정 저장 403" || fail "OPERATOR 설정 저장 HTTP $s"

RETRIED=0
for _ in $(seq 1 80); do
    if grep -qF 'Webhook retry' "$DLOG" 2>/dev/null && grep -qF 'Webhook DLQ stored' "$DLOG" 2>/dev/null; then RETRIED=1; break; fi
    sleep 0.5
done
if [ "$RETRIED" = 1 ]; then
    pass "재시도·DLQ 경로 실행됨"
    if ! grep -F 'Webhook retry' "$DLOG" | grep -qvE 'failed for https?://[^/" ]+"'; then
        pass "재시도 로그는 모두 scheme+host"
    else
        fail "재시도 로그 — $(grep -F 'Webhook retry' "$DLOG" | grep -vE 'failed for https?://[^/" ]+"' | head -1 | cut -c1-200)"
    fi
    n="$(get_body "$TOK_ADMIN" webhook/dlq | jq -r '(.data // .) | length' 2>/dev/null)"
    [ "${n:-0}" -ge 1 ] && pass "ADMIN DLQ에 항목 $n" || fail "ADMIN DLQ 항목 ${n:-?}"
else

    fail "재시도·DLQ 경로가 40초 안에 실행되지 않음 — 재시도 로그를 확인하지 못했다"
fi
LOGS=("$DLOG"); [ -f "$STATE/var-log/audit.log" ] && LOGS+=("$STATE/var-log/audit.log")
if grep -qF "Alert engine started" "$DLOG"; then
    pass "전제: 시작 로그 수집됨"
    if grep -F "Alert engine started" "$DLOG" | grep -qF "webhook=$ORIGIN_MAIN,"; then
        pass "시작 로그는 scheme+host"
    else
        fail "시작 로그 — $(grep -F 'Alert engine started' "$DLOG" | cut -c1-200)"
    fi
else
    fail "전제: 시작 로그 없음"
fi
if grep -qE "$FAKE_MAIN|$FAKE_CRIT|$FAKE_BODY_HEAD" "${LOGS[@]}"; then
    fail "로그에 가짜 토큰 원문 — 줄 $(grep -cE "$FAKE_MAIN|$FAKE_CRIT|$FAKE_BODY_HEAD" "${LOGS[@]}" | tr '\n' ' ')"
    grep -hE "$FAKE_MAIN|$FAKE_CRIT|$FAKE_BODY_HEAD" "${LOGS[@]}" | cut -c1-220 | sed 's/^/        /' | head -8
else
    pass "데몬 로그·감사 로그 전체에 토큰 표지 없음"
fi

s="$(iso_curl -s -o /dev/null -w '%{http_code}' --max-time 15 -X POST -H "Authorization: Bearer $TOK_ADMIN" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"sclog\",\"password\":\"$USER_PW\",\"role\":\"viewer\"}" "$BASE_URL/auth/users" 2>/dev/null || true)"
info "REST POST /auth/users HTTP $s"
n="$(grep -cF "$PW_HEAD" "${LOGS[@]}" | awk -F: '{t+=$NF} END {print t+0}')"
[ "$n" = "0" ] && pass "데몬 로그·감사 로그 전체에 비밀번호 표지 없음" || fail "로그에 비밀번호 표지 $n줄(내용은 출력하지 않는다)"

if grep -qE '→ RPC auth\.user\.create("|$)' "$DLOG" && grep -qE '→ RPC alert\.config\.set("|$)' "$DLOG"; then
    pass "→ RPC 로그는 method만 남는다(auth.user.create·alert.config.set)"
else
    fail "→ RPC 로그에 method만 남은 줄 없음"
fi
n="$(grep -F '→ RPC ' "$DLOG" | grep -cvE '→ RPC [A-Za-z0-9_.-]+("|$)')"
[ "$n" = "0" ] && pass "→ RPC 로그에 요청 본문 조각 없음" || fail "→ RPC 로그에 본문 조각 $n줄(내용은 출력하지 않는다)"
if aq "SELECT target||result||IFNULL(src_ip,'') FROM audit_log;" | grep -qE "$FAKE_MAIN|$FAKE_CRIT|$FAKE_BODY_HEAD"; then
    fail "감사 원장에 토큰 표지"
else
    pass "감사 원장에 토큰 표지 없음"
fi

n="$(aq "SELECT COUNT(*) FROM audit_log WHERE username='-' AND method IN ('auth.user.create','auth.user.delete','auth.role.set') AND IFNULL(src_ip,'')<>'bootstrap';")"
[ "${n:-x}" = "0" ] && pass "사용자 관리 행의 행위자 - 0(부트스트랩 제외)" || fail "행위자 - 행 ${n:-?}"

echo ""
echo -e "${CYAN}결과: PASS=$PASS FAIL=$FAIL${NC}"
[ "$FAIL" -eq 0 ]
