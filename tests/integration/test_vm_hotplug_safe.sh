#!/bin/bash
# tests/integration/test_vm_hotplug_safe.sh
# SAFE
#
# VM Hotplug / Lifecycle Control — SAFE Integration Tests
# Tests vm.pause, vm.resume, vm.metrics, vm.limit against the live daemon.
# The test:///default driver may not support pause/resume — graceful error
# handling (any valid JSON-RPC response) is acceptable.
#
# Prerequisites:
#   - purecvisorsd 또는 purecvisormd running with test:///default libvirt driver
#   - /var/run/purecvisor/daemon.sock present
#   - nc (netcat) installed
#
# Run: sudo bash tests/integration/test_vm_hotplug_safe.sh

set -uo pipefail

# ── Colors ────────────────────────────────────────────────────────────
GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

# ── Config ────────────────────────────────────────────────────────────
SOCKET_PATH="/var/run/purecvisor/daemon.sock"
PASS=0; FAIL=0; SKIP=0; TOTAL=0

# ── Helpers ───────────────────────────────────────────────────────────
log()  { echo -e "${CYAN}[INFO]${NC} $*"; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); }
skip() { echo -e "${YELLOW}[SKIP]${NC} $*"; SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); }

rpc() { echo "$1" | nc -U "$SOCKET_PATH" 2>/dev/null || true; }

rpc_check() {
    local name="$1" resp="$2"
    if [ -z "$resp" ]; then
        fail "$name (empty response)"; return
    fi
    if echo "$resp" | grep -q '"result"'; then
        pass "$name"; return
    fi
    if echo "$resp" | grep -q '"error"'; then
        if echo "$resp" | grep -q '32601'; then
            skip "$name (method not registered)"
        else
            pass "$name (valid error response)"
        fi
    else
        fail "$name (not valid JSON-RPC)"; echo "  Response: $resp"
    fi
}

assert_contains() {
    local name="$1" resp="$2" pat="$3"
    if echo "$resp" | grep -q "$pat"; then pass "$name"
    else fail "$name (expected '$pat')"; echo "  Response: $resp"; fi
}

assert_valid_jsonrpc() {
    local name="$1" resp="$2"
    if [ -z "$resp" ]; then fail "$name (empty response)"; return; fi
    if echo "$resp" | grep -q '"jsonrpc"'; then pass "$name"
    else fail "$name (not valid JSON-RPC)"; echo "  Response: $resp"; fi
}

# ── Preflight ─────────────────────────────────────────────────────────
log "=========================================="
log " VM Hotplug / Lifecycle Control — SAFE"
log "=========================================="
echo ""

if [ ! -S "$SOCKET_PATH" ]; then
    echo -e "${RED}[ERROR]${NC} Daemon socket not found: $SOCKET_PATH"
    echo "  SKIP: All tests skipped (daemon not running)"; exit 0
fi

PROBE=$(rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"probe"}')
if [ -z "$PROBE" ]; then
    echo -e "${RED}[ERROR]${NC} Daemon unresponsive"
    echo "  SKIP: All tests skipped"; exit 0
fi

log "Daemon socket verified: $SOCKET_PATH"

# Pick first available VM name from vm.list; fall back to a dummy name
TEST_VM=""
if echo "$PROBE" | grep -q '"name"'; then
    TEST_VM=$(echo "$PROBE" | grep -o '"name":"[^"]*"' | head -1 | sed 's/"name":"//;s/"//')
fi
TEST_VM="${TEST_VM:-test1}"
log "Test target VM: $TEST_VM"
echo ""

# ══════════════════════════════════════════════════════════════════════
# [1] vm.pause
# ══════════════════════════════════════════════════════════════════════
log "--- [1/5] vm.pause ---"

RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.pause\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"p1\"}")
assert_valid_jsonrpc "vm.pause: valid JSON-RPC response" "$RESP"

RESP=$(rpc '{"jsonrpc":"2.0","method":"vm.pause","params":{},"id":"p2"}')
assert_contains "vm.pause: missing name returns error" "$RESP" '"error"'

RESP=$(rpc '{"jsonrpc":"2.0","method":"vm.pause","params":{"name":"__nonexistent_vm_9999__"},"id":"p3"}')
assert_contains "vm.pause: nonexistent VM returns error" "$RESP" '"error"'

echo ""

# ══════════════════════════════════════════════════════════════════════
# [2] vm.resume
# ══════════════════════════════════════════════════════════════════════
log "--- [2/5] vm.resume ---"

RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.resume\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"r1\"}")
assert_valid_jsonrpc "vm.resume: valid JSON-RPC response" "$RESP"

RESP=$(rpc '{"jsonrpc":"2.0","method":"vm.resume","params":{},"id":"r2"}')
assert_contains "vm.resume: missing name returns error" "$RESP" '"error"'

RESP=$(rpc '{"jsonrpc":"2.0","method":"vm.resume","params":{"name":"__nonexistent_vm_9999__"},"id":"r3"}')
assert_contains "vm.resume: nonexistent VM returns error" "$RESP" '"error"'

echo ""

# ══════════════════════════════════════════════════════════════════════
# [3] vm.metrics
# ══════════════════════════════════════════════════════════════════════
log "--- [3/5] vm.metrics ---"

RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.metrics\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"m1\"}")
assert_valid_jsonrpc "vm.metrics: valid JSON-RPC response" "$RESP"

RESP=$(rpc '{"jsonrpc":"2.0","method":"vm.metrics","params":{},"id":"m2"}')
assert_valid_jsonrpc "vm.metrics: missing name returns valid JSON-RPC" "$RESP"

RESP=$(rpc '{"jsonrpc":"2.0","method":"vm.metrics","params":{"name":"__nonexistent_vm_9999__"},"id":"m3"}')
assert_valid_jsonrpc "vm.metrics: nonexistent VM returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [4] vm.limit (resource cap)
# ══════════════════════════════════════════════════════════════════════
log "--- [4/5] vm.limit ---"

RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.limit\",\"params\":{\"name\":\"$TEST_VM\",\"cpu_quota\":50,\"memory_mb\":512},\"id\":\"l1\"}")
assert_valid_jsonrpc "vm.limit: valid JSON-RPC response" "$RESP"

RESP=$(rpc '{"jsonrpc":"2.0","method":"vm.limit","params":{},"id":"l2"}')
assert_contains "vm.limit: missing name returns error" "$RESP" '"error"'

RESP=$(rpc '{"jsonrpc":"2.0","method":"vm.limit","params":{"name":"__nonexistent_vm_9999__","cpu_quota":50},"id":"l3"}')
assert_valid_jsonrpc "vm.limit: nonexistent VM returns valid JSON-RPC" "$RESP"

# cpu_quota out of range — validation should reject or daemon handles gracefully
RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.limit\",\"params\":{\"name\":\"$TEST_VM\",\"cpu_quota\":-1},\"id\":\"l4\"}")
assert_valid_jsonrpc "vm.limit: negative cpu_quota returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [5] Edge Cases / Error Paths
# ══════════════════════════════════════════════════════════════════════
log "--- [5/5] Edge Cases ---"

# Empty VM name
RESP=$(rpc '{"jsonrpc":"2.0","method":"vm.pause","params":{"name":""},"id":"e1"}')
assert_contains "vm.pause: empty name returns error" "$RESP" '"error"'

# SQL injection in name
RESP=$(rpc '{"jsonrpc":"2.0","method":"vm.metrics","params":{"name":"test; DROP TABLE vms;--"},"id":"e2"}')
assert_contains "vm.metrics: SQL injection in name returns error" "$RESP" '"error"'

# Path traversal
RESP=$(rpc '{"jsonrpc":"2.0","method":"vm.limit","params":{"name":"../../etc/passwd","cpu_quota":10},"id":"e3"}')
assert_contains "vm.limit: path traversal in name returns error" "$RESP" '"error"'

echo ""

# ══════════════════════════════════════════════════════════════════════
# Summary
# ══════════════════════════════════════════════════════════════════════
echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
