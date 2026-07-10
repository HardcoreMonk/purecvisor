#!/usr/bin/env bash
# =============================================================================
# 아키텍처 고도화 Phase 2 통합 테스트 — A(WebSocket) + E(스토리지 티어링)
# =============================================================================
set -uo pipefail
trap '' PIPE

SOCK="/var/run/purecvisor/daemon.sock"
REST="http://localhost:8080/api/v1"
PASS=0; FAIL=0; TOTAL=0
SSH_CMD="/usr/bin/ssh"
NODES=("192.0.2.19" "192.0.2.20" "192.0.2.21")
NODE_NAMES=("Node1" "Node2" "Node3")

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

detect_daemon_service() {
    if [ -n "${DAEMON_SERVICE:-}" ]; then
        echo "$DAEMON_SERVICE"
        return 0
    fi
    for svc in purecvisorsd purecvisormd; do
        pid=$(systemctl show -p MainPID "$svc" --value 2>/dev/null || true)
        if [ -n "$pid" ] && [ "$pid" != "0" ]; then
            echo "$svc"
            return 0
        fi
    done
    if [ "${EDITION:-multi}" = "single" ]; then
        echo "purecvisorsd"
    else
        echo "purecvisormd"
    fi
}

rpc() {
    local method="$1" params="${2-"{}"}"
    ( set +o pipefail
      echo "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":\"t$$\"}" \
          | nc -N -U "$SOCK" 2>/dev/null
    )
}

assert_contains() {
    local label="$1" haystack="$2" needle="$3"
    TOTAL=$((TOTAL + 1))
    if echo "$haystack" | grep -q "$needle" 2>/dev/null; then
        PASS=$((PASS + 1))
        echo -e "  ${GREEN}PASS${NC} $label"
    else
        FAIL=$((FAIL + 1))
        echo -e "  ${RED}FAIL${NC} $label — expected '$needle'"
        echo "       got: $(echo "$haystack" | head -c 200)"
    fi
}

assert_not_error() {
    local label="$1" resp="$2"
    TOTAL=$((TOTAL + 1))
    if echo "$resp" | grep -q '"error"' 2>/dev/null; then
        FAIL=$((FAIL + 1))
        echo -e "  ${RED}FAIL${NC} $label — unexpected error"
        echo "       got: $(echo "$resp" | head -c 200)"
    else
        PASS=$((PASS + 1))
        echo -e "  ${GREEN}PASS${NC} $label"
    fi
}

echo ""
echo "============================================================"
echo " Phase 2 A+E Integration Test"
echo "============================================================"
echo ""

# ══════════════════════════════════════════════════════════════
# A: WebSocket 백엔드 검증
# ══════════════════════════════════════════════════════════════
echo -e "${YELLOW}[1/8] A: WebSocket — 초기화 로그${NC}"
pid=$(systemctl show -p MainPID "$(detect_daemon_service)" --value)
init_log=$(journalctl _PID=$pid --no-pager 2>/dev/null | head -80)
assert_contains "ws_server init log" "$init_log" "WebSocket handler registered"

echo -e "${YELLOW}[2/8] A: WebSocket — HTTP Upgrade 경로 존재${NC}"
# WebSocket Upgrade는 HTTP GET + Upgrade 헤더가 필요
# curl로 일반 GET하면 400/404가 아닌 응답이면 핸들러가 등록된 것
ws_resp=$(curl -s -o /dev/null -w "%{http_code}" \
    -H "Connection: Upgrade" -H "Upgrade: websocket" \
    -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGVzdA==" \
    "http://localhost:8080/api/v1/ws/events" 2>/dev/null)
TOTAL=$((TOTAL + 1))
# 101=Switching Protocols 또는 200/400은 핸들러 존재 증거 (404=미등록)
if [ "$ws_resp" != "404" ] && [ "$ws_resp" != "000" ]; then
    PASS=$((PASS + 1))
    echo -e "  ${GREEN}PASS${NC} WebSocket endpoint responds (HTTP $ws_resp)"
else
    FAIL=$((FAIL + 1))
    echo -e "  ${RED}FAIL${NC} WebSocket endpoint not found (HTTP $ws_resp)"
fi

echo -e "${YELLOW}[3/8] A: WebSocket — 데몬 안정성 (WebSocket 후 RPC)${NC}"
rpc_after=$(rpc "vm.list")
assert_contains "RPC works after WS init" "$rpc_after" '"result"'

# ══════════════════════════════════════════════════════════════
# E: 스토리지 티어링 검증
# ══════════════════════════════════════════════════════════════
echo -e "${YELLOW}[4/8] E: Storage Tier — 초기화 로그${NC}"
assert_contains "storage_tier init log" "$init_log" "Storage tier initialized"

echo -e "${YELLOW}[5/8] E: Storage Tier — tier.list (기본 프리셋)${NC}"
# storage_tier.c의 티어 CRUD/QoS API는 호출부 0(도달불가)으로 감사에서
# 전면 삭제됨(pcv_storage_tier_init()만 보존) — 초기화 로그에서 기본 프리셋
# (ssd → pcvpool) 확인
assert_contains "default tier = ssd" "$init_log" "default: ssd"

echo -e "${YELLOW}[6/8] E: Storage Tier — ZFS 풀 사용량 조회${NC}"
# 기존 storage.pool.list RPC로 ZFS 풀 상태 확인
pool_resp=$(rpc "storage.pool.list")
assert_not_error "storage.pool.list works" "$pool_resp"
assert_contains "pcvpool exists" "$pool_resp" "pcvpool"

# ══════════════════════════════════════════════════════════════
# G: Prometheus 메트릭 — Phase 1 + Phase 2 통합 확인
# ══════════════════════════════════════════════════════════════
echo -e "${YELLOW}[7/8] G: Prometheus — 레지스트리 메트릭 누적 확인${NC}"
# 여러 RPC 실행 후 메트릭 카운터가 증가했는지 확인
rpc "dpdk.status" > /dev/null
rpc "sriov.status" > /dev/null
rpc "network.list" > /dev/null
sleep 1
metrics=$(curl -s "$REST/metrics" 2>/dev/null)
assert_contains "rpc_requests_total accumulated" "$metrics" "purecvisor_rpc_requests_total"

# ══════════════════════════════════════════════════════════════
# 3노드 일관성
# ══════════════════════════════════════════════════════════════
echo -e "${YELLOW}[8/8] 3-Node Consistency — A+E${NC}"
for i in 1 2; do
    ip="${NODES[$i]}"
    name="${NODE_NAMES[$i]}"
    # REST /health
    h=$($SSH_CMD pcvdev@"$ip" "curl -s http://localhost:8080/api/v1/health 2>/dev/null" 2>/dev/null)
    assert_contains "$name REST /health OK" "$h" '"status":"ok"'
    # UDS RPC
    r=$($SSH_CMD pcvdev@"$ip" 'echo "{\"jsonrpc\":\"2.0\",\"method\":\"vm.list\",\"params\":{},\"id\":\"1\"}" | sudo nc -N -U /var/run/purecvisor/daemon.sock 2>/dev/null' 2>/dev/null)
    assert_contains "$name UDS RPC OK" "$r" '"result"'
done

# ══════════════════════════════════════════════════════════════
echo ""
echo "============================================================"
echo -e " Results: ${GREEN}${PASS} passed${NC} / ${RED}${FAIL} failed${NC} / ${TOTAL} total"
echo "============================================================"
echo ""
if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
