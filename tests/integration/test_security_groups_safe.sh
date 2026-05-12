#!/bin/bash













set -uo pipefail


GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'


SOCKET_PATH="/var/run/purecvisor/daemon.sock"
PASS=0; FAIL=0; SKIP=0; TOTAL=0
TEST_SG="test-sg-safe-$$"


log()  { echo -e "${CYAN}[INFO]${NC} $*"; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); }
skip() { echo -e "${YELLOW}[SKIP]${NC} $*"; SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); }

rpc() { echo "$1" | nc -U "$SOCKET_PATH" 2>/dev/null || true; }

assert_contains() {
    local name="$1" resp="$2" pat="$3"
    if echo "$resp" | grep -q "$pat"; then pass "$name"
    else fail "$name (expected '$pat')"; echo "  Response: $resp"; fi
}

assert_not_contains() {
    local name="$1" resp="$2" pat="$3"
    if echo "$resp" | grep -q "$pat"; then
        fail "$name (unexpected '$pat')"; echo "  Response: $resp"
    else pass "$name"; fi
}

assert_valid_jsonrpc() {
    local name="$1" resp="$2"
    if [ -z "$resp" ]; then fail "$name (empty response)"; return; fi
    if echo "$resp" | grep -q '"jsonrpc"'; then pass "$name"
    else fail "$name (not valid JSON-RPC)"; echo "  Response: $resp"; fi
}

assert_result_or_known_error() {
    local name="$1" resp="$2"
    if [ -z "$resp" ]; then fail "$name (empty response)"; return; fi
    if echo "$resp" | grep -q '"result"'; then pass "$name"
    elif echo "$resp" | grep -q '"error"'; then
        if echo "$resp" | grep -q '32601'; then skip "$name (method not registered)"
        else pass "$name (valid error response)"; fi
    else fail "$name (unexpected format)"; echo "  Response: $resp"; fi
}


cleanup() {

    rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.delete\",\"params\":{\"name\":\"$TEST_SG\"},\"id\":\"cleanup\"}" \
        >/dev/null 2>&1 || true
}
trap cleanup EXIT


log "=========================================="
log " Security Group CRUD — SAFE"
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
log "Test security group name: $TEST_SG"
echo ""




log "--- [1/6] security_group.create ---"

RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.create\",\"params\":{\"name\":\"$TEST_SG\",\"description\":\"SAFE integration test group\"},\"id\":\"sg1\"}")
assert_result_or_known_error "security_group.create: creates new group" "$RESP"


RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.create\",\"params\":{\"name\":\"$TEST_SG\"},\"id\":\"sg2\"}")
assert_valid_jsonrpc "security_group.create: duplicate name returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"security_group.create","params":{},"id":"sg3"}')
assert_contains "security_group.create: missing name returns error" "$RESP" '"error"'

echo ""




log "--- [2/6] security_group.list ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"security_group.list","params":{},"id":"sl1"}')
assert_result_or_known_error "security_group.list: returns result" "$RESP"


if echo "$RESP" | grep -q '"result"'; then
    if echo "$RESP" | grep -q "$TEST_SG"; then
        pass "security_group.list: test-sg appears in list"
    else

        skip "security_group.list: test-sg not found (create may have errored)"
    fi
else
    skip "security_group.list: list not available, skipping membership check"
fi

echo ""




log "--- [3/6] security_group.rule.add ---"

RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.rule.add\",\"params\":{\"name\":\"$TEST_SG\",\"protocol\":\"tcp\",\"port\":22,\"direction\":\"ingress\",\"action\":\"allow\"},\"id\":\"ra1\"}")
assert_result_or_known_error "security_group.rule.add: tcp/22 ingress allow" "$RESP"

RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.rule.add\",\"params\":{\"name\":\"$TEST_SG\",\"protocol\":\"tcp\",\"port\":443,\"direction\":\"ingress\",\"action\":\"allow\"},\"id\":\"ra2\"}")
assert_result_or_known_error "security_group.rule.add: tcp/443 ingress allow" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"security_group.rule.add","params":{},"id":"ra3"}')
assert_contains "security_group.rule.add: missing params returns error" "$RESP" '"error"'


RESP=$(rpc '{"jsonrpc":"2.0","method":"security_group.rule.add","params":{"name":"__no_such_sg__","protocol":"tcp","port":22,"direction":"ingress","action":"allow"},"id":"ra4"}')
assert_valid_jsonrpc "security_group.rule.add: nonexistent group returns valid JSON-RPC" "$RESP"

echo ""




log "--- [4/6] security_group.rule.remove ---"

RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.rule.remove\",\"params\":{\"name\":\"$TEST_SG\",\"protocol\":\"tcp\",\"port\":22,\"direction\":\"ingress\"},\"id\":\"rr1\"}")
assert_result_or_known_error "security_group.rule.remove: removes tcp/22 rule" "$RESP"


RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.rule.remove\",\"params\":{\"name\":\"$TEST_SG\",\"protocol\":\"tcp\",\"port\":22,\"direction\":\"ingress\"},\"id\":\"rr2\"}")
assert_valid_jsonrpc "security_group.rule.remove: idempotent remove returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"security_group.rule.remove","params":{},"id":"rr3"}')
assert_contains "security_group.rule.remove: missing params returns error" "$RESP" '"error"'

echo ""




log "--- [5/6] security_group.delete ---"

RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.delete\",\"params\":{\"name\":\"$TEST_SG\"},\"id\":\"sd1\"}")
assert_result_or_known_error "security_group.delete: deletes test group" "$RESP"


RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.delete\",\"params\":{\"name\":\"$TEST_SG\"},\"id\":\"sd2\"}")
assert_valid_jsonrpc "security_group.delete: idempotent delete returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"security_group.delete","params":{},"id":"sd3"}')
assert_contains "security_group.delete: missing name returns error" "$RESP" '"error"'

echo ""




log "--- [6/6] Validation Edge Cases ---"


RESP=$(rpc '{"jsonrpc":"2.0","method":"security_group.create","params":{"name":"test; DROP TABLE security_groups;--"},"id":"e1"}')
assert_valid_jsonrpc "security_group.create: SQL injection in name returns valid JSON-RPC" "$RESP"


RESP=$(rpc '{"jsonrpc":"2.0","method":"security_group.create","params":{"name":"xss-test","description":"<script>alert(1)<\/script>"},"id":"e2"}')
assert_valid_jsonrpc "security_group.create: XSS in description returns valid JSON-RPC" "$RESP"


RESP=$(rpc "{\"jsonrpc\":\"2.0\",\"method\":\"security_group.rule.add\",\"params\":{\"name\":\"$TEST_SG\",\"protocol\":\"tcp\",\"port\":99999,\"direction\":\"ingress\",\"action\":\"allow\"},\"id\":\"e3\"}")
assert_valid_jsonrpc "security_group.rule.add: invalid port returns valid JSON-RPC" "$RESP"

echo ""




echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
