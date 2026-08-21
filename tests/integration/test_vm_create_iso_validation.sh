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

skip() { echo -e "${YELLOW}[SKIP]${NC} CMP-3 E2E: $*"; exit 0; }

                                                             
command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"

                             
if ! bwrap --unshare-user --uid 0 --gid 0 --ro-bind / / --dev /dev --proc /proc \
        /bin/true >/dev/null 2>&1; then
    skip "비특권 사용자 네임스페이스(uid-map) 불가 — 이 호스트에서 격리 데몬 기동 불가"
fi

                     
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

                                                         
pick_free_port() {
    local p
    for p in 28092 29092 31092 33092 37092 41092 43092; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-cmp3.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
VM="se-isocheck-001"

                                                           
                                                                  
                                                            
                                                           
                                                                      
                                        
cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/sh
                                                          
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
admin_password = Cmp3E2eFixedPw-not-for-prod
jwt_secret = cmp3-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
EOF
}

boot() {                           
    setsid bwrap \
        --unshare-user --uid 0 --gid 0 \
        --ro-bind / / \
        --dev /dev \
        --proc /proc \
        --tmpfs /tmp \
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

                                                                     
assert_rejected() {                              
    local label="$1" iso="$2" resp
    resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.create\",\"params\":{\"name\":\"$VM\",\"vcpu\":1,\"memory_mb\":512,\"disk_size_gb\":5,\"iso_path\":\"$iso\"},\"id\":\"$label\"}")"
    info "$label: iso_path='$iso' 응답 = ${resp:-<empty>}"
    if [ -z "${resp:-}" ]; then
        fail "$label: 빈 응답 (데몬 무응답)"
        return
    fi
    local has_error accepted
    has_error="$(printf '%s' "$resp" | jq -r 'if (.error != null) then "yes" else "no" end' 2>/dev/null)"
    accepted="$(printf '%s' "$resp" | jq -r 'if (.result.accepted == true) then "yes" else "no" end' 2>/dev/null)"
    if [ "$has_error" = "yes" ] && [ "$accepted" != "yes" ]; then
        pass "$label: iso_path='$iso' 거부됨 (.error != null, accepted 아님) — 라이브 배선 실검증"
    else
        fail "$label: iso_path='$iso' 거부 안 됨 (has_error=$has_error accepted=$accepted) — 검증 미배선/회귀"
    fi
}

                                                                          
                                                          
assert_rejected_base_image() {                                
    local label="$1" bimg="$2" resp
    resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.create\",\"params\":{\"name\":\"$VM\",\"vcpu\":1,\"memory_mb\":512,\"disk_size_gb\":5,\"base_image\":\"$bimg\"},\"id\":\"$label\"}")"
    info "$label: base_image='$bimg' 응답 = ${resp:-<empty>}"
    if [ -z "${resp:-}" ]; then
        fail "$label: 빈 응답 (데몬 무응답)"
        return
    fi
    local has_error accepted
    has_error="$(printf '%s' "$resp" | jq -r 'if (.error != null) then "yes" else "no" end' 2>/dev/null)"
    accepted="$(printf '%s' "$resp" | jq -r 'if (.result.accepted == true) then "yes" else "no" end' 2>/dev/null)"
    if [ "$has_error" = "yes" ] && [ "$accepted" != "yes" ]; then
        pass "$label: base_image='$bimg' 거부됨 (.error != null, accepted 아님) — base_image 실검증"
    else
        fail "$label: base_image='$bimg' 거부 안 됨 (has_error=$has_error accepted=$accepted) — 검증 미배선/회귀"
    fi
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  CMP-3 라이브 vm.create ISO 검증 배선 결정적 E2E     ${NC}"
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

                                                                 
                                            
                                                
                                                                 
assert_rejected "C1-etc-shadow" "/etc/shadow"

                                                                 
                                                          
                                                                 
assert_rejected "C2-traversal" "/pcvpool/iso/../../etc/shadow"

                                                                 
                                                       
                                                                            
                                                             
                                                                 
assert_rejected_base_image "C3-baseimg-shadow"   "/etc/shadow"
assert_rejected_base_image "C4-baseimg-traversal" "/pcvpool/img/../../etc/shadow.qcow2"

                                                                 
    
                                                                 
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
