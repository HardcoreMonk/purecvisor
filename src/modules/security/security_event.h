#ifndef PURECVISOR_SECURITY_EVENT_H
#define PURECVISOR_SECURITY_EVENT_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS





typedef enum {
    PCV_SECURITY_SOURCE_FILE_INTEGRITY,
    PCV_SECURITY_SOURCE_RUNTIME,
    PCV_SECURITY_SOURCE_LOG,
    PCV_SECURITY_SOURCE_PCV_AUDIT
} PcvSecuritySource;

typedef enum {
    PCV_SECURITY_EVENT_FILE_CHANGED,
    PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS,
    PCV_SECURITY_EVENT_APPARMOR_DENIED,
    PCV_SECURITY_EVENT_AUTH_BRUTEFORCE,
    PCV_SECURITY_EVENT_AUDIT_PATTERN
} PcvSecurityEventType;

typedef enum {
    PCV_SECURITY_SEVERITY_INFO,
    PCV_SECURITY_SEVERITY_WARN,
    PCV_SECURITY_SEVERITY_CRIT
} PcvSecuritySeverity;

typedef enum {
    PCV_SECURITY_TARGET_FILE,
    PCV_SECURITY_TARGET_PROCESS,
    PCV_SECURITY_TARGET_IP,
    PCV_SECURITY_TARGET_USER,
    PCV_SECURITY_TARGET_API_KEY,
    PCV_SECURITY_TARGET_SERVICE,
    PCV_SECURITY_TARGET_VM,
    PCV_SECURITY_TARGET_HOST
} PcvSecurityTargetKind;

typedef enum {
    PCV_SECURITY_STATUS_OPEN,
    PCV_SECURITY_STATUS_SUPPRESSED,
    PCV_SECURITY_STATUS_ACTION_PENDING,
    PCV_SECURITY_STATUS_RESOLVED
} PcvSecurityStatus;

typedef struct {

    gchar event_id[64];
    gint64 timestamp;
    PcvSecuritySource source;
    PcvSecurityEventType type;
    PcvSecuritySeverity severity;
    gint confidence;
    PcvSecurityTargetKind target_kind;
    gchar target[256];
    gchar summary[256];
    gchar recommended_action[64];
    PcvSecurityStatus status;
    gchar evidence_json[2048];
} PcvSecurityEvent;

const gchar *pcv_security_source_to_string(PcvSecuritySource v);
const gchar *pcv_security_type_to_string(PcvSecurityEventType v);
const gchar *pcv_security_severity_to_string(PcvSecuritySeverity v);
const gchar *pcv_security_target_kind_to_string(PcvSecurityTargetKind v);
const gchar *pcv_security_status_to_string(PcvSecurityStatus v);

gboolean pcv_security_severity_from_string(const gchar *s, PcvSecuritySeverity *out);
gboolean pcv_security_status_from_string(const gchar *s, PcvSecurityStatus *out);
JsonObject *pcv_security_event_to_json(const PcvSecurityEvent *ev);
gboolean pcv_security_event_from_json(JsonObject *obj, PcvSecurityEvent *out);
void pcv_security_event_make_id(PcvSecurityEvent *ev, const gchar *prefix);
gchar *pcv_security_event_coalesce_key(const PcvSecurityEvent *ev);

G_END_DECLS

#endif
