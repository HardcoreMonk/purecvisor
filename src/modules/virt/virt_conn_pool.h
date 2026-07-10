/* src/modules/virt/virt_conn_pool.h
 *
 * Sprint B-2: libvirt 커넥션 풀
 *
 * GMutex + GQueue + GCond 조합으로 최대 max_size 개의 virConnectPtr를
 * 재사용합니다. 워커 스레드와 메인 스레드 모두에서 안전하게 호출 가능합니다.
 *
 * 사용 패턴:
 *   virConnectPtr conn = virt_conn_pool_acquire();
 *   // ... libvirt API 호출 ...
 *   virt_conn_pool_release(conn);
 *
 * acquire()는 풀이 고갈되면 커넥션이 반환될 때까지 블로킹합니다.
 * 커넥션이 끊긴 경우 자동으로 재연결합니다.
 */

#ifndef PURECVISOR_VIRT_CONN_POOL_H
#define PURECVISOR_VIRT_CONN_POOL_H

#include <glib.h>
#include <libvirt/libvirt.h>

G_BEGIN_DECLS

/**
 * @brief 커넥션 풀을 초기화합니다.
 *        main.c에서 GMainLoop 시작 전 1회 호출해야 합니다.
 *
 * @param max_size  풀 최대 커넥션 수 (권장: 8)
 */
void virt_conn_pool_init(guint max_size);

/**
 * @brief 풀에서 커넥션을 하나 획득합니다.
 *        유휴 커넥션이 없으면 반환될 때까지 블로킹합니다.
 *        커넥션이 끊어진 경우 자동으로 재연결합니다.
 *
 * @return virConnectPtr — 반드시 virt_conn_pool_release()로 반환해야 합니다.
 *         초기화 실패 등 치명적 오류 시 NULL을 반환합니다.
 */
virConnectPtr virt_conn_pool_acquire(void);

/**
 * @brief 사용이 끝난 커넥션을 풀에 반환합니다.
 *        virConnectClose()를 직접 호출하지 마십시오.
 *
 * @param conn virt_conn_pool_acquire()가 반환한 포인터
 */
void virt_conn_pool_release(virConnectPtr conn);

/**
 * @brief 풀 내 모든 커넥션을 닫고 메모리를 해제합니다.
 *        데몬 종료 시 main.c에서 호출합니다.
 */
void virt_conn_pool_shutdown(void);

/**
 * @brief 풀 상태 조회 (Prometheus 메트릭 노출용)
 * @param out_idle    유휴 커넥션 수
 * @param out_total   생성된 커넥션 총 수
 * @param out_max     풀 최대 크기
 */
void virt_conn_pool_stats(guint *out_idle, guint *out_total, guint *out_max);

/**
 * @brief 풀 고갈 시 대기 시간 평균 (초 단위)
 * @return 평균 대기 시간 (초). 대기 발생 없으면 0.0.
 */
gdouble virt_conn_pool_wait_avg_seconds(void);

G_END_DECLS

#endif /* PURECVISOR_VIRT_CONN_POOL_H */
