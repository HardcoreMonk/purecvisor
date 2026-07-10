#!/usr/bin/env bash
# =============================================================================
# Phase 4 OVS-DPDK / SR-IOV 통합 테스트
# =============================================================================
# 실행: sudo bash tests/integration/test_phase4_accel.sh [NODE_IP]
#
# 테스트 시나리오:
#   1. DPDK graceful degradation (dpdk-init 미활성 환경)
#   2. SR-IOV graceful degradation (미지원 NIC 환경)
#   3. RPC 응답 구조 검증
#   4. 멱등 동작 검증
#   5. PCI 주소 검증 (잘못된 입력 거부)
#   6. 3노드 일관성 확인
# =============================================================================
set -uo pipefail
trap '' PIPE   # SIGPIPE 무시 — UDS 서버가 응답 후 즉시 close

SOCK="/var/run/purecvisor/daemon.sock"
PASS=0; FAIL=0; TOTAL=0
SSH_CMD="/usr/bin/ssh"
NODES=("192.0.2.19" "192.0.2.20" "192.0.2.21")
NODE_NAMES=("Node1" "Node2" "Node3")

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

rpc() {
    local method="$1" params="${2-"{}"}"
    ( set +o pipefail
      echo "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":\"t$$\"}" \
          | nc -N -U "$SOCK" 2>/dev/null
    )
}

rpc_remote() {
    local ip="$1" method="$2" params="${3:-{}}"
    $SSH_CMD pcvdev@"$ip" "echo '{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$params,\"id\":\"t1\"}' | sudo nc -N -U $SOCK 2>/dev/null" 2>/dev/null
}

assert_contains() {
    local label="$1" haystack="$2" needle="$3"
    TOTAL=$((TOTAL + 1))
    if echo "$haystack" | grep -q "$needle" 2>/dev/null; then
        PASS=$((PASS + 1))
        echo -e "  ${GREEN}PASS${NC} $label"
    else
        FAIL=$((FAIL + 1))
        echo -e "  ${RED}FAIL${NC} $label — expected '$needle' in response"
        echo "       got: $(echo "$haystack" | head -c 200)"
    fi
}

assert_not_error() {
    local label="$1" resp="$2"
    TOTAL=$((TOTAL + 1))
    if echo "$resp" | grep -q '"error"' 2>/dev/null; then
        FAIL=$((FAIL + 1))
        echo -e "  ${RED}FAIL${NC} $label — unexpected error"
        echo "       got: $(echo "$resp" | head -c 200)"
    else
        PASS=$((PASS + 1))
        echo -e "  ${GREEN}PASS${NC} $label"
    fi
}

assert_error() {
    local label="$1" resp="$2"
    TOTAL=$((TOTAL + 1))
    if echo "$resp" | grep -q '"error"' 2>/dev/null; then
        PASS=$((PASS + 1))
        echo -e "  ${GREEN}PASS${NC} $label (expected error)"
    else
        FAIL=$((FAIL + 1))
        echo -e "  ${RED}FAIL${NC} $label — expected error but got success"
        echo "       got: $(echo "$resp" | head -c 200)"
    fi
}

echo ""
echo "============================================"
echo " Phase 4 OVS-DPDK / SR-IOV Integration Test"
echo "============================================"
echo ""

# ── 1. DPDK Status (graceful degradation) ──
echo -e "${YELLOW}[1/6] DPDK Status — Graceful Degradation${NC}"
resp=$(rpc "dpdk.status")
assert_contains "dpdk.status has 'available'" "$resp" '"available"'
assert_contains "dpdk.status has 'vdev_count'" "$resp" '"vdev_count"'
assert_not_error "dpdk.status returns success" "$resp"

# ── 2. DPDK Hugepage Info ──
echo -e "${YELLOW}[2/6] DPDK Hugepage Info${NC}"
resp=$(rpc "dpdk.hugepage.info")
assert_contains "hugepage has 'total_mb'" "$resp" '"total_mb"'
assert_contains "hugepage has 'free_mb'" "$resp" '"free_mb"'
assert_contains "hugepage has '1g_total'" "$resp" '"hugepage_1g_total"'
assert_contains "hugepage has '2m_total'" "$resp" '"hugepage_2m_total"'
assert_not_error "dpdk.hugepage.info returns success" "$resp"

# ── 3. DPDK List (빈 배열 OK) ──
echo -e "${YELLOW}[3/6] DPDK List + SR-IOV List${NC}"
resp=$(rpc "dpdk.list")
assert_not_error "dpdk.list returns success" "$resp"

resp=$(rpc "sriov.list")
assert_not_error "sriov.list returns success" "$resp"

resp=$(rpc 'sriov.list' '{"pf":"nonexist99"}')
assert_not_error "sriov.list nonexist PF returns empty" "$resp"

# ── 4. SR-IOV Status ──
echo -e "${YELLOW}[4/6] SR-IOV Status${NC}"
resp=$(rpc "sriov.status")
assert_contains "sriov.status has 'available'" "$resp" '"available"'
assert_contains "sriov.status has 'physical_functions'" "$resp" '"physical_functions"'
assert_not_error "sriov.status returns success" "$resp"

# ── 5. 입력 검증 + 멱등 동작 ──
echo -e "${YELLOW}[5/6] Input Validation + Idempotent Operations${NC}"

# PCI 주소 검증 — 잘못된 입력
resp=$(rpc 'dpdk.bind' '{"pci_addr":"../../etc/passwd"}')
assert_error "dpdk.bind path traversal rejected" "$resp"

resp=$(rpc 'dpdk.bind' '{"pci_addr":"01:00.0"}')
assert_error "dpdk.bind short PCI rejected" "$resp"

resp=$(rpc 'dpdk.bind' '{}')
assert_error "dpdk.bind missing pci_addr rejected" "$resp"

# 멱등 삭제
resp=$(rpc 'dpdk.bridge.delete' '{"name":"nonexist-br-test"}')
assert_not_error "dpdk.bridge.delete idempotent" "$resp"

# SR-IOV 멱등
resp=$(rpc 'sriov.disable' '{"pf":"nonexist99"}')
assert_not_error "sriov.disable idempotent" "$resp"

# SR-IOV enable — 미지원 PF
resp=$(rpc 'sriov.enable' '{"pf":"nonexist99","num_vfs":1}')
assert_error "sriov.enable nonexist PF fails" "$resp"

# 필수 파라미터 누락
resp=$(rpc 'sriov.enable' '{}')
assert_error "sriov.enable missing pf rejected" "$resp"

resp=$(rpc 'sriov.attach' '{"vm_name":"test"}')
assert_error "sriov.attach missing pf rejected" "$resp"

resp=$(rpc 'sriov.detach' '{"vm_name":"test"}')
assert_error "sriov.detach missing pci_addr rejected" "$resp"

resp=$(rpc 'dpdk.bridge.create' '{}')
assert_error "dpdk.bridge.create missing name rejected" "$resp"

# DPDK unbind — 존재하지 않는 PCI (멱등)
resp=$(rpc 'dpdk.unbind' '{"pci_addr":"0000:ff:1f.7"}')
assert_not_error "dpdk.unbind nonexist PCI idempotent" "$resp"

# ── 6. 3노드 일관성 확인 ──
echo -e "${YELLOW}[6/6] 3-Node Consistency${NC}"
# Node1 (로컬)
resp=$(rpc "dpdk.status")
assert_contains "Node1 dpdk.status has 'available'" "$resp" '"available"'
resp=$(rpc "sriov.status")
assert_contains "Node1 sriov.status has 'available'" "$resp" '"available"'
# Node2, Node3 (원격)
for i in 1 2; do
    ip="${NODES[$i]}"
    name="${NODE_NAMES[$i]}"
    resp=$(rpc_remote "$ip" "dpdk.status")
    assert_contains "$name dpdk.status has 'available'" "$resp" '"available"'
    resp=$(rpc_remote "$ip" "sriov.status")
    assert_contains "$name sriov.status has 'available'" "$resp" '"available"'
done

# ── 결과 요약 ──
echo ""
echo "============================================"
echo -e " Results: ${GREEN}${PASS} passed${NC} / ${RED}${FAIL} failed${NC} / ${TOTAL} total"
echo "============================================"
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
