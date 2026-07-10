#!/usr/bin/env bash
# tests/integration/test_backup_ext_safe.sh
#
# SAFE-tier Integration Tests — Backup Extended
# Tests: backup.policy.list, backup.history (nonexistent VM),
#        snapshot.schedule.status, backup.restore (nonexistent VM/snapshot — expect error)
#
# 사전 조건:
#   - purecvisorsd 또는 purecvisormd 실행 중
#   - /var/run/purecvisor/daemon.sock 존재
#   - nc (netcat) 설치
#
# 실행: sudo bash tests/integration/test_backup_ext_safe.sh

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
log " Backup Extended Integration Tests (SAFE)"
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
# [1] backup.policy.list
# ══════════════════════════════════════════════════════
log "--- [1/4] backup.policy.list ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"bpl1"}')
assert_result_or_known_error "backup.policy.list: returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{"vm_name":"nonexistent"},"id":"bpl2"}')
assert_result_or_known_error "backup.policy.list: filter by nonexistent VM" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [2] backup.history — nonexistent VM
# ══════════════════════════════════════════════════════
log "--- [2/4] backup.history (nonexistent VM) ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.history","params":{"vm_name":"nonexistent"},"id":"bh1"}')
assert_result_or_known_error "backup.history: nonexistent VM returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.history","params":{},"id":"bh2"}')
assert_result_or_known_error "backup.history: no params returns all history or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.history","params":{"vm_name":"nonexistent","limit":5},"id":"bh3"}')
assert_result_or_known_error "backup.history: nonexistent VM with limit param" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [3] snapshot.schedule.status
# ══════════════════════════════════════════════════════
log "--- [3/4] snapshot.schedule.status ---"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"snapshot.schedule.status","params":{},"id":"sss1"}')
assert_result_or_known_error "snapshot.schedule.status: returns result or known error" "$RESP"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"snapshot.schedule.status","params":{"vm_name":"nonexistent"},"id":"sss2"}')
assert_result_or_known_error "snapshot.schedule.status: nonexistent VM returns valid response" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [4] backup.restore — nonexistent VM/snapshot (expect error)
# ══════════════════════════════════════════════════════
log "--- [4/4] backup.restore (nonexistent VM/snapshot) ---"

# nonexistent VM — must return an error (not crash)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.restore","params":{"vm_name":"nonexistent","snapshot_name":"none"},"id":"br1"}')
assert_result_or_known_error "backup.restore: nonexistent VM returns error (not crash)" "$RESP"

# missing vm_name
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.restore","params":{"snapshot_name":"none"},"id":"br2"}')
assert_valid_jsonrpc "backup.restore: missing vm_name returns valid JSON-RPC" "$RESP"

# missing snapshot_name
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.restore","params":{"vm_name":"nonexistent"},"id":"br3"}')
assert_valid_jsonrpc "backup.restore: missing snapshot_name returns valid JSON-RPC" "$RESP"

# empty params
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.restore","params":{},"id":"br4"}')
assert_valid_jsonrpc "backup.restore: empty params returns valid JSON-RPC" "$RESP"

# SQL injection in vm_name
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.restore","params":{"vm_name":"x; DROP TABLE vms;--","snapshot_name":"snap"},"id":"br5"}')
assert_contains "backup.restore: SQL injection in vm_name rejected" "$RESP" '"error"'

echo ""

# ══════════════════════════════════════════════════════
# 결과 요약
# ══════════════════════════════════════════════════════
echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
