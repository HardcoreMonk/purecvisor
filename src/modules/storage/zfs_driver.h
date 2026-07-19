/**
 * @file zfs_driver.h
 * @brief ZFS 스토리지 드라이버 API — zvol 프로비저닝 + 스냅샷 관리
 *
 * == 두 가지 API 스타일 ==
 *   1. 동기 API (zvol): pcv_spawn_sync 기반, GTask 워커 스레드에서 호출
 *      - purecvisor_zfs_create_volume()  : zvol 생성
 *      - purecvisor_zfs_destroy_volume() : zvol 삭제 (-r 옵션으로 하위 스냅샷 포함)
 *
 *   2. 비동기 API (스냅샷): GSubprocess + GTask 기반
 *      - snapshot_create/rollback/delete/list _async/_finish 쌍
 *      - 30초 타임아웃 + GCancellable 취소 지원
 *
 * == pool_name 파라미터 ==
 *   pcv_config_get_zvol_pool()의 반환값을 전달합니다.
 *   기본값: "pcvpool/vms"
 *   daemon.conf [storage] 섹션에서 zvol_pool 키로 오버라이드 가능합니다.
 */
/* src/modules/storage/zfs_driver.h */

#ifndef PURECVISOR_ZFS_DRIVER_H
#define PURECVISOR_ZFS_DRIVER_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ========================================================================= */
/* Phase 5: ZFS Volume Provisioning (이 두 줄이 반드시 있어야 합니다!) */
/* ========================================================================= */
gboolean purecvisor_zfs_create_volume(const gchar *pool_name, const gchar *vm_name, const gchar *size_str, GError **error);
gboolean purecvisor_zfs_destroy_volume(const gchar *pool_name, const gchar *vm_name, GError **error);


/* ========================================================================= */
/* Phase 6: Snapshot Management */
/* ========================================================================= */
/* 1. Snapshot Create */
void purecvisor_zfs_snapshot_create_async(const gchar *pool_name,
                                    const gchar *vm_name,
                                    const gchar *snap_name,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);

gboolean purecvisor_zfs_snapshot_create_finish(GAsyncResult *res, GError **error);

/* 2. Snapshot Delete */
void purecvisor_zfs_snapshot_delete_async(const gchar *pool_name,
                                    const gchar *vm_name,
                                    const gchar *snap_name,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);

gboolean purecvisor_zfs_snapshot_delete_finish(GAsyncResult *res, GError **error);

/* 3. Snapshot Rollback */
void purecvisor_zfs_snapshot_rollback_async(const gchar *pool_name,
                                      const gchar *vm_name,
                                      const gchar *snap_name,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

gboolean purecvisor_zfs_snapshot_rollback_finish(GAsyncResult *res, GError **error);

/* 4. Snapshot List (Returns GPtrArray of snapshot names) */
void purecvisor_zfs_snapshot_list_async(const gchar *pool_name,
                                  const gchar *vm_name,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);

GPtrArray* purecvisor_zfs_snapshot_list_finish(GAsyncResult *res, GError **error);

/* ========================================================================= */
/* Phase 7: ZFS Pool Management (Create / Destroy / Scrub)                  */
/* ========================================================================= */
gboolean purecvisor_zfs_create_pool(const gchar *name, const gchar *vdev_type,
                                     const gchar **disks, gint n_disks,
                                     const gchar *compression, /* "lz4"/"zstd"/"gzip"/"off", NULL→"lz4" */
                                     GError **error);
gboolean purecvisor_zfs_destroy_pool(const gchar *name, GError **error);
gboolean purecvisor_zfs_scrub_pool(const gchar *name, GError **error);

/* ========================================================================= */
/* VM Clone: ZFS CoW Clone + Full Copy                                     */
/* ========================================================================= */

/**
 * purecvisor_zfs_clone_volume:
 * ZFS CoW 클론을 동기적으로 생성합니다.
 * 명령: zfs clone <pool_name>/<source_vm>@<snap_name> <pool_name>/<clone_vm>
 *
 * @param pool_name  ZFS 풀 경로 (예: "pcvpool/vms")
 * @param source_vm  소스 VM 이름
 * @param snap_name  스냅샷 이름 (사전 생성 필요)
 * @param clone_vm   클론 VM 이름
 * @param error      실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 */
gboolean purecvisor_zfs_clone_volume(const gchar *pool_name, const gchar *source_vm,
                                      const gchar *snap_name, const gchar *clone_vm,
                                      GError **error);

/**
 * purecvisor_zfs_full_copy:
 * ZFS send/recv 기반 전체 복사를 동기적으로 수행합니다.
 * pcv_spawn_pipe_sync()로 send stdout을 recv stdin에 직접 연결합니다.
 * 셸 파이프, 리다이렉션, 대용량 임시 파일을 사용하지 않습니다.
 *
 * @param pool_name  ZFS 풀 경로
 * @param source_vm  소스 VM 이름
 * @param snap_name  스냅샷 이름
 * @param clone_vm   클론 VM 이름
 * @param error      실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 */
gboolean purecvisor_zfs_full_copy(const gchar *pool_name, const gchar *source_vm,
                                   const gchar *snap_name, const gchar *clone_vm,
                                   GError **error);

/* ========================================================================= */
/* Storage Pool Health Monitoring                                           */
/* ========================================================================= */

/**
 * ZfsPoolHealth:
 * zpool status 파싱 결과를 담는 구조체.
 * ebpf_telemetry.c에서 60초 주기로 수집하여 Prometheus 메트릭으로 노출.
 */
typedef struct {
    gchar    state[16];       /* ONLINE, DEGRADED, FAULTED, UNAVAIL */
    gint     errors_read;     /* read 에러 수 */
    gint     errors_write;    /* write 에러 수 */
    gint     errors_cksum;    /* checksum 에러 수 */
    gint64   scrub_age_sec;   /* 마지막 스크럽 이후 경과 시간 (초), -1이면 미실행 */
    gboolean scrub_running;   /* 스크럽 진행 중 여부 */
    gdouble  capacity_pct;    /* 풀 사용률 (0.0 ~ 100.0) */
} ZfsPoolHealth;

/**
 * pcv_zfs_pool_health:
 * @pool_name: ZFS 풀 이름 (예: "pcvpool")
 * @out:       결과를 받을 ZfsPoolHealth 구조체 포인터
 *
 * `zpool status -p <pool>` + `zpool list` 를 실행하여 풀 상태를 파싱합니다.
 * 동기 함수 — GTask 워커 스레드 또는 텔레메트리 스레드에서 호출.
 *
 * Returns: TRUE 성공, FALSE 실패 (zpool 명령 실행 오류)
 */
gboolean pcv_zfs_pool_health(const gchar *pool_name, ZfsPoolHealth *out);

/**
 * pcv_zfs_pool_health_to_json:
 * @h: ZfsPoolHealth 구조체 포인터
 *
 * ZfsPoolHealth를 JsonObject로 변환합니다. RPC 응답 빌드용.
 *
 * Returns: (transfer full): JsonObject (호출자가 json_object_unref 필요)
 */
JsonObject *pcv_zfs_pool_health_to_json(const ZfsPoolHealth *h);

/* ========================================================================= */
/* Pool SUSPENDED 탐지 + 가드된 자동복구 (L2/L3)                             */
/*                                                                           */
/* 배경: 단일 USB SSD 풀이 USB 단절로 SUSPENDED 되었으나 데몬이 SUSPENDED 를 */
/* "정상(0)"으로 매핑해 34시간 미인지·수동복구했다. 아래 3함수가             */
/*   (1) 상태→메트릭 매핑에 SUSPENDED(4) 를 추가하고                         */
/*   (2) 무한 clear-loop 없이 안전하게 자동복구를 시도한다.                   */
/* ========================================================================= */

/**
 * pcv_zfs_pool_state_metric_val:
 * @state: zpool state 문자열 (ONLINE/DEGRADED/FAULTED/UNAVAIL/SUSPENDED/…)
 *
 * zpool 풀 state 문자열을 Prometheus 게이지 값으로 매핑한다(순수 함수).
 *   ONLINE/UNKNOWN → 0, DEGRADED → 1, FAULTED → 2, UNAVAIL → 3, SUSPENDED → 4.
 * critical 판정은 값 >= 2 (FAULTED/UNAVAIL/SUSPENDED).
 *
 * [회귀 방지] SUSPENDED 가 0(정상)으로 매핑되던 버그의 단일 진실 소스.
 * scripts/check_zpool_suspend_recover.py 게이트가 SUSPENDED→비0 을 강제한다.
 */
gdouble pcv_zfs_pool_state_metric_val(const gchar *state);

/**
 * ZfsRecoverGuard:
 * 자동복구(zpool clear) 시도의 시간창 상한을 추적하는 서킷브레이커 상태.
 * "시간창당 최대 N회" 고정창 레이트리미터로, 무한 clear-loop(디바이스 flapping)를
 * 차단한다. 호출자가 소유(정적/스택). pcv_zfs_recover_guard_allow 로만 조작.
 */
typedef struct {
    gint64 window_start_us;   /* 현재 창 시작 시각 (g_get_monotonic_time, 0=미개시) */
    gint   attempts;          /* 현재 창에서 소비된 시도 횟수 */
} ZfsRecoverGuard;

/**
 * pcv_zfs_recover_guard_allow:
 * @g:            서킷브레이커 상태 (호출자 소유)
 * @now_us:       현재 시각 (g_get_monotonic_time 기준, 마이크로초) — 테스트 주입용
 * @window_us:    시간창 길이 (마이크로초). 예: 3600초 = 3600*G_USEC_PER_SEC
 * @max_attempts: 시간창당 허용 시도 상한 (예: 3)
 *
 * 고정창 레이트리미터(순수 함수). now_us 가 창 밖이면 창을 리셋한다. 창 내
 * attempts < max_attempts 이면 attempts 를 1 증가시키고 TRUE(허용), 상한 도달
 * 시 FALSE(차단)를 반환한다. TRUE 를 받은 호출자만 실제 clear 를 시도해야 한다.
 *
 * Returns: TRUE=이번 시도 허용, FALSE=시간창 상한 초과(차단)
 */
gboolean pcv_zfs_recover_guard_allow(ZfsRecoverGuard *g, gint64 now_us,
                                     gint64 window_us, gint max_attempts);

/**
 * PcvZfsRecoverResult:
 * pcv_zfs_pool_recover_suspended 의 결과. 호출자(텔레메트리 루프)가 이 값으로
 * 운영자 알림을 발화할지 결정한다.
 */
typedef enum {
    PCV_ZFS_RECOVER_DISABLED = 0,   /* [storage] auto_pool_recover=false — clear 안 함 */
    PCV_ZFS_RECOVER_NOT_SUSPENDED,  /* pool_name NULL 등 — 대상 아님 */
    PCV_ZFS_RECOVER_DEV_UNREADABLE, /* vdev 미존재/읽기불가 — clear 시도 안 함(안전) */
    PCV_ZFS_RECOVER_CB_TRIPPED,     /* 시간창 상한 초과 — clear 중단(flapping 방지) */
    PCV_ZFS_RECOVER_CLEARED,        /* zpool clear 성공 */
    PCV_ZFS_RECOVER_CLEAR_FAILED,   /* zpool clear 실행했으나 실패 */
} PcvZfsRecoverResult;

/**
 * pcv_zfs_pool_recover_suspended:
 * @pool_name: SUSPENDED 로 판정된 풀 이름
 * @guard:     시간창 서킷브레이커 상태 (호출자 소유, 정적 권장). NULL 이면 상한 미적용.
 *
 * SUSPENDED 풀의 가드된 자동복구를 시도한다. 순서:
 *   1. [storage] auto_pool_recover=false 이면 DISABLED 반환(clear 안 함).
 *   2. vdev 디바이스 경로 확인([storage] pool_device 우선, 없으면 zpool status 파싱)
 *      후 `dd bs=4k count=1` 읽기 테스트(8초 타임아웃). 미존재/읽기실패/타임아웃이면
 *      DEV_UNREADABLE 반환(진짜 죽은 디바이스에 clear 를 시도하지 않는다).
 *   3. 서킷브레이커(시간창당 상한, 기본 1시간 3회) 초과면 CB_TRIPPED 반환.
 *   4. 위를 통과하면 `zpool clear <pool>`(40초 타임아웃) 실행.
 * 매 결과를 pcv_audit_log 로 기록한다.
 *
 * [스레드] 블로킹 spawn 포함 — 텔레메트리/GTask 워커 스레드에서만 호출(GMainLoop 금지).
 */
PcvZfsRecoverResult pcv_zfs_pool_recover_suspended(const gchar *pool_name,
                                                   ZfsRecoverGuard *guard);

/* ========================================================================= */
/* Storage Capacity Forecasting (Linear Regression)                        */
/* ========================================================================= */

/**
 * pcv_zfs_pool_forecast:
 * @pool_name: ZFS 풀 이름 (예: "pcvpool")
 *
 * `zfs get` 명령으로 현재 사용량/전체 용량을 조회한 뒤,
 * 인메모리 히스토리 기반 선형 회귀(OLS)로 용량 소진 예측일을 계산합니다.
 *
 * Returns: (transfer full): JsonObject (호출자가 json_object_unref 필요)
 *   pool, used_bytes, total_bytes, used_pct,
 *   daily_growth_bytes, days_to_full, predicted_full_date,
 *   history_points, alert_level ("ok" / "warn" / "critical")
 */
JsonObject *pcv_zfs_pool_forecast(const gchar *pool_name);

/**
 * pcv_zfs_capacity_record:
 * @pool_name: ZFS 풀 이름
 *
 * 현재 사용량을 인메모리 히스토리에 기록합니다.
 * ebpf_telemetry.c에서 3600초(1시간)마다 호출합니다.
 */
void pcv_zfs_capacity_record(const gchar *pool_name);

/* ========================================================================= */
/* CE-6: Pool Health Detail + Clone Promote                                 */
/* ========================================================================= */

/**
 * purecvisor_zfs_pool_health_detail:
 * @pool_name: ZFS 풀 이름 (예: "pcvpool")
 *
 * zpool list + zpool status를 실행하여 풀 상세 정보를 JsonObject로 반환합니다.
 * name, health, allocated, size, free, fragmentation, capacity, scrub 상태 포함.
 *
 * Returns: (transfer full): JsonObject (호출자가 json_object_unref 필요)
 */
JsonObject *purecvisor_zfs_pool_health_detail(const gchar *pool_name);

/**
 * purecvisor_zfs_promote:
 * @clone_name: ZFS 클론 데이터셋 전체 경로 (예: "pcvpool/vms/clone-vm")
 *
 * `zfs promote`를 실행하여 클론의 원본 의존성을 해소합니다.
 * promote 후 클론은 독립 데이터셋이 되어 원본 삭제가 가능해집니다.
 *
 * Returns: TRUE 성공, FALSE 실패
 */
gboolean purecvisor_zfs_promote(const gchar *clone_name);

/* ========================================================================= */
/* CE-A3: ZFS Native Encryption                                            */
/* ========================================================================= */

/**
 * purecvisor_zfs_create_zvol_encrypted:
 * 암호화된 ZFS zvol을 생성합니다 (AES-256-GCM, passphrase stdin 전달).
 *
 * @param name        전체 데이터셋 경로 (예: "pcvpool/vms/secure-vm")
 * @param size        볼륨 크기 문자열 (예: "50G")
 * @param passphrase  암호화 패스프레이즈
 * @param error       실패 시 GError** 설정
 * @return TRUE: 성공, FALSE: 실패
 */
gboolean purecvisor_zfs_create_zvol_encrypted(const gchar *name, const gchar *size,
                                               const gchar *passphrase, GError **error);

/* ========================================================================= */
/* CE-A4: Snapshot Quota Enforcement                                       */
/* ========================================================================= */

/**
 * purecvisor_zfs_check_snapshot_quota:
 * 데이터셋의 현재 스냅샷 수가 max_snapshots 미만인지 확인합니다.
 *
 * @param dataset        ZFS 데이터셋 이름
 * @param max_snapshots  최대 허용 스냅샷 수 (0이면 무제한)
 * @return TRUE: 생성 허용, FALSE: 쿼터 초과
 */
[[nodiscard]] gboolean purecvisor_zfs_check_snapshot_quota(const gchar *dataset, gint max_snapshots);

G_END_DECLS

#endif /* PURECVISOR_ZFS_DRIVER_H */
