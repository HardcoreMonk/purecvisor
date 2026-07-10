#include <glib.h>
#include <json-glib/json-glib.h>
#include "modules/security/security_event.h"

static void
test_security_event_roundtrip(void)
{
    PcvSecurityEvent ev = {0};
    g_strlcpy(ev.event_id, "sec-123", sizeof ev.event_id);
    ev.timestamp = 1710000000;
    ev.source = PCV_SECURITY_SOURCE_FILE_INTEGRITY;
    ev.type = PCV_SECURITY_EVENT_FILE_CHANGED;
    ev.severity = PCV_SECURITY_SEVERITY_CRIT;
    ev.confidence = 95;
    ev.target_kind = PCV_SECURITY_TARGET_FILE;
    g_strlcpy(ev.target, "/etc/purecvisor/daemon.conf", sizeof ev.target);
    g_strlcpy(ev.summary, "daemon.conf changed", sizeof ev.summary);
    g_strlcpy(ev.recommended_action, "manual_runbook", sizeof ev.recommended_action);
    ev.status = PCV_SECURITY_STATUS_OPEN;

    JsonObject *obj = pcv_security_event_to_json(&ev);
    g_assert_nonnull(obj);
    g_assert_cmpstr(json_object_get_string_member(obj, "event_id"), ==, "sec-123");
    g_assert_cmpstr(json_object_get_string_member(obj, "severity"), ==, "crit");
    g_assert_cmpstr(json_object_get_string_member(obj, "target_kind"), ==, "file");

    PcvSecurityEvent parsed = {0};
    g_assert_true(pcv_security_event_from_json(obj, &parsed));
    g_assert_cmpstr(parsed.event_id, ==, ev.event_id);
    g_assert_cmpint(parsed.severity, ==, PCV_SECURITY_SEVERITY_CRIT);
    g_assert_cmpint(parsed.status, ==, PCV_SECURITY_STATUS_OPEN);
    json_object_unref(obj);
}

/* [B-2] evidence 가드 — 정상 통과 / 초과 시 유효 fallback JSON */
static void
test_security_event_evidence_guard(void)
{
    gchar buf[64];

    pcv_security_event_set_evidence(buf, sizeof buf, "{\"k\":1}");
    g_assert_cmpstr(buf, ==, "{\"k\":1}");

    gchar *big = g_strnfill(200, 'x');
    pcv_security_event_set_evidence(buf, sizeof buf, big);
    g_assert_true(g_str_has_prefix(buf, "{\"evidence_truncated\":true"));
    g_assert_true(g_str_has_suffix(buf, "}"));   /* 유효 JSON 로 닫힘 */
    g_free(big);

    pcv_security_event_set_evidence(buf, sizeof buf, NULL);
    g_assert_true(g_str_has_prefix(buf, "{\"evidence_truncated\":true"));
}

void
test_security_event_register(void)
{
    g_test_add_func("/security/event/roundtrip", test_security_event_roundtrip);
    g_test_add_func("/security/event/evidence_guard", test_security_event_evidence_guard);
}
