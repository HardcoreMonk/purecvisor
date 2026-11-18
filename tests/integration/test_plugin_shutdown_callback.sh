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

skip() { echo -e "${YELLOW}[SKIP]${NC} 플러그인 종료 콜백 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v pkg-config >/dev/null 2>&1 || skip "pkg-config 미설치 — 시험 플러그인을 빌드할 수 없다"
CC="$(command -v gcc-14 || command -v cc || true)"
[ -n "$CC" ] || skip "C 컴파일러 미설치 — 시험 플러그인을 빌드할 수 없다"
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

PSC_TMP="${TMPDIR:-/tmp}"
PSC_SOCK_SUFFIX="/pcv-psc.XXXXXX/var-lib/daemon.sock"
[ $(( ${#PSC_TMP} + ${#PSC_SOCK_SUFFIX} )) -le 100 ] || PSC_TMP=/tmp
STATE="$(mktemp -d "$PSC_TMP/pcv-psc.XXXXXX")" || skip "mktemp -d 실패 — 임시 상태 디렉터리를 만들 수 없다"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"
MARK="$STATE/var-lib/plugin-mark.txt"
LOG=""; BP=""; DPID=""; STOP_RC=""

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
    if [ -n "${PCV_PSC_KEEP:-}" ]; then echo "임시 디렉터리 보존: $STATE"; else rm -rf "$STATE" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT; trap 'exit 143' TERM

mkdir -p "$STATE/etc/plugins.d"
printf '%s\n' '#include <stdio.h>' '#include <stdlib.h>' '#include <unistd.h>' \
    '#include "modules/plugin/pcv_plugin_api.h"' > "$STATE/probe.c"
cat >> "$STATE/probe.c" <<'CEOF'

static const PcvPluginMeta META = { "pcv-test-probe", "1.0", "tests", PCV_PLUGIN_ABI_VERSION };
const PcvPluginMeta *pcv_plugin_get_meta(void) { return &META; }
void pcv_plugin_register(PcvPluginRegistry *reg) { (void)reg; }
void pcv_plugin_shutdown(void)
{
    const char *p = getenv("PCV_TEST_PLUGIN_MARK");
    FILE *f = p ? fopen(p, "a") : NULL;
    if (f) { fprintf(f, "shutdown pid=%d\n", (int)getpid()); fclose(f); }
    g_message("[pcv-test-probe] shutdown callback pid=%d", (int)getpid());
}
CEOF

if ! "$CC" -std=gnu23 -D_GNU_SOURCE -w -shared -fPIC -I"$REPO" -I"$REPO/src" -I"$REPO/include" -I"$REPO/include/purecvisor" \
        "$STATE/probe.c" -o "$STATE/etc/plugins.d/probe.so" $(pkg-config --cflags --libs glib-2.0 gio-2.0 json-glib-1.0) \
        > "$STATE/probe-build.log" 2>&1; then
    skip "시험 플러그인 빌드 실패: $(tail -2 "$STATE/probe-build.log" | tr '\n' ' ')"
fi
chmod 0644 "$STATE/etc/plugins.d/probe.so"

chmod 0755 "$STATE/etc" "$STATE/etc/plugins.d"
pcv_iso_trusted_root_args

cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = 0
admin_user = admin
admin_password = PscE2eFixedPw-not-for-prod
jwt_secret = psc-e2e-fixed-secret-not-for-prod-000000001
libvirt_uri = test:///default
log_level = info
drain_timeout = 5
CONF

boot() {
    local i
    LOG="$STATE/daemon.log"
    rm -f "$SOCK"
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_TRUSTED_ARGS[@]}" \
        "${EXEC_BIND[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin \
        --setenv PCV_TEST_PLUGIN_MARK /var/lib/purecvisor/plugin-mark.txt \
        --chdir / \
        prlimit --core=1:1 -- "$DAEMON_BIN" > "$LOG" 2>&1 < /dev/null &
    BP="$!"
    for ((i = 0; i < 120; i++)); do
        if [ -S "$SOCK" ] && rpc_ok; then
            DPID="$(pcv_iso_daemon_pid "$BP")"
            [ -n "$DPID" ] && return 0
        fi
        alive "$BP" || return 1
        sleep 0.5
    done
    return 1
}
count() { grep -cF -- "$1" "$LOG" 2>/dev/null || true; }
marks() { grep -c '^shutdown ' "$MARK" 2>/dev/null || echo 0; }

echo "=== 플러그인 종료 콜백 E2E (MODE=$MODE) ==="
if boot; then
    n="$(count 'Loaded plugin: pcv-test-probe')"
    if [ "$n" -eq 1 ]; then pass "시험 플러그인 로딩 1줄"; else fail "시험 플러그인 로딩 ${n}줄(기대 1): $(grep -F 'plugin' "$LOG" | tail -2 | tr '\n' ' ')"; fi
    if [ ${#EXEC_BIND[@]} -eq 0 ]; then
        info "SIGUSR2 단계 건너뜀 — 호스트에 $EXEC_TARGET 이 없어 샌드박스 안에서 교체 대상을 덮을 수 없다"
    else
        kill -USR2 "$DPID" 2>/dev/null || fail "SIGUSR2 보내기 실패"
        ok=0
        for _ in $(seq 1 180); do
            if [ "$(count 'All systems operational')" -ge 2 ] && rpc_ok; then ok=1; break; fi
            alive "$DPID" || break
            sleep 0.5
        done
        if [ "$ok" = 1 ]; then pass "SIGUSR2 교체 뒤 같은 PID($DPID)로 다시 기동"; else fail "SIGUSR2 교체 뒤 기동 확인 실패"; fi
        n="$(count 'Loaded plugin: pcv-test-probe')"
        if [ "$n" -eq 2 ]; then pass "교체 뒤 새 이미지가 다시 로딩(Loaded plugin 2줄)"; else fail "교체 뒤 Loaded plugin ${n}줄(기대 2)"; fi
        n="$(count '[pcv-test-probe] shutdown callback')"
        if [ "$n" -eq 0 ] && [ "$(marks)" -eq 0 ]; then pass "SIGUSR2 교체는 종료 콜백을 부르지 않는다(0)"; else fail "SIGUSR2 교체 중 종료 콜백 ${n}줄·표지 $(marks)(기대 0)"; fi
    fi
    stop_daemon
    if [ "$STOP_RC" = 0 ]; then pass "SIGTERM 정상 종료 rc 0"; else fail "종료 rc $STOP_RC(기대 0)"; fi
    n="$(count '[pcv-test-probe] shutdown callback')"
    if [ "$n" -eq 1 ]; then pass "종료 콜백 로그 정확히 1줄"; else fail "종료 콜백 로그 ${n}줄(기대 1)"; fi
    if [ "$(marks)" -eq 1 ]; then pass "종료 콜백 표지 정확히 1줄"; else fail "종료 콜백 표지 $(marks)줄(기대 1)"; fi
    n="$(count 'Plugin manager shut down (1 plugins, 1 shutdown callbacks)')"
    if [ "$n" -eq 1 ]; then pass "Plugin manager shut down 1줄"; else fail "Plugin manager shut down ${n}줄(기대 1)"; fi
    n="$(count 'exited cleanly')"
    if [ "$n" -ge 1 ]; then pass "정리 끝까지 진행(exited cleanly)"; else fail "exited cleanly 줄 없음"; fi
else
    fail "격리 데몬 기동 실패 — 로그: $(tail -3 "$LOG" 2>/dev/null | tr '\n' ' ')"; kill_daemon
fi

echo
[ "$MODE" = counterfactual ] && info "MODE=counterfactual — 이 기능이 없는 바이너리면 종료 콜백 단언 FAIL 이 반사실 RED 다"
echo -e "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
