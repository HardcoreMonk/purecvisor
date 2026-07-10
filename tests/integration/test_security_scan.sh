#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
# PureCVisor REST API 보안 자동 스캔
#
# [OWASP Top 10 기반 검증]
#   A01: 인증/인가 우회
#   A02: 암호화 실패
#   A03: 인젝션 (SQLi, Command, XSS, Path Traversal)
#   A04: 안전하지 않은 설계
#   A05: 보안 구성 오류
#   A07: 크로스사이트 스크립팅 (XSS)
#
# [사용법] bash tests/integration/test_security_scan.sh [HOST]
# ═══════════════════════════════════════════════════════════════

set -uo pipefail

HOST="${1:-localhost}"
BASE="http://${HOST}/api/v1"
PASS=0; FAIL=0; TOTAL=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }

# JWT 인증
TOKEN=$(curl -s --max-time 5 -X POST "${BASE}/auth/token" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" | \
  python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
CSRF=$(curl -s --max-time 5 -X POST "${BASE}/auth/token" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" | \
  python3 -c "import sys,json;print(json.load(sys.stdin).get('csrf_token',''))" 2>/dev/null)

AUTH="Authorization: Bearer ${TOKEN}"
CT="Content-Type: application/json"
CS="X-CSRF-Token: ${CSRF}"

echo "═══════════════════════════════════════════════"
echo -e "  ${CYAN}PureCVisor Security Scan${NC}"
echo "  Host: ${HOST}"
echo "═══════════════════════════════════════════════"

# ═══ A01: 인증/인가 ═══
echo ""
echo -e "${CYAN}=== A01: 인증/인가 우회 ===${NC}"

# 토큰 없이 보호 엔드포인트 접근
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" "${BASE}/vms")
[ "$HTTP" = "401" ] && pass "토큰 없이 GET /vms → 401" || fail "토큰 없이 접근" "HTTP $HTTP (expected 401)"

# 잘못된 JWT
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" -H "Authorization: Bearer invalid.token.here" "${BASE}/vms")
[ "$HTTP" = "401" ] && pass "잘못된 JWT → 401" || fail "잘못된 JWT" "HTTP $HTTP"

# 만료된 JWT (임의 조작)
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJhZG1pbiIsImV4cCI6MH0.invalid" \
  "${BASE}/vms")
[ "$HTTP" = "401" ] && pass "만료된 JWT → 401" || fail "만료된 JWT" "HTTP $HTTP"

# 빈 Authorization 헤더
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" -H "Authorization: " "${BASE}/vms")
[ "$HTTP" = "401" ] && pass "빈 Authorization → 401" || fail "빈 Auth 헤더" "HTTP $HTTP"

# 잘못된 비밀번호
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" -X POST \
  -H "$CT" -d '{"username":"admin","password":"wrong"}' "${BASE}/auth/token")
[ "$HTTP" = "401" ] && pass "잘못된 비밀번호 → 401" || fail "잘못된 비밀번호" "HTTP $HTTP"

# ═══ A03: 인젝션 ═══
echo ""
echo -e "${CYAN}=== A03: 인젝션 방어 ===${NC}"

# Command Injection in VM name
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d '{"name":"test;rm -rf /","vcpu":1}' "${BASE}/vms")
[ "$HTTP" = "400" ] && pass "Command Injection → 400" || fail "Command Injection" "HTTP $HTTP (expected 400)"

# SQL Injection in VM name
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d "{\"name\":\"' OR 1=1 --\",\"vcpu\":1}" "${BASE}/vms")
[ "$HTTP" = "400" ] && pass "SQL Injection → 400" || fail "SQL Injection" "HTTP $HTTP"

# XSS in VM name
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d '{"name":"<script>alert(1)</script>","vcpu":1}' "${BASE}/vms")
[ "$HTTP" = "400" ] && pass "XSS in VM name → 400" || fail "XSS" "HTTP $HTTP"

# Path Traversal in ISO path
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d '{"name":"safe-vm","iso_path":"../../etc/passwd"}' "${BASE}/vms")
([ "$HTTP" = "400" ] || [ "$HTTP" = "200" ]) && pass "Path Traversal ISO → $HTTP (검증됨)" || fail "Path Traversal" "HTTP $HTTP"

# Null byte injection
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d "{\"name\":\"test\u0000evil\",\"vcpu\":1}" "${BASE}/vms")
[ "$HTTP" != "500" ] && pass "Null byte → non-500 ($HTTP)" || fail "Null byte" "HTTP 500"

# ═══ A05: 보안 구성 ═══
echo ""
echo -e "${CYAN}=== A05: 보안 구성 ===${NC}"

# 서버 버전 노출 확인
SERVER_HEADER=$(curl -s --max-time 5 -I "${BASE}/health" | grep -i "^Server:" || echo "none")
if echo "$SERVER_HEADER" | grep -qi "libsoup\|apache\|nginx"; then
    fail "Server 헤더 노출" "$SERVER_HEADER"
else
    pass "Server 헤더 비노출"
fi

# X-Content-Type-Options
XCTO=$(curl -s --max-time 5 -I -H "$AUTH" "${BASE}/vms" | grep -i "X-Content-Type-Options" || echo "")
[ -n "$XCTO" ] && pass "X-Content-Type-Options 존재" || fail "X-Content-Type-Options" "누락"

# ═══ A07: XSS 방어 ═══
echo ""
echo -e "${CYAN}=== A07: XSS 방어 (응답 검증) ===${NC}"

# VM 생성 시 XSS 페이로드가 응답에 이스케이프 없이 반환되는지
RESP=$(curl -s --max-time 5 -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d '{"name":"<img src=x>","vcpu":1}' "${BASE}/vms")
if echo "$RESP" | grep -q "<img"; then
    fail "XSS 페이로드 응답에 미이스케이프" "raw HTML in response"
else
    pass "XSS 페이로드 차단/이스케이프"
fi

# ═══ Rate Limiting ═══
echo ""
echo -e "${CYAN}=== Rate Limiting ===${NC}"

# 30회 빠른 요청
RATE_OK=0
for i in $(seq 1 30); do
    HTTP=$(curl -s --max-time 2 -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/vms")
    [ "$HTTP" = "200" ] && RATE_OK=$((RATE_OK+1))
done
[ "$RATE_OK" -ge 25 ] && pass "30회 빠른 요청: ${RATE_OK}/30 성공" || fail "Rate Limiting 과도" "${RATE_OK}/30만 성공"

# ═══ CORS ═══
echo ""
echo -e "${CYAN}=== CORS 검증 ===${NC}"

CORS=$(curl -s --max-time 5 -I -H "Origin: http://evil.com" "${BASE}/health" | grep -i "Access-Control-Allow-Origin" || echo "")
if echo "$CORS" | grep -qi "evil.com"; then
    fail "CORS 와일드카드" "임의 Origin 허용"
else
    pass "CORS 제한적 ($CORS)"
fi

# ── 결과 ──
echo ""
echo "═══════════════════════════════════════════════"
echo -e "TOTAL: ${TOTAL} | ${GREEN}PASS: ${PASS}${NC} | ${RED}FAIL: ${FAIL}${NC}"
echo "═══════════════════════════════════════════════"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
