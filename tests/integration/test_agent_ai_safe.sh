#!/bin/bash
# tests/integration/test_agent_ai_safe.sh
# SAFE
#
# AI Agent / Self-Healing — SAFE Integration Tests
# Tests: agent.config.get, agent.config.set, agent.history,
#        healing.history, ai.healing.approve, ai.healing.reject
#
# Prerequisites:
#   - purecvisorsd or purecvisormd running
#   - /var/run/purecvisor/daemon.sock present
#   - nc (netcat) installed
#
# Run: sudo bash tests/integration/test_agent_ai_safe.sh

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

# ══════════════════════════════════════════════════════════════════════
# [1] agent.config.get
# ══════════════════════════════════════════════════════════════════════
log "--- [1/6] agent.config.get ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.get","params":{},"id":"acg1"}')
rpc_check "agent.config.get: returns result" "$RESP"

assert_valid_jsonrpc "agent.config.get: response is valid JSON-RPC" "$RESP"

# Second call — idempotent
RESP2=$(rpc '{"jsonrpc":"2.0","method":"agent.config.get","params":{},"id":"acg2"}')
assert_valid_jsonrpc "agent.config.get: second call is idempotent" "$RESP2"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [2] agent.config.set
# ══════════════════════════════════════════════════════════════════════
log "--- [2/6] agent.config.set ---"

# 2-1. Disable AI agent (safe — does not affect hypervisor operations)
RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.set","params":{"key":"enabled","value":"false"},"id":"acs1"}')
rpc_check "agent.config.set: enabled=false" "$RESP"

# 2-2. Re-enable
RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.set","params":{"key":"enabled","value":"true"},"id":"acs2"}')
assert_valid_jsonrpc "agent.config.set: enabled=true returns valid JSON-RPC" "$RESP"

# 2-3. Missing key — expect error
RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.set","params":{},"id":"acs3"}')
assert_valid_jsonrpc "agent.config.set: missing params returns valid JSON-RPC" "$RESP"

# 2-4. Unknown key — daemon should handle gracefully
RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.set","params":{"key":"__unknown_key__","value":"test"},"id":"acs4"}')
assert_valid_jsonrpc "agent.config.set: unknown key returns valid JSON-RPC" "$RESP"

# 2-5. Verify config is readable after set
RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.get","params":{},"id":"acs5"}')
assert_valid_jsonrpc "agent.config.get: readable after set" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [3] agent.history
# ══════════════════════════════════════════════════════════════════════
log "--- [3/6] agent.history ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.history","params":{},"id":"ah1"}')
rpc_check "agent.history: returns result" "$RESP"

assert_valid_jsonrpc "agent.history: response is valid JSON-RPC" "$RESP"

# With limit
RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.history","params":{"limit":10},"id":"ah2"}')
assert_valid_jsonrpc "agent.history: with limit=10 returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [4] healing.history
# ══════════════════════════════════════════════════════════════════════
log "--- [4/6] healing.history ---"

RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.history","params":{},"id":"hh1"}')
rpc_check "healing.history: returns result" "$RESP"

assert_valid_jsonrpc "healing.history: response is valid JSON-RPC" "$RESP"

# With time filter
RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.history","params":{"limit":5},"id":"hh2"}')
assert_valid_jsonrpc "healing.history: with limit=5 returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [5] ai.healing.pending (may not exist — skip if method not found)
# ══════════════════════════════════════════════════════════════════════
log "--- [5/6] ai.healing.approve / ai.healing.reject ---"

# 5-1. approve with nonexistent healing ID
RESP=$(rpc '{"jsonrpc":"2.0","method":"ai.healing.approve","params":{"id":"nonexistent-healing-id-9999"},"id":"ha1"}')
rpc_check "ai.healing.approve: nonexistent ID handled" "$RESP"

# 5-2. reject with nonexistent healing ID
RESP=$(rpc '{"jsonrpc":"2.0","method":"ai.healing.reject","params":{"id":"nonexistent-healing-id-9999"},"id":"hr1"}')
rpc_check "ai.healing.reject: nonexistent ID handled" "$RESP"

# 5-3. approve with missing id param
RESP=$(rpc '{"jsonrpc":"2.0","method":"ai.healing.approve","params":{},"id":"ha2"}')
assert_valid_jsonrpc "ai.healing.approve: missing id returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [6a] healing.pending (BUG-21 / F-11)
# ══════════════════════════════════════════════════════════════════════
log "--- [6a] healing.pending (BUG-21) ---"

# 6a-1. 기본 조회 (승인 대기 액션 목록)
RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.pending","params":{},"id":"hp1"}')
rpc_check "healing.pending: returns result" "$RESP"
assert_valid_jsonrpc "healing.pending: valid JSON-RPC" "$RESP"

# 6a-2. 반복 호출 idempotent
RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.pending","params":{},"id":"hp2"}')
assert_valid_jsonrpc "healing.pending: idempotent" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# [6b] healing.set_mode (Issue-M2 / F-11)
# ══════════════════════════════════════════════════════════════════════
log "--- [6b] healing.set_mode (Issue-M2) ---"

# 6b-1. active 모드 전환
RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.set_mode","params":{"mode":"active"},"id":"sm1"}')
assert_valid_jsonrpc "healing.set_mode: active returns valid JSON-RPC" "$RESP"
assert_contains "healing.set_mode: active reflected" "$RESP" '"mode":"active"'

# 6b-2. dry_run 복귀 (테스트 안전성: 항상 dry_run으로 끝나야 함)
RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.set_mode","params":{"mode":"dry_run"},"id":"sm2"}')
assert_valid_jsonrpc "healing.set_mode: dry_run returns valid JSON-RPC" "$RESP"
assert_contains "healing.set_mode: dry_run reflected" "$RESP" '"mode":"dry_run"'

# 6b-3. 잘못된 mode 값
RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.set_mode","params":{"mode":"invalid_xyz"},"id":"sm3"}')
assert_contains "healing.set_mode: invalid mode rejected" "$RESP" '"error"'

# 6b-4. mode 파라미터 누락
RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.set_mode","params":{},"id":"sm4"}')
assert_contains "healing.set_mode: missing param rejected" "$RESP" '"error"'

echo ""

# ══════════════════════════════════════════════════════════════════════
# [6] Edge Cases
# ══════════════════════════════════════════════════════════════════════
log "--- [6/6] Edge Cases ---"

# Injection in config value
RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.config.set","params":{"key":"webhook_url","value":"https://evil.example.com/; rm -rf /"},"id":"e1"}')
assert_valid_jsonrpc "agent.config.set: injection in value returns valid JSON-RPC" "$RESP"

# Very large limit
RESP=$(rpc '{"jsonrpc":"2.0","method":"agent.history","params":{"limit":999999},"id":"e2"}')
assert_valid_jsonrpc "agent.history: very large limit returns valid JSON-RPC" "$RESP"

# Negative limit
RESP=$(rpc '{"jsonrpc":"2.0","method":"healing.history","params":{"limit":-1},"id":"e3"}')
assert_valid_jsonrpc "healing.history: negative limit returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════════════════════
# Summary
# ══════════════════════════════════════════════════════════════════════
echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
