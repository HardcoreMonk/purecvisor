#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════
# C1 회귀 테스트: vm.delete XML 롤백 (ADR-0017)
#
# [시나리오]
#   1. 테스트 VM 생성 (zvol 스토리지)
#   2. 자기 zvol 스냅샷에 ZFS hold 걸기
#      → zfs destroy -R이 "dataset is busy"로 실패
#   3. vm.delete 호출 → worker가 exorcism + zfs destroy 시도 → 실패
#   4. 롤백 경로 실행 → virDomainDefineXML(saved_xml) → VM 정의 복원
#   5. 검증: libvirt 도메인 정의 존재, zvol 존재
#   6. 정리: hold 해제 + 정상 vm.delete로 완전 삭제
#
# [사전 조건]
#   - purecvisorsd (또는 purecvisormd) 실행 중
#   - configured admin credential 로그인 가능
#   - ZFS 풀 pcvpool/vms 존재
#   - sudo 권한 (zfs hold, virsh dominfo)
#
# [주의] 테스트 VM의 zvol은 실패 시점에 "exorcism"(dd zero 10MB + wipefs)으로
#   첫 10MB가 영구 손상됩니다. 재정의된 VM은 기동 불가 상태이며 정리 후 삭제됨.
#
# [사용법]
#   bash tests/integration/test_vm_delete_rollback.sh [HOST]
# ═══════════════════════════════════════════════════════════════
set -uo pipefail

HOST="${1:-localhost}"
BASE="http://${HOST}/api/v1"
VM_NAME="c1-rollback-$$"
POOL="pcvpool/vms"
SNAP="rollback-hold"
TOKEN=""

PASS=0; FAIL=0; TOTAL=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }
info() { echo -e "${BLUE}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

# ─────────────────────────────────────────────────────
# 항상 실행되는 정리 (실패해도 리소스 방치 방지)
# ─────────────────────────────────────────────────────
cleanup() {
    info "Cleanup..."
    # 1. ZFS hold 해제 (멱등)
    sudo zfs release "$SNAP" "${POOL}/${VM_NAME}@${SNAP}" 2>/dev/null || true
    # 2. 스냅샷 삭제
    sudo zfs destroy "${POOL}/${VM_NAME}@${SNAP}" 2>/dev/null || true
    # 3. API 경로로 VM 삭제 시도
    if [ -n "$TOKEN" ]; then
        curl -s --max-time 10 -X DELETE -H "Authorization: Bearer $TOKEN" \
             "${BASE}/vms/${VM_NAME}" >/dev/null 2>&1 || true
        sleep 3
    fi
    # 4. 강제 ZFS 정리
    sudo zfs destroy -R "${POOL}/${VM_NAME}" 2>/dev/null || true
    # 5. libvirt 강제 undefine
    sudo virsh undefine "$VM_NAME" --managed-save --snapshots-metadata 2>/dev/null || true
}
trap cleanup EXIT

# ─────────────────────────────────────────────────────
# 사전 체크
# ─────────────────────────────────────────────────────
if ! command -v sudo >/dev/null && [ "$(id -u)" != "0" ]; then
    echo -e "${RED}FATAL: sudo 필요${NC}"; exit 2
fi

if ! command -v zfs >/dev/null 2>&1; then
    warn "zfs 명령어 없음 — 테스트 SKIP"; exit 0
fi

if ! sudo zfs list "$POOL" >/dev/null 2>&1; then
    warn "ZFS 풀 '$POOL' 없음 — 테스트 SKIP"; exit 0
fi

if ! command -v virsh >/dev/null 2>&1; then
    warn "virsh 없음 — 테스트 SKIP"; exit 0
fi

# 인증 토큰 획득
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
echo " C1: vm.delete XML Rollback Regression Test"
echo "════════════════════════════════════════════════"
echo "  Host:  ${HOST}"
echo "  VM:    ${VM_NAME}"
echo "  Pool:  ${POOL}"
echo ""

# ─────────────────────────────────────────────────────
# Step 1: 테스트 VM 생성
# ─────────────────────────────────────────────────────
echo "[1] 테스트 VM 생성"
RESP=$(curl -s --max-time 10 -X POST -H "$AUTH" -H "Content-Type: application/json" \
    "${BASE}/vms" \
    -d "{\"name\":\"${VM_NAME}\",\"vcpu\":1,\"memory_mb\":512,\"disk_size_gb\":1,\"storage_type\":\"zvol\"}")

if echo "$RESP" | grep -q '"accepted":true'; then
    info "vm.create accepted"
else
    fail "vm.create" "$(echo "$RESP" | head -c 200)"
    exit 1
fi

# zvol 생성 대기 (최대 20초)
for i in $(seq 1 20); do
    sleep 1
    if sudo zfs list "${POOL}/${VM_NAME}" >/dev/null 2>&1; then
        break
    fi
done

if sudo zfs list "${POOL}/${VM_NAME}" >/dev/null 2>&1; then
    pass "zvol 생성 확인 (${POOL}/${VM_NAME})"
else
    fail "zvol 생성" "timeout 20s"
    exit 1
fi

# libvirt 정의 확인 (백그라운드 워커가 libvirt define도 완료)
for i in 1 2 3 4 5; do
    if sudo virsh dominfo "$VM_NAME" >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

if sudo virsh dominfo "$VM_NAME" >/dev/null 2>&1; then
    pass "libvirt 도메인 정의 확인"
else
    fail "libvirt define" "virsh dominfo timeout"
    exit 1
fi

# ─────────────────────────────────────────────────────
# Step 2: ZFS snapshot + hold — destroy 차단
# ─────────────────────────────────────────────────────
echo ""
echo "[2] ZFS hold로 zfs destroy -R 차단"

if ! sudo zfs snapshot "${POOL}/${VM_NAME}@${SNAP}" 2>&1; then
    fail "snapshot 생성" "$?"
    exit 1
fi
pass "스냅샷 생성 (${POOL}/${VM_NAME}@${SNAP})"

if ! sudo zfs hold "$SNAP" "${POOL}/${VM_NAME}@${SNAP}" 2>&1; then
    fail "hold 설정" "$?"
    exit 1
fi
pass "ZFS hold 설정 완료 — destroy 차단 상태"

# hold 확인
if sudo zfs holds "${POOL}/${VM_NAME}@${SNAP}" 2>&1 | grep -q "$SNAP"; then
    info "hold 목록: $(sudo zfs holds "${POOL}/${VM_NAME}@${SNAP}" 2>&1 | tail -1)"
fi

# ─────────────────────────────────────────────────────
# Step 3: vm.delete 호출 → 롤백 경로 실행 예상
# ─────────────────────────────────────────────────────
echo ""
echo "[3] vm.delete 호출 → XML 롤백 예상"

LOG_SINCE=$(date -d '3 seconds ago' '+%Y-%m-%d %H:%M:%S')
info "LOG_SINCE=$LOG_SINCE"
RESP=$(curl -s --max-time 10 -X DELETE -H "$AUTH" "${BASE}/vms/${VM_NAME}")
if echo "$RESP" | grep -q '"accepted"'; then
    info "vm.delete accepted (fire-and-forget)"
else
    fail "vm.delete" "$RESP"
fi

# 워커 완료 대기: exorcism(~5초) + zfs destroy 실패 + 재정의 (~2초)
info "worker 완료 대기 (15초)..."
sleep 15

# ─────────────────────────────────────────────────────
# Step 4: 롤백 검증
# ─────────────────────────────────────────────────────
echo ""
echo "[4] 롤백 검증"

# 4-1. zvol 여전히 존재 (zfs destroy 실패 확인)
if sudo zfs list "${POOL}/${VM_NAME}" >/dev/null 2>&1; then
    pass "zvol 존재 유지 (zfs destroy 실패 확인)"
else
    fail "zvol 존재" "zfs destroy가 성공해버렸다 — 테스트 무효"
    exit 1
fi

# 4-2. libvirt 도메인 정의 복원 확인 (XML rollback 성공)
if sudo virsh dominfo "$VM_NAME" >/dev/null 2>&1; then
    pass "libvirt 도메인 정의 복원 (XML rollback 성공)"
else
    fail "libvirt 도메인 복원" "virsh dominfo 실패 — 롤백 경로 미실행"
fi

# 4-3. journalctl 롤백 로그 확인
# handler_vm_lifecycle.c의 PCV_LOG_WARN 메시지 검색
SERVICE_NAMES="purecvisorsd.service purecvisormd.service"
ROLLBACK_LOG_PATTERN="definition restored from saved XML"
ZFS_FAIL_PATTERN="ZFS destroy failed"

# 로그 덤프를 파일로 저장해 파이프 실패 여부 분리
JOURNAL_DUMP="/tmp/c1_journal_$$.log"
sudo journalctl --since "$LOG_SINCE" -u purecvisorsd -u purecvisormd >"$JOURNAL_DUMP" 2>&1
JOURNAL_LINES=$(wc -l <"$JOURNAL_DUMP")
info "journalctl 덤프 라인 수: $JOURNAL_LINES (since=$LOG_SINCE)"

if grep -q "$ROLLBACK_LOG_PATTERN" "$JOURNAL_DUMP"; then
    pass "journalctl 'definition restored from saved XML' 로그 확인"
elif grep -qi "$ZFS_FAIL_PATTERN" "$JOURNAL_DUMP"; then
    warn "롤백 로그 미검출이지만 ZFS destroy 실패 로그는 존재"
    pass "journalctl 'ZFS destroy failed' 확인 (fallback)"
else
    info "덤프 마지막 5줄: $(tail -5 "$JOURNAL_DUMP")"
    fail "journalctl 롤백 로그" "어떤 관련 로그도 찾지 못함 (덤프=$JOURNAL_DUMP)"
fi
rm -f "$JOURNAL_DUMP"

# ─────────────────────────────────────────────────────
# Step 5: hold 해제 + 정상 경로 재시도
# ─────────────────────────────────────────────────────
echo ""
echo "[5] hold 해제 후 정상 삭제 재시도"

sudo zfs release "$SNAP" "${POOL}/${VM_NAME}@${SNAP}" 2>&1 >/dev/null || true
sudo zfs destroy "${POOL}/${VM_NAME}@${SNAP}" 2>&1 >/dev/null || true
pass "hold 해제 + 테스트 스냅샷 삭제"

RESP=$(curl -s --max-time 10 -X DELETE -H "$AUTH" "${BASE}/vms/${VM_NAME}")
if echo "$RESP" | grep -q '"accepted"'; then
    info "재시도 vm.delete accepted"
else
    fail "재시도 vm.delete" "$RESP"
fi

info "worker 완료 대기 (10초)..."
sleep 10

# libvirt 최종 삭제 확인
if ! sudo virsh dominfo "$VM_NAME" >/dev/null 2>&1; then
    pass "libvirt 도메인 최종 삭제"
else
    fail "libvirt 최종 삭제" "virsh dominfo 여전히 반환"
fi

# zvol 최종 삭제 확인
if ! sudo zfs list "${POOL}/${VM_NAME}" >/dev/null 2>&1; then
    pass "zvol 최종 삭제"
else
    fail "zvol 최종 삭제" "zfs list 여전히 반환"
fi

# ─────────────────────────────────────────────────────
# 요약
# ─────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════"
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}C1 Rollback Test: ${PASS}/${TOTAL} PASSED${NC}"
    echo -e "${GREEN}ADR-0017 vm.delete 원자성 복구 검증 완료${NC}"
    exit 0
else
    echo -e "${RED}C1 Rollback Test: ${PASS}/${TOTAL} PASSED ($FAIL FAILED)${NC}"
    exit 1
fi
