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
EXEC_TARGET=/usr/local/bin/purecvisorsd

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} 계수기 정지·교체 직전 저장 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v curl     >/dev/null 2>&1 || skip "curl 미설치"
command -v prlimit  >/dev/null 2>&1 || skip "prlimit 미설치 — 코어 덤프 억제 불가"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"
if [ "$DAEMON_BIN" -ef "$CAND_BIN" ]; then MODE=candidate; else MODE=counterfactual; fi
EXEC_BIND=()
if [ -f "$EXEC_TARGET" ]; then EXEC_BIND=(--ro-bind "$DAEMON_BIN" "$EXEC_TARGET"); fi

PCF_TMP="${TMPDIR:-/tmp}"
PCF_SOCK_SUFFIX="/pcv-pcf.XXXXXX/var-lib/daemon.sock"
[ $(( ${#PCF_TMP} + ${#PCF_SOCK_SUFFIX} )) -le 100 ] || PCF_TMP=/tmp
STATE="$(mktemp -d "$PCF_TMP/pcv-pcf.XXXXXX")" || skip "mktemp -d 실패 — 임시 상태 디렉터리를 만들 수 없다"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"
CKPT="$STATE/var-lib/prom_counters.json"
PORT=28161
METRICS_URL="http://127.0.0.1:$PORT/api/v1/metrics"
SERIES='purecvisor_rpc_requests_total{method="daemon.version",status="ok"}'
LABELS='method="daemon.version",status="ok"'
LOG=""; BP=""; DPID=""; STOP_RC=""; T_LAUNCH=0

cat > "$STATE/uds_call.py" <<'PYEOF'
import json, socket, sys
sock_path, method, tmo = sys.argv[1], sys.argv[2], float(sys.argv[3])
buf = b""
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(tmo)
    s.connect(sock_path)
    s.sendall(json.dumps({"jsonrpc": "2.0", "method": method, "params": {}, "id": "t"}).encode())
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
        try:
            json.loads(buf.decode(errors="replace"))
            break
        except ValueError:
            pass
    s.close()
    obj = json.loads(buf.decode(errors="replace"))
except Exception:
    sys.exit(1)
sys.exit(0 if isinstance(obj, dict) and ("result" in obj or "error" in obj) else 1)
PYEOF

rpc_ok() { python3 -I "$STATE/uds_call.py" "$SOCK" daemon.version 3 >/dev/null 2>&1; }
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

stop_daemon() {
    STOP_RC="timeout"
    [ -n "${BP:-}" ] || return 0
    if [ -n "${DPID:-}" ]; then
        kill -TERM "$DPID" 2>/dev/null || true
        for _ in $(seq 1 240); do alive "$DPID" || break; sleep 0.25; done
    fi
    if alive "${DPID:-}"; then kill_daemon; return 0; fi
    for _ in $(seq 1 40); do alive "$BP" || break; sleep 0.25; done
    if alive "$BP"; then kill_daemon; return 0; fi
    wait "$BP"; STOP_RC="$?"
    BP=""; DPID=""
}
cleanup() {
    kill_daemon
    if [ -n "${PCV_PCF_KEEP:-}" ]; then echo "임시 디렉터리 보존: $STATE"; else rm -rf "$STATE" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT; trap 'exit 143' TERM

cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = PcfE2eFixedPw-not-for-prod
jwt_secret = pcf-e2e-fixed-secret-not-for-prod-000000001
libvirt_uri = test:///default
log_level = info
drain_timeout = 5
EOF

boot() {
    local tag="$1" i
    LOG="$STATE/daemon-$tag.log"
    rm -f "$SOCK"
    T_LAUNCH=$(date +%s)
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        "${EXEC_BIND[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin \
        --chdir / \
        prlimit --core=1:1 -- "$DAEMON_BIN" > "$LOG" 2>&1 < /dev/null &
    BP="$!"
    for ((i = 0; i < 120; i++)); do
        if [ -S "$SOCK" ] && rpc_ok; then
            DPID="$(pcv_iso_daemon_pid "$BP")"
            [ -n "$DPID" ] && break
        fi
        alive "$BP" || return 1
        sleep 0.5
    done
    [ -n "$DPID" ] || return 1
    wait_metrics
}
METRIC=""

wait_metrics() {
    local i code
    for ((i = 0; i < 60; i++)); do
        code="$(pcv_iso_exec "$BP" curl -s -o "$STATE/metrics.txt" -w '%{http_code}' --max-time 5 "$METRICS_URL" 2>/dev/null)"
        if [ "$code" = 200 ]; then
            METRIC="$(awk -v s="$SERIES" '$1 == s {print $2}' "$STATE/metrics.txt" | head -1)"
            METRIC="${METRIC:-0}"
            return 0
        fi
        sleep 0.5
    done
    return 1
}
ckpt_value() {
    [ -f "$CKPT" ] || { echo absent; return; }
    jq -r --arg l "$LABELS" '[.counters[] | select(.name == "purecvisor_rpc_requests_total" and .labels == $l) | .value][0] // "none"' \
        "$CKPT" 2>/dev/null || echo unreadable
}
calls() { local n; for ((n = 0; n < $1; n++)); do rpc_ok || fail "daemon.version 호출 실패"; done; }
num_ge() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a + 0 >= b + 0 && a ~ /^[0-9.]+$/) }'; }
num_eq() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a ~ /^[0-9.]+$/ && a + 0 == b + 0) }'; }
within_window() {
    local el=$(( $(date +%s) - T_LAUNCH ))
    if [ "$el" -lt 50 ]; then pass "$1 기동 뒤 ${el}초(60초 체크포인트 전)"; else fail "$1 기동 뒤 ${el}초 — 60초 체크포인트 전제가 깨졌다"; fi
}
count() { grep -cF -- "$1" "$LOG" 2>/dev/null || true; }

echo "=== 누적 계수기 정지·exec 직전 저장 E2E (MODE=$MODE) ==="

echo "--- 기동 1: SIGTERM ---"
if boot 1; then
    calls 8
    wait_metrics || fail "1 지표 읽기 실패"
    V1="$METRIC"; info "1 V1=$V1 (데몬 PID $DPID)"
    if num_ge "$V1" 8; then pass "1 지표 $SERIES = $V1 (≥ 8)"; else fail "1 지표 값 $V1(기대 ≥ 8)"; fi
    within_window 1
    stop_daemon
    if [ "$STOP_RC" = 0 ]; then pass "1 SIGTERM 정상 종료 rc 0"; else fail "1 종료 rc $STOP_RC(기대 0)"; fi
    c="$(ckpt_value)"
    if num_eq "$c" "$V1"; then pass "1 정지 뒤 체크포인트 값 = V1($c)"; else fail "1 정지 뒤 체크포인트 값 $c(기대 $V1)"; fi
    n="$(count 'checkpoint write failed')$(count 'checkpoint rename failed')"
    if [ "$n" = 00 ]; then pass "1 체크포인트 저장 경고 0"; else fail "1 체크포인트 저장 경고 있음"; fi
else
    fail "1 격리 데몬 기동 실패 — 로그: $(tail -3 "$LOG" 2>/dev/null | tr '\n' ' ')"; kill_daemon; V1=0
fi

echo "--- 기동 2: 재기동·SIGUSR2 ---"
if boot 2; then
    V2="$METRIC"; info "2 V2=$V2"
    if num_ge "$V2" "$((${V1%.*} + 1))"; then pass "2 재기동 뒤 지표 $V2 ≥ V1+1(이어받음)"; else fail "2 재기동 뒤 지표 $V2 < V1+1=$((${V1%.*} + 1))(소실)"; fi
    if [ "$(count 'Counter checkpoint restored')" -ge 1 ]; then pass "2 Counter checkpoint restored 로그"; else fail "2 Counter checkpoint restored 로그 없음"; fi
    calls 5
    wait_metrics || fail "2 지표 읽기 실패"
    V3="$METRIC"; info "2 V3=$V3"
    if num_ge "$V3" "$((${V2%.*} + 5))"; then pass "2 증가 뒤 지표 $V3 ≥ V2+5"; else fail "2 증가 뒤 지표 $V3 < V2+5"; fi
    if [ ${#EXEC_BIND[@]} -eq 0 ]; then
        info "2 SIGUSR2 단계 건너뜀 — 호스트에 $EXEC_TARGET 이 없어 샌드박스 안에서 교체 대상을 덮을 수 없다"
    else
        within_window 2
        pid_before="$DPID"
        kill -USR2 "$DPID" 2>/dev/null || fail "2 SIGUSR2 보내기 실패"
        ok=0
        for _ in $(seq 1 180); do
            if [ "$(count 'All systems operational')" -ge 2 ] && rpc_ok; then ok=1; break; fi
            alive "$DPID" || break
            sleep 0.5
        done
        if [ "$ok" = 1 ]; then pass "2 SIGUSR2 교체 뒤 다시 기동(All systems operational 2줄, 같은 PID $pid_before)"; else fail "2 SIGUSR2 교체 뒤 기동 확인 실패"; fi
        if [ "$(count 'Executing binary upgrade')" -eq 1 ]; then pass "2 Executing binary upgrade 1줄"; else fail "2 Executing binary upgrade $(count 'Executing binary upgrade')줄(기대 1)"; fi
        c="$(ckpt_value)"
        if num_eq "$c" "$V3"; then pass "2 교체 직후 체크포인트 값 = V3($c)"; else fail "2 교체 직후 체크포인트 값 $c(기대 $V3)"; fi
        wait_metrics || fail "2 교체 뒤 지표 읽기 실패"
        V4="$METRIC"; info "2 V4=$V4"
        if num_ge "$V4" "$V3"; then pass "2 교체 뒤 지표 $V4 ≥ V3(이어받음)"; else fail "2 교체 뒤 지표 $V4 < V3=$V3(소실)"; fi
    fi
    V_LAST="$METRIC"
    stop_daemon
    if [ "$STOP_RC" = 0 ]; then pass "2 SIGTERM 정상 종료 rc 0"; else fail "2 종료 rc $STOP_RC(기대 0)"; fi
    c="$(ckpt_value)"
    if num_ge "$c" "$V_LAST"; then pass "2 정지 뒤 체크포인트 값 $c ≥ 마지막 지표 $V_LAST"; else fail "2 정지 뒤 체크포인트 값 $c(기대 ≥ $V_LAST)"; fi
else
    fail "2 격리 데몬 기동 실패 — 로그: $(tail -3 "$LOG" 2>/dev/null | tr '\n' ' ')"; kill_daemon
fi

echo
[ "$MODE" = counterfactual ] && info "MODE=counterfactual — 이 기능이 없는 바이너리면 체크포인트·이어받기 단언 FAIL 이 반사실 RED 다"
echo -e "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
