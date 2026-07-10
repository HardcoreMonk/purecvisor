#ifndef PURECVISOR_STORAGE_TIER_H
#define PURECVISOR_STORAGE_TIER_H

/**
 * @file storage_tier.h
 * @brief 스토리지 티어링 — 부팅 시 기본 SSD 티어 등록 (영역 E)
 *
 * 감사 재분류(2026-07-08, D8/M-1): 티어 CRUD·자동배치·온라인 마이그레이션·
 * QoS API(9함수)는 대응 RPC가 배선된 적이 없어 호출부 0(도달불가)으로
 * 확인되어 전면 삭제되었다. pcv_storage_tier_init()만 라이브 경로(기본
 * ssd 티어 등록 + 통합테스트 test_phase2_ae.sh/test_webui_full.sh가
 * init 로그에 의존)라 모듈 자체는 보존한다.
 * 상세: docs/operations/2026-07-08-remediation-roadmap-delta.md D8.
 */

#include <glib.h>

G_BEGIN_DECLS

/* 티어 타입 */
typedef enum {
    PCV_TIER_NVME = 0,
    PCV_TIER_SSD  = 1,
    PCV_TIER_HDD  = 2,
    PCV_TIER_AUTO = 99
} PcvStorageTier;

/* 초기화 — 기본 ssd 티어 등록 (부팅 시 1회, main.c) */
void pcv_storage_tier_init(void);

G_END_DECLS

#endif /* PURECVISOR_STORAGE_TIER_H */
