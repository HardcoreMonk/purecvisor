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

skip() { echo -e "${YELLOW}[SKIP]${NC} LXC unprivileged 생성 정책 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
[ -f /etc/subuid ] && [ -f /etc/subgid ] || skip "/etc/subuid·subgid 없음 — bind 대상 부재"

pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in 28097 29097 31097 33097 37097 41097 43097; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-z17pol.XXXXXX")"
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
admin_password = Z17PolE2eFixedPw-not-for-prod
jwt_secret = z17pol-e2e-fixed-secret-not-for-prod-0001
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
        --ro-bind "$SUBUID" /etc/subuid \
        --ro-bind "$SUBGID" /etc/subgid \
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

rpc() {
    uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"t\"}"
}

SUBUID="$STATE/subuid"; SUBGID="$STATE/subgid"
: > "$SUBUID"; : > "$SUBGID"
put_subid() {
    printf '%s' "$1" > "$SUBUID"; printf '%s' "$1" > "$SUBGID"
}

mkdir -p "$STATE/var-lib/lxc/z17-src"
printf 'lxc.uts.name = z17-src\n' > "$STATE/var-lib/lxc/z17-src/config"
printf 'op1\n' > "$STATE/var-lib/lxc/z17-src/purecvisor.owner"
NOT_READY="unprivileged containers are not available on this host: "

assert_rejected() {
    local resp code msg
    resp="$(rpc "$2" "$3")"
    code="$(printf '%s' "$resp" | jq -r '.error.code // "none"' 2>/dev/null)"
    msg="$(printf '%s' "$resp" | jq -r '.error.message // ""' 2>/dev/null)"
    info "$1: code=$code message='$msg'"
    case "$msg" in
        "$5"*) if [ "$code" = "$4" ] && { [ -z "${6:-}" ] || [[ "$msg" != *"$6"* ]]; }; then pass "$1"; return; fi ;;
    esac
    fail "$1: 기대 $4/'$5…' 실제 code=$code message='$msg' resp=${resp:-<empty>}"
}
assert_accepted() {
    local resp st
    resp="$(rpc "$2" "$3")"
    st="$(printf '%s' "$resp" | jq -r '.result.status // "none"' 2>/dev/null)"
    [ "$st" = "accepted" ] && pass "$1: accepted" || fail "$1: 기대 accepted 실제 resp=${resp:-<empty>}"
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  LXC unprivileged 생성 정책 동기 거절 E2E  ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE  uds=$SOCK${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

put_subid $'root:100000:65536\n'
write_conf
if ! boot; then
    fail "S1: 격리 데몬이 UDS 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 응답 ($SOCK)"

assert_rejected "1  rootless 문자열"            container.create '{"name":"z17-a","rootless":"true"}' -32602 "rootless must be a boolean"
assert_rejected "2  operator rootless:false"    container.create '{"name":"z17-b","rootless":false,"_pcv_caller_role":1}' -32006 "privileged containers require admin role"
assert_rejected "9  readonly 문자열"            container.volume.attach '{"name":"z17-src","host_path":"/tmp","container_path":"/mnt","readonly":"yes"}' -32602 "readonly must be a boolean"
assert_rejected "10 operator privileged 원본 복제" container.clone '{"source":"z17-src","dest":"z17-dst","_pcv_caller_role":1,"_pcv_caller_sub":"op1"}' -32006 "cloning a privileged container requires admin role"
put_subid ''
assert_rejected "3  빈 subid(root 없음)"        container.create '{"name":"z17-c","_pcv_caller_role":1}' -32002 "${NOT_READY}/etc/subuid and /etc/subgid must delegate 100000:65536 to root"
put_subid $'z17acct:200000:65536\n'
assert_rejected "4  다른 계정만"                container.create '{"name":"z17-d"}' -32002 "${NOT_READY}/etc/subuid and /etc/subgid must delegate"
put_subid $'root:100000:65536\nz17acct:165535:10\n'
assert_rejected "5  한 uid 겹침"                container.create '{"name":"z17-e","rootless":true}' -32002 "${NOT_READY}another account's subordinate id range overlaps" z17acct
put_subid $'pcvadmin:100000:65536\n'
assert_rejected "6  다른 계정이 root 범위 차지"  container.create '{"name":"z17-f"}' -32002 "${NOT_READY}another account's subordinate id range overlaps" pcvadmin

sleep 1.5
locks="$(sqlite3 -cmd ".timeout 5000" "$DB" "SELECT COUNT(*) FROM vm_locks;" 2>/dev/null || echo "?")"
zcreate="$( { grep -c '^create' "$ZFS_LOG" 2>/dev/null || true; } | head -1)"
aok="$(sqlite3 -cmd ".timeout 5000" "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method IN ('container.create','container.clone','container.volume.attach') AND result='ok';" 2>/dev/null || echo "?")"
dirs="$(find "$STATE/var-lib/lxc" -mindepth 1 -maxdepth 1 ! -name z17-src | wc -l)"
info "효과 없음 수치: vm_locks=$locks zfs_create=${zcreate:-0} audit_ok=$aok dirs=$dirs"
[ "$locks" = "0" ]        && pass "E1: vm_locks 0행"                 || fail "E1: vm_locks=$locks"
[ "${zcreate:-0}" = "0" ] && pass "E2: mock zfs create 0줄"          || fail "E2: zfs create ${zcreate}줄"
[ "$aok" = "0" ]          && pass "E3: 감사 성공 기록 0줄"           || fail "E3: audit_ok=$aok"
[ "$dirs" = "0" ]         && pass "E4: 컨테이너 디렉터리 0개"        || fail "E4: lxc 아래 $dirs개"

put_subid $'root:100000:65536\n'
assert_accepted "7  admin rootless:false(준비 불필요)" container.create '{"name":"z17-g","rootless":false}'
assert_accepted "8  operator 기본(준비됨)"            container.create '{"name":"z17-h","_pcv_caller_role":1}'

kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
