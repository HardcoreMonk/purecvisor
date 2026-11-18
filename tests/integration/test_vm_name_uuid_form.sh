#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="$REPO/bin/purecvisorsd"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} VM 이름 UUID 표기 거부 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"

pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in 28095 29095 31095 33095 37095 41095 43095; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-vmname.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"

cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/sh

echo "$*" >> /var/lib/purecvisor/zfs-calls.log 2>/dev/null
case "$1" in
    list) exit 1 ;;
    *)    exit 0 ;;
esac
MOCKEOF
chmod +x "$STATE/mockbin/zfs"

cat > "$STATE/uds_call.py" <<'PYEOF'
import socket, sys
sock_path, payload = sys.argv[1], sys.argv[2]
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(15.0)
    s.connect(sock_path)
    s.sendall(payload.encode())
    buf = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
    s.close()
    sys.stdout.write(buf.decode(errors="replace"))
except Exception as e:
    sys.stderr.write("uds_call error: %s\n" % e)
    sys.exit(2)
PYEOF

uds_call() {
    python3 "$STATE/uds_call.py" "$SOCK" "$1" 2>/dev/null
}

kill_daemon() {
    [ -f "$STATE/bwrap.pid" ] || return 0
    local bp; bp="$(cat "$STATE/bwrap.pid" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    local i
    for i in $(seq 1 12); do
        kill -0 "$bp" 2>/dev/null || break
        sleep 0.25
    done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
}

cleanup() {
    kill_daemon
    rm -f "$STATE/bwrap.pid"
    rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

write_conf() {
    cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = VmNameE2eFixedPw-not-for-prod
jwt_secret = vmname-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
EOF
}

boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --ro-bind "$STATE/mockbin/zfs" /usr/sbin/zfs \
        --setenv PCV_LIBVIRT_URI test:///default \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local i resp
    for i in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            resp="$(uds_call '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"ping"}')"
            case "$resp" in
                *'"jsonrpc"'*|*'"result"'*|*'"error"'*) return 0 ;;
            esac
        fi
        sleep 0.5
    done
    return 1
}

DB="$STATE/var-lib/vm_state.db"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
ZFS_LOG="$STATE/var-lib/zfs-calls.log"
U="8c1f5a3e-2b4d-4e6f-9a0b-1c2d3e4f5a6b"
H="8c1f5a3e2b4d4e6f9a0b1c2d3e4f5a6b"
UUID_MSG="VM name must not look like a UUID"

rpc() {
    uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"t\"}"
}

assert_uuid_rejected() {
    local label="$1" resp code msg
    resp="$(rpc "$2" "$3")"
    code="$(printf '%s' "$resp" | jq -r '.error.code // "none"' 2>/dev/null)"
    msg="$(printf '%s' "$resp" | jq -r '.error.message // ""' 2>/dev/null)"
    info "$label: code=$code message='$msg'"
    if [ "$code" = "-32602" ] && [ "$msg" = "$UUID_MSG" ]; then
        pass "$label: -32602 \"$UUID_MSG\""
    else
        fail "$label: 기대 -32602/\"$UUID_MSG\" 실제 code=$code message='$msg' resp=${resp:-<empty>}"
    fi
}

assert_not_uuid_msg() {
    local label="$1" resp code msg
    resp="$(rpc "$2" "$3")"
    code="$(printf '%s' "$resp" | jq -r '.error.code // "none"' 2>/dev/null)"
    msg="$(printf '%s' "$resp" | jq -r '.error.message // ""' 2>/dev/null)"
    info "$label: code=$code message='$msg'"
    if [ -z "${resp:-}" ]; then
        fail "$label: 빈 응답 (데몬 무응답)"
    elif [ "$msg" = "$UUID_MSG" ]; then
        fail "$label: UUID 메시지로 거부됨 (조회/보통 이름이 막힘)"
    else
        pass "$label: UUID 메시지 아님 (code=$code)"
    fi
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  새 VM 이름 UUID 표기 거부 E2E     ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE  uds=$SOCK${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

write_conf
if ! boot; then
    fail "S1: 격리 데몬이 UDS 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 응답 ($SOCK)"

CP='"vcpu":1,"memory_mb":512,"disk_gb":1,"disk_size_gb":1,"qos_min_mbps":10,"qos_max_mbps":100'
assert_uuid_rejected "1  vm.create name=U"            vm.create "{\"name\":\"$U\",$CP}"
assert_uuid_rejected "2  vm.create name=H"            vm.create "{\"name\":\"$H\",$CP}"
assert_uuid_rejected "3a vm.clone clone_name=U"       vm.clone  "{\"source\":\"pcv-src\",\"clone_name\":\"$U\"}"
assert_uuid_rejected "3b vm.clone target=H"           vm.clone  "{\"source\":\"pcv-src\",\"target\":\"$H\"}"
assert_uuid_rejected "3c vm.clone new_name=U"         vm.clone  "{\"source\":\"pcv-src\",\"new_name\":\"$U\"}"
assert_uuid_rejected "4a vm.rename new_name=U"        vm.rename "{\"name\":\"pcv-old\",\"new_name\":\"$U\"}"
assert_uuid_rejected "4b vm.rename target_name=U"     vm.rename "{\"name\":\"pcv-old\",\"target_name\":\"$U\"}"
assert_uuid_rejected "4c vm.rename target=U"          vm.rename "{\"name\":\"pcv-old\",\"target\":\"$U\"}"
assert_uuid_rejected "5  vm.import.ova name=U"        vm.import.ova "{\"ova_path\":\"/nonexistent.ova\",\"name\":\"$U\"}"

resp="$(rpc vm.rename "{\"name\":\"pcv-old\",\"new_name\":\" $U\"}")"
code="$(printf '%s' "$resp" | jq -r '.error.code // "none"' 2>/dev/null)"
msg="$(printf '%s' "$resp" | jq -r '.error.message // ""' 2>/dev/null)"
info "6  vm.rename new_name=' '+U: code=$code message='$msg'"
case "$msg" in
    "Invalid VM name"*) [ "$code" = "-32602" ] && pass "6: 문자 규칙 메시지 우선 (-32602 '$msg')" \
                        || fail "6: code=$code (기대 -32602)" ;;
    *) fail "6: 기대 'Invalid VM name' 시작, 실제 code=$code message='$msg'" ;;
esac

assert_not_uuid_msg "7  vm.rename name=U(옛 이름)"    vm.rename "{\"name\":\"$U\",\"new_name\":\"pcv-new\"}"
assert_not_uuid_msg "8  vm.rename 보통 이름"          vm.rename "{\"name\":\"pcv-old\",\"new_name\":\"pcv-new\"}"

sleep 1.5
locks="$(sqlite3 -cmd ".timeout 5000" "$DB" "SELECT COUNT(*) FROM vm_locks;" 2>/dev/null || echo "?")"
zcreate="$( { grep -c '^create' "$ZFS_LOG" 2>/dev/null || true; } | head -1)"
zall="$( { wc -l < "$ZFS_LOG" 2>/dev/null || echo 0; } | tr -d ' ')"
aok="$(sqlite3 -cmd ".timeout 5000" "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method IN ('vm.create','vm.clone','vm.rename','vm.import.ova') AND result='ok';" 2>/dev/null || echo "?")"
info "효과 없음 수치: vm_locks=$locks zfs_create=${zcreate:-0} (zfs 호출 전체=$zall) audit_ok=$aok"
[ "$locks" = "0" ]          && pass "E1: vm_locks 0행"                       || fail "E1: vm_locks=$locks"
[ "${zcreate:-0}" = "0" ]   && pass "E2: mock zfs create 호출 0줄"           || fail "E2: zfs create ${zcreate}줄"
[ "$aok" = "0" ]            && pass "E3: 감사 네 RPC 성공(result=ok) 0줄"    || fail "E3: audit_ok=$aok"

kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
