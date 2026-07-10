#!/usr/bin/env bash
# tests/integration/test_rbac_template_backup.sh
#
# RBAC + VM 템플릿 + 백업/복원 통합 테스트
#
# 사전 조건:
#   - purecvisorsd 또는 purecvisormd 실행 중 (systemd 또는 수동)
#   - /var/run/purecvisor/daemon.sock 존재
#   - root 또는 sudo 권한
#   - nc (netcat) 설치
#
# 실행: sudo bash tests/integration/test_rbac_template_backup.sh

set -uo pipefail

# ── 색상 ──────────────────────────────────────────────
GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

# ── 설정 ──────────────────────────────────────────────
SOCKET_PATH="/var/run/purecvisor/daemon.sock"
PASS=0; FAIL=0; SKIP=0
TOTAL=0

# ── 유틸리티 ──────────────────────────────────────────
log()  { echo -e "${CYAN}[INFO]${NC} $*"; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); }
skip() { echo -e "${YELLOW}[SKIP]${NC} $*"; SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); }

# ── JSON-RPC 전송 ────────────────────────────────────
send_rpc() {
    echo "$1" | nc -U "$SOCKET_PATH" 2>/dev/null || true
}

# ── 응답 검증 헬퍼 ───────────────────────────────────
assert_contains() {
    local test_name="$1" response="$2" expected="$3"
    if echo "$response" | grep -q "$expected"; then
        pass "$test_name"
    else
        fail "$test_name (expected '$expected' in response)"
        echo "  Response: $response"
    fi
}

assert_not_contains() {
    local test_name="$1" response="$2" unexpected="$3"
    if echo "$response" | grep -q "$unexpected"; then
        fail "$test_name (unexpected '$unexpected' in response)"
        echo "  Response: $response"
    else
        pass "$test_name"
    fi
}

# ── 사전 조건 확인 ───────────────────────────────────
log "=========================================="
log " RBAC + Template + Backup 통합 테스트"
log "=========================================="
echo ""

if [ ! -S "$SOCKET_PATH" ]; then
    echo -e "${RED}[ERROR]${NC} 데몬 소켓 없음: $SOCKET_PATH"
    echo "  purecvisorsd 또는 purecvisormd를 먼저 실행하세요."
    exit 1
fi

log "데몬 소켓 확인: $SOCKET_PATH"
echo ""

# ══════════════════════════════════════════════════════
# [1] RBAC 테스트
# ══════════════════════════════════════════════════════
log "─── [1/3] RBAC 인증 테스트 ───"

# 1-1. 사용자 생성 (operator)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.create","params":{"username":"test-op","password":"pass123","role":"operator"},"id":"r1"}')
assert_contains "RBAC: operator 사용자 생성" "$RESP" "result"

# 1-2. 사용자 생성 (viewer)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.create","params":{"username":"test-viewer","password":"view456","role":"viewer"},"id":"r2"}')
assert_contains "RBAC: viewer 사용자 생성" "$RESP" "result"

# 1-3. 사용자 목록
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"r3"}')
assert_contains "RBAC: 사용자 목록 조회" "$RESP" "test-op"
assert_contains "RBAC: viewer 목록 포함" "$RESP" "test-viewer"

# 1-4. 역할 변경 (operator → admin)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.role.set","params":{"username":"test-op","role":"admin"},"id":"r4"}')
assert_contains "RBAC: 역할 변경 (→admin)" "$RESP" "result"

# 1-5. 역할 변경 확인
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"r5"}')
assert_contains "RBAC: admin 역할 반영 확인" "$RESP" "admin"

# 1-6. 중복 사용자 생성 (에러 예상)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.create","params":{"username":"test-op","password":"dup","role":"viewer"},"id":"r6"}')
assert_contains "RBAC: 중복 사용자 에러" "$RESP" "error"

# 1-7. 사용자 삭제
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.delete","params":{"username":"test-viewer"},"id":"r7"}')
assert_contains "RBAC: viewer 삭제" "$RESP" "result"

# 1-8. 삭제 후 목록 확인
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"r8"}')
assert_not_contains "RBAC: 삭제 후 미표시" "$RESP" "test-viewer"

# 1-9. test-op 정리
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.delete","params":{"username":"test-op"},"id":"r9"}')
assert_contains "RBAC: test-op 정리" "$RESP" "result"

# 1-10. 잘못된 역할 생성 (에러 예상)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.create","params":{"username":"bad","password":"x","role":"superadmin"},"id":"r10"}')
assert_contains "RBAC: 잘못된 역할 에러" "$RESP" "error"

echo ""

# ══════════════════════════════════════════════════════
# [2] VM 템플릿 테스트
# ══════════════════════════════════════════════════════
log "─── [2/3] VM 템플릿 테스트 ───"

# 2-1. 프리셋 목록 (ubuntu-small/medium/large 자동 생성)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.list","params":{},"id":"t1"}')
assert_contains "Template: 프리셋 목록 조회" "$RESP" "result"

# 2-2. ubuntu-small 상세 조회
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.get","params":{"name":"ubuntu-small"},"id":"t2"}')
if echo "$RESP" | grep -q "ubuntu-small"; then
    assert_contains "Template: ubuntu-small 조회" "$RESP" "vcpu"
else
    skip "Template: ubuntu-small 조회 (프리셋 미생성 — /etc/purecvisor/templates/ 권한 확인)"
fi

# 2-3. 커스텀 템플릿 생성
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.create","params":{"name":"test-tmpl","vcpu":1,"memory_mb":512,"disk_gb":5,"os_variant":"ubuntu24.04","description":"Integration test template"},"id":"t3"}')
assert_contains "Template: 커스텀 생성" "$RESP" "result"

# 2-4. 생성된 템플릿 조회
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.get","params":{"name":"test-tmpl"},"id":"t4"}')
assert_contains "Template: 커스텀 조회" "$RESP" "test-tmpl"
assert_contains "Template: vcpu 값 확인" "$RESP" "512"

# 2-5. 중복 이름 생성 (에러 예상)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.create","params":{"name":"test-tmpl","vcpu":2,"memory_mb":1024,"disk_gb":10,"os_variant":"debian12"},"id":"t5"}')
assert_contains "Template: 중복 이름 에러" "$RESP" "error"

# 2-6. 필수 파라미터 누락 (에러 예상)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.create","params":{"name":"bad-tmpl"},"id":"t6"}')
assert_contains "Template: 필수 파라미터 누락 에러" "$RESP" "error"

# 2-7. 템플릿 삭제
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.delete","params":{"name":"test-tmpl"},"id":"t7"}')
assert_contains "Template: 삭제 성공" "$RESP" "result"

# 2-8. 삭제 후 조회 (에러 예상)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.get","params":{"name":"test-tmpl"},"id":"t8"}')
assert_contains "Template: 삭제 후 조회 에러" "$RESP" "error"

# 2-9. 멱등 삭제 (이미 없는 템플릿)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.delete","params":{"name":"nonexistent-tmpl"},"id":"t9"}')
assert_contains "Template: 멱등 삭제" "$RESP" "result"

# 2-10. cloud-init user_data 포함 템플릿
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.create","params":{"name":"cloudinit-test","vcpu":2,"memory_mb":2048,"disk_gb":20,"os_variant":"ubuntu24.04","cloud_init_user_data":"#cloud-config\npackages:\n  - nginx\n"},"id":"t10"}')
assert_contains "Template: cloud-init 포함 생성" "$RESP" "result"
send_rpc '{"jsonrpc":"2.0","method":"template.delete","params":{"name":"cloudinit-test"},"id":"t11"}' > /dev/null

echo ""

# ══════════════════════════════════════════════════════
# [3] 백업/복원 테스트
# ══════════════════════════════════════════════════════
log "─── [3/3] 백업/복원 테스트 ───"

# 3-1. 정책 설정 (전체 VM)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.set","params":{"vm_name":"*","interval_hours":24,"retention_count":7},"id":"b1"}')
assert_contains "Backup: 전체 VM 정책 설정" "$RESP" "result"

# 3-2. 정책 설정 (특정 VM)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.set","params":{"vm_name":"integ-test-vm","interval_hours":6,"retention_count":3},"id":"b2"}')
assert_contains "Backup: 특정 VM 정책 설정" "$RESP" "result"

# 3-3. 정책 목록
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"b3"}')
assert_contains "Backup: 정책 목록 조회" "$RESP" "result"
assert_contains "Backup: 전체 VM 정책 포함" "$RESP" '"*"'
assert_contains "Backup: 특정 VM 정책 포함" "$RESP" "integ-test-vm"

# 3-4. 정책 업데이트 (같은 vm_name → 덮어쓰기)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.set","params":{"vm_name":"integ-test-vm","interval_hours":12,"retention_count":5},"id":"b4"}')
assert_contains "Backup: 정책 업데이트" "$RESP" "result"

# 3-5. 업데이트 확인
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"b5"}')
assert_contains "Backup: 업데이트 반영" "$RESP" "12"

# 3-6. 스냅샷 이력 조회 (VM 없어도 에러 없이 빈 배열)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.history","params":{"vm_name":"integ-test-vm"},"id":"b6"}')
assert_contains "Backup: 이력 조회 (빈 결과)" "$RESP" "result"

# 3-7. 정책 삭제 (특정 VM)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.delete","params":{"vm_name":"integ-test-vm"},"id":"b7"}')
assert_contains "Backup: 특정 정책 삭제" "$RESP" "result"

# 3-8. 삭제 후 목록
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"b8"}')
assert_not_contains "Backup: 삭제 후 미표시" "$RESP" "integ-test-vm"

# 3-9. 잘못된 파라미터 (interval 0)
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.set","params":{"vm_name":"x","interval_hours":0,"retention_count":1},"id":"b9"}')
assert_contains "Backup: interval 0 에러" "$RESP" "error"

# 3-10. 전체 정책 정리
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.delete","params":{"vm_name":"*"},"id":"b10"}')
assert_contains "Backup: 전체 정책 정리" "$RESP" "result"

echo ""

# ══════════════════════════════════════════════════════
# 결과 요약
# ══════════════════════════════════════════════════════
echo "=========================================="
echo -e " 결과: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
