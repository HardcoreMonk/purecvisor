#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"
PCVCTL_BIN="$REPO/bin/pcvctl"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} security.event.close 격리 E2E: $*"; exit 0; }

for tool in bwrap sqlite3 curl python3 jq setsid; do
    command -v "$tool" >/dev/null 2>&1 || skip "$tool 미설치"
done
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
if [ ! -x "$DAEMON_BIN" ] || [ ! -x "$PCVCTL_BIN" ]; then
    info "데몬·pcvctl 바이너리 없음 — make daemon cli 시도"
    make -C "$REPO" daemon cli >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
[ -x "$PCVCTL_BIN" ] || skip "pcvctl 바이너리 빌드 실패 ($PCVCTL_BIN)"
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
PORT="$(pick_free_port 28226 29226 31226 33226 37226 41226)" || skip "빈 포트 확보 실패"
BASE_URL="http://127.0.0.1:$PORT/api/v1"

ADMIN_PW='SecCloseE2eAdminPw-not-for-prod'
VIEWER_PW='SecCloseE2eViewerPw-not-for-prod1'
OPERATOR_PW='SecCloseE2eOperPw-not-for-prod1'
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-secclose.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"
SEC_DB="$STATE/var-lib/pcv_security.db"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
DLOG="$STATE/daemon.log"

cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $ADMIN_PW
jwt_secret = sec-close-e2e-fixed-secret-not-for-prod-01
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
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
pcvctl_iso() {
    [ -S "$SOCK" ] || return 125
    "$PCVCTL_BIN" --socket="$SOCK" "$@"
}
login_token() {
    iso_curl -s --max-time 6 -X POST "$BASE_URL/auth/token" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null | jq -r '.access_token // empty' 2>/dev/null || true
}
uds() {
    printf '%s' "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"sec-close\"}" \
        | pcv_iso_uds_nc "$SOCK" 30 2>/dev/null || true
}

rest_rpc() {
    iso_curl -s -w '\n%{http_code}' --max-time 15 -X POST -H "$1" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":\"sec-close\",\"method\":\"$2\",\"params\":$3}" "$BASE_URL/rpc" 2>/dev/null \
        || printf '\n000'
}
http_code() { printf '%s' "${1##*$'\n'}"; }
http_body() { printf '%s' "${1%$'\n'*}"; }
ecode() { printf '%s' "$1" | jq -r '.error.code // empty' 2>/dev/null; }

sq() { sqlite3 -cmd ".timeout 5000" "$SEC_DB" "$1" 2>/dev/null; }
aq() { sqlite3 -cmd ".timeout 5000" "$AUDIT_DB" "$1" 2>/dev/null; }

event_field() { pcvctl_iso --format=json security event "$1" 2>/dev/null | jq -r ".result | $2" 2>/dev/null; }

audit_rows() {
    local n=0
    for _ in $(seq 1 20); do
        n="$(aq "SELECT COUNT(*) FROM audit_log WHERE method='security.event.close' AND target='$1';")"
        [ "${n:-0}" -ge 1 ] && break
        sleep 0.25
    done
    sleep 0.3
    aq "SELECT COUNT(*) FROM audit_log WHERE method='security.event.close' AND target='$1';"
}

boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --chdir / \
        "$DAEMON_BIN" > "$DLOG" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    for _ in $(seq 1 60); do
        if [ -S "$SOCK" ] && pcvctl_iso --format=json security events >/dev/null 2>&1 \
           && [ "$(sq "SELECT COUNT(*) FROM sqlite_master WHERE name='security_events';")" = "1" ] \
           && pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE_URL/health" >/dev/null; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

seed_event() {
    sq "INSERT INTO security_events(event_id,timestamp,source,type,severity,confidence,target_kind,target,summary,
        recommended_action,status,evidence_json,coalesce_key,occurrence_count,last_seen,created_at)
        VALUES('$1',strftime('%s','now'),'lsm','process_suspicious','$3',60,'process','/tmp/e2e-$1',
        'sec close e2e','manual_runbook','$2','{}','e2e-key-$1',1,strftime('%s','now'),strftime('%s','now'));"
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  security.event.close 운영자 단건 종결 격리 E2E${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if ! boot; then
    fail "S1: 격리 데몬 기동 실패(UDS·사건 DB·REST /health 준비 안 됨)"
    tail -30 "$DLOG" 2>/dev/null
    exit 1
fi
pass "S1: 격리 데몬 기동 + pcvctl(격리 소켓) 응답 + 사건 DB 스키마 + REST /health 준비"

rm_n="$(uds daemon.version '{}' | jq -r '.result.rpc_methods // empty' 2>/dev/null)"
if [ "$rm_n" = "323" ]; then pass "S3: daemon.version rpc_methods = 323"
else fail "S3: daemon.version rpc_methods = ${rm_n:-<없음>} (기대 323)"; fi

seed_event sec-e2e-cli open warn
seed_event sec-e2e-rest open crit
seed_event sec-e2e-done resolved warn
seed_event sec-e2e-pend open warn
sq "INSERT INTO security_actions(event_id,action,target_kind,target,status,ttl_sec,expires_at,requested_at)
    VALUES('sec-e2e-pend','manual_runbook','process','/tmp/e2e-sec-e2e-pend','pending',3600,
    strftime('%s','now')+3600,strftime('%s','now'));"
if [ "$(sq "SELECT COUNT(*) FROM security_events WHERE event_id LIKE 'sec-e2e-%';")" = "4" ]; then
    pass "S2: 격리 사건 DB 에 사건 4개(열림 3·닫힘 1)와 살아 있는 대기 행 1개를 넣음"
else
    fail "S2: 사건 준비 실패"; exit 1
fi

pcvctl_iso security close >/dev/null 2>&1; rc_noid=$?
pcvctl_iso security close sec-e2e-cli >/dev/null 2>&1; rc_noreason=$?
if [ "$rc_noid" = "2" ] && [ "$rc_noreason" = "2" ]; then
    pass "C3a: 사용법 오류(id 없음·--reason 없음) → rc 2·2"
else
    fail "C3a: 사용법 rc id없음=$rc_noid --reason없음=$rc_noreason (기대 2·2)"
fi

expect_reject() {
    local out rc
    out="$(pcvctl_iso --format=json security close "$2" --reason "$3" 2>/dev/null)"; rc=$?
    if [ "$rc" = "1" ] && [ "$(ecode "$out")" = "$4" ]; then
        pass "$1: rc 1 + $4 ($(printf '%s' "$out" | jq -r '.error.message' 2>/dev/null))"
    else
        fail "$1: rc=$rc code=$(ecode "$out") (기대 rc 1 + $4) — ${out:0:200}"
    fi
}
expect_reject "C3b(없음)" sec-e2e-missing "x" -32001
expect_reject "C3c(닫힘)" sec-e2e-done "x" -32002
expect_reject "C3d(대기 액션)" sec-e2e-pend "x" -32002
expect_reject "C3e(빈 사유)" sec-e2e-cli "" -32602
expect_reject "C3f(제어 문자뿐인 사유)" sec-e2e-cli $'\t \x01' -32602
if [ "$(event_field sec-e2e-cli .status)" = "open" ] && [ "$(event_field sec-e2e-pend .status)" = "open" ] \
   && [ "$(event_field sec-e2e-done '.closed_reason')" = "" ]; then
    pass "C3g: 거절된 사건은 그대로(open·open·resolved 의 closed_reason '')"
else
    fail "C3g: 거절 뒤 상태 cli=$(event_field sec-e2e-cli .status) pend=$(event_field sec-e2e-pend .status)"
fi

rows_before="$(aq "SELECT COUNT(*) FROM audit_log WHERE method='security.event.close' AND target='sec-e2e-cli' AND result='ok';")"
out="$(pcvctl_iso security close sec-e2e-cli --reason $'  checked\tfalse positive  ' 2>&1)"; rc=$?
if [ "$rc" = "0" ]; then pass "C1a: pcvctl security close → rc 0 ($(printf '%s' "$out" | head -1))"
else fail "C1a: rc=$rc — $out"; fi
got="$(pcvctl_iso --format=json security event sec-e2e-cli 2>/dev/null | jq -c '.result | {status,closed_reason,closed_by,close_note,ok_at:(.closed_at>0)}' 2>/dev/null)"
want='{"status":"resolved","closed_reason":"operator","closed_by":"local-admin","close_note":"checked false positive","ok_at":true}'
if [ "$got" = "$want" ]; then
    pass "C1b: security.event.get → $got"
else
    fail "C1b: security.event.get → $got (기대 $want)"
fi
n="$(audit_rows sec-e2e-cli)"
ok_rows="$(aq "SELECT COUNT(*) FROM audit_log WHERE method='security.event.close' AND target='sec-e2e-cli' AND result='ok' AND username='local-admin';")"

if [ "${rows_before:-0}" = "0" ] && [ "$ok_rows" = "1" ] && [ "$n" = "3" ]; then
    pass "C1c: 감사 ok 1행(op security.event.close, target sec-e2e-cli, local-admin), 이 target 전체 3행(거절 fail 2 + ok 1)"
else
    fail "C1c: ok 행 $ok_rows (기대 1, 이전 ${rows_before:-?}), 전체 ${n}행 (기대 3)"
    aq "SELECT id,username,method,target,result,error_code FROM audit_log WHERE method='security.event.close';"
fi

for spec in "e2e_sec_viewer:$VIEWER_PW:viewer" "e2e_sec_oper:$OPERATOR_PW:operator"; do
    IFS=: read -r u p role <<<"$spec"
    r="$(uds auth.user.create "{\"username\":\"$u\",\"password\":\"$p\",\"role\":\"$role\"}")"
    printf '%s' "$r" | jq -e '.result != null and .error == null' >/dev/null 2>&1 \
        || fail "R0: UDS auth.user.create $u($role) 실패 — ${r:0:200}"
done
TOK="$(login_token admin "$ADMIN_PW")"
TOK_VIEWER="$(login_token e2e_sec_viewer "$VIEWER_PW")"
TOK_OPER="$(login_token e2e_sec_oper "$OPERATOR_PW")"
if [ -n "$TOK" ] && [ -n "$TOK_VIEWER" ] && [ -n "$TOK_OPER" ]; then
    pass "R0: REST 로그인 admin·viewer·operator → access token"
else
    fail "R0: 로그인 실패(admin ${#TOK}, viewer ${#TOK_VIEWER}, operator ${#TOK_OPER})"; tail -20 "$DLOG"; exit 1
fi

r="$(rest_rpc "Authorization: Bearer $TOK" security.event.close \
    '{"event_id":"sec-e2e-rest","reason":"rest close","_pcv_username":"mallory"}')"
body="$(http_body "$r")"
if [ "$(http_code "$r")" = "200" ] && [ "$(printf '%s' "$body" | jq -r '.data.status' 2>/dev/null)" = "resolved" ]; then
    pass "C2a: REST ADMIN security.event.close → 200, 응답 status resolved"
else
    fail "C2a: HTTP $(http_code "$r") — ${body:0:200}"
fi
by="$(event_field sec-e2e-rest .closed_by)"
audit_rows sec-e2e-rest >/dev/null
au="$(aq "SELECT GROUP_CONCAT(username) FROM audit_log WHERE method='security.event.close' AND target='sec-e2e-rest';")"
mal="$(aq "SELECT COUNT(*) FROM audit_log WHERE username='mallory';")"
if [ "$by" = "admin" ] && [ "$au" = "admin" ] && [ "$mal" = "0" ]; then
    pass "C2b: closed_by=admin·감사 행위자=admin(1행), mallory 0행 — 파라미터 위조 무시"
else
    fail "C2b: closed_by=$by 감사=$au mallory 행=$mal (기대 admin·admin·0)"
fi

r="$(rest_rpc "Authorization: Bearer $TOK_VIEWER" security.action.pending '{}')"
if [ "$(http_code "$r")" = "200" ]; then pass "C4a: VIEWER security.action.pending → 200 (보안 화면 부팅 RPC)"
else fail "C4a: VIEWER security.action.pending → $(http_code "$r") (기대 200) — $(http_body "$r" | head -c 200)"; fi
r="$(rest_rpc "Authorization: Bearer $TOK_VIEWER" security.event.close '{"event_id":"sec-e2e-pend","reason":"x"}')"
if [ "$(http_code "$r")" = "403" ]; then pass "C4b: VIEWER security.event.close → 403"
else fail "C4b: VIEWER security.event.close → $(http_code "$r") (기대 403)"; fi

r="$(rest_rpc "Authorization: Bearer $TOK_OPER" security.action.dismiss '{"event_id":"sec-e2e-nope"}')"
c="$(http_code "$r")"; e="$(ecode "$(http_body "$r")")"
if [ "$c" != "403" ] && [ "$c" != "000" ] && [ "$e" != "-32006" ]; then
    pass "C5: OPERATOR security.action.dismiss → HTTP $c, code ${e:-없음} (403 아님 — 대상 오류)"
else
    fail "C5: OPERATOR security.action.dismiss → HTTP $c code $e (기대 403 아님)"
fi

kill_daemon
echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
