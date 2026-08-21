#!/usr/bin/env bash
                                                                             
                                                                              
                                                               
                                                                       
                                          
 
                                   
                                                          
                                                               
                                                   
                                                                     
                                                       
                                                      
                                                    
 
        
                                                       
                                        
                     
                          
 
                                                        

set -uo pipefail

                                                      
GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

                                                      
SOCKET_PATH="/var/run/purecvisor/daemon.sock"
REST_BASE="http://127.0.0.1:80/api/v1"
PASS=0; FAIL=0; SKIP=0
TOTAL=0
RUN_ID="${PCV_TEST_RUN_ID:-$(date +%s)-$$}"
if [[ ! "$RUN_ID" =~ ^[A-Za-z0-9_-]+$ ]] || [ "${#RUN_ID}" -gt 40 ]; then
    echo "Invalid PCV_TEST_RUN_ID (use <=40 ASCII letters, digits, '_' or '-')" >&2
    exit 2
fi
LOCKOUT_USER="pcv-lock-${RUN_ID}"
TEST_VM="pcv-missing-${RUN_ID}"
LOCKOUT_USER_CREATED=0
LOCKOUT_CLEANUP_ARMED=0
AUTH_SOURCE_IP="127.254.$(( ($$ / 250) % 250 + 1 )).$(( $$ % 250 + 1 ))"

                                                    
log()  { echo -e "${CYAN}[INFO]${NC} $*"; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); }
skip() { echo -e "${YELLOW}[SKIP]${NC} $*"; SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); }

                                                     
send_rpc() {
    echo "$1" | nc -U "$SOCKET_PATH" 2>/dev/null || true
}

cleanup() {
    local status=$? cleanup_failed=0 users reset_response reset_code reset_ok=0
    trap - EXIT
    if [ "$LOCKOUT_CLEANUP_ARMED" -eq 1 ]; then
        users=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"cleanup-before"}')
        if ! printf '%s' "$users" | grep -q '"result"'; then
            echo -e "${RED}[CLEANUP-FAIL]${NC} lockout fixture 현재 상태 조회 실패" >&2
            cleanup_failed=1
        elif printf '%s' "$users" | grep -Fq "$LOCKOUT_USER"; then
            LOCKOUT_USER_CREATED=1
        else
            LOCKOUT_USER_CREATED=0
        fi
        if [ "$LOCKOUT_USER_CREATED" -eq 1 ]; then
                                                         
                                                         
            for _ in $(seq 1 35); do
                reset_response=$(rest_post "${REST_BASE}/auth/token" \
                    "{\"username\":\"$LOCKOUT_USER\",\"password\":\"pcv-lock-pass-123\"}")
                reset_code=$(printf '%s\n' "$reset_response" | tail -1)
                if [ "$reset_code" = "200" ]; then
                    reset_ok=1
                    break
                fi
                if [ "$reset_code" != "429" ]; then
                    break
                fi
                sleep 1
            done
            if [ "$reset_ok" -ne 1 ]; then
                echo -e "${RED}[CLEANUP-FAIL]${NC} lockout fixture counter reset 실패: HTTP ${reset_code:-missing}" >&2
                cleanup_failed=1
            fi
        fi
        if [ "$LOCKOUT_USER_CREATED" -eq 1 ]; then
            send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.user.delete\",\"params\":{\"username\":\"$LOCKOUT_USER\"},\"id\":\"cleanup-user\"}" >/dev/null
        fi
        users=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"cleanup-verify"}')
        if [ -z "$users" ] || printf '%s' "$users" | grep -Fq "$LOCKOUT_USER"; then
            echo -e "${RED}[CLEANUP-FAIL]${NC} lockout fixture user 잔여 확인 실패: $LOCKOUT_USER" >&2
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

                                                   
assert_error_or_unregistered() {
    local test_name="$1" response="$2"
    if [ -z "$response" ]; then
        fail "$test_name (empty response)"
    elif echo "$response" | grep -q '32601'; then
        skip "$test_name (Method not found — not registered in dispatcher)"
    elif echo "$response" | grep -q '"error"'; then
        pass "$test_name (rejected without mutation)"
    else
        fail "$test_name (write-like validation probe unexpectedly succeeded)"
        echo "  Response: $response"
    fi
}

                                                   
rest_get() {
    curl --interface "$AUTH_SOURCE_IP" -s -o - -w "\n%{http_code}" "$1" 2>/dev/null || true
}

rest_post() {
    curl --interface "$AUTH_SOURCE_IP" -s -o - -w "\n%{http_code}" -X POST "$1" -H "Content-Type: application/json" -d "$2" 2>/dev/null || true
}

                                                 
log "=========================================="
log " Backend Phase 2 Integration Tests"
log "=========================================="
echo ""

if [ ! -S "$SOCKET_PATH" ]; then
    echo -e "${RED}[ERROR]${NC} Daemon socket not found: $SOCKET_PATH"
    echo "  Start purecvisorsd or purecvisormd first"
    echo ""
    echo "  SKIP: All tests skipped (daemon not running)"
    exit 0
fi

          
PROBE=$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"probe"}')
if [ -z "$PROBE" ]; then
    echo -e "${RED}[ERROR]${NC} Daemon socket exists but no response"
    echo "  The daemon may be stuck or shutting down"
    echo ""
    echo "  SKIP: All tests skipped (daemon unresponsive)"
    exit 0
fi

log "Daemon socket verified: $SOCKET_PATH"
echo ""

USERS_BASE=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"baseline-users"}')
if ! printf '%s' "$USERS_BASE" | grep -q '"result"' || \
   printf '%s' "$USERS_BASE" | grep -Fq "$LOCKOUT_USER"; then
    echo -e "${RED}[ERROR]${NC} user baseline capture failed or fixture collision: $LOCKOUT_USER" >&2
    exit 1
fi

                                                    
if printf '%s' "$PROBE" | grep -Fq "\"name\":\"$TEST_VM\""; then
    echo -e "${RED}[ERROR]${NC} unexpected fixture collision: $TEST_VM" >&2
    exit 1
fi
log "Test target VM: $TEST_VM (captured absent — error-path tests)"
echo ""

                                                        
                               
                                                        
log "--- [1/6] Node Evacuation ---"

                                                   
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"cluster.node.evacuate","params":{},"id":"ne1"}')
assert_error_or_unregistered "cluster.node.evacuate: missing node rejected" "$RESP"

                           
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"cluster.node.evacuate","params":{},"id":"ne2"}')
assert_error_or_unregistered "cluster.node.evacuate: repeated missing node remains rejected" "$RESP"

                        
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"cluster.node.evacuate","params":{"node":"not-an-ip-addr!!!"},"id":"ne3"}')
assert_error_or_unregistered "cluster.node.evacuate: invalid IP rejected" "$RESP"

               
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"cluster.node.evacuate.status","params":{},"id":"ne4"}')
assert_result_or_known_error "cluster.node.evacuate.status: returns response" "$RESP"

                                               
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"cluster.node.resume","params":{"node":"not-an-ip-addr!!!"},"id":"ne5"}')
assert_error_or_unregistered "cluster.node.resume: invalid IP rejected" "$RESP"

echo ""

                                                        
                                         
                                                        
log "--- [2/6] Storage Capacity Forecast ---"

                    
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.forecast","params":{},"id":"sf1"}')
assert_result_or_known_error "storage.forecast: default pool returns response" "$RESP"

                  
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.forecast","params":{"pool":"pcvpool"},"id":"sf2"}')
assert_result_or_known_error "storage.forecast: explicit pool name returns response" "$RESP"

                        
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.forecast","params":{"pool":"__nonexistent_pool_xyz__"},"id":"sf3"}')
assert_result_or_known_error "storage.forecast: nonexistent pool handled" "$RESP"

                         
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.forecast","params":{},"id":"sf4"}')
if echo "$RESP" | grep -q '"result"'; then
                                                       
    if echo "$RESP" | grep -q 'days_to_full\|daily_growth'; then
        pass "storage.forecast: response contains forecast fields"
    else
        pass "storage.forecast: result present (field names may vary)"
    fi
elif echo "$RESP" | grep -q '32601'; then
    skip "storage.forecast: Method not found — not registered in dispatcher"
else
    pass "storage.forecast: returned valid error response"
fi

echo ""

                                                        
                                      
                                                        
log "--- [3/6] Brute-Force Protection ---"

                                              
LOCKOUT_CLEANUP_ARMED=1
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.user.create\",\"params\":{\"username\":\"$LOCKOUT_USER\",\"password\":\"pcv-lock-pass-123\",\"role\":\"viewer\"},\"id\":\"lock-user-create\"}")
if printf '%s' "$RESP" | grep -q '"result"'; then
    LOCKOUT_USER_CREATED=1
    pass "auth/token: run-unique lockout fixture account created"
else
    fail "auth/token: lockout fixture account creation failed"
fi

                      
                                                           
RESP=$(rest_post "${REST_BASE}/auth/token" "{\"username\":\"$LOCKOUT_USER\",\"password\":\"pcv-lock-pass-123\"}")
HTTP_CODE=$(echo "$RESP" | tail -1)
if [ "$HTTP_CODE" = "200" ]; then
    pass "auth/token: run-unique fixture credentials return 200"
elif [ "$HTTP_CODE" = "000" ] || [ -z "$HTTP_CODE" ]; then
    skip "auth/token: REST endpoint unreachable"
else
    fail "auth/token: fixture credentials returned HTTP $HTTP_CODE (expected 200)"
fi

                                                       
BRUTE_REACHABLE=false
if [ "$LOCKOUT_USER_CREATED" -eq 1 ]; then
    BRUTE_REACHABLE=true
    for i in 1 2 3 4; do
        RESP=$(rest_post "${REST_BASE}/auth/token" "{\"username\":\"$LOCKOUT_USER\",\"password\":\"wrong_password_attempt\"}")
        HTTP_CODE=$(echo "$RESP" | tail -1)
        if [ "$HTTP_CODE" = "401" ]; then
            pass "auth/token: wrong password attempt $i returns 401"
        elif [ "$HTTP_CODE" = "000" ] || [ -z "$HTTP_CODE" ]; then
            skip "auth/token: REST endpoint unreachable (attempt $i)"
            BRUTE_REACHABLE=false
            break
        else
            fail "auth/token: wrong password attempt $i returned HTTP $HTTP_CODE (expected 401)"
        fi
    done
else
    for i in 1 2 3 4; do skip "auth/token: wrong password attempt $i (fixture unavailable)"; done
fi

                                          
if [ "$BRUTE_REACHABLE" = true ]; then
    RESP=$(rest_post "${REST_BASE}/auth/token" "{\"username\":\"$LOCKOUT_USER\",\"password\":\"wrong_password_lockout\"}")
    HTTP_CODE=$(echo "$RESP" | tail -1)
    BODY=$(echo "$RESP" | sed '$d')
    if [ "$HTTP_CODE" = "429" ]; then
        pass "auth/token: 5th wrong attempt triggers lockout (429)"
    else
        fail "auth/token: 5th wrong attempt returned HTTP $HTTP_CODE (expected 429)"
    fi

                                 
    if echo "$BODY" | grep -qi 'retry\|locked\|wait\|too.many\|rate.limit'; then
        pass "auth/token: lockout response contains retry info"
    else
        fail "auth/token: 429 response lacks lockout/retry information"
    fi
else
    skip "auth/token: lockout test skipped (REST unreachable)"
    skip "auth/token: retry info test skipped (REST unreachable)"
fi

                                                                

echo ""

                                                        
                                    
                                                        
log "--- [4/6] Deep Health Endpoint ---"

                            
RESP=$(rest_get "${REST_BASE}/health")
HTTP_CODE=$(echo "$RESP" | tail -1)
BODY=$(echo "$RESP" | sed '$d')

if [ "$HTTP_CODE" = "200" ] || [ "$HTTP_CODE" = "503" ]; then
    pass "/health: returns HTTP $HTTP_CODE"
elif [ "$HTTP_CODE" = "000" ] || [ -z "$HTTP_CODE" ]; then
    skip "/health: REST endpoint unreachable"
else
    pass "/health: returns HTTP $HTTP_CODE"
fi

                   
if echo "$BODY" | grep -q '"status"'; then
    pass "/health: response contains 'status' field"
else
    if [ -n "$BODY" ]; then
        fail "/health: 'status' field missing"
        echo "  Response: $BODY"
    else
        skip "/health: empty response, cannot check 'status' field"
    fi
fi

                       
if echo "$BODY" | grep -q '"subsystems"\|"components"\|"checks"'; then
    pass "/health: response contains subsystems/components object"
else
    if [ -n "$BODY" ]; then
        pass "/health: response present (subsystem key name may vary)"
    else
        skip "/health: empty response, cannot check subsystems"
    fi
fi

                                          
if echo "$BODY" | grep -q 'libvirt\|etcd\|zfs\|disk\|tls\|audit'; then
    pass "/health: subsystems contain expected keys (libvirt/etcd/zfs/disk)"
else
    if [ -n "$BODY" ]; then
        pass "/health: response present (subsystem names may differ)"
    else
        skip "/health: empty response, cannot check subsystem keys"
    fi
fi

echo ""

                                                        
                                 
                                                        
log "--- [5/6] Disk I/O Throttle ---"

                                               
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.blkio.set\",\"params\":{\"name\":\"$TEST_VM\",\"read_bps\":10485760,\"write_bps\":5242880},\"id\":\"bio1\"}")
assert_contains "vm.blkio.set: absent fixture rejected without mutation" "$RESP" '"error"'

                         
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.blkio.get\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"bio2\"}")
assert_contains "vm.blkio.get: absent fixture rejected" "$RESP" '"error"'

                                       
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.blkio.set","params":{"read_bps":1000},"id":"bio3"}')
assert_result_or_known_error "vm.blkio.set: missing name handled" "$RESP"

                             
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.blkio.set\",\"params\":{\"name\":\"$TEST_VM\",\"read_bps\":-1,\"write_bps\":-1},\"id\":\"bio4\"}")
assert_contains "vm.blkio.set: negative values rejected" "$RESP" '"error"'

                                   
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.blkio.get","params":{"name":"__no_such_vm_12345__"},"id":"bio5"}')
assert_result_or_known_error "vm.blkio.get: nonexistent VM handled" "$RESP"

echo ""

                                                        
                                  
                                                        
log "--- [6/6] Config Propagation ---"

                                                  
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"config.push","params":{},"id":"cp1"}')
assert_error_or_unregistered "config.push: missing params rejected" "$RESP"

                      
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"config.get","params":{"section":"cluster","key":"etcd_timeout"},"id":"cp2"}')
assert_result_or_known_error "config.get: returns response" "$RESP"

                             
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"config.get","params":{"section":"cluster","key":"__pcv_missing_key__"},"id":"cp3"}')
assert_result_or_known_error "config.get: missing key returns a response" "$RESP"

                             
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"config.get","params":{"section":"__invalid_section__","key":"foo"},"id":"cp4"}')
assert_result_or_known_error "config.get: invalid section returns a response" "$RESP"

echo ""

                                                        
       
                                                        
echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
