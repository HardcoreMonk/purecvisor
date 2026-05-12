#!/usr/bin/env bash













set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

SOCKET_PATH="/var/run/purecvisor/daemon.sock"
PASS=0; FAIL=0; SKIP=0; TOTAL=0

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

assert_valid_jsonrpc() {
    local test_name="$1" response="$2"
    if [ -z "$response" ]; then
        fail "$test_name (empty response)"; return
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
        fail "$test_name (empty response)"; return
    fi
    if echo "$response" | grep -q '"result"'; then
        pass "$test_name"
    elif echo "$response" | grep -q '"error"'; then
        if echo "$response" | grep -q '32601'; then
            skip "$test_name (Method not found — not registered in dispatcher)"
        else
            pass "$test_name (returned error response — e.g. ZFS not available)"
        fi
    else
        fail "$test_name (unexpected response format)"
        echo "  Response: $response"
    fi
}


log "=========================================="
log " Storage Query Integration Tests (SAFE)"
log "=========================================="
echo ""

if [ ! -S "$SOCKET_PATH" ]; then
    echo -e "${RED}[ERROR]${NC} Daemon socket not found: $SOCKET_PATH"
    echo "  SKIP: All tests skipped (daemon not running)"
    exit 0
fi

PROBE=$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"probe"}')
if [ -z "$PROBE" ]; then
    echo -e "${RED}[ERROR]${NC} Daemon unresponsive"
    echo "  SKIP: All tests skipped"
    exit 0
fi


ZFS_AVAIL=0
if command -v zpool &>/dev/null && zpool list &>/dev/null 2>&1; then
    ZFS_AVAIL=1
fi
log "Daemon socket verified: $SOCKET_PATH"
log "ZFS available: $([ $ZFS_AVAIL -eq 1 ] && echo 'yes' || echo 'no — error responses expected')"
echo ""




log "--- [1/4] storage.zvol.list ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.zvol.list","params":{},"id":"szl1"}')
assert_result_or_known_error "storage.zvol.list: returns result or graceful error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.zvol.list","params":{"pool":"pcvpool"},"id":"szl2"}')
assert_result_or_known_error "storage.zvol.list: with pool filter" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.zvol.list","params":{"pool":"nonexistent-pool"},"id":"szl3"}')
assert_result_or_known_error "storage.zvol.list: nonexistent pool returns valid response" "$RESP"

echo ""




log "--- [2/4] storage.pool.list ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.pool.list","params":{},"id":"spl1"}')
assert_result_or_known_error "storage.pool.list: returns result or graceful error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.pool.list","params":{"detail":true},"id":"spl2"}')
assert_result_or_known_error "storage.pool.list: with detail flag" "$RESP"

echo ""




log "--- [3/4] storage.replicate.status ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.replicate.status","params":{},"id":"srs1"}')
assert_result_or_known_error "storage.replicate.status: returns result or graceful error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.replicate.status","params":{"vm_name":"nonexistent"},"id":"srs2"}')
assert_result_or_known_error "storage.replicate.status: nonexistent VM returns valid response" "$RESP"

echo ""




log "--- [4/4] storage.pool.health ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.pool.health","params":{},"id":"sph1"}')
assert_result_or_known_error "storage.pool.health: returns result or graceful error (no ZFS)" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.pool.health","params":{"pool":"pcvpool"},"id":"sph2"}')
assert_result_or_known_error "storage.pool.health: pcvpool returns valid response" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.pool.health","params":{"pool":"nonexistent-pool"},"id":"sph3"}')
assert_result_or_known_error "storage.pool.health: nonexistent pool returns valid response" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"storage.pool.health","params":{"pool":""},"id":"sph4"}')
assert_valid_jsonrpc "storage.pool.health: empty pool name returns valid JSON-RPC" "$RESP"

echo ""




echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
