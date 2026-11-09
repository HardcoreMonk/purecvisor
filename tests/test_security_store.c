                                                                                   
                                                                                                     
                                                                  
                                                            
                        
                              
  
                                                                           
  
                                             
                                                                 
                                                        
                                                    
                                                                             
                                                                          
                                                                 
                                                      
                  
                                                                             
                                                      
                                                      
  
                                       
   
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
const gchar *pcv_test_audit_method_at(gint row);
const gchar *pcv_test_audit_target_at(gint row);
                                                                                    
void pcv_security_store_set_retention_cap_for_test(gint cap);

void pcv_security_store_set_now_for_test(gint64 t);
void pcv_sec_bounds_reset_for_test(gint64 now);
void pcv_sec_bounds_set_caps_for_test(gint cap, gint batch);

gchar *pcv_sec_bounds_close_note_sanitize(const gchar *raw);

#define SEC_T0 ((gint64)1710000100)

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

static gchar *
open_test_store(gint64 now)
{
    pcv_sec_bounds_reset_for_test(now);
    pcv_security_store_set_now_for_test(now);
    gchar *path = make_store_path();
    g_assert_true(pcv_security_store_open(path));
    pcv_test_audit_reset();
    pcv_test_alert_reset();
    return path;
}

static void
close_test_store(gchar *path)
{
    pcv_security_store_close();
    pcv_security_store_set_now_for_test(0);
    pcv_sec_bounds_reset_for_test(0);
    pcv_security_store_set_retention_cap_for_test(0);
    unlink_store_path(path);
    g_free(path);
}

static PcvSecurityEvent
make_event(PcvSecuritySource source, PcvSecuritySeverity severity, const gchar *target)
{
    PcvSecurityEvent ev = {0};
    ev.source = source;
    ev.type = PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS;
    ev.severity = severity;
    ev.confidence = 60;
    ev.target_kind = PCV_SECURITY_TARGET_PROCESS;
    g_strlcpy(ev.target, target, sizeof ev.target);
    g_strlcpy(ev.summary, "bounds fixture", sizeof ev.summary);
    ev.status = PCV_SECURITY_STATUS_OPEN;
    return ev;
}

static void
submit_fresh(const PcvSecurityEvent *tmpl, gchar *id_out)
{
    PcvSecurityEvent ev = *tmpl;
    ev.event_id[0] = '\0';
    GError *error = NULL;
    g_assert_true(pcv_security_submit_event(&ev, &error));
    g_assert_no_error(error);
    if (id_out) {
        g_strlcpy(id_out, ev.event_id, 64);
    }
}

static void
submit_aged(PcvSecuritySource source, PcvSecuritySeverity severity,
            const gchar *prefix, gint n, gint64 base_ts, gchar (*ids)[64])
{
    for (gint i = 0; i < n; i++) {
        g_autofree gchar *target = g_strdup_printf("%s-%d", prefix, i);
        PcvSecurityEvent t = make_event(source, severity, target);
        t.timestamp = base_ts > 0 ? base_ts + i : 0;
        submit_fresh(&t, ids ? ids[i] : NULL);
    }
}

static void
submit_distinct(PcvSecuritySource source, PcvSecuritySeverity severity,
                const gchar *prefix, gint n, gchar (*ids)[64])
{
    submit_aged(source, severity, prefix, n, 0, ids);
}

static gint64
event_int_member(const gchar *event_id, const gchar *member)
{
    JsonObject *obj = pcv_security_store_get_event(event_id);
    g_assert_nonnull(obj);
    gint64 v = json_object_get_int_member(obj, member);
    json_object_unref(obj);
    return v;
}

static void
assert_event_state(const gchar *event_id, const gchar *status, const gchar *closed_reason)
{
    JsonObject *obj = pcv_security_store_get_event(event_id);
    g_assert_nonnull(obj);
    g_assert_cmpstr(json_object_get_string_member(obj, "status"), ==, status);
    g_assert_cmpstr(json_object_get_string_member(obj, "closed_reason"), ==, closed_reason);
    json_object_unref(obj);
}

static void
test_security_store_insert_list_get(void)
{
                                                                                
    gchar *path = open_test_store(SEC_T0);

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

    close_test_store(path);
}

static void
test_security_store_submit_warn_audits_and_alerts(void)
{
                                                                                  
    gchar *path = open_test_store(SEC_T0);

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

    close_test_store(path);
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
insert_fixture_event(const gchar *event_id, gint64 ts, PcvSecurityStatus status,
                     PcvSecuritySeverity severity)
{
                                                                                
    PcvSecurityEvent ev = {0};
    g_strlcpy(ev.event_id, event_id, sizeof ev.event_id);
    ev.timestamp = ts;
    ev.source = PCV_SECURITY_SOURCE_RUNTIME;
    ev.type = PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS;
    ev.severity = severity;
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
      
                                                                                  
                                                                                 
                                                             
       
    gchar *path = open_test_store(SEC_T0);
    pcv_security_store_set_retention_cap_for_test(3);

                                                                                 
    const PcvSecuritySeverity warn = PCV_SECURITY_SEVERITY_WARN;
    insert_fixture_event("res-1", 1710000001, PCV_SECURITY_STATUS_RESOLVED, warn);
    insert_fixture_event("res-2", 1710000002, PCV_SECURITY_STATUS_RESOLVED, warn);
    insert_fixture_event("sup-3", 1710000003, PCV_SECURITY_STATUS_SUPPRESSED, warn);
    insert_fixture_event("res-4", 1710000004, PCV_SECURITY_STATUS_RESOLVED, warn);
    insert_fixture_event("sup-5", 1710000005, PCV_SECURITY_STATUS_SUPPRESSED, warn);

                                                                     
    insert_fixture_event("open-1", 1710000006, PCV_SECURITY_STATUS_OPEN, PCV_SECURITY_SEVERITY_CRIT);
    insert_fixture_event("open-2", 1710000007, PCV_SECURITY_STATUS_OPEN, PCV_SECURITY_SEVERITY_CRIT);

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

    close_test_store(path);
}

static void
seed_repeats(const PcvSecurityEvent *t, gint n, gchar *first_id)
{
    submit_fresh(t, first_id);
    for (gint i = 1; i < n; i++) {
        submit_fresh(t, NULL);
    }
}

static void
test_security_store_coalesced_repeats_not_audited(void)
{

    gchar *path = open_test_store(SEC_T0);
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:rep");
    gchar first[64];
    seed_repeats(&t, 10, first);

    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "security.event");
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, first);
    g_assert_cmpint(pcv_test_alert_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_alert_last_event_id(), ==, first);
    g_assert_cmpint(event_int_member(first, "occurrence_count"), ==, 10);
    close_test_store(path);
}

static void
test_security_store_repeat_summary_hourly(void)
{
    gchar *path = open_test_store(SEC_T0);
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:rep");
    gchar first[64];
    seed_repeats(&t, 10, first);

    pcv_security_store_set_now_for_test(SEC_T0 + 3600);
    gchar last[64];
    submit_fresh(&t, last);
    g_assert_cmpstr(last, !=, first);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 2);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "security.event.repeat");
    g_autofree gchar *want = g_strdup_printf("%s#n=10", first);
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, want);
    g_assert_cmpint(pcv_test_alert_call_count(), ==, 1);

    submit_fresh(&t, NULL);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 2);
    close_test_store(path);
}

static void
test_security_store_repeat_summary_respects_policy(void)
{

    gchar *path = open_test_store(SEC_T0);
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_INFO, "pid:info");
    seed_repeats(&t, 3, NULL);
    pcv_security_store_set_now_for_test(SEC_T0 + 3600);
    submit_fresh(&t, NULL);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 0);
    g_assert_cmpint(pcv_test_alert_call_count(), ==, 0);
    close_test_store(path);
}

static void
test_security_store_repeat_summary_survives_reopen(void)
{

    gchar *path = open_test_store(SEC_T0);
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:reopen");
    seed_repeats(&t, 2, NULL);
    pcv_security_store_set_now_for_test(SEC_T0 + 3600);
    submit_fresh(&t, NULL);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "security.event.repeat");
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 2);

    pcv_security_store_close();
    pcv_sec_bounds_reset_for_test(SEC_T0 + 3600);
    g_assert_true(pcv_security_store_open(path));
    pcv_security_store_set_now_for_test(SEC_T0 + 3600 + 1800);
    submit_fresh(&t, NULL);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 2);
    close_test_store(path);
}

static void
test_security_store_repeat_summary_clock_backwards_rebases(void)
{

    gchar *path = open_test_store(SEC_T0);
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:ntp");
    gchar first[64];
    seed_repeats(&t, 3, first);

    const gint64 back = SEC_T0 - 600;
    pcv_security_store_set_now_for_test(back);
    submit_fresh(&t, NULL);
    pcv_security_store_set_now_for_test(back + 3599);
    submit_fresh(&t, NULL);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);

    pcv_security_store_set_now_for_test(back + 3600);
    submit_fresh(&t, NULL);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 2);
    g_autofree gchar *want = g_strdup_printf("%s#n=5", first);
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, want);
    close_test_store(path);
}

static void
test_security_store_new_event_budget_floods_to_summary(void)
{

    gchar *path = open_test_store(SEC_T0);
    submit_distinct(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:flood", 505, NULL);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 500);
    g_assert_cmpint(pcv_test_alert_call_count(), ==, 500);

    pcv_security_store_set_now_for_test(SEC_T0 + 3600);
    pcv_test_audit_reset();
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:after");
    gchar id[64];
    submit_fresh(&t, id);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 2);
    g_autofree gchar *want = g_strdup_printf("lsm#n=5,since=%" G_GINT64_FORMAT, SEC_T0);
    g_assert_cmpstr(pcv_test_audit_method_at(0), ==, "security.event.flood");
    g_assert_cmpstr(pcv_test_audit_target_at(0), ==, want);
    g_assert_cmpstr(pcv_test_audit_method_at(1), ==, "security.event");
    g_assert_cmpstr(pcv_test_audit_target_at(1), ==, id);
    close_test_store(path);
}

static void
test_security_store_flood_reported_on_other_source_submit(void)
{

    gchar *path = open_test_store(SEC_T0);
    PcvSecurityEvent b = make_event(PCV_SECURITY_SOURCE_SURICATA, PCV_SECURITY_SEVERITY_INFO, "ip:b");
    submit_fresh(&b, NULL);
    submit_distinct(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:a", 503, NULL);

    pcv_security_store_set_now_for_test(SEC_T0 + 3600);
    pcv_test_audit_reset();
    submit_fresh(&b, NULL);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_autofree gchar *want = g_strdup_printf("lsm#n=3,since=%" G_GINT64_FORMAT, SEC_T0);
    g_assert_cmpstr(pcv_test_audit_method_at(0), ==, "security.event.flood");
    g_assert_cmpstr(pcv_test_audit_target_at(0), ==, want);
    close_test_store(path);
}

static void
test_security_store_repeat_summaries_bounded_by_source_budget(void)
{

    enum { KEYS = 605 };
    gchar (*ids)[64] = g_malloc0(sizeof *ids * KEYS);
    gchar *path = open_test_store(SEC_T0);
    submit_distinct(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, "pid:many", KEYS, ids);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 600);

    pcv_security_store_set_now_for_test(SEC_T0 + 3600);
    pcv_test_audit_reset();
    submit_distinct(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, "pid:many", KEYS, NULL);
    g_assert_cmpstr(pcv_test_audit_method_at(0), ==, "security.event.flood");
    g_assert_cmpstr(pcv_test_audit_method_at(1), ==, "security.event.repeat");
    g_assert_cmpint(pcv_test_audit_call_count() - 1, <=, 600);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 601);

    pcv_security_store_set_now_for_test(SEC_T0 + 7200);
    pcv_test_audit_reset();
    for (gint i = KEYS - 5; i < KEYS; i++) {
        g_autofree gchar *target = g_strdup_printf("pid:many-%d", i);
        PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, target);
        submit_fresh(&t, NULL);
    }
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 5);
    for (gint i = 0; i < 5; i++) {
        g_autofree gchar *want = g_strdup_printf("%s#n=2", ids[KEYS - 5 + i]);
        g_assert_cmpstr(pcv_test_audit_method_at(i), ==, "security.event.repeat");
        g_assert_cmpstr(pcv_test_audit_target_at(i), ==, want);
    }
    close_test_store(path);
    g_free(ids);
}

static void
test_security_store_warn_flood_leaves_crit_share(void)
{

    gchar *path = open_test_store(SEC_T0);
    submit_distinct(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:warn", 600, NULL);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 500);

    pcv_test_audit_reset();
    pcv_test_alert_reset();
    PcvSecurityEvent c = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, "pid:crit");
    gchar id[64];
    submit_fresh(&c, id);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "security.event");
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, id);
    g_assert_cmpint(pcv_test_alert_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_alert_last_event_id(), ==, id);
    close_test_store(path);
}

static gchar *
open_capped_store(void)
{
    gchar *path = open_test_store(SEC_T0);
    pcv_sec_bounds_set_caps_for_test(5, 3);
    return path;
}

static void
test_security_store_overflow_folds_oldest_noncrit(void)
{

    gchar *path = open_capped_store();
    gchar ids[8][64];
    submit_aged(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_INFO, "pid:fold", 8, SEC_T0 - 100, ids);
    for (gint i = 0; i < 8; i++) {
        assert_event_state(ids[i], i < 3 ? "suppressed" : "open", i < 3 ? "overflow" : "");
    }
    g_assert_cmpint(count_events_by_status("open"), ==, 5);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_method_at(0), ==, "security.event.overflow");
    g_autofree gchar *want = g_strdup_printf("security_events#folded=3,oldest=%" G_GINT64_FORMAT,
                                             SEC_T0 - 100);
    g_assert_cmpstr(pcv_test_audit_target_at(0), ==, want);
    close_test_store(path);
}

static void
test_security_store_overflow_folds_info_rows(void)
{

    gchar *path = open_capped_store();
    gchar ids[6][64];
    submit_aged(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_INFO, "pid:info", 6, SEC_T0 - 100, ids);
    for (gint i = 0; i < 6; i++) {
        assert_event_state(ids[i], i < 3 ? "suppressed" : "open", i < 3 ? "overflow" : "");
    }
    close_test_store(path);
}

static void
add_pending_action(const gchar *event_id, gint ttl_sec)
{
    PcvSecurityEvent ev = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:act");
    g_strlcpy(ev.event_id, event_id, sizeof ev.event_id);
    GError *error = NULL;
    g_assert_true(pcv_security_store_upsert_pending_action(&ev, "manual_runbook", ttl_sec, &error));
    g_assert_no_error(error);
}

static void
test_security_store_overflow_skips_crit_and_pending(void)
{

    gchar *path = open_capped_store();
    gchar c1[64], c2[64], p1[64], ap[64], e1[64];
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, "pid:c1");
    t.timestamp = SEC_T0 - 60;
    submit_fresh(&t, c1);
    t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, "pid:c2");
    t.timestamp = SEC_T0 - 59;
    submit_fresh(&t, c2);
    t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:p1");
    t.timestamp = SEC_T0 - 58;
    submit_fresh(&t, p1);
    add_pending_action(p1, 3600);
    t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:ap");
    t.timestamp = SEC_T0 - 57;
    t.status = PCV_SECURITY_STATUS_ACTION_PENDING;
    submit_fresh(&t, ap);
    t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:e1");
    t.timestamp = SEC_T0 - 56;
    submit_fresh(&t, e1);
    add_pending_action(e1, 10);

    pcv_security_store_set_now_for_test(SEC_T0 + 20);
    gchar n[5][64];
    submit_aged(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_INFO, "pid:n", 5, SEC_T0 - 40, n);

    assert_event_state(c1, "open", "");
    assert_event_state(c2, "open", "");
    assert_event_state(p1, "open", "");
    assert_event_state(ap, "action_pending", "");
    assert_event_state(e1, "suppressed", "overflow");
    assert_event_state(n[0], "suppressed", "overflow");
    assert_event_state(n[1], "suppressed", "overflow");
    for (gint i = 2; i < 5; i++) {
        assert_event_state(n[i], "open", "");
    }
    close_test_store(path);
}

static void
test_security_store_crit_open_over_cap_not_folded_health_warns(void)
{

    gchar *path = open_capped_store();
    submit_distinct(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, "pid:crit", 5, NULL);
    JsonObject *h = pcv_security_store_health();
    g_assert_false(json_object_get_boolean_member(h, "protected_open_over_cap"));
    g_assert_cmpint(json_object_get_int_member(h, "protected_open_count"), ==, 5);
    json_object_unref(h);

    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, "pid:crit-6");
    submit_fresh(&t, NULL);
    g_assert_cmpint(count_events_by_status("suppressed"), ==, 0);
    g_assert_cmpint(count_events_by_status("open"), ==, 6);
    h = pcv_security_store_health();
    g_assert_true(json_object_get_boolean_member(h, "protected_open_over_cap"));
    g_assert_cmpint(json_object_get_int_member(h, "protected_open_count"), ==, 6);
    g_assert_false(json_object_get_boolean_member(h, "degraded"));
    json_object_unref(h);
    close_test_store(path);
}

static void
test_security_store_expire_after_14_days(void)
{

    gchar *path = open_capped_store();
    const gint64 day = 86400;
    gchar old_warn[64], young[64], old_crit[64];
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:old");
    t.timestamp = SEC_T0 - 15 * day;
    submit_fresh(&t, old_warn);
    t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:young");
    t.timestamp = SEC_T0 - 13 * day;
    submit_fresh(&t, young);
    t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, "pid:oldcrit");
    t.timestamp = SEC_T0 - 15 * day;
    submit_fresh(&t, old_crit);
    assert_event_state(old_warn, "open", "");

    pcv_security_store_set_now_for_test(SEC_T0 + 3600);
    pcv_test_audit_reset();
    PcvSecurityEvent tick = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_INFO, "pid:tick");
    submit_fresh(&tick, NULL);
    assert_event_state(old_warn, "suppressed", "expired");
    assert_event_state(young, "open", "");
    assert_event_state(old_crit, "open", "");
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_method_at(0), ==, "security.event.expire");
    g_autofree gchar *want = g_strdup_printf("security_events#expired=1,before=%" G_GINT64_FORMAT,
                                             SEC_T0 + 3600 - 1209600);
    g_assert_cmpstr(pcv_test_audit_target_at(0), ==, want);
    close_test_store(path);
}

static void
test_security_store_expire_timer_clock_backwards(void)
{

    gchar *path = open_capped_store();
    gchar stale[64];
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_INFO, "pid:stale");
    t.timestamp = SEC_T0 - 20 * 86400;
    submit_fresh(&t, stale);

    PcvSecurityEvent tick = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_INFO, "pid:tick");
    const gint64 back = SEC_T0 - 1000;
    pcv_security_store_set_now_for_test(back);
    submit_fresh(&tick, NULL);
    assert_event_state(stale, "open", "");
    pcv_security_store_set_now_for_test(back + 3599);
    submit_fresh(&tick, NULL);
    assert_event_state(stale, "open", "");

    pcv_security_store_set_now_for_test(back + 3600);
    pcv_test_audit_reset();
    submit_fresh(&tick, NULL);
    assert_event_state(stale, "suppressed", "expired");
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_method_at(0), ==, "security.event.expire");
    close_test_store(path);
}

static void
test_security_store_folded_key_reopens_as_new_row(void)
{

    gchar *path = open_capped_store();
    gchar ids[6][64];
    submit_aged(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:reo", 6, SEC_T0 - 100, ids);
    assert_event_state(ids[0], "suppressed", "overflow");

    pcv_test_audit_reset();
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:reo-0");
    gchar again[64];
    submit_fresh(&t, again);
    g_assert_cmpstr(again, !=, ids[0]);
    assert_event_state(again, "open", "");
    assert_event_state(ids[0], "suppressed", "overflow");
    g_assert_cmpint(event_int_member(ids[0], "occurrence_count"), ==, 1);
    g_assert_cmpint(event_int_member(again, "occurrence_count"), ==, 1);
    g_strlcpy(t.recommended_action, "manual_runbook", sizeof t.recommended_action);
    g_autofree gchar *key = pcv_security_event_coalesce_key(&t);
    g_assert_cmpint(pcv_security_store_count_by_coalesce_key(key), ==, 2);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "security.event");
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, again);

    pcv_security_store_set_now_for_test(SEC_T0 + 3600);
    pcv_test_audit_reset();
    submit_fresh(&t, NULL);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "security.event.repeat");
    g_autofree gchar *want = g_strdup_printf("%s#n=1", again);
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, want);
    close_test_store(path);
}

static void
test_security_store_event_json_has_closed_reason(void)
{

    gchar *path = open_capped_store();
    gchar ids[6][64];
    submit_aged(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_INFO, "pid:json", 6, SEC_T0 - 100, ids);
    assert_event_state(ids[0], "suppressed", "overflow");
    assert_event_state(ids[5], "open", "");
    JsonArray *list = pcv_security_store_list_events(0, 10, NULL, NULL, NULL);
    g_assert_cmpuint(json_array_get_length(list), ==, 6);
    for (guint i = 0; i < json_array_get_length(list); i++) {
        JsonObject *o = json_array_get_object_element(list, i);
        const gchar *status = json_object_get_string_member(o, "status");
        g_assert_true(json_object_has_member(o, "closed_reason"));
        g_assert_cmpstr(json_object_get_string_member(o, "closed_reason"), ==,
                        g_strcmp0(status, "suppressed") == 0 ? "overflow" : "");
    }
    json_array_unref(list);
    close_test_store(path);
}

static void
test_security_store_coalesced_warn_row_becomes_info(void)
{
    gchar *path = open_test_store(SEC_T0);

    PcvSecurityEvent old = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "/usr/sbin/ip");
    g_strlcpy(old.event_id, "sec-known-tool-old", sizeof old.event_id);
    g_strlcpy(old.recommended_action, "manual_runbook", sizeof old.recommended_action);
    old.timestamp = SEC_T0 - 60;
    g_assert_true(pcv_security_store_insert_event(&old, NULL));
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 0);

    PcvSecurityEvent again = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "/usr/sbin/ip");
    submit_fresh(&again, NULL);

    JsonObject *row = pcv_security_store_get_event("sec-known-tool-old");
    g_assert_nonnull(row);
    g_assert_cmpstr(json_object_get_string_member(row, "severity"), ==, "info");
    g_assert_cmpstr(json_object_get_string_member(row, "status"), ==, "open");
    g_assert_cmpint(json_object_get_int_member(row, "occurrence_count"), ==, 2);
    json_object_unref(row);
    JsonArray *list = pcv_security_store_list_events(0, 10, NULL, NULL, NULL);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    json_array_unref(list);

    g_assert_cmpint(pcv_test_audit_call_count(), ==, 0);
    g_assert_cmpint(pcv_test_alert_call_count(), ==, 0);
    close_test_store(path);
}

static void
assert_close_fields(const gchar *event_id, const gchar *closed_by, gint64 closed_at,
                    const gchar *close_note)
{
    JsonObject *obj = pcv_security_store_get_event(event_id);
    g_assert_nonnull(obj);
    g_assert_cmpstr(json_object_get_string_member(obj, "closed_by"), ==, closed_by);
    g_assert_cmpint(json_object_get_int_member(obj, "closed_at"), ==, closed_at);
    g_assert_cmpstr(json_object_get_string_member(obj, "close_note"), ==, close_note);
    json_object_unref(obj);
}

static PcvSecCloseResult
close_event(const gchar *event_id, const gchar *reason)
{
    GError *error = NULL;
    PcvSecCloseResult r = pcv_security_store_close_event(event_id, "alice", reason, &error);
    g_assert_no_error(error);
    return r;
}

static void
test_security_store_close_open_event_sets_fields(void)
{
    gchar *path = open_test_store(SEC_T0);
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_CRIT, "pid:close");
    gchar id[64];
    submit_fresh(&t, id);
    assert_close_fields(id, "", 0, "");

    pcv_security_store_set_now_for_test(SEC_T0 + 42);
    g_assert_cmpint(close_event(id, "  checked\tfalse positive  "), ==, PCV_SEC_CLOSE_OK);
    assert_event_state(id, "resolved", "operator");
    assert_close_fields(id, "alice", SEC_T0 + 42, "checked false positive");

    JsonArray *list = pcv_security_store_list_events(0, 10, NULL, NULL, NULL);
    g_assert_cmpuint(json_array_get_length(list), ==, 1);
    JsonObject *o = json_array_get_object_element(list, 0);
    g_assert_cmpstr(json_object_get_string_member(o, "closed_by"), ==, "alice");
    g_assert_cmpint(json_object_get_int_member(o, "closed_at"), ==, SEC_T0 + 42);
    g_assert_cmpstr(json_object_get_string_member(o, "close_note"), ==, "checked false positive");
    json_array_unref(list);
    close_test_store(path);
}

static void
test_security_store_close_rejects_missing(void)
{
    gchar *path = open_test_store(SEC_T0);
    g_assert_cmpint(close_event("sec-no-such-event", "x"), ==, PCV_SEC_CLOSE_NOT_FOUND);
    close_test_store(path);
}

static void
test_security_store_close_rejects_closed(void)
{

    gchar *path = open_test_store(SEC_T0);
    insert_fixture_event("res-c", SEC_T0 - 10, PCV_SECURITY_STATUS_RESOLVED, PCV_SECURITY_SEVERITY_WARN);
    insert_fixture_event("sup-c", SEC_T0 - 9, PCV_SECURITY_STATUS_SUPPRESSED, PCV_SECURITY_SEVERITY_WARN);
    g_assert_cmpint(close_event("res-c", "x"), ==, PCV_SEC_CLOSE_NOT_OPEN);
    g_assert_cmpint(close_event("sup-c", "x"), ==, PCV_SEC_CLOSE_NOT_OPEN);
    assert_event_state("res-c", "resolved", "");
    assert_event_state("sup-c", "suppressed", "");
    assert_close_fields("res-c", "", 0, "");
    assert_close_fields("sup-c", "", 0, "");

    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:twice");
    gchar id[64];
    submit_fresh(&t, id);
    g_assert_cmpint(close_event(id, "first"), ==, PCV_SEC_CLOSE_OK);
    pcv_security_store_set_now_for_test(SEC_T0 + 5);
    g_assert_cmpint(close_event(id, "second"), ==, PCV_SEC_CLOSE_NOT_OPEN);
    assert_close_fields(id, "alice", SEC_T0, "first");
    close_test_store(path);
}

static void
test_security_store_close_rejects_pending(void)
{

    gchar *path = open_test_store(SEC_T0);
    gchar ap[64], live[64];
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:ap");
    t.status = PCV_SECURITY_STATUS_ACTION_PENDING;
    submit_fresh(&t, ap);
    t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:live");
    submit_fresh(&t, live);
    add_pending_action(live, 3600);

    g_assert_cmpint(close_event(ap, "x"), ==, PCV_SEC_CLOSE_PENDING);
    g_assert_cmpint(close_event(live, "x"), ==, PCV_SEC_CLOSE_PENDING);
    assert_event_state(ap, "action_pending", "");
    assert_event_state(live, "open", "");
    assert_close_fields(live, "", 0, "");
    close_test_store(path);
}

static void
test_security_store_close_allows_expired_pending(void)
{

    gchar *path = open_test_store(SEC_T0);
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:exp");
    gchar id[64];
    submit_fresh(&t, id);
    add_pending_action(id, 10);
    g_assert_cmpint(close_event(id, "x"), ==, PCV_SEC_CLOSE_PENDING);
    pcv_security_store_set_now_for_test(SEC_T0 + 20);
    g_assert_cmpint(close_event(id, "stale action"), ==, PCV_SEC_CLOSE_OK);
    assert_event_state(id, "resolved", "operator");
    assert_close_fields(id, "alice", SEC_T0 + 20, "stale action");
    close_test_store(path);
}

static void
assert_sanitized(const gchar *raw, const gchar *want)
{
    g_autofree gchar *got = pcv_sec_bounds_close_note_sanitize(raw);
    g_assert_cmpstr(got, ==, want);
    if (got) {
        g_assert_true(g_utf8_validate(got, -1, NULL));
    }
}

static void
test_security_store_close_reason_sanitize(void)
{

    static const gunichar replaced[] = {
        0x01, 0x09, 0x0A, 0x0D, 0x1B, 0x1F, 0x7F, 0x80, 0x85, 0x9F,
        0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069,
    };
    for (gsize i = 0; i < G_N_ELEMENTS(replaced); i++) {
        gchar mid[8] = {0};
        g_unichar_to_utf8(replaced[i], mid);
        g_autofree gchar *raw = g_strdup_printf("a%sb", mid);
        assert_sanitized(raw, "a b");
    }

    static const gunichar kept[] = { 0xA0, 0x2029, 0x202F, 0x2065, 0x206A };
    for (gsize i = 0; i < G_N_ELEMENTS(kept); i++) {
        gchar mid[8] = {0};
        g_unichar_to_utf8(kept[i], mid);
        g_autofree gchar *raw = g_strdup_printf("a%sb", mid);
        assert_sanitized(raw, raw);
    }

    assert_sanitized("  ok  ", "ok");
    assert_sanitized("\t\n ok \r", "ok");
    assert_sanitized("   ", NULL);
    assert_sanitized("\x01\x7f", NULL);
    assert_sanitized("", NULL);
    assert_sanitized(NULL, NULL);

    GString *s = g_string_new(NULL);
    for (gint i = 0; i < 200; i++) {
        g_string_append(s, "가");
    }
    assert_sanitized(s->str, s->str);
    g_autofree gchar *padded = g_strdup_printf("  %s  ", s->str);
    assert_sanitized(padded, s->str);
    g_string_append(s, "가");
    assert_sanitized(s->str, NULL);
    g_string_free(s, TRUE);

    assert_sanitized("\xffok", "\xef\xbf\xbdok");
    g_autofree gchar *a199 = g_strnfill(199, 'a');
    g_autofree gchar *broken = g_strdup_printf("%s\xff", a199);
    g_autofree gchar *fixed = g_strdup_printf("%s\xef\xbf\xbd", a199);
    assert_sanitized(broken, fixed);
    g_autofree gchar *broken201 = g_strdup_printf("%s\xff\xff", a199);
    assert_sanitized(broken201, NULL);

    gchar *path = open_test_store(SEC_T0);
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:san");
    gchar id[64];
    submit_fresh(&t, id);
    g_assert_cmpint(close_event(id, " \x01\x1b "), ==, PCV_SEC_CLOSE_INVALID_REASON);
    assert_event_state(id, "open", "");
    g_assert_cmpint(close_event(id, "line1\nline2\xe2\x80\xae"), ==, PCV_SEC_CLOSE_OK);
    assert_close_fields(id, "alice", SEC_T0, "line1 line2");
    close_test_store(path);
}

static void
test_security_store_retention_keeps_recently_closed(void)
{

    gchar *path = open_test_store(SEC_T0);
    pcv_security_store_set_retention_cap_for_test(3);
    const PcvSecuritySeverity warn = PCV_SECURITY_SEVERITY_WARN;
    insert_fixture_event("res-1", 1710000001, PCV_SECURITY_STATUS_RESOLVED, warn);
    insert_fixture_event("res-2", 1710000002, PCV_SECURITY_STATUS_RESOLVED, warn);
    insert_fixture_event("res-3", 1710000003, PCV_SECURITY_STATUS_RESOLVED, warn);
    insert_fixture_event("old-crit", 1700000000, PCV_SECURITY_STATUS_OPEN, PCV_SECURITY_SEVERITY_CRIT);
    g_assert_cmpint(close_event("old-crit", "reviewed"), ==, PCV_SEC_CLOSE_OK);

    insert_fixture_event("res-4", 1710000004, PCV_SECURITY_STATUS_RESOLVED, warn);
    g_assert_cmpint(count_events_by_status("resolved"), ==, 3);
    assert_event_state("old-crit", "resolved", "operator");
    assert_close_fields("old-crit", "alice", SEC_T0, "reviewed");
    g_assert_null(pcv_security_store_get_event("res-1"));
    g_assert_null(pcv_security_store_get_event("res-2"));
    JsonObject *kept = pcv_security_store_get_event("res-3");
    g_assert_nonnull(kept);
    json_object_unref(kept);
    close_test_store(path);
}

static void
test_security_store_same_key_after_close_opens_new_row(void)
{

    gchar *path = open_test_store(SEC_T0);
    PcvSecurityEvent t = make_event(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_SEVERITY_WARN, "pid:again");
    gchar first[64], again[64];
    submit_fresh(&t, first);
    g_assert_cmpint(close_event(first, "done"), ==, PCV_SEC_CLOSE_OK);
    submit_fresh(&t, again);
    g_assert_cmpstr(again, !=, first);
    assert_event_state(again, "open", "");
    assert_close_fields(again, "", 0, "");
    assert_event_state(first, "resolved", "operator");
    g_assert_cmpint(event_int_member(first, "occurrence_count"), ==, 1);
    g_strlcpy(t.recommended_action, "manual_runbook", sizeof t.recommended_action);
    g_autofree gchar *key = pcv_security_event_coalesce_key(&t);
    g_assert_cmpint(pcv_security_store_count_by_coalesce_key(key), ==, 2);
    close_test_store(path);
}

void
test_security_store_register(void)
{
    g_test_add_func("/security/store/insert-list-get", test_security_store_insert_list_get);
    g_test_add_func("/security/store/submit-warn-audits-and-alerts",
                    test_security_store_submit_warn_audits_and_alerts);
    g_test_add_func("/security/store/retention-bounds-terminal-events",
                    test_security_store_retention_bounds_terminal_events);
    g_test_add_func("/security/store/coalesced_repeats_not_audited",
                    test_security_store_coalesced_repeats_not_audited);
    g_test_add_func("/security/store/repeat_summary_hourly",
                    test_security_store_repeat_summary_hourly);
    g_test_add_func("/security/store/repeat_summary_respects_policy",
                    test_security_store_repeat_summary_respects_policy);
    g_test_add_func("/security/store/repeat_summary_survives_reopen",
                    test_security_store_repeat_summary_survives_reopen);
    g_test_add_func("/security/store/repeat_summary_clock_backwards_rebases",
                    test_security_store_repeat_summary_clock_backwards_rebases);
    g_test_add_func("/security/store/new_event_budget_floods_to_summary",
                    test_security_store_new_event_budget_floods_to_summary);
    g_test_add_func("/security/store/flood_reported_on_other_source_submit",
                    test_security_store_flood_reported_on_other_source_submit);
    g_test_add_func("/security/store/repeat_summaries_bounded_by_source_budget",
                    test_security_store_repeat_summaries_bounded_by_source_budget);
    g_test_add_func("/security/store/warn_flood_leaves_crit_share",
                    test_security_store_warn_flood_leaves_crit_share);
    g_test_add_func("/security/store/overflow_folds_oldest_noncrit",
                    test_security_store_overflow_folds_oldest_noncrit);
    g_test_add_func("/security/store/overflow_folds_info_rows",
                    test_security_store_overflow_folds_info_rows);
    g_test_add_func("/security/store/overflow_skips_crit_and_pending",
                    test_security_store_overflow_skips_crit_and_pending);
    g_test_add_func("/security/store/crit_open_over_cap_not_folded_health_warns",
                    test_security_store_crit_open_over_cap_not_folded_health_warns);
    g_test_add_func("/security/store/expire_after_14_days", test_security_store_expire_after_14_days);
    g_test_add_func("/security/store/expire_timer_clock_backwards",
                    test_security_store_expire_timer_clock_backwards);
    g_test_add_func("/security/store/folded_key_reopens_as_new_row",
                    test_security_store_folded_key_reopens_as_new_row);
    g_test_add_func("/security/store/event_json_has_closed_reason",
                    test_security_store_event_json_has_closed_reason);
    g_test_add_func("/security/store/coalesced_warn_row_becomes_info",
                    test_security_store_coalesced_warn_row_becomes_info);
    g_test_add_func("/security/store/close_open_event_sets_fields",
                    test_security_store_close_open_event_sets_fields);
    g_test_add_func("/security/store/close_rejects_missing", test_security_store_close_rejects_missing);
    g_test_add_func("/security/store/close_rejects_closed", test_security_store_close_rejects_closed);
    g_test_add_func("/security/store/close_rejects_pending", test_security_store_close_rejects_pending);
    g_test_add_func("/security/store/close_allows_expired_pending",
                    test_security_store_close_allows_expired_pending);
    g_test_add_func("/security/store/close_reason_sanitize", test_security_store_close_reason_sanitize);
    g_test_add_func("/security/store/retention_keeps_recently_closed",
                    test_security_store_retention_keeps_recently_closed);
    g_test_add_func("/security/store/same_key_after_close_opens_new_row",
                    test_security_store_same_key_after_close_opens_new_row);
}
