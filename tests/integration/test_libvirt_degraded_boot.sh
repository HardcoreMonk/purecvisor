#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
CAND_BIN="$REPO/bin/purecvisorsd"
DAEMON_BIN="${DAEMON_BIN:-$CAND_BIN}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} libvirt DEGRADED 기동 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v pgrep    >/dev/null 2>&1 || skip "pgrep 미설치"
command -v prlimit  >/dev/null 2>&1 || skip "prlimit 미설치 — 코어 덤프 억제 불가"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"
if [ "$DAEMON_BIN" -ef "$CAND_BIN" ]; then MODE=candidate; else MODE=counterfactual; fi

LDB_TMP="${TMPDIR:-/tmp}"
LDB_SOCK_SUFFIX="/pcv-ldb.XXXXXX/var-lib/daemon.sock"
[ $(( ${#LDB_TMP} + ${#LDB_SOCK_SUFFIX} )) -le 100 ] || LDB_TMP=/tmp
STATE="$(mktemp -d "$LDB_TMP/pcv-ldb.XXXXXX")" || skip "mktemp -d 실패 — 임시 상태 디렉터리를 만들 수 없다"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"
STATE_DB="$STATE/var-lib/vm_state.db"
LOG=""
BP=""; DPID=""; STUB_PID=""

cat > "$STATE/uds_call.py" <<'PYEOF'
import socket, sys, time
sock_path, payload, tmo = sys.argv[1], sys.argv[2], float(sys.argv[3])
t0 = time.monotonic()
buf = b""
status = "ok"
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(tmo)
    s.connect(sock_path)
    s.sendall(payload.encode())
    while True:
        left = tmo - (time.monotonic() - t0)
        if left <= 0:
            status = "timeout"
            break
        s.settimeout(left)
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            status = "timeout"
            break
        if not chunk:
            break
        buf += chunk
    s.close()
except Exception as e:
    status = "error:%s" % str(e).replace(" ", "_")
sys.stdout.write("%d %s\n" % (int((time.monotonic() - t0) * 1000), status))
sys.stdout.write(buf.decode(errors="replace"))
PYEOF

cat > "$STATE/stall_stub.py" <<'PYEOF'
import os, socket, sys
path = sys.argv[1]
try:
    os.unlink(path)
except FileNotFoundError:
    pass
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(path)
srv.listen(16)
held = []
while True:
    c, _ = srv.accept()
    held.append(c)
PYEOF

now_ms() { local t="${EPOCHREALTIME//[!0-9]/}"; echo $(( t / 1000 )); }
req() { printf '{"jsonrpc":"2.0","method":"%s","params":%s,"id":"t"}' "$1" "$2"; }
parse_out() {
    local head="${1%%$'\n'*}"
    R_MS="${head%% *}"; R_ST="${head#* }"
    [[ "$R_MS" =~ ^[0-9]+$ ]] || { R_MS=999999; R_ST="error:no-output"; }
    if [[ "$1" == *$'\n'* ]]; then R_BODY="${1#*$'\n'}"; else R_BODY=""; fi
}
rpc_t() {
    parse_out "$(python3 "$STATE/uds_call.py" "$SOCK" "$(req "$1" "$2")" "$3" 2>/dev/null)"
}
jget() { printf '%s' "$R_BODY" | jq -r "$1" 2>/dev/null; }
short() { local b="${R_BODY:-<empty>}"; printf '%s' "${b:0:240}"; }
is_rpc() { printf '%s' "$R_BODY" | jq -e 'type == "object" and (has("result") or has("error"))' >/dev/null 2>&1; }
alive() {
    local st
    [ -n "${1:-}" ] && [ -r "/proc/$1/stat" ] || return 1
    read -r _ _ st _ 2>/dev/null < "/proc/$1/stat" || return 1
    [ "$st" != "Z" ]
}
kill_daemon() {
    [ -n "${BP:-}" ] || return 0
    kill -9 -- "-$BP" 2>/dev/null || true
    kill -9 "$BP" 2>/dev/null || true
    for _ in $(seq 1 20); do alive "$BP" || break; sleep 0.25; done
    wait "$BP" 2>/dev/null || true
    BP=""; DPID=""
}
kill_stub() {
    [ -n "${STUB_PID:-}" ] || return 0
    kill "$STUB_PID" 2>/dev/null || true
    for _ in $(seq 1 20); do alive "$STUB_PID" || break; sleep 0.25; done
    kill -9 "$STUB_PID" 2>/dev/null || true
    wait "$STUB_PID" 2>/dev/null || true
    STUB_PID=""
}
cleanup() {
    kill_daemon
    kill_stub
    if [ -n "${PCV_LDB_KEEP:-}" ]; then
        echo "임시 디렉터리 보존: $STATE"
    else
        rm -rf "$STATE" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT; trap 'exit 143' TERM

cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = 0
admin_user = admin
admin_password = LdbE2eFixedPw-not-for-prod
jwt_secret = ldb-e2e-fixed-secret-not-for-prod-000000001
libvirt_uri = test:///default
log_level = info
drain_timeout = 5
EOF

boot() {
    local tag="$1" uri="$2" wait_sec="$3" i
    LOG="$STATE/daemon-$tag.log"
    rm -f "$SOCK"
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --setenv PCV_LIBVIRT_URI "$uri" \
        --setenv PURECVISOR_LIBVIRT_URI "$uri" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin \
        --chdir / \
        prlimit --core=1:1 -- "$DAEMON_BIN" > "$LOG" 2>&1 < /dev/null &
    BP="$!"
    for ((i = 0; i < wait_sec * 2; i++)); do
        if [ -S "$SOCK" ]; then
            rpc_t daemon.version '{}' 3
            if is_rpc; then
                DPID="$(pcv_iso_daemon_pid "$BP")"
                [ -n "$DPID" ] && return 0
            fi
        fi
        alive "$BP" || return 1
        sleep 0.5
    done
    return 1
}

banner_lines() { grep -c 'MODE : \*\*\* DEGRADED (libvirt unavailable) \*\*\*' "$LOG" 2>/dev/null || true; }
degraded_lines() { grep -c 'DEGRADED' "$LOG" 2>/dev/null || true; }
stage_ms() {
    sed -n 's/.*\[init\] Stage [0-9]* (libvirt-dispatcher) completed in \([0-9]*\)ms.*/\1/p' "$LOG" 2>/dev/null | head -1
}
stage_lines() { grep -E '\[init\] Stage [0-9]+ \(' "$LOG" 2>/dev/null | sed 's/^.*\[init\]/[init]/' || true; }

echo "=== libvirt DEGRADED 기동 판정 E2E (MODE=$MODE) ==="

echo "--- 기동 A: test:///default ---"
if boot A "test:///default" 40; then
    info "A 데몬 PID $DPID"
    n="$(degraded_lines)"
    if [ "${n:-0}" -eq 0 ]; then pass "A 로그에 DEGRADED 0줄"; else fail "A 로그에 DEGRADED ${n}줄: $(grep DEGRADED "$LOG" | head -2 | tr '\n' ' ')"; fi
    rpc_t vm.list '{}' 20
    if [ "$R_ST" = ok ] && printf '%s' "$R_BODY" | jq -e '.result | type == "array"' >/dev/null 2>&1; then
        pass "A vm.list result 가 배열"
    else
        fail "A vm.list 기대 배열 — ${R_ST}: $(short)"
    fi
else
    fail "A 격리 데몬 기동 실패 — 로그: $(tail -3 "$LOG" 2>/dev/null | tr '\n' ' ')"
fi
kill_daemon

echo "--- 기동 B: 없는 소켓 ---"
if boot B "qemu+unix:///system?socket=/var/lib/purecvisor/no-such.sock" 90; then
    info "B 데몬 PID $DPID"
    n="$(banner_lines)"
    if [ "${n:-0}" -eq 1 ]; then pass "B DEGRADED 배너 1줄"; else fail "B DEGRADED 배너 ${n:-0}줄(기대 1)"; fi
    rpc_t daemon.version '{}' 5
    if [ "$R_ST" = ok ] && [ -n "$(jget '.result.version // empty')" ]; then pass "B daemon.version 성공"; else fail "B daemon.version 실패 — ${R_ST}: $(short)"; fi

    LEAK_P_DETACH='{"vm_id":"pcv-lockleak","target":"vdb"}'
    LEAK_P_ATTACH='{"vm_id":"pcv-lockleak","source":"/dev/null","target":"vdb"}'
    LEAK_P_USB='{"vm_id":"pcv-lockleak","vendor_id":"0x1234","product_id":"0x5678"}'
    for step in "device.disk.detach:$LEAK_P_DETACH" "device.disk.detach:$LEAK_P_DETACH" \
                "device.disk.attach:$LEAK_P_ATTACH" "device.disk.attach:$LEAK_P_ATTACH" \
                "vm.usb.attach:$LEAK_P_USB" "vm.usb.attach:$LEAK_P_USB" \
                "vm.usb.detach:$LEAK_P_USB" "vm.usb.detach:$LEAK_P_USB"; do
        method="${step%%:*}"; params="${step#*:}"
        rpc_t "$method" "$params" 30
        code="$(jget '.error.code')"; msg="$(jget '.error.message')"
        if [ "$code" = "-32000" ] && [[ "$msg" == *"Hypervisor connection pool exhausted"* ]]; then
            pass "B $method → −32000 Hypervisor connection pool exhausted"
        else
            fail "B $method 기대 −32000 'Hypervisor connection pool exhausted' — 실제 code=$code message=$msg 응답=$(short)"
        fi
    done
    rows="$(sqlite3 "$STATE_DB" "SELECT COUNT(*) FROM vm_locks WHERE vm_id='pcv-lockleak'" 2>/dev/null | head -1)"
    if [ "$rows" = 0 ]; then pass "B vm_locks 'pcv-lockleak' 행 0"; else fail "B vm_locks 'pcv-lockleak' 행 ${rows:-?}(기대 0)"; fi
else
    fail "B 격리 데몬 기동 실패 — 로그: $(tail -3 "$LOG" 2>/dev/null | tr '\n' ' ')"
fi
kill_daemon

echo "--- 기동 C: 무응답 스텁 소켓 ---"
setsid python3 -I "$STATE/stall_stub.py" "$STATE/var-lib/stall.sock" > "$STATE/stub.log" 2>&1 < /dev/null &
STUB_PID="$!"
for _ in $(seq 1 20); do [ -S "$STATE/var-lib/stall.sock" ] && break; sleep 0.2; done
[ -S "$STATE/var-lib/stall.sock" ] || fail "C 스텁 소켓이 만들어지지 않았다: $(cat "$STATE/stub.log" 2>/dev/null)"

C_URI="qemu+unix:///system?socket=/var/lib/purecvisor/stall.sock"
if [ "$MODE" = counterfactual ]; then

    LOG="$STATE/daemon-C.log"
    rm -f "$SOCK"
    setsid "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" \
        --setenv PCV_LIBVIRT_URI "$C_URI" --setenv PURECVISOR_LIBVIRT_URI "$C_URI" \
        --bind "$STATE/var-lib" /var/lib/purecvisor --bind "$STATE/etc" /etc/purecvisor \
        --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin --chdir / \
        prlimit --core=1:1 -- "$DAEMON_BIN" > "$LOG" 2>&1 < /dev/null &
    BP="$!"
    last_seen=""; last_at="$(now_ms)"; hung=0; ms=""
    for _ in $(seq 1 180); do
        ms="$(stage_ms)"; [ -n "$ms" ] && break
        cur="$(stage_lines | tail -1)"
        if [ "$cur" != "$last_seen" ]; then last_seen="$cur"; last_at="$(now_ms)"; fi
        if [ -n "$last_seen" ] && [ $(( $(now_ms) - last_at )) -ge 30000 ]; then hung=1; break; fi
        alive "$BP" || break
        sleep 0.5
    done
    if [ "$hung" = 1 ]; then
        info "C(base) 직전 단계 뒤 30초 안에 libvirt-dispatcher 줄이 없다 — 멈춤. 마지막 단계: $last_seen"
    else
        info "C(base) libvirt-dispatcher 단계 ${ms:-없음}ms"
    fi
    kill_daemon
else
    if boot C "$C_URI" 90; then
        info "C 데몬 PID $DPID"
        up=1
    else
        up=0
        info "C UDS 가 90초 안에 뜨지 않았다 — 뒤 단계 묶임 후보. 단계별 completed 시각:"
        stage_lines | while read -r l; do info "  $l"; done
    fi
    ms="$(stage_ms)"
    if [ -z "$ms" ]; then
        fail "C libvirt-dispatcher 단계 completed 줄이 없다 — 이 단계가 스텁에 묶였다"
    elif [ "$ms" -le 11000 ]; then
        pass "C libvirt-dispatcher 단계 ${ms}ms ≤ 11000"
    else
        fail "C libvirt-dispatcher 단계 ${ms}ms > 11000"
    fi
    n="$(banner_lines)"
    if [ "${n:-0}" -eq 1 ]; then pass "C DEGRADED 배너 1줄"; elif [ "$up" = 1 ]; then fail "C DEGRADED 배너 ${n:-0}줄(기대 1)"; else info "C 배너는 기동 완료 뒤에 나온다 — ${n:-0}줄(UDS 미기동)"; fi
    if [ "$up" = 1 ]; then
        rpc_t daemon.version '{}' 5
        if [ "$R_ST" = ok ] && [ -n "$(jget '.result.version // empty')" ]; then pass "C daemon.version 성공"; else fail "C daemon.version 실패 — ${R_ST}: $(short)"; fi
    fi
    kill_daemon
fi

kill_stub
left="$(pgrep -f "$STATE/var-lib/stall.sock" 2>/dev/null | wc -l)"
if [ "$left" -eq 0 ]; then pass "스텁 프로세스 정리(pgrep -f stall.sock 0)"; else fail "스텁 프로세스 $left 개 남음"; fi

echo
echo -e "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
