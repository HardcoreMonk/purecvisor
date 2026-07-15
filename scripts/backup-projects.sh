#!/usr/bin/env bash
# =============================================================================
# PureCVisor 프로젝트 백업 스크립트
# =============================================================================
# 소스: /home/operator/projects
# 대상 1: /iso/backups/projects (nvme1n1, XFS — rsync 증분)
# 대상 2: pcvpool/projects (ZFS — rsync + 스냅샷)
#
# 사용법:
#   scripts/backup-projects.sh              # 전체 백업 (양쪽)
#   scripts/backup-projects.sh --nvme       # nvme1n1만
#   scripts/backup-projects.sh --zfs        # pcvpool만
#   scripts/backup-projects.sh --dry-run    # 실제 복사 없이 미리보기
#   scripts/backup-projects.sh --list       # ZFS 스냅샷 목록
#   scripts/backup-projects.sh --cleanup 7  # 7일 초과 스냅샷 삭제
# =============================================================================
set -euo pipefail

# ── 설정 ──────────────────────────────────────────────────────
SRC="${PURECVISOR_PROJECT_BACKUP_SRC:-/home/operator/projects}"
NVME_DEST="/iso/backups/projects"
ZFS_DATASET="pcvpool/projects"
ZFS_MOUNT="/pcvpool/projects"
SNAP_PREFIX="backup"
RETENTION_DAYS=30
LOG="/var/log/purecvisor/backup-projects.log"

# ── 색상 ──────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${GREEN}[+]${NC} $(date '+%H:%M:%S') $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $(date '+%H:%M:%S') $*"; }
error() { echo -e "${RED}[-]${NC} $(date '+%H:%M:%S') $*"; }
log()   { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG" 2>/dev/null || true; }

# ── 인자 파싱 ─────────────────────────────────────────────────
DO_NVME=1
DO_ZFS=1
DRY_RUN=""
ACTION="backup"
CLEANUP_DAYS=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --nvme)     DO_NVME=1; DO_ZFS=0; shift ;;
        --zfs)      DO_NVME=0; DO_ZFS=1; shift ;;
        --dry-run)  DRY_RUN="--dry-run"; shift ;;
        --list)     ACTION="list"; shift ;;
        --cleanup)  ACTION="cleanup"; CLEANUP_DAYS="${2:-$RETENTION_DAYS}"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--nvme|--zfs|--dry-run|--list|--cleanup N]"
            exit 0 ;;
        *)  error "Unknown: $1"; exit 1 ;;
    esac
done

# ── rsync 공통 옵션 ──────────────────────────────────────────
RSYNC_OPTS=(
    -aHAX                    # archive + hardlinks + ACLs + xattrs
    --delete                 # 소스에서 삭제된 파일 대상에서도 삭제
    --delete-excluded        # 제외 패턴에 해당하는 파일도 삭제
    --info=progress2,stats2  # 진행률 + 통계
    --exclude='.git/objects/pack/*.tmp'
    --exclude='node_modules/'
    --exclude='__pycache__/'
    --exclude='.cache/'
    --exclude='*.o'
    --exclude='*.d'
    --exclude='bin/purecvisorsd'
    --exclude='bin/pcvctl'
    --exclude='test_runner'
)

# ── 스냅샷 목록 ──────────────────────────────────────────────
if [[ "$ACTION" == "list" ]]; then
    info "ZFS 스냅샷 목록 (${ZFS_DATASET}):"
    zfs list -t snapshot -o name,used,creation -s creation -r "$ZFS_DATASET" 2>/dev/null || echo "  (없음)"
    exit 0
fi

# ── 스냅샷 정리 ──────────────────────────────────────────────
if [[ "$ACTION" == "cleanup" ]]; then
    info "ZFS 스냅샷 정리 (${CLEANUP_DAYS}일 초과):"
    CUTOFF=$(date -d "${CLEANUP_DAYS} days ago" '+%s')
    zfs list -t snapshot -H -o name,creation -s creation -r "$ZFS_DATASET" 2>/dev/null | while read -r snap cdate; do
        snap_ts=$(date -d "$cdate" '+%s' 2>/dev/null || echo 0)
        if [[ $snap_ts -lt $CUTOFF ]]; then
            echo "  삭제: $snap ($cdate)"
            zfs destroy "$snap" 2>/dev/null && echo "    OK" || echo "    FAIL"
        fi
    done
    info "정리 완료"
    exit 0
fi

# ── 사전 검증 ─────────────────────────────────────────────────
[[ -d "$SRC" ]] || { error "소스 없음: $SRC"; exit 1; }

SRC_SIZE=$(du -sh "$SRC" 2>/dev/null | awk '{print $1}')
info "백업 시작 — 소스: ${SRC} (${SRC_SIZE})"
[[ -n "$DRY_RUN" ]] && warn "DRY-RUN 모드 (실제 복사 없음)"
log "START src=$SRC size=$SRC_SIZE nvme=$DO_NVME zfs=$DO_ZFS dry=$DRY_RUN"

FAIL=0
START_TS=$(date '+%s')

# ── 대상 1: NVMe (/iso) ──────────────────────────────────────
if [[ $DO_NVME -eq 1 ]]; then
    info "${CYAN}[1/2] NVMe 백업 → ${NVME_DEST}${NC}"

    if ! mountpoint -q /iso 2>/dev/null; then
        error "/iso 미마운트 — nvme1n1 확인 필요"
        FAIL=1
    else
        sudo mkdir -p "$NVME_DEST"
        NVME_FREE=$(df -BG /iso | tail -1 | awk '{print $4}' | tr -d 'G')
        info "NVMe 여유: ${NVME_FREE}G"

        if rsync "${RSYNC_OPTS[@]}" $DRY_RUN "$SRC/" "$NVME_DEST/"; then
            NVME_SIZE=$(du -sh "$NVME_DEST" 2>/dev/null | awk '{print $1}')
            info "NVMe 백업 완료 (${NVME_SIZE})"
            log "NVME OK dest=$NVME_DEST size=$NVME_SIZE"
        else
            error "NVMe rsync 실패 (exit: $?)"
            FAIL=1
            log "NVME FAIL"
        fi
    fi
    echo
fi

# ── 대상 2: ZFS (pcvpool) ────────────────────────────────────
if [[ $DO_ZFS -eq 1 ]]; then
    info "${CYAN}[2/2] ZFS 백업 → ${ZFS_MOUNT}${NC}"

    if ! zfs list "$ZFS_DATASET" >/dev/null 2>&1; then
        error "ZFS 데이터셋 없음: $ZFS_DATASET"
        FAIL=1
    else
        ZFS_FREE=$(zfs list -H -o avail "$ZFS_DATASET" | tr -d ' ')
        info "ZFS 여유: ${ZFS_FREE}"

        if rsync "${RSYNC_OPTS[@]}" $DRY_RUN "$SRC/" "$ZFS_MOUNT/"; then
            ZFS_SIZE=$(du -sh "$ZFS_MOUNT" 2>/dev/null | awk '{print $1}')
            info "ZFS rsync 완료 (${ZFS_SIZE})"

            # ZFS 스냅샷 생성 (dry-run이 아닌 경우만)
            if [[ -z "$DRY_RUN" ]]; then
                SNAP_NAME="${ZFS_DATASET}@${SNAP_PREFIX}-$(date '+%Y%m%d-%H%M%S')"
                if zfs snapshot "$SNAP_NAME" 2>/dev/null; then
                    info "ZFS 스냅샷 생성: ${SNAP_NAME}"
                    log "ZFS SNAP $SNAP_NAME"
                else
                    warn "ZFS 스냅샷 실패"
                fi

                # 보존 기간 초과 스냅샷 자동 정리
                CUTOFF=$(date -d "${RETENTION_DAYS} days ago" '+%s')
                zfs list -t snapshot -H -o name,creation -s creation -r "$ZFS_DATASET" 2>/dev/null | while read -r snap cdate; do
                    snap_ts=$(date -d "$cdate" '+%s' 2>/dev/null || echo 0)
                    if [[ $snap_ts -lt $CUTOFF ]]; then
                        zfs destroy "$snap" 2>/dev/null && info "스냅샷 정리: $snap"
                    fi
                done
            fi
            log "ZFS OK dest=$ZFS_MOUNT size=$ZFS_SIZE"
        else
            error "ZFS rsync 실패 (exit: $?)"
            FAIL=1
            log "ZFS FAIL"
        fi
    fi
fi

# ── 결과 ──────────────────────────────────────────────────────
END_TS=$(date '+%s')
ELAPSED=$((END_TS - START_TS))
echo
if [[ $FAIL -eq 0 ]]; then
    info "════════════════════════════════════════"
    info "  백업 완료 — ${ELAPSED}초 소요"
    info "════════════════════════════════════════"
    log "DONE elapsed=${ELAPSED}s"
else
    error "════════════════════════════════════════"
    error "  백업 일부 실패 — ${ELAPSED}초 소요"
    error "════════════════════════════════════════"
    log "PARTIAL elapsed=${ELAPSED}s"
    exit 1
fi
