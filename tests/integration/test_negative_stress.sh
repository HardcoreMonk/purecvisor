#!/usr/bin/env bash





set -euo pipefail

HOST="${1:-localhost}"
BASE="http://${HOST}/api/v1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/auth_test_lib.sh"
PASS=0; FAIL=0; TOTAL=0

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }


if pcv_resolve_auth "${BASE}"; then
    TOKEN="$PCV_AUTH_TOKEN"
    CSRF="$PCV_AUTH_CSRF"
else
    TOKEN=""
    CSRF=""
fi
AUTH="Authorization: Bearer ${TOKEN}"
CSRFH="X-CSRF-Token: ${CSRF}"

if [ -z "$TOKEN" ]; then
    echo "INFO: auth-dependent negative/stress checks skipped (set PCV_TEST_ADMIN_USER/PCV_TEST_ADMIN_PASSWORD to enable)."
    pass "인증 의존 케이스 건너뜀"
    echo ""
    echo "=== 부정 테스트: 인증 ==="
    HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" -H "Authorization: Bearer invalid.token.here" "${BASE}/vms")
    [ "$HTTP" = "401" ] && pass "잘못된 JWT → 401" || fail "잘못된 JWT" "HTTP $HTTP"

    HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" -H "Authorization: Bearer " "${BASE}/vms")
    [ "$HTTP" = "401" ] && pass "빈 JWT → 401" || fail "빈 JWT" "HTTP $HTTP"

    echo ""
    echo "=== 스트레스 후 안정성 확인 ==="
    HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" "${BASE}/health")
    [ "$HTTP" = "200" ] && pass "스트레스 후 /health → 200" || fail "/health 이상" "HTTP $HTTP"

    echo ""
    echo "=========================================="
    echo -e "TOTAL: ${TOTAL} | ${GREEN}PASS: ${PASS}${NC} | ${RED}FAIL: ${FAIL}${NC}"
    echo "=========================================="
    [ "$FAIL" -eq 0 ] && exit 0 || exit 1
fi

echo "=== 부정 테스트: 잘못된 입력 ==="


HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/vms/nonexistent-vm-999")
[ "$HTTP" != "500" ] && pass "GET /vms/nonexistent → 비500 ($HTTP)" || fail "GET 존재하지 않는 VM" "500 서버 에러"


HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" -X POST -H "$AUTH" -H "$CSRFH" \
  -H 'Content-Type: application/json' -d '{}' "${BASE}/vms")
[ "$HTTP" = "400" ] && pass "POST /vms (빈 body) → 400" || fail "POST /vms 빈 body" "HTTP $HTTP"


HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" -X POST -H "$AUTH" -H "$CSRFH" \
  -H 'Content-Type: application/json' -d 'NOT_JSON' "${BASE}/vms")
[ "$HTTP" != "500" ] && pass "POST /vms (잘못된 JSON) → 비500 ($HTTP)" || fail "잘못된 JSON" "500"


LONGNAME=$(python3 -c "print('a'*500)")
HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" -X POST -H "$AUTH" -H "$CSRFH" \
  -H 'Content-Type: application/json' -d "{\"name\":\"${LONGNAME}\"}" "${BASE}/vms")
[ "$HTTP" != "500" ] && pass "POST /vms (500자 이름) → 비500 ($HTTP)" || fail "긴 이름" "500"


HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" -X POST -H "$AUTH" -H "$CSRFH" \
  -H 'Content-Type: application/json' -d '{"name":"<script>alert(1)</script>"}' "${BASE}/vms")
[ "$HTTP" != "500" ] && pass "POST /vms (XSS 이름) → 비500 ($HTTP)" || fail "XSS 이름" "500"


HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" -X POST -H "$AUTH" -H "$CSRFH" \
  -H 'Content-Type: application/json' -d '{"name":"test; DROP TABLE users;"}' "${BASE}/vms")
[ "$HTTP" != "500" ] && pass "POST /vms (SQL Injection) → 비500 ($HTTP)" || fail "SQL Injection" "500"


echo ""
echo "=== 부정 테스트: 인증 ==="
HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" -H "Authorization: Bearer invalid.token.here" "${BASE}/vms")
[ "$HTTP" = "401" ] && pass "잘못된 JWT → 401" || fail "잘못된 JWT" "HTTP $HTTP"

HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" -H "Authorization: Bearer " "${BASE}/vms")
[ "$HTTP" = "401" ] && pass "빈 JWT → 401" || fail "빈 JWT" "HTTP $HTTP"


echo ""
echo "=== 스트레스 테스트: Rate Limiting ==="
CODES=""
for i in $(seq 1 20); do
  HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/vms")
  CODES="${CODES}${HTTP} "
done
echo "  20연속 GET /vms: ${CODES}"

echo "$CODES" | grep -q "429" && fail "Rate limit 20회에서 429 발생" "" || pass "20연속 요청 정상 처리 (rate limit 미초과)"


echo ""
echo "=== 스트레스 테스트: 동시 요청 ==="
for i in $(seq 1 10); do
  curl -s -o /dev/null -H "$AUTH" "${BASE}/vms" &
done
wait
pass "10 동시 GET /vms 완료"


echo ""
echo "=== 스트레스 후 안정성 확인 ==="
HTTP=$(curl -s --max-time 8 -o /dev/null -w "%{http_code}" "${BASE}/health")
[ "$HTTP" = "200" ] && pass "스트레스 후 /health → 200" || fail "/health 이상" "HTTP $HTTP"


echo ""
echo "=========================================="
echo -e "TOTAL: ${TOTAL} | ${GREEN}PASS: ${PASS}${NC} | ${RED}FAIL: ${FAIL}${NC}"
echo "=========================================="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
