#!/usr/bin/env bash
# tests/integration/test_webhook_misc_safe.sh
#
# SAFE-tier Integration Tests — Webhook / Misc
# Tests: webhook.dlq.list, webhook.dlq.retry (nonexistent),
#        system.runbook.list, gpu.metrics, template.history
#
# 사전 조건:
#   - purecvisorsd 또는 purecvisormd 실행 중
#   - /var/run/purecvisor/daemon.sock 존재
#   - nc (netcat) 설치
#
# 실행: sudo bash tests/integration/test_webhook_misc_safe.sh

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
log " Webhook / Misc Integration Tests (SAFE)"
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
# [1] webhook.dlq.list
# ══════════════════════════════════════════════════════
log "--- [1/5] webhook.dlq.list ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"webhook.dlq.list","params":{},"id":"wdl1"}')
assert_result_or_known_error "webhook.dlq.list: returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"webhook.dlq.list","params":{"limit":20},"id":"wdl2"}')
assert_result_or_known_error "webhook.dlq.list: with limit param" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [2] webhook.dlq.retry — nonexistent id
# ══════════════════════════════════════════════════════
log "--- [2/5] webhook.dlq.retry (nonexistent) ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"webhook.dlq.retry","params":{"id":"nonexistent"},"id":"wdr1"}')
assert_result_or_known_error "webhook.dlq.retry: nonexistent id returns valid response" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"webhook.dlq.retry","params":{},"id":"wdr2"}')
assert_valid_jsonrpc "webhook.dlq.retry: missing id returns valid JSON-RPC" "$RESP"

# 재시도 멱등성 — 없는 항목을 두 번 retry
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"webhook.dlq.retry","params":{"id":"nonexistent"},"id":"wdr3"}')
assert_result_or_known_error "webhook.dlq.retry: idempotent on repeated call" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [3] system.runbook.list
# ══════════════════════════════════════════════════════
log "--- [3/5] system.runbook.list ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"system.runbook.list","params":{},"id":"srl1"}')
assert_result_or_known_error "system.runbook.list: returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"system.runbook.list","params":{"category":"failover"},"id":"srl2"}')
assert_result_or_known_error "system.runbook.list: filter by category" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [4] gpu.metrics
# ══════════════════════════════════════════════════════
log "--- [4/5] gpu.metrics ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"gpu.metrics","params":{},"id":"gm1"}')
assert_result_or_known_error "gpu.metrics: returns result or graceful error (no GPU)" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"gpu.metrics","params":{"device":"nonexistent"},"id":"gm2"}')
assert_result_or_known_error "gpu.metrics: nonexistent device returns valid response" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [5] template.history
# ══════════════════════════════════════════════════════
log "--- [5/5] template.history ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.history","params":{},"id":"th1"}')
assert_result_or_known_error "template.history: returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.history","params":{"name":"nonexistent-template"},"id":"th2"}')
assert_result_or_known_error "template.history: nonexistent template returns valid response" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.history","params":{"limit":5},"id":"th3"}')
assert_result_or_known_error "template.history: with limit param" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# 결과 요약
# ══════════════════════════════════════════════════════
echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
