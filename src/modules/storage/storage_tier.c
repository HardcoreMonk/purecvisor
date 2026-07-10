/**
 * @file storage_tier.c
 * @brief 스토리지 티어링 — 부팅 시 기본 SSD 티어 등록 (영역 E)
 *
 * [초기화]
 *   pcv_storage_tier_init() 이 부팅 시 1회 호출되어 기본 프리셋
 *   "ssd" → pcv_config_get_zvol_pool()(기존 풀)을 등록한다.
 *
 * 감사 재분류(2026-07-08, D8/M-1): 이 파일에 있던 티어 CRUD·자동배치·
 * 온라인 마이그레이션(ZFS send/recv)·QoS(virsh blkiotune) API(9함수)는
 * 대응 RPC가 한 번도 배선되지 않아 호출부 0(도달불가)으로 확인되어
 * 전면 삭제되었다. pcv_storage_tier_init()만 라이브 경로(기본 ssd 티어
 * 등록 + 통합테스트 test_phase2_ae.sh/test_webui_full.sh가 init 로그에
 * 의존)라 모듈 자체는 보존한다.
 * 상세: docs/operations/2026-07-08-remediation-roadmap-delta.md D8.
 */
#include "storage_tier.h"
#include "utils/pcv_log.h"
#include "utils/pcv_config.h"

/** 로그 도메인 — journalctl에서 storage_tier로 필터링 가능 */
#define TIER_LOG_DOM  "storage_tier"

/** 최대 등록 가능 티어 수 (NVMe + SSD + HDD 등 물리적 한계 반영) */
#define MAX_TIERS     8

/**
 * TierDef:
 * 스토리지 티어의 정의 레코드.
 * 각 티어는 ZFS 풀 하나와 매핑되며, 디스크 타입(NVMe/SSD/HDD)을 분류합니다.
 *
 * 디자인 결정: 고정 크기 문자 배열을 사용하여 동적 할당/해제 오버헤드를 제거합니다.
 * 티어 이름(name)과 풀 경로(pool) 모두 64바이트 이하로 충분합니다.
 */
typedef struct {
    gchar          name[32];   /* 티어 이름 (예: "ssd", "nvme", "hdd") */
    gchar          pool[64];   /* ZFS 풀 경로 (예: "pcvpool/vms") */
    PcvStorageTier type;       /* 디스크 유형 열거값 (NVMe/SSD/HDD) */
    gboolean       active;     /* 활성 상태 플래그 */
} TierDef;

/** 전역 티어 관리 상태 — 프로세스당 1개 인스턴스 */
static struct {
    TierDef   tiers[MAX_TIERS];  /* 티어 정의 배열 */
    gint      count;              /* 현재 등록된 티어 수 */
    GMutex    mu;                 /* 모든 상태 접근 직렬화 */
    gboolean  initialized;        /* 초기화 여부 플래그 */
} G = {0};

/* ── 초기화 ─────────────────────────────────────── */

/**
 * pcv_storage_tier_init:
 * 스토리지 티어 매니저를 초기화하고 기본 프리셋을 등록합니다.
 * 기본 프리셋: "ssd" → pcv_config_get_zvol_pool() (보통 "pcvpool/vms")
 */
void
pcv_storage_tier_init(void)
{
    g_mutex_init(&G.mu);
    G.count = 0;

    /* 기본 프리셋: ssd → pcvpool (기존 풀) */
    g_strlcpy(G.tiers[0].name, "ssd", sizeof(G.tiers[0].name));
    g_strlcpy(G.tiers[0].pool, pcv_config_get_zvol_pool(), sizeof(G.tiers[0].pool));
    G.tiers[0].type = PCV_TIER_SSD;
    G.tiers[0].active = TRUE;
    G.count = 1;

    G.initialized = TRUE;
    PCV_LOG_INFO(TIER_LOG_DOM, "Storage tier initialized (default: ssd → %s)",
                 G.tiers[0].pool);
}
