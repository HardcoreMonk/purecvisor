#!/usr/bin/env bash
                                                                               
                                              
                                                                               
                          
                                                                   
                                                                        
                                                    
                                                                        
                                                                
                                                        
                                        
                                                      
                                                              
 
                      
                                                   
                                                      
                                                        
                                                
set -euo pipefail

                              
                                            
                                             
if [ -n "${PCV_NODES:-}" ]; then
    read -ra NODES <<< "$PCV_NODES"
else
    NODES=()
fi
NODE_NAMES=("Node1" "Node2" "Node3")
LOCAL_IP="${PCV_LOCAL_IP:-127.0.0.1}"
LOCAL_NAME="Local-Dev"
SSH_USER="${PCV_SSH_USER:-pcvdev}"
SSH_BIN="${PCV_SSH_BIN:-/usr/bin/ssh}"
SCP_BIN="${PCV_SCP_BIN:-/usr/bin/scp}"
INSTALL_DIR="/usr/local/bin"
UI_DIR="/usr/local/share/purecvisor/ui"
EDITION="single"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RUNTIME_PREREQ_HELPER="$PROJECT_DIR/scripts/install-runtime-prereqs.sh"
NGINX_TERMINATION_HELPER="$PROJECT_DIR/scripts/install-nginx-termination.sh"
WAIT_FOR_LOCAL_IP_HELPER="$PROJECT_DIR/scripts/wait-for-local-ip.sh"
HOST_TUNING_HELPER="$PROJECT_DIR/scripts/install-host-tuning.sh"
HOST_TUNING_UNIT="$PROJECT_DIR/packaging/systemd/purecvisor-host-tuning.service"
NGINX_BIND_IP="${PCV_NGINX_BIND_IP:-}"
                                                       
                                                            
NGINX_BIND_IP_SENTINEL="__PCV_NGINX_BIND_IP_EMPTY__"
NGINX_BIND_IP_ARG="${NGINX_BIND_IP:-$NGINX_BIND_IP_SENTINEL}"
NGINX_VERIFY_TIMEOUT="${PCV_NGINX_VERIFY_TIMEOUT:-60}"
BPF_STAGE="$PROJECT_DIR/build/bpf"
BPF_OBJECT="$PROJECT_DIR/build/bpf/pcv_lsm.bpf.o"
BPF_SHARED_OBJECT="$PROJECT_DIR/build/bpf/pcv_shared_bridge.bpf.o"
BPF_MANIFEST="$PROJECT_DIR/build/bpf/manifest.json"

                       
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[-]${NC} $*"; }

if [[ ! "$NGINX_VERIFY_TIMEOUT" =~ ^[1-9][0-9]*$ ]] ||
   (( NGINX_VERIFY_TIMEOUT > 60 )); then
                                                   
                                              
    error "PCV_NGINX_VERIFY_TIMEOUT must be an integer from 1 to 60"
    exit 2
fi

if [[ -n "$NGINX_BIND_IP" ]]; then
    if ! python3 - "$NGINX_BIND_IP" <<'PY'
import ipaddress
import sys
try:
    address = ipaddress.ip_address(sys.argv[1])
except ValueError:
    raise SystemExit(1)
raise SystemExit(0 if address.version == 4 and str(address) == sys.argv[1] else 1)
PY
    then
        error "PCV_NGINX_BIND_IP must be one canonical IPv4 literal"
        exit 2
    fi
    for helper in "$NGINX_TERMINATION_HELPER" "$WAIT_FOR_LOCAL_IP_HELPER"; do
        if [[ ! -x "$helper" ]]; then
            error "nginx termination helper missing or not executable"
            exit 1
        fi
    done
fi

                         
BUILD_MODE="release"
NODES_FILTER=""
SKIP_BUILD=0
NO_LOCAL=0

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --edition E     Edition: single only (default: single)"
    echo "  --debug         Build in debug mode (default: release)"
    echo "  --skip-build    Skip build, deploy existing binaries"
    echo "  --nodes N1,N2   Deploy only to specified nodes (1,2,3,local)"
    echo "  --no-local      Skip local dev server deployment"
    echo "  -h, --help      Show this help"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --edition)    EDITION="$2"; shift 2 ;;
        --debug)      BUILD_MODE="debug"; shift ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --nodes)      NODES_FILTER="$2"; shift 2 ;;
        --no-local)   NO_LOCAL=1; shift ;;
        -h|--help)    usage ;;
        *)            error "Unknown option: $1"; usage ;;
    esac
done

if [[ "$EDITION" != "single" ]]; then
    error "purecvisor-single supports --edition single only"
    exit 2
fi
DAEMON_BIN="purecvisorsd"
SERVICE="purecvisorsd"
MAKE_TARGET="single"
info "Edition: Single Edge (${DAEMON_BIN})"
BINS=("$DAEMON_BIN" pcvctl)

               
if [[ $SKIP_BUILD -eq 0 ]]; then
    info "Building ($BUILD_MODE, $EDITION)..."
    cd "$PROJECT_DIR"
    make clean
    if [[ "$BUILD_MODE" == "release" ]]; then
        make release 2>&1
    else
        make "$MAKE_TARGET" 2>&1
    fi
    make bpf 2>&1

    info "Build complete. Binaries:"
    ls -lh "bin/$DAEMON_BIN" bin/pcvctl
fi

                                                            
for bin in "${BINS[@]}"; do
    if [[ ! -f "bin/$bin" ]]; then
        error "Binary not found: bin/$bin"
        exit 1
    fi
done

                                                             
                                                    
for asset in "$BPF_OBJECT" "$BPF_SHARED_OBJECT" "$BPF_MANIFEST"; do
    if [[ ! -f "$asset" ]]; then
        error "BPF deploy asset missing: $asset"
        exit 1
    fi
done
if [[ ! -x "$RUNTIME_PREREQ_HELPER" ]]; then
    error "Runtime prerequisite helper missing or not executable"
    exit 1
fi
"$RUNTIME_PREREQ_HELPER" --verify-only --bpf-stage "$BPF_STAGE"

                                       
info "=== ZFS Version Check ==="
ZFS_CHECK_FAIL=0
for i in "${!NODES[@]}"; do
    if [[ -n "$NODES_FILTER" ]]; then
        node_num=$((i + 1))
        if [[ ! "$NODES_FILTER" =~ $node_num ]]; then
            continue
        fi
    fi
    ip=${NODES[$i]}; name=${NODE_NAMES[$i]}
                                                                
    ZFS_MISMATCH=$("$SSH_BIN" -o ConnectTimeout=5 "${SSH_USER}@${ip}" \
        'KMOD=$(cat /sys/module/zfs/version 2>/dev/null | grep -oP "^\d+\.\d+\.\d+"); \
         USER=$(zfs version 2>/dev/null | head -1 | grep -oP "\d+\.\d+\.\d+" || dpkg-query -W -f="${Version}" zfsutils-linux 2>/dev/null | grep -oP "^\d+\.\d+\.\d+"); \
         [ "$KMOD" != "$USER" ] && echo "MISMATCH kmod=$KMOD user=$USER" || echo "OK"' 2>/dev/null || echo "UNREACHABLE")
    if [[ "$ZFS_MISMATCH" == OK* ]]; then
        info "[$name] ZFS version OK"
    else
        warn "[$name] ZFS $ZFS_MISMATCH — fix the target node before production use"
        ZFS_CHECK_FAIL=1
    fi
done
if [[ $ZFS_CHECK_FAIL -eq 1 ]]; then
    warn "ZFS version mismatch detected. Deploy continues, but fix is recommended."
fi
echo ""

cleanup_remote_runtime_stage() {
    local ip="$1"
    local runtime_stage="$2"

    [[ "$runtime_stage" =~ ^/tmp/pcv_runtime_prereqs\.[0-9a-f]{32}$ ]] ||
        return 1
    "$SSH_BIN" -o ConnectTimeout=5 "${SSH_USER}@${ip}" \
        bash -s -- "$runtime_stage" <<'CLEANUP_EOF' 2>/dev/null || true
set -euo pipefail
[[ "$1" =~ ^/tmp/pcv_runtime_prereqs\.[0-9a-f]{32}$ ]] || exit 2
rm -rf -- "$1"
CLEANUP_EOF
}

                             
deploy_node() {
    local idx=$1
    local ip=${NODES[$idx]}
    local name=${NODE_NAMES[$idx]}

    info "[$name] Deploying to $ip..."

                          
    if ! "$SCP_BIN" -o ConnectTimeout=5 "bin/$DAEMON_BIN" bin/pcvctl \
         "${SSH_USER}@${ip}:/tmp/" 2>/dev/null; then
        error "[$name] SCP failed to $ip"
        return 1
    fi
                                           
    for ui_file in index.html style.css app.js app.bundle.js i18n.js sw.js manifest.json docs.html guide.html guide-content.md icon-192.png icon-512.png; do
        if [ -f "${PROJECT_DIR}/ui/${ui_file}" ]; then
            "$SCP_BIN" -o ConnectTimeout=5 "${PROJECT_DIR}/ui/${ui_file}" \
                "${SSH_USER}@${ip}:/tmp/pcv_ui_${ui_file}" 2>/dev/null || true
        fi
    done
                                                                                
    if [ -d "${PROJECT_DIR}/ui/vendor" ]; then
        "$SSH_BIN" -o ConnectTimeout=5 "${SSH_USER}@${ip}" "mkdir -p /tmp/pcv_ui_vendor" 2>/dev/null || true
        "$SCP_BIN" -r -o ConnectTimeout=5 "${PROJECT_DIR}/ui/vendor/"* \
            "${SSH_USER}@${ip}:/tmp/pcv_ui_vendor/" 2>/dev/null || true
    fi
                       
    if [ -d "${PROJECT_DIR}/ui/modules" ]; then
        "$SSH_BIN" -o ConnectTimeout=5 "${SSH_USER}@${ip}" "mkdir -p /tmp/pcv_ui_modules" 2>/dev/null || true
        "$SCP_BIN" -o ConnectTimeout=5 "${PROJECT_DIR}/ui/modules/"*.js \
            "${SSH_USER}@${ip}:/tmp/pcv_ui_modules/" 2>/dev/null || true
    fi
                                                           
    if [ -d "${PROJECT_DIR}/ui/samples" ]; then
        "$SSH_BIN" -o ConnectTimeout=5 "${SSH_USER}@${ip}" "mkdir -p /tmp/pcv_ui_samples" 2>/dev/null || true
        "$SCP_BIN" -r -o ConnectTimeout=5 "${PROJECT_DIR}/ui/samples/"* \
            "${SSH_USER}@${ip}:/tmp/pcv_ui_samples/" 2>/dev/null || true
    fi
    "$SCP_BIN" -o ConnectTimeout=5 "${PROJECT_DIR}/systemd/purecvisor.logrotate" \
        "${SSH_USER}@${ip}:/tmp/purecvisor.logrotate" 2>/dev/null || true

                                                         
                                                        
    local runtime_token runtime_stage stage_response
    runtime_token="$(python3 -c 'import secrets; print(secrets.token_hex(16))')"
    if [[ ! "$runtime_token" =~ ^[0-9a-f]{32}$ ]]; then
        error "[$name] Runtime staging token generation failed"
        return 1
    fi
    runtime_stage="/tmp/pcv_runtime_prereqs.${runtime_token}"
    if ! stage_response=$(
        "$SSH_BIN" -o ConnectTimeout=5 "${SSH_USER}@${ip}" \
            bash -s -- "$runtime_stage" <<'STAGE_EOF'
set -euo pipefail
[[ "$1" =~ ^/tmp/pcv_runtime_prereqs\.[0-9a-f]{32}$ ]] || exit 2
umask 077
mkdir -m 0700 -- "$1"
printf 'PCV_RUNTIME_STAGE=%s\n' "$1"
STAGE_EOF
    ); then
        cleanup_remote_runtime_stage "$ip" "$runtime_stage"
        error "[$name] Runtime staging creation failed on $ip"
        return 1
    fi
    if [[ "$stage_response" != "PCV_RUNTIME_STAGE=$runtime_stage" ]]; then
        cleanup_remote_runtime_stage "$ip" "$runtime_stage"
        error "[$name] Runtime staging returned an invalid response"
        return 1
    fi
    if ! "$SCP_BIN" -o ConnectTimeout=5 \
        "$RUNTIME_PREREQ_HELPER" \
        "$BPF_OBJECT" \
        "$BPF_SHARED_OBJECT" \
        "$BPF_MANIFEST" \
        "$HOST_TUNING_HELPER" \
        "$HOST_TUNING_UNIT" \
        "${SSH_USER}@${ip}:${runtime_stage}/" 2>/dev/null; then
        cleanup_remote_runtime_stage "$ip" "$runtime_stage"
        error "[$name] Runtime prerequisite SCP failed to $ip"
        return 1
    fi
    if [[ -n "$NGINX_BIND_IP" ]]; then
        if ! "$SCP_BIN" -o ConnectTimeout=5 \
            "$NGINX_TERMINATION_HELPER" \
            "$WAIT_FOR_LOCAL_IP_HELPER" \
            "${SSH_USER}@${ip}:${runtime_stage}/" 2>/dev/null; then
            cleanup_remote_runtime_stage "$ip" "$runtime_stage"
            error "[$name] nginx termination helper SCP failed to $ip"
            return 1
        fi
    fi

                                                              
                                                        
    if ! "$SSH_BIN" -o ConnectTimeout=5 "${SSH_USER}@${ip}" bash -s -- \
        "$DAEMON_BIN" "$SERVICE" "$runtime_stage" "$NGINX_BIND_IP_ARG" \
        "$NGINX_VERIFY_TIMEOUT" <<'REMOTE_EOF'
        set -euo pipefail
        DAEMON_BIN="$1"
        SERVICE="$2"
        RUNTIME_STAGE="$3"
        if [[ "$4" == "__PCV_NGINX_BIND_IP_EMPTY__" ]]; then
            NGINX_BIND_IP=""
        else
            NGINX_BIND_IP="$4"
        fi
        NGINX_VERIFY_TIMEOUT="$5"
        NGINX_DEPLOYMENT_ID=""
        NGINX_COMMITTED=0
        cleanup_runtime_stage() {
            rm -rf -- "$RUNTIME_STAGE"
        }
        rollback_nginx_transaction() {
            local saved_rc=$?
            if [[ -n "$NGINX_DEPLOYMENT_ID" && "$NGINX_COMMITTED" -eq 0 ]]; then
                if ! sudo "$RUNTIME_STAGE/install-nginx-termination.sh" \
                    --rollback "$NGINX_DEPLOYMENT_ID" >/dev/null 2>&1; then
                    echo "error: nginx transaction rollback failed; recovery state preserved" >&2
                    cleanup_runtime_stage
                    (( saved_rc != 0 )) || saved_rc=1
                    exit "$saved_rc"
                fi
                if ! timeout --foreground 10 sudo systemctl daemon-reload \
                    >/dev/null 2>&1 ||
                    ! timeout --foreground 190 sudo systemctl restart "$SERVICE" \
                    `# [AUD-F11] 데몬 restart = drain(기본 30s, conf 로 상향 가능) 포함 —` \
                    `# 10s 예산은 클라이언트만 죽이고 서비스는 뒤늦게 복귀해 "recovery` \
                    `# failed" 오탐을 낸다(.53 첫 배포 실측).` \
                    `# [XC-1] 90s 재산정 → 190s: 데몬 자신의 문서화된 최악 종료는` \
                    `# packaging/deb/purecvisorsd.service 의 TimeoutStopSec=180 주석대로` \
                    `# webpush 30s + suricata 헬스 join ~115s = ~145s 다. 90s 예산은 그` \
                    `# 구간에서 AUD-F11 오탐을 그대로 재발시킨다. 190 = 180 + 10s 마진.` \
                    >/dev/null 2>&1 ||
                    ! timeout --foreground 10 sudo systemctl restart nginx \
                    >/dev/null 2>&1; then
                    echo "error: nginx rollback service recovery failed; recovery state preserved" >&2
                    cleanup_runtime_stage
                    (( saved_rc != 0 )) || saved_rc=1
                    exit "$saved_rc"
                fi
                if ! sudo "$RUNTIME_STAGE/install-nginx-termination.sh" \
                    --finalize-rollback "$NGINX_DEPLOYMENT_ID" >/dev/null 2>&1; then
                    echo "error: nginx rollback finalization failed; recovery state preserved" >&2
                    cleanup_runtime_stage
                    (( saved_rc != 0 )) || saved_rc=1
                    exit "$saved_rc"
                fi
            fi
            cleanup_runtime_stage
            exit "$saved_rc"
        }
        trap rollback_nginx_transaction EXIT
        trap 'exit 130' INT
        trap 'exit 143' TERM
        [[ "$RUNTIME_STAGE" =~ ^/tmp/pcv_runtime_prereqs\.[0-9a-f]{32}$ ]] ||
            exit 2
        chmod 0700 "$RUNTIME_STAGE" \
            "$RUNTIME_STAGE/install-runtime-prereqs.sh" \
            "$RUNTIME_STAGE/install-host-tuning.sh"
        chmod 0600 "$RUNTIME_STAGE/pcv_lsm.bpf.o" \
            "$RUNTIME_STAGE/pcv_shared_bridge.bpf.o" \
            "$RUNTIME_STAGE/manifest.json" \
            "$RUNTIME_STAGE/purecvisor-host-tuning.service"
        if [[ -n "$NGINX_BIND_IP" ]]; then
            chmod 0700 "$RUNTIME_STAGE/install-nginx-termination.sh" \
                "$RUNTIME_STAGE/wait-for-local-ip.sh"
        fi
        sudo systemctl stop "$SERVICE" 2>/dev/null || true
                              
                                                    
        sudo cp "/tmp/$DAEMON_BIN" "/usr/local/bin/$DAEMON_BIN"
        sudo cp /tmp/pcvctl /usr/local/bin/
        sudo chmod 755 "/usr/local/bin/$DAEMON_BIN" /usr/local/bin/pcvctl
                                                           
                                                        
        sudo rm -f /usr/local/bin/pcvtui
                                               
        sudo mkdir -p /usr/local/share/purecvisor/ui/modules
                                                                   
                                                         
        sudo rm -f /usr/local/share/purecvisor/ui/bundle.js
        for ui_file in index.html style.css app.js app.bundle.js i18n.js sw.js manifest.json docs.html guide.html guide-content.md icon-192.png icon-512.png; do
            if [ -f "/tmp/pcv_ui_${ui_file}" ]; then
                sudo cp "/tmp/pcv_ui_${ui_file}" "/usr/local/share/purecvisor/ui/${ui_file}"
                rm -f "/tmp/pcv_ui_${ui_file}"
            fi
        done
                                        
        if [ -d /tmp/pcv_ui_vendor ] && ls /tmp/pcv_ui_vendor/* >/dev/null 2>&1; then
            sudo mkdir -p /usr/local/share/purecvisor/ui/vendor
            sudo cp -a /tmp/pcv_ui_vendor/. /usr/local/share/purecvisor/ui/vendor/
            rm -rf /tmp/pcv_ui_vendor
        fi
                           
        if [ -d /tmp/pcv_ui_modules ] && ls /tmp/pcv_ui_modules/*.js >/dev/null 2>&1; then
            sudo cp /tmp/pcv_ui_modules/*.js /usr/local/share/purecvisor/ui/modules/
            rm -rf /tmp/pcv_ui_modules
        fi
                           
        if [ -d /tmp/pcv_ui_samples ] && ls /tmp/pcv_ui_samples/* >/dev/null 2>&1; then
            sudo mkdir -p /usr/local/share/purecvisor/ui/samples
            sudo cp -a /tmp/pcv_ui_samples/. /usr/local/share/purecvisor/ui/samples/
            rm -rf /tmp/pcv_ui_samples
        fi
                                 
        if [ -f /tmp/purecvisor.logrotate ]; then
            sudo cp /tmp/purecvisor.logrotate /etc/logrotate.d/purecvisor
            rm -f /tmp/purecvisor.logrotate
        fi
        sudo "$RUNTIME_STAGE/install-runtime-prereqs.sh" --bpf-stage "$RUNTIME_STAGE"
                                                                         
        sudo "$RUNTIME_STAGE/install-host-tuning.sh" \
            --unit "$RUNTIME_STAGE/purecvisor-host-tuning.service" ||
            echo "[!] host-tuning unit install failed (non-blocking)"
        if [[ -n "$NGINX_BIND_IP" ]]; then
            install_result="$(
                sudo env \
                    PCV_WAIT_FOR_LOCAL_IP_SOURCE="$RUNTIME_STAGE/wait-for-local-ip.sh" \
                    "$RUNTIME_STAGE/install-nginx-termination.sh" \
                    --nginx-bind-ip "$NGINX_BIND_IP"
            )"
            [[ "$install_result" =~ ^PCV_NGINX_DEPLOYMENT_ID=([0-9a-f]{64})$ ]] ||
                exit 1
            NGINX_DEPLOYMENT_ID="${BASH_REMATCH[1]}"
            nginx_deadline=$((SECONDS + NGINX_VERIFY_TIMEOUT))
        fi
        if [[ -n "$NGINX_BIND_IP" ]]; then
            remaining=$((nginx_deadline - SECONDS))
            (( remaining > 0 )) || exit 1
            timeout --foreground "$remaining" sudo systemctl start "$SERVICE"
        else
            sudo systemctl start "$SERVICE"
        fi
        if [[ -n "$NGINX_BIND_IP" ]]; then
            remaining=$((nginx_deadline - SECONDS))
            (( remaining > 0 )) || exit 1
            timeout --foreground "$remaining" sudo systemctl restart nginx
            verified=0
            while (( SECONDS < nginx_deadline )); do
                remaining=$((nginx_deadline - SECONDS))
                (( remaining > 0 )) || break
                curl_max=5
                (( remaining < curl_max )) && curl_max="$remaining"
                if timeout --foreground "$remaining" sudo systemctl is-active --quiet "$SERVICE" &&
                   timeout --foreground "$remaining" sudo systemctl is-active --quiet nginx &&
                   timeout --foreground "$remaining" sudo ss -ltnH | awk -v ip="$NGINX_BIND_IP" '
                       $4 == ip ":80" { http=1 }
                       $4 == ip ":443" { https=1 }
                       END { exit !(http && https) }
                   ' &&
                   health="$(
                       curl --fail --silent --show-error \
                           --connect-timeout 3 --max-time "$curl_max" \
                           --max-filesize 1048576 \
                           --insecure "https://${NGINX_BIND_IP}/api/v1/health"
                   )" &&
                   python3 -c '
import json, sys
data = json.load(sys.stdin)
tls = data.get("checks", {}).get("tls", {})
ok = (
    tls.get("enabled") is False
    and tls.get("degraded") is False
    and tls.get("status") == "disabled_by_config"
    and tls.get("mode") == "external_termination"
)
raise SystemExit(0 if ok else 1)
' <<<"$health"; then
                    verified=1
                    break
                fi
                retry_sleep=2
                (( remaining < retry_sleep )) && retry_sleep="$remaining"
                sleep "$retry_sleep"
            done
            [[ "$verified" -eq 1 ]] || exit 1
            sudo "$RUNTIME_STAGE/install-nginx-termination.sh" \
                --commit "$NGINX_DEPLOYMENT_ID" >/dev/null
            NGINX_COMMITTED=1
            NGINX_DEPLOYMENT_ID=""
        fi
        rm -f "/tmp/$DAEMON_BIN" /tmp/pcvctl
        trap - EXIT INT TERM
        cleanup_runtime_stage
        echo "OK"
REMOTE_EOF
    then
        cleanup_remote_runtime_stage "$ip" "$runtime_stage"
        error "[$name] Deploy failed on $ip"
        return 1
    fi

    info "[$name] Deploy OK — $ip"
}

DEPLOY_COUNT=0
FAIL_COUNT=0
NODE_FAILED=()
LOCAL_FAILED=0

for i in "${!NODES[@]}"; do
                               
    if [[ -n "$NODES_FILTER" ]]; then
        node_num=$((i + 1))
        if [[ ! "$NODES_FILTER" =~ $node_num ]]; then
            continue
        fi
    fi

    if deploy_node "$i"; then
        DEPLOY_COUNT=$((DEPLOY_COUNT + 1))
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        NODE_FAILED[i]=1
    fi
done

                                 
if [[ $NO_LOCAL -eq 0 ]]; then
                                                                  
    if [[ -z "$NODES_FILTER" ]] || [[ "$NODES_FILTER" =~ local ]]; then
        info "[$LOCAL_NAME] Deploying to local ($LOCAL_IP)..."
        sudo systemctl stop "$SERVICE" 2>/dev/null || true
        sleep 1
        sudo cp "bin/$DAEMON_BIN" "$INSTALL_DIR/$DAEMON_BIN"
        sudo cp bin/pcvctl "$INSTALL_DIR/"
        sudo chmod 755 "$INSTALL_DIR/$DAEMON_BIN" "$INSTALL_DIR/pcvctl"
                                        
        sudo rm -f "$INSTALL_DIR/pcvtui"
        sudo mkdir -p "$UI_DIR/modules"
                                                                     
                                           
        sudo rm -f "$UI_DIR/bundle.js"
        for ui_file in index.html style.css app.js app.bundle.js i18n.js sw.js manifest.json docs.html guide.html guide-content.md icon-192.png icon-512.png; do
            [ -f "${PROJECT_DIR}/ui/${ui_file}" ] && sudo cp "${PROJECT_DIR}/ui/${ui_file}" "$UI_DIR/"
        done
        if [ -d "${PROJECT_DIR}/ui/vendor" ]; then
            sudo mkdir -p "$UI_DIR/vendor"
            sudo cp -a "${PROJECT_DIR}/ui/vendor/." "$UI_DIR/vendor/"
        fi
        [ -d "${PROJECT_DIR}/ui/modules" ] && sudo cp "${PROJECT_DIR}/ui/modules/"*.js "$UI_DIR/modules/"
        if [ -d "${PROJECT_DIR}/ui/samples" ]; then
            sudo mkdir -p "$UI_DIR/samples"
            sudo cp -a "${PROJECT_DIR}/ui/samples/." "$UI_DIR/samples/"
        fi
        if [ -f "${PROJECT_DIR}/systemd/purecvisor.logrotate" ]; then
            sudo cp "${PROJECT_DIR}/systemd/purecvisor.logrotate" /etc/logrotate.d/purecvisor
        fi
        sudo "$RUNTIME_PREREQ_HELPER" --bpf-stage "$BPF_STAGE"
                                                                         
        sudo "$HOST_TUNING_HELPER" --unit "$HOST_TUNING_UNIT" ||
            warn "[$LOCAL_NAME] host-tuning unit install failed (non-blocking)"
        local_nginx_deployment_id=""
        local_nginx_source_stage=""
        if [[ -n "$NGINX_BIND_IP" ]]; then
            cleanup_local_nginx_source_stage() {
                [[ "$local_nginx_source_stage" =~ ^/tmp/pcv_nginx_source\.[A-Za-z0-9]{10}$ ]] ||
                    return 1
                rm -rf -- "$local_nginx_source_stage"
                local_nginx_source_stage=""
            }
            local_nginx_source_stage="$(
                mktemp -d "/tmp/pcv_nginx_source.XXXXXXXXXX"
            )"
            chmod 0700 "$local_nginx_source_stage"
            install -m 0700 "$WAIT_FOR_LOCAL_IP_HELPER" \
                "$local_nginx_source_stage/wait-for-local-ip.sh"
            trap cleanup_local_nginx_source_stage EXIT
            trap 'exit 130' INT
            trap 'exit 143' TERM
            local_install_result="$(
                sudo env \
                    PCV_WAIT_FOR_LOCAL_IP_SOURCE="$local_nginx_source_stage/wait-for-local-ip.sh" \
                    "$NGINX_TERMINATION_HELPER" --nginx-bind-ip "$NGINX_BIND_IP"
            )"
            if [[ "$local_install_result" =~ ^PCV_NGINX_DEPLOYMENT_ID=([0-9a-f]{64})$ ]]; then
                local_nginx_deployment_id="${BASH_REMATCH[1]}"
                local_nginx_deadline=$((SECONDS + NGINX_VERIFY_TIMEOUT))
                local_nginx_cleanup() {
                    local saved_rc=$?
                    trap - EXIT INT TERM
                    if [[ -n "$local_nginx_deployment_id" ]]; then
                        if ! sudo "$NGINX_TERMINATION_HELPER" \
                            --rollback "$local_nginx_deployment_id" >/dev/null 2>&1; then
                            echo "error: nginx transaction rollback failed; recovery state preserved" >&2
                            [[ -z "$local_nginx_source_stage" ]] ||
                                cleanup_local_nginx_source_stage
                            (( saved_rc != 0 )) || saved_rc=1
                            exit "$saved_rc"
                        fi
                        if ! timeout --foreground 10 sudo systemctl daemon-reload \
                            >/dev/null 2>&1 ||
                            ! timeout --foreground 190 sudo systemctl restart "$SERVICE" \
                            `# [AUD-F11] 위 원격 경로와 동일 근거 — drain 포함 예산` \
                            `# [XC-1] 90s → 190s 재산정도 동일 근거(TimeoutStopSec=180 +` \
                            `# 10s 마진). 산정 근거는 위 원격 경로 주석이 정본.` \
                            >/dev/null 2>&1 ||
                            ! timeout --foreground 10 sudo systemctl restart nginx \
                            >/dev/null 2>&1; then
                            echo "error: nginx rollback service recovery failed; recovery state preserved" >&2
                            [[ -z "$local_nginx_source_stage" ]] ||
                                cleanup_local_nginx_source_stage
                            (( saved_rc != 0 )) || saved_rc=1
                            exit "$saved_rc"
                        fi
                        if ! sudo "$NGINX_TERMINATION_HELPER" \
                            --finalize-rollback "$local_nginx_deployment_id" \
                            >/dev/null 2>&1; then
                            echo "error: nginx rollback finalization failed; recovery state preserved" >&2
                            [[ -z "$local_nginx_source_stage" ]] ||
                                cleanup_local_nginx_source_stage
                            (( saved_rc != 0 )) || saved_rc=1
                            exit "$saved_rc"
                        fi
                    fi
                    [[ -z "$local_nginx_source_stage" ]] ||
                        cleanup_local_nginx_source_stage
                    exit "$saved_rc"
                }
                trap local_nginx_cleanup EXIT
                trap 'exit 130' INT
                trap 'exit 143' TERM
                cleanup_local_nginx_source_stage
            else
                error "[$LOCAL_NAME] nginx transaction returned invalid state"
                exit 1
            fi
        fi
        if [[ -n "$NGINX_BIND_IP" ]]; then
            local_remaining=$((local_nginx_deadline - SECONDS))
            (( local_remaining > 0 )) || exit 1
            timeout --foreground "$local_remaining" sudo systemctl start "$SERVICE"
        else
            sudo systemctl start "$SERVICE"
        fi
        if [[ -n "$NGINX_BIND_IP" ]]; then
            local_nginx_ok=0
            local_remaining=$((local_nginx_deadline - SECONDS))
            (( local_remaining > 0 )) || exit 1
            timeout --foreground "$local_remaining" sudo systemctl restart nginx
            while (( SECONDS < local_nginx_deadline )); do
                local_remaining=$((local_nginx_deadline - SECONDS))
                (( local_remaining > 0 )) || break
                local_curl_max=5
                (( local_remaining < local_curl_max )) &&
                    local_curl_max="$local_remaining"
                if timeout --foreground "$local_remaining" \
                       sudo systemctl is-active --quiet "$SERVICE" &&
                   timeout --foreground "$local_remaining" \
                       sudo systemctl is-active --quiet nginx &&
                   timeout --foreground "$local_remaining" sudo ss -ltnH |
                       awk -v ip="$NGINX_BIND_IP" '
                       $4 == ip ":80" { http=1 }
                       $4 == ip ":443" { https=1 }
                       END { exit !(http && https) }
                   ' &&
                   local_health="$(
                       curl --fail --silent --show-error --insecure \
                           --connect-timeout 3 --max-time "$local_curl_max" \
                           --max-filesize 1048576 \
                           "https://${NGINX_BIND_IP}/api/v1/health"
                   )" &&
                   python3 -c '
import json, sys
tls = json.load(sys.stdin).get("checks", {}).get("tls", {})
raise SystemExit(0 if tls.get("enabled") is False
                 and tls.get("degraded") is False
                 and tls.get("status") == "disabled_by_config"
                 and tls.get("mode") == "external_termination" else 1)
' <<<"$local_health"; then
                    local_nginx_ok=1
                    break
                fi
                local_sleep=2
                (( local_remaining < local_sleep )) && local_sleep="$local_remaining"
                sleep "$local_sleep"
            done
            if [[ "$local_nginx_ok" -eq 1 ]]; then
                sudo "$NGINX_TERMINATION_HELPER" \
                    --commit "$local_nginx_deployment_id" >/dev/null
                local_nginx_deployment_id=""
                trap - EXIT INT TERM
            else
                error "[$LOCAL_NAME] nginx transport verification failed"
                exit 1
            fi
        fi
        local_status=$(sudo systemctl is-active "$SERVICE" 2>/dev/null || echo "UNKNOWN")
        if [[ "$local_status" == "active" ]]; then
            info "[$LOCAL_NAME] Deploy OK — $LOCAL_IP"
            DEPLOY_COUNT=$((DEPLOY_COUNT + 1))
        else
            error "[$LOCAL_NAME] Deploy failed — $local_status"
            FAIL_COUNT=$((FAIL_COUNT + 1))
            LOCAL_FAILED=1
        fi
    fi
fi

                    
echo ""
info "=== Health Check ==="
for i in "${!NODES[@]}"; do
    if [[ -n "$NODES_FILTER" ]]; then
        node_num=$((i + 1))
        if [[ ! "$NODES_FILTER" =~ $node_num ]]; then
            continue
        fi
    fi
    ip=${NODES[$i]}
    name=${NODE_NAMES[$i]}
    status=$("$SSH_BIN" -o ConnectTimeout=3 "${SSH_USER}@${ip}" "sudo systemctl is-active $SERVICE" 2>/dev/null || echo "UNKNOWN")
    if [[ "$status" == "active" ]]; then
        info "[$name] $ip: ${GREEN}active${NC}"
    else
        error "[$name] $ip: $status"
        if [[ ${NODE_FAILED[i]:-0} -eq 0 ]]; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            NODE_FAILED[i]=1
        fi
    fi
done

                    
if [[ $NO_LOCAL -eq 0 ]] && { [[ -z "$NODES_FILTER" ]] || [[ "$NODES_FILTER" =~ local ]]; }; then
    local_status=$(sudo systemctl is-active "$SERVICE" 2>/dev/null || echo "UNKNOWN")
    if [[ "$local_status" == "active" ]]; then
        info "[$LOCAL_NAME] $LOCAL_IP: ${GREEN}active${NC}"
    else
        error "[$LOCAL_NAME] $LOCAL_IP: $local_status"
        if [[ $LOCAL_FAILED -eq 0 ]]; then
            FAIL_COUNT=$((FAIL_COUNT + 1))
            LOCAL_FAILED=1
        fi
    fi
fi

                                             
echo ""
info "=== Deployment Summary ==="
info "Deployed: $DEPLOY_COUNT nodes, Failed: $FAIL_COUNT nodes"
if [[ $FAIL_COUNT -gt 0 ]]; then
    error "Some deployments failed!"
    exit 1
fi
