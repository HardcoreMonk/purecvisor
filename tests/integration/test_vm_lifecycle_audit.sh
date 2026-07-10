#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
# PureCVisor VM 라이프사이클 감사 회귀 테스트 (C2/C3/C5)
#
# 커밋 94e394a(18건 수정) + 후속 A1/A3에서 수정된 라이프사이클 결함에
# 대한 회귀 테스트. C1(vm.delete 롤백)은 ZFS destroy 실패 주입이 복잡해
# 별도 단위 테스트로 다룬다. C4(_lock_expiry_for_op)는 test_vm_state.c에
# 단위 테스트로 구현.
#
# [커버 범위]
#   W2/C2: vm.delete idempotent success (존재하지 않는 VM 삭제 → accepted)
#   W3/C3: vm.start on already-running (running VM에 재시작 → 성공)
#   W5/C5: vm.create 수치 파라미터 범위 검증 (vcpu/mem/disk)
#
# [사용법]
#   bash tests/integration/test_vm_lifecycle_audit.sh [HOST]
# ═══════════════════════════════════════════════════════════════
set -uo pipefail

HOST="${1:-localhost}"
BASE="http://${HOST}/api/v1"
PASS=0; FAIL=0; TOTAL=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }

TOKEN=$(curl -s --max-time 5 -X POST "${BASE}/auth/token" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" | \
  python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)

if [ -z "$TOKEN" ]; then
    echo -e "${RED}FATAL: 인증 실패 (${BASE})${NC}"
    exit 1
fi
AUTH="Authorization: Bearer ${TOKEN}"

echo ""
echo "════════════════════════════════════════════════"
echo " VM Lifecycle Audit Regression Test"
echo " Host: ${HOST}"
echo "════════════════════════════════════════════════"

# ─────────────────────────────────────────────────────
# W5/C5: vm.create 수치 파라미터 범위 검증
# ─────────────────────────────────────────────────────
echo ""
echo "[C5] vm.create 수치 파라미터 범위 검증"
echo "─────────────────────────────────────────"

assert_invalid_param() {
    local label="$1"
    local body="$2"
    local expected_field="$3"
    local resp=$(curl -s --max-time 5 -X POST -H "$AUTH" -H "Content-Type: application/json" \
        "${BASE}/vms" -d "$body")
    if echo "$resp" | grep -q '"code":-32602'; then
        if echo "$resp" | grep -q "$expected_field"; then
            pass "$label"
        else
            fail "$label" "wrong field in error: $(echo "$resp" | head -c 200)"
        fi
    else
        fail "$label" "expected -32602, got: $(echo "$resp" | head -c 200)"
    fi
}

assert_invalid_param "vcpu=0 거부" \
    '{"name":"audit-t1","vcpu":0,"memory_mb":512}' \
    "vcpu"

assert_invalid_param "vcpu=257 거부" \
    '{"name":"audit-t2","vcpu":257,"memory_mb":512}' \
    "vcpu"

assert_invalid_param "memory_mb=50 거부" \
    '{"name":"audit-t3","vcpu":1,"memory_mb":50}' \
    "memory"

assert_invalid_param "memory_mb=2000000 거부" \
    '{"name":"audit-t4","vcpu":1,"memory_mb":2000000}' \
    "memory"

assert_invalid_param "disk_size_gb=999999 거부" \
    '{"name":"audit-t5","vcpu":1,"memory_mb":512,"disk_size_gb":999999}' \
    "disk"

assert_invalid_param "vlan_id=5000 거부" \
    '{"name":"audit-t6","vcpu":1,"memory_mb":512,"vlan_id":5000}' \
    "vlan"

# ─────────────────────────────────────────────────────
# W2/C2: vm.delete idempotent 성공 (존재하지 않는 VM)
# ─────────────────────────────────────────────────────
echo ""
echo "[C2] vm.delete 멱등성 (존재하지 않는 VM)"
echo "─────────────────────────────────────────"

# 다소 무작위성 있는 이름으로 충돌 회피
MISSING_VM="audit-missing-$$-$(date +%s)"
RESP=$(curl -s --max-time 10 -X DELETE -H "$AUTH" \
    "${BASE}/vms/${MISSING_VM}")

if echo "$RESP" | grep -q '"accepted"'; then
    pass "존재하지 않는 VM 삭제 → accepted (idempotent)"
else
    fail "존재하지 않는 VM 삭제" "$RESP"
fi

# 2회 연속 호출 — 동일 결과
RESP2=$(curl -s --max-time 10 -X DELETE -H "$AUTH" \
    "${BASE}/vms/${MISSING_VM}")
if echo "$RESP2" | grep -q '"accepted"'; then
    pass "연속 2회 호출 → 동일한 accepted"
else
    fail "연속 2회 호출" "$RESP2"
fi

# ─────────────────────────────────────────────────────
# W3/C3: vm.start on already-running
# ─────────────────────────────────────────────────────
echo ""
echo "[C3] vm.start on already-running (best-effort)"
echo "─────────────────────────────────────────"

# 기존 running VM 존재 시 테스트, 없으면 skip
RUNNING_VM=$(curl -s --max-time 5 -H "$AUTH" "${BASE}/vms" | \
    python3 -c "
import sys, json
try:
    r = json.load(sys.stdin)
    vms = r if isinstance(r, list) else r.get('data', [])
    for v in vms:
        if v.get('state') == 'running':
            print(v.get('name',''))
            break
except: pass
" 2>/dev/null)

if [ -n "$RUNNING_VM" ]; then
    RESP=$(curl -s --max-time 10 -X POST -H "$AUTH" "${BASE}/vms/${RUNNING_VM}/start")
    if echo "$RESP" | grep -qE '"accepted"|"ok"|"data"'; then
        pass "running VM에 재start → 성공 응답 (${RUNNING_VM})"
    else
        fail "running VM에 재start" "$RESP"
    fi
else
    echo -e "  ${YELLOW}[SKIP]${NC} running VM 없음 — 수동 검증 필요"
fi

# ─────────────────────────────────────────────────────
# 요약
# ─────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════"
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}Result: ${PASS}/${TOTAL} PASSED${NC}"
    exit 0
else
    echo -e "${RED}Result: ${PASS}/${TOTAL} PASSED ($FAIL FAILED)${NC}"
    exit 1
fi
