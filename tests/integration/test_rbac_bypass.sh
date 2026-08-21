#!/usr/bin/env bash
                                                                       
                                                      
                                                                      
set -uo pipefail
HOST="${1:-192.0.2.53}"
BASE="http://$HOST/api/v1"
TEST_USER="${PCV_RBAC_TEST_USER:-rbac_v_$$}"
AT=""
USER_CREATED=0
PASS=0; FAIL=0; TOTAL=0
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }
                                                                                   
is_blocked() { [ "$1" = "401" ] || [ "$1" = "403" ] || [ "$1" = "400" ] || [ "$1" = "404" ] || [ "$1" = "500" ] || [ "$1" = "000" ]; }

                                                  
                                          
cleanup() {
  if [ "$USER_CREATED" -eq 1 ] && [ -n "$AT" ]; then
    curl -sS -X DELETE "$BASE/auth/users/$TEST_USER" \
      -H "Authorization: Bearer $AT" >/dev/null 2>&1 || \
      echo "WARN: failed to remove RBAC fixture user $TEST_USER" >&2
  fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo -e "${CYAN}═══ RBAC Bypass Attempt Test ═══${NC}"

AT=$(curl -s -X POST "$BASE/auth/token" -H "Content-Type: application/json" -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" | python3 -c "import json,sys;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
[ -z "$AT" ] && { echo "FATAL: no admin token"; exit 1; }

CREATE_BODY=$(printf '{"username":"%s","password":"v123456v","role":"viewer"}' "$TEST_USER")
curl -s -X POST "$BASE/auth/users" -H "Authorization: Bearer $AT" -H "Content-Type: application/json" -d "$CREATE_BODY" >/dev/null 2>&1
USER_CREATED=1
TOKEN_BODY=$(printf '{"username":"%s","password":"v123456v"}' "$TEST_USER")
VT=$(curl -s -X POST "$BASE/auth/token" -H "Content-Type: application/json" -d "$TOKEN_BODY" | python3 -c "import json,sys;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
[ -z "$VT" ] && VT="invalid"

echo -e "${CYAN}=== VIEWER→ADMIN ===${NC}"
for ep in "POST /auth/users" "POST /auth/role" "DELETE /auth/users/$TEST_USER" "PUT /alerts/config" "POST /cloud/cancel" "PUT /config/daemon"; do
  M="${ep%% *}"; P="${ep#* }"
  C=$(curl -s -o /dev/null -w "%{http_code}" -X "$M" "$BASE$P" -H "Authorization: Bearer $VT" -H "Content-Type: application/json" -d '{}' 2>/dev/null)
  is_blocked "$C" && pass "$ep → $C" || fail "$ep → $C" "expected blocked"
done

echo -e "${CYAN}=== JWT 조작 ===${NC}"
C=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/vms" -H "Authorization: Bearer ${AT%.*}.TAMPERED"); is_blocked "$C" && pass "변조JWT→$C" || fail "변조JWT→$C" "expected blocked"
C=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/vms" -H "Authorization: Bearer "); is_blocked "$C" && pass "빈Bearer→$C" || fail "빈Bearer→$C" "expected blocked"
C=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/vms" -H "X-API-Key: pcv_fake"); is_blocked "$C" && pass "위조Key→$C" || fail "위조Key→$C" "expected blocked"

echo -e "${CYAN}=== CSRF ===${NC}"
C=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/vms/x/snapshot/delete_all" -H "Authorization: Bearer $AT" -H "Content-Type: application/json" -d '{}'); is_blocked "$C" && pass "CSRF없이→$C" || fail "CSRF없이→$C" "expected blocked"
C=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/vms/x/snapshot/delete_all" -H "Authorization: Bearer $AT" -H "X-CSRF-Token: wrong" -H "Content-Type: application/json" -d '{}'); is_blocked "$C" && pass "잘못된CSRF→$C" || fail "잘못된CSRF→$C" "expected blocked"

cleanup
USER_CREATED=0
echo
echo "═══════════════════════════════════════════════"
echo -e "TOTAL: $TOTAL | ${GREEN}PASS: $PASS${NC} | ${RED}FAIL: $FAIL${NC}"
echo "═══════════════════════════════════════════════"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
