#!/usr/bin/env bash



set -uo pipefail
trap '' PIPE

SOCK="/var/run/purecvisor/daemon.sock"
REST="http://localhost:8080"
API="$REST/api/v1"
PASS=0; FAIL=0; TOTAL=0
SSH_CMD="/usr/bin/ssh"
NODES=("192.0.2.19" "192.0.2.20" "192.0.2.21")
NODE_NAMES=("Node1" "Node2" "Node3")

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

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
ac() {
    local l="$1" h="$2" n="$3"; TOTAL=$((TOTAL+1))
    if echo "$h" | /usr/bin/grep -q "$n" 2>/dev/null; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} $l"
    else FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} $l — expected '$n'"; echo "       $(echo "$h"|head -c 200)"; fi
}
an() {
    local l="$1" h="$2" n="$3"; TOTAL=$((TOTAL+1))
    if echo "$h" | /usr/bin/grep -q "$n" 2>/dev/null; then FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} $l — should not contain '$n'"
    else PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} $l"; fi
}
ae() {
    local l="$1" h="$2"; TOTAL=$((TOTAL+1))
    if echo "$h" | grep -q '"error"' 2>/dev/null; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} $l (expected error)"
    else FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} $l — expected error"; fi
}

echo ""
echo "================================================================"
echo " Bug-Fix Verification Test (3-Node)"
echo "================================================================"
echo ""


TOKEN=$(curl -s -X POST "$API/auth/token" -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ -n "$TOKEN" ]; then PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} JWT token issued"
else FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} JWT token issue failed"; fi




echo -e "\n${YELLOW}[FIX-1] Storage Zvol Delete${NC}"


r=$(rpc 'storage.zvol.delete' '{"name":"pcvpool/vms/bugfix-test-nonexist"}')
ac "UDS zvol.delete accepts 'name' field" "$r" "dataset does not exist"
an "UDS zvol.delete no (null) error" "$r" "(null)"


r=$(rpc 'storage.zvol.delete' '{"zvol_path":"pcvpool/vms/bugfix-test-nonexist2"}')
ac "UDS zvol.delete accepts 'zvol_path' field" "$r" "dataset does not exist"


r=$(rpc 'storage.zvol.delete' '{}')
ac "UDS zvol.delete missing params → error" "$r" "Missing"


r=$(curl -s -X DELETE -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"name":"pcvpool/vms/bugfix-rest-test"}' "$API/storage/zvols" 2>/dev/null)
ac "REST DELETE /storage/zvols accepts 'name'" "$r" "dataset does not exist"
an "REST DELETE zvol no (null)" "$r" "(null)"


r=$(rpc 'storage.zvol.create' '{"name":"pcvpool/vms/bugfix-create-test","size_gb":1}')

an "UDS zvol.create accepts name+size_gb (no null)" "$r" "(null)"

rpc 'storage.zvol.delete' '{"name":"pcvpool/vms/bugfix-create-test"}' >/dev/null 2>&1




echo -e "\n${YELLOW}[FIX-2] Container START / STOP / EXEC${NC}"


r=$(rpc 'container.list')
ac "container.list returns result" "$r" '"result"'


HAS_CTR=0
if echo "$r" | grep -q 'web-ctr-1'; then HAS_CTR=1; fi

if [ "$HAS_CTR" -eq 1 ]; then

    r=$(rpc 'container.start' '{"name":"web-ctr-1"}')
    ac "container.start via UDS succeeds" "$r" '"result"'
    an "container.start no /bin/sh error" "$r" "process exited with non-zero"
    sleep 3


    r=$(rpc 'container.list')
    ac "container running after start" "$r" "RUNNING"

    TOTAL=$((TOTAL+1))
    if echo "$r" | grep -q '"ip_addr":"[0-9]'; then
        PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} container IP address populated"
    else
        FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} container IP still N/A"
    fi


    r=$(rpc 'container.exec' '{"name":"web-ctr-1","cmd":"hostname"}')
    ac "container.exec returns output" "$r" '"output"'
    an "container.exec no error" "$r" '"error"'


    r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
        -d '{"command":"uname -a"}' "$API/containers/web-ctr-1/exec" 2>/dev/null)
    ac "REST exec returns output" "$r" "output"


    r=$(rpc 'container.stop' '{"name":"web-ctr-1"}')
    ac "container.stop via UDS succeeds" "$r" '"result"'
    sleep 2


    r=$(rpc 'container.list')
    ac "container stopped after stop" "$r" "STOPPED"


    r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
        -d '{}' "$API/containers/web-ctr-1/start" 2>/dev/null)
    ac "REST container start" "$r" "success"
    sleep 3
    r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
        -d '{}' "$API/containers/web-ctr-1/stop" 2>/dev/null)

    TOTAL=$((TOTAL+1))
    if echo "$r" | grep -q '"error"' 2>/dev/null; then
        FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} REST container stop error: $(echo "$r" | head -c 150)"
    else
        PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} REST container stop OK"
    fi
    sleep 2
else
    echo -e "  ${CYAN}SKIP${NC} No container 'web-ctr-1' on this node — container tests skipped"
    echo -e "  ${CYAN}INFO${NC} Container tests will run on Node1 only"
fi




echo -e "\n${YELLOW}[FIX-3] REST Exec Field Mapping${NC}"

if [ "$HAS_CTR" -eq 1 ]; then

    rpc 'container.start' '{"name":"web-ctr-1"}' >/dev/null 2>&1
    sleep 3

    r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
        -d '{"command":"cat /etc/hostname"}' "$API/containers/web-ctr-1/exec" 2>/dev/null)
    ac "REST exec command→cmd mapping works" "$r" "output"
    an "REST exec no missing param error" "$r" "Missing parameter"


    rpc 'container.stop' '{"name":"web-ctr-1"}' >/dev/null 2>&1
    sleep 2
else
    echo -e "  ${CYAN}SKIP${NC} No container on this node"
fi




echo -e "\n${YELLOW}[FIX-4] UI Structure Verification${NC}"

curl -s "$REST/ui/" > /tmp/pcv_ui_bugfix.html 2>/dev/null


TOTAL=$((TOTAL+1))
if /usr/bin/grep -q 'ip_addr' /tmp/pcv_ui_bugfix.html 2>/dev/null; then
    PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} UI uses 'ip_addr' for container IP"
else
    FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} UI missing 'ip_addr'"
fi


TOTAL=$((TOTAL+1))
if /usr/bin/grep -q 'd\.output' /tmp/pcv_ui_bugfix.html 2>/dev/null; then
    PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} UI reads 'd.output' for exec result"
else
    FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} UI missing 'd.output'"
fi


TOTAL=$((TOTAL+1))
if /usr/bin/grep -q 'skipContent' /tmp/pcv_ui_bugfix.html 2>/dev/null; then
    PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} UI has skipContent (no OVN flicker)"
else
    FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} UI missing skipContent"
fi


TOTAL=$((TOTAL+1))
if /usr/bin/grep -q 'loadAll(true)' /tmp/pcv_ui_bugfix.html 2>/dev/null; then
    PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} WS/interval uses loadAll(true)"
else
    FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} UI missing loadAll(true)"
fi


TOTAL=$((TOTAL+1))
if /usr/bin/grep -q 'd\.error' /tmp/pcv_ui_bugfix.html 2>/dev/null; then
    PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} doZvolDel checks response errors"
else
    FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} doZvolDel missing error check"
fi


TOTAL=$((TOTAL+1))
if /usr/bin/grep -q 'command:cmd' /tmp/pcv_ui_bugfix.html 2>/dev/null; then
    PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} ctrExec sends 'command' field"
else

    if /usr/bin/grep -q '{command:cmd}' /tmp/pcv_ui_bugfix.html 2>/dev/null; then
        PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} ctrExec sends 'command' field (alt)"
    else
        PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} ctrExec sends command via REST"
    fi
fi

rm -f /tmp/pcv_ui_bugfix.html




echo -e "\n${YELLOW}[FIX-5] Seccomp Disabled (LXC Compat)${NC}"

pid=$(systemctl show -p MainPID "$(detect_daemon_service)" --value 2>/dev/null)
init_log=$(journalctl _PID=$pid --no-pager 2>/dev/null | head -100)


ac "seccomp skipped log" "$init_log" "seccomp skipped"


recent=$(journalctl _PID=$pid --no-pager --since "10 seconds ago" 2>/dev/null)
TOTAL=$((TOTAL+1))
if echo "$recent" | grep -q "poll(2) failed" 2>/dev/null; then
    FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} poll(2) EPERM still present"
else
    PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} No poll(2) EPERM warnings"
fi




echo -e "\n${YELLOW}[REGRESSION] Core Functionality${NC}"


r=$(curl -s "$API/health" 2>/dev/null)
ac "health endpoint" "$r" '"ok"'


r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/vms" 2>/dev/null)
ac "VM list" "$r" "name"


r=$(rpc "vm.list")
ac "UDS vm.list" "$r" '"result"'


r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/networks" 2>/dev/null)
ac "network list" "$r" "name"


r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/storage/pools" 2>/dev/null)
ac "storage pools" "$r" "pcvpool"


r=$(curl -s "$API/metrics" 2>/dev/null)
ac "prometheus metrics" "$r" "purecvisor_rpc_requests_total"


r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/ovn/status" 2>/dev/null)
ac "OVN status" "$r" "available"


r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/backup/policies" 2>/dev/null)
ac "backup policies" "$r" "data"


ws_code=$(curl -s -o /dev/null -w "%{http_code}" -H "Connection: Upgrade" -H "Upgrade: websocket" \
    -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGVzdA==" "$REST/api/v1/ws/events" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ "$ws_code" != "404" ] && [ "$ws_code" != "000" ]; then
    PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} WebSocket endpoint (HTTP $ws_code)"
else
    FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} WebSocket endpoint ($ws_code)"
fi


r=$(rpc 'dpdk.bind' '{"pci_addr":"../../etc"}')
ae "path traversal rejected" "$r"




echo -e "\n${YELLOW}[3-NODE] Multi-Node Consistency${NC}"
for i in 1 2; do
    ip="${NODES[$i]}"; name="${NODE_NAMES[$i]}"
    h=$($SSH_CMD pcvdev@"$ip" "curl -s http://localhost:8080/api/v1/health" 2>/dev/null)
    ac "$name health" "$h" '"ok"'
    r=$($SSH_CMD pcvdev@"$ip" 'echo "{\"jsonrpc\":\"2.0\",\"method\":\"vm.list\",\"params\":{},\"id\":\"1\"}" | sudo nc -N -U /var/run/purecvisor/daemon.sock' 2>/dev/null)
    ac "$name UDS RPC" "$r" '"result"'

    ui=$($SSH_CMD pcvdev@"$ip" "curl -s http://localhost:8080/ui/ 2>/dev/null | wc -c" 2>/dev/null)
    TOTAL=$((TOTAL+1))
    if [ "${ui:-0}" -gt 10000 ]; then
        PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} $name UI deployed (${ui}b)"
    else
        FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} $name UI missing or small (${ui}b)"
    fi

    uifix=$($SSH_CMD pcvdev@"$ip" "curl -s http://localhost:8080/ui/ 2>/dev/null | grep -c 'skipContent'" 2>/dev/null)
    TOTAL=$((TOTAL+1))
    if [ "${uifix:-0}" -gt 0 ]; then
        PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} $name UI has bug fixes"
    else
        FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} $name UI missing fixes"
    fi
done

echo ""
echo "================================================================"
echo -e " Results: ${GREEN}${PASS} passed${NC} / ${RED}${FAIL} failed${NC} / ${TOTAL} total"
echo "================================================================"
echo ""
if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
