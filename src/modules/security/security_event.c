#include "modules/security/security_event.h"

/*
 * Security events cross process boundaries as stable lowercase strings. These
 * maps are the canonical translation layer between C enums, SQLite rows, JSON
 * RPC responses, and Web UI filters.
 */
typedef struct {
    gint value;
    const gchar *name;
} PcvSecurityStringMap;

static const PcvSecurityStringMap k_sources[] = {
    { PCV_SECURITY_SOURCE_FILE_INTEGRITY, "file_integrity" },
    { PCV_SECURITY_SOURCE_RUNTIME, "runtime" },
    { PCV_SECURITY_SOURCE_LOG, "log" },
    { PCV_SECURITY_SOURCE_PCV_AUDIT, "pcv_audit" },
};

static const PcvSecurityStringMap k_types[] = {
    { PCV_SECURITY_EVENT_FILE_CHANGED, "file_changed" },
    { PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS, "process_suspicious" },
    { PCV_SECURITY_EVENT_APPARMOR_DENIED, "apparmor_denied" },
    { PCV_SECURITY_EVENT_AUTH_BRUTEFORCE, "auth_bruteforce" },
    { PCV_SECURITY_EVENT_AUDIT_PATTERN, "audit_pattern" },
};

static const PcvSecurityStringMap k_severities[] = {
    { PCV_SECURITY_SEVERITY_INFO, "info" },
    { PCV_SECURITY_SEVERITY_WARN, "warn" },
    { PCV_SECURITY_SEVERITY_CRIT, "crit" },
};

static const PcvSecurityStringMap k_target_kinds[] = {
    { PCV_SECURITY_TARGET_FILE, "file" },
    { PCV_SECURITY_TARGET_PROCESS, "process" },
    { PCV_SECURITY_TARGET_IP, "ip" },
    { PCV_SECURITY_TARGET_USER, "user" },
    { PCV_SECURITY_TARGET_API_KEY, "api_key" },
    { PCV_SECURITY_TARGET_SERVICE, "service" },
    { PCV_SECURITY_TARGET_VM, "vm" },
    { PCV_SECURITY_TARGET_HOST, "host" },
};

static const PcvSecurityStringMap k_statuses[] = {
    { PCV_SECURITY_STATUS_OPEN, "open" },
    { PCV_SECURITY_STATUS_SUPPRESSED, "suppressed" },
    { PCV_SECURITY_STATUS_ACTION_PENDING, "action_pending" },
    { PCV_SECURITY_STATUS_RESOLVED, "resolved" },
};

static const gchar *
lookup_name(const PcvSecurityStringMap *items, gsize n_items, gint value)
{
    for (gsize i = 0; i < n_items; i++) {
        if (items[i].value == value) {
            return items[i].name;
        }
    }
    return items[0].name;
}

static gboolean
lookup_value(const PcvSecurityStringMap *items, gsize n_items, const gchar *name, gint *out)
{
    if (!name || !out) {
        return FALSE;
    }

    for (gsize i = 0; i < n_items; i++) {
        if (g_strcmp0(items[i].name, name) == 0) {
            *out = items[i].value;
            return TRUE;
        }
    }

    return FALSE;
}

static const gchar *
json_get_string_or_empty(JsonObject *obj, const gchar *name)
{
    if (!json_object_has_member(obj, name)) {
        return "";
    }
    return json_object_get_string_member(obj, name);
}

const gchar *
pcv_security_source_to_string(PcvSecuritySource v)
{
    return lookup_name(k_sources, G_N_ELEMENTS(k_sources), (gint)v);
}

const gchar *
pcv_security_type_to_string(PcvSecurityEventType v)
{
    return lookup_name(k_types, G_N_ELEMENTS(k_types), (gint)v);
}

const gchar *
pcv_security_severity_to_string(PcvSecuritySeverity v)
{
    return lookup_name(k_severities, G_N_ELEMENTS(k_severities), (gint)v);
}

const gchar *
pcv_security_target_kind_to_string(PcvSecurityTargetKind v)
{
    return lookup_name(k_target_kinds, G_N_ELEMENTS(k_target_kinds), (gint)v);
}

const gchar *
pcv_security_status_to_string(PcvSecurityStatus v)
{
    return lookup_name(k_statuses, G_N_ELEMENTS(k_statuses), (gint)v);
}

gboolean
pcv_security_severity_from_string(const gchar *s, PcvSecuritySeverity *out)
{
    gint value = 0;
    if (!lookup_value(k_severities, G_N_ELEMENTS(k_severities), s, &value)) {
        return FALSE;
    }
    if (out) {
        *out = (PcvSecuritySeverity)value;
    }
    return TRUE;
}

gboolean
pcv_security_status_from_string(const gchar *s, PcvSecurityStatus *out)
{
    gint value = 0;
    if (!lookup_value(k_statuses, G_N_ELEMENTS(k_statuses), s, &value)) {
        return FALSE;
    }
    if (out) {
        *out = (PcvSecurityStatus)value;
    }
    return TRUE;
}

JsonObject *
pcv_security_event_to_json(const PcvSecurityEvent *ev)
{
    if (!ev) {
        return NULL;
    }

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "event_id", ev->event_id);
    json_object_set_int_member(obj, "timestamp", ev->timestamp);
    json_object_set_string_member(obj, "source", pcv_security_source_to_string(ev->source));
    json_object_set_string_member(obj, "type", pcv_security_type_to_string(ev->type));
    json_object_set_string_member(obj, "severity", pcv_security_severity_to_string(ev->severity));
    json_object_set_int_member(obj, "confidence", CLAMP(ev->confidence, 0, 100));
    json_object_set_string_member(obj, "target_kind",
                                  pcv_security_target_kind_to_string(ev->target_kind));
    json_object_set_string_member(obj, "target", ev->target);
    json_object_set_string_member(obj, "summary", ev->summary);
    json_object_set_string_member(obj, "recommended_action", ev->recommended_action);
    json_object_set_string_member(obj, "status", pcv_security_status_to_string(ev->status));
    json_object_set_string_member(obj, "evidence_json", ev->evidence_json);
    return obj;
}

gboolean
pcv_security_event_from_json(JsonObject *obj, PcvSecurityEvent *out)
{
    if (!obj || !out) {
        return FALSE;
    }

    gint value = 0;
    const gchar *source = json_get_string_or_empty(obj, "source");
    const gchar *type = json_get_string_or_empty(obj, "type");
    const gchar *severity = json_get_string_or_empty(obj, "severity");
    const gchar *target_kind = json_get_string_or_empty(obj, "target_kind");
    const gchar *status = json_get_string_or_empty(obj, "status");

    if (!lookup_value(k_sources, G_N_ELEMENTS(k_sources), source, &value)) {
        return FALSE;
    }
    out->source = (PcvSecuritySource)value;

    if (!lookup_value(k_types, G_N_ELEMENTS(k_types), type, &value)) {
        return FALSE;
    }
    out->type = (PcvSecurityEventType)value;

    if (!lookup_value(k_severities, G_N_ELEMENTS(k_severities), severity, &value)) {
        return FALSE;
    }
    out->severity = (PcvSecuritySeverity)value;

    if (!lookup_value(k_target_kinds, G_N_ELEMENTS(k_target_kinds), target_kind, &value)) {
        return FALSE;
    }
    out->target_kind = (PcvSecurityTargetKind)value;

    if (!lookup_value(k_statuses, G_N_ELEMENTS(k_statuses), status, &value)) {
        return FALSE;
    }
    out->status = (PcvSecurityStatus)value;

    g_strlcpy(out->event_id, json_get_string_or_empty(obj, "event_id"), sizeof out->event_id);
    out->timestamp = json_object_has_member(obj, "timestamp")
        ? json_object_get_int_member(obj, "timestamp")
        : 0;
    out->confidence = json_object_has_member(obj, "confidence")
        ? CLAMP((gint)json_object_get_int_member(obj, "confidence"), 0, 100)
        : 0;
    g_strlcpy(out->target, json_get_string_or_empty(obj, "target"), sizeof out->target);
    g_strlcpy(out->summary, json_get_string_or_empty(obj, "summary"), sizeof out->summary);
    g_strlcpy(out->recommended_action,
              json_get_string_or_empty(obj, "recommended_action"),
              sizeof out->recommended_action);
    /* [B-2] 역직렬화 site 도 프로듀서와 동일 가드 — 저장값이 버퍼 초과면
     * 중간 절단된 invalid JSON 대신 유효 fallback (M-10 대칭성 완결). */
    pcv_security_event_set_evidence(out->evidence_json,
                                    sizeof out->evidence_json,
                                    json_get_string_or_empty(obj, "evidence_json"));
    return TRUE;
}

/* [M-10/B-2] evidence_json 고정 버퍼 가드 — 프로듀서(SG 2곳)와 역직렬화 site 공용.
 * 초과 시 값 중간 절단(invalid JSON) 대신 유효 fallback 을 저장한다. */
void
pcv_security_event_set_evidence(gchar *dst, gsize dstsz, const gchar *ejstr)
{
    if (ejstr && strlen(ejstr) < dstsz)
        g_strlcpy(dst, ejstr, dstsz);
    else
        g_snprintf(dst, dstsz, "{\"evidence_truncated\":true,\"bytes\":%zu}",
                   ejstr ? strlen(ejstr) : 0);
}

void
pcv_security_event_make_id(PcvSecurityEvent *ev, const gchar *prefix)
{
    if (!ev) {
        return;
    }

    const gchar *p = (prefix && *prefix) ? prefix : "sec";
    g_snprintf(ev->event_id, sizeof ev->event_id,
               "%s-%" G_GINT64_FORMAT "-%08x",
               p, g_get_real_time(), g_random_int());
}

gchar *
pcv_security_event_coalesce_key(const PcvSecurityEvent *ev)
{
    if (!ev) {
        return g_strdup("");
    }

    /*
     * Exclude event_id and timestamp so repeated observations of the same risk
     * merge into one open queue item until the operator resolves or suppresses it.
     */
    return g_strdup_printf("%s:%s:%s:%s:%s",
                           pcv_security_source_to_string(ev->source),
                           pcv_security_type_to_string(ev->type),
                           pcv_security_target_kind_to_string(ev->target_kind),
                           ev->target,
                           ev->recommended_action);
}
