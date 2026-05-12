#!/usr/bin/env bash












set -euo pipefail

if [ -n "${PCV_NODES:-}" ]; then
    read -ra NODES <<< "$PCV_NODES"
    NODE_NAMES=()
    for i in "${!NODES[@]}"; do NODE_NAMES+=("Node$((i+1))"); done
else
    NODES=()
    NODE_NAMES=()
fi
SSH_USER="${PCV_SSH_USER:-pcvdev}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[-]${NC} $*"; }
header(){ echo -e "${CYAN}=== $* ===${NC}"; }

MODE="local"
CHECK_ONLY=0
NODES_FILTER=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --remote)     MODE="remote"; shift ;;
        --check)      CHECK_ONLY=1; shift ;;
        --nodes)      NODES_FILTER="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--remote] [--check] [--nodes N1,N2]"
            echo ""
            echo "  --remote    Execute on all 3 cluster nodes via SSH"
            echo "  --check     Check version mismatch only (no changes)"
            echo "  --nodes     Target specific nodes (e.g., 1,2)"
            exit 0
            ;;
        *) error "Unknown option: $1"; exit 1 ;;
    esac
done




zfs_sync_payload() {
    cat <<'SYNC_SCRIPT'

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[-]${NC} $*"; }

CHECK_ONLY="${1:-0}"


KMOD_VER=$(cat /sys/module/zfs/version 2>/dev/null || echo "unknown")

ZFS_VER_OUTPUT=$(zfs version 2>/dev/null | head -1 || echo "")
if [[ -n "$ZFS_VER_OUTPUT" ]]; then
    USER_VER="$ZFS_VER_OUTPUT"
else
    USER_VER=$(dpkg-query -W -f='${Version}' zfsutils-linux 2>/dev/null || echo "unknown")
fi


KMOD_UPSTREAM=$(echo "$KMOD_VER" | grep -oP '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
USER_UPSTREAM=$(echo "$USER_VER" | grep -oP '[0-9]+\.[0-9]+\.[0-9]+' | head -1)

info "ZFS kernel module : $KMOD_VER (upstream: $KMOD_UPSTREAM)"
info "ZFS userland      : $USER_VER (upstream: $USER_UPSTREAM)"

if [[ "$KMOD_UPSTREAM" == "$USER_UPSTREAM" ]]; then
    info "Version matched — no action needed."
    exit 0
fi

warn "VERSION MISMATCH: kernel=$KMOD_UPSTREAM vs userland=$USER_UPSTREAM"

if [[ "$CHECK_ONLY" == "1" ]]; then
    warn "Check-only mode — skipping fix."
    exit 1
fi


APT_MATCH=$(apt-cache madison zfsutils-linux 2>/dev/null | grep "$KMOD_UPSTREAM" | head -1 || true)
if [[ -n "$APT_MATCH" ]]; then
    info "Found matching apt package — installing via apt..."
    TARGET_VER=$(echo "$APT_MATCH" | awk '{print $3}')
    sudo apt-get update -qq
    sudo apt-get install -y --allow-downgrades \
        zfsutils-linux="$TARGET_VER" \
        libzfs4linux="$TARGET_VER" \
        libzpool5linux="$TARGET_VER" \
        zfs-zed="$TARGET_VER" \
        zfs-initramfs="$TARGET_VER" 2>/dev/null || \
    sudo apt-get install -y \
        zfsutils-linux="$TARGET_VER" 2>/dev/null
    info "apt install complete."
else
    warn "No matching apt package for $KMOD_UPSTREAM — building from source..."


    sudo apt-get update -qq
    sudo apt-get install -y \
        build-essential autoconf automake libtool gawk alien fakeroot dkms \
        libblkid-dev uuid-dev libudev-dev libssl-dev zlib1g-dev \
        libaio-dev libattr1-dev libelf-dev python3 python3-dev \
        python3-setuptools python3-cffi libffi-dev python3-packaging \
        libcurl4-openssl-dev libpam0g-dev libtirpc-dev 2>/dev/null


    BUILD_DIR=$(mktemp -d /tmp/zfs-build-XXXXXX)
    cd "$BUILD_DIR"

    ZFS_TAG="zfs-${KMOD_UPSTREAM}"
    info "Downloading OpenZFS $ZFS_TAG ..."
    if ! curl -sL "https://github.com/openzfs/zfs/releases/download/${ZFS_TAG}/${ZFS_TAG}.tar.gz" -o zfs.tar.gz; then
        error "Download failed for $ZFS_TAG"
        rm -rf "$BUILD_DIR"
        exit 1
    fi

    tar xzf zfs.tar.gz
    cd "${ZFS_TAG}"

    info "Configuring..."
    ./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
        --with-config=user 2>&1 | tail -3

    info "Building ($(nproc) threads)..."
    make -j"$(nproc)" 2>&1 | tail -3

    info "Installing..."
    sudo make install 2>&1 | tail -3


    cd /
    rm -rf "$BUILD_DIR"
    info "Source build complete."
fi


NEW_USER_VER=$(zfs version 2>/dev/null | head -1 || echo "unknown")
info "Post-fix ZFS userland: $NEW_USER_VER"
info "Post-fix ZFS kmod:     $KMOD_VER"

NEW_USER_UP=$(echo "$NEW_USER_VER" | grep -oP '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
if [[ "$NEW_USER_UP" == "$KMOD_UPSTREAM" ]]; then
    info "SUCCESS — ZFS versions synchronized."
else
    warn "Version still differs — manual review needed."
    exit 1
fi
SYNC_SCRIPT
}




if [[ "$MODE" == "local" ]]; then
    header "ZFS Version Sync (local)"
    zfs_sync_payload | bash -s "$CHECK_ONLY"
else
    header "ZFS Version Sync (remote — 3-node cluster)"
    if [[ "${#NODES[@]}" -eq 0 ]]; then
        error "Set PCV_NODES before remote execution, for example: PCV_NODES='192.0.2.19 192.0.2.20'"
        exit 2
    fi
    SUCCESS=0
    FAIL=0

    for i in "${!NODES[@]}"; do
        if [[ -n "$NODES_FILTER" ]]; then
            node_num=$((i + 1))
            if [[ ! "$NODES_FILTER" =~ $node_num ]]; then
                continue
            fi
        fi

        ip="${NODES[$i]}"
        name="${NODE_NAMES[$i]}"
        header "$name ($ip)"

        if /usr/bin/ssh -o ConnectTimeout=5 "${SSH_USER}@${ip}" \
            "bash -s $CHECK_ONLY" < <(zfs_sync_payload); then
            info "[$name] Done."
            SUCCESS=$((SUCCESS + 1))
        else
            if [[ "$CHECK_ONLY" == "1" ]]; then
                warn "[$name] Mismatch detected."
            else
                error "[$name] Fix failed."
            fi
            FAIL=$((FAIL + 1))
        fi
        echo ""
    done

    header "Summary"
    info "Success: $SUCCESS, Issues: $FAIL"
    [[ $FAIL -eq 0 ]]
fi
