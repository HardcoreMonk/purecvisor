#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_HELP_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} help 예시 격리 E2E: $*"; exit 0; }

for tool in bwrap python3 setsid jq curl; do
    command -v "$tool" >/dev/null 2>&1 || skip "$tool 미설치"
done
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
if [ ! -x "$DAEMON_BIN" ]; then
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

PORT=""
for p in 28251 29251 31251 33251 37251 41251; do
    ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)" || { PORT="$p"; break; }
done
[ -n "$PORT" ] || skip "빈 포트 확보 실패"
BASE_URL="http://127.0.0.1:$PORT/api/v1"
ADMIN_PW='HelpExamples-E2e-Admin-not-for-prod-1'

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-helpex.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
chmod 0755 "$STATE"
mkdir -p "$STATE/var-lib/lxc" "$STATE/etc" "$STATE/var-log"
SOCK="$STATE/var-lib/daemon.sock"
DLOG="$STATE/daemon.log"
LIBVIRT_URI='test:///default'

cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $ADMIN_PW
jwt_secret = help-examples-e2e-fixed-secret-not-for-prod
libvirt_uri = $LIBVIRT_URI
log_level = info
drain_timeout = 1
pool_max_conn = 2
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

boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --bind "$STATE/var-log" /var/log/purecvisor \
        --bind "$STATE" "$STATE" \
        --setenv PCV_LIBVIRT_URI "$LIBVIRT_URI" \
        --setenv PURECVISOR_LIBVIRT_URI "$LIBVIRT_URI" \
        --chdir / \
        "$DAEMON_BIN" > "$DLOG" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    for _ in $(seq 1 60); do
        if [ -S "$SOCK" ] && pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE_URL/health" >/dev/null; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

if ! boot; then
    fail "격리 데몬 기동 실패"
    tail -30 "$DLOG" 2>/dev/null
    exit 1
fi

TOKEN=""
for _ in $(seq 1 40); do
    TOKEN="$(iso_curl -s --max-time 10 -X POST "$BASE_URL/auth/token" -H 'Content-Type: application/json' \
        -d "{\"username\":\"admin\",\"password\":\"$ADMIN_PW\"}" 2>/dev/null | jq -r '.access_token // empty' 2>/dev/null)"
    [ -n "$TOKEN" ] && break
    sleep 0.25
done
[ -n "$TOKEN" ] || { fail "관리자 로그인 실패"; tail -30 "$DLOG" 2>/dev/null; exit 1; }

send() {
    iso_curl -s --max-time 30 -X "$1" -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
        ${3:+-d "$3"} "$BASE_URL$2" 2>/dev/null || true
}

route_missing() { printf '%s' "$1" | grep -q 'Endpoint not found or method not allowed'; }

key_rejected() {
    printf '%s' "$1" | jq -e '(.error.code == -32602)
        and ((.error.message // "") | test("missing|required|non-empty"; "i"))' >/dev/null 2>&1
}

invalid_params() { printf '%s' "$1" | jq -e '.error.code == -32602' >/dev/null 2>&1; }
USB_CHECKED=0

EXAMPLES="$STATE/examples.jsonl"
python3 "$REPO/scripts/check_help_contract.py" --dump-examples > "$EXAMPLES" || { fail "예시 추출 실패"; exit 1; }
COUNT=0
while IFS= read -r line; do
    m="$(printf '%s' "$line" | jq -r '.m')"
    path="$(printf '%s' "$line" | jq -r '.path')"
    body="$(printf '%s' "$line" | jq -r '.body // empty')"
    case "$path" in /auth/logout*|/auth/refresh|/auth/token|/auth/register|/auth/password|/auth/totp/verify|/auth/totp/enroll) continue ;; esac
    COUNT=$((COUNT+1))
    resp="$(send "$m" "$path" "$body")"
    if route_missing "$resp"; then
        fail "$m $path: 경로 없음 — $resp"
    elif key_rejected "$resp"; then
        fail "$m $path: 필수 키 거절 — body=$body resp=$resp"
    elif [[ "$path" == /vms/*/usb && "$m" != GET ]] && { USB_CHECKED=$((USB_CHECKED+1)); invalid_params "$resp"; }; then
        fail "$m $path: USB 예시가 인자 검사(−32602)에 걸렸다 — body=$body resp=$resp"
    else
        pass "$m $path"
    fi
done < "$EXAMPLES"
[ "$COUNT" -ge 50 ] && pass "예시 $COUNT건 전송" || fail "전송한 예시가 너무 적다($COUNT) — 추출이 비었다"
[ "$USB_CHECKED" -eq 2 ] && pass "USB 예시 2건 −32602 아님 확인" || fail "USB 변경 예시를 $USB_CHECKED건 봤다(기대 2)"

old_rejected() {
    local resp
    resp="$(send "$1" "$2" "$3")"
    if key_rejected "$resp"; then
        pass "반사실 $4: 옛 본문 거절 확인"
    else
        fail "반사실 $4: 옛 본문이 거절되지 않았다(판정이 공허하다) resp=$resp"
    fi
}
old_rejected POST /storage/pools '{"name":"pcv-example-pool","devices":["sdb"]}' "옛 예시 #14 devices"
old_rejected POST /backup/replicate '{"vm":"pcv-example-vm","target":"pcv-example-node","user":"pcv-example-user"}' "옛 예시 #7 vm·target·user"
old_rejected POST /vms/test/guest-exec '{"cmd":"uptime"}' "옛 예시 #11 cmd"
for verb in POST DELETE; do
    resp="$(send "$verb" /vms/test/usb '{"vendor_id":"1234","product_id":"5678"}')"
    if invalid_params "$resp"; then
        pass "반사실 $verb /vms/test/usb: 옛 본문(0x 없음) −32602 확인"
    else
        fail "반사실 $verb /vms/test/usb: 옛 본문이 −32602가 아니다(판정이 공허하다) resp=$resp"
    fi
done
resp="$(send POST /vms/import '{"ova_path":"/x.ova","name":"pcv-example-vm"}')"
if route_missing "$resp"; then
    pass "반사실 POST /vms/import: 경로 없음 확인"
else
    fail "반사실 POST /vms/import: 경로 판정이 공허하다 resp=$resp"
fi

echo
echo "help 예시 격리 E2E: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
