#!/usr/bin/env bash
# =============================================================================
# Proxmox 9.1 LXC Container UI — 전체 기능 검증 (3-Node)
# =============================================================================
set -uo pipefail
trap '' PIPE

API="http://localhost:8080/api/v1"
REST="http://localhost:8080"
SOCK="/var/run/purecvisor/daemon.sock"
PASS=0; FAIL=0; TOTAL=0
SSH="/usr/bin/ssh"

RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[1;33m'; CYN='\033[0;36m'; NC='\033[0m'
ac(){ TOTAL=$((TOTAL+1)); if echo "$2"|/usr/bin/grep -q "$3" 2>/dev/null; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} $1"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} $1";echo "       $(echo "$2"|head -c 200)";fi; }
an(){ TOTAL=$((TOTAL+1)); if echo "$2"|/usr/bin/grep -q "$3" 2>/dev/null; then FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} $1 — should NOT have '$3'"; else PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} $1";fi; }

echo ""
echo "================================================================"
echo " Proxmox 9.1 LXC UI — Full E2E Verification (3-Node)"
echo "================================================================"

TOKEN=$(curl -s -X POST "$API/auth/token" -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ -n "$TOKEN" ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} JWT issued"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} JWT failed"; fi

# ═══════════════════════════════════════════════════════
# A. UI STRUCTURE — Proxmox 9탭 + 레이아웃 + 기능
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[A] UI Structure — 9 Tabs + Proxmox Layout (19 checks)${NC}"
UI=$(curl -s "$REST/ui/" 2>/dev/null)
for kw in "ctrTab" "ctrRenderTab" "ctrHist" "ctr-tab-content" "min-width:220px" "ctrReboot" "ctrSnapCreate" "ctrSnapRb" "ctrSnapDel" "ctrDnsAdd" "ctrRunCmd"; do
    ac "JS function: $kw" "$UI" "$kw"
done
for tab in "summary" "console" "resources" "network" "dns" "options" "snapshots" "notes" "tasks"; do
    ac "Tab: $tab" "$UI" "'$tab'"
done
# Proxmox-specific UI patterns
ac "Left panel fixed width" "$UI" "min-width:220px"
TOTAL=$((TOTAL+1))
uisz=$(echo "$UI" | wc -c)
if [ "$uisz" -gt 50000 ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} UI size: ${uisz}b (>50KB)"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} UI too small: ${uisz}b";fi

# ═══════════════════════════════════════════════════════
# B. CONTAINER SNAPSHOT REST ROUTES (4 routes)
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[B] Container Snapshot REST Routes (4 checks)${NC}"
# GET snapshots
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/containers/prometheus-ctr/snapshots" 2>/dev/null)
an "GET /snapshots not 404" "$r" "NOT_FOUND"
# POST create
r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"snap_name":"e2e-test-snap"}' "$API/containers/prometheus-ctr/snapshots" 2>/dev/null)
an "POST /snapshots not 404" "$r" "NOT_FOUND"
# POST rollback
r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"snap_name":"e2e-test-snap"}' "$API/containers/prometheus-ctr/snapshots/rollback" 2>/dev/null)
an "POST /snapshots/rollback not 404" "$r" "NOT_FOUND"
# DELETE
r=$(curl -s -X DELETE -H "Authorization: Bearer $TOKEN" "$API/containers/prometheus-ctr/snapshots/e2e-test-snap" 2>/dev/null)
an "DELETE /snapshots/{snap} not 404" "$r" "NOT_FOUND"

# ═══════════════════════════════════════════════════════
# C. CONTAINER LIFECYCLE — Summary Tab Data
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[C] Container Lifecycle — Summary Tab Data (8 checks)${NC}"
# List
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/containers" 2>/dev/null)
ac "container list has prometheus-ctr" "$r" "prometheus-ctr"
ac "container list has grafana-ctr" "$r" "grafana-ctr"

# Metrics (for summary tab)
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/containers/prometheus-ctr/metrics" 2>/dev/null)
ac "metrics: cpu_percent" "$r" "cpu_percent"
ac "metrics: net_rx_mb" "$r" "net_rx_mb"
ac "metrics: init_pid" "$r" "init_pid"

# Exec commands (summary tab uses these)
r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"command":"hostname"}' "$API/containers/prometheus-ctr/exec" 2>/dev/null)
ac "exec hostname" "$r" "prometheus-ctr"

r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"command":"uptime -p"}' "$API/containers/prometheus-ctr/exec" 2>/dev/null)
ac "exec uptime" "$r" "up"

r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"command":"nproc"}' "$API/containers/prometheus-ctr/exec" 2>/dev/null)
ac "exec nproc" "$r" "output"

# ═══════════════════════════════════════════════════════
# D. CONSOLE TAB — Shell Exec
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[D] Console Tab — Shell Commands (4 checks)${NC}"
r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"command":"ls /"}' "$API/containers/prometheus-ctr/exec" 2>/dev/null)
ac "exec ls /" "$r" "output"

r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"command":"whoami"}' "$API/containers/prometheus-ctr/exec" 2>/dev/null)
ac "exec whoami" "$r" "root"

r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"command":"echo hello-proxmox"}' "$API/containers/prometheus-ctr/exec" 2>/dev/null)
ac "exec echo" "$r" "hello-proxmox"

# Exec on stopped container should fail gracefully via UI state check
r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"command":"hostname"}' "$API/containers/web-ctr-1/exec" 2>/dev/null)
# web-ctr-1 is stopped, so exec will fail with error
ac "exec on stopped → error" "$r" "error"

# ═══════════════════════════════════════════════════════
# E. RESOURCES TAB — System Info Commands
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[E] Resources Tab — System Info (3 checks)${NC}"
r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"command":"free -h"}' "$API/containers/prometheus-ctr/exec" 2>/dev/null)
ac "exec free -h (memory)" "$r" "Mem"

r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"command":"uname -r"}' "$API/containers/prometheus-ctr/exec" 2>/dev/null)
ac "exec uname -r (kernel)" "$r" "output"

r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"command":"ps aux --no-headers | wc -l"}' "$API/containers/prometheus-ctr/exec" 2>/dev/null)
ac "exec process count" "$r" "output"

# ═══════════════════════════════════════════════════════
# F. DNS TAB — resolv.conf
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[F] DNS Tab — resolv.conf (1 check)${NC}"
r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"command":"cat /etc/resolv.conf"}' "$API/containers/prometheus-ctr/exec" 2>/dev/null)
ac "exec resolv.conf" "$r" "nameserver"

# ═══════════════════════════════════════════════════════
# G. ACCOUNT MANAGEMENT — RBAC REST
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[G] Account Management — RBAC (5 checks)${NC}"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/auth/users" 2>/dev/null)
ac "GET /auth/users" "$r" "admin"

r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"username":"e2e-user","password":"pass123","role":"viewer"}' "$API/auth/users" 2>/dev/null)
an "POST create user (no error)" "$r" '"error"'

r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/auth/users" 2>/dev/null)
ac "User e2e-user exists" "$r" "e2e-user"

r=$(curl -s -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"username":"e2e-user","role":"operator"}' "$API/auth/role" 2>/dev/null)
an "Set role (no error)" "$r" '"error"'

r=$(curl -s -X DELETE -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
    -d '{"username":"e2e-user"}' "$API/auth/users" 2>/dev/null)
an "Delete user (no error)" "$r" '"error"'

# ═══════════════════════════════════════════════════════
# H. REGRESSION — Core Infrastructure
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[H] Regression — Core Infrastructure (10 checks)${NC}"
r=$(curl -s "$API/health" 2>/dev/null); ac "health" "$r" '"ok"'
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/vms" 2>/dev/null); ac "vm.list" "$r" "name"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/networks" 2>/dev/null); ac "network.list" "$r" "name"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/storage/pools" 2>/dev/null); ac "storage.pools" "$r" "pcvpool"
r=$(curl -s "$API/metrics" 2>/dev/null); ac "prometheus metrics" "$r" "purecvisor_rpc_requests_total"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/ovn/status" 2>/dev/null); ac "ovn status" "$r" "available"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/backup/policies" 2>/dev/null); ac "backup policies" "$r" "data"
r=$(curl -s -H "Authorization: Bearer $TOKEN" "$API/dpdk/status" 2>/dev/null); ac "dpdk status" "$r" "available"

ws_code=$(curl -s -o /dev/null -w "%{http_code}" -H "Connection: Upgrade" -H "Upgrade: websocket" \
    -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGVzdA==" "$REST/api/v1/ws/events" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ "$ws_code" != "404" ] && [ "$ws_code" != "000" ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} WebSocket events ($ws_code)"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} WebSocket ($ws_code)";fi

vnc_code=$(curl -s -o /dev/null -w "%{http_code}" -H "Connection: Upgrade" -H "Upgrade: websocket" \
    -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGVzdA==" "$REST/api/v1/ws/vnc?port=5900" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ "$vnc_code" != "404" ] && [ "$vnc_code" != "000" ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} VNC proxy WS ($vnc_code)"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} VNC proxy ($vnc_code)";fi

# ═══════════════════════════════════════════════════════
# I. GRAFANA + PROMETHEUS — Monitoring Stack
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[I] Grafana + Prometheus Stack (5 checks)${NC}"
r=$(curl -s -o /dev/null -w "%{http_code}" http://192.0.2.60:9090/-/healthy 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ "$r" = "200" ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} Prometheus :9090"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} Prometheus ($r)";fi

r=$(curl -s -o /dev/null -w "%{http_code}" http://192.0.2.61:3000/api/health 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ "$r" = "200" ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} Grafana :3000"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} Grafana ($r)";fi

r=$(sudo lxc-attach -P /var/lib/purecvisor/lxc -n grafana-ctr -- curl -s "http://${PCV_TEST_GRAFANA_USER:-grafana-user}:${PCV_TEST_GRAFANA_PASSWORD:-grafana-password}@localhost:3000/api/search?tag=purecvisor" 2>/dev/null)
DASHCNT=$(echo "$r" | python3 -c "import sys,json;print(len(json.load(sys.stdin)))" 2>/dev/null)
TOTAL=$((TOTAL+1))
if [ "${DASHCNT:-0}" -ge 5 ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} Grafana dashboards: $DASHCNT (≥5)"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} Only $DASHCNT dashboards";fi

r=$(sudo lxc-attach -P /var/lib/purecvisor/lxc -n grafana-ctr -- curl -s "http://${PCV_TEST_GRAFANA_USER:-grafana-user}:${PCV_TEST_GRAFANA_PASSWORD:-grafana-password}@localhost:3000/api/datasources" 2>/dev/null)
ac "Datasource URL correct" "$r" "192.0.2.60"

r=$(sudo lxc-attach -P /var/lib/purecvisor/lxc -n prometheus-ctr -- curl -s 'http://localhost:9090/api/v1/query?query=purecvisor_host_cpu_percent' 2>/dev/null)
ac "Prometheus scraping active" "$r" '"result"'

# ═══════════════════════════════════════════════════════
# J. 3-NODE CONSISTENCY
# ═══════════════════════════════════════════════════════
echo -e "\n${YLW}[J] 3-Node Consistency (6 checks)${NC}"
for ip in 192.0.2.20 192.0.2.21; do
    nm="$([ "$ip" = "192.0.2.20" ] && echo Node2 || echo Node3)"
    h=$($SSH pcvdev@"$ip" "curl -s http://localhost:8080/api/v1/health" 2>/dev/null)
    ac "$nm health" "$h" '"ok"'
    u=$($SSH pcvdev@"$ip" 'echo "{\"jsonrpc\":\"2.0\",\"method\":\"vm.list\",\"params\":{},\"id\":\"1\"}" | sudo nc -N -U /var/run/purecvisor/daemon.sock' 2>/dev/null)
    ac "$nm UDS RPC" "$u" '"result"'
    ui=$($SSH pcvdev@"$ip" "curl -s http://localhost:8080/ui/ 2>/dev/null | grep -c 'ctrRenderTab'" 2>/dev/null)
    TOTAL=$((TOTAL+1))
    if [ "${ui:-0}" -gt 0 ]; then PASS=$((PASS+1));echo -e "  ${GRN}PASS${NC} $nm Proxmox UI deployed"; else FAIL=$((FAIL+1));echo -e "  ${RED}FAIL${NC} $nm UI missing";fi
done

echo ""
echo "================================================================"
echo -e " Results: ${GRN}${PASS} passed${NC} / ${RED}${FAIL} failed${NC} / ${TOTAL} total"
echo "================================================================"
echo ""
if [ "$FAIL" -gt 0 ]; then exit 1; fi
exit 0
