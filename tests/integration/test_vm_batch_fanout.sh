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
                                                                
. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} vm.batch 팬아웃 효과-테스트: $*"; exit 0; }

                                                             
command -v bwrap   >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v setsid  >/dev/null 2>&1 || skip "setsid 미설치"
command -v python3 >/dev/null 2>&1 || skip "python3 미설치(응답 JSON 파싱에 필요)"

                                              
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

                                                      
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make single 시도"
    make -C "$REPO" single >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
                                                              
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

                                                            
VM_A='batch-vm-a'; VM_B='batch-vm-b'; VM_C='batch-vm-c'
GHOST='batch-vm-ghost'                              
S11_VMS=(batch-vm-s11-1 batch-vm-s11-2 batch-vm-s11-3 batch-vm-s11-4 batch-vm-s11-5)             

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-vmbatch.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }                                                   
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
$(for v in "${S11_VMS[@]}"; do
printf '  <domain type="test">\n    <name>%s</name>\n    <memory>65536</memory>\n    <os><type>hvm</type></os>\n  </domain>\n' "$v"
done)
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
pool_max_conn = 2
EOF

                                                      
send_rpc() { echo "$1" | pcv_iso_uds_nc "$SOCK" 2>/dev/null || true; }

                                                                
                                                       
                                                
                                                                           
                                                                           
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
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
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

                                                                 
                                                                      
                                                                    
                                                                        
                                              
                                                                 
stop_lock_ok=1
for v in "$VM_A" "$VM_B" "$VM_C"; do
    grep -Fq "[vm.stop] graceful submit '$v' (origin=vm.batch)" "$DLOG" || stop_lock_ok=0
done
if [ "$stop_lock_ok" -eq 1 ]; then
    pass "S7(반사실): 배치 stop 이 VM 마다 op-lock 을 잡은 정상 종료 진입점을 지남"
else
    fail "S7(반사실): 배치 stop 의 op-lock 진입 로그 부재"
    grep -F "[vm.stop]" "$DLOG" | tail -10 || true
fi

                                                                 
                                                                   
                                                                            
                                                                         
                                                                
                                                                                         
                                                               
                                                                       
                                    
                                                                 
if command -v sqlite3 >/dev/null 2>&1; then
    VM_STATE_DB="$STATE/var-lib/vm_state.db"
    now_s="$(date +%s)"
    c_sub_before=$(grep -cF "[vm.stop] graceful submit '$VM_C'" "$DLOG")
    a_sub_before=$(grep -cF "[vm.stop] graceful submit '$VM_A'" "$DLOG")
    if sqlite3 "$VM_STATE_DB" "PRAGMA busy_timeout=5000; INSERT OR REPLACE INTO vm_locks (vm_id, op_type, pid, locked_at) VALUES ('$VM_C', 2, 1, $now_s);" >/dev/null 2>&1; then
        BATCH_LOCKED="{\"jsonrpc\":\"2.0\",\"method\":\"vm.batch\",\"params\":{\"action\":\"stop\",\"vms\":[\"$VM_A\",\"$VM_C\"]},\"id\":\"b4\"}"
        RESP_LOCKED="$(send_rpc "$BATCH_LOCKED")"
        info "S10: 잠금 충돌 stop 배치 응답 = ${RESP_LOCKED}"
        c_sub_after=$(grep -cF "[vm.stop] graceful submit '$VM_C'" "$DLOG")
        a_sub_after=$(grep -cF "[vm.stop] graceful submit '$VM_A'" "$DLOG")
                                                                             
        if RESP="$RESP_LOCKED" VA="$VM_A" VC="$VM_C" python3 - <<'PY'
import json, os, sys
d = json.loads(os.environ["RESP"]); r = d.get("result") or {}
va, vc = os.environ["VA"], os.environ["VC"]
ok = r.get("accepted") == [va, vc] and not (r.get("rejected") or [])
if not ok:
    print(f"기대: accepted=[{va},{vc}], rejected=[] / 실제: {r}", file=sys.stderr)
sys.exit(0 if ok else 1)
PY
        then
            pass "S10a(후속 반사실): 잠긴 $VM_C 도 응답 accepted(잠금은 정지 차례에 잡힘), rejected 비어 있음"
        else
            fail "S10a(후속 반사실): 응답 계약 불일치 — $RESP_LOCKED"
        fi
        c_rej=0; c_fail_rows=0
        for i in $(seq 1 20); do
            c_rej=$(grep -cF "[vm.stop] batch stop rejected '$VM_C' (origin=vm.batch)" "$DLOG")
            c_fail_rows="$(sqlite3 "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method='vm.stop' AND target='$VM_C' AND result='fail'" 2>/dev/null || echo 0)"
            [ "$c_rej" -ge 1 ] && [ "${c_fail_rows:-0}" -ge 1 ] && break
            sleep 0.25
        done
        c_sub_after=$(grep -cF "[vm.stop] graceful submit '$VM_C'" "$DLOG")
        a_sub_after=$(grep -cF "[vm.stop] graceful submit '$VM_A'" "$DLOG")
        if [ "$c_rej" -ge 1 ] && [ "${c_fail_rows:-0}" -eq 1 ] && [ "$c_sub_after" -eq "$c_sub_before" ] && [ "$a_sub_after" -gt "$a_sub_before" ]; then
            pass "S10b(후속): $VM_C 는 거절 로그+audit fail 1행·submit 로그 없음, $VM_A 는 정지 제출됨"
        else
            fail "S10b(후속): 거절 보고 불일치(rejected 로그 $c_rej, audit fail $c_fail_rows, C submit $c_sub_before->$c_sub_after, A submit $a_sub_before->$a_sub_after)"
        fi
        sqlite3 "$VM_STATE_DB" "PRAGMA busy_timeout=5000; DELETE FROM vm_locks WHERE vm_id='$VM_C';" >/dev/null 2>&1 || true
    else
        fail "S10: 격리 vm_state DB($VM_STATE_DB)에 잠금 행 삽입 실패"
    fi
else
    note "S10 잠금 충돌 거절 분기 건너뜀 — sqlite3 미설치(vm_locks 행 직접 삽입 필요)"
fi

                                                                 
                                                                             
                                                                                    
                                                                   
                                                
                                                                 
S11_JSON="$(printf '"%s",' "${S11_VMS[@]}")"; S11_JSON="[${S11_JSON%,}]"
BATCH_S11="{\"jsonrpc\":\"2.0\",\"method\":\"vm.batch\",\"params\":{\"action\":\"stop\",\"vms\":${S11_JSON}},\"id\":\"b6\"}"
RESP_S11="$(send_rpc "$BATCH_S11")"
info "S11: 5대 stop 배치 응답 = ${RESP_S11}"
s11_ok=0
for i in $(seq 1 60); do                       
    s11_done=0
    for v in "${S11_VMS[@]}"; do
        grep -F "$v" "$DLOG" 2>/dev/null | grep -Fq "shut down gracefully" && s11_done=$((s11_done+1))
    done
    [ "$s11_done" -eq 5 ] && { s11_ok=1; break; }
    sleep 0.5
done
if [ "$s11_ok" -eq 1 ] && DLOG="$DLOG" VMS="${S11_VMS[*]}" python3 - <<'PY'
import os, sys
vms = os.environ["VMS"].split()
lines = open(os.environ["DLOG"], errors="replace").read().splitlines()
def first(pred):
    for i, l in enumerate(lines):
        if pred(l): return i
    return None
subs = [first(lambda l, v=v: f"[vm.stop] graceful submit '{v}' (origin=vm.batch)" in l) for v in vms]
done = [first(lambda l, v=v: v in l and "shut down gracefully" in l) for v in vms]
if None in subs or None in done:
    print(f"submit/done 로그 누락: subs={subs} done={done}", file=sys.stderr); sys.exit(1)
ok = all(subs[i] < done[i] < subs[i+1] for i in range(len(vms) - 1)) and subs[-1] < done[-1]
if not ok:
    print(f"겹침 의심: subs={subs} done={done}", file=sys.stderr)
sys.exit(0 if ok else 1)
PY
then
    pass "S11(정지 풀): 5대 배치 stop 이 모두 shutoff, submit 이 상한 1 로 겹치지 않음"
else
    fail "S11(정지 풀): 5대 정지 완료/비중첩 불일치(done=$s11_done)"
    grep -F "[vm.stop]" "$DLOG" | tail -12 || true
fi
if command -v sqlite3 >/dev/null 2>&1; then
    s11_rows=0
    for i in $(seq 1 20); do
        s11_rows="$(sqlite3 "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method='vm.stop' AND result='ok' AND target LIKE 'batch-vm-s11-%'" 2>/dev/null || echo 0)"
        [ "${s11_rows:-0}" -ge 5 ] && break
        sleep 0.25
    done
    if [ "${s11_rows:-0}" -eq 5 ]; then
        pass "S11b(정지 풀): 'vm.stop' ok 감사 정확히 5행"
    else
        fail "S11b(정지 풀): vm.stop ok 감사 행 수 $s11_rows (기대 5)"
    fi
fi

                                                                 
                                                     
                                                                                                 
                                                                                       
                                                 
                                                     
                                                                 
BATCH_START="{\"jsonrpc\":\"2.0\",\"method\":\"vm.batch\",\"params\":{\"action\":\"start\",\"vms\":[\"$VM_A\",\"$VM_B\",\"$VM_A\"]},\"id\":\"b5\"}"
RESP_START="$(send_rpc "$BATCH_START")"
info "S8: start 배치 응답 = ${RESP_START}"
if RESP="$RESP_START" VA="$VM_A" VB="$VM_B" python3 - <<'PY'
import json, os, sys
d = json.loads(os.environ["RESP"]); r = d.get("result") or {}
ok = r.get("action") == "start" and r.get("accepted") == [os.environ["VA"], os.environ["VB"]]
ok = ok and r.get("rejected") == [{"vm": os.environ["VA"], "reason": "duplicate"}]
sys.exit(0 if ok else 1)
PY
then
    pass "S9: 중복 이름은 rejected 'duplicate', 나머지는 accepted 순서 유지"
else
    fail "S9: 중복 처리·accepted 순서 불일치 — $RESP_START"
fi
seq_ok=0
for i in $(seq 1 40); do                        
    a_done=$(grep -nF "VM '$VM_A' started successfully (async, origin=vm.batch)" "$DLOG" | head -1 | cut -d: -f1)
    b_sub=$(grep -nF "[vm.start] submit '$VM_B' (origin=vm.batch)" "$DLOG" | head -1 | cut -d: -f1)
    if [ -n "$a_done" ] && [ -n "$b_sub" ]; then
        [ "$b_sub" -gt "$a_done" ] && seq_ok=1
        break
    fi
    sleep 0.25
done
a_sub=$(grep -cF "[vm.start] submit '$VM_A' (origin=vm.batch)" "$DLOG")
if [ "$seq_ok" -eq 1 ] && [ "$a_sub" -eq 1 ]; then
    pass "S8(반사실): 배치 start 가 정식 vm.start 진입점을 지나고 $VM_B 는 $VM_A 완료 뒤 제출(순차, 중복 1회만 제출)"
else
    fail "S8(반사실): 정식 진입점·순차성 증거 부족(a_done=$a_done b_sub=$b_sub a_sub=$a_sub)"
    grep -F "[vm.start]" "$DLOG" | tail -10 || true
fi
if command -v sqlite3 >/dev/null 2>&1; then
    start_rows=0
    for i in $(seq 1 20); do
        start_rows="$(sqlite3 "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method='vm.start' AND target IN ('$VM_A','$VM_B')" 2>/dev/null || echo 0)"
        [ "${start_rows:-0}" -ge 2 ] && break
        sleep 0.25
    done
    if [ "${start_rows:-0}" -eq 2 ]; then
        pass "S8b: VM 마다 'vm.start' 감사 정확히 1행(배치 쪽 중복 audit 없음)"
    else
        fail "S8b: vm.start 감사 행 수 $start_rows (기대 2)"
    fi
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
