#!/usr/bin/env bash
                                                                               
                                   
                                                                               
      
                                                            
                                                            
                                                   
                                                  
                                                        
 
                          
                                                                     
                                                                  
                                                             
                                                              
                                                                               
                                              
 
                      
                                                 
                                                            
                                                                               
set -euo pipefail

if [[ -n "${PCV_NODES:-}" ]]; then
    read -ra NODES <<<"$PCV_NODES"
else
    NODES=()
fi
NODE_NAMES=()
for i in "${!NODES[@]}"; do NODE_NAMES+=("Node$((i + 1))"); done
LOCAL_IP="${PCV_LOCAL_IP:-127.0.0.1}"
SSH_USER="${PCV_SSH_USER:-pcvdev}"
SSH_BIN="${PCV_SSH_BIN:-/usr/bin/ssh}"

          
TARGET=""                     
LEVEL=""                       
DOMAIN=""                       
COUNT=100
SINCE=""
ERRORS_ONLY=0

RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo "  -n NODE     Use 'local' or a 1-based index from PCV_NODES."
    echo "  -l LEVEL    Filter: info, warn, crit, audit"
    echo "  -d DOMAIN   Filter domain: dispatcher, rest_server, etcd, cluster, etc."
    echo "  -c COUNT    Number of lines (default: 100)"
    echo "  -s SINCE    Time range: '1 hour ago', '30 min ago', 'today'"
    echo "  --errors    Show WARN/CRIT only, deduplicated"
    echo "  -h          Help"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -n) TARGET="$2"; shift 2 ;;
        -l) LEVEL="$2"; shift 2 ;;
        -d) DOMAIN="$2"; shift 2 ;;
        -c) COUNT="$2"; shift 2 ;;
        -s) SINCE="$2"; shift 2 ;;
        --errors) ERRORS_ONLY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown: $1" >&2; usage >&2; exit 2 ;;
    esac
done

SERVICE_NAME="${SERVICE_NAME:-purecvisorsd}"

                                                  
[[ "$COUNT" =~ ^[1-9][0-9]{0,5}$ ]] || { echo "COUNT must be 1..999999" >&2; exit 2; }
[[ "$LEVEL" =~ ^(info|warn|crit|audit)?$ ]] || { echo "invalid LEVEL" >&2; exit 2; }
[[ "$DOMAIN" =~ ^[A-Za-z0-9_.:-]*$ ]] || { echo "invalid DOMAIN" >&2; exit 2; }
[[ "$SERVICE_NAME" =~ ^[A-Za-z0-9_.@-]+$ ]] || { echo "invalid SERVICE_NAME" >&2; exit 2; }
[[ "$SSH_USER" =~ ^[A-Za-z_][A-Za-z0-9_.-]*$ ]] || { echo "invalid PCV_SSH_USER" >&2; exit 2; }
if [[ -n "$TARGET" && "$TARGET" != "local" ]]; then
    [[ "$TARGET" =~ ^[1-9][0-9]*$ ]] || { echo "NODE must be local or a positive index" >&2; exit 2; }
    (( TARGET <= ${#NODES[@]} )) || { echo "NODE index is not configured in PCV_NODES" >&2; exit 2; }
fi
for node in "${NODES[@]}"; do
    [[ "$node" =~ ^[A-Za-z0-9][A-Za-z0-9_.:-]*$ ]] || { echo "invalid node in PCV_NODES" >&2; exit 2; }
done

JOURNAL_ARGS=(sudo journalctl -u "$SERVICE_NAME" --no-pager --output=cat)
if [[ -n "$SINCE" ]]; then
    JOURNAL_ARGS+=(--since "$SINCE")
else
    JOURNAL_ARGS+=(-n "$COUNT")
fi
LEVEL_UP="${LEVEL^^}"

filter_stream() {
                                                          
    awk -v errors="$ERRORS_ONLY" -v level="$LEVEL_UP" -v domain="$DOMAIN" '
        errors && index($0, "\"lvl\":\"WARN\"") == 0 &&
                  index($0, "\"lvl\":\"CRIT\"") == 0 { next }
        !errors && level != "" && index($0, "\"lvl\":\"" level "\"") == 0 { next }
        domain != "" && index($0, "\"dom\":\"" domain "\"") == 0 { next }
        { print }
    '
}

colorize_stream() {
    while IFS= read -r line; do
        if [[ "$line" == *'"lvl":"CRIT"'* ]]; then
            echo -e "${RED}${line}${NC}"
        elif [[ "$line" == *'"lvl":"WARN"'* ]]; then
            echo -e "${YELLOW}${line}${NC}"
        else
            echo "$line"
        fi
    done
}

query_node() {
    local name=$1 mode=$2 host=${3:-} remote_command=""
    echo -e "${CYAN}═══ $name ═══${NC}"
    if [[ "$mode" == "local" ]]; then
        "${JOURNAL_ARGS[@]}" 2>/dev/null | filter_stream | colorize_stream
    else
                                                           
                                            
        printf -v remote_command '%q ' "${JOURNAL_ARGS[@]}"
        "$SSH_BIN" -o ConnectTimeout=3 -- "${SSH_USER}@${host}" "$remote_command" \
            2>/dev/null | filter_stream | colorize_stream
    fi
    echo ""
}

         
if [[ "$TARGET" == "local" ]]; then
    query_node "$LOCAL_IP (Local-Dev)" local
elif [[ -n "$TARGET" ]]; then
    idx=$((TARGET - 1))
    query_node "${NODE_NAMES[$idx]} (${NODES[$idx]})" remote "${NODES[$idx]}"
else
    for i in "${!NODES[@]}"; do
        query_node "${NODE_NAMES[$i]} (${NODES[$i]})" remote "${NODES[$i]}"
    done
    query_node "Local-Dev ($LOCAL_IP)" local
fi
