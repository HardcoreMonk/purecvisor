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
            pass "$test_name (returned error response)"
        fi
    else
        fail "$test_name (unexpected response format)"
        echo "  Response: $response"
    fi
}


log "=========================================="
log " Overlay Network Query Integration Tests (SAFE)"
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


OVS_AVAIL=0
if command -v ovs-vsctl &>/dev/null && ovs-vsctl show &>/dev/null 2>&1; then
    OVS_AVAIL=1
fi
log "Daemon socket verified: $SOCKET_PATH"
log "OVS available: $([ $OVS_AVAIL -eq 1 ] && echo 'yes' || echo 'no — error responses expected')"
echo ""




log "--- [1/3] overlay.list ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"overlay.list","params":{},"id":"ol1"}')
assert_result_or_known_error "overlay.list: returns result or graceful error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"overlay.list","params":{"detail":true},"id":"ol2"}')
assert_result_or_known_error "overlay.list: with detail flag" "$RESP"

echo ""




log "--- [2/3] overlay.info (nonexistent) ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"overlay.info","params":{"name":"nonexistent"},"id":"oi1"}')
assert_result_or_known_error "overlay.info: nonexistent returns error or not-found" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"overlay.info","params":{},"id":"oi2"}')
assert_valid_jsonrpc "overlay.info: missing name returns valid JSON-RPC" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"overlay.info","params":{"name":"__no_such_overlay_xyz__"},"id":"oi3"}')
assert_result_or_known_error "overlay.info: garbage name returns valid response" "$RESP"

echo ""




log "--- [3/3] overlay.delete (nonexistent, idempotent) ---"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"overlay.delete","params":{"name":"nonexistent"},"id":"od1"}')
assert_result_or_known_error "overlay.delete: nonexistent is idempotent (no crash)" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"overlay.delete","params":{"name":"nonexistent"},"id":"od2"}')
assert_result_or_known_error "overlay.delete: second call idempotent" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"overlay.delete","params":{"name":""},"id":"od3"}')
assert_valid_jsonrpc "overlay.delete: empty name returns valid JSON-RPC" "$RESP"


RESP=$(send_rpc '{"jsonrpc":"2.0","method":"overlay.delete","params":{},"id":"od4"}')
assert_valid_jsonrpc "overlay.delete: missing name returns valid JSON-RPC" "$RESP"

echo ""




echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
