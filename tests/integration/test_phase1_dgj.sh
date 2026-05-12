#!/usr/bin/env bash



set -uo pipefail
trap '' PIPE

SOCK="/var/run/purecvisor/daemon.sock"
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

assert_not_empty() {
    local label="$1" value="$2"
    TOTAL=$((TOTAL + 1))
    if [ -n "$value" ] && [ "$value" != "{}" ] && [ "$value" != "[]" ]; then
        PASS=$((PASS + 1))
        echo -e "  ${GREEN}PASS${NC} $label"
    else
        FAIL=$((FAIL + 1))
        echo -e "  ${RED}FAIL${NC} $label — got empty/null"
    fi
}

echo ""
echo "============================================================"
echo " Phase 1 D+G+J Integration Test"
echo "============================================================"
echo ""




echo -e "${YELLOW}[1/7] G: Prometheus Exporter — 초기화 로그 확인${NC}"
pid=$(systemctl show -p MainPID "$(detect_daemon_service)" --value)
init_log=$(journalctl _PID=$pid --no-pager 2>/dev/null | head -100)
assert_contains "prom_export init log" "$init_log" "Prometheus exporter initialized"

echo -e "${YELLOW}[2/7] G: Prometheus 메트릭 — RPC 후 카운터 증가${NC}"

rpc "vm.list" > /dev/null
rpc "dpdk.status" > /dev/null
rpc "sriov.status" > /dev/null
sleep 1


metrics=$(curl -s http://localhost:8080/api/v1/metrics 2>/dev/null)
assert_contains "metrics has purecvisor_rpc_requests_total" "$metrics" "purecvisor_rpc_requests_total"
assert_contains "metrics has purecvisor_rpc_duration_ms" "$metrics" "purecvisor_rpc_duration_ms"
assert_contains "metrics has purecvisor_info" "$metrics" "purecvisor_info"




echo -e "${YELLOW}[3/7] J: Audit Trail — 초기화 로그 확인${NC}"
assert_contains "audit init log" "$init_log" "Audit trail initialized"

echo -e "${YELLOW}[4/7] J: Audit Trail — SQLite DB 존재 확인${NC}"
db_exists="no"
if [ -f /var/lib/purecvisor/pcv_audit.db ]; then db_exists="yes"; fi
TOTAL=$((TOTAL + 1))
if [ "$db_exists" = "yes" ]; then
    PASS=$((PASS + 1))
    echo -e "  ${GREEN}PASS${NC} pcv_audit.db exists"
else
    FAIL=$((FAIL + 1))
    echo -e "  ${RED}FAIL${NC} pcv_audit.db not found"
fi

echo -e "${YELLOW}[5/7] J: Audit Trail — RPC 후 감사 로그 파일 확인${NC}"
sleep 2

audit_log="/var/log/purecvisor/audit.log"
if [ -f "$audit_log" ]; then
    audit_lines=$(tail -20 "$audit_log" 2>/dev/null | grep -c "vm.list\|dpdk.status\|sriov.status" 2>/dev/null || echo "0")
    TOTAL=$((TOTAL + 1))
    if [ "$audit_lines" -ge 1 ] 2>/dev/null; then
        PASS=$((PASS + 1))
        echo -e "  ${GREEN}PASS${NC} audit.log has $audit_lines matching records"
    else
        FAIL=$((FAIL + 1))
        echo -e "  ${RED}FAIL${NC} audit.log has $audit_lines records (expected >= 1)"
    fi

    sample=$(tail -5 "$audit_log" 2>/dev/null | head -1)
    assert_not_empty "audit.log record not empty" "$sample"
else
    TOTAL=$((TOTAL + 2))
    FAIL=$((FAIL + 2))
    echo -e "  ${RED}FAIL${NC} audit.log not found"
    echo -e "  ${RED}FAIL${NC} audit record check skipped"
fi




echo -e "${YELLOW}[6/7] D: Hot Reload — 버전 정보 확인${NC}"


rpc_after=$(rpc "vm.list")
assert_contains "daemon stable after D/G/J init" "$rpc_after" '"result"'




echo -e "${YELLOW}[7/7] 3-Node Consistency — Prometheus + Audit${NC}"
for i in 1 2; do
    ip="${NODES[$i]}"
    name="${NODE_NAMES[$i]}"

    m=$($SSH_CMD pcvdev@"$ip" "curl -s http://localhost:8080/api/v1/metrics 2>/dev/null" 2>/dev/null)
    assert_contains "$name has purecvisor_info metric" "$m" "purecvisor_info"

    r=$($SSH_CMD pcvdev@"$ip" 'echo "{\"jsonrpc\":\"2.0\",\"method\":\"vm.list\",\"params\":{},\"id\":\"1\"}" | sudo nc -N -U /var/run/purecvisor/daemon.sock 2>/dev/null' 2>/dev/null)
    assert_contains "$name UDS RPC works" "$r" '"result"'
done




echo ""
echo "============================================================"
echo -e " Results: ${GREEN}${PASS} passed${NC} / ${RED}${FAIL} failed${NC} / ${TOTAL} total"
echo "============================================================"
echo ""

if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
