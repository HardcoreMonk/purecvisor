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
    ( set +o pipefail; echo "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":\"t$$\"}" | nc -N -U "$SOCK" 2>/dev/null )
}
ac() { local l="$1" h="$2" n="$3"; TOTAL=$((TOTAL+1)); if echo "$h" | grep -q "$n" 2>/dev/null; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} $l"; else FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} $l — expected '$n'"; echo "       $(echo "$h"|head -c 150)"; fi; }

echo ""
echo "============================================================"
echo " Phase 3 B+H+I Integration Test (3-Node)"
echo "============================================================"
echo ""


pid=$(systemctl show -p MainPID "$(detect_daemon_service)" --value)
init_log=$(journalctl _PID=$pid --no-pager 2>/dev/null | head -90)




echo -e "${YELLOW}[1/9] B: GPU Manager — 초기화${NC}"
ac "gpu_manager init log" "$init_log" "GPU manager initialized"

echo -e "${YELLOW}[2/9] B: GPU — 모듈 로드 확인${NC}"

ac "gpu_manager module loaded" "$init_log" "gpu_manager"

stable=$(rpc "vm.list")
ac "daemon stable after GPU init" "$stable" '"result"'




echo -e "${YELLOW}[3/9] H: Plugin Manager — 초기화${NC}"
ac "plugin_mgr init log" "$init_log" "Plugin"

echo -e "${YELLOW}[4/9] H: Plugin — dispatcher 플러그인 fallback 등록 확인${NC}"

ac "plugin fallback in dispatcher" "$init_log" "plugin_mgr"

dpdk_ok=$(rpc "dpdk.status")
ac "dpdk.status still works" "$dpdk_ok" '"available"'

echo -e "${YELLOW}[5/9] H: Plugin — 기존 RPC 정상 동작 (플러그인 삽입 후)${NC}"
vm_after=$(rpc "vm.list")
ac "vm.list still works after plugin init" "$vm_after" '"result"'




echo -e "${YELLOW}[6/9] I: mTLS — 초기화 (graceful degradation)${NC}"
ac "pcv_tls init log" "$init_log" "TLS disabled"

echo -e "${YELLOW}[7/9] I: mTLS — HTTP 평문 유지 확인${NC}"
health=$(curl -s http://localhost:8080/api/v1/health 2>/dev/null)
ac "REST /health still works (no TLS)" "$health" '"ok"'




echo -e "${YELLOW}[8/9] Regression — 핵심 기능 확인${NC}"

met=$(curl -s http://localhost:8080/api/v1/metrics 2>/dev/null)
ac "Prometheus metrics available" "$met" "purecvisor_rpc_requests_total"

TOTAL=$((TOTAL+1))
if [ -f /var/lib/purecvisor/pcv_audit.db ]; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} audit DB exists"
else FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} audit DB missing"; fi

ac "io_uring mode" "$init_log" "io_uring mode"

ac "WebSocket registered" "$init_log" "WebSocket handler registered"




echo -e "${YELLOW}[9/9] 3-Node Consistency${NC}"
for i in 0 1 2; do
    ip="${NODES[$i]}"; name="${NODE_NAMES[$i]}"
    if [ "$i" -eq 0 ]; then
        h=$(curl -s http://localhost:8080/api/v1/health 2>/dev/null)
    else
        h=$($SSH_CMD pcvdev@"$ip" "curl -s http://localhost:8080/api/v1/health 2>/dev/null" 2>/dev/null)
    fi
    ac "$name health ok" "$h" '"ok"'
done

echo ""
echo "============================================================"
echo -e " Results: ${GREEN}${PASS} passed${NC} / ${RED}${FAIL} failed${NC} / ${TOTAL} total"
echo "============================================================"
echo ""
if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
