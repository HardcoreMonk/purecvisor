#!/usr/bin/env bash
                                                                                           
                                                                        
                                                
                                                                
                                                
 
                              
 
        
                                                       
                                        
                     
                    
 
                                                              

set -uo pipefail

                                                      
GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

                                                      
SOCKET_PATH="${PCV_TEST_SOCKET_PATH:-/var/run/purecvisor/daemon.sock}"
PASS=0; FAIL=0; SKIP=0
TOTAL=0
RUN_ID="${PCV_TEST_RUN_ID:-$(date +%s)-$$}"
if [[ ! "$RUN_ID" =~ ^[A-Za-z0-9_-]+$ ]] || [ "${#RUN_ID}" -gt 40 ]; then
    echo "Invalid PCV_TEST_RUN_ID (use <=40 ASCII letters, digits, '_' or '-')" >&2
    exit 2
fi
OP_USER="pcv-op-${RUN_ID}"
VIEW_USER="pcv-view-${RUN_ID}"
BAD_USER="pcv-bad-${RUN_ID}"
TMPL_NAME="pcv-tmpl-${RUN_ID}"
CLOUD_TMPL="pcv-cloud-${RUN_ID}"
MISSING_TMPL="pcv-missing-${RUN_ID}"
POLICY_ALL="pcv-policy-all-${RUN_ID}"
POLICY_VM="pcv-policy-vm-${RUN_ID}"
INVALID_POLICY="pcv-policy-bad-${RUN_ID}"
MUTATION_STARTED=0

                                                    
log()  { echo -e "${CYAN}[INFO]${NC} $*"; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); }
skip() { echo -e "${YELLOW}[SKIP]${NC} $*"; SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); }

                                                     
send_rpc() {
    echo "$1" | nc -U "$SOCKET_PATH" 2>/dev/null || true
}

cleanup() {
    local status=$? cleanup_failed=0 users templates policies
    trap - EXIT
    if [ "$MUTATION_STARTED" -eq 1 ]; then
        for user in "$OP_USER" "$VIEW_USER" "$BAD_USER"; do
            send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.user.delete\",\"params\":{\"username\":\"$user\"},\"id\":\"cleanup-user\"}" >/dev/null
        done
        for template in "$TMPL_NAME" "$CLOUD_TMPL" "$MISSING_TMPL"; do
            send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"template.delete\",\"params\":{\"name\":\"$template\"},\"id\":\"cleanup-template\"}" >/dev/null
        done
        for policy in "$POLICY_ALL" "$POLICY_VM" "$INVALID_POLICY"; do
            send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.policy.delete\",\"params\":{\"vm_name\":\"$policy\"},\"id\":\"cleanup-policy\"}" >/dev/null
        done
        users=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"cleanup-users"}')
        templates=$(send_rpc '{"jsonrpc":"2.0","method":"template.list","params":{},"id":"cleanup-templates"}')
        policies=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"cleanup-policies"}')
        if ! printf '%s' "$users" | grep -q '"result"' || \
           ! printf '%s' "$templates" | grep -q '"result"' || \
           ! printf '%s' "$policies" | grep -q '"result"' || \
           printf '%s%s%s' "$users" "$templates" "$policies" | grep -Fq "$RUN_ID"; then
            echo -e "${RED}[CLEANUP-FAIL]${NC} RBAC/template/backup fixture 잔여 확인 실패 (run=$RUN_ID)" >&2
            cleanup_failed=1
        fi
    fi
    [ "$cleanup_failed" -eq 0 ] || exit 90
    exit "$status"
}
trap cleanup EXIT

                                                 
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
BASE_USERS=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"baseline-users"}')
BASE_TEMPLATES=$(send_rpc '{"jsonrpc":"2.0","method":"template.list","params":{},"id":"baseline-templates"}')
BASE_POLICIES=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"baseline-policies"}')
if ! printf '%s' "$BASE_USERS" | grep -q '"result"' || \
   ! printf '%s' "$BASE_TEMPLATES" | grep -q '"result"' || \
   ! printf '%s' "$BASE_POLICIES" | grep -q '"result"' || \
   printf '%s%s%s' "$BASE_USERS" "$BASE_TEMPLATES" "$BASE_POLICIES" | grep -Fq "$RUN_ID"; then
    echo -e "${RED}[ERROR]${NC} baseline 목록 캡처 실패 또는 fixture 충돌 (run=$RUN_ID)" >&2
    exit 1
fi
MUTATION_STARTED=1
echo ""

                                                        
              
                                                        
log "─── [1/3] RBAC 인증 테스트 ───"

                        
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.user.create\",\"params\":{\"username\":\"$OP_USER\",\"password\":\"pass123\",\"role\":\"operator\"},\"id\":\"r1\"}")
assert_contains "RBAC: operator 사용자 생성" "$RESP" "result"

                      
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.user.create\",\"params\":{\"username\":\"$VIEW_USER\",\"password\":\"view456\",\"role\":\"viewer\"},\"id\":\"r2\"}")
assert_contains "RBAC: viewer 사용자 생성" "$RESP" "result"

             
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"r3"}')
assert_contains "RBAC: 사용자 목록 조회" "$RESP" "$OP_USER"
assert_contains "RBAC: viewer 목록 포함" "$RESP" "$VIEW_USER"

                               
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.role.set\",\"params\":{\"username\":\"$OP_USER\",\"role\":\"admin\"},\"id\":\"r4\"}")
assert_contains "RBAC: 역할 변경 (→admin)" "$RESP" "result"

               
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"r5"}')
assert_contains "RBAC: admin 역할 반영 확인" "$RESP" "admin"

                        
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.user.create\",\"params\":{\"username\":\"$OP_USER\",\"password\":\"dup\",\"role\":\"viewer\"},\"id\":\"r6\"}")
assert_contains "RBAC: 중복 사용자 에러" "$RESP" "error"

             
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.user.delete\",\"params\":{\"username\":\"$VIEW_USER\"},\"id\":\"r7\"}")
assert_contains "RBAC: viewer 삭제" "$RESP" "result"

                 
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"r8"}')
assert_not_contains "RBAC: 삭제 후 미표시" "$RESP" "$VIEW_USER"

                                     
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.user.delete\",\"params\":{\"username\":\"$OP_USER\"},\"id\":\"r9\"}")
assert_contains "RBAC: operator fixture 정리" "$RESP" "result"

                         
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.user.create\",\"params\":{\"username\":\"$BAD_USER\",\"password\":\"x\",\"role\":\"superadmin\"},\"id\":\"r10\"}")
assert_contains "RBAC: 잘못된 역할 에러" "$RESP" "error"

echo ""

                                                        
                
                                                        
log "─── [2/3] VM 템플릿 테스트 ───"

                                               
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.list","params":{},"id":"t1"}')
assert_contains "Template: 프리셋 목록 조회" "$RESP" "result"

                         
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"template.get","params":{"name":"ubuntu-small"},"id":"t2"}')
if echo "$RESP" | grep -q "ubuntu-small"; then
    assert_contains "Template: ubuntu-small 조회" "$RESP" "vcpu"
else
    skip "Template: ubuntu-small 조회 (프리셋 미생성 — /etc/purecvisor/templates/ 권한 확인)"
fi

                 
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"template.create\",\"params\":{\"name\":\"$TMPL_NAME\",\"vcpu\":1,\"memory_mb\":512,\"disk_gb\":5,\"os_variant\":\"ubuntu24.04\",\"description\":\"Integration test template\"},\"id\":\"t3\"}")
assert_contains "Template: 커스텀 생성" "$RESP" "result"

                 
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"template.get\",\"params\":{\"name\":\"$TMPL_NAME\"},\"id\":\"t4\"}")
assert_contains "Template: 커스텀 조회" "$RESP" "$TMPL_NAME"
assert_contains "Template: vcpu 값 확인" "$RESP" "512"

                       
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"template.create\",\"params\":{\"name\":\"$TMPL_NAME\",\"vcpu\":2,\"memory_mb\":1024,\"disk_gb\":10,\"os_variant\":\"debian12\"},\"id\":\"t5\"}")
assert_contains "Template: 중복 이름 에러" "$RESP" "error"

                         
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"template.create\",\"params\":{\"name\":\"$MISSING_TMPL\"},\"id\":\"t6\"}")
assert_contains "Template: 필수 파라미터 누락 에러" "$RESP" "error"

             
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"template.delete\",\"params\":{\"name\":\"$TMPL_NAME\"},\"id\":\"t7\"}")
assert_contains "Template: 삭제 성공" "$RESP" "result"

                      
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"template.get\",\"params\":{\"name\":\"$TMPL_NAME\"},\"id\":\"t8\"}")
assert_contains "Template: 삭제 후 조회 에러" "$RESP" "error"

                        
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"template.delete\",\"params\":{\"name\":\"$MISSING_TMPL\"},\"id\":\"t9\"}")
assert_contains "Template: 멱등 삭제" "$RESP" "result"

                                   
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"template.create\",\"params\":{\"name\":\"$CLOUD_TMPL\",\"vcpu\":2,\"memory_mb\":2048,\"disk_gb\":20,\"os_variant\":\"ubuntu24.04\",\"cloud_init_user_data\":\"#cloud-config\\npackages:\\n  - nginx\\n\"},\"id\":\"t10\"}")
assert_contains "Template: cloud-init 포함 생성" "$RESP" "result"
send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"template.delete\",\"params\":{\"name\":\"$CLOUD_TMPL\"},\"id\":\"t11\"}" > /dev/null

echo ""

                                                        
               
                                                        
log "─── [3/3] 백업/복원 테스트 ───"

                                               
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.policy.set\",\"params\":{\"vm_name\":\"$POLICY_ALL\",\"interval_hours\":24,\"retention_count\":7},\"id\":\"b1\"}")
assert_contains "Backup: 첫 fixture 정책 설정" "$RESP" "result"

                    
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.policy.set\",\"params\":{\"vm_name\":\"$POLICY_VM\",\"interval_hours\":6,\"retention_count\":3},\"id\":\"b2\"}")
assert_contains "Backup: 특정 VM 정책 설정" "$RESP" "result"

            
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"b3"}')
assert_contains "Backup: 정책 목록 조회" "$RESP" "result"
assert_contains "Backup: 첫 fixture 정책 포함" "$RESP" "$POLICY_ALL"
assert_contains "Backup: 특정 VM 정책 포함" "$RESP" "$POLICY_VM"

                                  
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.policy.set\",\"params\":{\"vm_name\":\"$POLICY_VM\",\"interval_hours\":12,\"retention_count\":5},\"id\":\"b4\"}")
assert_contains "Backup: 정책 업데이트" "$RESP" "result"

              
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"b5"}')
assert_contains "Backup: 업데이트 반영" "$RESP" "12"

                                    
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.history\",\"params\":{\"vm_name\":\"$POLICY_VM\"},\"id\":\"b6\"}")
assert_contains "Backup: 이력 조회 (빈 결과)" "$RESP" "result"

                    
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.policy.delete\",\"params\":{\"vm_name\":\"$POLICY_VM\"},\"id\":\"b7\"}")
assert_contains "Backup: 특정 정책 삭제" "$RESP" "result"

              
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"backup.policy.list","params":{},"id":"b8"}')
assert_not_contains "Backup: 삭제 후 미표시" "$RESP" "$POLICY_VM"

                            
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.policy.set\",\"params\":{\"vm_name\":\"$INVALID_POLICY\",\"interval_hours\":0,\"retention_count\":1},\"id\":\"b9\"}")
assert_contains "Backup: interval 0 에러" "$RESP" "error"

                       
RESP=$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.policy.delete\",\"params\":{\"vm_name\":\"$POLICY_ALL\"},\"id\":\"b10\"}")
assert_contains "Backup: 첫 fixture 정책 정리" "$RESP" "result"

echo ""

                                                        
       
                                                        
echo "=========================================="
echo -e " 결과: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
