
#include "alert_dlq.h"
#include "utils/pcv_log.h"

#include <glib.h>
#include <string.h>

#define ALERT_DLQ_LOG_DOM "alert_engine"

constexpr int WEBHOOK_DLQ_MAX = 1000;
static_assert(WEBHOOK_DLQ_MAX >= 100, "DLQ buffer too small");

static GPtrArray  *g_webhook_dlq = nullptr;
static GMutex      g_dlq_mu;
static GMutex      g_dlq_retry_mu;
static PcvDlqPostFn g_post_fn = nullptr;

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

    g_mutex_lock(&g_dlq_retry_mu);

    GPtrArray *snapshot   = pcv_alert_dlq_snapshot();
    GPtrArray *ok_entries = g_ptr_array_new_with_free_func(g_free);

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
                g_ptr_array_add(ok_entries, g_strdup(entry));
            } else {
                failed++;
            }
        } else {
            failed++;
        }
    }

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
