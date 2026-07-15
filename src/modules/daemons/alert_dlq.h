/* src/modules/daemons/alert_dlq.h
 *
 * [AIO-4] Webhook DLQ(Dead Letter Queue) 스토어 — alert_engine.c 에서 추출.
 *
 * [추출 배경 — AIO-4 (AIO-3 alert_silence 선례)]
 *   원래 alert_engine.c 안에 있던 self-contained DLQ 스토어(g_webhook_dlq +
 *   store/list/retry)를 별도 링크가능 TU 로 분리했다. 이유:
 *     - 값매칭 제거의 회귀 테스트가 실제 프로덕션 함수를 호출해야 하는데,
 *       alert_engine.c 전체를 test_runner 에 링크하면 test_stubs.c 스텁과
 *       중복정의로 충돌한다 (alert_silence.c 추출과 동일 이유).
 *     - HTTP 전송(_webhook_post)은 alert_engine.c 에 잔존하고, 함수포인터
 *       seam(pcv_alert_dlq_set_post_fn)으로 주입해 테스트가 실 HTTP 없이
 *       스냅샷·제거 로직만 검증할 수 있게 한다.
 *
 * [락 보유 중 동기 HTTP 금지 — AIO-4 시정]
 *   기존 pcv_alert_engine_dlq_retry 는 g_dlq_mu 를 보유한 채 각 DLQ 항목에
 *   동기 HTTP(_webhook_post, 10s 타임아웃)를 돌렸다 → 최악 수시간 락 보유,
 *   dlq_list/dlq_store 전면 블록. 시정: snapshot(락 하 deep copy) → unlock →
 *   HTTP(락 밖) → 성공 원문 값매칭 제거(재락). 락은 스냅샷·제거 순간만 보유하고
 *   HTTP 동안엔 절대 보유하지 않는다. 제거는 인덱스가 아닌 값매칭이다 — 락 해제
 *   중 g_webhook_dlq 가 (dlq_store 로) 변동할 수 있어 스냅샷 시점 인덱스는 무효.
 */
#ifndef PURECVISOR_ALERT_DLQ_H
#define PURECVISOR_ALERT_DLQ_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * PcvDlqPostFn:
 * @url:     대상 URL
 * @payload: JSON 페이로드
 *
 * DLQ 재시도 시 단일 Webhook 전송을 수행하는 함수 포인터 타입. 프로덕션에서는
 * alert_engine.c 의 _webhook_post 를 init 에서 등록한다(pcv_alert_dlq_set_post_fn).
 * 미등록(NULL)이면 재시도는 전송 실패로 취급한다.
 *
 * Returns: 전송 성공 시 TRUE.
 */
typedef gboolean (*PcvDlqPostFn)(const gchar *url, const gchar *payload);

/**
 * pcv_alert_dlq_set_post_fn:
 * @fn: 전송 함수(NULL 허용 — 미등록 상태로 되돌림)
 *
 * DLQ 재시도가 사용할 HTTP 전송 seam 을 등록한다. alert_engine.c 가 init 에서
 * 실 _webhook_post 를, 테스트가 목을 주입한다.
 */
void        pcv_alert_dlq_set_post_fn(PcvDlqPostFn fn);

/**
 * pcv_alert_dlq_store:
 * @url:     대상 URL
 * @payload: JSON 페이로드
 *
 * 재시도 소진 후 실패 Webhook 을 "url|payload" 문자열로 DLQ 에 저장한다.
 * 최대 WEBHOOK_DLQ_MAX(1000)개까지, 초과 시 드롭. (구 _webhook_dlq_store)
 */
void        pcv_alert_dlq_store(const gchar *url, const gchar *payload);

/**
 * pcv_alert_dlq_list:
 *
 * DLQ 항목을 {"url","payload","index"} JsonObject 배열로 반환한다.
 * Returns: (transfer full): 호출자 소유 JsonArray. (구 pcv_alert_engine_dlq_list 코어)
 */
JsonArray  *pcv_alert_dlq_list(void);

/**
 * pcv_alert_dlq_retry:
 *
 * DLQ 항목을 재전송 시도한다. 락은 스냅샷·제거 순간만 보유하고 HTTP 동안엔
 * 보유하지 않는다(AIO-4). 성공 항목은 값매칭으로 제거된다.
 * Returns: (transfer full): JsonObject {retried, succeeded, failed}. (구 코어)
 */
JsonObject *pcv_alert_dlq_retry(void);

/**
 * pcv_alert_dlq_snapshot:
 *
 * g_dlq_mu 를 보유한 채 전 항목을 deep copy 한 새 GPtrArray 를 반환한다.
 * retry 의 1단계 프리미티브이자, 값매칭 제거 테스트의 검증 포인트.
 * Returns: (transfer full): g_free free-func GPtrArray — 호출자가 g_ptr_array_unref().
 */
GPtrArray  *pcv_alert_dlq_snapshot(void);

/**
 * pcv_alert_dlq_remove_matching:
 * @values: 제거할 원문 문자열 배열 (성공 원문 목록)
 *
 * g_dlq_mu 를 보유한 채 @values 의 각 문자열에 대해 g_webhook_dlq 에서 **값이
 * 일치하는 첫 항목**을 제거한다(성공 1건당 1건, break). 인덱스가 아닌 값 기준 —
 * 스냅샷 이후 배열이 변동해도 올바른 항목을 지운다. retry 의 3단계 프리미티브.
 */
void        pcv_alert_dlq_remove_matching(GPtrArray *values);

/**
 * pcv_alert_dlq_reset:
 *
 * DLQ 를 비운다 — 테스트 격리 전용.
 */
void        pcv_alert_dlq_reset(void);

G_END_DECLS

#endif /* PURECVISOR_ALERT_DLQ_H */
