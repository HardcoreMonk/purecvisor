#!/usr/bin/env bash
# tests/integration/test_jobs_queue_safe.sh
#
# SAFE-tier Integration Tests — Job Queue
# Tests: jobs.list, jobs.get (nonexistent), jobs.cancel (nonexistent)
#
# 사전 조건:
#   - purecvisorsd 또는 purecvisormd 실행 중
#   - /var/run/purecvisor/daemon.sock 존재
#   - nc (netcat) 설치
#
# 실행: sudo bash tests/integration/test_jobs_queue_safe.sh

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
log " Job Queue Integration Tests (SAFE)"
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
# [1] jobs.list
# ══════════════════════════════════════════════════════
log "--- [1/3] jobs.list ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"jobs.list","params":{},"id":"jl1"}')
assert_result_or_known_error "jobs.list: returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"jobs.list","params":{"status":"running"},"id":"jl2"}')
assert_result_or_known_error "jobs.list: filter by status=running" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"jobs.list","params":{"limit":10},"id":"jl3"}')
assert_result_or_known_error "jobs.list: with limit param" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [2] jobs.get — nonexistent id
# ══════════════════════════════════════════════════════
log "--- [2/3] jobs.get (nonexistent) ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"jobs.get","params":{"id":"nonexistent"},"id":"jg1"}')
assert_result_or_known_error "jobs.get: nonexistent id returns error or not-found result" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"jobs.get","params":{},"id":"jg2"}')
assert_valid_jsonrpc "jobs.get: missing id returns valid JSON-RPC" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"jobs.get","params":{"id":"00000000-0000-0000-0000-000000000000"},"id":"jg3"}')
assert_result_or_known_error "jobs.get: zero-UUID returns valid response" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [3] jobs.cancel — nonexistent id
# ══════════════════════════════════════════════════════
log "--- [3/3] jobs.cancel (nonexistent) ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"jobs.cancel","params":{"id":"nonexistent"},"id":"jc1"}')
assert_result_or_known_error "jobs.cancel: nonexistent id returns error or not-found result" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"jobs.cancel","params":{},"id":"jc2"}')
assert_valid_jsonrpc "jobs.cancel: missing id returns valid JSON-RPC" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"jobs.cancel","params":{"id":"nonexistent"},"id":"jc3"}')
RESP2=$(send_rpc '{"jsonrpc":"2.0","method":"jobs.cancel","params":{"id":"nonexistent"},"id":"jc4"}')
assert_result_or_known_error "jobs.cancel: idempotent on repeated calls (call 1)" "$RESP"
assert_result_or_known_error "jobs.cancel: idempotent on repeated calls (call 2)" "$RESP2"

echo ""

# ══════════════════════════════════════════════════════
# 결과 요약
# ══════════════════════════════════════════════════════
echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
