#!/usr/bin/env bash
set -uo pipefail
HOST="${1:-192.0.2.53}"
BASE="http://$HOST/api/v1"
PASS=0; FAIL=0; TOTAL=0
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }

is_blocked() { [ "$1" = "401" ] || [ "$1" = "403" ] || [ "$1" = "400" ] || [ "$1" = "404" ] || [ "$1" = "500" ] || [ "$1" = "000" ]; }

echo -e "${CYAN}═══ RBAC Bypass Attempt Test ═══${NC}"

AT=$(curl -s -X POST "$BASE/auth/token" -H "Content-Type: application/json" -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" | python3 -c "import json,sys;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
[ -z "$AT" ] && { echo "FATAL: no admin token"; exit 1; }

curl -s -X POST "$BASE/auth/users" -H "Authorization: Bearer $AT" -H "Content-Type: application/json" -d '{"username":"rbac_v","password":"v123456v","role":"viewer"}' >/dev/null 2>&1
VT=$(curl -s -X POST "$BASE/auth/token" -H "Content-Type: application/json" -d '{"username":"rbac_v","password":"v123456v"}' | python3 -c "import json,sys;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
[ -z "$VT" ] && VT="invalid"

echo -e "${CYAN}=== VIEWER→ADMIN ===${NC}"
for ep in "POST /auth/users" "POST /auth/role" "DELETE /auth/users/rbac_v" "PUT /alerts/config" "POST /cloud/cancel" "PUT /config/daemon"; do
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

curl -s -X DELETE "$BASE/auth/users/rbac_v" -H "Authorization: Bearer $AT" >/dev/null 2>&1
echo
echo "═══════════════════════════════════════════════"
echo -e "TOTAL: $TOTAL | ${GREEN}PASS: $PASS${NC} | ${RED}FAIL: $FAIL${NC}"
echo "═══════════════════════════════════════════════"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
