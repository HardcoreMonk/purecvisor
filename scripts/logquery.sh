#!/usr/bin/env bash
# =============================================================================
# PureCVisor Single Edge 로그 조회 유틸리티
# =============================================================================
# 사용법:
#   scripts/logquery.sh                     # local 최근 에러/경고
#   scripts/logquery.sh -l warn -c 50      # WARN 레벨, 최근 50줄
#   scripts/logquery.sh -d dispatcher      # 특정 도메인
#   scripts/logquery.sh -s "1 hour ago"    # 시간 범위
#   scripts/logquery.sh --errors           # 에러만 (중복 제거)
# =============================================================================
set -euo pipefail

NODES=()
NODE_NAMES=()
LOCAL_IP="${PCV_LOCAL_IP:-127.0.0.1}"
SSH_USER="pcvdev"

# Defaults
TARGET=""          # all nodes
LEVEL=""           # all levels
DOMAIN=""          # all domains
COUNT=100
SINCE=""
ERRORS_ONLY=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo "  -n NODE     Use 'local'. Remote nodes require PCV_NODES in this script."
    echo "  -l LEVEL    Filter: info, warn, crit, audit"
    echo "  -d DOMAIN   Filter domain: dispatcher, rest_server, etcd, cluster, etc."
    echo "  -c COUNT    Number of lines (default: 100)"
    echo "  -s SINCE    Time range: '1 hour ago', '30 min ago', 'today'"
    echo "  --errors    Show WARN/CRIT only, deduplicated"
    echo "  -h          Help"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -n) TARGET="$2"; shift 2 ;;
        -l) LEVEL="$2"; shift 2 ;;
        -d) DOMAIN="$2"; shift 2 ;;
        -c) COUNT="$2"; shift 2 ;;
        -s) SINCE="$2"; shift 2 ;;
        --errors) ERRORS_ONLY=1; shift ;;
        -h|--help) usage ;;
        *) echo "Unknown: $1"; usage ;;
    esac
done

# Build journalctl command
SERVICE_NAME="${SERVICE_NAME:-purecvisorsd}"
JC="sudo journalctl -u ${SERVICE_NAME} --no-pager --output=cat"
[[ -n "$SINCE" ]] && JC="$JC --since '$SINCE'" || JC="$JC -n $COUNT"

# Build grep filter
GREP=""
if [[ $ERRORS_ONLY -eq 1 ]]; then
    GREP='grep -E "\"lvl\":\"(WARN|CRIT)\""'
elif [[ -n "$LEVEL" ]]; then
    LEVEL_UP=$(echo "$LEVEL" | tr '[:lower:]' '[:upper:]')
    GREP="grep '\"lvl\":\"$LEVEL_UP\"'"
fi
[[ -n "$DOMAIN" ]] && {
    [[ -n "$GREP" ]] && GREP="$GREP | grep '\"dom\":\"$DOMAIN\"'" || GREP="grep '\"dom\":\"$DOMAIN\"'"
}

query_node() {
    local name=$1 cmd=$2
    echo -e "${CYAN}═══ $name ═══${NC}"
    eval "$cmd" 2>/dev/null | while IFS= read -r line; do
        # Color by level
        if echo "$line" | grep -q '"lvl":"CRIT"'; then
            echo -e "${RED}$line${NC}"
        elif echo "$line" | grep -q '"lvl":"WARN"'; then
            echo -e "${YELLOW}$line${NC}"
        else
            echo "$line"
        fi
    done
    echo ""
}

# Execute
if [[ "$TARGET" == "local" ]]; then
    CMD="$JC"
    [[ -n "$GREP" ]] && CMD="$CMD | $GREP"
    query_node "$LOCAL_IP (Local-Dev)" "$CMD"
elif [[ -n "$TARGET" ]]; then
    idx=$((TARGET - 1))
    CMD="ssh -o ConnectTimeout=3 ${SSH_USER}@${NODES[$idx]} \"$JC\""
    [[ -n "$GREP" ]] && CMD="$CMD | $GREP"
    query_node "${NODE_NAMES[$idx]} (${NODES[$idx]})" "$CMD"
else
    # All nodes
    for i in "${!NODES[@]}"; do
        CMD="ssh -o ConnectTimeout=3 ${SSH_USER}@${NODES[$i]} \"$JC\""
        [[ -n "$GREP" ]] && CMD="$CMD | $GREP"
        query_node "${NODE_NAMES[$i]} (${NODES[$i]})" "$CMD"
    done
    # Local
    CMD="$JC"
    [[ -n "$GREP" ]] && CMD="$CMD | $GREP"
    query_node "Local-Dev ($LOCAL_IP)" "$CMD"
fi
