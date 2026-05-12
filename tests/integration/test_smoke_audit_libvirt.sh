#!/usr/bin/env bash

















set -uo pipefail

HOST="${1:-localhost}"
BASE="http://${HOST}/api/v1"
DB="/var/lib/purecvisor/pcv_audit.db"
PASS=0; FAIL=0; TOTAL=0
TOKEN=""

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
info() { echo -e "${BLUE}[INFO]${NC} $1"; }


if ! sudo test -r "$DB"; then
    echo -e "${YELLOW}SKIP: audit DB ($DB) 미존재${NC}"; exit 0
fi
if ! command -v virsh >/dev/null 2>&1; then
    echo -e "${YELLOW}SKIP: virsh 미존재${NC}"; exit 0
fi

TOKEN=$(curl -s --max-time 5 -X POST "${BASE}/auth/token" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" | \
  python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
[ -z "$TOKEN" ] && { echo -e "${RED}FATAL: 인증 실패${NC}"; exit 1; }

echo ""
echo "════════════════════════════════════════════════"
echo " B3 Smoke: audit DB ↔ libvirt 정합성 (ADR-0018)"
echo "════════════════════════════════════════════════"
echo ""





echo "[1] vm.start 거짓 'ok' 회귀 (최근 5분)"
LIES=$(sudo sqlite3 "$DB" \
  "SELECT COUNT(*) FROM audit_log WHERE method='vm.start' AND target='' AND ts > datetime('now','-5 minutes');")
if [ "$LIES" = "0" ]; then
    pass "최근 5분 vm.start target='' (거짓 dispatcher 자동기록) 0건"
else
    fail "vm.start dispatcher 자동 audit 회귀" "${LIES}건 발견"
fi




echo ""
echo "[2] 최근 vm.start result=ok ↔ libvirt 정의 일치성"
OK_VMS=$(sudo sqlite3 "$DB" \
  "SELECT DISTINCT target FROM audit_log WHERE method='vm.start' AND result='ok' AND target!='' AND ts > datetime('now','-1 hour');" 2>/dev/null || true)
if [ -z "$OK_VMS" ]; then
    info "최근 1시간 vm.start ok 레코드 없음 — Test 2 SKIP"
else
    MISMATCH=0
    for VM in $OK_VMS; do
        if ! sudo virsh dominfo "$VM" >/dev/null 2>&1; then
            warn "audit ok 였지만 libvirt에 미정의: $VM"
            MISMATCH=$((MISMATCH+1))
        fi
    done
    if [ $MISMATCH -eq 0 ]; then
        pass "vm.start ok 기록의 모든 VM이 libvirt에 정의됨"
    else
        fail "audit ↔ libvirt 정합성" "${MISMATCH}개 VM 불일치"
    fi
fi




echo ""
echo "[3] vm.start result=fail ↔ libvirt 상태 정합성"
FAIL_VMS=$(sudo sqlite3 "$DB" \
  "SELECT DISTINCT target FROM audit_log WHERE method='vm.start' AND result='fail' AND target!='' AND ts > datetime('now','-1 hour');" 2>/dev/null || true)
if [ -z "$FAIL_VMS" ]; then
    info "최근 1시간 vm.start fail 레코드 없음 — Test 3 SKIP"
else
    UNEXPECTED=0
    for VM in $FAIL_VMS; do

        if STATE=$(sudo virsh domstate "$VM" 2>/dev/null) && [ "$STATE" = "running" ]; then

            LATER_OK=$(sudo sqlite3 "$DB" "SELECT COUNT(*) FROM audit_log WHERE method='vm.start' AND target='$VM' AND result='ok' AND ts > (SELECT MAX(ts) FROM audit_log WHERE method='vm.start' AND target='$VM' AND result='fail');")
            if [ "$LATER_OK" = "0" ]; then
                warn "fail 기록 후 ok 없는데 running: $VM"
                UNEXPECTED=$((UNEXPECTED+1))
            fi
        fi
    done
    if [ $UNEXPECTED -eq 0 ]; then
        pass "vm.start fail 기록의 VM 상태 일관됨"
    else
        warn "${UNEXPECTED}개 모호한 케이스 — 수동 검토 필요"

    fi
fi




echo ""
echo "[4] vm.start E2E — 정상 호출 → 즉시 audit 기록"
EXISTING_VM=$(curl -s --max-time 5 -H "Authorization: Bearer $TOKEN" "${BASE}/vms" | \
  python3 -c "import sys,json;d=json.load(sys.stdin).get('data',[]);print((d[0]['name'] if d else ''))" 2>/dev/null)
if [ -z "$EXISTING_VM" ]; then
    info "기존 VM 없음 — Test 4 SKIP"
else
    BEFORE=$(sudo sqlite3 "$DB" "SELECT COUNT(*) FROM audit_log WHERE method='vm.start' AND target='$EXISTING_VM';")
    curl -s --max-time 5 -X POST -H "Authorization: Bearer $TOKEN" "${BASE}/vms/${EXISTING_VM}/start" -d '{}' >/dev/null
    sleep 3
    AFTER=$(sudo sqlite3 "$DB" "SELECT COUNT(*) FROM audit_log WHERE method='vm.start' AND target='$EXISTING_VM';")
    if [ "$AFTER" -gt "$BEFORE" ]; then
        pass "vm.start 호출 후 audit 신규 레코드 1+ 추가 (BEFORE=$BEFORE AFTER=$AFTER)"
    else
        fail "vm.start audit 기록" "신규 레코드 없음"
    fi
fi




echo ""
echo "════════════════════════════════════════════════"
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}B3 Smoke: ${PASS}/${TOTAL} PASSED${NC}"
    echo -e "${GREEN}audit DB ↔ libvirt 정합성 OK${NC}"
    exit 0
else
    echo -e "${RED}B3 Smoke: ${PASS}/${TOTAL} ($FAIL FAILED)${NC}"
    exit 1
fi
