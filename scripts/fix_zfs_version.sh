#!/usr/bin/env bash
# =============================================================================
# PureCVisor — ZFS Kernel/Userland Version Sync Fix
# =============================================================================
# Ubuntu HWE 커널 업그레이드 시 커널 내장 ZFS 모듈만 올라가고
# 유저랜드(zfsutils-linux)는 구버전에 고정되는 문제를 자동 해결한다.
#
# 사용법:
#   scripts/fix_zfs_version.sh                    # 로컬 실행
#   scripts/fix_zfs_version.sh --remote           # 3노드 원격 실행
#   scripts/fix_zfs_version.sh --remote --nodes 1,2  # 특정 노드만
#   scripts/fix_zfs_version.sh --check            # 점검만 (변경 없음)
# =============================================================================
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

# ---------------------------------------------------------------------------
# ZFS 버전 동기화 로직 (로컬 실행용)
# ---------------------------------------------------------------------------
zfs_sync_payload() {
    cat <<'SYNC_SCRIPT'
#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[-]${NC} $*"; }

CHECK_ONLY="${1:-0}"

# 1) 버전 감지
KMOD_VER=$(cat /sys/module/zfs/version 2>/dev/null || echo "unknown")
# 실제 바이너리 버전 우선, 없으면 dpkg 폴백
ZFS_VER_OUTPUT=$(zfs version 2>/dev/null | head -1 || echo "")
if [[ -n "$ZFS_VER_OUTPUT" ]]; then
    USER_VER="$ZFS_VER_OUTPUT"
else
    USER_VER=$(dpkg-query -W -f='${Version}' zfsutils-linux 2>/dev/null || echo "unknown")
fi

# upstream 버전만 추출 (2.3.4-1ubuntu2 -> 2.3.4, zfs-2.3.4-1 -> 2.3.4)
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

# 2) apt에 매칭 버전이 있는지 확인
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

    # 3) 소스 빌드 의존성 설치
    sudo apt-get update -qq
    sudo apt-get install -y \
        build-essential autoconf automake libtool gawk alien fakeroot dkms \
        libblkid-dev uuid-dev libudev-dev libssl-dev zlib1g-dev \
        libaio-dev libattr1-dev libelf-dev python3 python3-dev \
        python3-setuptools python3-cffi libffi-dev python3-packaging \
        libcurl4-openssl-dev libpam0g-dev libtirpc-dev 2>/dev/null

    # 4) 소스 다운로드 및 빌드
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

    # 정리
    cd /
    rm -rf "$BUILD_DIR"
    info "Source build complete."
fi

# 5) 검증
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

# ---------------------------------------------------------------------------
# 실행
# ---------------------------------------------------------------------------
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
