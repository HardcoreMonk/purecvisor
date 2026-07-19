#include <glib.h>
#include <glib/gstdio.h>
#include "modules/security/security_store.h"

void pcv_test_audit_reset(void);
gint pcv_test_audit_call_count(void);
const gchar *pcv_test_audit_last_method(void);
const gchar *pcv_test_audit_last_target(void);
void pcv_test_alert_reset(void);
gint pcv_test_alert_call_count(void);
const gchar *pcv_test_alert_last_event_id(void);

void pcv_security_store_set_retention_cap_for_test(gint cap);

static gchar *
make_store_path(void)
{
    return g_strdup_printf("%s/pcv-security-test-%u.db",
                           g_get_tmp_dir(), g_random_int());
}

static void
unlink_store_path(const gchar *path)
{
    g_unlink(path);
    g_autofree gchar *wal = g_strdup_printf("%s-wal", path);
    g_autofree gchar *shm = g_strdup_printf("%s-shm", path);
    g_unlink(wal);
    g_unlink(shm);
}

static void
test_security_store_insert_list_get(void)
{

    gchar *path = make_store_path();
    g_assert_true(pcv_security_store_open(path));

    PcvSecurityEvent ev = {0};
    g_strlcpy(ev.event_id, "sec-store-1", sizeof ev.event_id);
    ev.timestamp = 1710000001;
    ev.source = PCV_SECURITY_SOURCE_RUNTIME;
    ev.type = PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS;
    ev.severity = PCV_SECURITY_SEVERITY_WARN;
    ev.confidence = 72;
    ev.target_kind = PCV_SECURITY_TARGET_PROCESS;
    g_strlcpy(ev.target, "pid:123", sizeof ev.target);
    g_strlcpy(ev.summary, "unexpected parent", sizeof ev.summary);
    ev.status = PCV_SECURITY_STATUS_OPEN;

    g_assert_true(pcv_security_store_insert_event(&ev, NULL));
    JsonArray *list = pcv_security_store_list_events(0, 10, NULL, NULL, NULL);
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    json_array_unref(list);

    JsonObject *obj = pcv_security_store_get_event("sec-store-1");
    g_assert_nonnull(obj);
    g_assert_cmpstr(json_object_get_string_member(obj, "event_id"), ==, "sec-store-1");
    json_object_unref(obj);

    PcvSecurityEvent ev2 = ev;
    g_strlcpy(ev2.event_id, "sec-store-2", sizeof ev2.event_id);
    g_assert_true(pcv_security_store_insert_event(&ev2, NULL));
    gchar *key = pcv_security_event_coalesce_key(&ev);
    g_assert_cmpint(pcv_security_store_count_by_coalesce_key(key), ==, 1);
    g_free(key);

    g_assert_false(pcv_security_store_get_bool_config("enabled", TRUE));
    g_assert_true(pcv_security_store_set_bool_config("enabled", TRUE, "admin", NULL));
    g_assert_true(pcv_security_store_get_bool_config("enabled", FALSE));

    pcv_security_store_close();
    unlink_store_path(path);
    g_free(path);
}

static void
test_security_store_submit_warn_audits_and_alerts(void)
{

    gchar *path = make_store_path();
    g_assert_true(pcv_security_store_open(path));

    PcvSecurityEvent ev = {0};
    g_strlcpy(ev.event_id, "sec-store-1", sizeof ev.event_id);
    ev.timestamp = 1710000002;
    ev.source = PCV_SECURITY_SOURCE_RUNTIME;
    ev.type = PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS;
    ev.severity = PCV_SECURITY_SEVERITY_INFO;
    ev.confidence = 88;
    ev.target_kind = PCV_SECURITY_TARGET_PROCESS;
    g_strlcpy(ev.target, "pid:987", sizeof ev.target);
    g_strlcpy(ev.summary, "unexpected executable", sizeof ev.summary);
    ev.status = PCV_SECURITY_STATUS_OPEN;

    pcv_test_audit_reset();
    pcv_test_alert_reset();

    GError *error = NULL;
    g_assert_true(pcv_security_submit_event(&ev, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(pcv_security_severity_to_string(ev.severity), ==, "warn");
    g_assert_cmpstr(ev.recommended_action, ==, "manual_runbook");
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "security.event");
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, "sec-store-1");
    g_assert_cmpint(pcv_test_alert_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_alert_last_event_id(), ==, "sec-store-1");

    pcv_security_store_close();
    unlink_store_path(path);
    g_free(path);
}

static gint
count_events_by_status(const gchar *status)
{
    JsonArray *list = pcv_security_store_list_events(0, 500, NULL, NULL, status);
    gint n = list ? (gint)json_array_get_length(list) : 0;
    if (list) {
        json_array_unref(list);
    }
    return n;
}

static void
insert_fixture_event(const gchar *event_id, gint64 ts, PcvSecurityStatus status)
{

    PcvSecurityEvent ev = {0};
    g_strlcpy(ev.event_id, event_id, sizeof ev.event_id);
    ev.timestamp = ts;
    ev.source = PCV_SECURITY_SOURCE_RUNTIME;
    ev.type = PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS;
    ev.severity = PCV_SECURITY_SEVERITY_WARN;
    ev.confidence = 50;
    ev.target_kind = PCV_SECURITY_TARGET_PROCESS;
    g_strlcpy(ev.target, event_id, sizeof ev.target);
    g_strlcpy(ev.summary, "retention fixture", sizeof ev.summary);
    ev.status = status;
    g_assert_true(pcv_security_store_insert_event(&ev, NULL));
}

static void
test_security_store_retention_bounds_terminal_events(void)
{

    gchar *path = make_store_path();
    g_assert_true(pcv_security_store_open(path));
    pcv_security_store_set_retention_cap_for_test(3);

    insert_fixture_event("res-1", 1710000001, PCV_SECURITY_STATUS_RESOLVED);
    insert_fixture_event("res-2", 1710000002, PCV_SECURITY_STATUS_RESOLVED);
    insert_fixture_event("sup-3", 1710000003, PCV_SECURITY_STATUS_SUPPRESSED);
    insert_fixture_event("res-4", 1710000004, PCV_SECURITY_STATUS_RESOLVED);
    insert_fixture_event("sup-5", 1710000005, PCV_SECURITY_STATUS_SUPPRESSED);

    insert_fixture_event("open-1", 1710000006, PCV_SECURITY_STATUS_OPEN);
    insert_fixture_event("open-2", 1710000007, PCV_SECURITY_STATUS_OPEN);

    g_assert_cmpint(count_events_by_status("resolved") +
                        count_events_by_status("suppressed"), ==, 3);
    g_assert_cmpint(count_events_by_status("open"), ==, 2);

    JsonObject *oldest = pcv_security_store_get_event("res-1");
    g_assert_null(oldest);
    JsonObject *newest = pcv_security_store_get_event("sup-5");
    g_assert_nonnull(newest);
    json_object_unref(newest);
    JsonObject *open_row = pcv_security_store_get_event("open-1");
    g_assert_nonnull(open_row);
    json_object_unref(open_row);

    pcv_security_store_set_retention_cap_for_test(0);
    pcv_security_store_close();
    unlink_store_path(path);
    g_free(path);
}

void
test_security_store_register(void)
{
    g_test_add_func("/security/store/insert-list-get", test_security_store_insert_list_get);
    g_test_add_func("/security/store/submit-warn-audits-and-alerts",
                    test_security_store_submit_warn_audits_and_alerts);
    g_test_add_func("/security/store/retention-bounds-terminal-events",
                    test_security_store_retention_bounds_terminal_events);
}
