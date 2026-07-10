/* src/modules/virt/circuit_breaker.h
 *
 * Sprint C-2: libvirt 커넥션 Circuit Breaker
 *
 * [상태 전이]
 *
 *   CLOSED ──(연속 5회 실패)──► OPEN ──(백오프 만료)──► HALF_OPEN
 *     ▲                                                      │
 *     └──────────────(프로브 성공)──────────────────────────┘
 *                         │
 *                 (프로브 실패)──► OPEN (백오프 재시작)
 *
 * [백오프 스케줄]
 *   200ms → 400ms → 800ms → 1.6s → 3.2s → 6.4s → 12.8s → 25.6s → 30s (상한)
 *
 * [virt_conn_pool.c 통합]
 *   pool_new_conn() 실패 시 cb_record_failure()
 *   conn 획득 성공 시    cb_record_success()
 *   OPEN 상태이면       acquire() 즉시 NULL 반환
 */

#ifndef PURECVISOR_CIRCUIT_BREAKER_H
#define PURECVISOR_CIRCUIT_BREAKER_H

#include <glib.h>

G_BEGIN_DECLS

/* ── 상태 열거형 ─────────────────────────────────────── */

typedef enum {
    CB_STATE_CLOSED    = 0,  /**< 정상 동작 중                         */
    CB_STATE_OPEN      = 1,  /**< 차단 중 — 요청 즉시 거부              */
    CB_STATE_HALF_OPEN = 2,  /**< 프로브 허용 — 1건 테스트 중           */
} CbState;

/* ── 설정 기본값 ─────────────────────────────────────── */

#define CB_FAILURE_THRESHOLD_DEFAULT  5      /**< CLOSED→OPEN 연속 실패 임계값 기본값 */
#define CB_FAILURE_THRESHOLD   5             /**< 컴파일 시 기본값 (하위 호환)         */
#define CB_BACKOFF_INITIAL_MS  200           /**< 첫 번째 백오프 (밀리초)              */
#define CB_BACKOFF_MAX_MS      30000         /**< 최대 백오프 (30초)                   */

/* ── C23 컴파일 타임 검증 ────────────────────────────── */
static_assert(CB_BACKOFF_MAX_MS >= CB_BACKOFF_INITIAL_MS);
static_assert(CB_FAILURE_THRESHOLD_DEFAULT >= 1);

/* ── 공개 API ────────────────────────────────────────── */

/**
 * cb_init:
 * Circuit Breaker 초기화. virt_conn_pool_init() 에서 1회 호출.
 */
void cb_init(void);

/**
 * cb_is_open:
 * 현재 차단 상태인지 반환.
 * TRUE 이면 acquire() 는 즉시 NULL 을 반환해야 함.
 *
 * HALF_OPEN 상태에서는 첫 번째 호출만 FALSE 반환 (프로브 허용).
 * 이후 호출은 프로브 결과가 나올 때까지 TRUE 반환.
 */
[[nodiscard]] gboolean cb_is_open(void);

/**
 * cb_record_success:
 * libvirt 커넥션 획득/사용 성공 시 호출.
 * HALF_OPEN → CLOSED 전이 또는 실패 카운터 초기화.
 */
void cb_record_success(void);

/**
 * cb_record_failure:
 * libvirt 커넥션 실패(virConnectOpen 실패 또는 재연결 실패) 시 호출.
 * CLOSED: 카운터 증가 → 임계값 도달 시 OPEN
 * HALF_OPEN: OPEN 으로 복귀 + 백오프 2배 증가
 * OPEN: 무시
 */
void cb_record_failure(void);

/**
 * cb_get_state:
 * 현재 상태를 CbState 열거형으로 반환 (0=CLOSED, 1=OPEN, 2=HALF_OPEN).
 * Prometheus 메트릭 출력용.
 */
CbState cb_get_state(void);

/**
 * cb_get_state_str:
 * 현재 상태를 문자열로 반환 ("CLOSED"/"OPEN"/"HALF_OPEN").
 */
const gchar *cb_get_state_str(void);

/**
 * cb_get_failure_count:
 * 현재 연속 실패 횟수 반환 (진단용).
 */
gint cb_get_failure_count(void);

/**
 * cb_set_failure_threshold:
 * 연속 실패 임계값을 런타임에 변경한다.
 * daemon.conf [libvirt] cb_failure_threshold 로 설정 가능.
 * 범위: 1~50 (범위 외 값은 클램핑).
 */
void cb_set_failure_threshold(gint threshold);

/**
 * cb_get_failure_threshold:
 * 현재 설정된 연속 실패 임계값을 반환한다.
 */
gint cb_get_failure_threshold(void);

/**
 * cb_get_named_state:
 * 이름으로 식별된 CB 인스턴스의 상태를 조회한다.
 * 인스턴스가 없으면 CLOSED 상태로 새로 생성.
 * 예: cb_get_named_state("etcd"), cb_get_named_state("s3")
 */
CbState cb_get_named_state(const gchar *name);

/**
 * cb_get_prometheus_metrics:
 * CB 상태를 Prometheus exposition 형식 문자열로 반환한다.
 * 호출자가 g_free()로 해제해야 한다.
 */
gchar *cb_get_prometheus_metrics(void);

/**
 * cb_shutdown:
 * Circuit Breaker 자원 해제. virt_conn_pool_shutdown() 에서 호출.
 */
void cb_shutdown(void);

G_END_DECLS

#endif /* PURECVISOR_CIRCUIT_BREAKER_H */
