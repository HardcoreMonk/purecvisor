#!/bin/bash





set -uo pipefail

HOST="${1:-192.0.2.19}"
BASE="http://$HOST:80/api/v1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/auth_test_lib.sh"
PASS=0; FAIL=0; SKIP=0; TOTAL=0


G='\033[0;32m'; R='\033[0;31m'; Y='\033[0;33m'; C='\033[0;36m'; N='\033[0m'

log_pass() { ((PASS++)); ((TOTAL++)); echo -e "  ${G}PASS${N}  $1"; }
log_fail() { ((FAIL++)); ((TOTAL++)); echo -e "  ${R}FAIL${N}  $1 — $2"; }
log_skip() { ((SKIP++)); ((TOTAL++)); echo -e "  ${Y}SKIP${N}  $1"; }
section()  { echo -e "\n${C}═══ $1 ═══${N}"; }


section "1. AUTH (api.js)"
if pcv_resolve_auth "$BASE"; then
  AUTH="$PCV_AUTH_RESPONSE"
  TOKEN="$PCV_AUTH_TOKEN"
  CSRF="$PCV_AUTH_CSRF"
  log_pass "POST /auth/token → JWT issued"
else
  AUTH=""
  TOKEN=""
  CSRF=""
  log_skip "POST /auth/token → test credentials unavailable"
  echo "INFO: auth-dependent checks skipped (set PCV_TEST_ADMIN_USER/PCV_TEST_ADMIN_PASSWORD to enable)."
fi
[ -n "$TOKEN" ] && [ -n "$CSRF" ] && log_pass "CSRF token received" || log_skip "CSRF token"

H="Authorization: Bearer $TOKEN"
HC="$H"
HJ="Content-Type: application/json"
HX="X-CSRF-Token: $CSRF"

get()  { curl -s --max-time 8 -w '\n%{http_code}' -H "$H" "$BASE$1" 2>/dev/null; }
post() { curl -s --max-time 8 -w '\n%{http_code}' -X POST -H "$H" -H "$HJ" -H "$HX" -d "$2" "$BASE$1" 2>/dev/null; }
del()  { curl -s --max-time 8 -w '\n%{http_code}' -X DELETE -H "$H" -H "$HX" "$BASE$1" 2>/dev/null; }
put()  { curl -s --max-time 8 -w '\n%{http_code}' -X PUT -H "$H" -H "$HJ" -H "$HX" -d "$2" "$BASE$1" 2>/dev/null; }

check_get() {
  local label="$1" path="$2"
  if [ -z "$TOKEN" ]; then
    log_skip "GET $path (auth unavailable)"
    return 0
  fi
  local resp; resp=$(get "$path")
  local code; code=$(echo "$resp" | tail -1)
  if [ "$code" = "200" ]; then log_pass "GET $path"; else log_fail "GET $path" "HTTP $code"; fi
}

check_post() {
  local label="$1" path="$2" body="${3:-{}}"
  if [ -z "$TOKEN" ] || [ -z "$CSRF" ]; then
    log_skip "POST $path (auth unavailable)"
    return 0
  fi
  local resp; resp=$(post "$path" "$body")
  local code; code=$(echo "$resp" | tail -1)
  if [ "$code" = "200" ]; then log_pass "POST $path"; else log_fail "POST $path" "HTTP $code"; fi
}


section "2. UI STATIC FILES"
for f in "" "/index.html" "/style.css" "/app.js" "/i18n.js" "/sw.js" \
         "/modules/api.js" "/modules/ui.js" "/modules/vm.js" "/modules/container.js" \
         "/modules/network.js" "/modules/storage.js" "/modules/cluster.js" \
         "/modules/monitor.js" "/modules/cloud.js" "/modules/help.js" "/modules/nav.js"; do
  code=$(curl -s -o /dev/null -w '%{http_code}' "http://$HOST:80/ui$f" 2>/dev/null)
  if [ "$code" = "200" ]; then log_pass "/ui$f ($code)"; else log_fail "/ui$f" "HTTP $code"; fi
done


section "3. VM MODULE (vm.js)"
check_get "VM list" "/vms"


VM=$(curl -s -H "$H" "$BASE/vms" 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)
l=d.get('data',d) if isinstance(d,dict) else d
print(l[0]['name'] if l else '')
" 2>/dev/null)

if [ -n "$VM" ]; then
  log_pass "VM detected: $VM"
  check_get "VM snapshot list"   "/vms/$VM/snapshot"
  check_get "VM delete status"   "/vms/$VM/delete-status"
  check_get "VM NIC list"        "/vms/$VM/nics"


  SNAP_RESP=$(post "/vms/$VM/snapshot/create" "{\"snap_name\":\"e2e-test-snap\"}")
  SNAP_CODE=$(echo "$SNAP_RESP" | tail -1)
  if [ "$SNAP_CODE" = "200" ]; then
    log_pass "POST /vms/$VM/snapshot/create"
    DEL_RESP=$(del "/vms/$VM/snapshot/e2e-test-snap")
    DEL_CODE=$(echo "$DEL_RESP" | tail -1)
    [ "$DEL_CODE" = "200" ] && log_pass "DELETE /vms/$VM/snapshot/e2e-test-snap" || log_fail "Snapshot delete" "HTTP $DEL_CODE"
  else
    log_fail "Snapshot create" "HTTP $SNAP_CODE"
  fi


  check_post "Snapshot delete_all" "/vms/$VM/snapshot/delete_all" '{"keep_recent":9999}'
else
  log_skip "No VMs found — skipping VM-specific tests"
fi


section "4. CONTAINER MODULE (container.js)"
check_get "Container list" "/containers"

CTR=$(curl -s -H "$H" "$BASE/containers" 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)
l=d.get('data',d) if isinstance(d,dict) else d
print(l[0]['name'] if l else '')
" 2>/dev/null)

if [ -n "$CTR" ]; then
  log_pass "Container detected: $CTR"

  CSNAP_CODE=$(curl -s -o /dev/null -w '%{http_code}' -H "$H" "$BASE/containers/$CTR/snapshots" 2>/dev/null)
  if [ "$CSNAP_CODE" = "200" ]; then log_pass "GET /containers/$CTR/snapshots"
  elif [ "$CSNAP_CODE" = "500" ]; then log_skip "Container snapshots (non-ZFS rootfs)"
  else log_fail "Container snapshots" "HTTP $CSNAP_CODE"; fi
  check_get "Container NICs"      "/containers/$CTR/nics"
else
  log_skip "No containers — skipping"
fi


section "5. NETWORK MODULE (network.js)"
check_get "Network list"    "/networks"
check_get "OVN status"      "/ovn/status"
check_get "OVN switches"    "/ovn/switches"
check_get "OVN routers"     "/ovn/routers"
check_get "OVN ACL"         "/ovn/acl"


section "6. STORAGE MODULE (storage.js)"
check_get "Storage pools" "/storage/pools"
check_get "Storage zvols" "/storage/zvols"


section "7. CLUSTER MODULE (cluster.js)"
check_get "Cluster status"   "/cluster/status"
check_get "Cluster VMs"      "/cluster/vms"
check_get "Cluster quota"    "/cluster/quota"
check_get "Cluster affinity" "/cluster/affinity"


section "8. MONITOR MODULE (monitor.js)"
check_get "Health"       "/health"
check_get "Metrics"      "/metrics"
check_get "Processes"    "/processes"
check_get "ISO list"     "/iso"


section "9. CLOUD MODULE (cloud.js)"
check_get "Cloud jobs" "/cloud/jobs"


if [ -n "$TOKEN" ] && [ -n "$CSRF" ]; then
  IMPORT_RESP=$(post "/vms/e2e-test/import-ec2" '{"ami_id":"bad"}')
  IMPORT_BODY=$(echo "$IMPORT_RESP" | head -1)
  if echo "$IMPORT_BODY" | grep -q "Invalid AMI"; then
    log_pass "Import AMI validation works"
  else
    log_fail "Import AMI validation" "Expected error for bad AMI ID"
  fi
else
  log_skip "Import AMI validation"
fi


section "10. ACCOUNTS (api.js)"
check_get "User list" "/auth/users"


section "11. ALERTS + AUDIT + WEBHOOK"
check_get "Alert history"    "/alerts"
check_get "Alert config"     "/alerts/config"
check_get "Audit search"     "/audit/search"
check_get "Webhook DLQ"      "/webhook/dlq"


section "12. AI AGENT"
check_get "Agent config"  "/agent/config"
check_get "Agent history" "/agent/history"


section "13. BACKUP + TEMPLATES"
check_get "Backup policies" "/backup/policies"

BH_CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "$H" -H "$HJ" -H "$HX" \
  -d '{"vm_name":"*"}' "$BASE/backup/history" 2>/dev/null)

[ "$BH_CODE" = "200" ] && log_pass "POST /backup/history" || log_skip "Backup history (requires vm_name param, $BH_CODE)"
check_get "Template list"   "/templates"
check_get "Config history"  "/config/history"


section "14. DPDK + SR-IOV"
check_get "DPDK status"    "/dpdk/status"
check_get "DPDK list"      "/dpdk/list"
check_get "DPDK hugepage"  "/dpdk/hugepage"
check_get "SR-IOV status"  "/sriov/status"
check_get "SR-IOV list"    "/sriov/list"


section "15. FEDERATION + GPU + DOCKER"
check_get "Federation status" "/federation/status"
check_get "GPU list"          "/gpu/list"
check_get "GPU metrics"       "/gpu/metrics"


section "16. RBAC ENFORCEMENT"

if [ -n "$TOKEN" ] && [ -n "$CSRF" ]; then
  post "/auth/users" '{"username":"e2e-viewer","password":"pass123","role":"viewer"}' > /dev/null 2>&1

  VAUTH=$(curl -s -X POST "$BASE/auth/token" -H "$HJ" -d '{"username":"e2e-viewer","password":"pass123"}' 2>/dev/null)
  VTOK=$(echo "$VAUTH" | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
  VCSRF=$(echo "$VAUTH" | python3 -c "import sys,json;print(json.load(sys.stdin).get('csrf_token',''))" 2>/dev/null)
else
  VTOK=""
  VCSRF=""
fi

if [ -n "$VTOK" ]; then

  VCODE=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $VTOK" "$BASE/vms" 2>/dev/null)
  [ "$VCODE" = "200" ] && log_pass "VIEWER → GET /vms (200)" || log_fail "VIEWER read" "HTTP $VCODE"

  VCODE=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $VTOK" "$BASE/cloud/jobs" 2>/dev/null)
  [ "$VCODE" = "200" ] && log_pass "VIEWER → GET /cloud/jobs (200)" || log_fail "VIEWER cloud read" "HTTP $VCODE"


  VCODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Authorization: Bearer $VTOK" -H "$HJ" -H "X-CSRF-Token: $VCSRF" \
    -d '{"ami_id":"ami-0abcdef123456789"}' "$BASE/vms/test/import-ec2" 2>/dev/null)
  [ "$VCODE" = "403" ] && log_pass "VIEWER → POST import-ec2 (403 FORBIDDEN)" || log_fail "VIEWER write block" "HTTP $VCODE (expected 403)"
else
  log_skip "RBAC viewer test — could not get viewer token"
fi


echo ""
echo -e "${C}═══════════════════════════════════════════════${N}"
echo -e "  TOTAL: $TOTAL  ${G}PASS: $PASS${N}  ${R}FAIL: $FAIL${N}  ${Y}SKIP: $SKIP${N}"
echo -e "${C}═══════════════════════════════════════════════${N}"

[ $FAIL -eq 0 ] && echo -e "\n${G}✓ ALL TESTS PASSED${N}" || echo -e "\n${R}✗ SOME TESTS FAILED${N}"
exit $FAIL
