#!/usr/bin/env bash



















set -uo pipefail

HOST="${1:-localhost}"
BASE="http://${HOST}/api/v1"
TOKEN=""
PASS=0; FAIL=0; TOTAL=0
NONEXIST_VM="b2-noexist-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }
info() { echo -e "${BLUE}[INFO]${NC} $1"; }




if ! command -v sudo >/dev/null && [ "$(id -u)" != "0" ]; then
    echo -e "${RED}FATAL: sudo 필요${NC}"; exit 2
fi
if ! sudo test -r /var/lib/purecvisor/pcv_audit.db; then
    echo -e "${YELLOW}SKIP: audit DB 미존재${NC}"; exit 0
fi


TOKEN=$(curl -s --max-time 5 -X POST "${BASE}/auth/token" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" | \
  python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
if [ -z "$TOKEN" ]; then
    echo -e "${RED}FATAL: 인증 실패${NC}"; exit 1
fi
AUTH="Authorization: Bearer ${TOKEN}"

echo ""
echo "════════════════════════════════════════════════"
echo " B2: vm.start 실패 audit 정합성 (ADR-0018)"
echo "════════════════════════════════════════════════"
echo "  Host: ${HOST}"
echo "  Test VM (non-existent): ${NONEXIST_VM}"
echo ""




echo "[1] 존재하지 않는 VM start"
RESP=$(curl -s --max-time 5 -X POST -H "$AUTH" "${BASE}/vms/${NONEXIST_VM}/start" -d '{}')
if echo "$RESP" | grep -q '"accepted"'; then
    info "API accepted (fire-and-forget)"
else
    fail "API accepted" "$RESP"
fi


sleep 4


ROW=$(sudo sqlite3 /var/lib/purecvisor/pcv_audit.db \
  "SELECT method, target, result, error_code FROM audit_log WHERE method='vm.start' AND target='${NONEXIST_VM}' ORDER BY id DESC LIMIT 1;" 2>&1)

if [ -z "$ROW" ]; then
    fail "audit DB 기록" "vm.start ${NONEXIST_VM} 항목 없음"
elif echo "$ROW" | grep -q "fail"; then
    pass "audit DB result=fail 기록 확인 ($ROW)"
else
    fail "audit DB 결과" "fail이 아님: $ROW"
fi




echo ""
echo "[2] /health/recent-errors 엔드포인트 조회"
RESP=$(curl -s --max-time 5 "${BASE}/health/recent-errors?vm=${NONEXIST_VM}&limit=3")
if echo "$RESP" | python3 -c "import sys,json;d=json.load(sys.stdin);print('OK' if any(e.get('target')=='${NONEXIST_VM}' for e in d.get('data',[])) else 'NO')" 2>/dev/null | grep -q OK; then
    pass "/health/recent-errors 에서 ${NONEXIST_VM} 발견"
else
    fail "/health/recent-errors" "조회 실패: $(echo "$RESP" | head -c 200)"
fi


RESP=$(curl -s --max-time 5 "${BASE}/health/recent-errors?limit=3")
COUNT=$(echo "$RESP" | python3 -c "import sys,json;print(len(json.load(sys.stdin).get('data',[])))" 2>/dev/null || echo "0")
if [ "$COUNT" -gt 0 ]; then
    pass "/health/recent-errors (필터 없음) 응답: ${COUNT}건"
else
    fail "/health/recent-errors 필터 없음" "결과 0건"
fi




echo ""
echo "[3] 정상 VM start (있을 경우 ok 기록 확인)"
EXISTING=$(curl -s --max-time 5 -H "$AUTH" "${BASE}/vms" | \
  python3 -c "import sys,json;d=json.load(sys.stdin).get('data',[]);print((d[0]['name'] if d else ''))" 2>/dev/null)

if [ -n "$EXISTING" ]; then
    info "테스트 대상: $EXISTING"
    curl -s --max-time 5 -X POST -H "$AUTH" "${BASE}/vms/${EXISTING}/start" -d '{}' >/dev/null
    sleep 3
    ROW=$(sudo sqlite3 /var/lib/purecvisor/pcv_audit.db \
      "SELECT method, target, result FROM audit_log WHERE method='vm.start' AND target='${EXISTING}' ORDER BY id DESC LIMIT 1;" 2>&1)
    if echo "$ROW" | grep -q "ok"; then
        pass "정상 VM start audit ok 기록 확인 ($ROW)"
    else
        fail "정상 VM start ok 기록" "$ROW"
    fi
else
    info "기존 VM 없음 — Test 3 SKIP"
fi




echo ""
echo "[4] dispatcher 자동 audit 회귀 방지"
DISP_LIES=$(sudo sqlite3 /var/lib/purecvisor/pcv_audit.db \
  "SELECT COUNT(*) FROM audit_log WHERE method='vm.start' AND target='' AND ts > datetime('now','-1 minute');" 2>&1)
if [ "$DISP_LIES" = "0" ]; then
    pass "dispatcher 자동 audit 거짓 'ok' 미발생 (target='' count=0)"
else
    fail "dispatcher 자동 audit 회귀" "target='' 빈 vm.start 레코드 ${DISP_LIES}건 발견"
fi




echo ""
echo "════════════════════════════════════════════════"
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}B2 vm.start 실패 audit: ${PASS}/${TOTAL} PASSED${NC}"
    echo -e "${GREEN}ADR-0018 fire-and-forget audit 정합성 검증 완료${NC}"
    exit 0
else
    echo -e "${RED}B2 vm.start 실패 audit: ${PASS}/${TOTAL} ($FAIL FAILED)${NC}"
    exit 1
fi
