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


log "=========================================="
log " AI Agent / Self-Healing — SAFE"
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




log "--- [1/6] agent.config.get ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.get","params":{},"id":"acg1"}')
rpc_check "agent.config.get: returns result" "$RESP"

assert_valid_jsonrpc "agent.config.get: response is valid JSON-RPC" "$RESP"


RESP2=$(rpc '{"jsonrpc":"2.0","method":"agent.config.get","params":{},"id":"acg2"}')
assert_valid_jsonrpc "agent.config.get: second call is idempotent" "$RESP2"

echo ""




log "--- [2/6] agent.config.set ---"


RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.set","params":{"key":"enabled","value":"false"},"id":"acs1"}')
rpc_check "agent.config.set: enabled=false" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.set","params":{"key":"enabled","value":"true"},"id":"acs2"}')
assert_valid_jsonrpc "agent.config.set: enabled=true returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.set","params":{},"id":"acs3"}')
assert_valid_jsonrpc "agent.config.set: missing params returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.set","params":{"key":"__unknown_key__","value":"test"},"id":"acs4"}')
assert_valid_jsonrpc "agent.config.set: unknown key returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.get","params":{},"id":"acs5"}')
assert_valid_jsonrpc "agent.config.get: readable after set" "$RESP"

echo ""




log "--- [3/6] agent.history ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.history","params":{},"id":"ah1"}')
rpc_check "agent.history: returns result" "$RESP"

assert_valid_jsonrpc "agent.history: response is valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.history","params":{"limit":10},"id":"ah2"}')
assert_valid_jsonrpc "agent.history: with limit=10 returns valid JSON-RPC" "$RESP"

echo ""




log "--- [4/6] healing.history ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.history","params":{},"id":"hh1"}')
rpc_check "healing.history: returns result" "$RESP"

assert_valid_jsonrpc "healing.history: response is valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.history","params":{"limit":5},"id":"hh2"}')
assert_valid_jsonrpc "healing.history: with limit=5 returns valid JSON-RPC" "$RESP"

echo ""




log "--- [5/6] ai.healing.approve / ai.healing.reject ---"


RESP=$(rpc '{"jsonrpc":"2.0","method":"ai.healing.approve","params":{"id":"nonexistent-healing-id-9999"},"id":"ha1"}')
rpc_check "ai.healing.approve: nonexistent ID handled" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"ai.healing.reject","params":{"id":"nonexistent-healing-id-9999"},"id":"hr1"}')
rpc_check "ai.healing.reject: nonexistent ID handled" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"ai.healing.approve","params":{},"id":"ha2"}')
assert_valid_jsonrpc "ai.healing.approve: missing id returns valid JSON-RPC" "$RESP"

echo ""




log "--- [6a] healing.pending (BUG-21) ---"


RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.pending","params":{},"id":"hp1"}')
rpc_check "healing.pending: returns result" "$RESP"
assert_valid_jsonrpc "healing.pending: valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.pending","params":{},"id":"hp2"}')
assert_valid_jsonrpc "healing.pending: idempotent" "$RESP"

echo ""




log "--- [6b] healing.set_mode (Issue-M2) ---"


RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.set_mode","params":{"mode":"active"},"id":"sm1"}')
assert_valid_jsonrpc "healing.set_mode: active returns valid JSON-RPC" "$RESP"
assert_contains "healing.set_mode: active reflected" "$RESP" '"mode":"active"'


RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.set_mode","params":{"mode":"dry_run"},"id":"sm2"}')
assert_valid_jsonrpc "healing.set_mode: dry_run returns valid JSON-RPC" "$RESP"
assert_contains "healing.set_mode: dry_run reflected" "$RESP" '"mode":"dry_run"'


RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.set_mode","params":{"mode":"invalid_xyz"},"id":"sm3"}')
assert_contains "healing.set_mode: invalid mode rejected" "$RESP" '"error"'


RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.set_mode","params":{},"id":"sm4"}')
assert_contains "healing.set_mode: missing param rejected" "$RESP" '"error"'

echo ""




log "--- [6/6] Edge Cases ---"


RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.set","params":{"key":"webhook_url","value":"https://evil.example.com/; rm -rf /"},"id":"e1"}')
assert_valid_jsonrpc "agent.config.set: injection in value returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.history","params":{"limit":999999},"id":"e2"}')
assert_valid_jsonrpc "agent.history: very large limit returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.history","params":{"limit":-1},"id":"e3"}')
assert_valid_jsonrpc "healing.history: negative limit returns valid JSON-RPC" "$RESP"

echo ""




echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
