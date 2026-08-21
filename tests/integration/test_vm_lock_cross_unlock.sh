#!/usr/bin/env bash
                                                                                             
                                                                         
                                                       
                                                               
                                                
 
                                            
                                                                         
                                                                           
                                                                      
                                                                        
                                                                             
                                                                      
                                                                   
                                                                  
                                                   
                                            
                                           
                                                             
                                                        
 
                         
                                                                         
                                                                      
                                                                          
                                                             
                                                               
                                                   
                                                   
                                                                            
                        
                                                                        
                                                                       
                                                         
                              
                                                                 
                                                            
 
                                                      
                                                                  
                                                                    
                                                                     
                                                            
                                                              
                                                                       
 
                                                              
 
                                                         
                                             

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
note() { echo -e "${YELLOW}[NOTE]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="$REPO/bin/purecvisorsd"

skip() { echo -e "${YELLOW}[SKIP]${NC} CMP-1 E2E: $*"; exit 0; }

                                                             
command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"

                             
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
    for p in 28082 29082 31082 33082 37082 41082 43082; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-cmp1.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc"
DB="$STATE/var-lib/vm_state.db"
SOCK="$STATE/var-lib/daemon.sock"

                                     
SUF="$$-$RANDOM"
VM_LOCKED="cmp1-locked-$SUF"                      
VM_REGRESS="cmp1-regress-$SUF"                   
                                               
CLONE_SRC_A="cmp7-src-a-$SUF"                                              
CLONE_TGT_A="cmp7-tgt-a-$SUF"                           
CLONE_SRC_B="cmp7-src-b-$SUF"                                         
CLONE_TGT_B="cmp7-tgt-b-$SUF"                                           
                                                               
                                                                    
                                                           
                                                          
                                                          
HOTPLUG_VM="test"                                                

                                                            
                                                                  
                                                                      
                                                    
                                                             
LOCK_PID="$$"

                                                         
cat > "$STATE/uds_call.py" <<'PYEOF'
import socket, sys
sock_path, payload = sys.argv[1], sys.argv[2]
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(8.0)
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

sq() {                                                  
                                                                     
    sqlite3 -cmd ".timeout 5000" "$DB" "$1" 2>/dev/null
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
admin_password = Cmp1E2eFixedPw-not-for-prod
jwt_secret = cmp1-e2e-fixed-secret-not-for-prod-0001
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

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  CMP-1 VM 락 교차 unlock 차단 결정적 E2E            ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE  uds=$SOCK${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

                                                                 
                                              
                                                                 
write_conf
if ! boot; then
    fail "S1: 격리 데몬이 UDS 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
                    
tbl="$(sq "SELECT name FROM sqlite_master WHERE type='table' AND name='vm_locks';")"
if [ "$tbl" = "vm_locks" ]; then
    pass "S1: 격리 데몬 기동 + vm_locks 테이블 준비 ($DB)"
else
    fail "S1: vm_locks 테이블 부재 (tbl='$tbl')"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

                                                                 
                                                
                                                                     
                                                                 
NOW="$(date +%s)"
sq "INSERT INTO vm_locks (vm_id, op_type, pid, locked_at) VALUES ('$VM_LOCKED', 3, $LOCK_PID, $NOW);"
row="$(sq "SELECT vm_id || '|' || op_type || '|' || pid FROM vm_locks WHERE vm_id='$VM_LOCKED';")"
if [ "$row" = "$VM_LOCKED|3|$LOCK_PID" ]; then
    pass "S2: DELETING 락 보유 ($row) — op_type 3 = VM_OP_DELETING, pid=$LOCK_PID(alive)"
else
    fail "S2: DELETING 락 INSERT 실패 (row='$row')"
    exit 1
fi

                                                                 
                                                   
                                                              
                                       
                                                                 
lim_resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.limit\",\"params\":{\"vm_id\":\"$VM_LOCKED\",\"cpu\":50},\"id\":\"limit1\"}")"
info "S3: vm.limit 응답 = ${lim_resp:-<empty>}"
sleep 1.5                  
cnt_core="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$VM_LOCKED';")"
if [ "$cnt_core" = "1" ]; then
    pass "S4(CMP-1 핵심): vm.limit 후 DELETING 락 잔존 (count=$cnt_core) — 교차 unlock 차단"
else
    fail "S4(CMP-1 핵심): vm.limit 후 count=$cnt_core (기대 1) — 락이 교차 삭제됨(수정 미적용/회귀)"
fi

                                                                 
                                             
                                                            
                                                                 
stop_resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.stop\",\"params\":{\"vm_id\":\"$VM_LOCKED\"},\"id\":\"stop-blocked\"}")"
info "S5: vm.stop(락 보유 VM) 응답 = ${stop_resp:-<empty>}"
sleep 0.5
cnt_after_stop="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$VM_LOCKED';")"
                                                        
rejected=0
case "$stop_resp" in
    *'"error"'*) rejected=1 ;;
esac
locked_word=0
case "$stop_resp" in
    *locked*|*DELETING*) locked_word=1 ;;
esac
if [ "$rejected" = "1" ] && [ "$locked_word" = "1" ] && [ "$cnt_after_stop" = "1" ]; then
    pass "S5(직렬화): vm.stop 이 DELETING 락 충돌로 거부됨 + 락 잔존 (count=$cnt_after_stop)"
elif [ "$cnt_after_stop" = "1" ]; then
                                                    
    note "S5: DELETING 락은 잔존(count=1)하나 거부 응답 문구 확인 불가 — 응답='$stop_resp'"
    fail "S5(직렬화): vm.stop 거부 응답 문구 미확인 (rejected=$rejected locked_word=$locked_word)"
else
    fail "S5(직렬화): vm.stop 후 DELETING 락 count=$cnt_after_stop (기대 1) — 직렬화 붕괴"
fi

                                                                 
                                                   
                                                                 
                                                
                                                                 
pre_regress="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$VM_REGRESS';")"
reg_resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.stop\",\"params\":{\"vm_id\":\"$VM_REGRESS\"},\"id\":\"stop-regress\"}")"
info "S6: vm.stop(무락 VM, 사전 count=$pre_regress) 응답 = ${reg_resp:-<empty>}"
sleep 1.5                          
cnt_regress="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$VM_REGRESS';")"
if [ "$pre_regress" = "0" ] && [ "$cnt_regress" = "0" ]; then
    pass "S6(무회귀): stop 이 STOPPING 락을 정상 해제 (완료 후 count=$cnt_regress)"
else
    fail "S6(무회귀): stop 후 count=$cnt_regress (기대 0, 사전=$pre_regress) — 락 미해제 회귀"
fi

                                               
final_locked="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$VM_LOCKED';")"
info "최종: DELETING 락(count=$final_locked) / 무회귀 VM 락(count=$cnt_regress)"

                                                                 
                                                                 
                                                                      
                                                         
                                                            
                                                                 
NOW_A="$(date +%s)"
sq "INSERT INTO vm_locks (vm_id, op_type, pid, locked_at) VALUES ('$CLONE_SRC_A', 3, $LOCK_PID, $NOW_A);"
clone_a_resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.clone\",\"params\":{\"source\":\"$CLONE_SRC_A\",\"clone_name\":\"$CLONE_TGT_A\",\"template_prepared\":true},\"id\":\"clone-a\"}")"
info "S7: vm.clone(source DELETING-locked) 응답 = ${clone_a_resp:-<empty>}"
a_err=0; a_busy=0; a_accepted=0
case "$clone_a_resp" in *'"error"'*)  a_err=1 ;; esac
case "$clone_a_resp" in *-32004*)     a_busy=1 ;; esac
case "$clone_a_resp" in *accepted*)   a_accepted=1 ;; esac
if [ "$a_err" = "1" ] && [ "$a_busy" = "1" ] && [ "$a_accepted" = "0" ]; then
    pass "S7(CMP-7 a): source DELETING-locked → vm.clone 동기 busy 거부(-32004), accepted 아님"
else
    fail "S7(CMP-7 a): 예상 busy(-32004,accepted아님) 미확인 (err=$a_err busy=$a_busy accepted=$a_accepted) resp='$clone_a_resp'"
fi

                                                                 
                                                                 
                                                         
                                                                
                                                                          
                                                                 
NOW_B="$(date +%s)"
sq "INSERT INTO vm_locks (vm_id, op_type, pid, locked_at) VALUES ('$CLONE_TGT_B', 4, $LOCK_PID, $NOW_B);"
pre_src_b="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$CLONE_SRC_B';")"
clone_b_resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.clone\",\"params\":{\"source\":\"$CLONE_SRC_B\",\"clone_name\":\"$CLONE_TGT_B\",\"template_prepared\":true},\"id\":\"clone-b\"}")"
info "S8: vm.clone(target CREATING-locked, source 무락 pre=$pre_src_b) 응답 = ${clone_b_resp:-<empty>}"
sleep 0.3                          
src_b_after="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$CLONE_SRC_B';")"
tgt_b_after="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$CLONE_TGT_B';")"
b_err=0; b_busy=0
case "$clone_b_resp" in *'"error"'*) b_err=1 ;; esac
case "$clone_b_resp" in *-32004*)    b_busy=1 ;; esac
if [ "$b_err" = "1" ] && [ "$b_busy" = "1" ] && [ "$pre_src_b" = "0" ] \
   && [ "$src_b_after" = "0" ] && [ "$tgt_b_after" = "1" ]; then
    pass "S8(CMP-7 b): target 충돌 거부(-32004) + 부분획득 source 락 해제(count=0, 누수없음) + target 락 미교차삭제(count=1)"
else
    fail "S8(CMP-7 b): 예상 busy&src0&tgt1, 실제 err=$b_err busy=$b_busy pre_src=$pre_src_b src_after=$src_b_after tgt_after=$tgt_b_after resp='$clone_b_resp'"
fi

                                                                 
                                                                        
                                                                 
                                                                  
                                                           
                                 
                                                                    
                                                        
                                                             
                                                                 
NOW_H="$(date +%s)"
sq "INSERT INTO vm_locks (vm_id, op_type, pid, locked_at) VALUES ('$HOTPLUG_VM', 3, $LOCK_PID, $NOW_H);"
hp_resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"device.disk.attach\",\"params\":{\"vm_id\":\"$HOTPLUG_VM\",\"source\":\"/dev/zero\",\"target\":\"vdb\"},\"id\":\"hotplug-blocked\"}")"
info "S9: device.disk.attach(DELETING-locked) 응답 = ${hp_resp:-<empty>}"
sleep 0.3                       
hp_after="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$HOTPLUG_VM';")"
h_err=0; h_busy=0; h_locked=0
case "$hp_resp" in *'"error"'*)        h_err=1 ;; esac
case "$hp_resp" in *-32004*)           h_busy=1 ;; esac
case "$hp_resp" in *"already locked"*) h_locked=1 ;; esac
if [ "$h_err" = "1" ] && [ "$h_busy" = "1" ] && [ "$h_locked" = "1" ] && [ "$hp_after" = "1" ]; then
    pass "S9(CMP-10): device.disk.attach 이 DELETING 락 충돌로 동기 busy 거부(-32004, 'already locked') + DELETING 락 미교차삭제(count=$hp_after)"
else
    fail "S9(CMP-10): 예상 busy(-32004,'already locked',락잔존) 미확인 (err=$h_err busy=$h_busy locked=$h_locked after=$hp_after) resp='$hp_resp'"
fi

                                                                 
                                                                    
                                                                    
                                                                         
                                                                
                                                                         
                                            
                                                                        
                                                           
                                                                  
                                                            
                     
                                                                 
pre_usb="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$HOTPLUG_VM';")"
if [ "$pre_usb" = "0" ]; then
    NOW_U="$(date +%s)"
    sq "INSERT INTO vm_locks (vm_id, op_type, pid, locked_at) VALUES ('$HOTPLUG_VM', 3, $LOCK_PID, $NOW_U);"
fi
usb_resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.usb.attach\",\"params\":{\"vm_id\":\"$HOTPLUG_VM\",\"vendor_id\":\"0x1234\",\"product_id\":\"0x5678\"},\"id\":\"usb-blocked\"}")"
info "S10: vm.usb.attach(DELETING-locked) 응답 = ${usb_resp:-<empty>}"
sleep 0.3                       
usb_after="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$HOTPLUG_VM';")"
u_err=0; u_busy=0; u_locked=0
case "$usb_resp" in *'"error"'*)        u_err=1 ;; esac
case "$usb_resp" in *-32004*)           u_busy=1 ;; esac
case "$usb_resp" in *"already locked"*) u_locked=1 ;; esac
if [ "$u_err" = "1" ] && [ "$u_busy" = "1" ] && [ "$u_locked" = "1" ] && [ "$usb_after" = "1" ]; then
    pass "S10(CMP-10): vm.usb.attach 이 DELETING 락 충돌로 동기 busy 거부(-32004, 'already locked') + DELETING 락 미교차삭제(count=$usb_after)"
else
    fail "S10(CMP-10): 예상 busy(-32004,'already locked',락잔존) 미확인 (err=$u_err busy=$u_busy locked=$u_locked after=$usb_after) resp='$usb_resp'"
fi

                                                                 
                                                                    
                                                                          
                                                                   
                                                          
                                                                  
                                                                     
                                                             
                                                                 
pre_rz="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$HOTPLUG_VM';")"
if [ "$pre_rz" = "0" ]; then
    NOW_R="$(date +%s)"
    sq "INSERT INTO vm_locks (vm_id, op_type, pid, locked_at) VALUES ('$HOTPLUG_VM', 3, $LOCK_PID, $NOW_R);"
fi
rz_resp="$(uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"vm.resize_disk\",\"params\":{\"name\":\"$HOTPLUG_VM\",\"new_size_gb\":10},\"id\":\"resize-blocked\"}")"
info "S11: vm.resize_disk(DELETING-locked) 응답 = ${rz_resp:-<empty>}"
sleep 0.3                       
rz_after="$(sq "SELECT count(*) FROM vm_locks WHERE vm_id='$HOTPLUG_VM';")"
r_err=0; r_busy=0; r_locked=0; r_accepted=0
case "$rz_resp" in *'"error"'*)        r_err=1 ;; esac
case "$rz_resp" in *-32004*)           r_busy=1 ;; esac
case "$rz_resp" in *"already locked"*) r_locked=1 ;; esac
case "$rz_resp" in *accepted*)         r_accepted=1 ;; esac
if [ "$r_err" = "1" ] && [ "$r_busy" = "1" ] && [ "$r_locked" = "1" ] \
   && [ "$r_accepted" = "0" ] && [ "$rz_after" = "1" ]; then
    pass "S11(락 하드닝): vm.resize_disk 이 DELETING 락 충돌로 동기 busy 거부(-32004, 'already locked'), accepted 아님 + 락 미교차삭제(count=$rz_after)"
else
    fail "S11(락 하드닝): 예상 busy(-32004,'already locked',accepted아님,락잔존) 미확인 (err=$r_err busy=$r_busy locked=$r_locked accepted=$r_accepted after=$rz_after) resp='$rz_resp'"
fi

                                                                 
    
                                                                 
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
