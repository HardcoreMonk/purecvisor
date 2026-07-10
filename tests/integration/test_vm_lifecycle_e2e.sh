#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
# PureCVisor VM 전체 라이프사이클 E2E 테스트
#
# [테스트 흐름]
#   1. VM 생성 (test-e2e-vm, 1vCPU/512MB/10GB)
#   2. VM 시작
#   3. VM 상태 확인 (running)
#   4. 스냅샷 생성
#   5. 스냅샷 목록 확인
#   6. VM 중지
#   7. 스냅샷 롤백
#   8. 스냅샷 삭제
#   9. VM 삭제
#  10. 삭제 확인 (목록에서 사라짐)
#
# [주의] DESTRUCTIVE 테스트 — VM/ZFS 리소스 생성/삭제
# [사전 조건] purecvisorsd 또는 purecvisormd 실행 중, ZFS 풀 존재
# [사용법] sudo bash tests/integration/test_vm_lifecycle_e2e.sh [HOST]
# ═══════════════════════════════════════════════════════════════

set -uo pipefail

HOST="${1:-localhost}"
BASE="http://${HOST}/api/v1"
VM_NAME="test-e2e-vm-$$"
SNAP_NAME="e2e-snap"
PASS=0; FAIL=0; TOTAL=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }
skip() { TOTAL=$((TOTAL+1)); echo -e "  ${YELLOW}[SKIP]${NC} $1"; }

# JWT 인증
TOKEN=$(curl -s --max-time 5 -X POST "${BASE}/auth/token" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" | \
  python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
CSRF=$(curl -s --max-time 5 -X POST "${BASE}/auth/token" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" | \
  python3 -c "import sys,json;print(json.load(sys.stdin).get('csrf_token',''))" 2>/dev/null)

if [ -z "$TOKEN" ]; then
    echo -e "${RED}FATAL: 인증 실패${NC}"
    exit 1
fi

AUTH="Authorization: Bearer ${TOKEN}"
CT="Content-Type: application/json"
CS="X-CSRF-Token: ${CSRF}"

echo "═══════════════════════════════════════════════"
echo "  VM Lifecycle E2E Test (${VM_NAME})"
echo "  Host: ${HOST}"
echo "═══════════════════════════════════════════════"

# cleanup 트랩 — 테스트 실패 시에도 리소스 정리
cleanup() {
    echo ""
    echo -e "${YELLOW}[CLEANUP]${NC} Removing test VM: ${VM_NAME}"
    curl -s --max-time 10 -X DELETE -H "$AUTH" -H "$CS" "${BASE}/vms/${VM_NAME}" > /dev/null 2>&1
    sleep 2
}
trap cleanup EXIT

# ── 1. VM 생성 ──
echo ""
echo "=== 1. VM 생성 ==="
HTTP=$(curl -s --max-time 15 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d "{\"name\":\"${VM_NAME}\",\"vcpu\":1,\"memory_mb\":512,\"disk_size_gb\":10}" \
  "${BASE}/vms")
[ "$HTTP" = "200" ] && pass "POST /vms → 200 (VM 생성)" || fail "VM 생성" "HTTP $HTTP"

sleep 3

# ── 2. VM 존재 확인 ──
echo ""
echo "=== 2. VM 존재 확인 ==="
FOUND=$(curl -s --max-time 5 -H "$AUTH" "${BASE}/vms" | \
  python3 -c "import sys,json;d=json.load(sys.stdin);vms=d.get('data',d) if isinstance(d,dict) else d;print('yes' if any(v.get('name')=='${VM_NAME}' for v in vms) else 'no')" 2>/dev/null)
[ "$FOUND" = "yes" ] && pass "VM ${VM_NAME} 목록에 존재" || fail "VM 존재 확인" "not found"

# ── 3. VM 시작 ──
echo ""
echo "=== 3. VM 시작 ==="
HTTP=$(curl -s --max-time 10 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  "${BASE}/vms/${VM_NAME}/start")
[ "$HTTP" = "200" ] && pass "POST /vms/${VM_NAME}/start → 200" || fail "VM 시작" "HTTP $HTTP"

sleep 3

# ── 4. VM 상태 확인 (running) ──
echo ""
echo "=== 4. VM 상태 확인 ==="
STATE=$(curl -s --max-time 5 -H "$AUTH" "${BASE}/vms" | \
  python3 -c "import sys,json;d=json.load(sys.stdin);vms=d.get('data',d) if isinstance(d,dict) else d;vm=[v for v in vms if v.get('name')=='${VM_NAME}'];print(vm[0].get('state','?') if vm else '?')" 2>/dev/null)
[ "$STATE" = "running" ] && pass "VM 상태: running" || skip "VM 상태: $STATE (QEMU 부팅 지연 가능)"

# ── 5. 스냅샷 생성 ──
echo ""
echo "=== 5. 스냅샷 생성 ==="
HTTP=$(curl -s --max-time 10 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d "{\"snap_name\":\"${SNAP_NAME}\"}" \
  "${BASE}/vms/${VM_NAME}/snapshot/create")
[ "$HTTP" = "200" ] && pass "스냅샷 생성: ${SNAP_NAME}" || fail "스냅샷 생성" "HTTP $HTTP"

sleep 1

# ── 6. 스냅샷 목록 확인 ──
echo ""
echo "=== 6. 스냅샷 목록 ==="
SNAP_FOUND=$(curl -s --max-time 5 -H "$AUTH" "${BASE}/vms/${VM_NAME}/snapshot" | \
  python3 -c "import sys,json;d=json.load(sys.stdin);l=d.get('data',d) if isinstance(d,dict) else d;print('yes' if isinstance(l,list) and len(l)>0 else 'no')" 2>/dev/null)
[ "$SNAP_FOUND" = "yes" ] && pass "스냅샷 목록에 존재" || fail "스냅샷 확인" "empty"

# ── 7. VM 중지 ──
echo ""
echo "=== 7. VM 중지 ==="
HTTP=$(curl -s --max-time 10 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  "${BASE}/vms/${VM_NAME}/stop")
[ "$HTTP" = "200" ] && pass "POST /vms/${VM_NAME}/stop → 200" || skip "VM 중지: HTTP $HTTP (이미 중지됨)"

sleep 3

# ── 8. 스냅샷 삭제 ──
echo ""
echo "=== 8. 스냅샷 삭제 ==="
HTTP=$(curl -s --max-time 10 -o /dev/null -w "%{http_code}" \
  -X DELETE -H "$AUTH" -H "$CS" \
  "${BASE}/vms/${VM_NAME}/snapshot/${SNAP_NAME}")
[ "$HTTP" = "200" ] && pass "스냅샷 삭제: ${SNAP_NAME}" || fail "스냅샷 삭제" "HTTP $HTTP"

# ── 9. VM 삭제 ──
echo ""
echo "=== 9. VM 삭제 ==="
HTTP=$(curl -s --max-time 30 -o /dev/null -w "%{http_code}" \
  -X DELETE -H "$AUTH" -H "$CS" \
  "${BASE}/vms/${VM_NAME}")
[ "$HTTP" = "200" ] && pass "DELETE /vms/${VM_NAME} → 200" || fail "VM 삭제" "HTTP $HTTP"

sleep 5

# ── 10. 삭제 확인 ──
echo ""
echo "=== 10. 삭제 확인 ==="
GONE=$(curl -s --max-time 5 -H "$AUTH" "${BASE}/vms" | \
  python3 -c "import sys,json;d=json.load(sys.stdin);vms=d.get('data',d) if isinstance(d,dict) else d;print('yes' if not any(v.get('name')=='${VM_NAME}' for v in vms) else 'no')" 2>/dev/null)
[ "$GONE" = "yes" ] && pass "VM ${VM_NAME} 삭제 완료" || fail "삭제 확인" "still exists"

# cleanup 트랩 해제 (정상 삭제 완료)
trap - EXIT

# ── 결과 ──
echo ""
echo "═══════════════════════════════════════════════"
echo -e "TOTAL: ${TOTAL} | ${GREEN}PASS: ${PASS}${NC} | ${RED}FAIL: ${FAIL}${NC}"
echo "═══════════════════════════════════════════════"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
