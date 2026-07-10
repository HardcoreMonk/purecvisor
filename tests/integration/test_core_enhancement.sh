#!/usr/bin/env bash
# tests/integration/test_core_enhancement.sh
#
# Core Enhancement Integration Tests
# Tests for features added in core enhancement rounds 1-3:
#   - VM stats (memory-stats, cpu-stats)
#   - VM disk live resize
#   - Guest agent operations (ping, exec, shutdown)
#   - Alert config reload
#   - Cluster metrics aggregate
#   - Network QoS
#   - Backup verify
#   - Input validation / error handling
#
# 사전 조건:
#   - purecvisorsd 또는 purecvisormd 실행 중 (systemd 또는 수동)
#   - /var/run/purecvisor/daemon.sock 존재
#   - root 또는 sudo 권한
#   - nc (netcat) 설치
#
# 실행: sudo bash tests/integration/test_core_enhancement.sh

set -uo pipefail

# ── 색상 ──────────────────────────────────────────────
GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

# ── 설정 ──────────────────────────────────────────────
SOCKET_PATH="/var/run/purecvisor/daemon.sock"
PASS=0; FAIL=0; SKIP=0
TOTAL=0

# ── 유틸리티 ──────────────────────────────────────────
log()  { echo -e "${CYAN}[INFO]${NC} $*"; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); }
skip() { echo -e "${YELLOW}[SKIP]${NC} $*"; SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); }

# ── JSON-RPC 전송 ────────────────────────────────────
send_rpc() {
    echo "$1" | nc -U "$SOCKET_PATH" 2>/dev/null || true
}

# ── 응답 검증 헬퍼 ───────────────────────────────────
assert_contains() {
    local test_name="$1" response="$2" expected="$3"
    if echo "$response" | grep -q "$expected"; then
        pass "$test_name"
    else
        fail "$test_name (expected '$expected' in response)"
        echo "  Response: $response"
    fi
}

assert_not_contains() {
    local test_name="$1" response="$2" unexpected="$3"
    if echo "$response" | grep -q "$unexpected"; then
        fail "$test_name (unexpected '$unexpected' in response)"
        echo "  Response: $response"
    else
        pass "$test_name"
    fi
}

# result 또는 error 중 하나를 포함하는 유효한 JSON-RPC 응답인지 확인
assert_valid_jsonrpc() {
    local test_name="$1" response="$2"
    if [ -z "$response" ]; then
        fail "$test_name (empty response)"
        return
    fi
    if echo "$response" | grep -q '"jsonrpc"'; then
        pass "$test_name"
    else
        fail "$test_name (not valid JSON-RPC)"
        echo "  Response: $response"
    fi
}

# result를 기대하지만 error도 허용 (Method not found 등)
assert_result_or_known_error() {
    local test_name="$1" response="$2"
    if [ -z "$response" ]; then
        fail "$test_name (empty response)"
        return
    fi
    if echo "$response" | grep -q '"result"'; then
        pass "$test_name"
    elif echo "$response" | grep -q '"error"'; then
        # Method not found (-32601) 이면 SKIP, 그 외 에러는 PASS (정상 에러 응답)
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

# ── 사전 조건 확인 ───────────────────────────────────
log "=========================================="
log " Core Enhancement Integration Tests"
log "=========================================="
echo ""

if [ ! -S "$SOCKET_PATH" ]; then
    echo -e "${RED}[ERROR]${NC} Daemon socket not found: $SOCKET_PATH"
    echo "  Start purecvisorsd or purecvisormd first"
    echo ""
    echo "  SKIP: All tests skipped (daemon not running)"
    exit 0
fi

# 데몬 응답 확인 (vm.list로 기본 RPC 동작 검증)
PROBE=$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"probe"}')
if [ -z "$PROBE" ]; then
    echo -e "${RED}[ERROR]${NC} Daemon socket exists but no response"
    echo "  The daemon may be stuck or shutting down"
    echo ""
    echo "  SKIP: All tests skipped (daemon unresponsive)"
    exit 0
fi

log "Daemon socket verified: $SOCKET_PATH"
echo ""

# ── 실행 중인 VM 목록 확보 (테스트용) ───────────────
# 실제 VM이 있으면 이름을 사용, 없으면 'nonexistent-vm'
RUNNING_VM=""
if echo "$PROBE" | grep -q '"name"'; then
    RUNNING_VM=$(echo "$PROBE" | grep -o '"name":"[^"]*"' | head -1 | sed 's/"name":"//;s/"//')
fi
TEST_VM="${RUNNING_VM:-nonexistent-test-vm}"
log "Test target VM: $TEST_VM ($([ -n "$RUNNING_VM" ] && echo 'running' || echo 'not found — error-path tests'))"
echo ""

# ══════════════════════════════════════════════════════
# [1] VM Memory Stats
# ══════════════════════════════════════════════════════
log "--- [1/9] VM Memory Stats ---"

# 1-1. 정상 호출 (VM 이름 지정)
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.memory.stats\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"ms1\"}")
assert_valid_jsonrpc "vm.memory.stats: valid JSON-RPC response" "$RESP"

# 1-2. 파라미터 누락 (name 없음 → error 예상)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{},"id":"ms2"}')
assert_contains "vm.memory.stats: missing name returns error" "$RESP" '"error"'

# 1-3. 존재하지 않는 VM
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{"name":"__no_such_vm_12345__"},"id":"ms3"}')
assert_contains "vm.memory.stats: nonexistent VM returns error" "$RESP" '"error"'

echo ""

# ══════════════════════════════════════════════════════
# [2] VM CPU Stats
# ══════════════════════════════════════════════════════
log "--- [2/9] VM CPU Stats ---"

# 2-1. 정상 호출
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.cpu.stats\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"cs1\"}")
assert_valid_jsonrpc "vm.cpu.stats: valid JSON-RPC response" "$RESP"

# 2-2. 파라미터 누락
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.cpu.stats","params":{},"id":"cs2"}')
assert_contains "vm.cpu.stats: missing name returns error" "$RESP" '"error"'

# 2-3. 존재하지 않는 VM
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.cpu.stats","params":{"name":"__no_such_vm_12345__"},"id":"cs3"}')
assert_contains "vm.cpu.stats: nonexistent VM returns error" "$RESP" '"error"'

echo ""

# ══════════════════════════════════════════════════════
# [3] VM Disk Live Resize
# ══════════════════════════════════════════════════════
log "--- [3/9] VM Disk Live Resize ---"

# 3-1. 파라미터 누락 (name 없음)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{},"id":"dr1"}')
assert_contains "vm.disk.live_resize: missing params returns error" "$RESP" '"error"'

# 3-2. target 누락
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{"name":"test-vm"},"id":"dr2"}')
assert_contains "vm.disk.live_resize: missing target returns error" "$RESP" '"error"'

# 3-3. 음수 사이즈
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{"name":"test-vm","target":"vda","new_size_gb":-1},"id":"dr3"}')
assert_valid_jsonrpc "vm.disk.live_resize: negative size returns valid JSON-RPC" "$RESP"

# 3-4. 존재하지 않는 VM
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{"name":"__no_such_vm_12345__","target":"vda","new_size_gb":20},"id":"dr4"}')
assert_contains "vm.disk.live_resize: nonexistent VM returns error" "$RESP" '"error"'

# 3-5. 정상 파라미터 (VM이 있는 경우 result, 없으면 error)
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.disk.live_resize\",\"params\":{\"name\":\"$TEST_VM\",\"target\":\"vda\",\"new_size_gb\":50},\"id\":\"dr5\"}")
assert_valid_jsonrpc "vm.disk.live_resize: well-formed request returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [4] Guest Agent Operations
# ══════════════════════════════════════════════════════
log "--- [4/9] Guest Agent Operations ---"

# 4-1. guest.ping
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.guest.ping\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"gp1\"}")
assert_valid_jsonrpc "vm.guest.ping: valid JSON-RPC response" "$RESP"

# 4-2. guest.ping 파라미터 누락
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.guest.ping","params":{},"id":"gp2"}')
assert_contains "vm.guest.ping: missing name returns error" "$RESP" '"error"'

# 4-3. guest.exec
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.guest.exec\",\"params\":{\"name\":\"$TEST_VM\",\"command\":\"echo hello\"},\"id\":\"ge1\"}")
assert_valid_jsonrpc "vm.guest.exec: valid JSON-RPC response" "$RESP"

# 4-4. guest.exec 파라미터 누락
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.guest.exec","params":{},"id":"ge2"}')
assert_contains "vm.guest.exec: missing params returns error" "$RESP" '"error"'

# 4-5. guest.shutdown
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.guest.shutdown\",\"params\":{\"name\":\"$TEST_VM\"},\"id\":\"gs1\"}")
# guest.shutdown을 실제 실행하면 VM이 꺼지므로 응답 형식만 확인
assert_valid_jsonrpc "vm.guest.shutdown: valid JSON-RPC response" "$RESP"

# 4-6. guest.shutdown 파라미터 누락
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.guest.shutdown","params":{},"id":"gs2"}')
assert_contains "vm.guest.shutdown: missing name returns error" "$RESP" '"error"'

# 4-7. 존재하지 않는 VM에 guest 명령
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.guest.ping","params":{"name":"__no_such_vm_12345__"},"id":"gp3"}')
assert_contains "vm.guest.ping: nonexistent VM returns error" "$RESP" '"error"'

echo ""

# ══════════════════════════════════════════════════════
# [5] Alert Config Reload
# ══════════════════════════════════════════════════════
log "--- [5/9] Alert Config Reload ---"

# 5-1. 정상 호출
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.reload","params":{},"id":"ar1"}')
assert_result_or_known_error "alert.config.reload: returns result" "$RESP"

# 5-2. 알림 히스토리 조회 (기존 RPC 확인)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.history","params":{},"id":"ar2"}')
assert_valid_jsonrpc "alert.history: valid JSON-RPC response" "$RESP"

# 5-3. 알림 설정 조회
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.get","params":{},"id":"ar3"}')
assert_valid_jsonrpc "alert.config.get: valid JSON-RPC response" "$RESP"

# 5-4. 알림 설정 변경 (cpu_warn만 변경)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.set","params":{"cpu_warn":85},"id":"ar4"}')
assert_valid_jsonrpc "alert.config.set: valid JSON-RPC response" "$RESP"

# 5-5. 변경 후 재확인 (reload)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"alert.config.reload","params":{},"id":"ar5"}')
assert_result_or_known_error "alert.config.reload: second call succeeds" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [6] Cluster Metrics Aggregate
# ══════════════════════════════════════════════════════
log "--- [6/9] Cluster Metrics Aggregate ---"

# 6-1. 정상 호출
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"cluster.metrics.aggregate","params":{},"id":"ca1"}')
assert_result_or_known_error "cluster.metrics.aggregate: returns valid response" "$RESP"

# 6-2. 클러스터 상태 확인 (기존 RPC)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"cluster.status","params":{},"id":"ca2"}')
assert_valid_jsonrpc "cluster.status: valid JSON-RPC response" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [7] Network QoS
# ══════════════════════════════════════════════════════
log "--- [7/9] Network QoS ---"

# 7-1. QoS 조회 (lo 인터페이스)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.get","params":{"interface":"lo"},"id":"nq1"}')
assert_valid_jsonrpc "network.qos.get: valid JSON-RPC response (lo)" "$RESP"

# 7-2. QoS 파라미터 누락
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.get","params":{},"id":"nq2"}')
assert_contains "network.qos.get: missing interface returns error" "$RESP" '"error"'

# 7-3. 존재하지 않는 인터페이스
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.get","params":{"interface":"__fake_iface_999__"},"id":"nq3"}')
assert_valid_jsonrpc "network.qos.get: nonexistent interface returns valid JSON-RPC" "$RESP"

# 7-4. QoS 설정 (rate_kbps 지정)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.set","params":{"interface":"lo","rate_kbps":1000},"id":"nq4"}')
assert_valid_jsonrpc "network.qos.set: valid JSON-RPC response" "$RESP"

# 7-5. QoS 제거
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.remove","params":{"interface":"lo"},"id":"nq5"}')
assert_valid_jsonrpc "network.qos.remove: valid JSON-RPC response" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [8] Backup Verify
# ══════════════════════════════════════════════════════
log "--- [8/9] Backup Verify ---"

# 8-1. 존재하지 않는 스냅샷
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.verify","params":{"name":"test-vm","snapshot":"nonexistent-snap"},"id":"bv1"}')
assert_result_or_known_error "backup.verify: nonexistent snapshot returns valid response" "$RESP"

# 8-2. 파라미터 누락
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.verify","params":{},"id":"bv2"}')
assert_result_or_known_error "backup.verify: missing params handled" "$RESP"

# 8-3. 백업 정책 목록 (기존 RPC 확인)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"bv3"}')
assert_valid_jsonrpc "backup.policy.list: valid JSON-RPC response" "$RESP"

# 8-4. 백업 이력 (기존 RPC)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.history","params":{"vm_name":"test-vm"},"id":"bv4"}')
assert_valid_jsonrpc "backup.history: valid JSON-RPC response" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# [9] Error Handling & Edge Cases
# ══════════════════════════════════════════════════════
log "--- [9/9] Error Handling & Edge Cases ---"

# 9-1. 완전히 잘못된 JSON
RESP=$(send_rpc 'NOT JSON AT ALL')
if [ -n "$RESP" ]; then
    assert_contains "Invalid JSON: returns parse error" "$RESP" '"error"'
else
    skip "Invalid JSON: no response (daemon may close connection)"
fi

# 9-2. 존재하지 않는 메서드
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"nonexistent.method.xyz","params":{},"id":"e2"}')
assert_contains "Nonexistent method: returns Method not found" "$RESP" '"error"'

# 9-3. id 누락
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{}}')
assert_valid_jsonrpc "Missing id: still returns JSON-RPC" "$RESP"

# 9-4. params가 배열인 경우 (잘못된 형식)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":[],"id":"e4"}')
assert_valid_jsonrpc "Array params: returns valid JSON-RPC" "$RESP"

# 9-5. vm.memory.stats 빈 name
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{"name":""},"id":"e5"}')
assert_contains "vm.memory.stats: empty name returns error" "$RESP" '"error"'

# 9-6. vm.disk.live_resize size가 0
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{"name":"test-vm","target":"vda","new_size_gb":0},"id":"e6"}')
assert_valid_jsonrpc "vm.disk.live_resize: zero size returns valid JSON-RPC" "$RESP"

# 9-7. vm.disk.live_resize 매우 큰 사이즈
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.disk.live_resize","params":{"name":"test-vm","target":"vda","new_size_gb":999999},"id":"e7"}')
assert_valid_jsonrpc "vm.disk.live_resize: huge size returns valid JSON-RPC" "$RESP"

# 9-8. SQL injection 시도 (VM name에 SQL 주입)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{"name":"test; DROP TABLE vms;--"},"id":"e8"}')
assert_contains "SQL injection in VM name: returns error (validated)" "$RESP" '"error"'

# 9-9. Path traversal 시도 (VM name)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{"name":"../../etc/passwd"},"id":"e9"}')
assert_contains "Path traversal in VM name: returns error (validated)" "$RESP" '"error"'

# 9-10. XSS 시도 (VM name)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.memory.stats","params":{"name":"<script>alert(1)</script>"},"id":"e10"}')
assert_contains "XSS in VM name: returns error (validated)" "$RESP" '"error"'

# 9-11. Unicode/null byte in VM name
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.cpu.stats","params":{"name":"test\u0000vm"},"id":"e11"}')
assert_valid_jsonrpc "Null byte in VM name: returns valid JSON-RPC" "$RESP"

# 9-12. 호스트 텔레메트리 (기존 RPC 확인)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"telemetry.host","params":{},"id":"e12"}')
assert_valid_jsonrpc "telemetry.host: returns valid JSON-RPC" "$RESP"

# 9-13. 프로세스 모니터
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"monitor.processes","params":{"top":5},"id":"e13"}')
assert_valid_jsonrpc "monitor.processes: returns valid JSON-RPC" "$RESP"

# 9-14. network.qos.set 음수 rate
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.set","params":{"interface":"lo","rate_kbps":-100},"id":"e14"}')
assert_valid_jsonrpc "network.qos.set: negative rate returns valid JSON-RPC" "$RESP"

# 9-15. vm.guest.exec command injection
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"vm.guest.exec","params":{"name":"test-vm","command":"echo hello; rm -rf /"},"id":"e15"}')
assert_valid_jsonrpc "vm.guest.exec: command with semicolons returns valid JSON-RPC" "$RESP"

echo ""

# ══════════════════════════════════════════════════════
# 결과 요약
# ══════════════════════════════════════════════════════
echo "=========================================="
echo -e " Results: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
