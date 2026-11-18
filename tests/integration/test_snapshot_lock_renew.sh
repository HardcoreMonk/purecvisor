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

skip() { echo -e "${YELLOW}[SKIP]${NC} 스냅샷 잠금 갱신 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v prlimit  >/dev/null 2>&1 || skip "prlimit 미설치 — 코어 덤프 억제 불가"
[ -n "${EPOCHREALTIME:-}" ] || skip "bash 5 이상 필요(EPOCHREALTIME)"
[ -e /usr/sbin/zfs ] || skip "/usr/sbin/zfs 없음 — mock bind 대상 부재(읽기 전용 루트 위에는 마운트 지점을 만들 수 없다)"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"
if [ "$DAEMON_BIN" -ef "$CAND_BIN" ]; then MODE=candidate; else MODE=counterfactual; fi

SLR_TMP="${TMPDIR:-/tmp}"
SLR_SOCK_SUFFIX="/pcv-slr.XXXXXX/var-lib/daemon.sock"
[ $(( ${#SLR_TMP} + ${#SLR_SOCK_SUFFIX} )) -le 100 ] || SLR_TMP=/tmp
STATE="$(mktemp -d "$SLR_TMP/pcv-slr.XXXXXX")" || skip "mktemp -d 실패 — 임시 상태 디렉터리를 만들 수 없다"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib/slr-mock" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
MK="$STATE/var-lib/slr-mock"
STATE_DB="$STATE/var-lib/vm_state.db"
VMX=kpvm
LOG=""
BP=""; DPID=""; CPID=""

cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/bash

D=/var/lib/purecvisor/slr-mock
if [ "${1:-}" = "snapshot" ] && [ "${2:-}" = "pcvpool/vms/kpvm@k1" ]; then
    printf 'snapshot\n' >> "$D/zfs.calls"
    for ((i = 0; i < 1650; i++)); do
        [ -e "$D/hang-snap" ] || break
        /usr/bin/sleep 0.2
    done
    exit 0
fi
exit 0
MOCKEOF
chmod +x "$STATE/mockbin/zfs"

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

now_s() { local t="${EPOCHREALTIME//[!0-9]/}"; echo $(( t / 1000000 )); }
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
    rm -f "$MK/hang-snap"
    [ -n "${CPID:-}" ] && kill "$CPID" 2>/dev/null
    [ -n "${BP:-}" ] || return 0
    kill -9 -- "-$BP" 2>/dev/null || true
    kill -9 "$BP" 2>/dev/null || true
    for _ in $(seq 1 20); do alive "$BP" || break; sleep 0.25; done
    wait "$BP" 2>/dev/null || true
    BP=""; DPID=""
}
cleanup() {
    kill_daemon
    if [ -n "${PCV_SLR_KEEP:-}" ]; then
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
admin_password = SlrE2eFixedPw-not-for-prod
jwt_secret = slr-e2e-fixed-secret-not-for-prod-000000001
libvirt_uri = test:///default
log_level = info
drain_timeout = 5
EOF

boot() {
    LOG="$STATE/daemon.log"
    rm -f "$SOCK"
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --ro-bind "$STATE/mockbin/zfs" /usr/sbin/zfs \
        --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin \
        --chdir / \
        prlimit --core=1:1 -- "$DAEMON_BIN" > "$LOG" 2>&1 < /dev/null &
    BP="$!"
    for _ in $(seq 1 60); do
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

locked_at() { sqlite3 "$STATE_DB" "SELECT locked_at FROM vm_locks WHERE vm_id='$VMX'" 2>/dev/null | head -1; }
lock_rows() { sqlite3 "$STATE_DB" "SELECT count(*) FROM vm_locks WHERE vm_id='$VMX'" 2>/dev/null | head -1; }

echo "=== 비동기 스냅샷 잠금 갱신 E2E (MODE=$MODE, 약 6분) ==="
if ! boot; then
    fail "격리 데몬 기동 실패 — 로그: $(tail -3 "$LOG" 2>/dev/null | tr '\n' ' ')"
    echo; echo -e "PASS=$PASS FAIL=$FAIL"; exit 1
fi
info "데몬 PID $DPID"

: > "$MK/hang-snap"
( python3 "$STATE/uds_call.py" "$SOCK" "$(req vm.snapshot.create "{\"vm_id\":\"$VMX\",\"snapshot_name\":\"k1\"}")" 400 > "$STATE/create.out" 2>/dev/null ) &
CPID=$!
T0="$(now_s)"
for _ in $(seq 1 50); do [ -s "$MK/zfs.calls" ] && break; sleep 0.2; done
if [ -s "$MK/zfs.calls" ]; then pass "① mock zfs snapshot 진입(잠금 아래에서 대기 중)"; else fail "① zfs snapshot 이 호출되지 않았다 — 로그: $(tail -3 "$LOG" | tr '\n' ' ')"; fi

declare -A SEEN=()
while [ $(( $(now_s) - T0 )) -lt 305 ]; do
    v="$(locked_at)"; [ -n "$v" ] && SEEN["$v"]=1
    sleep 10
done
v="$(locked_at)"; [ -n "$v" ] && SEEN["$v"]=1
DISTINCT="${#SEEN[@]}"
info "서로 다른 locked_at 값 $DISTINCT 개: ${!SEEN[*]}"
if [ "$MODE" = counterfactual ]; then
    info "② base 기대 locked_at 값 1개 — 실제 $DISTINCT"
elif [ "$DISTINCT" -ge 5 ]; then
    pass "② locked_at 이 갱신됐다(서로 다른 값 $DISTINCT ≥ 5)"
else
    fail "② locked_at 갱신 부족(서로 다른 값 $DISTINCT < 5)"
fi

rpc_t vm.snapshot.delete "{\"vm_id\":\"$VMX\",\"snapshot_name\":\"k1\"}" 20
code="$(jget '.error.code')"; msg="$(jget '.error.message')"
info "③ vm.snapshot.delete → code=$code message=$msg"
if [ "$MODE" = counterfactual ]; then
    info "③ base 기대: 삭제 접수(−32004 아님)"
elif [ "$code" = "-32004" ] && [[ "$msg" == *"VM 'kpvm' is already locked"* ]]; then
    pass "③ 305초에도 삭제가 −32004 로 막혔다"
else
    fail "③ 기대 −32004 \"VM 'kpvm' is already locked\" — 실제 $(short)"
fi

while [ $(( $(now_s) - T0 )) -lt 310 ]; do sleep 1; done
rm -f "$MK/hang-snap"
wait "$CPID" 2>/dev/null; CPID=""
parse_out "$(cat "$STATE/create.out" 2>/dev/null)"
info "④ 생성 응답: $(short)"
if [ "$R_ST" = ok ] && [ "$(jget '.result')" = "true" ]; then pass "④ 생성 응답 성공"; else fail "④ 생성 응답 실패 — ${R_ST}: $(short)"; fi

OVER="$(grep -c 'Overwriting expired lock' "$LOG" 2>/dev/null || true)"
if [ "$MODE" = counterfactual ]; then
    info "④ 'Overwriting expired lock' ${OVER:-0} 줄(base 기대 1)"
elif [ "${OVER:-0}" -eq 0 ]; then
    pass "④ 로그에 'Overwriting expired lock' 0줄"
else
    fail "④ 만료 잠금 덮어쓰기 로그 $OVER 줄"
fi

ROWS=""
for _ in $(seq 1 25); do ROWS="$(lock_rows)"; [ "$ROWS" = 0 ] && break; sleep 0.4; done
if [ "$ROWS" = 0 ]; then pass "④ 끝난 뒤 vm_locks '$VMX' 행 0"; else fail "④ 끝난 뒤 vm_locks 행 ${ROWS:-?}"; fi

if alive "$DPID"; then pass "데몬 PID $DPID 생존"; else fail "데몬이 죽었다"; fi

echo
echo -e "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
