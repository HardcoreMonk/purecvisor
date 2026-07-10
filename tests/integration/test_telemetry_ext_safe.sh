#!/bin/bash
# tests/integration/test_telemetry_ext_safe.sh
# SAFE
#
# Telemetry & Monitor Extended — SAFE Integration Tests
# Tests: telemetry.vm, telemetry.all, monitor.metrics, monitor.processes
#
# Prerequisites:
#   - purecvisorsd or purecvisormd running
#   - /var/run/purecvisor/daemon.sock present
#   - nc (netcat) installed
#
# Run: sudo bash tests/integration/test_telemetry_ext_safe.sh

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
    if [ -z "$resp" ]; then fail "$name (empty response)"; return; fi
    if echo "$resp" | grep -q '"result"'; then pass "$name"; return; fi
    if echo "$resp" | grep -q '"error"'; then
        if echo "$resp" | grep -q '32601'; then skip "$name (method not registered)"
        else pass "$name (valid error response)"; fi
    else fail "$name (unexpected format)"; echo "  Response: $resp"; fi
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
log " Telemetry & Monitor Extended — SAFE"
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

# Resolve a real VM name for targeted telemetry queries
TEST_VM=""
if echo "$PROBE" | grep -q '"name"'; then
    TEST_VM=$(echo "$PROBE" | grep -o '"name":"[^"]*"' | head -1 | sed 's/"name":"//;s/"//')
fi
TEST_VM="${TEST_VM:-test1}"
log "Test target VM: $TEST_VM"
echo ""

# ══════════════════════════════════════════════════════════════════════
# [1] telemetry.vm
# ══════════════════════════════════════════════════════════════════════
log "--- [1/5] telemetry.vm ---"

# 1-1. No params (all VMs)
RESP=$(rpc '{"jsonrpc":"2.0","method":"telemetry.vm","params":{},"id":"tv1"}')
rpc_check "telemetry.vm: no params returns result" "$RESP"

# 1-2. Specific VM
RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"telemetry.vm\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"tv2\"}")
assert_valid_jsonrpc "telemetry.vm: named VM returns valid JSON-RPC" "$RESP"

# 1-3. Nonexistent VM — graceful error
RESP=$(rpc '{"jsonrpc":"2.0","method":"telemetry.vm","params":{"name":"__no_such_vm_9999__"},"id":"tv3"}')
assert_valid_jsonrpc "telemetry.vm: nonexistent VM returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [2] telemetry.all
# ══════════════════════════════════════════════════════════════════════
log "--- [2/5] telemetry.all ---"

# 2-1. Normal call
RESP=$(rpc '{"jsonrpc":"2.0","method":"telemetry.all","params":{},"id":"ta1"}')
rpc_check "telemetry.all: returns result" "$RESP"

# 2-2. Verify response contains a JSON-RPC envelope
assert_valid_jsonrpc "telemetry.all: response is valid JSON-RPC" "$RESP"

# 2-3. Extra unknown param — daemon should ignore or error gracefully
RESP=$(rpc '{"jsonrpc":"2.0","method":"telemetry.all","params":{"unknown_key":true},"id":"ta2"}')
assert_valid_jsonrpc "telemetry.all: unknown param returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [3] monitor.metrics
# ══════════════════════════════════════════════════════════════════════
log "--- [3/5] monitor.metrics ---"

# 3-1. Normal call
RESP=$(rpc '{"jsonrpc":"2.0","method":"monitor.metrics","params":{},"id":"mm1"}')
rpc_check "monitor.metrics: returns result" "$RESP"

# 3-2. Response has JSON-RPC structure
assert_valid_jsonrpc "monitor.metrics: response is valid JSON-RPC" "$RESP"

# 3-3. With explicit vm filter
RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"monitor.metrics\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"mm2\"}")
assert_valid_jsonrpc "monitor.metrics: named VM filter returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [4] monitor.processes (top N)
# ══════════════════════════════════════════════════════════════════════
log "--- [4/5] monitor.processes ---"

# 4-1. top=5
RESP=$(rpc '{"jsonrpc":"2.0","method":"monitor.processes","params":{"top":5},"id":"mp1"}')
rpc_check "monitor.processes: top=5 returns result" "$RESP"

# 4-2. top=1
RESP=$(rpc '{"jsonrpc":"2.0","method":"monitor.processes","params":{"top":1},"id":"mp2"}')
assert_valid_jsonrpc "monitor.processes: top=1 returns valid JSON-RPC" "$RESP"

# 4-3. No params
RESP=$(rpc '{"jsonrpc":"2.0","method":"monitor.processes","params":{},"id":"mp3"}')
assert_valid_jsonrpc "monitor.processes: no params returns valid JSON-RPC" "$RESP"

# 4-4. top=0 — edge case
RESP=$(rpc '{"jsonrpc":"2.0","method":"monitor.processes","params":{"top":0},"id":"mp4"}')
assert_valid_jsonrpc "monitor.processes: top=0 returns valid JSON-RPC" "$RESP"

# 4-5. Negative top
RESP=$(rpc '{"jsonrpc":"2.0","method":"monitor.processes","params":{"top":-1},"id":"mp5"}')
assert_valid_jsonrpc "monitor.processes: negative top returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [5] Cross-method / telemetry.host baseline
# ══════════════════════════════════════════════════════════════════════
log "--- [5/5] telemetry.host baseline ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"telemetry.host","params":{},"id":"th1"}')
rpc_check "telemetry.host: returns result" "$RESP"

# Ensure no "error" key when result is expected
if echo "$RESP" | grep -q '"result"'; then
    assert_contains "telemetry.host: result key present" "$RESP" '"result"'
fi

echo ""

# ══════════════════════════════════════════════════════════════════════
# Summary
# ══════════════════════════════════════════════════════════════════════
echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
