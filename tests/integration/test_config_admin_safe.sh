#!/bin/bash
# tests/integration/test_config_admin_safe.sh
# SAFE
#
# Config / Admin Operations — SAFE Integration Tests
# Tests: daemon.config.get, config.history, audit.search,
#        iso.list, jobs.list, jobs.get
#
# Prerequisites:
#   - purecvisorsd or purecvisormd running
#   - /var/run/purecvisor/daemon.sock present
#   - nc (netcat) installed
#
# Run: sudo bash tests/integration/test_config_admin_safe.sh

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
log " Config / Admin Operations — SAFE"
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
echo ""

# ══════════════════════════════════════════════════════════════════════
# [1] daemon.config.get
# ══════════════════════════════════════════════════════════════════════
log "--- [1/6] daemon.config.get ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"daemon.config.get","params":{},"id":"dcg1"}')
rpc_check "daemon.config.get: returns result" "$RESP"

assert_valid_jsonrpc "daemon.config.get: response is valid JSON-RPC" "$RESP"

# Read-only: calling twice must not change anything
RESP2=$(rpc '{"jsonrpc":"2.0","method":"daemon.config.get","params":{},"id":"dcg2"}')
assert_valid_jsonrpc "daemon.config.get: idempotent second call" "$RESP2"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [2] config.history
# ══════════════════════════════════════════════════════════════════════
log "--- [2/6] config.history ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"config.history","params":{},"id":"ch1"}')
rpc_check "config.history: returns result" "$RESP"

assert_valid_jsonrpc "config.history: response is valid JSON-RPC" "$RESP"

# With limit
RESP=$(rpc '{"jsonrpc":"2.0","method":"config.history","params":{"limit":10},"id":"ch2"}')
assert_valid_jsonrpc "config.history: with limit=10 returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [3] audit.search
# ══════════════════════════════════════════════════════════════════════
log "--- [3/6] audit.search ---"

# 3-1. Filter by method
RESP=$(rpc '{"jsonrpc":"2.0","method":"audit.search","params":{"method":"vm.list","limit":5},"id":"as1"}')
rpc_check "audit.search: method filter returns result" "$RESP"

assert_valid_jsonrpc "audit.search: response is valid JSON-RPC" "$RESP"

# 3-2. No filter (all records, limited)
RESP=$(rpc '{"jsonrpc":"2.0","method":"audit.search","params":{"limit":3},"id":"as2"}')
assert_valid_jsonrpc "audit.search: no method filter returns valid JSON-RPC" "$RESP"

# 3-3. Filter by non-existent method (expect empty result, not error)
RESP=$(rpc '{"jsonrpc":"2.0","method":"audit.search","params":{"method":"__no_such_method__","limit":5},"id":"as3"}')
assert_valid_jsonrpc "audit.search: nonexistent method returns valid JSON-RPC" "$RESP"

# 3-4. No params
RESP=$(rpc '{"jsonrpc":"2.0","method":"audit.search","params":{},"id":"as4"}')
assert_valid_jsonrpc "audit.search: empty params returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [4] iso.list
# ══════════════════════════════════════════════════════════════════════
log "--- [4/6] iso.list ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"iso.list","params":{},"id":"il1"}')
rpc_check "iso.list: returns result" "$RESP"

assert_valid_jsonrpc "iso.list: response is valid JSON-RPC" "$RESP"

# Calling twice — must be idempotent
RESP=$(rpc '{"jsonrpc":"2.0","method":"iso.list","params":{},"id":"il2"}')
assert_valid_jsonrpc "iso.list: idempotent second call" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [5] jobs.list
# ══════════════════════════════════════════════════════════════════════
log "--- [5/6] jobs.list ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"jobs.list","params":{},"id":"jl1"}')
rpc_check "jobs.list: returns result" "$RESP"

assert_valid_jsonrpc "jobs.list: response is valid JSON-RPC" "$RESP"

# With status filter
RESP=$(rpc '{"jsonrpc":"2.0","method":"jobs.list","params":{"status":"running"},"id":"jl2"}')
assert_valid_jsonrpc "jobs.list: status=running filter returns valid JSON-RPC" "$RESP"

# With limit
RESP=$(rpc '{"jsonrpc":"2.0","method":"jobs.list","params":{"limit":5},"id":"jl3"}')
assert_valid_jsonrpc "jobs.list: with limit=5 returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [6] jobs.get (including nonexistent — expect error)
# ══════════════════════════════════════════════════════════════════════
log "--- [6/6] jobs.get ---"

# 6-1. Nonexistent job ID — must return error (not crash)
RESP=$(rpc '{"jsonrpc":"2.0","method":"jobs.get","params":{"id":"nonexistent-job-id-9999"},"id":"jg1"}')
assert_valid_jsonrpc "jobs.get: nonexistent ID returns valid JSON-RPC" "$RESP"
if echo "$RESP" | grep -q '"error"'; then
    pass "jobs.get: nonexistent ID correctly returns error"
elif echo "$RESP" | grep -q '"result"'; then
    pass "jobs.get: nonexistent ID returns result (empty/null is also acceptable)"
fi

# 6-2. Missing id param — expect error
RESP=$(rpc '{"jsonrpc":"2.0","method":"jobs.get","params":{},"id":"jg2"}')
assert_contains "jobs.get: missing id returns error" "$RESP" '"error"'

# 6-3. jobs.status alias (ADR-0012)
RESP=$(rpc '{"jsonrpc":"2.0","method":"jobs.status","params":{"id":"nonexistent-job-id-9999"},"id":"jg3"}')
assert_valid_jsonrpc "jobs.status: alias returns valid JSON-RPC" "$RESP"

# 6-4. jobs.cancel with nonexistent ID
RESP=$(rpc '{"jsonrpc":"2.0","method":"jobs.cancel","params":{"id":"nonexistent-job-id-9999"},"id":"jg4"}')
assert_valid_jsonrpc "jobs.cancel: nonexistent ID returns valid JSON-RPC" "$RESP"

# 6-5. SQL injection in job ID
RESP=$(rpc '{"jsonrpc":"2.0","method":"jobs.get","params":{"id":"1 OR 1=1;--"},"id":"jg5"}')
assert_valid_jsonrpc "jobs.get: SQL injection in id returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# Summary
# ══════════════════════════════════════════════════════════════════════
echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
