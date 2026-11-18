#!/bin/bash
                          
                                                                  
                                                                          
                                                   
                                               
                                                              
                                                             
                                                 
                                                  
                                                               
                
 
                      
                                                                        
                                                
                                                 
                                                                     
                                           
 
              
                                                        
                                                                           
                                              
 
       
                                                                             
                                                                          
                                                            
                                                             
                                                                 
                                                             
                                                                        
 
                                  
                                     
                                                        
                                                                     

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
INTEG_DIR="$PROJECT_ROOT/tests/integration"
SOCKET_PATH="${PCV_TEST_SOCKET_PATH:-/var/run/purecvisor/daemon.sock}"

                                                             
HOST="localhost"
RUN_TIER0=true
RUN_TIER1=true
RUN_TIER2=false
CI_MODE=false
SPECIFIC_TIER=""

while [ $# -gt 0 ]; do
    case "$1" in
        --all)       RUN_TIER2=true ;;
        --tier)
            [ $# -ge 2 ] || { echo "Missing value: --tier" >&2; exit 2; }
            SPECIFIC_TIER="$2"; shift ;;
        --host)
            [ $# -ge 2 ] || { echo "Missing value: --host" >&2; exit 2; }
            HOST="$2"; shift ;;
        --ci)        CI_MODE=true ;;
        -h|--help)
            echo "Usage: $0 [--all] [--tier 0|1|2] [--host HOST] [--ci]"
            exit 0 ;;
        *) HOST="$1" ;;
    esac
    shift
done

                                                   
                                                       
if ! python3 - "$HOST" <<'PY'
import ipaddress
import re
import sys
from urllib.parse import urlsplit

value = sys.argv[1]
try:
    parsed = urlsplit("http://" + value)
    port = parsed.port
except ValueError:
    raise SystemExit(1)
if (not parsed.hostname or parsed.username is not None or parsed.password is not None
        or parsed.path or parsed.query or parsed.fragment or parsed.netloc != value):
    raise SystemExit(1)
host = parsed.hostname
try:
    ipaddress.ip_address(host)
except ValueError:
    labels = host.rstrip(".").split(".")
    if not labels or len(host) > 253 or any(
        not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label)
        for label in labels
    ):
        raise SystemExit(1)
if port is not None and not (1 <= port <= 65535):
    raise SystemExit(1)
PY
then
    echo "Invalid host: expected hostname, IPv4, or bracketed IPv6 with optional port" >&2
    exit 2
fi

if [ -n "$SPECIFIC_TIER" ]; then
    RUN_TIER0=false; RUN_TIER1=false; RUN_TIER2=false
    case "$SPECIFIC_TIER" in
        0) RUN_TIER0=true ;;
        1) RUN_TIER1=true ;;
        2) RUN_TIER2=true ;;
        *) echo "Invalid tier: $SPECIFIC_TIER (0, 1, 2)"; exit 1 ;;
    esac
fi

                                                               
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_SKIP=0
TIER_RESULTS=""
START_TIME=$(date +%s)

run_test() {
    local name="$1"
    local tier="$2"
    shift 2

    echo ""
    echo -e "${CYAN}────────────────────────────────────────${NC}"
    echo -e "${BOLD}[$tier] $name${NC}"
    echo -e "${CYAN}────────────────────────────────────────${NC}"

    local t_start
    t_start=$(date +%s)
    local output
    local exit_code

                                                          
                                          
    output=$("$@" 2>&1)
    exit_code=$?

                                         
    local clean_output
    clean_output=$(printf '%s\n' "$output" | sed -E 's/\x1B\[[0-9;]*[A-Za-z]//g')

                                                         
                                                            
                                                                               
                                                             
    local pass fail_count skip_count
    read -r pass fail_count skip_count < <(printf '%s\n' "$clean_output" | awk '
        /^[[:space:]]*(ok |not ok |1\.\.0)/ && /#[[:space:]]*[Ss][Kk][Ii][Pp]([[:space:]]|$)/ { skip++; next }
        /^[[:space:]]*(SKIP(:|[[:space:]]|$)|\[SKIP\])/ { skip++; next }
        /^[[:space:]]*(ok |PASS([[:space:]]|$)|\[PASS\])/ { pass++; next }
        /^[[:space:]]*(not ok |FAIL([[:space:]]|$)|\[FAIL\])/ { fail++ }
        END { print pass+0, fail+0, skip+0 }
    ')
                                                    
                                          
    if [ "$exit_code" -ne 0 ] && [ "$fail_count" -eq 0 ]; then
        fail_count=1
    elif [ "$exit_code" -eq 0 ] && [ "$pass" -eq 0 ] && [ "$fail_count" -eq 0 ] && [ "$skip_count" -eq 0 ]; then
        pass=1
    fi

    local t_end
    t_end=$(date +%s)
    local duration=$((t_end - t_start))

                   
    echo "$output" | tail -20

    TOTAL_PASS=$((TOTAL_PASS + pass))
    TOTAL_FAIL=$((TOTAL_FAIL + fail_count))
    TOTAL_SKIP=$((TOTAL_SKIP + skip_count))

    local status
    if [ "$fail_count" -eq 0 ] && [ "$pass" -eq 0 ] && [ "$skip_count" -gt 0 ]; then
        status="${YELLOW}SKIP${NC}"
    elif [ "$fail_count" -eq 0 ]; then
        status="${GREEN}PASS${NC}"
    else
        status="${RED}FAIL${NC}"
    fi

    TIER_RESULTS="${TIER_RESULTS}\n  $status  $name (${duration}s, pass:$pass fail:$fail_count skip:$skip_count)"
}

run_unit_suite() {
                                                       
                                                        
    (cd "$PROJECT_ROOT" && make test-auto)
}

check_daemon() {
    if curl -s -o /dev/null -w "%{http_code}" "http://$HOST/api/v1/health" 2>/dev/null | grep -q "200"; then
        return 0
    fi
    return 1
}

check_socket() {
    [ -S "$SOCKET_PATH" ] && return 0
    return 1
}

edition_service_hint() {
    if [ -n "${DAEMON_SERVICE:-}" ]; then
        echo "$DAEMON_SERVICE"
    else
        echo "purecvisorsd"
    fi
}

                                                                             
                                                                       
                                                                           
                                                          
                                                                           
                                                                                    
                                                         
                                                              
                                                            
run_isolated_test() {
    local name="$1"
    shift
    if [ -z "${ISOLATED_DAEMON_OK:-}" ]; then
        ISOLATED_DAEMON_OK=0
        ISOLATED_DAEMON_REASON="격리 데몬 하네스 부재($INTEG_DIR/lib/pcv_isolated_daemon.sh)"
        if [ -f "$INTEG_DIR/lib/pcv_isolated_daemon.sh" ]; then
                                                                            
            . "$INTEG_DIR/lib/pcv_isolated_daemon.sh"
            if pcv_iso_capable; then
                ISOLATED_DAEMON_OK=1
                                                                          
                                                                      
                                                             
                if ! ISOLATED_DAEMON_STATE_ERR="$(pcv_iso_check_state_fs "${TMPDIR:-/tmp}" 2>&1)"; then
                    ISOLATED_DAEMON_OK=2
                fi
            fi
            ISOLATED_DAEMON_REASON="$PCV_ISO_SKIP_REASON"
        fi
    fi
    if [ "$ISOLATED_DAEMON_OK" -eq 1 ]; then
        run_test "$name" "$@"
        return
    fi
    if [ "$ISOLATED_DAEMON_OK" -eq 2 ]; then
        echo ""
        echo -e "  ${RED}FAIL${NC}  $name — ${ISOLATED_DAEMON_STATE_ERR}"
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        TIER_RESULTS="${TIER_RESULTS}\n  ${RED}FAIL${NC}  $name (격리 상태 폴더가 운영 LSM 감시 경로와 같은 fs)"
        return
    fi
    echo ""
    echo -e "  ${YELLOW}SKIP${NC}  $name — ${ISOLATED_DAEMON_REASON}"
    TOTAL_SKIP=$((TOTAL_SKIP + 1))
    TIER_RESULTS="${TIER_RESULTS}\n  ${YELLOW}SKIP${NC}  $name (격리 데몬 실행 불가)"
}

                                                                                  
                                                                        
                                                                 
                                       
run_root_namespace_test() {
    local name="$1"
    shift
    if [ "$(id -u)" -eq 0 ]; then
        run_test "$name" "$@"
        return
    fi
    echo ""
    echo -e "  ${YELLOW}SKIP${NC}  $name — 자체 unshare 격리에 root 필요(비root 실행)"
    TOTAL_SKIP=$((TOTAL_SKIP + 1))
    TIER_RESULTS="${TIER_RESULTS}\n  ${YELLOW}SKIP${NC}  $name (root 필요)"
}

                                                                 
echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  PureCVisor 자동화 테스트 러너${NC}"
echo -e "${BOLD}  Host: $HOST | Tiers: $(${RUN_TIER0} && echo '0')$(${RUN_TIER1} && echo '+1')$(${RUN_TIER2} && echo '+2')${NC}"
echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"

                                                            
if $RUN_TIER0; then
    echo ""
    echo -e "${BOLD}${CYAN}▶ TIER 0: 유닛 테스트 (외부 의존성 없음)${NC}"

                                                            
    run_test "make test-auto (유닛 테스트)" "T0" run_unit_suite
fi

                                                            
if $RUN_TIER1; then
    echo ""
    echo -e "${BOLD}${CYAN}▶ TIER 1: SAFE 통합 테스트 (격리 또는 제한된 부작용)${NC}"

                                                        
                                                
    run_test "프론트엔드 API 전송 보안 기본값 (격리 HTTP)" \
        "T1" bash "$INTEG_DIR/test_frontend_api_transport_contract.sh"

                                                                        
                                                               
                                                              
                                                          
                                                      
    case "$HOST" in
        localhost|127.0.0.1|"")
            run_root_namespace_test "SEC-2 부트스트랩 fallback 백도어 차단 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_sec2_bootstrap_fallback.sh"

                                                            
                                                      
                                   
            run_isolated_test "VM 락 교차 unlock 차단 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_vm_lock_cross_unlock.sh"
            run_isolated_test "vm.batch 팬아웃·정식 진입점 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_vm_batch_fanout.sh"
            run_isolated_test "스냅샷 rollback 재기동 정식 진입점 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_snapshot_rollback_restart_entry.sh"

                                                          
                                                            
                                   
            run_isolated_test "세션 revoke 강제 로그아웃 실동작 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_session_revoke.sh"

                                                                            
                                                                    
                                                                 
            run_isolated_test "refresh-remint 사용자 세션 취소 re-mint 거부 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_user_sessions_revoke.sh"

                                                                                    
                                                                       
                                                        
            run_isolated_test "세션 단위 로그아웃 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_logout_scope.sh"

                                                                                              
                                                                                
                                                                                        
            run_isolated_test "삭제 주체 인증 수단 거부 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_auth_deleted_subject.sh"

                                                                                                    
                                                                                     
                                                                                        
            run_isolated_test "VPC 관리 대역 보호·확인 대기 자동 원복 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_vpc_mgmt_guard.sh"

                                                                                                       
                                                                                                  
                                                                                                           
            run_isolated_test "VPC 테넌트 주소 풀 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_vpc_tenant_pool.sh"

                                                        
                                                             
                                                                         
            run_isolated_test "JSON ingress 경계 크래시0 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_json_ingress_boundary.sh"

                                                                    
                                                                     
                                                           
                                                               
            run_isolated_test "STO-2 스냅샷 prune 데이터유실 방지 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_backup_retention_effect.sh"

                                                                                  
                                                                                     
                                                                      
            run_isolated_test "복제 SSH 호스트 키 고정 (격리 데몬)" "T1" bash "$INTEG_DIR/test_backup_replicate_hostkey.sh"

                                                                
                                                                       
                                                                        
                                                            
            run_isolated_test "graceful drain 실배선 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_graceful_drain_effect.sh"

                                                                                   
                                                                                    
                                                           
                                                                                
            run_isolated_test "SEC-3 API Key 저장 role 집행 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_apikey_role_enforce.sh"

                                                                                                       
                                                                                          
                                                                                                    
                                                                                                  
            run_isolated_test "보안 사건 운영자 단건 종결 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_security_event_close.sh"

                                                                                                          
                                                                                             
            run_isolated_test "CLI 계약: agent set 배열·새 컨테이너 이름 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_contract_drift_cli.sh"

                                                                                           
                                                                                                    
                                                                                           
            run_isolated_test "화면 본문·실데몬 계약 대조 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_contract_drift_ui.sh"

                                                                                        
                                                                                                
                                                                                          
            run_isolated_test "관리면 노출 수집기 정책·프록시 신뢰 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_management_plane_exposure.sh"

                                                                        
                                                                            
                                                                             
                                                                            
                                                                      
                                         
            run_isolated_test "CMP-3 vm.create ISO 검증 배선 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_vm_create_iso_validation.sh"

                                                                                       
                                                                   
                                                                    
            run_isolated_test "UUID 표기 새 VM 이름 거부 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_vm_name_uuid_form.sh"

                                                                                                    
                                                                                        
                                                                                          
            run_isolated_test "LXC unprivileged 생성 정책 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_lxc_unprivileged_policy.sh"

                                                                                               
                                                                         
                                                                        
            run_isolated_test "vm.delete Job 접수·완료·상태 조회 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_vm_delete_job.sh"

                                                                                           
                                                                             
                                                                                     
            run_isolated_test "비동기 감사 행위자·즉시 거절 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_async_audit_actor.sh"

                                                                                                       
                                                                                                
                                                                          
            run_isolated_test "보안 일관성: 행위자·역할·테넌트·웹훅 판정 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_sec_consistency.sh"

                                                                                               
                                                                                     
            run_isolated_test "help REST 예시 실데몬 대조 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_help_examples_live.sh"

                                                                                       
                                                                              
                                                                    
                                                                     
            run_isolated_test "무인증 OPTIONS·401 데몬 생존 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_rest_options_preflight.sh"

                                                                            
                                                                           
                                                                         
                                                                            
                                                                        
                              
            run_isolated_test "NET-4 QoS 재수화 늦은 vnet 재적용 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_qos_rehydrate_effect.sh"

                                                                                                      
                                                                   
            run_isolated_test "DPDK 관리 NIC 가드 RPC 경계 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_dpdk_guard_effect.sh"

                                                                                                       
                                                                                    
            run_isolated_test "OVN VPC 실패 폭발 반경 (격리 데몬 + 일회용 NB)" \
                "T1" bash "$INTEG_DIR/test_vpc_ovn_blast_radius.sh"

                                                                
                                                                    
                                                               
                                                                             
                                                                             
                                           
            run_isolated_test "TOTP 2FA REST 2단계 로그인 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_totp_login.sh"

                                                                                                              
                                                                                             
                                                                              
                                                                                          
                                                               
            run_isolated_test "컨테이너 볼륨 원본 링크 거절·시작 전 재검사 (격리 데몬)" "T1" bash "$INTEG_DIR/test_container_volume_source_guard.sh"

                                                                                                          
                                                                                               
                                                                                                  
                                                                                                          
                                                                                                                         
                                         
                                                
            run_isolated_test "컨테이너 명령 실행 격리 — 상한·환경 차단·종료 (격리 데몬)" "T1" bash "$INTEG_DIR/test_container_exec_isolation.sh"

                                                                                                           
                                                                                                               
                                                                                                          
                                                                                                             
                                                                                                           
                                                                                                         
                                   
            run_isolated_test "컨테이너 유닛 시작·미설치·실패 원인·헬스·logs·별칭 (격리 데몬)" "T1" bash "$INTEG_DIR/test_container_unit_start.sh"

                                                                                              
                                                                                         
                                                                                                            
                                                                                        
                                                                
            run_isolated_test "OPERATOR 호스트 자원 참조 경계 (격리 데몬)" "T1" bash "$INTEG_DIR/test_operator_resource_boundary.sh"

                                                                                                                
                                                                                                                         
                                                                                                             
                                                                                                                     
                                                                                               
                                                                                                     
                                                
            run_isolated_test "메인 루프 정지 방지 1단계" "T1" bash "$INTEG_DIR/test_mainloop_blocking_phase1.sh"
            run_isolated_test "메인 루프 정지 방지 2단계" "T1" bash "$INTEG_DIR/test_mainloop_blocking_phase2.sh"

                                                                                                                
                                                                                                   
                                                                                                    
                                                                                                   
            run_isolated_test "데몬 견고성 (disk.detach·vm.vnc 실패 입력)" "T1" bash "$INTEG_DIR/test_daemon_robustness.sh"

                                                                                                            
                                                                                                       
                                                                                                       
                                                                                           
            run_isolated_test "스냅샷 잠금 갱신 (약 6분)" "T1" bash "$INTEG_DIR/test_snapshot_lock_renew.sh"

                                                                                                           
                                                                                                                    
                                                                                                           
            run_isolated_test "libvirt DEGRADED 기동 판정 (격리 데몬 3회 기동)" "T1" bash "$INTEG_DIR/test_libvirt_degraded_boot.sh"

                                                                                                    
                                                                                                     
                                                                                                                
            run_isolated_test "부팅 뒤 자동 시작 판정 (격리 데몬 5회 기동)" "T1" bash "$INTEG_DIR/test_vm_boot_autostart_boot.sh"
            run_isolated_test "VM 진단 표본기 (격리 데몬)" "T1" bash "$INTEG_DIR/test_monitor_vm_diag.sh"
            run_isolated_test "주체 이름 공간 (격리 데몬)" "T1" bash "$INTEG_DIR/test_principal_name_namespace.sh"
            run_isolated_test "비ADMIN VM 읽기 범위 (격리 데몬)" "T1" bash "$INTEG_DIR/test_vm_read_tenant_scope.sh"

                                                                                                        
                                                                                         
                                                                      
            run_isolated_test "계수기 정지·교체 직전 저장 (격리 데몬 2회 기동 + SIGUSR2)" "T1" bash "$INTEG_DIR/test_prom_counter_flush.sh"

                                                                                                            
                                                                                                     
            run_isolated_test "플러그인 종료 콜백 (격리 데몬 + 시험 플러그인)" "T1" bash "$INTEG_DIR/test_plugin_shutdown_callback.sh"

                                                                                                                          
                                                                                                             
                                                                                                  
                                                                            
            run_isolated_test "플러그인 API 내보내기·응답 도우미 (격리 데몬 + 시험 플러그인)" "T1" bash "$INTEG_DIR/test_plugin_api_export.sh"
            ;;
    esac

                    
    if check_daemon; then
        echo -e "  ${GREEN}✓${NC} 데몬 접근 가능 (http://$HOST/api/v1/health)"

        run_test "REST API 통합 (88 엔드포인트)" \
            "T1" bash "$INTEG_DIR/test_rest_api_full.sh" "$HOST"

        run_test "네거티브/스트레스 테스트" \
            "T1" bash "$INTEG_DIR/test_negative_stress.sh" "$HOST"

        run_test "프론트엔드↔백엔드 정합성 (70항목)" \
            "T1" bash "$INTEG_DIR/test_frontend_api.sh" "$HOST"
    else
        echo -e "  ${YELLOW}⚠${NC} 데몬 미실행 — Tier 1 건너뜀 (http://$HOST/api/v1/health 응답 없음)"
        echo -e "  ${YELLOW}힌트${NC}: sudo systemctl start $(edition_service_hint) 또는 --host <노드IP>"
        TOTAL_SKIP=$((TOTAL_SKIP + 3))
        TIER_RESULTS="${TIER_RESULTS}\n  ${YELLOW}SKIP${NC}  REST API 통합 (데몬 미실행)"
        TIER_RESULTS="${TIER_RESULTS}\n  ${YELLOW}SKIP${NC}  네거티브/스트레스 (데몬 미실행)"
        TIER_RESULTS="${TIER_RESULTS}\n  ${YELLOW}SKIP${NC}  프론트엔드↔백엔드 (데몬 미실행)"
    fi
fi

                                                            
if $RUN_TIER2; then
    echo ""
    echo -e "${BOLD}${CYAN}▶ TIER 2: MODERATE 통합 테스트 (리소스 생성→삭제)${NC}"

                                                       
                                                           
                                                      
    TIER2_RUN_ID="auto-$(date +%s)-$$-${RANDOM}"
    if [[ ! "$TIER2_RUN_ID" =~ ^[A-Za-z0-9_-]+$ ]] ||
       [ "${#TIER2_RUN_ID}" -gt 40 ]; then
        echo -e "  ${RED}✗${NC} Tier 2 run ID 생성 실패" >&2
        exit 1
    fi

    if check_socket; then
        echo -e "  ${GREEN}✓${NC} UDS 소켓 접근 가능 (run=${TIER2_RUN_ID})"

        run_test "RBAC/템플릿/백업 (34 케이스)" \
            "T2" sudo env "PCV_TEST_SOCKET_PATH=$SOCKET_PATH" \
            "PCV_TEST_RUN_ID=$TIER2_RUN_ID" \
            bash "$INTEG_DIR/test_rbac_template_backup.sh"

        run_test "코어 고도화 검증" \
            "T2" sudo env "PCV_TEST_SOCKET_PATH=$SOCKET_PATH" \
            "PCV_TEST_RUN_ID=$TIER2_RUN_ID" \
            bash "$INTEG_DIR/test_core_enhancement.sh"

                                                       
                                                     
                                                  
        echo ""
        echo -e "  ${CYAN}리소스 정리 확인:${NC}"
        LEAKED_USERS=$(echo '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"1"}' | \
            nc -U "$SOCKET_PATH" 2>/dev/null | \
            python3 -c '
import json, sys
run_id = sys.argv[1]
d = json.load(sys.stdin)
if not isinstance(d, dict) or "error" in d:
    raise SystemExit(2)
if "result" in d:
    users = d["result"]
elif "data" in d:
    users = d["data"]
else:
    raise SystemExit(2)
if not isinstance(users, list):
    raise SystemExit(2)
names = []
for user in users:
    if not isinstance(user, dict) or not isinstance(user.get("username"), str):
        raise SystemExit(2)
    names.append(user["username"])
expected = {
    f"pcv-op-{run_id}",
    f"pcv-view-{run_id}",
    f"pcv-bad-{run_id}",
}
print(sum(name in expected for name in names))
' "$TIER2_RUN_ID" 2>/dev/null || echo "?")
        if [ "$LEAKED_USERS" = "0" ]; then
            echo -e "    ${GREEN}✓${NC} 테스트 사용자 잔류 없음"
        elif [ "$LEAKED_USERS" = "?" ]; then
            echo -e "    ${RED}✗${NC} 테스트 사용자 잔류 조회 실패"
            TOTAL_FAIL=$((TOTAL_FAIL + 1))
            TIER_RESULTS="${TIER_RESULTS}\n  ${RED}FAIL${NC}  Tier 2 cleanup 조회 실패"
        else
            echo -e "    ${RED}✗${NC} 테스트 사용자 ${LEAKED_USERS}건 잔류"
            TOTAL_FAIL=$((TOTAL_FAIL + LEAKED_USERS))
            TIER_RESULTS="${TIER_RESULTS}\n  ${RED}FAIL${NC}  Tier 2 테스트 사용자 ${LEAKED_USERS}건 잔류"
        fi
    else
        echo -e "  ${YELLOW}⚠${NC} UDS 소켓 미접근 — Tier 2 건너뜀"
        echo -e "  ${YELLOW}힌트${NC}: sudo systemctl start $(edition_service_hint)"
        TOTAL_SKIP=$((TOTAL_SKIP + 2))
        TIER_RESULTS="${TIER_RESULTS}\n  ${YELLOW}SKIP${NC}  RBAC/템플릿/백업 (소켓 미접근)"
        TIER_RESULTS="${TIER_RESULTS}\n  ${YELLOW}SKIP${NC}  코어 고도화 (소켓 미접근)"
    fi
fi

                                                             
END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

echo ""
echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  테스트 결과 요약${NC}"
echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"
echo -e "$TIER_RESULTS"
echo ""
echo -e "  ${GREEN}PASS${NC}: $TOTAL_PASS  ${RED}FAIL${NC}: $TOTAL_FAIL  ${YELLOW}SKIP${NC}: $TOTAL_SKIP"
echo -e "  소요 시간: ${DURATION}초"
echo -e "${BOLD}═══════════════════════════════════════════════════════════${NC}"

if [ "$TOTAL_FAIL" -gt 0 ]; then
    echo -e "\n${RED}✗ 테스트 실패 — $TOTAL_FAIL건${NC}"
    $CI_MODE && exit 1
    exit 1
fi

if [ "$TOTAL_SKIP" -gt 0 ] && [ "$TOTAL_PASS" -eq 0 ]; then
    echo -e "\n${YELLOW}⚠ 모든 테스트 건너뜀 — 각 실행의 환경 조건을 확인하십시오${NC}"
    $CI_MODE && exit 1
    exit 0
fi

if [ "$TOTAL_SKIP" -gt 0 ]; then
    echo -e "\n${GREEN}✓ 실행한 테스트 통과 — 건너뜀 ${TOTAL_SKIP}건은 미검증${NC}"
else
    echo -e "\n${GREEN}✓ 모든 테스트 통과${NC}"
fi
exit 0
