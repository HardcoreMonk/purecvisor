#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
# B2 회귀 테스트: vm.start 실패 시 audit DB가 진짜 결과를 기록하는가
#
# [목적] ADR-0018 검증 — fire-and-forget RPC가 거짓 'ok'를 기록하지 않고,
#       실제 워커 결과를 audit_log 테이블에 정확히 남기는지 확인.
#
# [시나리오]
#   1. 존재하지 않는 VM start → audit DB result=fail 기록 확인
#   2. /health/recent-errors 엔드포인트로 동일 사유 조회 가능 확인
#   3. (선택) 실제 broken VM start → 워커 실패 → audit DB fail
#
# [사전 조건]
#   - purecvisorsd 실행 중
#   - configured admin credential 로그인 가능
#   - /var/lib/purecvisor/pcv_audit.db 접근 가능 (sudo)
#
# [사용법]
#   bash tests/integration/test_vm_start_failures.sh [HOST]
# ═══════════════════════════════════════════════════════════════
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

# ─────────────────────────────────────────────────────
# 사전 체크
# ─────────────────────────────────────────────────────
if ! command -v sudo >/dev/null && [ "$(id -u)" != "0" ]; then
    echo -e "${RED}FATAL: sudo 필요${NC}"; exit 2
fi
if ! sudo test -r /var/lib/purecvisor/pcv_audit.db; then
    echo -e "${YELLOW}SKIP: audit DB 미존재${NC}"; exit 0
fi

# 인증 토큰
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

# ─────────────────────────────────────────────────────
# Test 1: 존재하지 않는 VM 시작 → audit DB result=fail
# ─────────────────────────────────────────────────────
echo "[1] 존재하지 않는 VM start"
RESP=$(curl -s --max-time 5 -X POST -H "$AUTH" "${BASE}/vms/${NONEXIST_VM}/start" -d '{}')
if echo "$RESP" | grep -q '"accepted"'; then
    info "API accepted (fire-and-forget)"
else
    fail "API accepted" "$RESP"
fi

# 워커 완료 대기 (1-2초로 충분, 안전하게 4초)
sleep 4

# audit DB 확인
ROW=$(sudo sqlite3 /var/lib/purecvisor/pcv_audit.db \
  "SELECT method, target, result, error_code FROM audit_log WHERE method='vm.start' AND target='${NONEXIST_VM}' ORDER BY id DESC LIMIT 1;" 2>&1)

if [ -z "$ROW" ]; then
    fail "audit DB 기록" "vm.start ${NONEXIST_VM} 항목 없음"
elif echo "$ROW" | grep -q "fail"; then
    pass "audit DB result=fail 기록 확인 ($ROW)"
else
    fail "audit DB 결과" "fail이 아님: $ROW"
fi

# ─────────────────────────────────────────────────────
# Test 2: /health/recent-errors 엔드포인트
# ─────────────────────────────────────────────────────
echo ""
echo "[2] /health/recent-errors 엔드포인트 조회"
RESP=$(curl -s --max-time 5 "${BASE}/health/recent-errors?vm=${NONEXIST_VM}&limit=3")
if echo "$RESP" | python3 -c "import sys,json;d=json.load(sys.stdin);print('OK' if any(e.get('target')=='${NONEXIST_VM}' for e in d.get('data',[])) else 'NO')" 2>/dev/null | grep -q OK; then
    pass "/health/recent-errors 에서 ${NONEXIST_VM} 발견"
else
    fail "/health/recent-errors" "조회 실패: $(echo "$RESP" | head -c 200)"
fi

# vm 필터 없이 호출도 작동 확인
RESP=$(curl -s --max-time 5 "${BASE}/health/recent-errors?limit=3")
COUNT=$(echo "$RESP" | python3 -c "import sys,json;print(len(json.load(sys.stdin).get('data',[])))" 2>/dev/null || echo "0")
if [ "$COUNT" -gt 0 ]; then
    pass "/health/recent-errors (필터 없음) 응답: ${COUNT}건"
else
    fail "/health/recent-errors 필터 없음" "결과 0건"
fi

# ─────────────────────────────────────────────────────
# Test 3: 정상 vm.start (test VM이 있다면) → result=ok
# ─────────────────────────────────────────────────────
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

# ─────────────────────────────────────────────────────
# Test 4: 거짓 'ok' 회귀 방지 — target=빈문자열로 vm.start 자동기록 없음
# ─────────────────────────────────────────────────────
echo ""
echo "[4] dispatcher 자동 audit 회귀 방지"
DISP_LIES=$(sudo sqlite3 /var/lib/purecvisor/pcv_audit.db \
  "SELECT COUNT(*) FROM audit_log WHERE method='vm.start' AND target='' AND ts > datetime('now','-1 minute');" 2>&1)
if [ "$DISP_LIES" = "0" ]; then
    pass "dispatcher 자동 audit 거짓 'ok' 미발생 (target='' count=0)"
else
    fail "dispatcher 자동 audit 회귀" "target='' 빈 vm.start 레코드 ${DISP_LIES}건 발견"
fi

# ─────────────────────────────────────────────────────
# 요약
# ─────────────────────────────────────────────────────
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
