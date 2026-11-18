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

skip() { echo -e "${YELLOW}[SKIP]${NC} 부팅 뒤 자동 시작 E2E: $*"; exit 0; }
                                                             
command -v bwrap     >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3   >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid    >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq        >/dev/null 2>&1 || skip "jq 미설치"
command -v sqlite3   >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v sha256sum >/dev/null 2>&1 || skip "sha256sum 미설치"
command -v prlimit   >/dev/null 2>&1 || skip "prlimit 미설치 — 코어 덤프 억제 불가"
[ -r /proc/sys/kernel/random/boot_id ] || skip "/proc/sys/kernel/random/boot_id 를 읽을 수 없다"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"
if [ "$DAEMON_BIN" -ef "$CAND_BIN" ]; then MODE=candidate; else MODE=counterfactual; fi

                                                            
                                                                        
VBA_TMP="${TMPDIR:-/tmp}"
VBA_SOCK_SUFFIX="/pcv-vba.XXXXXX/var-lib/daemon.sock"
[ $(( ${#VBA_TMP} + ${#VBA_SOCK_SUFFIX} )) -le 100 ] || VBA_TMP=/tmp
STATE="$(mktemp -d "$VBA_TMP/pcv-vba.XXXXXX")" || skip "mktemp -d 실패 — 임시 상태 디렉터리를 만들 수 없다"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }                                                   
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"
MARKER="$STATE/var-lib/vm_boot_autostart.json"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
HOST_BOOT_ID="$(tr -d '[:space:]' < /proc/sys/kernel/random/boot_id)"
FAKE_BOOT_ID="00000000-0000-4000-8000-000000000000"                           
SUMMARY_ZERO="[boot-autostart] summary decision=act started=0 failed=0 rejected=0 already_active=0 lock_busy=0 skipped_state_reason=0 skipped_bridge_missing=0 not_pcv=0"
LOG=""
BP=""; DPID=""; STOP_RC=""

                                                                        
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

                                                 
cat > "$STATE/set_boot_id.py" <<'PYEOF'
import json, sys
path, boot_id = sys.argv[1], sys.argv[2]
with open(path) as f:
    data = json.load(f)
data["boot_id"] = boot_id
with open(path, "w") as f:
    json.dump(data, f)
PYEOF

req() { printf '{"jsonrpc":"2.0","method":"%s","params":%s,"id":"t"}' "$1" "$2"; }
R_BODY=""
rpc_t() {                                                       
    local out
    out="$(python3 -I "$STATE/uds_call.py" "$SOCK" "$(req "$1" "$2")" "$3" 2>/dev/null)"
    if [[ "$out" == *$'\n'* ]]; then R_BODY="${out#*$'\n'}"; else R_BODY=""; fi
}
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
                                                                                       
                                                   
stop_daemon() {
    STOP_RC="timeout"
    [ -n "${BP:-}" ] || return 0
    if [ -n "${DPID:-}" ]; then
        kill -TERM "$DPID" 2>/dev/null || true
        for _ in $(seq 1 240); do alive "$DPID" || break; sleep 0.25; done
    fi
    if alive "${DPID:-}"; then
        kill_daemon
        return 0
    fi
    for _ in $(seq 1 40); do alive "$BP" || break; sleep 0.25; done
    if alive "$BP"; then
        kill_daemon
        return 0
    fi
    wait "$BP"; STOP_RC="$?"
    BP=""; DPID=""
}
cleanup() {
    kill_daemon
    if [ -n "${PCV_VBA_KEEP:-}" ]; then
        echo "임시 디렉터리 보존: $STATE"
    else
        rm -rf "$STATE" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT; trap 'exit 143' TERM

                                                                  
write_conf() {
    cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = 0
admin_user = admin
admin_password = VbaE2eFixedPw-not-for-prod
jwt_secret = vba-e2e-fixed-secret-not-for-prod-000000001
libvirt_uri = test:///default
log_level = info
drain_timeout = 5
EOF
    if [ "$1" = 0 ]; then
        printf '\n[network]\nvm_autostart_after_bootstrap = 0\n' >> "$STATE/etc/daemon.conf"
    fi
}

                                                                                      
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

count() { grep -cF -- "$1" "$LOG" 2>/dev/null || true; }
marker_get() { jq -r ".$1 // empty" "$MARKER" 2>/dev/null; }
mode_of() { stat -c '%a' "$1" 2>/dev/null; }
fingerprint() { printf '%s %s' "$(stat -c '%y' "$MARKER" 2>/dev/null)" "$(sha256sum "$MARKER" 2>/dev/null | cut -d' ' -f1)"; }
audit_rows() { sqlite3 "$AUDIT_DB" "SELECT count(*) FROM audit_log WHERE $1;" 2>/dev/null || echo "?"; }

                                                                        
run_boot() {
    local tag="$1" n
    if ! boot "$tag" "$2" "$3"; then
        fail "$tag 격리 데몬 기동 실패 — 로그: $(tail -3 "$LOG" 2>/dev/null | tr '\n' ' ')"
        kill_daemon
        return 1
    fi
    info "$tag 데몬 PID $DPID"
    stop_daemon
    n="$(count 'All systems operational')"
    if [ "${n:-0}" -eq 1 ]; then pass "$tag All systems operational 1줄"; else fail "$tag All systems operational ${n:-0}줄(기대 1)"; fi
    n="$(count '[boot-autostart] decision=')"
    if [ "$MODE" = candidate ]; then
        if [ "${n:-0}" -eq 1 ]; then pass "$tag [boot-autostart] decision= 1줄"; else fail "$tag [boot-autostart] decision= ${n:-0}줄(기대 1)"; fi
    fi
    if [ "$STOP_RC" = 0 ]; then pass "$tag SIGTERM 정상 종료 rc 0"; else fail "$tag 종료 rc $STOP_RC(기대 0)"; fi
    return 0
}

echo "=== 부팅 뒤 자동 시작 보완 판정 E2E (MODE=$MODE, host boot_id ${HOST_BOOT_ID:0:8}) ==="
write_conf 1

                                                              
echo "--- 기동 A: 표지 없음 ---"
if run_boot A "test:///default" 60; then
    if [ "$MODE" = counterfactual ]; then
        n="$(count '[boot-autostart]')"
        if [ "${n:-0}" -eq 0 ] && [ ! -e "$MARKER" ]; then
            info "A(base) [boot-autostart] 0줄·표지 없음 — 반사실 RED 기대대로(이 기능이 없는 바이너리)"
        else
            fail "A(base) [boot-autostart] ${n:-0}줄·표지 $([ -e "$MARKER" ] && echo 있음 || echo 없음) — 반사실이 무효"
        fi
        echo
        echo -e "PASS=$PASS FAIL=$FAIL"
        [ "$FAIL" -eq 0 ]
        exit $?
    fi
    if [ "$(count '[boot-autostart] decision=record_only')" -eq 1 ]; then pass "A decision=record_only"; else fail "A decision=record_only 아님: $(grep -F '[boot-autostart] decision=' "$LOG" | head -1)"; fi
    if [ -f "$MARKER" ]; then pass "A 표지 생성"; else fail "A 표지 없음($MARKER)"; fi
    m="$(mode_of "$MARKER")"
    if [ "$m" = 600 ]; then pass "A 표지 모드 600"; else fail "A 표지 모드 ${m:-?}(기대 600)"; fi
    if [ "$(marker_get boot_id)" = "$HOST_BOOT_ID" ]; then pass "A 표지 boot_id = 호스트 boot_id"; else fail "A 표지 boot_id $(marker_get boot_id) ≠ $HOST_BOOT_ID"; fi
    if [ "$(marker_get decision)" = record_only ]; then pass "A 표지 decision record_only"; else fail "A 표지 decision $(marker_get decision)"; fi
fi

                                                            
echo "--- 기동 B: 같은 부팅 ---"
before="$(fingerprint)"
if run_boot B "test:///default" 60; then
    if [ "$(count '[boot-autostart] decision=same_boot')" -eq 1 ]; then pass "B decision=same_boot"; else fail "B decision=same_boot 아님: $(grep -F '[boot-autostart] decision=' "$LOG" | head -1)"; fi
    n="$(count 'already evaluated in this boot')"
    if [ "${n:-0}" -eq 1 ]; then pass "B already evaluated in this boot 1줄"; else fail "B already evaluated ${n:-0}줄(기대 1)"; fi
    after="$(fingerprint)"
    if [ -n "$before" ] && [ "$before" = "$after" ]; then pass "B 표지 mtime·sha256 불변"; else fail "B 표지가 바뀌었다: '$before' → '$after'"; fi
fi

                                                              
echo "--- 기동 C: 새 부팅 모사 ---"
python3 -I "$STATE/set_boot_id.py" "$MARKER" "$FAKE_BOOT_ID" || fail "C 표지 boot_id 바꾸기 실패"
if run_boot C "test:///default" 60; then
    if [ "$(count '[boot-autostart] decision=act')" -eq 1 ]; then pass "C decision=act"; else fail "C decision=act 아님: $(grep -F '[boot-autostart] decision=' "$LOG" | head -1)"; fi
    n="$(count "$SUMMARY_ZERO")"; s="$(count '[boot-autostart] summary')"
    if [ "${n:-0}" -eq 1 ] && [ "${s:-0}" -eq 1 ]; then pass "C 요약 줄(전부 0) 정확히 1줄"; else fail "C 요약 줄 ${n:-0}/${s:-0}줄: $(grep -F '[boot-autostart] summary' "$LOG" | head -2 | tr '\n' ' ')"; fi
    r="$(audit_rows "method='vm.boot_autostart' AND result='none' AND target='host started=0 failed=0 rejected=0 already_active=0 lock_busy=0 skipped=0'")"
    if [ "$r" = 1 ]; then pass "C 감사 vm.boot_autostart none 1행"; else fail "C 감사 vm.boot_autostart none ${r}행(기대 1)"; fi
    if [ "$(marker_get boot_id)" = "$HOST_BOOT_ID" ]; then pass "C 표지 boot_id = 호스트 boot_id"; else fail "C 표지 boot_id $(marker_get boot_id)"; fi
    if [ "$(marker_get decision)" = act ]; then pass "C 표지 decision act"; else fail "C 표지 decision $(marker_get decision)"; fi
fi

                                                                
echo "--- 기동 D: 스위치 off ---"
write_conf 0
python3 -I "$STATE/set_boot_id.py" "$MARKER" "$FAKE_BOOT_ID" || fail "D 표지 boot_id 바꾸기 실패"
if run_boot D "test:///default" 60; then
    if [ "$(count '[boot-autostart] decision=disabled')" -eq 1 ]; then pass "D decision=disabled"; else fail "D decision=disabled 아님: $(grep -F '[boot-autostart] decision=' "$LOG" | head -1)"; fi
    if [ "$(marker_get boot_id)" = "$HOST_BOOT_ID" ]; then pass "D 표지 boot_id = 호스트 boot_id"; else fail "D 표지 boot_id $(marker_get boot_id)"; fi
    if [ "$(marker_get decision)" = disabled ]; then pass "D 표지 decision disabled"; else fail "D 표지 decision $(marker_get decision)"; fi
    s="$(count '[boot-autostart] summary')"
    if [ "${s:-0}" -eq 0 ]; then pass "D 요약 줄 0"; else fail "D 요약 줄 ${s}줄(기대 0)"; fi
fi

                                                               
echo "--- 기동 E: libvirt 없음(없는 소켓) ---"
write_conf 1
python3 -I "$STATE/set_boot_id.py" "$MARKER" "$FAKE_BOOT_ID" || fail "E 표지 boot_id 바꾸기 실패"
if run_boot E "qemu+unix:///system?socket=/var/lib/purecvisor/no-such.sock" 120; then
    n="$(count '[boot-autostart] deferred: libvirt unavailable')"
    if [ "${n:-0}" -eq 1 ]; then pass "E deferred: libvirt unavailable 1줄"; else fail "E deferred ${n:-0}줄(기대 1)"; fi
    if [ "$(marker_get boot_id)" = "$FAKE_BOOT_ID" ]; then pass "E 표지 boot_id 가짜 그대로(쓰지 않음)"; else fail "E 표지 boot_id $(marker_get boot_id)(기대 가짜 값)"; fi
    s="$(count '[boot-autostart] summary')"
    if [ "${s:-0}" -eq 0 ]; then pass "E 요약 줄 0"; else fail "E 요약 줄 ${s}줄(기대 0)"; fi
fi

                                                      
r="$(audit_rows "method='vm.boot_autostart'")"
if [ "$r" = 1 ]; then pass "vm.boot_autostart 감사 전체 1행(C)"; else fail "vm.boot_autostart 감사 전체 ${r}행(기대 1)"; fi

echo
echo -e "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
