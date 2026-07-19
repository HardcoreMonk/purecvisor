
#ifndef PURECVISOR_ALERT_DLQ_H
#define PURECVISOR_ALERT_DLQ_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef gboolean (*PcvDlqPostFn)(const gchar *url, const gchar *payload);

void        pcv_alert_dlq_set_post_fn(PcvDlqPostFn fn);

void        pcv_alert_dlq_store(const gchar *url, const gchar *payload);

JsonArray  *pcv_alert_dlq_list(void);

JsonObject *pcv_alert_dlq_retry(void);

GPtrArray  *pcv_alert_dlq_snapshot(void);

void        pcv_alert_dlq_remove_matching(GPtrArray *values);

void        pcv_alert_dlq_reset(void);

G_END_DECLS

#endif
