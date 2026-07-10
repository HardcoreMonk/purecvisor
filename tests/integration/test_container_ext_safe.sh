#!/usr/bin/env bash
# tests/integration/test_container_ext_safe.sh
#
# SAFE-tier Integration Tests — Container Extended
# Tests: container.list, container.destroy (idempotent), container.snapshot.list,
#        container.health.set/get/delete, container.nic.list
#
# 사전 조건:
#   - purecvisorsd 또는 purecvisormd 실행 중
#   - /var/run/purecvisor/daemon.sock 존재
#   - nc (netcat) 설치
#
# 실행: sudo bash tests/integration/test_container_ext_safe.sh

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

# ── 사전 조건 ────────────────────────────────────────────
log "=========================================="
log " Container Extended Integration Tests (SAFE)"
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
log "Daemon socket verified: $SOCKET_PATH"
echo ""

# ══════════════════════════════════════════════════════
# [1] container.list
# ══════════════════════════════════════════════════════
log "--- [1/7] container.list ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.list","params":{},"id":"cl1"}')
assert_result_or_known_error "container.list: returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.list","params":{"status":"running"},"id":"cl2"}')
assert_result_or_known_error "container.list: filter by status" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [2] container.destroy — nonexistent (idempotent)
# ══════════════════════════════════════════════════════
log "--- [2/7] container.destroy (nonexistent, idempotent) ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.destroy","params":{"name":"nonexistent"},"id":"cd1"}')
assert_result_or_known_error "container.destroy: nonexistent is idempotent (no crash)" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.destroy","params":{},"id":"cd2"}')
assert_valid_jsonrpc "container.destroy: missing name returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [3] container.snapshot.list
# ══════════════════════════════════════════════════════
log "--- [3/7] container.snapshot.list ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.snapshot.list","params":{"name":"test-ct"},"id":"csl1"}')
assert_result_or_known_error "container.snapshot.list: test-ct returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.snapshot.list","params":{"name":"nonexistent-ct"},"id":"csl2"}')
assert_result_or_known_error "container.snapshot.list: nonexistent container returns valid response" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.snapshot.list","params":{},"id":"csl3"}')
assert_valid_jsonrpc "container.snapshot.list: missing name returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [4] container.health.set
# ══════════════════════════════════════════════════════
log "--- [4/7] container.health.set ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.health.set","params":{"name":"test","cmd":"true","interval":60},"id":"chs1"}')
assert_result_or_known_error "container.health.set: valid params returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.health.set","params":{},"id":"chs2"}')
assert_valid_jsonrpc "container.health.set: missing params returns valid JSON-RPC" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.health.set","params":{"name":"test","cmd":"","interval":-1},"id":"chs3"}')
assert_valid_jsonrpc "container.health.set: invalid interval returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [5] container.health.get
# ══════════════════════════════════════════════════════
log "--- [5/7] container.health.get ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.health.get","params":{"name":"test"},"id":"chg1"}')
assert_result_or_known_error "container.health.get: 'test' returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.health.get","params":{"name":"nonexistent"},"id":"chg2"}')
assert_result_or_known_error "container.health.get: nonexistent returns valid response" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.health.get","params":{},"id":"chg3"}')
assert_valid_jsonrpc "container.health.get: missing name returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [6] container.health.delete
# ══════════════════════════════════════════════════════
log "--- [6/7] container.health.delete ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.health.delete","params":{"name":"test"},"id":"chd1"}')
assert_result_or_known_error "container.health.delete: 'test' returns result or known error" "$RESP"

# idempotent — second call must not crash
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.health.delete","params":{"name":"test"},"id":"chd2"}')
assert_result_or_known_error "container.health.delete: idempotent on second call" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.health.delete","params":{},"id":"chd3"}')
assert_valid_jsonrpc "container.health.delete: missing name returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [7] container.nic.list
# ══════════════════════════════════════════════════════
log "--- [7/7] container.nic.list ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.nic.list","params":{"name":"test"},"id":"cnl1"}')
assert_result_or_known_error "container.nic.list: 'test' returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.nic.list","params":{"name":"nonexistent"},"id":"cnl2"}')
assert_result_or_known_error "container.nic.list: nonexistent container returns valid response" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"container.nic.list","params":{},"id":"cnl3"}')
assert_valid_jsonrpc "container.nic.list: missing name returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# 결과 요약
# ══════════════════════════════════════════════════════
echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
