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
UI_ASSET_MANIFEST="$PROJECT_DIR/packaging/ui-assets.manifest"
RUNTIME_PREREQ_HELPER="$PROJECT_DIR/scripts/install-runtime-prereqs.sh"
ABI_PREFLIGHT_HELPER="$PROJECT_DIR/scripts/check-deploy-abi.sh"
NGINX_TERMINATION_HELPER="$PROJECT_DIR/scripts/install-nginx-termination.sh"
WAIT_FOR_LOCAL_IP_HELPER="$PROJECT_DIR/scripts/wait-for-local-ip.sh"
HOST_TUNING_HELPER="$PROJECT_DIR/scripts/install-host-tuning.sh"
HOST_TUNING_UNIT="$PROJECT_DIR/packaging/systemd/purecvisor-host-tuning.service"
VFIO_MODULES_FILE="$PROJECT_DIR/packaging/deb/purecvisor-vfio.conf"
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




UI_ASSET_SOURCES=()
load_ui_asset_manifest() {
    local source target policy extra entry_count=0
    local -A seen_sources=() seen_targets=()

    if [[ ! -r "$UI_ASSET_MANIFEST" ]]; then
        error "UI asset manifest missing or unreadable: $UI_ASSET_MANIFEST"
        return 1
    fi
    while read -r source target policy extra; do
        [[ -z "${source:-}" || "$source" == \#* ]] && continue
        if [[ -n "${extra:-}" ]] ||
           [[ ! "$source" =~ ^ui/[A-Za-z0-9][A-Za-z0-9._-]*$ ]] ||
           [[ ! "$target" =~ ^(ui|fallback)/[A-Za-z0-9][A-Za-z0-9._-]*$ ]] ||
           [[ "$policy" != "replace" && "$policy" != "seed" ]] ||
           [[ "$policy" == "seed" && "$target" != fallback/* ]]; then
            error "Invalid UI asset manifest entry: $source $target $policy ${extra:-}"
            return 1
        fi
        if [[ -n "${seen_targets[$target]+present}" ]]; then
            error "Duplicate UI asset manifest target: $target"
            return 1
        fi
        if [[ ! -f "$PROJECT_DIR/$source" ]]; then
            error "UI asset source missing: $source"
            return 1
        fi
        seen_targets[$target]=1
        if [[ -z "${seen_sources[$source]+present}" ]]; then
            UI_ASSET_SOURCES+=("$PROJECT_DIR/$source")
            seen_sources[$source]=1
        fi
        entry_count=$((entry_count + 1))
    done < "$UI_ASSET_MANIFEST"
    if (( entry_count == 0 )); then
        error "UI asset manifest is empty"
        return 1
    fi
}



install_local_ui_manifest_assets() {
    local source target policy extra source_path target_path target_dir staged_path

    while read -r source target policy extra; do
        [[ -z "${source:-}" || "$source" == \#* ]] && continue
        source_path="$PROJECT_DIR/$source"
        target_path="/usr/local/share/purecvisor/$target"
        target_dir="${target_path%/*}"
        sudo install -d -m 0755 -- "$target_dir"
        if [[ "$policy" == "seed" ]] && sudo test -e "$target_path"; then
            continue
        fi
        staged_path="${target_path}.pcv-install.$$"
        sudo install -m 0644 -- "$source_path" "$staged_path"
        if ! sudo mv -fT -- "$staged_path" "$target_path"; then
            sudo rm -f -- "$staged_path"
            return 1
        fi
    done < "$UI_ASSET_MANIFEST"
}

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
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --edition|--nodes)
            if [[ $# -lt 2 || -z "$2" || "$2" == -* ]]; then
                error "Missing value for $1" >&2
                usage 2 >&2
            fi
            if [[ "$1" == --edition ]]; then EDITION="$2"; else NODES_FILTER="$2"; fi
            shift 2 ;;
        --debug)      BUILD_MODE="debug"; shift ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --no-local)   NO_LOCAL=1; shift ;;
        -h|--help)    usage ;;
        *)            error "Unknown option: $1" >&2; usage 2 >&2 ;;
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

load_ui_asset_manifest || exit 1

               
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
if [[ ! -x "$ABI_PREFLIGHT_HELPER" ]]; then
    error "Deploy ABI preflight helper missing or not executable"
    exit 1
fi
if [[ ! -r "$VFIO_MODULES_FILE" ]]; then
    error "VFIO modules-load prerequisite missing: $VFIO_MODULES_FILE"
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

cleanup_remote_abi_artifacts() {
    local ip="$1"

    "$SSH_BIN" -o ConnectTimeout=5 "${SSH_USER}@${ip}" bash -s -- \
        "/tmp/$DAEMON_BIN" /tmp/pcvctl /tmp/check-deploy-abi.sh \
        <<'ABI_CLEANUP_EOF' 2>/dev/null || true
set -euo pipefail

[[ "$1" == /tmp/purecvisorsd ]] || exit 2
[[ "$2" == /tmp/pcvctl ]] || exit 2
[[ "$3" == /tmp/check-deploy-abi.sh ]] || exit 2
rm -f -- "$1" "$2" "$3"
ABI_CLEANUP_EOF
}

                             
deploy_node() {
    local idx=$1
    local ip=${NODES[$idx]}
    local name=${NODE_NAMES[$idx]}

    info "[$name] Deploying to $ip..."

                          
    if ! "$SCP_BIN" -o ConnectTimeout=5 "bin/$DAEMON_BIN" bin/pcvctl \
         "$ABI_PREFLIGHT_HELPER" \
         "${SSH_USER}@${ip}:/tmp/" 2>/dev/null; then
        cleanup_remote_abi_artifacts "$ip"
        error "[$name] SCP failed to $ip"
        return 1
    fi




    local abi_response
    if ! abi_response=$(
        "$SSH_BIN" -o ConnectTimeout=5 "${SSH_USER}@${ip}" bash -s -- \
            "/tmp/$DAEMON_BIN" /tmp/pcvctl /tmp/check-deploy-abi.sh \
            <<'ABI_PREFLIGHT_EOF'
set -euo pipefail
DAEMON_PATH="$1"
CLI_PATH="$2"
ABI_HELPER="$3"
cleanup_abi_preflight() {
    local saved_rc=$?
    rm -f -- "$ABI_HELPER"
    if (( saved_rc != 0 )); then
        rm -f -- "$DAEMON_PATH" "$CLI_PATH"
    fi
    return "$saved_rc"
}
trap cleanup_abi_preflight EXIT
[[ "$DAEMON_PATH" == /tmp/purecvisorsd ]] || exit 2
[[ "$CLI_PATH" == /tmp/pcvctl ]] || exit 2
[[ "$ABI_HELPER" == /tmp/check-deploy-abi.sh ]] || exit 2
[[ -f "$ABI_HELPER" && ! -L "$ABI_HELPER" ]] || exit 1
abi_result="$(bash "$ABI_HELPER" "$DAEMON_PATH" "$CLI_PATH")"
[[ "$abi_result" == 'PCV_DEPLOY_ABI_PREFLIGHT=OK binaries=2' ]] || exit 1
printf '%s\n' "$abi_result"
ABI_PREFLIGHT_EOF
    ); then
        cleanup_remote_abi_artifacts "$ip"
        error "[$name] Remote ABI preflight failed before service stop"
        return 1
    fi
    if [[ "$abi_response" != "PCV_DEPLOY_ABI_PREFLIGHT=OK binaries=2" ]]; then
        cleanup_remote_abi_artifacts "$ip"
        error "[$name] Remote ABI preflight returned an invalid response"
        return 1
    fi
    info "[$name] Remote ABI preflight PASS (daemon + CLI)"



                                                                                
    if [ -d "${PROJECT_DIR}/ui/vendor" ]; then
        "$SSH_BIN" -o ConnectTimeout=5 "${SSH_USER}@${ip}" "mkdir -p /tmp/pcv_ui_vendor" 2>/dev/null || true
        "$SCP_BIN" -r -o ConnectTimeout=5 "${PROJECT_DIR}/ui/vendor/"* \
            "${SSH_USER}@${ip}:/tmp/pcv_ui_vendor/" 2>/dev/null || true
    fi


    if [ -d "${PROJECT_DIR}/ui/assets" ]; then
        "$SSH_BIN" -o ConnectTimeout=5 "${SSH_USER}@${ip}" "mkdir -p /tmp/pcv_ui_assets" 2>/dev/null || true
        "$SCP_BIN" -r -o ConnectTimeout=5 "${PROJECT_DIR}/ui/assets/"* \
            "${SSH_USER}@${ip}:/tmp/pcv_ui_assets/" 2>/dev/null || true
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
    mkdir -m 0700 -- "$1/ui"
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
        "${UI_ASSET_SOURCES[@]}" \
        "${SSH_USER}@${ip}:${runtime_stage}/ui/" 2>/dev/null; then
        cleanup_remote_runtime_stage "$ip" "$runtime_stage"
        error "[$name] Required UI asset SCP failed to $ip"
        return 1
    fi
    if ! "$SCP_BIN" -o ConnectTimeout=5 \
        "$RUNTIME_PREREQ_HELPER" \
        "$BPF_OBJECT" \
        "$BPF_SHARED_OBJECT" \
        "$BPF_MANIFEST" \
        "$UI_ASSET_MANIFEST" \
        "$HOST_TUNING_HELPER" \
        "$HOST_TUNING_UNIT" \
        "$VFIO_MODULES_FILE" \
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



        install_ui_manifest_assets() {
            local stage_root="$1"
            local source target policy extra source_path target_path target_dir staged_path

            while read -r source target policy extra; do
                [[ -z "${source:-}" || "$source" == \#* ]] && continue
                if [[ -n "${extra:-}" ]] ||
                   [[ ! "$source" =~ ^ui/[A-Za-z0-9][A-Za-z0-9._-]*$ ]] ||
                   [[ ! "$target" =~ ^(ui|fallback)/[A-Za-z0-9][A-Za-z0-9._-]*$ ]] ||
                   [[ "$policy" != "replace" && "$policy" != "seed" ]] ||
                   [[ "$policy" == "seed" && "$target" != fallback/* ]]; then
                    echo "error: invalid staged UI asset manifest entry" >&2
                    return 1
                fi
                source_path="$stage_root/$source"
                target_path="/usr/local/share/purecvisor/$target"
                target_dir="${target_path%/*}"
                [[ -f "$source_path" && ! -L "$source_path" ]] || return 1
                sudo install -d -m 0755 -- "$target_dir"
                if [[ "$policy" == "seed" ]] && sudo test -e "$target_path"; then
                    continue
                fi
                staged_path="${target_path}.pcv-install.$$"
                sudo install -m 0644 -- "$source_path" "$staged_path"
                if ! sudo mv -fT -- "$staged_path" "$target_path"; then
                    sudo rm -f -- "$staged_path"
                    return 1
                fi
            done < "$RUNTIME_STAGE/ui-assets.manifest"
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
                                                                                \
                                                                  \
                                                     \
                                                                  \
                                                                                     \
                                                                                  \
                                                                        \
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
        chmod 0700 "$RUNTIME_STAGE" "$RUNTIME_STAGE/ui" \
            "$RUNTIME_STAGE/install-runtime-prereqs.sh" \
            "$RUNTIME_STAGE/install-host-tuning.sh"
        chmod 0600 "$RUNTIME_STAGE/pcv_lsm.bpf.o" \
            "$RUNTIME_STAGE/pcv_shared_bridge.bpf.o" \
            "$RUNTIME_STAGE/manifest.json" \
            "$RUNTIME_STAGE/ui-assets.manifest" \
            "$RUNTIME_STAGE"/ui/* \
            "$RUNTIME_STAGE/purecvisor-host-tuning.service" \
            "$RUNTIME_STAGE/purecvisor-vfio.conf"
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
        install_ui_manifest_assets "$RUNTIME_STAGE"
                                        
        if [ -d /tmp/pcv_ui_vendor ] && ls /tmp/pcv_ui_vendor/* >/dev/null 2>&1; then
            sudo mkdir -p /usr/local/share/purecvisor/ui/vendor
            sudo cp -a /tmp/pcv_ui_vendor/. /usr/local/share/purecvisor/ui/vendor/
            rm -rf /tmp/pcv_ui_vendor
        fi

        if [ -d /tmp/pcv_ui_assets ] && ls /tmp/pcv_ui_assets/* >/dev/null 2>&1; then
            sudo mkdir -p /usr/local/share/purecvisor/ui/assets
            sudo cp -a /tmp/pcv_ui_assets/. /usr/local/share/purecvisor/ui/assets/
            rm -rf /tmp/pcv_ui_assets
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



        sudo install -m 0644 "$RUNTIME_STAGE/purecvisor-vfio.conf" \
            /etc/modules-load.d/purecvisor-vfio.conf
        sudo modprobe vfio-pci
        sudo test -e /sys/bus/pci/drivers/vfio-pci/bind
                                                                         
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
        install_local_ui_manifest_assets
        if [ -d "${PROJECT_DIR}/ui/vendor" ]; then
            sudo mkdir -p "$UI_DIR/vendor"
            sudo cp -a "${PROJECT_DIR}/ui/vendor/." "$UI_DIR/vendor/"
        fi
        if [ -d "${PROJECT_DIR}/ui/assets" ]; then
            sudo mkdir -p "$UI_DIR/assets"
            sudo cp -a "${PROJECT_DIR}/ui/assets/." "$UI_DIR/assets/"
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
        sudo install -m 0644 "$VFIO_MODULES_FILE" \
            /etc/modules-load.d/purecvisor-vfio.conf
        sudo modprobe vfio-pci
        sudo test -e /sys/bus/pci/drivers/vfio-pci/bind
                                                                         
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
                                                                       \
                                                                                  \
                                                                \
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



    status=$("$SSH_BIN" -o ConnectTimeout=3 "${SSH_USER}@${ip}" "systemctl is-active $SERVICE" 2>/dev/null || echo "UNKNOWN")
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
