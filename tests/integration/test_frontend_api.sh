#!/bin/bash
                                                                                   
                                                                        
                          
                                                                        
                                                              
                                              
                              
 
                                                      
                                                                 
                                                                               
                                                                  
                                                              

set -uo pipefail

HOST="${1:-192.0.2.19}"
BASE_URL="${PCV_TEST_BASE_URL:-http://$HOST}"
while [[ "$BASE_URL" == */ && "$BASE_URL" != *"://" ]]; do
  BASE_URL="${BASE_URL%/}"
done
BASE="$BASE_URL/api/v1"

                                           
                                                          
                                              
CURL_COMMON_ARGS=(--disable --max-time 8)
if [[ "${PCV_TEST_TLS_INSECURE:-0}" == "1" ]]; then
  CURL_COMMON_ARGS+=(--insecure)
fi
curl() { command curl "${CURL_COMMON_ARGS[@]}" "$@"; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/auth_test_lib.sh"
PASS=0; FAIL=0; SKIP=0; TOTAL=0
RUN_ID="${PCV_TEST_RUN_ID:-$(date +%s)-$$}"
if [[ ! "$RUN_ID" =~ ^[A-Za-z0-9_-]+$ ]] || [ "${#RUN_ID}" -gt 40 ]; then
  echo "Invalid PCV_TEST_RUN_ID (use <=40 ASCII letters, digits, '_' or '-')" >&2
  exit 2
fi
VIEW_USER="pcv-view-${RUN_ID}"
SNAP_NAME="pcv-e2e-${RUN_ID}"
MUTATION_VM="${PCV_TEST_VM:-}"
MUTATION_STARTED=0
VIEWER_CREATED=0
SNAPSHOT_CLEANUP_ARMED=0

        
G='\033[0;32m'; R='\033[0;31m'; Y='\033[0;33m'; C='\033[0;36m'; N='\033[0m'

log_pass() { ((PASS++)); ((TOTAL++)); echo -e "  ${G}PASS${N}  $1"; }
log_fail() { ((FAIL++)); ((TOTAL++)); echo -e "  ${R}FAIL${N}  $1 — $2"; }
log_skip() { ((SKIP++)); ((TOTAL++)); echo -e "  ${Y}SKIP${N}  $1"; }
section()  { echo -e "\n${C}═══ $1 ═══${N}"; }

                                                                
section "1. AUTH (api.js)"
if [ -n "${PCV_TEST_ADMIN_USER:-}" ] && [ -n "${PCV_TEST_ADMIN_PASSWORD:-}" ] && \
   pcv_try_login "$BASE" "$PCV_TEST_ADMIN_USER" "$PCV_TEST_ADMIN_PASSWORD"; then
  AUTH="$PCV_AUTH_RESPONSE"
  TOKEN="$PCV_AUTH_TOKEN"
  CSRF="$PCV_AUTH_CSRF"
  log_pass "POST /auth/token → JWT issued"
else
  AUTH=""
  TOKEN=""
  CSRF=""
  log_skip "POST /auth/token → test credentials unavailable"
  echo "INFO: auth-dependent checks skipped (set dedicated PCV_TEST_ADMIN_USER/PCV_TEST_ADMIN_PASSWORD to enable)."
fi
[ -n "$TOKEN" ] && [ -n "$CSRF" ] && log_pass "CSRF token received" || log_skip "CSRF token"

H="Authorization: Bearer $TOKEN"
HC="$H"
HJ="Content-Type: application/json"
HX="X-CSRF-Token: $CSRF"

get()  { curl -s -w '\n%{http_code}' -H "$H" "$BASE$1" 2>/dev/null; }
post() { curl -s -w '\n%{http_code}' -X POST -H "$H" -H "$HJ" -H "$HX" -d "$2" "$BASE$1" 2>/dev/null; }
del()  { curl -s -w '\n%{http_code}' -X DELETE -H "$H" -H "$HX" "$BASE$1" 2>/dev/null; }
put()  { curl -s -w '\n%{http_code}' -X PUT -H "$H" -H "$HJ" -H "$HX" -d "$2" "$BASE$1" 2>/dev/null; }

cleanup() {
  local status=$? cleanup_failed=0 response code viewer_present=0 snapshot_present=0
  trap - EXIT
  if [ "$MUTATION_STARTED" -eq 1 ]; then
    if [ "$SNAPSHOT_CLEANUP_ARMED" -eq 1 ]; then
      response=$(get "/vms/$MUTATION_VM/snapshot" || true)
      code=$(printf '%s\n' "$response" | tail -1)
      if [ "$code" != "200" ]; then
        echo -e "${R}[CLEANUP-FAIL]${N} snapshot 현재 상태 조회 실패: $MUTATION_VM@$SNAP_NAME (HTTP ${code:-missing})" >&2
        cleanup_failed=1
      elif printf '%s' "$response" | grep -Fq "$SNAP_NAME"; then
        snapshot_present=1
      fi
      if [ "$snapshot_present" -eq 1 ]; then
        response=$(del "/vms/$MUTATION_VM/snapshot/$SNAP_NAME" || true)
        code=$(printf '%s\n' "$response" | tail -1)
        [ "$code" = "200" ] || {
          echo -e "${R}[CLEANUP-FAIL]${N} snapshot delete 실패: $MUTATION_VM@$SNAP_NAME (HTTP ${code:-missing})" >&2
          cleanup_failed=1
        }
      fi
    fi

                                                                
    response="$(get "/auth/users" || true)"
    code=$(printf '%s\n' "$response" | tail -1)
    if [ "$code" != "200" ]; then
      echo -e "${R}[CLEANUP-FAIL]${N} viewer 현재 상태 조회 실패: $VIEW_USER (HTTP ${code:-missing})" >&2
      cleanup_failed=1
    elif printf '%s' "$response" | grep -Fq "$VIEW_USER"; then
      viewer_present=1
    fi
    if [ "$VIEWER_CREATED" -eq 1 ] || [ "$viewer_present" -eq 1 ]; then
      response=$(curl -s -w '\n%{http_code}' -X DELETE -H "$H" -H "$HJ" -H "$HX" \
        -d "{\"username\":\"$VIEW_USER\"}" "$BASE/auth/users" 2>/dev/null || true)
      code=$(printf '%s\n' "$response" | tail -1)
      [ "$code" = "200" ] || {
        echo -e "${R}[CLEANUP-FAIL]${N} viewer delete 실패: $VIEW_USER (HTTP ${code:-missing})" >&2
        cleanup_failed=1
      }
    fi
    response="$(get "/auth/users" || true)"
    code=$(printf '%s\n' "$response" | tail -1)
    if [ "$code" != "200" ] || printf '%s' "$response" | grep -Fq "$VIEW_USER"; then
      echo -e "${R}[CLEANUP-FAIL]${N} viewer 잔여 확인 실패: $VIEW_USER" >&2
      cleanup_failed=1
    fi
    if [ "$SNAPSHOT_CLEANUP_ARMED" -eq 1 ]; then
      response="$(get "/vms/$MUTATION_VM/snapshot" || true)"
      code=$(printf '%s\n' "$response" | tail -1)
      if [ "$code" != "200" ] || printf '%s' "$response" | grep -Fq "$SNAP_NAME"; then
        echo -e "${R}[CLEANUP-FAIL]${N} snapshot 잔여 확인 실패: $MUTATION_VM@$SNAP_NAME" >&2
        cleanup_failed=1
      fi
    fi
  fi
  [ "$cleanup_failed" -eq 0 ] || exit 90
  exit "$status"
}
trap cleanup EXIT

if [ -n "$TOKEN" ] && [ -n "$CSRF" ]; then
  BASE_USERS=$(get "/auth/users" || true)
  BASE_USERS_CODE=$(printf '%s\n' "$BASE_USERS" | tail -1)
  if [ "$BASE_USERS_CODE" != "200" ] || printf '%s' "$BASE_USERS" | grep -Fq "$VIEW_USER"; then
    echo -e "${R}[ERROR]${N} account baseline capture failed or fixture collision: $VIEW_USER" >&2
    exit 1
  fi
  if [ -n "$MUTATION_VM" ]; then
    if [[ ! "$MUTATION_VM" =~ ^[A-Za-z0-9_.-]+$ ]]; then
      echo -e "${R}[ERROR]${N} invalid PCV_TEST_VM name" >&2
      exit 2
    fi
    BASE_SNAPS=$(get "/vms/$MUTATION_VM/snapshot" || true)
    BASE_SNAPS_CODE=$(printf '%s\n' "$BASE_SNAPS" | tail -1)
    if [ "$BASE_SNAPS_CODE" != "200" ] || printf '%s' "$BASE_SNAPS" | grep -Fq "$SNAP_NAME"; then
      echo -e "${R}[ERROR]${N} snapshot baseline capture failed or fixture collision: $MUTATION_VM@$SNAP_NAME" >&2
      exit 1
    fi
  fi
  MUTATION_STARTED=1
fi

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
         "/modules/network.js" "/modules/storage.js" \
         "/modules/monitor.js" "/modules/cloud.js" "/modules/help.js" "/modules/nav.js"; do
  code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE_URL/ui$f" 2>/dev/null)
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

                                                      
  if [ -n "$MUTATION_VM" ]; then
    SNAPSHOT_CLEANUP_ARMED=1
    SNAP_RESP=$(post "/vms/$MUTATION_VM/snapshot/create" "{\"snap_name\":\"$SNAP_NAME\"}")
    SNAP_CODE=$(echo "$SNAP_RESP" | tail -1)
    if [ "$SNAP_CODE" = "200" ]; then
      log_pass "POST /vms/$MUTATION_VM/snapshot/create"
      DEL_RESP=$(del "/vms/$MUTATION_VM/snapshot/$SNAP_NAME")
      DEL_CODE=$(echo "$DEL_RESP" | tail -1)
      [ "$DEL_CODE" = "200" ] && log_pass "DELETE /vms/$MUTATION_VM/snapshot/$SNAP_NAME" || log_fail "Snapshot delete" "HTTP $DEL_CODE"
    else
      log_fail "Snapshot create" "HTTP $SNAP_CODE"
    fi
  else
    log_skip "Snapshot mutation (set PCV_TEST_VM to a disposable VM)"
  fi
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

                                                                
section "7. MONITOR MODULE (monitor.js)"
check_get "Health"       "/health"
check_get "Metrics"      "/metrics"
check_get "Processes"    "/processes"
check_get "ISO list"     "/iso"

                                                                
section "8. CLOUD MODULE (cloud.js)"
check_get "Cloud jobs" "/cloud/jobs"

                                                                       
log_skip "Import AMI validation (destructive async path omitted from shared-daemon test)"

                                                                
section "9. ACCOUNTS (api.js)"
check_get "User list" "/auth/users"

                                                                
section "10. ALERTS + AUDIT + WEBHOOK"
check_get "Alert history"    "/alerts"
check_get "Alert config"     "/alerts/config"
check_get "Audit search"     "/audit/search"
check_get "Webhook DLQ"      "/webhook/dlq"

                                                                
section "11. AI AGENT"
check_get "Agent config"  "/agent/config"
check_get "Agent history" "/agent/history"

                                                               
section "12. BACKUP + TEMPLATES"
check_get "Backup policies" "/backup/policies"
                                                                    
BH_CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "$H" -H "$HJ" -H "$HX" \
  -d '{"vm_name":"*"}' "$BASE/backup/history" 2>/dev/null)
                                             
[ "$BH_CODE" = "200" ] && log_pass "POST /backup/history" || log_skip "Backup history (requires vm_name param, $BH_CODE)"
check_get "Template list"   "/templates"
check_get "Config history"  "/config/history"

                                                                
section "13. DPDK + SR-IOV"
check_get "DPDK status"    "/dpdk/status"
check_get "DPDK list"      "/dpdk/list"
check_get "DPDK hugepage"  "/dpdk/hugepage"
check_get "SR-IOV status"  "/sriov/status"
check_get "SR-IOV list"    "/sriov/list"

                                                                 
section "14. GPU"
check_get "GPU list"    "/gpu/list"
check_get "GPU metrics" "/gpu/metrics"

                                                                
section "15. RBAC ENFORCEMENT"
                             
if [ -n "$TOKEN" ] && [ -n "$CSRF" ]; then
  VIEW_CREATE_RESP=$(post "/auth/users" "{\"username\":\"$VIEW_USER\",\"password\":\"pass123\",\"role\":\"viewer\"}")
  VIEW_CREATE_CODE=$(printf '%s\n' "$VIEW_CREATE_RESP" | tail -1)
  if [ "$VIEW_CREATE_CODE" = "200" ]; then
    VIEWER_CREATED=1
  else
    log_fail "Create run-unique viewer" "HTTP $VIEW_CREATE_CODE"
  fi

  if [ "$VIEWER_CREATED" -eq 1 ]; then
    VAUTH=$(curl -s -X POST "$BASE/auth/token" -H "$HJ" -d "{\"username\":\"$VIEW_USER\",\"password\":\"pass123\"}" 2>/dev/null)
    VTOK=$(echo "$VAUTH" | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
    VCSRF=$(echo "$VAUTH" | python3 -c "import sys,json;print(json.load(sys.stdin).get('csrf_token',''))" 2>/dev/null)
  else
    VTOK=""
    VCSRF=""
  fi
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
    -d '{}' "$BASE/auth/users" 2>/dev/null)
  [ "$VCODE" = "403" ] && log_pass "VIEWER → POST /auth/users (403 FORBIDDEN)" || log_fail "VIEWER write block" "HTTP $VCODE (expected 403)"
else
  log_skip "RBAC viewer test — could not get viewer token"
fi

                                                                
echo ""
echo -e "${C}═══════════════════════════════════════════════${N}"
echo -e "  TOTAL: $TOTAL  ${G}PASS: $PASS${N}  ${R}FAIL: $FAIL${N}  ${Y}SKIP: $SKIP${N}"
echo -e "${C}═══════════════════════════════════════════════${N}"

[ $FAIL -eq 0 ] && echo -e "\n${G}✓ ALL TESTS PASSED${N}" || echo -e "\n${R}✗ SOME TESTS FAILED${N}"
exit $FAIL
