#!/usr/bin/env bash








set -uo pipefail

HOST="${1:-localhost}"
CONCURRENCY="${2:-10}"
TOTAL="${3:-100}"
BASE="http://${HOST}/api/v1"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

echo "═══════════════════════════════════════════════"
echo -e "  ${CYAN}PureCVisor REST Load Test${NC}"
echo "  Host: ${HOST} | Concurrency: ${CONCURRENCY} | Total: ${TOTAL}"
echo "═══════════════════════════════════════════════"


TOKEN=$(curl -s --max-time 5 -X POST "${BASE}/auth/token" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" | \
  python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)

if [ -z "$TOKEN" ]; then
    echo -e "${RED}FATAL: 인증 실패${NC}"
    exit 1
fi

AUTH="Authorization: Bearer ${TOKEN}"
TMPDIR=$(mktemp -d)


echo ""
echo -e "${CYAN}[1/4] GET /health (인증 불필요)${NC}"
START=$(date +%s%3N)
SUCCESS=0; ERRORS=0
for i in $(seq 1 "$TOTAL"); do
    curl -s --max-time 3 -o /dev/null -w "%{http_code}" "${BASE}/health" > "$TMPDIR/r_$i" &

    if [ $((i % CONCURRENCY)) -eq 0 ]; then wait; fi
done
wait
END=$(date +%s%3N)
for f in "$TMPDIR"/r_*; do
    code=$(cat "$f" 2>/dev/null)
    [ "$code" = "200" ] && SUCCESS=$((SUCCESS+1)) || ERRORS=$((ERRORS+1))
done
DURATION=$((END - START))
RPS=$((TOTAL * 1000 / (DURATION > 0 ? DURATION : 1)))
echo -e "  ${GREEN}${SUCCESS}${NC}/${TOTAL} OK | ${RED}${ERRORS}${NC} ERR | ${DURATION}ms | ${CYAN}${RPS} req/s${NC}"
rm -f "$TMPDIR"/r_*


echo ""
echo -e "${CYAN}[2/4] GET /vms (JWT 인증)${NC}"
START=$(date +%s%3N)
SUCCESS=0; ERRORS=0
for i in $(seq 1 "$TOTAL"); do
    curl -s --max-time 3 -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/vms" > "$TMPDIR/r_$i" &
    if [ $((i % CONCURRENCY)) -eq 0 ]; then wait; fi
done
wait
END=$(date +%s%3N)
for f in "$TMPDIR"/r_*; do
    code=$(cat "$f" 2>/dev/null)
    [ "$code" = "200" ] && SUCCESS=$((SUCCESS+1)) || ERRORS=$((ERRORS+1))
done
DURATION=$((END - START))
RPS=$((TOTAL * 1000 / (DURATION > 0 ? DURATION : 1)))
echo -e "  ${GREEN}${SUCCESS}${NC}/${TOTAL} OK | ${RED}${ERRORS}${NC} ERR | ${DURATION}ms | ${CYAN}${RPS} req/s${NC}"
rm -f "$TMPDIR"/r_*


echo ""
echo -e "${CYAN}[3/4] 혼합 엔드포인트 (8종)${NC}"
ENDPOINTS=("/vms" "/networks" "/containers" "/storage/pools" "/health" "/alerts" "/iso" "/auth/users")
START=$(date +%s%3N)
SUCCESS=0; ERRORS=0; COUNT=0
for i in $(seq 1 "$TOTAL"); do
    ep_idx=$((i % 8))
    ep="${ENDPOINTS[$ep_idx]}"
    curl -s --max-time 3 -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}${ep}" > "$TMPDIR/r_$i" &
    COUNT=$((COUNT+1))
    if [ $((i % CONCURRENCY)) -eq 0 ]; then wait; fi
done
wait
END=$(date +%s%3N)
for f in "$TMPDIR"/r_*; do
    code=$(cat "$f" 2>/dev/null)
    [ "$code" = "200" ] && SUCCESS=$((SUCCESS+1)) || ERRORS=$((ERRORS+1))
done
DURATION=$((END - START))
RPS=$((COUNT * 1000 / (DURATION > 0 ? DURATION : 1)))
echo -e "  ${GREEN}${SUCCESS}${NC}/${COUNT} OK | ${RED}${ERRORS}${NC} ERR | ${DURATION}ms | ${CYAN}${RPS} req/s${NC}"
rm -f "$TMPDIR"/r_*


echo ""
echo -e "${CYAN}[4/4] CLOSE-WAIT 검증${NC}"
if [ "$HOST" = "localhost" ] || [ "$HOST" = "127.0.0.1" ]; then
    CW=$(ss -tnp state close-wait dst :80 2>/dev/null | wc -l)
    CW=$((CW - 1))
    [ "$CW" -le 0 ] && CW=0
    if [ "$CW" -eq 0 ]; then
        echo -e "  ${GREEN}CLOSE-WAIT: ${CW}건${NC}"
    else
        echo -e "  ${YELLOW}CLOSE-WAIT: ${CW}건${NC}"
    fi
else
    echo -e "  ${YELLOW}SKIP (원격 호스트 — 로컬에서만 검증 가능)${NC}"
fi


echo ""
echo "═══════════════════════════════════════════════"
echo -e "  부하 테스트 완료: ${TOTAL} × ${CONCURRENCY} 동시 요청"
echo "═══════════════════════════════════════════════"

rm -rf "$TMPDIR"
