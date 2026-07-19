
#ifndef PURECVISOR_AUDIT_H
#define PURECVISOR_AUDIT_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct {
    gchar  *username;
    gchar  *method;
    gchar  *target;
    gchar  *result;
    gint    error_code;
    gint64  duration_ms;
    gchar  *src_ip;
    gint64  event_us;
} PcvAuditRecord;

void pcv_audit_init(const gchar *db_path);

void pcv_audit_shutdown(void);

void pcv_audit_log(const gchar *username, const gchar *method,
                    const gchar *target, const gchar *result,
                    gint error_code, gint64 duration_ms,
                    const gchar *src_ip);

void pcv_audit_log_rpc(const gchar *method, const gchar *result,
                        gint error_code, gint64 duration_ms);

gint64 pcv_audit_get_total_count(void);

gint pcv_audit_get_queue_depth(void);

gint64 pcv_audit_get_dropped_count(void);

JsonArray *pcv_audit_search(const gchar *from_ts, const gchar *to_ts,
                             const gchar *username, const gchar *method_pattern,
                             gint limit);

JsonArray *pcv_audit_recent_failures(const gchar *target_filter, gint limit);

gboolean pcv_audit_verify_chain(gsize *first_break_rowid);

G_END_DECLS

#endif
