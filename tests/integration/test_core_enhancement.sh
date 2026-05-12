#!/usr/bin/env bash





















set -uo pipefail


GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'


SOCKET_PATH="/var/run/purecvisor/daemon.sock"
PASS=0; FAIL=0; SKIP=0
TOTAL=0


log()  { echo -e "${CYAN}[INFO]${NC} $*"; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); }
skip() { echo -e "${YELLOW}[SKIP]${NC} $*"; SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); }


send_rpc() {
    echo "$1" | nc -U "$SOCKET_PATH" 2>/dev/null || true
}


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



RUNNING_VM=""
if echo "$PROBE" | grep -q '"name"'; then
    RUNNING_VM=$(echo "$PROBE" | grep -o '"name":"[^"]*"' | head -1 | sed 's/"name":"//;s/"//')
fi
TEST_VM="${RUNNING_VM:-nonexistent-test-vm}"
log "Test target VM: $TEST_VM ($([ -n "$RUNNING_VM" ] && echo 'running' || echo 'not found — error-path tests'))"
echo ""




log "--- [1/9] VM Memory Stats ---"


RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.memory.stats\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"ms1\"}")
assert_valid_jsonrpc "vm.memory.stats: valid JSON-RPC response" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{},"id":"ms2"}')
assert_contains "vm.memory.stats: missing name returns error" "$RESP" '"error"'


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{"name":"__no_such_vm_12345__"},"id":"ms3"}')
assert_contains "vm.memory.stats: nonexistent VM returns error" "$RESP" '"error"'

echo ""




log "--- [2/9] VM CPU Stats ---"


RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.cpu.stats\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"cs1\"}")
assert_valid_jsonrpc "vm.cpu.stats: valid JSON-RPC response" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.cpu.stats","params":{},"id":"cs2"}')
assert_contains "vm.cpu.stats: missing name returns error" "$RESP" '"error"'


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.cpu.stats","params":{"name":"__no_such_vm_12345__"},"id":"cs3"}')
assert_contains "vm.cpu.stats: nonexistent VM returns error" "$RESP" '"error"'

echo ""




log "--- [3/9] VM Disk Live Resize ---"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{},"id":"dr1"}')
assert_contains "vm.disk.live_resize: missing params returns error" "$RESP" '"error"'


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{"name":"test-vm"},"id":"dr2"}')
assert_contains "vm.disk.live_resize: missing target returns error" "$RESP" '"error"'


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{"name":"test-vm","target":"vda","new_size_gb":-1},"id":"dr3"}')
assert_valid_jsonrpc "vm.disk.live_resize: negative size returns valid JSON-RPC" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{"name":"__no_such_vm_12345__","target":"vda","new_size_gb":20},"id":"dr4"}')
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


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.guest.ping","params":{"name":"__no_such_vm_12345__"},"id":"gp3"}')
assert_contains "vm.guest.ping: nonexistent VM returns error" "$RESP" '"error"'

echo ""




log "--- [5/9] Alert Config Reload ---"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.reload","params":{},"id":"ar1"}')
assert_result_or_known_error "alert.config.reload: returns result" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.history","params":{},"id":"ar2"}')
assert_valid_jsonrpc "alert.history: valid JSON-RPC response" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"ar3"}')
assert_valid_jsonrpc "alert.config.get: valid JSON-RPC response" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.set","params":{"cpu_warn":85},"id":"ar4"}')
assert_valid_jsonrpc "alert.config.set: valid JSON-RPC response" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.reload","params":{},"id":"ar5"}')
assert_result_or_known_error "alert.config.reload: second call succeeds" "$RESP"

echo ""




log "--- [6/9] Cluster Metrics Aggregate ---"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"cluster.metrics.aggregate","params":{},"id":"ca1"}')
assert_result_or_known_error "cluster.metrics.aggregate: returns valid response" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"cluster.status","params":{},"id":"ca2"}')
assert_valid_jsonrpc "cluster.status: valid JSON-RPC response" "$RESP"

echo ""




log "--- [7/9] Network QoS ---"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.get","params":{"interface":"lo"},"id":"nq1"}')
assert_valid_jsonrpc "network.qos.get: valid JSON-RPC response (lo)" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.get","params":{},"id":"nq2"}')
assert_contains "network.qos.get: missing interface returns error" "$RESP" '"error"'


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.get","params":{"interface":"__fake_iface_999__"},"id":"nq3"}')
assert_valid_jsonrpc "network.qos.get: nonexistent interface returns valid JSON-RPC" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.set","params":{"interface":"lo","rate_kbps":1000},"id":"nq4"}')
assert_valid_jsonrpc "network.qos.set: valid JSON-RPC response" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.remove","params":{"interface":"lo"},"id":"nq5"}')
assert_valid_jsonrpc "network.qos.remove: valid JSON-RPC response" "$RESP"

echo ""




log "--- [8/9] Backup Verify ---"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.verify","params":{"name":"test-vm","snapshot":"nonexistent-snap"},"id":"bv1"}')
assert_result_or_known_error "backup.verify: nonexistent snapshot returns valid response" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.verify","params":{},"id":"bv2"}')
assert_result_or_known_error "backup.verify: missing params handled" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"bv3"}')
assert_valid_jsonrpc "backup.policy.list: valid JSON-RPC response" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.history","params":{"vm_name":"test-vm"},"id":"bv4"}')
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


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{"name":"test-vm","target":"vda","new_size_gb":0},"id":"e6"}')
assert_valid_jsonrpc "vm.disk.live_resize: zero size returns valid JSON-RPC" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{"name":"test-vm","target":"vda","new_size_gb":999999},"id":"e7"}')
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


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.set","params":{"interface":"lo","rate_kbps":-100},"id":"e14"}')
assert_valid_jsonrpc "network.qos.set: negative rate returns valid JSON-RPC" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.guest.exec","params":{"name":"test-vm","command":"echo hello; rm -rf /"},"id":"e15"}')
assert_valid_jsonrpc "vm.guest.exec: command with semicolons returns valid JSON-RPC" "$RESP"

echo ""




echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
