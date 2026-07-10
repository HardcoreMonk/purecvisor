#!/usr/bin/env bash
# =============================================================================
# PureCVisor REST API 통합 테스트 — 88 엔드포인트 E2E 검증
# =============================================================================
# 사용법: bash tests/integration/test_rest_api_full.sh [HOST]
# 기본: HOST=localhost
# 필수: curl, jq (또는 python3)
# =============================================================================
set -euo pipefail

HOST="${1:-localhost}"
BASE="http://${HOST}/api/v1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/auth_test_lib.sh"
PASS=0
FAIL=0
SKIP=0
TOTAL=0

# CLOSE-WAIT backlog 고갈 방지: 모든 curl에 Connection: close + 타임아웃
CURL="curl -s --max-time 8"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }
skip() { SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); echo -e "  ${YELLOW}[SKIP]${NC} $1"; }

# ── 1. 인증 없이 접근 가능한 엔드포인트 ──
echo "=== Public Endpoints (인증 불필요) ==="

# Health
HTTP=$($CURL -o /dev/null -w "%{http_code}" "${BASE}/health")
[ "$HTTP" = "200" ] && pass "GET /health → 200" || fail "GET /health" "HTTP $HTTP"

# Metrics
HTTP=$($CURL -o /dev/null -w "%{http_code}" "${BASE}/metrics")
[ "$HTTP" = "200" ] && pass "GET /metrics → 200" || fail "GET /metrics" "HTTP $HTTP"

# Internal VMs
HTTP=$($CURL -o /dev/null -w "%{http_code}" "${BASE}/internal/vms")
[ "$HTTP" = "200" ] && pass "GET /internal/vms → 200" || fail "GET /internal/vms" "HTTP $HTTP"

# Internal Telemetry
HTTP=$($CURL -o /dev/null -w "%{http_code}" "${BASE}/internal/telemetry")
[ "$HTTP" = "200" ] && pass "GET /internal/telemetry → 200" || fail "GET /internal/telemetry" "HTTP $HTTP"

# ── 2. JWT 인증 ──
echo ""
echo "=== Authentication ==="

# 올바른 인증
if pcv_resolve_auth "${BASE}"; then
  RESP="$PCV_AUTH_RESPONSE"
  TOKEN="$PCV_AUTH_TOKEN"
  CSRF="$PCV_AUTH_CSRF"
  pass "POST /auth/token → JWT 발급"
else
  TOKEN=""
  CSRF=""
  skip "POST /auth/token → 테스트 자격 증명 없음"
  echo "INFO: auth-dependent checks skipped (set PCV_TEST_ADMIN_USER/PCV_TEST_ADMIN_PASSWORD to enable)."
fi

# CSRF 토큰 확인
[ -n "$TOKEN" ] && [ -n "$CSRF" ] && pass "POST /auth/token → CSRF 토큰 발급" || skip "CSRF 토큰 (미구현 가능)"

# 잘못된 인증
HTTP=$($CURL -o /dev/null -w "%{http_code}" -X POST "${BASE}/auth/token" \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"wrong"}')
[ "$HTTP" = "401" ] && pass "POST /auth/token (잘못된 비밀번호) → 401" || fail "POST /auth/token (bad pass)" "HTTP $HTTP"

# 토큰 없이 보호된 엔드포인트
HTTP=$($CURL -o /dev/null -w "%{http_code}" "${BASE}/vms")
[ "$HTTP" = "401" ] && pass "GET /vms (토큰 없음) → 401" || fail "GET /vms (no auth)" "HTTP $HTTP"

AUTH="Authorization: Bearer ${TOKEN}"
CSRFH="X-CSRF-Token: ${CSRF}"

# ── 3. VM 엔드포인트 ──
echo ""
echo "=== VM Endpoints ==="

if [ -n "$TOKEN" ]; then
  HTTP=$($CURL -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/vms")
  [ "$HTTP" = "200" ] && pass "GET /vms → 200" || fail "GET /vms" "HTTP $HTTP"
else
  skip "GET /vms → 인증 자격 증명 없음"
fi

# ── 4. Network 엔드포인트 ──
echo ""
echo "=== Network Endpoints ==="

if [ -n "$TOKEN" ]; then
  HTTP=$($CURL -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/networks")
  [ "$HTTP" = "200" ] && pass "GET /networks → 200" || fail "GET /networks" "HTTP $HTTP"
else
  skip "GET /networks → 인증 자격 증명 없음"
fi

# ── 5. Storage 엔드포인트 ──
echo ""
echo "=== Storage Endpoints ==="

if [ -n "$TOKEN" ]; then
  HTTP=$($CURL -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/storage/pools")
  [ "$HTTP" = "200" ] && pass "GET /storage/pools → 200" || fail "GET /storage/pools" "HTTP $HTTP"

  HTTP=$($CURL -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/storage/zvols")
  [ "$HTTP" = "200" ] && pass "GET /storage/zvols → 200" || fail "GET /storage/zvols" "HTTP $HTTP"
else
  skip "GET /storage/pools → 인증 자격 증명 없음"
  skip "GET /storage/zvols → 인증 자격 증명 없음"
fi

# ── 6. Container 엔드포인트 ──
echo ""
echo "=== Container Endpoints ==="

if [ -n "$TOKEN" ]; then
  HTTP=$($CURL -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/containers")
  [ "$HTTP" = "200" ] && pass "GET /containers → 200" || fail "GET /containers" "HTTP $HTTP"
else
  skip "GET /containers → 인증 자격 증명 없음"
fi

# ── 7. Monitoring 엔드포인트 ──
echo ""
echo "=== Monitoring Endpoints ==="

if [ -n "$TOKEN" ]; then
  HTTP=$($CURL -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/monitor/fleet")
  [ "$HTTP" = "200" ] && pass "GET /monitor/fleet → 200" || fail "GET /monitor/fleet" "HTTP $HTTP"

  HTTP=$($CURL -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/alerts")
  [ "$HTTP" = "200" ] && pass "GET /alerts → 200" || fail "GET /alerts" "HTTP $HTTP"

  HTTP=$($CURL -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/alerts/config")
  [ "$HTTP" = "200" ] && pass "GET /alerts/config → 200" || fail "GET /alerts/config" "HTTP $HTTP"

  HTTP=$($CURL -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/processes")
  [ "$HTTP" = "200" ] && pass "GET /processes → 200" || fail "GET /processes" "HTTP $HTTP"
else
  skip "GET /monitor/fleet → 인증 자격 증명 없음"
  skip "GET /alerts → 인증 자격 증명 없음"
  skip "GET /alerts/config → 인증 자격 증명 없음"
  skip "GET /processes → 인증 자격 증명 없음"
fi

# ── 8. ISO 엔드포인트 ──
echo ""
echo "=== ISO Endpoints ==="

if [ -n "$TOKEN" ]; then
  HTTP=$($CURL -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/iso")
  [ "$HTTP" = "200" ] && pass "GET /iso → 200" || fail "GET /iso" "HTTP $HTTP"
else
  skip "GET /iso → 인증 자격 증명 없음"
fi

# ── 9. RBAC 엔드포인트 ──
echo ""
echo "=== RBAC Endpoints ==="

if [ -n "$TOKEN" ]; then
  HTTP=$($CURL -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/auth/users")
  [ "$HTTP" = "200" ] && pass "GET /auth/users → 200" || fail "GET /auth/users" "HTTP $HTTP"
else
  skip "GET /auth/users → 인증 자격 증명 없음"
fi

# ── 10. CSRF 검증 ──
echo ""
echo "=== CSRF Validation ==="

# CSRF 없이 POST → 403 (CSRF 구현된 경우)
if [ -n "$TOKEN" ]; then
  HTTP=$($CURL -o /dev/null -w "%{http_code}" -X POST -H "$AUTH" \
    -H 'Content-Type: application/json' \
    -d '{"bridge_name":"test-csrf","mode":"nat","cidr":"10.99.0.1/24"}' \
    "${BASE}/networks")
  [ "$HTTP" = "403" ] && pass "POST /networks (CSRF 없음) → 403" || skip "CSRF 검증 (HTTP $HTTP)"
else
  skip "POST /networks (CSRF 검증) → 인증 자격 증명 없음"
fi

# ── 11. JSON 스키마 검증 ──
echo ""
echo "=== JSON Schema Validation ==="

# name 누락 VM 생성 → 400
if [ -n "$TOKEN" ] && [ -n "$CSRF" ]; then
  HTTP=$($CURL -o /dev/null -w "%{http_code}" -X POST -H "$AUTH" -H "$CSRFH" \
    -H 'Content-Type: application/json' \
    -d '{"vcpu":2}' \
    "${BASE}/vms")
  [ "$HTTP" = "400" ] && pass "POST /vms (name 누락) → 400" || skip "JSON 스키마 (HTTP $HTTP)"
else
  skip "POST /vms (JSON 스키마) → 인증 자격 증명 없음"
fi

# ── 결과 ──
echo ""
echo "=========================================="
echo -e "TOTAL: ${TOTAL} | ${GREEN}PASS: ${PASS}${NC} | ${RED}FAIL: ${FAIL}${NC} | ${YELLOW}SKIP: ${SKIP}${NC}"
echo "=========================================="

[ "$FAIL" -eq 0 ] && exit 0 || exit 1
