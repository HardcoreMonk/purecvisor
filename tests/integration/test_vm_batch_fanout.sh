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
                                                 
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

skip() { echo -e "${YELLOW}[SKIP]${NC} vm.batch 팬아웃 효과-테스트: $*"; exit 0; }

                                                             
command -v bwrap   >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v nc      >/dev/null 2>&1 || skip "nc(netcat) 미설치"
command -v setsid  >/dev/null 2>&1 || skip "setsid 미설치"
command -v python3 >/dev/null 2>&1 || skip "python3 미설치(응답 JSON 파싱에 필요)"

                             
if ! bwrap --unshare-user --uid 0 --gid 0 --ro-bind / / --dev /dev --proc /proc \
        /bin/true >/dev/null 2>&1; then
    skip "비특권 사용자 네임스페이스(uid-map) 불가 — 이 호스트에서 격리 데몬 기동 불가"
fi

                                                      
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make single 시도"
    make -C "$REPO" single >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

                                                            
VM_A='batch-vm-a'; VM_B='batch-vm-b'; VM_C='batch-vm-c'
GHOST='batch-vm-ghost'                              

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-vmbatch.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"                                                     
AUDIT_DB="$STATE/var-lib/pcv_audit.db"                                               
DLOG="$STATE/daemon.log"

                                                       
                                                                   
                                                            
cat > "$STATE/etc/topo.xml" <<XML
<node>
  <domain type="test">
    <name>${VM_A}</name>
    <memory>65536</memory>
    <os><type>hvm</type></os>
  </domain>
  <domain type="test">
    <name>${VM_B}</name>
    <memory>65536</memory>
    <os><type>hvm</type></os>
  </domain>
  <domain type="test">
    <name>${VM_C}</name>
    <memory>65536</memory>
    <os><type>hvm</type></os>
  </domain>
</node>
XML
LIBVIRT_URI='test:///etc/purecvisor/topo.xml'

                                                    
cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
libvirt_uri = ${LIBVIRT_URI}
rest_port = 0
admin_user = admin
admin_password = VmBatchFanout-not-for-prod
jwt_secret = vmbatch-fanout-fixed-secret-not-for-prod-0001
log_level = info
drain_timeout = 1
EOF

                                                      
send_rpc() { echo "$1" | nc -U "$SOCK" 2>/dev/null || true; }

                                                                
                                                       
                                                
                                                                           
                                                                           
check_batch_resp() {
    RESP="$1" WANT_ACC="$2" WANT_REJ="$3" python3 - <<'PY'
import json, os, sys
resp = os.environ["RESP"]
want_acc = [x for x in os.environ["WANT_ACC"].split(",") if x]
want_rej = [x for x in os.environ["WANT_REJ"].split(",") if x]
try:
    d = json.loads(resp)
except Exception as e:
    print(f"JSON 파싱 실패: {e} :: {resp!r}", file=sys.stderr); sys.exit(1)
res = d.get("result")
if not isinstance(res, dict):
    print(f"result 객체 없음 (error={d.get('error')})", file=sys.stderr); sys.exit(1)
acc = res.get("accepted") or []
rej = res.get("rejected") or []
rej_names = { (r.get("vm") if isinstance(r, dict) else None) for r in rej }
ok = True
for v in want_acc:
    if v not in acc:
        print(f"accepted 에 '{v}' 없음: {acc}", file=sys.stderr); ok = False
for v in want_rej:
    if v in acc:
        print(f"'{v}' 가 accepted 에 잘못 포함(스텁 회귀 의심): {acc}", file=sys.stderr); ok = False
    match = [r for r in rej if isinstance(r, dict) and r.get("vm") == v]
    if not match:
        print(f"rejected 에 '{v}' 없음: {rej}", file=sys.stderr); ok = False
    elif "VM not found" not in (match[0].get("reason") or ""):
        print(f"'{v}' rejected reason 이 'VM not found' 아님: {match[0]}", file=sys.stderr); ok = False
sys.exit(0 if ok else 1)
PY
}

                                             
check_error_resp() {
    RESP="$1" WANT_CODE="$2" WANT_MSG="$3" python3 - <<'PY'
import json, os, sys
resp = os.environ["RESP"]
want_code = int(os.environ["WANT_CODE"]); want_msg = os.environ["WANT_MSG"]
try:
    d = json.loads(resp)
except Exception as e:
    print(f"JSON 파싱 실패: {e} :: {resp!r}", file=sys.stderr); sys.exit(1)
err = d.get("error")
if not isinstance(err, dict):
    print(f"error 객체 없음 (result={d.get('result')})", file=sys.stderr); sys.exit(1)
if err.get("code") != want_code:
    print(f"error.code {err.get('code')} != {want_code}", file=sys.stderr); sys.exit(1)
if want_msg not in (err.get("message") or ""):
    print(f"error.message 에 '{want_msg}' 없음: {err.get('message')!r}", file=sys.stderr); sys.exit(1)
sys.exit(0)
PY
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

boot() {                                    
    setsid bwrap \
        --unshare-user --uid 0 --gid 0 \
        --ro-bind / / \
        --dev /dev \
        --proc /proc \
        --tmpfs /tmp \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PCV_LIBVIRT_URI "$LIBVIRT_URI" \
        --setenv PURECVISOR_LIBVIRT_URI "$LIBVIRT_URI" \
        --chdir / \
        "$DAEMON_BIN" > "$DLOG" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"

    local i
                    
    for i in $(seq 1 60); do
        [ -S "$SOCK" ] && break
        sleep 0.5
    done
    [ -S "$SOCK" ] || return 1
                                      
    for i in $(seq 1 20); do
        local p; p="$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"probe"}')"
        [ -n "$p" ] && return 0
        sleep 0.5
    done
    return 1
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  ADR-0025 효과-테스트: vm.batch 팬아웃                ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE${NC}"
echo -e "${CYAN}  libvirt=$LIBVIRT_URI (running: $VM_A,$VM_B,$VM_C)${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

                                                                 
                                           
                                                                 
if ! boot; then
    fail "S1: 격리 데몬이 UDS 소켓 프로브에 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$DLOG" 2>/dev/null
    note "로컬에서 데몬 부팅이 불가하면(libvirt 등) 이 테스트는 .50 E2E 서버 대상이다."
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 소켓($SOCK) 프로브 응답 (서빙 확인)"

                                                                 
                                                      
                                                                            
                                                         
                                                   
                                                                 
BATCH_STOP="{\"jsonrpc\":\"2.0\",\"method\":\"vm.batch\",\"params\":{\"action\":\"stop\",\"vms\":[\"$VM_A\",\"$VM_B\",\"$VM_C\",\"$GHOST\"]},\"id\":\"b1\"}"
RESP_STOP="$(send_rpc "$BATCH_STOP")"
info "S2: stop 배치 응답 = ${RESP_STOP}"
if check_batch_resp "$RESP_STOP" "$VM_A,$VM_B,$VM_C" "$GHOST"; then
    pass "S2(반사실): accepted={$VM_A,$VM_B,$VM_C}, rejected={$GHOST:'VM not found'} — 미존재는 accept 안 됨(스텁이면 RED)"
else
    fail "S2(반사실): accept/reject 판별 실패 (스텁 회귀 의심) — 위 진단 참고"
fi

                                                                 
                                                              
                                                   
                                                                 
BATCH_PAUSE="{\"jsonrpc\":\"2.0\",\"method\":\"vm.batch\",\"params\":{\"action\":\"pause\",\"vms\":[\"$VM_A\"]},\"id\":\"b2\"}"
RESP_PAUSE="$(send_rpc "$BATCH_PAUSE")"
info "S3: pause 배치 응답 = ${RESP_PAUSE}"
if check_error_resp "$RESP_PAUSE" "-32602" "unsupported batch action"; then
    pass "S3(반사실): action=pause → error -32602 'unsupported batch action' (스텁이면 accepted 반환이라 RED)"
else
    fail "S3(반사실): pause 가 -32602 'unsupported batch action' 을 반환하지 않음 — 위 진단 참고"
fi

                                                                 
                                      
                                                   
                                                                 
BATCH_NOVMS='{"jsonrpc":"2.0","method":"vm.batch","params":{"action":"stop"},"id":"b3"}'
RESP_NOVMS="$(send_rpc "$BATCH_NOVMS")"
info "S4: vms 누락 응답 = ${RESP_NOVMS}"
if check_error_resp "$RESP_NOVMS" "-32602" "action and vms"; then
    pass "S4(계약): vms 누락 → error -32602 'action and vms[] required'"
else
    fail "S4(계약): vms 누락이 -32602 를 반환하지 않음 — 위 진단 참고"
fi

                                                                 
                                                             
                                                              
                                                        
                                        
                                                                        
                                                    
                                                            
                                                
                                                                 
seen_all_transitions() {
    local v
    for v in "$VM_A" "$VM_B" "$VM_C"; do
        grep -F "$v" "$DLOG" 2>/dev/null | grep -Fq "shut down gracefully" || return 1
    done
    return 0
}
TRANS_OK=0
for i in $(seq 1 30); do                       
    if seen_all_transitions; then TRANS_OK=1; break; fi
    sleep 0.5
done
if [ "$TRANS_OK" -eq 1 ]; then
    pass "S5(실 전이/dispatch 증거): $VM_A/$VM_B/$VM_C 각각 stop 워커가 VIR_DOMAIN_SHUTOFF 관측 후 'shut down gracefully' 기록 — 실제 팬아웃 dispatch + 상태전이(스텁이면 0줄이라 RED)"
else
    fail "S5(실 전이/dispatch 증거): 일부 VM 의 stop 워커 상태전이 로그 부재 (팬아웃 미dispatch 의심)"
    echo "---- daemon.log: 'shut down'/'destroy' 라인 ----"
    grep -E "shut down|destroy|batch-vm-" "$DLOG" 2>/dev/null | tail -20 || echo "(관련 로그 없음)"
fi

                                                                 
                                                               
                                                               
                                                  
                                                         
                         
                                                                 
if command -v sqlite3 >/dev/null 2>&1; then
    stop_rows=0
    for i in $(seq 1 20); do
        stop_rows="$(sqlite3 "$AUDIT_DB" "SELECT COUNT(DISTINCT target) FROM audit_log WHERE method='vm.stop' AND target IN ('$VM_A','$VM_B','$VM_C')" 2>/dev/null || echo 0)"
        [ "${stop_rows:-0}" -ge 3 ] && break
        sleep 0.25
    done
    info "S6: audit(vm.stop, accepted VM) distinct target 수 = ${stop_rows}"
    if [ "${stop_rows:-0}" -ge 3 ]; then
        pass "S6(M-1 반사실): 팬아웃 accepted 3 VM 각각 per-VM 'vm.stop' 감사 기록 — 옛 NULL 콜백이면 0행이라 RED"
    else
        fail "S6(M-1 반사실): per-VM 'vm.stop' 감사 행 부족(distinct target=$stop_rows, 기대≥3) — M-1 콜백 미배선 의심"
    fi
else
    note "S6 per-VM 감사 반사실 건너뜀 — sqlite3 미설치(팬아웃/전이 단언 S2~S5 는 유효)"
fi

note ".50 이연: 각 VM 의 최종 상태를 배치와 **독립한** 조회(virsh/vm.list)로"
note "          교차 재조회하는 실-VM E2E 는 실 libvirt(공유 상태) 필요 → .50 배포 게이트."
note "          (test 드라이버는 연결별 인메모리라 로컬 교차 재조회는 비결정적 — S5 는"
note "           워커 자신의 진짜 전이 관측 로그로 dispatch+전이를 결정적으로 고정한다.)"

                                                                 
    
                                                                 
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
