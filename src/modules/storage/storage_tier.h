#ifndef PURECVISOR_STORAGE_TIER_H
#define PURECVISOR_STORAGE_TIER_H

/**
 * @file storage_tier.h
 * @brief 스토리지 티어링 — NVMe/SSD/HDD 자동 배치 + QoS (영역 E)
 *
 * ZFS 풀 기반 논리 티어 정의, 디바이스 타입 감지,
 * VM 생성 시 자동 티어 선택, QoS 정책 관리.
 */

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* 티어 타입 */
typedef enum {
    PCV_TIER_NVME = 0,
    PCV_TIER_SSD  = 1,
    PCV_TIER_HDD  = 2,
    PCV_TIER_AUTO = 99
} PcvStorageTier;

/* 초기화/해제 */
void pcv_storage_tier_init(void);
void pcv_storage_tier_shutdown(void);

/* 티어 CRUD */
JsonArray  *pcv_storage_tier_list(void);
JsonObject *pcv_storage_tier_info(const gchar *tier_name);
gboolean    pcv_storage_tier_create(const gchar *name, const gchar *pool,
                                     PcvStorageTier type, GError **error);

/* 자동 배치 */
const gchar *pcv_storage_tier_auto_select(gint vcpu, gint ram_mb);

/* 티어 이동 */
gboolean pcv_storage_tier_migrate(const gchar *vm_name, const gchar *target_tier,
                                   GError **error);

/* QoS */
gboolean pcv_storage_qos_set(const gchar *vm_name, gint64 read_bps, gint64 write_bps,
                              gint64 read_iops, gint64 write_iops, GError **error);
JsonObject *pcv_storage_qos_get(const gchar *vm_name);
gboolean pcv_storage_qos_delete(const gchar *vm_name, GError **error);

G_END_DECLS

#endif /* PURECVISOR_STORAGE_TIER_H */
