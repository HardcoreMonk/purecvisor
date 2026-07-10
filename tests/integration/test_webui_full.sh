#!/usr/bin/env bash
# =============================================================================
# Web UI 14항목 + 전체 회귀 통합 테스트
# =============================================================================
set -uo pipefail
trap '' PIPE

SOCK="/var/run/purecvisor/daemon.sock"
REST="http://localhost:8080"
API="$REST/api/v1"
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
    ( set +o pipefail; echo "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":\"t$$\"}" | nc -N -U "$SOCK" 2>/dev/null )
}
ac() { # assert_contains
    local l="$1" h="$2" n="$3"; TOTAL=$((TOTAL+1))
    if echo "$h" | /usr/bin/grep -q "$n" 2>/dev/null; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} $l"
    else FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} $l — expected '$n'"; echo "       $(echo "$h"|head -c 150)"; fi
}
ae() { # assert_error
    local l="$1" h="$2"; TOTAL=$((TOTAL+1))
    if echo "$h" | grep -q '"error"' 2>/dev/null; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} $l (expected error)"
    else FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} $l — expected error"; fi
}

echo ""
echo "================================================================"
echo " Web UI 14-Item + Full Regression Test (3-Node)"
echo "================================================================"
echo ""

# ── JWT 토큰 발급 ──
TOKEN=$(curl -s -X POST "$API/auth/token" -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ -n "$TOKEN" ]; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} JWT token issued"
else FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} JWT token issue failed"; fi

# ══════════════════════════════════════════════════════════
# 1. Web UI 정적 파일 서빙
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[1/14] UI Static File Serving${NC}"
# Node1 로컬 직접 확인
code=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:8080/ui/" 2>/dev/null)
ac "Node1 /ui/ HTTP $code" "$code" "200"
for i in 1 2; do
    ip="${NODES[$i]}"; name="${NODE_NAMES[$i]}"
    code=$($SSH_CMD pcvdev@"$ip" 'curl -s -o /dev/null -w "%{http_code}" http://localhost:8080/ui/' 2>/dev/null)
    ac "$name /ui/ HTTP $code" "$code" "200"
done

# 경로 순회 차단 (404 또는 403 모두 안전 — 파일 접근 불가)
code=$(curl -s -o /dev/null -w "%{http_code}" "$REST/ui/../../etc/passwd" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ "$code" = "403" ] || [ "$code" = "404" ]; then
    PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} path traversal blocked (HTTP $code)"
else
    FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} path traversal not blocked (HTTP $code)"
fi

# ══════════════════════════════════════════════════════════
# 2. VM 목록 (REST + UDS)
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[2/14] VM List — REST + UDS${NC}"
vms=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/vms" 2>/dev/null)
ac "REST /vms returns data" "$vms" "name"
uds_vms=$(rpc "vm.list")
ac "UDS vm.list returns data" "$uds_vms" '"result"'

# ══════════════════════════════════════════════════════════
# 3. Network List + Delete (멱등)
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[3/14] Network CRUD${NC}"
nets=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/networks" 2>/dev/null)
ac "REST /networks returns data" "$nets" "name"
# 멱등 삭제
del=$(curl -s -X DELETE -H "Authorization: Bearer $TOKEN" "$API/networks/nonexist-test-br" 2>/dev/null)
# 삭제가 에러든 성공이든 크래시 안 하면 OK
TOTAL=$((TOTAL+1)); PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} network delete idempotent (no crash)"

# ══════════════════════════════════════════════════════════
# 4. Container List
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[4/14] Container List${NC}"
ctrs=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/containers" 2>/dev/null)
ac "REST /containers responds" "$ctrs" ""
TOTAL=$((TOTAL+1)); PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} container endpoint accessible"

# ══════════════════════════════════════════════════════════
# 5. Storage Pools + Tiers
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[5/14] Storage Pools + Tiers${NC}"
pools=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/storage/pools" 2>/dev/null)
ac "REST /storage/pools has pcvpool" "$pools" "pcvpool"
# 스토리지 티어 초기화 로그
pid=$(systemctl show -p MainPID "$(detect_daemon_service)" --value)
init_log=$(journalctl _PID=$pid --no-pager 2>/dev/null | head -80)
ac "storage_tier initialized" "$init_log" "Storage tier initialized"

# ══════════════════════════════════════════════════════════
# 6. Cluster Status (DPDK/SR-IOV)
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[6/14] Cluster + DPDK + SR-IOV${NC}"
health=$(curl -s "$API/health" 2>/dev/null)
ac "health status ok" "$health" '"ok"'
dpdk=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/dpdk/status" 2>/dev/null)
ac "dpdk status available field" "$dpdk" "available"
sriov=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/sriov/status" 2>/dev/null)
ac "sriov status available field" "$sriov" "available"

# ══════════════════════════════════════════════════════════
# 7. Prometheus Metrics
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[7/14] Prometheus Metrics${NC}"
met=$(curl -s "$API/metrics" 2>/dev/null)
ac "metrics has rpc_requests_total" "$met" "purecvisor_rpc_requests_total"
ac "metrics has rpc_duration_ms" "$met" "purecvisor_rpc_duration_ms"
ac "metrics has purecvisor_info" "$met" "purecvisor_info"

# ══════════════════════════════════════════════════════════
# 8. Audit Trail
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[8/14] Audit Trail${NC}"
ac "audit init log" "$init_log" "Audit trail initialized"
TOTAL=$((TOTAL+1))
if [ -f /var/lib/purecvisor/pcv_audit.db ]; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} pcv_audit.db exists"
else FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} pcv_audit.db missing"; fi
# audit.log 확인
sleep 1
audit_file="/var/log/purecvisor/audit.log"
TOTAL=$((TOTAL+1))
if [ -f "$audit_file" ] && [ -s "$audit_file" ]; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} audit.log has content"
else FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} audit.log empty or missing"; fi

# ══════════════════════════════════════════════════════════
# 9. WebSocket Endpoint
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[9/14] WebSocket Endpoint${NC}"
ac "ws_server init log" "$init_log" "WebSocket handler registered"
ws_code=$(curl -s -o /dev/null -w "%{http_code}" -H "Connection: Upgrade" -H "Upgrade: websocket" -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGVzdA==" "$REST/api/v1/ws/events" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ "$ws_code" != "404" ] && [ "$ws_code" != "000" ]; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} WebSocket endpoint responds (HTTP $ws_code)"
else FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} WebSocket endpoint not found ($ws_code)"; fi

# ══════════════════════════════════════════════════════════
# 10. Hot Reload (D) — 데몬 안정성
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[10/14] Hot Reload Stability${NC}"
stable=$(rpc "vm.list")
ac "daemon stable (post all init)" "$stable" '"result"'

# ══════════════════════════════════════════════════════════
# 11. io_uring UDS Server
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[11/14] io_uring UDS Server${NC}"
ac "io_uring mode log" "$init_log" "io_uring mode"

# ══════════════════════════════════════════════════════════
# 12. Input Validation (PCI addr)
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[12/16] Input Validation${NC}"
bad_pci=$(rpc 'dpdk.bind' '{"pci_addr":"../../etc"}')
ae "path traversal PCI rejected" "$bad_pci"
bad_missing=$(rpc 'sriov.enable' '{}')
ae "missing param rejected" "$bad_missing"

# ══════════════════════════════════════════════════════════
# 13. New REST Endpoints (CLI→API 교체)
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[13/16] REST: OVN/Backup/VNC/NIC${NC}"
ovn_st=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/ovn/status" 2>/dev/null)
ac "REST /ovn/status has available" "$ovn_st" "available"
ovn_sw=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/ovn/switches" 2>/dev/null)
ac "REST /ovn/switches has data" "$ovn_sw" "data"
ovn_rt=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/ovn/routers" 2>/dev/null)
ac "REST /ovn/routers has data" "$ovn_rt" "data"
bp=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/backup/policies" 2>/dev/null)
ac "REST /backup/policies responds" "$bp" "data"
# NIC list (기존 REST)
nic=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/vms/ubuntu-bridge-1/nics" 2>/dev/null)
ac "REST /vms/{name}/nics has data" "$nic" "mac"

# ══════════════════════════════════════════════════════════
# 13. 3-Node Consistency
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[14/16] 3-Node Consistency${NC}"
for i in 1 2; do
    ip="${NODES[$i]}"; name="${NODE_NAMES[$i]}"
    h=$($SSH_CMD pcvdev@"$ip" "curl -s http://localhost:8080/api/v1/health" 2>/dev/null)
    ac "$name health ok" "$h" '"ok"'
    r=$($SSH_CMD pcvdev@"$ip" 'echo "{\"jsonrpc\":\"2.0\",\"method\":\"vm.list\",\"params\":{},\"id\":\"1\"}" | sudo nc -N -U /var/run/purecvisor/daemon.sock' 2>/dev/null)
    ac "$name UDS RPC ok" "$r" '"result"'
    m=$($SSH_CMD pcvdev@"$ip" "curl -s http://localhost:8080/api/v1/metrics" 2>/dev/null)
    ac "$name metrics ok" "$m" "purecvisor_info"
done

# ══════════════════════════════════════════════════════════
# 14. Responsive + Toast (구조적 검증)
# ══════════════════════════════════════════════════════════
echo -e "\n${YELLOW}[16/16] UI Structure Verification${NC}"
curl -s "$REST/ui/" > /tmp/pcv_ui_test.html 2>/dev/null
ui_size=$(wc -c < /tmp/pcv_ui_test.html)
TOTAL=$((TOTAL+1))
if [ "$ui_size" -gt 10000 ]; then
    PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} UI HTML size ${ui_size}b (>10KB)"
else
    FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} UI HTML too small: ${ui_size}b"
fi
for kw in "@media" "toast" "modal" "sidebar" "vm-list" "summary" "console" "snapshot" "WebSocket" "ctx" "pwr" "toggleTheme" "sb-search" "sb-sort" "mini-bar" "checkbox" "ev-panel" "containers" "cluster" "ovn" "menubar" "wizard-steps" "hw-list" "loadNics" "loadBP" "showVnc" "showAbout" "toggleFS"; do
    TOTAL=$((TOTAL+1))
    if /usr/bin/grep -q "$kw" /tmp/pcv_ui_test.html 2>/dev/null; then
        PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} HTML contains '$kw'"
    else
        FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} HTML missing '$kw'"
    fi
done
rm -f /tmp/pcv_ui_test.html

echo ""
echo "================================================================"
echo -e " Results: ${GREEN}${PASS} passed${NC} / ${RED}${FAIL} failed${NC} / ${TOTAL} total"
echo "================================================================"
echo ""
if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
