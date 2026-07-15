/* src/modules/daemons/alert_dlq.c
 *
 * [AIO-4] Webhook DLQ 스토어 — alert_engine.c 에서 추출.
 * 배경/락 규율/값매칭 의미는 alert_dlq.h 헤더 주석 참조. HTTP 전송(_webhook_post)과
 * 발화측 배선은 alert_engine.c 에 잔존하며 pcv_alert_dlq_set_post_fn 으로 주입된다.
 */
#include "alert_dlq.h"
#include "utils/pcv_log.h"

#include <glib.h>
#include <string.h>

#define ALERT_DLQ_LOG_DOM "alert_engine"

constexpr int WEBHOOK_DLQ_MAX = 1000;
static_assert(WEBHOOK_DLQ_MAX >= 100, "DLQ buffer too small");

static GPtrArray  *g_webhook_dlq = nullptr;
static GMutex      g_dlq_mu;        /* 항목 스토어 보호 — zero-init 정적 GMutex */
static GMutex      g_dlq_retry_mu; /* AIO-4: 동시 retry 직렬화(outer). g_dlq_mu 는 inner. */
static PcvDlqPostFn g_post_fn = nullptr; /* HTTP 전송 seam — alert_engine.c init 서 등록 */

void
pcv_alert_dlq_set_post_fn(PcvDlqPostFn fn)
{
    g_post_fn = fn;
}

void
pcv_alert_dlq_store(const gchar *url, const gchar *payload)
{
    g_mutex_lock(&g_dlq_mu);
    if (!g_webhook_dlq)
        g_webhook_dlq = g_ptr_array_new_with_free_func(g_free);
    if (g_webhook_dlq->len < WEBHOOK_DLQ_MAX) {
        gchar *entry = g_strdup_printf("%s|%s", url, payload);
        g_ptr_array_add(g_webhook_dlq, entry);
        PCV_LOG_WARN(ALERT_DLQ_LOG_DOM, "Webhook DLQ stored (%u entries)", g_webhook_dlq->len);
    } else {
        PCV_LOG_WARN(ALERT_DLQ_LOG_DOM, "Webhook DLQ full (%d), dropping entry", WEBHOOK_DLQ_MAX);
    }
    g_mutex_unlock(&g_dlq_mu);
}

JsonArray *
pcv_alert_dlq_list(void)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&g_dlq_mu);
    if (g_webhook_dlq) {
        for (guint i = 0; i < g_webhook_dlq->len; i++) {
            const gchar *entry = g_ptr_array_index(g_webhook_dlq, i);
            /* entry format: "url|payload" */
            const gchar *sep = strchr(entry, '|');
            JsonObject *obj = json_object_new();
            if (sep) {
                gchar *url = g_strndup(entry, (gsize)(sep - entry));
                json_object_set_string_member(obj, "url", url);
                json_object_set_string_member(obj, "payload", sep + 1);
                g_free(url);
            } else {
                json_object_set_string_member(obj, "url", "");
                json_object_set_string_member(obj, "payload", entry);
            }
            json_object_set_int_member(obj, "index", (gint64)i);
            json_array_add_object_element(arr, obj);
        }
    }
    g_mutex_unlock(&g_dlq_mu);
    return arr;
}

GPtrArray *
pcv_alert_dlq_snapshot(void)
{
    GPtrArray *snap = g_ptr_array_new_with_free_func(g_free);
    g_mutex_lock(&g_dlq_mu);
    if (g_webhook_dlq) {
        for (guint i = 0; i < g_webhook_dlq->len; i++)
            g_ptr_array_add(snap, g_strdup(g_ptr_array_index(g_webhook_dlq, i)));
    }
    g_mutex_unlock(&g_dlq_mu);
    return snap;
}

void
pcv_alert_dlq_remove_matching(GPtrArray *values)
{
    if (!values || values->len == 0) return;
    g_mutex_lock(&g_dlq_mu);
    if (g_webhook_dlq) {
        for (guint v = 0; v < values->len; v++) {
            const gchar *want = g_ptr_array_index(values, v);
            /* 값매칭 — 스냅샷 이후 인덱스가 변동했을 수 있으므로 값으로 첫 매칭을
             * 찾아 제거한다(성공 1건당 1건, break). */
            for (guint i = 0; i < g_webhook_dlq->len; i++) {
                if (g_strcmp0(g_ptr_array_index(g_webhook_dlq, i), want) == 0) {
                    g_ptr_array_remove_index(g_webhook_dlq, i);
                    break;
                }
            }
        }
    }
    g_mutex_unlock(&g_dlq_mu);
}

JsonObject *
pcv_alert_dlq_retry(void)
{
    JsonObject *result = json_object_new();
    gint retried = 0, succeeded = 0, failed = 0;

    /* AIO-4: 동시 retry 를 직렬화(outer) — 원본이 g_dlq_mu 로 얻던 "동시 재시도
     * 이중전송 방지"를 유지하되, list/store 는 g_dlq_mu 만 잡으므로 블록되지 않는다. */
    g_mutex_lock(&g_dlq_retry_mu);

    /* (1) 락 하 snapshot deep copy → 즉시 unlock (HTTP 는 락 밖) */
    GPtrArray *snapshot   = pcv_alert_dlq_snapshot();
    GPtrArray *ok_entries = g_ptr_array_new_with_free_func(g_free);

    /* (2) 락 밖 HTTP 재시도 — 성공 원문 수집. g_dlq_mu 는 이 루프 동안 보유하지 않음. */
    for (guint i = 0; i < snapshot->len; i++) {
        const gchar *entry = g_ptr_array_index(snapshot, i);
        const gchar *sep = strchr(entry, '|');
        retried++;
        if (sep) {
            gchar *url = g_strndup(entry, (gsize)(sep - entry));
            gboolean ok = (g_post_fn != nullptr) && g_post_fn(url, sep + 1);
            g_free(url);
            if (ok) {
                succeeded++;
                g_ptr_array_add(ok_entries, g_strdup(entry)); /* 원문 보존 → 값매칭 제거 */
            } else {
                failed++;
            }
        } else {
            failed++;
        }
    }

    /* (3) 재락 → 성공 항목을 값매칭으로 제거 */
    pcv_alert_dlq_remove_matching(ok_entries);

    g_ptr_array_unref(snapshot);
    g_ptr_array_unref(ok_entries);

    g_mutex_unlock(&g_dlq_retry_mu);

    json_object_set_int_member(result, "retried", retried);
    json_object_set_int_member(result, "succeeded", succeeded);
    json_object_set_int_member(result, "failed", failed);
    PCV_LOG_INFO(ALERT_DLQ_LOG_DOM, "DLQ retry: %d retried, %d succeeded, %d failed",
                 retried, succeeded, failed);
    return result;
}

/* 테스트 격리 전용 헬퍼 — 유일한 소비자는 test_runner (tests/test_alert_dlq.c).
 * dead-export 게이트는 src 하위 .c 만 스캔해 이 test-only export 를 dead 로 보나
 * 의도된 것 (alert_silence.c pcv_alert_silence_reset 선례와 동일).
 * PCV_DEAD_EXPORT_OK */
void
pcv_alert_dlq_reset(void)
{
    g_mutex_lock(&g_dlq_mu);
    if (g_webhook_dlq)
        g_ptr_array_set_size(g_webhook_dlq, 0);
    g_mutex_unlock(&g_dlq_mu);
}
