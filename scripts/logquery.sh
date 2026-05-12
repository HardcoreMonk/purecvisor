#!/usr/bin/env bash










set -euo pipefail

NODES=()
NODE_NAMES=()
LOCAL_IP="${PCV_LOCAL_IP:-127.0.0.1}"
SSH_USER="pcvdev"


TARGET=""
LEVEL=""
DOMAIN=""
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


SERVICE_NAME="${SERVICE_NAME:-purecvisorsd}"
JC="sudo journalctl -u ${SERVICE_NAME} --no-pager --output=cat"
[[ -n "$SINCE" ]] && JC="$JC --since '$SINCE'" || JC="$JC -n $COUNT"


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

    for i in "${!NODES[@]}"; do
        CMD="ssh -o ConnectTimeout=3 ${SSH_USER}@${NODES[$i]} \"$JC\""
        [[ -n "$GREP" ]] && CMD="$CMD | $GREP"
        query_node "${NODE_NAMES[$i]} (${NODES[$i]})" "$CMD"
    done

    CMD="$JC"
    [[ -n "$GREP" ]] && CMD="$CMD | $GREP"
    query_node "Local-Dev ($LOCAL_IP)" "$CMD"
fi
