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

                              
    local pass fail_count
    pass=$(printf '%s\n' "$clean_output" | grep -E -c '^[[:space:]]*(ok |PASS([[:space:]]|$)|\[PASS\])' || true)
    fail_count=$(printf '%s\n' "$clean_output" | grep -E -c '^[[:space:]]*(not ok |FAIL([[:space:]]|$)|\[FAIL\])' || true)
                                                    
                                          
    if [ "$exit_code" -ne 0 ] && [ "$fail_count" -eq 0 ]; then
        fail_count=1
    elif [ "$exit_code" -eq 0 ] && [ "$pass" -eq 0 ] && [ "$fail_count" -eq 0 ]; then
        pass=1
    fi

    local t_end
    t_end=$(date +%s)
    local duration=$((t_end - t_start))

                   
    echo "$output" | tail -20

    TOTAL_PASS=$((TOTAL_PASS + pass))
    TOTAL_FAIL=$((TOTAL_FAIL + fail_count))

    local status
    if [ "$fail_count" -eq 0 ]; then
        status="${GREEN}PASS${NC}"
    else
        status="${RED}FAIL${NC}"
    fi

    TIER_RESULTS="${TIER_RESULTS}\n  $status  $name (${duration}s, pass:$pass fail:$fail_count)"
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
            run_test "SEC-2 부트스트랩 fallback 백도어 차단 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_sec2_bootstrap_fallback.sh"

                                                                  
                                                      
                                   
            run_test "CMP-1 VM 락 교차 unlock 차단 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_vm_lock_cross_unlock.sh"

                                                                
                                                            
                                   
            run_test "SEC-1 세션 revoke 강제 로그아웃 실동작 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_session_revoke.sh"

                                                                        
                                                                    
                                                                 
            run_test "refresh-remint 사용자 세션 취소 re-mint 거부 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_user_sessions_revoke.sh"

                                                                    
                                                             
                                                                         
            run_test "게이트#2 JSON ingress / DISP-1 크래시0 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_json_ingress_disp1.sh"

                                                                    
                                                                     
                                                           
                                                               
            run_test "STO-2 스냅샷 prune 데이터유실 방지 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_backup_retention_effect.sh"

                                                                       
                                                                       
                                                                        
                                                            
            run_test "DISP-4 graceful drain 실배선 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_graceful_drain_effect.sh"

                                                                                   
                                                                                    
                                                           
                                                                                
            run_test "SEC-3 API Key 저장 role 집행 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_apikey_role_enforce.sh"

                                                                        
                                                                            
                                                                             
                                                                            
                                                                      
                                         
            run_test "CMP-3 vm.create ISO 검증 배선 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_vm_create_iso_validation.sh"

                                                                            
                                                                           
                                                                         
                                                                            
                                                                        
                              
            run_test "NET-4 QoS 재수화 늦은 vnet 재적용 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_qos_rehydrate_effect.sh"

                                                                
                                                                    
                                                               
                                                                             
                                                                             
                                           
            run_test "TOTP 2FA REST 2단계 로그인 (격리 데몬)" \
                "T1" bash "$INTEG_DIR/test_totp_login.sh"
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
    echo -e "\n${YELLOW}⚠ 모든 테스트 건너뜀 — 데몬 실행 필요${NC}"
    $CI_MODE && exit 1
    exit 0
fi

echo -e "\n${GREEN}✓ 모든 테스트 통과${NC}"
exit 0
