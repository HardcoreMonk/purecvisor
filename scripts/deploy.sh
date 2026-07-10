#!/usr/bin/env bash
# =============================================================================
# PureCVisor Single Edge Build & Deploy Script
# =============================================================================
set -euo pipefail

# 환경변수로 원격 단일 노드 목록을 지정할 수 있다:
#   PCV_NODES="192.0.2.53" scripts/deploy.sh
#   PCV_LOCAL_IP=192.0.2.50 scripts/deploy.sh
if [ -n "${PCV_NODES:-}" ]; then
    read -ra NODES <<< "$PCV_NODES"
else
    NODES=()
fi
NODE_NAMES=("Node1" "Node2" "Node3")
LOCAL_IP="${PCV_LOCAL_IP:-127.0.0.1}"
LOCAL_NAME="Local-Dev"
SSH_USER="${PCV_SSH_USER:-pcvdev}"
INSTALL_DIR="/usr/local/bin"
UI_DIR="/usr/local/share/purecvisor/ui"
EDITION="single"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# --- Color helpers ---
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[-]${NC} $*"; }

# --- Parse arguments ---
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
BINS=("$DAEMON_BIN" pcvctl pcvtui)

# --- Build ---
if [[ $SKIP_BUILD -eq 0 ]]; then
    info "Building ($BUILD_MODE, $EDITION)..."
    cd "$PROJECT_DIR"
    make clean
    if [[ "$BUILD_MODE" == "release" ]]; then
        make release 2>&1
    else
        make "$MAKE_TARGET" 2>&1
    fi

    # Verify binaries exist
    for bin in "${BINS[@]}"; do
        if [[ ! -f "bin/$bin" ]]; then
            error "Binary not found: bin/$bin"
            exit 1
        fi
    done
    info "Build complete. Binaries:"
    ls -lh "bin/$DAEMON_BIN" bin/pcvctl bin/pcvtui
fi

# --- Pre-deploy: ZFS version check ---
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
    ZFS_MISMATCH=$(/usr/bin/ssh -o ConnectTimeout=5 "${SSH_USER}@${ip}" \
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

# --- Deploy to each node ---
deploy_node() {
    local idx=$1
    local ip=${NODES[$idx]}
    local name=${NODE_NAMES[$idx]}

    info "[$name] Deploying to $ip..."

    # Upload binaries + UI
    if ! scp -o ConnectTimeout=5 "bin/$DAEMON_BIN" bin/pcvctl bin/pcvtui \
         "${SSH_USER}@${ip}:/tmp/" 2>/dev/null; then
        error "[$name] SCP failed to $ip"
        return 1
    fi
    # Upload all UI files (root + modules/)
    for ui_file in index.html style.css app.js app.bundle.js i18n.js sw.js manifest.json guide.html guide-content.md icon-192.png icon-512.png; do
        if [ -f "${PROJECT_DIR}/ui/${ui_file}" ]; then
            scp -o ConnectTimeout=5 "${PROJECT_DIR}/ui/${ui_file}" \
                "${SSH_USER}@${ip}:/tmp/pcv_ui_${ui_file}" 2>/dev/null || true
        fi
    done
    # Upload vendored third-party browser assets (self-hosted to keep CSP tight)
    if [ -d "${PROJECT_DIR}/ui/vendor" ]; then
        /usr/bin/ssh -o ConnectTimeout=5 "${SSH_USER}@${ip}" "mkdir -p /tmp/pcv_ui_vendor" 2>/dev/null || true
        scp -r -o ConnectTimeout=5 "${PROJECT_DIR}/ui/vendor/"* \
            "${SSH_USER}@${ip}:/tmp/pcv_ui_vendor/" 2>/dev/null || true
    fi
    # Upload UI modules
    if [ -d "${PROJECT_DIR}/ui/modules" ]; then
        /usr/bin/ssh -o ConnectTimeout=5 "${SSH_USER}@${ip}" "mkdir -p /tmp/pcv_ui_modules" 2>/dev/null || true
        scp -o ConnectTimeout=5 "${PROJECT_DIR}/ui/modules/"*.js \
            "${SSH_USER}@${ip}:/tmp/pcv_ui_modules/" 2>/dev/null || true
    fi
    # Upload UI samples linked from the guide and DESIGN.md
    if [ -d "${PROJECT_DIR}/ui/samples" ]; then
        /usr/bin/ssh -o ConnectTimeout=5 "${SSH_USER}@${ip}" "mkdir -p /tmp/pcv_ui_samples" 2>/dev/null || true
        scp -r -o ConnectTimeout=5 "${PROJECT_DIR}/ui/samples/"* \
            "${SSH_USER}@${ip}:/tmp/pcv_ui_samples/" 2>/dev/null || true
    fi
    scp -o ConnectTimeout=5 "${PROJECT_DIR}/systemd/purecvisor.logrotate" \
        "${SSH_USER}@${ip}:/tmp/purecvisor.logrotate" 2>/dev/null || true

    # Stop, copy, start
    if ! ssh -o ConnectTimeout=5 "${SSH_USER}@${ip}" bash -s -- "$DAEMON_BIN" "$SERVICE" <<'REMOTE_EOF'
        set -e
        DAEMON_BIN="$1"
        SERVICE="$2"
        sudo systemctl stop "$SERVICE" 2>/dev/null || true
        # 에디션별 공식 바이너리명만 설치한다.
        # legacy 공용 데몬 심링크는 더 이상 기본 호환 경로로 유지하지 않는다.
        sudo cp "/tmp/$DAEMON_BIN" "/usr/local/bin/$DAEMON_BIN"
        sudo cp /tmp/pcvctl /tmp/pcvtui /usr/local/bin/
        sudo chmod 755 "/usr/local/bin/$DAEMON_BIN" /usr/local/bin/pcvctl /usr/local/bin/pcvtui
        # Deploy Web UI (root files + modules/)
        sudo mkdir -p /usr/local/share/purecvisor/ui/modules
        for ui_file in index.html style.css app.js app.bundle.js i18n.js sw.js manifest.json guide.html guide-content.md icon-192.png icon-512.png; do
            if [ -f "/tmp/pcv_ui_${ui_file}" ]; then
                sudo cp "/tmp/pcv_ui_${ui_file}" "/usr/local/share/purecvisor/ui/${ui_file}"
                rm -f "/tmp/pcv_ui_${ui_file}"
            fi
        done
        # Deploy vendored browser assets
        if [ -d /tmp/pcv_ui_vendor ] && ls /tmp/pcv_ui_vendor/* >/dev/null 2>&1; then
            sudo mkdir -p /usr/local/share/purecvisor/ui/vendor
            sudo cp -a /tmp/pcv_ui_vendor/. /usr/local/share/purecvisor/ui/vendor/
            rm -rf /tmp/pcv_ui_vendor
        fi
        # Deploy UI modules
        if [ -d /tmp/pcv_ui_modules ] && ls /tmp/pcv_ui_modules/*.js >/dev/null 2>&1; then
            sudo cp /tmp/pcv_ui_modules/*.js /usr/local/share/purecvisor/ui/modules/
            rm -rf /tmp/pcv_ui_modules
        fi
        # Deploy UI samples
        if [ -d /tmp/pcv_ui_samples ] && ls /tmp/pcv_ui_samples/* >/dev/null 2>&1; then
            sudo mkdir -p /usr/local/share/purecvisor/ui/samples
            sudo cp -a /tmp/pcv_ui_samples/. /usr/local/share/purecvisor/ui/samples/
            rm -rf /tmp/pcv_ui_samples
        fi
        # Deploy logrotate config
        if [ -f /tmp/purecvisor.logrotate ]; then
            sudo cp /tmp/purecvisor.logrotate /etc/logrotate.d/purecvisor
            rm -f /tmp/purecvisor.logrotate
        fi
        sudo systemctl start "$SERVICE"
        rm -f "/tmp/$DAEMON_BIN" /tmp/pcvctl /tmp/pcvtui
        echo "OK"
REMOTE_EOF
    then
        error "[$name] Deploy failed on $ip"
        return 1
    fi

    info "[$name] Deploy OK — $ip"
}

DEPLOY_COUNT=0
FAIL_COUNT=0

for i in "${!NODES[@]}"; do
    # Filter nodes if specified
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
    fi
done

# --- Local Dev Server Deploy ---
if [[ $NO_LOCAL -eq 0 ]]; then
    # Deploy if --nodes not specified, or --nodes contains "local"
    if [[ -z "$NODES_FILTER" ]] || [[ "$NODES_FILTER" =~ local ]]; then
        info "[$LOCAL_NAME] Deploying to local ($LOCAL_IP)..."
        sudo systemctl stop "$SERVICE" 2>/dev/null || true
        sleep 1
        sudo cp "bin/$DAEMON_BIN" "$INSTALL_DIR/$DAEMON_BIN"
        sudo cp bin/pcvctl bin/pcvtui "$INSTALL_DIR/"
        sudo chmod 755 "$INSTALL_DIR/$DAEMON_BIN" "$INSTALL_DIR/pcvctl" "$INSTALL_DIR/pcvtui"
        sudo mkdir -p "$UI_DIR/modules"
        for ui_file in index.html style.css app.js app.bundle.js i18n.js sw.js manifest.json guide.html guide-content.md icon-192.png icon-512.png; do
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
        sudo systemctl start "$SERVICE"
        local_status=$(sudo systemctl is-active "$SERVICE" 2>/dev/null || echo "UNKNOWN")
        if [[ "$local_status" == "active" ]]; then
            info "[$LOCAL_NAME] Deploy OK — $LOCAL_IP"
            DEPLOY_COUNT=$((DEPLOY_COUNT + 1))
        else
            error "[$LOCAL_NAME] Deploy failed — $local_status"
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
    fi
fi

# --- Verify ---
echo ""
info "=== Deployment Summary ==="
info "Deployed: $DEPLOY_COUNT nodes, Failed: $FAIL_COUNT nodes"

if [[ $FAIL_COUNT -gt 0 ]]; then
    error "Some deployments failed!"
    exit 1
fi

# Quick health check
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
    status=$(ssh -o ConnectTimeout=3 "${SSH_USER}@${ip}" "sudo systemctl is-active $SERVICE" 2>/dev/null || echo "UNKNOWN")
    if [[ "$status" == "active" ]]; then
        info "[$name] $ip: ${GREEN}active${NC}"
    else
        error "[$name] $ip: $status"
    fi
done

# Local health check
if [[ $NO_LOCAL -eq 0 ]] && { [[ -z "$NODES_FILTER" ]] || [[ "$NODES_FILTER" =~ local ]]; }; then
    local_status=$(sudo systemctl is-active "$SERVICE" 2>/dev/null || echo "UNKNOWN")
    if [[ "$local_status" == "active" ]]; then
        info "[$LOCAL_NAME] $LOCAL_IP: ${GREEN}active${NC}"
    else
        error "[$LOCAL_NAME] $LOCAL_IP: $local_status"
    fi
fi
