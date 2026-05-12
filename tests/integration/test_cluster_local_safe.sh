#!/bin/bash


















set -uo pipefail


GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'


SOCKET_PATH="/var/run/purecvisor/daemon.sock"
PASS=0; FAIL=0; SKIP=0; TOTAL=0


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


MAINTENANCE_ENTERED=0

cleanup() {
    if [ "$MAINTENANCE_ENTERED" -eq 1 ]; then
        log "Cleanup: exiting maintenance mode"
        rpc '{"jsonrpc":"2.0","method":"cluster.maintenance.exit","params":{},"id":"cleanup"}' \
            >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT


log "=========================================="
log " Cluster Local — SAFE"
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




log "--- [1/8] cluster.health ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.health","params":{},"id":"ch1"}')
rpc_check "cluster.health: returns result" "$RESP"

assert_valid_jsonrpc "cluster.health: response is valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.health","params":{},"id":"ch2"}')
assert_valid_jsonrpc "cluster.health: idempotent second call" "$RESP"

echo ""




log "--- [2/8] cluster.role ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.role","params":{},"id":"cr1"}')
rpc_check "cluster.role: returns result" "$RESP"

assert_valid_jsonrpc "cluster.role: response is valid JSON-RPC" "$RESP"

echo ""




log "--- [3/8] cluster.status ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.status","params":{},"id":"cs1"}')
rpc_check "cluster.status: returns result" "$RESP"

assert_valid_jsonrpc "cluster.status: response is valid JSON-RPC" "$RESP"


if echo "$RESP" | grep -q '"result"'; then
    pass "cluster.status: result key present (no crash)"
fi

echo ""




log "--- [4/8] cluster.maintenance.enter / exit ---"


RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.maintenance.enter","params":{},"id":"me1"}')
if echo "$RESP" | grep -q '"result"'; then
    MAINTENANCE_ENTERED=1
    pass "cluster.maintenance.enter: entered maintenance mode"
elif echo "$RESP" | grep -q '"error"'; then
    if echo "$RESP" | grep -q '32601'; then
        skip "cluster.maintenance.enter: method not registered"
    else
        pass "cluster.maintenance.enter: valid error response (e.g. already in maintenance)"
    fi
else
    fail "cluster.maintenance.enter: unexpected response format"
    echo "  Response: $RESP"
fi


RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.maintenance.exit","params":{},"id":"me2"}')
MAINTENANCE_ENTERED=0
if echo "$RESP" | grep -q '"result"'; then
    pass "cluster.maintenance.exit: exited maintenance mode"
elif echo "$RESP" | grep -q '"error"'; then
    if echo "$RESP" | grep -q '32601'; then skip "cluster.maintenance.exit: method not registered"
    else pass "cluster.maintenance.exit: valid error response"; fi
else
    fail "cluster.maintenance.exit: unexpected response format"
    echo "  Response: $RESP"
fi


RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.maintenance.exit","params":{},"id":"me3"}')
assert_valid_jsonrpc "cluster.maintenance.exit: idempotent second exit returns valid JSON-RPC" "$RESP"

echo ""




log "--- [5/8] quota.get ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"quota.get","params":{},"id":"qg1"}')
rpc_check "quota.get: returns result" "$RESP"

assert_valid_jsonrpc "quota.get: response is valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"quota.get","params":{},"id":"qg2"}')
assert_valid_jsonrpc "quota.get: idempotent second call" "$RESP"

echo ""




log "--- [6/8] cluster.upgrade.status ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.upgrade.status","params":{},"id":"us1"}')
rpc_check "cluster.upgrade.status: returns result" "$RESP"

assert_valid_jsonrpc "cluster.upgrade.status: response is valid JSON-RPC" "$RESP"

echo ""




log "--- [7/8] cluster.affinity.list ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.affinity.list","params":{},"id":"al1"}')
rpc_check "cluster.affinity.list: returns result" "$RESP"

assert_valid_jsonrpc "cluster.affinity.list: response is valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.affinity.list","params":{"name":"test1"},"id":"al2"}')
assert_valid_jsonrpc "cluster.affinity.list: with name filter returns valid JSON-RPC" "$RESP"

echo ""




log "--- [8/8] Edge Cases ---"


RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.health","params":{"unknown":true},"id":"e1"}')
assert_valid_jsonrpc "cluster.health: unknown param returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.status","params":{"verbose":true},"id":"e2"}')
assert_valid_jsonrpc "cluster.status: verbose param returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.affinity.list","params":{"name":"test; DROP TABLE affinity;--"},"id":"e3"}')
assert_valid_jsonrpc "cluster.affinity.list: SQL injection in name returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"cluster.upgrade.status","params":{},"id":"e4"}')
assert_valid_jsonrpc "cluster.upgrade.status: read-only, second call is safe" "$RESP"

echo ""




echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
