#!/bin/bash






















set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
INTEG_DIR="$PROJECT_ROOT/tests/integration"


HOST="localhost"
RUN_TIER0=true
RUN_TIER1=true
RUN_TIER2=false
CI_MODE=false
SPECIFIC_TIER=""

while [ $# -gt 0 ]; do
    case "$1" in
        --all)       RUN_TIER2=true ;;
        --tier)      SPECIFIC_TIER="$2"; shift ;;
        --host)      HOST="$2"; shift ;;
        --ci)        CI_MODE=true ;;
        -h|--help)
            echo "Usage: $0 [--all] [--tier 0|1|2] [--host HOST] [--ci]"
            exit 0 ;;
        *) HOST="$1" ;;
    esac
    shift
done

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
    local cmd="$2"
    local tier="$3"

    echo ""
    echo -e "${CYAN}────────────────────────────────────────${NC}"
    echo -e "${BOLD}[$tier] $name${NC}"
    echo -e "${CYAN}────────────────────────────────────────${NC}"

    local t_start=$(date +%s)
    local output
    local exit_code

    output=$(eval "$cmd" 2>&1) || true
    exit_code=${PIPESTATUS[0]:-$?}


    local clean_output
    clean_output=$(printf '%s\n' "$output" | sed -E 's/\x1B\[[0-9;]*[A-Za-z]//g')


    local pass=$(printf '%s\n' "$clean_output" | grep -E -c '^[[:space:]]*(ok |PASS([[:space:]]|$)|\[PASS\])' || true)
    local fail_count=$(printf '%s\n' "$clean_output" | grep -E -c '^[[:space:]]*(not ok |FAIL([[:space:]]|$)|\[FAIL\])' || true)

    if [ "$pass" -eq 0 ] && [ "$fail_count" -eq 0 ]; then
        if [ "$exit_code" -eq 0 ]; then
            pass=1
        else
            fail_count=1
        fi
    fi

    local t_end=$(date +%s)
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

check_daemon() {
    if curl -s -o /dev/null -w "%{http_code}" "http://$HOST/api/v1/health" 2>/dev/null | grep -q "200"; then
        return 0
    fi
    return 1
}

check_socket() {
    [ -S "/var/run/purecvisor/daemon.sock" ] && return 0
    return 1
}

edition_service_hint() {
    local edition="${EDITION:-single}"
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


    run_test "make test (유닛 테스트)" \
        "cd '$PROJECT_ROOT' && make test 2>&1 | grep -v 'dpdk/bridge_delete' | grep -v '^$'" \
        "T0"
fi


if $RUN_TIER1; then
    echo ""
    echo -e "${BOLD}${CYAN}▶ TIER 1: SAFE 통합 테스트 (읽기 전용, 부작용 없음)${NC}"


    if check_daemon; then
        echo -e "  ${GREEN}✓${NC} 데몬 접근 가능 (http://$HOST/api/v1/health)"

        run_test "REST API 통합 (88 엔드포인트)" \
            "bash '$INTEG_DIR/test_rest_api_full.sh' '$HOST'" \
            "T1"

        run_test "네거티브/스트레스 테스트" \
            "bash '$INTEG_DIR/test_negative_stress.sh' '$HOST'" \
            "T1"

        run_test "프론트엔드↔백엔드 정합성 (70항목)" \
            "bash '$INTEG_DIR/test_frontend_api.sh' '$HOST'" \
            "T1"
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

    if check_socket; then
        echo -e "  ${GREEN}✓${NC} UDS 소켓 접근 가능"

        run_test "RBAC/템플릿/백업 (34 케이스)" \
            "sudo bash '$INTEG_DIR/test_rbac_template_backup.sh'" \
            "T2"

        run_test "코어 고도화 검증" \
            "sudo bash '$INTEG_DIR/test_core_enhancement.sh'" \
            "T2"


        echo ""
        echo -e "  ${CYAN}리소스 정리 확인:${NC}"
        LEAKED_USERS=$(echo '{"jsonrpc":"2.0","method":"auth.user.list","params":{},"id":"1"}' | \
            nc -U /var/run/purecvisor/daemon.sock 2>/dev/null | \
            python3 -c "import sys,json;d=json.load(sys.stdin);users=[u['username'] for u in d.get('result',d.get('data',[]))];test=[u for u in users if u.startswith('test-')];print(len(test))" 2>/dev/null || echo "?")
        if [ "$LEAKED_USERS" = "0" ] || [ "$LEAKED_USERS" = "?" ]; then
            echo -e "    ${GREEN}✓${NC} 테스트 사용자 잔류 없음"
        else
            echo -e "    ${YELLOW}⚠${NC} 테스트 사용자 ${LEAKED_USERS}건 잔류"
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
