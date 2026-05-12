#!/bin/bash


















set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
REST_SERVER="$PROJECT_ROOT/src/api/rest_server.c"
DISPATCHER="$PROJECT_ROOT/src/api/dispatcher.c"
ENDPOINTS_JS="$PROJECT_ROOT/ui/modules/endpoints.js"

CI_MODE=false
[ "${1:-}" = "--ci" ] && CI_MODE=true

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

PASS_COUNT=0
WARN_COUNT=0
FAIL_COUNT=0

pass() { echo -e "${GREEN}PASS${NC} $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
warn() { echo -e "${YELLOW}WARN${NC} $1"; WARN_COUNT=$((WARN_COUNT + 1)); }
fail() { echo -e "${RED}FAIL${NC} $1"; FAIL_COUNT=$((FAIL_COUNT + 1)); }

echo "═══════════════════════════════════════════════════════════════"
echo "  PureCVisor REST ↔ RPC ↔ Frontend 정합성 검증"
echo "═══════════════════════════════════════════════════════════════"
echo ""


echo -e "${CYAN}[1/4] REST → Dispatcher 정합성 검증${NC}"
echo "  REST 서버가 호출하는 모든 RPC 메서드가 디스패처에 등록되어 있는지 확인"
echo ""



REST_METHODS=$(grep -oP '_build_rpc(_name)?\("\K[^"]+' "$REST_SERVER" | sort -u)


DISP_METHODS=$(grep -oP 'g_hash_table_insert\(g_rpc_routes,\s*"\K[^"]+' "$DISPATCHER" | sort -u)



DISP_SPECIAL="vm.create"


REST_ONLY=""
while IFS= read -r method; do

    if echo "$DISP_SPECIAL" | grep -qx "$method"; then
        continue
    fi
    if ! echo "$DISP_METHODS" | grep -qx "$method"; then
        REST_ONLY="${REST_ONLY}${method}\n"
        fail "REST calls '$method' but NOT registered in dispatcher → -32601 at runtime"
    fi
done <<< "$REST_METHODS"

if [ -z "$REST_ONLY" ]; then
    pass "All REST RPC methods ($(echo "$REST_METHODS" | wc -l)) are registered in dispatcher"
fi

echo ""


echo -e "${CYAN}[2/4] Dispatcher → REST 커버리지${NC}"
echo "  디스패처에 등록된 RPC 메서드 중 REST 엔드포인트가 없는 것 (UDS 전용)"
echo ""

DISP_ONLY_COUNT=0
DISP_ONLY_LIST=""
while IFS= read -r method; do
    if ! echo "$REST_METHODS" | grep -qx "$method"; then
        DISP_ONLY_LIST="${DISP_ONLY_LIST}  - ${method}\n"
        DISP_ONLY_COUNT=$((DISP_ONLY_COUNT + 1))
    fi
done <<< "$DISP_METHODS"

DISP_TOTAL=$(echo "$DISP_METHODS" | wc -l)
REST_TOTAL=$(echo "$REST_METHODS" | wc -l)

if [ "$DISP_ONLY_COUNT" -gt 0 ]; then
    warn "${DISP_ONLY_COUNT} dispatcher methods have no REST endpoint (UDS-only):"
    echo -e "$DISP_ONLY_LIST"
else
    pass "All dispatcher methods have REST endpoints"
fi

echo ""


echo -e "${CYAN}[3/4] 디스패처 중복 등록 검사${NC}"
echo "  동일 메서드명이 여러 번 등록되면 마지막 등록만 유효 (의도치 않은 덮어쓰기)"
echo ""

DISP_ALL=$(grep -oP 'g_hash_table_insert\(g_rpc_routes,\s*"\K[^"]+' "$DISPATCHER" | sort)
DISP_DUPES=$(echo "$DISP_ALL" | uniq -d)

if [ -n "$DISP_DUPES" ]; then
    while IFS= read -r dup; do
        COUNT=$(echo "$DISP_ALL" | grep -cx "$dup")
        LINES=$(grep -n "g_hash_table_insert(g_rpc_routes, \"$dup\"" "$DISPATCHER" | cut -d: -f1 | tr '\n' ',' | sed 's/,$//')
        warn "Duplicate registration: '$dup' registered ${COUNT} times (lines: $LINES)"
    done <<< "$DISP_DUPES"
else
    pass "No duplicate method registrations in dispatcher"
fi

echo ""


echo -e "${CYAN}[4/4] Frontend EP 레지스트리 검증${NC}"
echo "  프론트엔드 모듈에서 EP 함수 없이 API_BASE를 직접 사용하는 곳 검출"
echo ""


STALE_COUNT=0
for jsfile in "$PROJECT_ROOT"/ui/modules/*.js; do
    basename=$(basename "$jsfile")

    case "$basename" in
        api.js|endpoints.js|theme.js|help.js|nav.js|ui.js|i18n.js|sw.js) continue ;;
    esac

    STALE=$(grep -c "API_BASE + '" "$jsfile" 2>/dev/null || true)
    STALE=${STALE:-0}
    if [ "$STALE" -gt 0 ] 2>/dev/null; then
        warn "$basename: ${STALE} hardcoded API_BASE paths (should use EP registry)"

        grep -n "API_BASE + '" "$jsfile" | head -5 | while IFS= read -r line; do
            echo "    $line"
        done
        STALE_COUNT=$((STALE_COUNT + STALE))
    fi
done

if [ "$STALE_COUNT" -eq 0 ]; then
    pass "All frontend modules use EP registry (0 hardcoded API_BASE paths)"
fi


EP_TOTAL=0
for jsfile in "$PROJECT_ROOT"/ui/modules/*.js; do
    basename=$(basename "$jsfile")
    case "$basename" in endpoints.js) continue ;; esac
    count=$(grep -c "EP\." "$jsfile" 2>/dev/null || true)
    count=${count:-0}
    EP_TOTAL=$((EP_TOTAL + count))
done

echo ""


echo "═══════════════════════════════════════════════════════════════"
echo -e "  ${GREEN}PASS${NC}: $PASS_COUNT  ${YELLOW}WARN${NC}: $WARN_COUNT  ${RED}FAIL${NC}: $FAIL_COUNT"
echo ""
echo "  REST RPC methods:       $REST_TOTAL"
echo "  Dispatcher methods:     $DISP_TOTAL"
echo "  UDS-only methods:       $DISP_ONLY_COUNT"
echo "  Frontend EP references: $EP_TOTAL"
echo "═══════════════════════════════════════════════════════════════"

if [ "$FAIL_COUNT" -gt 0 ]; then
    echo ""
    echo -e "${RED}CRITICAL: $FAIL_COUNT REST→RPC mismatches will cause -32601 errors at runtime!${NC}"
    $CI_MODE && exit 1
fi

if $CI_MODE && [ "$WARN_COUNT" -gt 0 ]; then
    echo ""
    echo -e "${YELLOW}INFO: $WARN_COUNT warnings detected (review recommended)${NC}"
fi

exit 0
