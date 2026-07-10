#!/usr/bin/env bash
# =============================================================================
# Full Feature Verification — 삭제진행표시 + 컨테이너IP폴링 + Swagger + 전체 회귀
# =============================================================================
set -uo pipefail
trap '' PIPE

SOCK="/var/run/purecvisor/daemon.sock"
API="http://localhost:8080/api/v1"
REST="http://localhost:8080"
PASS=0; FAIL=0; TOTAL=0
SSH="/usr/bin/ssh"
NODES=("192.0.2.19" "192.0.2.20" "192.0.2.21")
NNAMES=("Node1" "Node2" "Node3")

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; CYN='\033[0;36m'; NC='\033[0m'

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

rpc(){ local m="$1" p="${2-"{}"}"; ( set +o pipefail; echo "{\"jsonrpc\":\"2.0\",\"method\":\"$m\",\"params\":$p,\"id\":\"t$$\"}" | nc -N -U "$SOCK" 2>/dev/null ); }
ac(){ local l="$1" h="$2" n="$3"; TOTAL=$((TOTAL+1)); if echo "$h"|/usr/bin/grep -q "$n" 2>/dev/null; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} $l"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} $l — want '$n'";echo "       $(echo "$h"|head -c 200)";fi; }
an(){ local l="$1" h="$2" n="$3"; TOTAL=$((TOTAL+1)); if echo "$h"|/usr/bin/grep -q "$n" 2>/dev/null; then FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} $l — should NOT have '$n'"; else PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} $l";fi; }
ae(){ local l="$1" h="$2"; TOTAL=$((TOTAL+1)); if echo "$h"|grep -q '"error"' 2>/dev/null; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} $l (expected error)"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} $l — want error";fi; }

echo ""
echo "================================================================"
echo " Full Feature Verification Test (3-Node)"
echo "================================================================"
echo ""

TOKEN=$(curl -s -X POST "$API/auth/token" -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ -n "$TOKEN" ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} JWT issued"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} JWT failed"; fi

# ═══════════════════════════════════════════════════════
# A. ZVOL DELETE (-r 재귀 + name 필드)
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[A] Zvol Delete — -r recursive + name field${NC}"
r=$(rpc 'storage.zvol.delete' '{"name":"pcvpool/vms/nonexist-test"}')
ac "zvol.delete name field works" "$r" "dataset does not exist"
an "no (null) in zvol delete" "$r" "(null)"
r=$(rpc 'storage.zvol.delete' '{}')
ac "zvol.delete missing param → error" "$r" "Missing"

# ═══════════════════════════════════════════════════════
# B. CONTAINER LIFECYCLE (START→IP→EXEC→STOP→DELETE)
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[B] Container Lifecycle${NC}"
r=$(rpc 'container.list')
ac "container.list" "$r" '"result"'
HAS_CTR=0; echo "$r"|grep -q 'web-ctr-1' && HAS_CTR=1

if [ "$HAS_CTR" -eq 1 ]; then
    # B1. START
    r=$(rpc 'container.start' '{"name":"web-ctr-1"}')
    ac "container.start UDS" "$r" '"result"'
    an "no /bin/sh error" "$r" "non-zero"

    # B2. IP POLLING (최대 12초)
    GOT_IP=0
    for i in $(seq 1 8); do
        sleep 1.5
        r=$(rpc 'container.list')
        if echo "$r"|grep -q '"ip_addr":"[0-9]'; then GOT_IP=1; break; fi
    done
    TOTAL=$((TOTAL+1))
    if [ "$GOT_IP" -eq 1 ]; then
        IP=$(echo "$r"|python3 -c "import sys,json;[print(c['ip_addr']) for c in json.loads(sys.stdin.read().split('\"result\":')[1].rstrip('}'))  if c.get('name')=='web-ctr-1']" 2>/dev/null || echo "?")
        PASS=$((PASS+1)); echo -e "  ${GRN}PASS${NC} container IP acquired: $IP"
    else
        FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} container IP not acquired in 12s"
    fi

    # B3. EXEC via UDS
    r=$(rpc 'container.exec' '{"name":"web-ctr-1","cmd":"cat /etc/hostname"}')
    ac "container.exec UDS output" "$r" '"output"'

    # B4. EXEC via REST (command→cmd mapping)
    r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
        -d '{"command":"uname -r"}' "$API/containers/web-ctr-1/exec" 2>/dev/null)
    ac "REST exec command→cmd" "$r" "output"

    # B5. STOP
    r=$(rpc 'container.stop' '{"name":"web-ctr-1"}')
    ac "container.stop" "$r" '"result"'
    sleep 3

    # B6. verify stopped
    r=$(rpc 'container.list')
    ac "container state STOPPED" "$r" "STOPPED"

    # B7. REST lifecycle
    r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d '{}' "$API/containers/web-ctr-1/start" 2>/dev/null)
    ac "REST start" "$r" "success"
    sleep 2
    r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d '{}' "$API/containers/web-ctr-1/stop" 2>/dev/null)
    TOTAL=$((TOTAL+1))
    if ! echo "$r"|grep -q '"error"' 2>/dev/null; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} REST stop"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} REST stop error";fi
    sleep 3

    # B8. daemon stability after stop
    r=$(curl -s "$API/health" 2>/dev/null)
    ac "daemon stable after container stop" "$r" '"ok"'
else
    echo -e "  ${CYN}SKIP${NC} No container — B tests skipped"
fi

# ═══════════════════════════════════════════════════════
# C. UI STRUCTURE — 진행표시 + Swagger + 컨테이너 삭제
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[C] UI Structure — Progress + Swagger + Delete${NC}"
curl -s "$REST/ui/" > /tmp/_pcv_ui.html 2>/dev/null
uisz=$(wc -c < /tmp/_pcv_ui.html)
TOTAL=$((TOTAL+1))
if [ "$uisz" -gt 10000 ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} UI size ${uisz}b (>10KB)"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} UI too small ${uisz}b";fi

for kw in "prog-bar" "prog-fill" "spinner" "prog-status" "ctrDel" "doCtrDel" "del-ctr-confirm" "swTry" "Swagger" "ip_addr" "d.output" "skipContent" "loadAll(true)" "d.error" "zfs destroy -r" "dv-p" "dn-p" "dz-p" "dc-p"; do
    TOTAL=$((TOTAL+1))
    if /usr/bin/grep -q "$kw" /tmp/_pcv_ui.html 2>/dev/null; then
        PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} UI has '$kw'"
    else
        FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} UI missing '$kw'"
    fi
done
rm -f /tmp/_pcv_ui.html

# ═══════════════════════════════════════════════════════
# D. SWAGGER API — Try-It 실제 호출 시뮬레이션
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[D] Swagger Try-It API Calls${NC}"
r=$(curl -s "$API/health" 2>/dev/null)
ac "GET /health" "$r" '"ok"'
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/vms" 2>/dev/null)
ac "GET /vms" "$r" "name"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/containers" 2>/dev/null)
ac "GET /containers" "$r" "data"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/networks" 2>/dev/null)
ac "GET /networks" "$r" "name"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/storage/pools" 2>/dev/null)
ac "GET /storage/pools" "$r" "pcvpool"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/storage/zvols" 2>/dev/null)
ac "GET /storage/zvols" "$r" "data"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/ovn/status" 2>/dev/null)
ac "GET /ovn/status" "$r" "available"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/ovn/switches" 2>/dev/null)
ac "GET /ovn/switches" "$r" "data"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/ovn/routers" 2>/dev/null)
ac "GET /ovn/routers" "$r" "data"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/backup/policies" 2>/dev/null)
ac "GET /backup/policies" "$r" "data"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/dpdk/status" 2>/dev/null)
ac "GET /dpdk/status" "$r" "available"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/sriov/status" 2>/dev/null)
ac "GET /sriov/status" "$r" "available"
r=$(curl -s "$API/metrics" 2>/dev/null)
ac "GET /metrics (Prometheus)" "$r" "purecvisor_rpc_requests_total"

# ═══════════════════════════════════════════════════════
# E. REGRESSION — 핵심 기능 회귀 테스트
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[E] Regression — Core Functions${NC}"
r=$(rpc "vm.list")
ac "UDS vm.list" "$r" '"result"'

ws_code=$(curl -s -o /dev/null -w "%{http_code}" -H "Connection: Upgrade" -H "Upgrade: websocket" -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGVzdA==" "$REST/api/v1/ws/events" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ "$ws_code" != "404" ] && [ "$ws_code" != "000" ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} WebSocket (HTTP $ws_code)"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} WebSocket ($ws_code)";fi

r=$(rpc 'dpdk.bind' '{"pci_addr":"../../etc"}')
ae "path traversal rejected" "$r"

# seccomp disabled
pid=$(systemctl show -p MainPID "$(detect_daemon_service)" --value 2>/dev/null)
init_log=$(journalctl _PID=$pid --no-pager 2>/dev/null | head -100)
ac "seccomp skipped" "$init_log" "seccomp skipped"
ac "io_uring mode" "$init_log" "io_uring"

# path traversal UI block
code=$(curl -s -o /dev/null -w "%{http_code}" "$REST/ui/../../etc/passwd" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ "$code" = "403" ] || [ "$code" = "404" ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} UI path traversal blocked ($code)"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} UI path traversal ($code)";fi

# audit
TOTAL=$((TOTAL+1))
if [ -f /var/lib/purecvisor/pcv_audit.db ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} audit.db exists"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} audit.db missing";fi

# ═══════════════════════════════════════════════════════
# F. 3-NODE CONSISTENCY
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[F] 3-Node Consistency${NC}"
for i in 1 2; do
    ip="${NODES[$i]}"; nm="${NNAMES[$i]}"
    h=$($SSH pcvdev@"$ip" "curl -s http://localhost:8080/api/v1/health" 2>/dev/null)
    ac "$nm health" "$h" '"ok"'
    r=$($SSH pcvdev@"$ip" 'echo "{\"jsonrpc\":\"2.0\",\"method\":\"vm.list\",\"params\":{},\"id\":\"1\"}" | sudo nc -N -U /var/run/purecvisor/daemon.sock' 2>/dev/null)
    ac "$nm UDS RPC" "$r" '"result"'
    m=$($SSH pcvdev@"$ip" "curl -s http://localhost:8080/api/v1/metrics" 2>/dev/null)
    ac "$nm metrics" "$m" "purecvisor_info"
    ui=$($SSH pcvdev@"$ip" "curl -s http://localhost:8080/ui/ 2>/dev/null | wc -c" 2>/dev/null)
    TOTAL=$((TOTAL+1))
    if [ "${ui:-0}" -gt 10000 ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} $nm UI (${ui}b)"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} $nm UI (${ui}b)";fi
    sw=$($SSH pcvdev@"$ip" 'curl -s http://localhost:8080/ui/ 2>/dev/null | grep -c "Swagger"' 2>/dev/null)
    TOTAL=$((TOTAL+1))
    if [ "${sw:-0}" -gt 0 ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} $nm Swagger deployed"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} $nm Swagger missing";fi
done

echo ""
echo "================================================================"
echo -e " Results: ${GRN}${PASS} passed${NC} / ${RED}${FAIL} failed${NC} / ${TOTAL} total"
echo "================================================================"
echo ""
if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
