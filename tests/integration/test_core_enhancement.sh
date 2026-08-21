#!/usr/bin/env bash
                                                                                    
                                                                       
                                                                         
                                                                 
                                            
 
                                    
                                                          
                                        
                         
                                                   
                         
                               
                 
                   
                                       
 
        
                                                       
                                        
                     
                    
 
                          
                                                        
                                        
                                                                                      

set -uo pipefail

                                                      
GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

                                                      
SOCKET_PATH="${PCV_TEST_SOCKET_PATH:-/var/run/purecvisor/daemon.sock}"
PASS=0; FAIL=0; SKIP=0
TOTAL=0
RUN_ID="${PCV_TEST_RUN_ID:-$(date +%s)-$$}"
if [[ ! "$RUN_ID" =~ ^[A-Za-z0-9_-]+$ ]] || [ "${#RUN_ID}" -gt 40 ]; then
    echo "Invalid PCV_TEST_RUN_ID (use <=40 ASCII letters, digits, '_' or '-')" >&2
    exit 2
fi
TEST_VM="pcv-missing-${RUN_ID}"
QOS_IFACE="pcvq$(( $$ % 100000 ))"
ALERT_BASE_CAPTURED=0
ALERT_BASE_RESULT=""
FIXTURE_BASE_CAPTURED=0

                                                    
log()  { echo -e "${CYAN}[INFO]${NC} $*"; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); }
skip() { echo -e "${YELLOW}[SKIP]${NC} $*"; SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); }

                                                     
send_rpc() {
    echo "$1" | nc -U "$SOCKET_PATH" 2>/dev/null || true
}

json_result_int() {
    python3 -c 'import json,sys; print(json.load(sys.stdin)["result"][sys.argv[1]])' "$1"
}

cleanup() {
    local status=$? cleanup_failed=0 current result vm_state
    trap - EXIT
    if [ "$ALERT_BASE_CAPTURED" -eq 1 ]; then
        current=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"cleanup-get"}')
        result=$(printf '%s' "$current" | python3 -c 'import json,sys; r=json.load(sys.stdin).get("result"); print(json.dumps(r, sort_keys=True, separators=(",",":")) if r is not None else "")' 2>/dev/null || true)
        if [ -z "$result" ] || [ "$result" != "$ALERT_BASE_RESULT" ]; then
            echo -e "${RED}[CLEANUP-FAIL]${NC} non-mutating alert probe 전후 exact snapshot 불일치" >&2
            cleanup_failed=1
        fi
    fi
    if [ "$FIXTURE_BASE_CAPTURED" -eq 1 ]; then
        vm_state=$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"cleanup-vms"}')
        if ! printf '%s' "$vm_state" | grep -q '"result"' || \
           printf '%s' "$vm_state" | grep -Fq "\"name\":\"$TEST_VM\""; then
            echo -e "${RED}[CLEANUP-FAIL]${NC} run-unique VM fixture 부재 재검증 실패: $TEST_VM" >&2
            cleanup_failed=1
        fi
        if ip link show "$QOS_IFACE" >/dev/null 2>&1; then
            echo -e "${RED}[CLEANUP-FAIL]${NC} run-unique QoS interface가 새로 생김: $QOS_IFACE" >&2
            cleanup_failed=1
        fi
    fi
    [ "$cleanup_failed" -eq 0 ] || exit 90
    exit "$status"
}
trap cleanup EXIT

                                                 
assert_contains() {
    local test_name="$1" response="$2" expected="$3"
    if echo "$response" | grep -q "$expected"; then
        pass "$test_name"
    else
        fail "$test_name (expected '$expected' in response)"
        echo "  Response: $response"
    fi
}

assert_not_contains() {
    local test_name="$1" response="$2" unexpected="$3"
    if echo "$response" | grep -q "$unexpected"; then
        fail "$test_name (unexpected '$unexpected' in response)"
        echo "  Response: $response"
    else
        pass "$test_name"
    fi
}

                                                 
assert_valid_jsonrpc() {
    local test_name="$1" response="$2"
    if [ -z "$response" ]; then
        fail "$test_name (empty response)"
        return
    fi
    if echo "$response" | grep -q '"jsonrpc"'; then
        pass "$test_name"
    else
        fail "$test_name (not valid JSON-RPC)"
        echo "  Response: $response"
    fi
}

                                              
assert_result_or_known_error() {
    local test_name="$1" response="$2"
    if [ -z "$response" ]; then
        fail "$test_name (empty response)"
        return
    fi
    if echo "$response" | grep -q '"result"'; then
        pass "$test_name"
    elif echo "$response" | grep -q '"error"'; then
                                                                    
        if echo "$response" | grep -q '32601'; then
            skip "$test_name (Method not found — not registered in dispatcher)"
        else
            pass "$test_name (returned error response)"
        fi
    else
        fail "$test_name (unexpected response format)"
        echo "  Response: $response"
    fi
}

                                                 
log "=========================================="
log " Core Enhancement Integration Tests"
log "=========================================="
echo ""

if [ ! -S "$SOCKET_PATH" ]; then
    echo -e "${RED}[ERROR]${NC} Daemon socket not found: $SOCKET_PATH"
    echo "  Start purecvisorsd or purecvisormd first"
    echo ""
    if [ "${PCV_REQUIRE_LIVE_DAEMON:-0}" = "1" ]; then
        echo "  FAIL: live daemon is required (PCV_REQUIRE_LIVE_DAEMON=1)"
        exit 1
    fi
    echo "  SKIP: All tests skipped (daemon not running)"
    exit 0
fi

                                  
PROBE=$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"probe"}')
if [ -z "$PROBE" ]; then
    echo -e "${RED}[ERROR]${NC} Daemon socket exists but no response"
    echo "  The daemon may be stuck or shutting down"
    echo ""
    if [ "${PCV_REQUIRE_LIVE_DAEMON:-0}" = "1" ]; then
        echo "  FAIL: responsive live daemon is required (PCV_REQUIRE_LIVE_DAEMON=1)"
        exit 1
    fi
    echo "  SKIP: All tests skipped (daemon unresponsive)"
    exit 0
fi

log "Daemon socket verified: $SOCKET_PATH"
echo ""

if printf '%s' "$PROBE" | grep -Fq "\"name\":\"$TEST_VM\""; then
    echo -e "${RED}[ERROR]${NC} unexpected fixture VM collision: $TEST_VM" >&2
    exit 1
fi
if ip link show "$QOS_IFACE" >/dev/null 2>&1; then
    echo -e "${RED}[ERROR]${NC} unexpected fixture interface collision: $QOS_IFACE" >&2
    exit 1
fi
log "Test target VM: $TEST_VM (captured absent — error-path tests)"
FIXTURE_BASE_CAPTURED=1
echo ""

                                                        
                     
                                                        
log "--- [1/9] VM Memory Stats ---"

                       
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.memory.stats\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"ms1\"}")
assert_valid_jsonrpc "vm.memory.stats: valid JSON-RPC response" "$RESP"

                                   
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{},"id":"ms2"}')
assert_contains "vm.memory.stats: missing name returns error" "$RESP" '"error"'

                 
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.memory.stats\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"ms3\"}")
assert_contains "vm.memory.stats: nonexistent VM returns error" "$RESP" '"error"'

echo ""

                                                        
                  
                                                        
log "--- [2/9] VM CPU Stats ---"

            
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.cpu.stats\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"cs1\"}")
assert_valid_jsonrpc "vm.cpu.stats: valid JSON-RPC response" "$RESP"

              
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.cpu.stats","params":{},"id":"cs2"}')
assert_contains "vm.cpu.stats: missing name returns error" "$RESP" '"error"'

                 
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.cpu.stats\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"cs3\"}")
assert_contains "vm.cpu.stats: nonexistent VM returns error" "$RESP" '"error"'

echo ""

                                                        
                         
                                                        
log "--- [3/9] VM Disk Live Resize ---"

                        
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{},"id":"dr1"}')
assert_contains "vm.disk.live_resize: missing params returns error" "$RESP" '"error"'

                
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.disk.live_resize\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"dr2\"}")
assert_contains "vm.disk.live_resize: missing target returns error" "$RESP" '"error"'

             
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.disk.live_resize\",\"params\":{\"name\":\"$TEST_VM\",\"target\":\"vda\",\"new_size_gb\":-1},\"id\":\"dr3\"}")
assert_valid_jsonrpc "vm.disk.live_resize: negative size returns valid JSON-RPC" "$RESP"

                 
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.disk.live_resize\",\"params\":{\"name\":\"$TEST_VM\",\"target\":\"vda\",\"new_size_gb\":20},\"id\":\"dr4\"}")
assert_contains "vm.disk.live_resize: nonexistent VM returns error" "$RESP" '"error"'

                                            
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.disk.live_resize\",\"params\":{\"name\":\"$TEST_VM\",\"target\":\"vda\",\"new_size_gb\":50},\"id\":\"dr5\"}")
assert_valid_jsonrpc "vm.disk.live_resize: well-formed request returns valid JSON-RPC" "$RESP"

echo ""

                                                        
                            
                                                        
log "--- [4/9] Guest Agent Operations ---"

                 
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.guest.ping\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"gp1\"}")
assert_valid_jsonrpc "vm.guest.ping: valid JSON-RPC response" "$RESP"

                         
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.guest.ping","params":{},"id":"gp2"}')
assert_contains "vm.guest.ping: missing name returns error" "$RESP" '"error"'

                 
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.guest.exec\",\"params\":{\"name\":\"$TEST_VM\",\"command\":\"echo hello\"},\"id\":\"ge1\"}")
assert_valid_jsonrpc "vm.guest.exec: valid JSON-RPC response" "$RESP"

                         
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.guest.exec","params":{},"id":"ge2"}')
assert_contains "vm.guest.exec: missing params returns error" "$RESP" '"error"'

                     
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.guest.shutdown\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"gs1\"}")
                                 
assert_valid_jsonrpc "vm.guest.shutdown: valid JSON-RPC response" "$RESP"

                             
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.guest.shutdown","params":{},"id":"gs2"}')
assert_contains "vm.guest.shutdown: missing name returns error" "$RESP" '"error"'

                           
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.guest.ping\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"gp3\"}")
assert_contains "vm.guest.ping: nonexistent VM returns error" "$RESP" '"error"'

echo ""

                                                        
                         
                                                        
log "--- [5/9] Alert Config Reload ---"

                                                                           
                                  
skip "alert.config.reload: destructive reload omitted (requires disposable-daemon gate)"

                             
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.history","params":{},"id":"ar2"}')
assert_valid_jsonrpc "alert.history: valid JSON-RPC response" "$RESP"

                         
BASE_RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"ar3"}')
assert_contains "alert.config.get: exposes config_revision" "$BASE_RESP" '"config_revision"'
ALERT_BASE_RESULT=$(printf '%s' "$BASE_RESP" | python3 -c 'import json,sys; r=json.load(sys.stdin).get("result"); print(json.dumps(r, sort_keys=True, separators=(",",":")) if r is not None else "")' 2>/dev/null || true)
BASE_REV=$(echo "$BASE_RESP" | json_result_int config_revision)
BASE_WARN=$(echo "$BASE_RESP" | json_result_int cpu_warn)
BASE_CRIT=$(echo "$BASE_RESP" | json_result_int cpu_crit)
if [ -z "$ALERT_BASE_RESULT" ] || [ -z "$BASE_REV" ] || [ -z "$BASE_WARN" ] || [ -z "$BASE_CRIT" ]; then
    echo -e "${RED}[ERROR]${NC} alert config canonical baseline capture failed" >&2
    exit 1
fi
ALERT_BASE_CAPTURED=1
if [ $((BASE_WARN + 1)) -lt "$BASE_CRIT" ]; then
    TARGET_WARN=$((BASE_WARN + 1))
elif [ "$BASE_WARN" -gt 0 ]; then
    TARGET_WARN=$((BASE_WARN - 1))
else
    TARGET_WARN="$BASE_WARN"
fi

                                                          
RESP_REV_MISSING=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"alert.config.set\",\"params\":{\"cpu_warn\":$TARGET_WARN},\"id\":\"ar3m\"}")
assert_contains "alert.config.set: missing revision rejected" "$RESP_REV_MISSING" '"code":-32602'

RESP_REV_STRING=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"alert.config.set\",\"params\":{\"expected_revision\":\"$BASE_REV\",\"cpu_warn\":$TARGET_WARN},\"id\":\"ar3s\"}")
assert_contains "alert.config.set: string revision rejected" "$RESP_REV_STRING" '"code":-32602'

RESP_REV_ZERO=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"alert.config.set\",\"params\":{\"expected_revision\":0,\"cpu_warn\":$TARGET_WARN},\"id\":\"ar3z\"}")
assert_contains "alert.config.set: zero revision rejected" "$RESP_REV_ZERO" '"code":-32602'

RESP_REV_NEGATIVE=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"alert.config.set\",\"params\":{\"expected_revision\":-1,\"cpu_warn\":$TARGET_WARN},\"id\":\"ar3n\"}")
assert_contains "alert.config.set: negative revision rejected" "$RESP_REV_NEGATIVE" '"code":-32602'

RESP_REV_AFTER=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"ar3a"}')
REV_AFTER_INVALID=$(echo "$RESP_REV_AFTER" | json_result_int config_revision)
WARN_AFTER_INVALID=$(echo "$RESP_REV_AFTER" | json_result_int cpu_warn)
CRIT_AFTER_INVALID=$(echo "$RESP_REV_AFTER" | json_result_int cpu_crit)
if [ "$REV_AFTER_INVALID" -eq "$BASE_REV" ] \
    && [ "$WARN_AFTER_INVALID" -eq "$BASE_WARN" ] \
    && [ "$CRIT_AFTER_INVALID" -eq "$BASE_CRIT" ]; then
    pass "alert.config.set revision validation: effective state remains unchanged"
else
    fail "alert.config.set revision validation: state changed after rejection"
fi

                                                                                 
skip "alert.config.set threshold-pair validation: irreversible regression path omitted"

RESP_AFTER=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"ar4d"}')
AFTER_RESULT=$(printf '%s' "$RESP_AFTER" | python3 -c 'import json,sys; r=json.load(sys.stdin).get("result"); print(json.dumps(r, sort_keys=True, separators=(",",":")) if r is not None else "")' 2>/dev/null || true)
if [ -n "$AFTER_RESULT" ] && [ "$AFTER_RESULT" = "$ALERT_BASE_RESULT" ]; then
    pass "alert.config.set failures: exact config snapshot remains unchanged"
else
    fail "alert.config.set failures: exact config snapshot changed after rejection"
fi

                                                                    
skip "alert.config.set success/CLI: irreversible revision change omitted"

                                       
skip "alert.config.reload: second destructive reload omitted"

echo ""

                                                        
                               
                                                        
log "--- [6/9] Cluster Metrics Aggregate ---"

            
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"cluster.metrics.aggregate","params":{},"id":"ca1"}')
assert_result_or_known_error "cluster.metrics.aggregate: returns valid response" "$RESP"

                          
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"cluster.status","params":{},"id":"ca2"}')
assert_valid_jsonrpc "cluster.status: valid JSON-RPC response" "$RESP"

echo ""

                                                        
                 
                                                        
log "--- [7/9] Network QoS ---"

                              
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"network.qos.get\",\"params\":{\"interface\":\"$QOS_IFACE\"},\"id\":\"nq1\"}")
assert_valid_jsonrpc "network.qos.get: absent fixture returns valid JSON-RPC" "$RESP"

                  
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.get","params":{},"id":"nq2"}')
assert_contains "network.qos.get: missing interface returns error" "$RESP" '"error"'

                    
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.get","params":{"interface":"invalid/interface"},"id":"nq3"}')
assert_valid_jsonrpc "network.qos.get: invalid interface returns valid JSON-RPC" "$RESP"

                                                                
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"network.qos.set\",\"params\":{\"interface\":\"$QOS_IFACE\",\"rate_kbps\":1000},\"id\":\"nq4\"}")
assert_contains "network.qos.set: legacy rate unit rejected before tc" "$RESP" '"error"'

                                                         
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.remove","params":{},"id":"nq5"}')
assert_contains "network.qos.remove: missing interface rejected before tc" "$RESP" '"error"'

echo ""

                                                        
                   
                                                        
log "--- [8/9] Backup Verify ---"

                  
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.verify\",\"params\":{\"name\":\"$TEST_VM\",\"snapshot\":\"pcv-missing-$RUN_ID\"},\"id\":\"bv1\"}")
assert_result_or_known_error "backup.verify: nonexistent snapshot returns valid response" "$RESP"

              
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.verify","params":{},"id":"bv2"}')
assert_result_or_known_error "backup.verify: missing params handled" "$RESP"

                           
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"bv3"}')
assert_valid_jsonrpc "backup.policy.list: valid JSON-RPC response" "$RESP"

                     
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.history\",\"params\":{\"vm_name\":\"$TEST_VM\"},\"id\":\"bv4\"}")
assert_valid_jsonrpc "backup.history: valid JSON-RPC response" "$RESP"

echo ""

                                                        
                                 
                                                        
log "--- [9/9] Error Handling & Edge Cases ---"

                   
RESP=$(send_rpc 'NOT JSON AT ALL')
if [ -n "$RESP" ]; then
    assert_contains "Invalid JSON: returns parse error" "$RESP" '"error"'
else
    skip "Invalid JSON: no response (daemon may close connection)"
fi

                  
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"nonexistent.method.xyz","params":{},"id":"e2"}')
assert_contains "Nonexistent method: returns Method not found" "$RESP" '"error"'

            
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{}}')
assert_valid_jsonrpc "Missing id: still returns JSON-RPC" "$RESP"

                              
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":[],"id":"e4"}')
assert_valid_jsonrpc "Array params: returns valid JSON-RPC" "$RESP"

                             
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{"name":""},"id":"e5"}')
assert_contains "vm.memory.stats: empty name returns error" "$RESP" '"error"'

                                  
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.disk.live_resize\",\"params\":{\"name\":\"$TEST_VM\",\"target\":\"vda\",\"new_size_gb\":0},\"id\":\"e6\"}")
assert_valid_jsonrpc "vm.disk.live_resize: zero size returns valid JSON-RPC" "$RESP"

                                   
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.disk.live_resize\",\"params\":{\"name\":\"$TEST_VM\",\"target\":\"vda\",\"new_size_gb\":999999},\"id\":\"e7\"}")
assert_valid_jsonrpc "vm.disk.live_resize: huge size returns valid JSON-RPC" "$RESP"

                                         
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{"name":"test; DROP TABLE vms;--"},"id":"e8"}')
assert_contains "SQL injection in VM name: returns error (validated)" "$RESP" '"error"'

                                  
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{"name":"../../etc/passwd"},"id":"e9"}')
assert_contains "Path traversal in VM name: returns error (validated)" "$RESP" '"error"'

                        
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{"name":"<script>alert(1)</script>"},"id":"e10"}')
assert_contains "XSS in VM name: returns error (validated)" "$RESP" '"error"'

                                    
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.cpu.stats","params":{"name":"test\u0000vm"},"id":"e11"}')
assert_valid_jsonrpc "Null byte in VM name: returns valid JSON-RPC" "$RESP"

                             
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"telemetry.host","params":{},"id":"e12"}')
assert_valid_jsonrpc "telemetry.host: returns valid JSON-RPC" "$RESP"

                
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"monitor.processes","params":{"top":5},"id":"e13"}')
assert_valid_jsonrpc "monitor.processes: returns valid JSON-RPC" "$RESP"

                               
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"network.qos.set\",\"params\":{\"interface\":\"$QOS_IFACE\",\"rate_mbps\":-100},\"id\":\"e14\"}")
assert_contains "network.qos.set: negative rate rejected before tc" "$RESP" '"error"'

                                       
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.guest.exec\",\"params\":{\"name\":\"$TEST_VM\",\"command\":\"echo hello; printf harmless\"},\"id\":\"e15\"}")
assert_valid_jsonrpc "vm.guest.exec: harmless semicolon command on absent fixture returns valid JSON-RPC" "$RESP"

echo ""

                                                        
       
                                                        
echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
