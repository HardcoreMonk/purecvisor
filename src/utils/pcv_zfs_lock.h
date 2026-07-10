/* ==========================================================================
 * src/utils/pcv_zfs_lock.h
 * PureCVisor — ZFS pool 수준 intra-node 직렬화 락
 *
 * [파일 역할]
 *   같은 노드에서 동시에 실행되는 ZFS 작업(`zfs create`, `zfs recv`,
 *   `zfs rollback`, `zfs destroy`)을 pool 단위로 직렬화한다.
 *
 * [왜 필요한가 — BUG-18 배경]
 *   OpenZFS는 pool-level txg 락을 사용하며, concurrent recv+rollback 등이
 *   D-state deadlock을 유발한 사례가 있다 (2026-04-14 Node2 incident).
 *   본 모듈은 application 계층에서 operation을 직렬화하여 pool-level 락
 *   경합 자체를 줄인다.
 *
 * [Phase 1 (이번 구현)] intra-node GMutex
 *   - 같은 프로세스 내에서만 유효
 *   - pool 이름 → GMutex 매핑 (해시 테이블)
 *   - 대부분의 BUG-18 시나리오(같은 노드의 replication + manual op)를 해결
 *
 * [Phase 2 (향후)] etcd lease 기반 분산 락 (ADR-0021 예정)
 *   - 여러 노드 간 ZFS 작업 순서 보장
 *   - `/purecvisor/zfs/inflight/<pool>` 키 + TTL
 *
 * [사용 패턴]
 *     GError *err = NULL;
 *     if (!pcv_zfs_pool_lock(pool, "create", 30000, &err)) {
 *         // 락 획득 실패 (타임아웃 또는 에러)
 *         return -1;
 *     }
 *     // ... ZFS operation ...
 *     pcv_zfs_pool_unlock(pool);
 *
 * [성능]
 *   lock/unlock은 GMutex 기반으로 100ns 수준. ZFS 자체 작업이 ms~s 단위이므로
 *   직렬화 오버헤드는 무시 가능.
 * ========================================================================== */

#ifndef PURECVISOR_ZFS_LOCK_H
#define PURECVISOR_ZFS_LOCK_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * pcv_zfs_pool_lock_init:
 * 모듈 초기화. main.c에서 1회 호출.
 */
void pcv_zfs_pool_lock_init(void);

/**
 * pcv_zfs_pool_lock_shutdown:
 * 모듈 종료. 등록된 모든 pool mutex 해제.
 */
void pcv_zfs_pool_lock_shutdown(void);

/**
 * pcv_zfs_pool_lock:
 * @pool:        ZFS pool 이름 (예: "pcvpool", "pcvpool/vms"). 첫 슬래시 이전 토큰만 사용.
 * @op:          작업 종류 식별 문자열 (로그용, 예: "create", "recv", "rollback")
 * @timeout_ms:  락 획득 최대 대기시간 (ms). 0이면 바로 시도 후 실패 반환.
 * @error:       실패 시 에러 정보 (옵션)
 *
 * pool에 대한 배타 락을 획득한다. 이미 다른 op이 락을 보유 중이면 타임아웃까지
 * 대기. 타임아웃 시 WARN 로그 + FALSE 반환.
 *
 * Returns: TRUE 획득 성공, FALSE 타임아웃/에러
 */
gboolean pcv_zfs_pool_lock(const gchar *pool, const gchar *op,
                            gint timeout_ms, GError **error);

/**
 * pcv_zfs_pool_unlock:
 * @pool: ZFS pool 이름 (pcv_zfs_pool_lock과 동일한 문자열)
 *
 * pool 락을 해제한다. 자신이 보유하지 않은 락을 해제하려 하면 WARN 로그.
 * (자기 자신이 보유 확인은 GLib GMutex의 non-recursive 특성에 의존)
 */
void pcv_zfs_pool_unlock(const gchar *pool);

/**
 * pcv_zfs_pool_get_stats:
 * 현재 등록된 pool 수와 경합 통계 반환 (모니터링/디버깅용).
 *
 * @registered: (out) 등록된 pool 수
 * @contentions: (out) 누적 타임아웃 건수
 */
void pcv_zfs_pool_get_stats(gint *registered, gint64 *contentions);

G_END_DECLS

#endif /* PURECVISOR_ZFS_LOCK_H */
