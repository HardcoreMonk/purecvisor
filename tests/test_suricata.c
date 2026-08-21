                                                                                             
                                                                                                 
                                                                                
                                                              
                           
                        
  
                                   
  
                                                                           
                                                             
                                                          
                                                        
                                                      
                                              
  
                                                         
                                                                   
                                                  
                                                          
                                   
  
                                                                     
                                                                
                                                               
                                                    
                                                      
  
                                                         
                                                                  
                                                   
                                                       
                                              
                                                                 
                                                      
   
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>                                                 
#include "modules/security/pcv_suricata.h"
#include "modules/security/pcv_suricata_rules.h"
#include "modules/security/security_event.h"
#include "modules/security/security_policy.h"
#include "modules/auth/pcv_rbac.h"                                     

                                                                       
                                                                                
                                                  
                                       
void     pcv_dispatcher_init_policy_map(void);
gboolean pcv_dispatcher_check_rbac(const gchar *method, gint caller_role);

                                                               

static JsonObject *
_mk_eve_alert(gint severity)
{
    JsonObject *eve = json_object_new();
    json_object_set_string_member(eve, "event_type", "alert");
    json_object_set_string_member(eve, "src_ip", "192.0.2.7");
    json_object_set_string_member(eve, "dest_ip", "10.0.0.5");
    json_object_set_string_member(eve, "proto", "TCP");
    json_object_set_int_member(eve, "src_port", 443);
    json_object_set_int_member(eve, "dest_port", 51234);
    json_object_set_string_member(eve, "app_proto", "tls");
    json_object_set_string_member(eve, "in_iface", "enx00e04c680190");

    JsonObject *alert = json_object_new();
    json_object_set_int_member(alert, "signature_id", 2100498);
    json_object_set_int_member(alert, "rev", 7);
    json_object_set_string_member(alert, "signature",
        "GPL ATTACK_RESPONSE id check returned root");
    json_object_set_string_member(alert, "category", "Potentially Bad Traffic");
    json_object_set_int_member(alert, "severity", severity);
    json_object_set_object_member(eve, "alert", alert);

    return eve;
}

static JsonObject *
_mk_eve_flow(void)
{
    JsonObject *eve = json_object_new();
    json_object_set_string_member(eve, "event_type", "flow");
    return eve;
}

                                                          
static JsonObject *
_mk_eve_alert_variant(gint64 sid, const char *src_ip, const char *dest_ip)
{
    JsonObject *eve = json_object_new();
    json_object_set_string_member(eve, "event_type", "alert");
    json_object_set_string_member(eve, "src_ip", src_ip);
    json_object_set_string_member(eve, "dest_ip", dest_ip);

    JsonObject *alert = json_object_new();
    json_object_set_int_member(alert, "signature_id", sid);
    json_object_set_string_member(alert, "signature", "test signature");
    json_object_set_int_member(alert, "severity", 2);
    json_object_set_object_member(eve, "alert", alert);

    return eve;
}

static void
test_state_str_mapping(void)
{
    g_assert_cmpstr(pcv_suricata_state_str(PCV_SURICATA_ACTIVE),   ==, "active");
    g_assert_cmpstr(pcv_suricata_state_str(PCV_SURICATA_INACTIVE), ==, "inactive");
    g_assert_cmpstr(pcv_suricata_state_str(PCV_SURICATA_FAILED),   ==, "failed");
    g_assert_cmpstr(pcv_suricata_state_str(PCV_SURICATA_ABSENT),   ==, "absent");
}

static void
test_state_from_output_active(void)
{
    g_assert_cmpint(pcv_suricata_state_from_output("active\n"), ==, PCV_SURICATA_ACTIVE);
    g_assert_cmpint(pcv_suricata_state_from_output("active"),   ==, PCV_SURICATA_ACTIVE);
}

static void
test_state_from_output_failed(void)
{
    g_assert_cmpint(pcv_suricata_state_from_output("failed\n"), ==, PCV_SURICATA_FAILED);
}

static void
test_state_from_output_inactive(void)
{
    g_assert_cmpint(pcv_suricata_state_from_output("inactive\n"), ==, PCV_SURICATA_INACTIVE);
}

static void
test_state_from_output_activating_is_inactive(void)
{
                                                             
                                                           
    g_assert_cmpint(pcv_suricata_state_from_output("activating\n"), ==, PCV_SURICATA_INACTIVE);
}

static void
test_state_from_output_null_is_inactive(void)
{
    g_assert_cmpint(pcv_suricata_state_from_output(NULL), ==, PCV_SURICATA_INACTIVE);
}

static void
test_state_from_output_empty_is_inactive(void)
{
    g_assert_cmpint(pcv_suricata_state_from_output(""), ==, PCV_SURICATA_INACTIVE);
}

static void
test_state_from_output_whitespace_is_inactive(void)
{
    g_assert_cmpint(pcv_suricata_state_from_output("   \n"), ==, PCV_SURICATA_INACTIVE);
}

                                                               

static void
test_eve_severity_crit(void)
{
                                                
    JsonObject *eve = _mk_eve_alert(1);
    PcvSecurityEvent ev; GError *e = NULL;
    g_assert_true(pcv_suricata_eve_to_event(eve, &ev, &e));
    g_assert_cmpint(ev.source, ==, PCV_SECURITY_SOURCE_SURICATA);
    g_assert_cmpint(ev.type, ==, PCV_SECURITY_EVENT_NETWORK_THREAT);
    g_assert_cmpint(ev.severity, ==, PCV_SECURITY_SEVERITY_CRIT);
    json_object_unref(eve);
}

static void
test_eve_severity_warn(void)
{
    JsonObject *eve = _mk_eve_alert(2);
    PcvSecurityEvent ev; GError *e = NULL;
    g_assert_true(pcv_suricata_eve_to_event(eve, &ev, &e));
    g_assert_cmpint(ev.severity, ==, PCV_SECURITY_SEVERITY_WARN);
    json_object_unref(eve);
}

static void
test_eve_severity_info_default(void)
{
                            
    JsonObject *eve = _mk_eve_alert(3);
    PcvSecurityEvent ev; GError *e = NULL;
    g_assert_true(pcv_suricata_eve_to_event(eve, &ev, &e));
    g_assert_cmpint(ev.severity, ==, PCV_SECURITY_SEVERITY_INFO);
    json_object_unref(eve);
}

static void
test_eve_non_alert_rejected(void)
{
    JsonObject *eve = _mk_eve_flow();                              
    PcvSecurityEvent ev; GError *e = NULL;
    g_assert_false(pcv_suricata_eve_to_event(eve, &ev, &e));                   
    g_assert_nonnull(e);
    g_clear_error(&e);
    json_object_unref(eve);
}

                                                                          

static void
test_eve_coalesce_fields(void)
{
    JsonObject *eve = _mk_eve_alert(2);
    PcvSecurityEvent ev; GError *e = NULL;
    g_assert_true(pcv_suricata_eve_to_event(eve, &ev, &e));
    g_assert_cmpint(ev.target_kind, ==, PCV_SECURITY_TARGET_IP);
    g_assert_cmpstr(ev.target, ==, "192.0.2.7->10.0.0.5");
    g_assert_cmpstr(ev.recommended_action, ==, "review sid:2100498");
    g_assert_cmpstr(ev.summary, ==, "Suricata: GPL ATTACK_RESPONSE id check returned root");
    json_object_unref(eve);
}

static void
test_eve_coalesce_key_stable_for_repeated_alert(void)
{
                                                          
                                                    
    JsonObject *eve1 = _mk_eve_alert(2);
    JsonObject *eve2 = _mk_eve_alert(2);
    PcvSecurityEvent ev1, ev2; GError *e = NULL;
    g_assert_true(pcv_suricata_eve_to_event(eve1, &ev1, &e));
    g_assert_true(pcv_suricata_eve_to_event(eve2, &ev2, &e));

    gchar *k1 = pcv_security_event_coalesce_key(&ev1);
    gchar *k2 = pcv_security_event_coalesce_key(&ev2);
    g_assert_cmpstr(k1, ==, k2);

    g_free(k1);
    g_free(k2);
    json_object_unref(eve1);
    json_object_unref(eve2);
}

static void
test_eve_coalesce_key_differs_by_signature(void)
{
                                                        
                                                         
    JsonObject *eve1 = _mk_eve_alert_variant(2100498, "192.0.2.7", "10.0.0.5");
    JsonObject *eve2 = _mk_eve_alert_variant(2034636, "192.0.2.7", "10.0.0.5");
    PcvSecurityEvent ev1, ev2; GError *e = NULL;
    g_assert_true(pcv_suricata_eve_to_event(eve1, &ev1, &e));
    g_assert_true(pcv_suricata_eve_to_event(eve2, &ev2, &e));

    gchar *k1 = pcv_security_event_coalesce_key(&ev1);
    gchar *k2 = pcv_security_event_coalesce_key(&ev2);
    g_assert_cmpstr(k1, !=, k2);

    g_free(k1);
    g_free(k2);
    json_object_unref(eve1);
    json_object_unref(eve2);
}

static void
test_eve_coalesce_key_differs_by_dest(void)
{
                                                            
                                     
    JsonObject *eve1 = _mk_eve_alert_variant(2100498, "192.0.2.7", "10.0.0.5");
    JsonObject *eve2 = _mk_eve_alert_variant(2100498, "192.0.2.7", "10.0.0.9");
    PcvSecurityEvent ev1, ev2; GError *e = NULL;
    g_assert_true(pcv_suricata_eve_to_event(eve1, &ev1, &e));
    g_assert_true(pcv_suricata_eve_to_event(eve2, &ev2, &e));

    gchar *k1 = pcv_security_event_coalesce_key(&ev1);
    gchar *k2 = pcv_security_event_coalesce_key(&ev2);
    g_assert_cmpstr(k1, !=, k2);

    g_free(k1);
    g_free(k2);
    json_object_unref(eve1);
    json_object_unref(eve2);
}

static void
test_eve_missing_fields_default_to_placeholder(void)
{
                                                           
                                                   
    JsonObject *eve = json_object_new();
    json_object_set_string_member(eve, "event_type", "alert");
    json_object_set_string_member(eve, "src_ip", "192.0.2.7");
                                 

    PcvSecurityEvent ev; GError *e = NULL;
    g_assert_true(pcv_suricata_eve_to_event(eve, &ev, &e));
    g_assert_cmpint(ev.severity, ==, PCV_SECURITY_SEVERITY_INFO);
    g_assert_cmpstr(ev.recommended_action, ==, "review sid:?");
    g_assert_cmpstr(ev.target, ==, "192.0.2.7->?");
    g_assert_cmpstr(ev.summary, ==, "Suricata: ?");
    json_object_unref(eve);
}

                                                                         

  
                                                 
                                                                  
                                                   
                                                     
                                           
                
   
static guint g_json_bad_log_count = 0;

static void
_count_json_bad_logs(const gchar *log_domain, GLogLevelFlags log_level,
                     const gchar *message, gpointer user_data)
{
    (void)log_domain; (void)message; (void)user_data;
    if (log_level & (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING)) {
        g_json_bad_log_count++;
    }
}

static void
test_eve_malformed_scalar_fields_no_critical_and_safe_defaults(void)
{
                                                               
                                                      
             
    g_json_bad_log_count = 0;
    guint handler_id = g_log_set_handler("Json",
        G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING | G_LOG_FLAG_RECURSION,
        _count_json_bad_logs, NULL);

    JsonObject *eve = json_object_new();
    json_object_set_string_member(eve, "event_type", "alert");

    JsonArray *bad_src = json_array_new();
    json_array_add_int_element(bad_src, 1);
    json_array_add_int_element(bad_src, 2);
    json_object_set_array_member(eve, "src_ip", bad_src);                    
    json_object_set_string_member(eve, "alert", "not-an-object");             

    PcvSecurityEvent ev; GError *e = NULL;
    gboolean ok = pcv_suricata_eve_to_event(eve, &ev, &e);

    g_log_remove_handler("Json", handler_id);

    g_assert_true(ok);
    g_assert_cmpint(g_json_bad_log_count, ==, 0);                             
    g_assert_cmpstr(ev.target, ==, "?->?");                                             
    g_assert_cmpint(ev.severity, ==, PCV_SECURITY_SEVERITY_INFO);                         
    g_assert_cmpstr(ev.recommended_action, ==, "review sid:?");

    json_object_unref(eve);
}

static void
test_eve_malformed_signature_id_no_critical_and_safe_default(void)
{
                                                           
                                                        
                       
    g_json_bad_log_count = 0;
    guint handler_id = g_log_set_handler("Json",
        G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING | G_LOG_FLAG_RECURSION,
        _count_json_bad_logs, NULL);

    JsonObject *eve = json_object_new();
    json_object_set_string_member(eve, "event_type", "alert");
    json_object_set_string_member(eve, "src_ip", "192.0.2.7");
    json_object_set_string_member(eve, "dest_ip", "10.0.0.5");

    JsonObject *alert = json_object_new();
    JsonObject *bad_sid = json_object_new();
    json_object_set_int_member(bad_sid, "a", 1);
    json_object_set_object_member(alert, "signature_id", bad_sid);                  
    json_object_set_string_member(alert, "signature", "test");
    json_object_set_int_member(alert, "severity", 2);
    json_object_set_object_member(eve, "alert", alert);

    PcvSecurityEvent ev; GError *e = NULL;
    gboolean ok = pcv_suricata_eve_to_event(eve, &ev, &e);

    g_log_remove_handler("Json", handler_id);

    g_assert_true(ok);
    g_assert_cmpint(g_json_bad_log_count, ==, 0);
    g_assert_cmpstr(ev.recommended_action, ==, "review sid:?");
    g_assert_cmpint(ev.severity, ==, PCV_SECURITY_SEVERITY_WARN);                          

    json_object_unref(eve);
}

                                                                    

static void
test_line_feed_accumulates_until_newline(void)
{
    GString *buf = g_string_new(NULL);
    gboolean discarding = FALSE;
    guint64 oversized = 0;

    const char *text = "hello";
    for (gsize i = 0; i < strlen(text); i++) {
        PcvSuricataLineFeedResult r =
            pcv_suricata_eve_line_feed(buf, &discarding, &oversized, text[i]);
        g_assert_cmpint(r, ==, PCV_SURICATA_LINE_FEED_PENDING);
    }
    g_assert_cmpstr(buf->str, ==, "hello");

    PcvSuricataLineFeedResult r = pcv_suricata_eve_line_feed(buf, &discarding, &oversized, '\n');
    g_assert_cmpint(r, ==, PCV_SURICATA_LINE_FEED_READY);
    g_assert_cmpstr(buf->str, ==, "hello");                                  
    g_assert_cmpint(oversized, ==, 0);

    g_string_free(buf, TRUE);
}

static void
test_line_feed_cap_exceeded_discards_then_recovers(void)
{
                                                 
                                                 
    GString *buf = g_string_new(NULL);
    gboolean discarding = FALSE;
    guint64 oversized = 0;
    gboolean saw_cap_exceeded = FALSE;

                                                         
    for (gsize i = 0; i < PCV_SURICATA_EVE_MAX_LINE + 500; i++) {
        PcvSuricataLineFeedResult r =
            pcv_suricata_eve_line_feed(buf, &discarding, &oversized, 'A');
        if (r == PCV_SURICATA_LINE_FEED_CAP_EXCEEDED) {
            g_assert_false(saw_cap_exceeded);                
            saw_cap_exceeded = TRUE;
            g_assert_true(discarding);
            g_assert_cmpuint(buf->len, ==, 0);                     
        } else {
            g_assert_cmpint(r, ==, PCV_SURICATA_LINE_FEED_PENDING);
        }
    }
    g_assert_true(saw_cap_exceeded);
    g_assert_cmpint(oversized, ==, 1);

                                                               
    PcvSuricataLineFeedResult r_end =
        pcv_suricata_eve_line_feed(buf, &discarding, &oversized, '\n');
    g_assert_cmpint(r_end, ==, PCV_SURICATA_LINE_FEED_DISCARDED);
    g_assert_false(discarding);

                                                  
    const char *next_line = "normal line after cap";
    for (gsize i = 0; i < strlen(next_line); i++) {
        PcvSuricataLineFeedResult r =
            pcv_suricata_eve_line_feed(buf, &discarding, &oversized, next_line[i]);
        g_assert_cmpint(r, ==, PCV_SURICATA_LINE_FEED_PENDING);
    }
    PcvSuricataLineFeedResult r_ready =
        pcv_suricata_eve_line_feed(buf, &discarding, &oversized, '\n');
    g_assert_cmpint(r_ready, ==, PCV_SURICATA_LINE_FEED_READY);
    g_assert_cmpstr(buf->str, ==, next_line);              
    g_assert_cmpint(oversized, ==, 1);                              

    g_string_free(buf, TRUE);
}

                                                            

static void
test_rate_allow_burst_capacity_then_exhausted(void)
{
    PcvSuricataRateState st = {0};
    gint64 now = 0;

    for (gint i = 0; i < PCV_SURICATA_RATE_BURST; i++) {
        g_assert_true(pcv_suricata_rate_allow(&st, now));
    }
                                     
    g_assert_false(pcv_suricata_rate_allow(&st, now));
}

static void
test_rate_allow_refills_after_elapsed_time(void)
{
    PcvSuricataRateState st = {0};
    gint64 now = 0;

    for (gint i = 0; i < PCV_SURICATA_RATE_BURST; i++) {
        g_assert_true(pcv_suricata_rate_allow(&st, now));
    }
    g_assert_false(pcv_suricata_rate_allow(&st, now));

                                                       
    now += G_USEC_PER_SEC;
    for (gint i = 0; i < PCV_SURICATA_RATE_PER_SEC; i++) {
        g_assert_true(pcv_suricata_rate_allow(&st, now));
    }
    g_assert_false(pcv_suricata_rate_allow(&st, now));
}

                                                                       
                                                                      

static void
test_policy_network_threat_prefilled_action_passes_through(void)
{
    PcvSecurityEvent ev = {0};
    ev.type = PCV_SECURITY_EVENT_NETWORK_THREAT;
    ev.target_kind = PCV_SECURITY_TARGET_IP;
    g_strlcpy(ev.recommended_action, "review sid:2100498", sizeof ev.recommended_action);

    g_assert_cmpstr(pcv_security_policy_recommend_action(&ev), ==, "review sid:2100498");
}

static void
test_policy_network_threat_empty_action_falls_back_to_manual_runbook(void)
{
    PcvSecurityEvent ev = {0};
    ev.type = PCV_SECURITY_EVENT_NETWORK_THREAT;
    ev.target_kind = PCV_SECURITY_TARGET_IP;
                                                               

    g_assert_cmpstr(pcv_security_policy_recommend_action(&ev), ==, "manual_runbook");
}

static void
test_policy_auth_bruteforce_still_recommends_block_ip(void)
{
                                                         
                                                               
                                                         
    PcvSecurityEvent ev = {0};
    ev.type = PCV_SECURITY_EVENT_AUTH_BRUTEFORCE;
    ev.target_kind = PCV_SECURITY_TARGET_IP;
    g_strlcpy(ev.target, "192.0.2.10", sizeof ev.target);

    g_assert_cmpstr(pcv_security_policy_recommend_action(&ev), ==, "block_ip");
}

                                                              
                                                       
                                                             

static gboolean g_t3_download_ok;
static gint     g_t3_download_call_count;
static const gchar *g_t3_download_content;
static gboolean g_t3_validate_ok;
static gboolean g_t3_reload_ok_seq[2];                                        
static gint     g_t3_reload_call_count;

static void
t3_reset_hooks(void)
{
    g_t3_download_ok = TRUE;
    g_t3_download_call_count = 0;
    g_t3_download_content = "new-rule-content\n";
    g_t3_validate_ok = TRUE;
    g_t3_reload_ok_seq[0] = TRUE;
    g_t3_reload_ok_seq[1] = TRUE;
    g_t3_reload_call_count = 0;
}

static gboolean
t3_mock_download(const gchar *url, const gchar *tmp_path, GError **error)
{
    (void)url;
    g_t3_download_call_count++;
    if (!g_t3_download_ok) {
        g_set_error(error, g_quark_from_static_string("t3-mock"), 1, "mock download failure");
        return FALSE;
    }
    return g_file_set_contents(tmp_path, g_t3_download_content, -1, error);
}

static gboolean
t3_mock_validate(const gchar *rules_path, GError **error)
{
    (void)rules_path;
    if (!g_t3_validate_ok) {
        g_set_error(error, g_quark_from_static_string("t3-mock"), 2, "mock validate failure");
        return FALSE;
    }
    return TRUE;
}

static gboolean
t3_mock_reload(GError **error)
{
    gint idx = g_t3_reload_call_count;
    g_t3_reload_call_count++;
    gboolean ok = (idx < (gint)G_N_ELEMENTS(g_t3_reload_ok_seq)) ? g_t3_reload_ok_seq[idx] : TRUE;
    if (!ok) {
        g_set_error(error, g_quark_from_static_string("t3-mock"), 3, "mock reload failure");
    }
    return ok;
}

                                                  
static void
test_rules_update_validate_failure_keeps_existing_rules(void)
{
    t3_reset_hooks();
    g_t3_validate_ok = FALSE;

    gchar *dir = g_dir_make_tmp("pcv-suricata-rules-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *rules_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *tmp_path = g_build_filename(dir, "suricata.rules.tmp", NULL);
    gchar *bak_path = g_build_filename(dir, "suricata.rules.bak", NULL);
    g_assert_true(g_file_set_contents(rules_path, "old-rules\n", -1, NULL));

    PcvSuricataRulesHooks hooks = { t3_mock_download, t3_mock_validate, t3_mock_reload };
    GError *err = NULL;
    gboolean ok = pcv_suricata_rules_update_at(dir, "https://example.test/emerging.rules",
                                                "alice", &hooks, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);

    gchar *content = NULL;
    g_assert_true(g_file_get_contents(rules_path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "old-rules\n");
    g_free(content);
    g_assert_false(g_file_test(tmp_path, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(bak_path, G_FILE_TEST_EXISTS));

    g_remove(rules_path);
    g_rmdir(dir);
    g_free(rules_path); g_free(tmp_path); g_free(bak_path); g_free(dir);
}

                                                            
static void
test_rules_update_reload_failure_restores_backup(void)
{
    t3_reset_hooks();
    g_t3_reload_ok_seq[0] = FALSE;                     
    g_t3_reload_ok_seq[1] = TRUE;                         

    gchar *dir = g_dir_make_tmp("pcv-suricata-rules-XXXXXX", NULL);
    gchar *rules_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *tmp_path = g_build_filename(dir, "suricata.rules.tmp", NULL);
    gchar *bak_path = g_build_filename(dir, "suricata.rules.bak", NULL);
    g_assert_true(g_file_set_contents(rules_path, "old-rules\n", -1, NULL));

    PcvSuricataRulesHooks hooks = { t3_mock_download, t3_mock_validate, t3_mock_reload };
    GError *err = NULL;
    gboolean ok = pcv_suricata_rules_update_at(dir, "https://example.test/emerging.rules",
                                                "alice", &hooks, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);

    gchar *content = NULL;
    g_assert_true(g_file_get_contents(rules_path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "old-rules\n");                  
    g_free(content);
    g_assert_cmpint(g_t3_reload_call_count, ==, 2);                    
    g_assert_false(g_file_test(tmp_path, G_FILE_TEST_EXISTS));
                                                           
                                                   
                                             
                                                                        
    g_assert_false(g_file_test(bak_path, G_FILE_TEST_EXISTS));

    g_remove(rules_path);
    g_rmdir(dir);
    g_free(rules_path); g_free(tmp_path); g_free(bak_path); g_free(dir);
}

                                           
static void
test_rules_update_success_replaces_rules_and_keeps_backup(void)
{
    t3_reset_hooks();
    g_t3_download_content = "new-rules\n";

    gchar *dir = g_dir_make_tmp("pcv-suricata-rules-XXXXXX", NULL);
    gchar *rules_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *tmp_path = g_build_filename(dir, "suricata.rules.tmp", NULL);
    gchar *bak_path = g_build_filename(dir, "suricata.rules.bak", NULL);
    g_assert_true(g_file_set_contents(rules_path, "old-rules\n", -1, NULL));

    PcvSuricataRulesHooks hooks = { t3_mock_download, t3_mock_validate, t3_mock_reload };
    GError *err = NULL;
    gboolean ok = pcv_suricata_rules_update_at(dir, "http://example.test/emerging.rules",
                                                "alice", &hooks, &err);
    g_assert_true(ok);
    g_assert_no_error(err);

    gchar *content = NULL;
    g_assert_true(g_file_get_contents(rules_path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "new-rules\n");
    g_free(content);
    g_assert_true(g_file_get_contents(bak_path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "old-rules\n");
    g_free(content);
    g_assert_false(g_file_test(tmp_path, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_t3_reload_call_count, ==, 1);

    g_remove(rules_path); g_remove(bak_path);
    g_rmdir(dir);
    g_free(rules_path); g_free(tmp_path); g_free(bak_path); g_free(dir);
}

                                           
static guint32
_t3_mode(const gchar *path)
{
    GStatBuf st;
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    return (guint32)(st.st_mode & 07777);
}

                                             
  
                                                    
                                                                   
                                                     
                                                        
                                           
                                                   
static void
test_rules_update_fixes_mode_0644(void)
{
    t3_reset_hooks();
    g_t3_download_content = "new-rules\n";

    gchar *dir = g_dir_make_tmp("pcv-suricata-rules-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *rules_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *bak_path = g_build_filename(dir, "suricata.rules.bak", NULL);
    g_assert_true(g_file_set_contents(rules_path, "old-rules\n", -1, NULL));
    g_assert_cmpint(g_chmod(rules_path, 0644), ==, 0);                         

    PcvSuricataRulesHooks hooks = { t3_mock_download, t3_mock_validate, t3_mock_reload };
    GError *err = NULL;
    mode_t old_umask = umask(0077);                        
    gboolean ok = pcv_suricata_rules_update_at(dir, "https://example.test/emerging.rules",
                                                "alice", &hooks, &err);
    umask(old_umask);
    g_assert_true(ok);
    g_assert_no_error(err);

                                                                       
    g_assert_cmphex(_t3_mode(rules_path), ==, 0644);
                                                                     
    g_assert_cmphex(_t3_mode(bak_path), ==, 0644);

    g_remove(rules_path); g_remove(bak_path);
    g_rmdir(dir);
    g_free(rules_path); g_free(bak_path); g_free(dir);
}

                                                            
                                                            
                                                
                                                  
static void
test_rules_update_bak_copy_failure_keeps_existing_rules(void)
{
    t3_reset_hooks();

    gchar *dir = g_dir_make_tmp("pcv-suricata-rules-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *rules_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *tmp_path = g_build_filename(dir, "suricata.rules.tmp", NULL);
    gchar *bak_path = g_build_filename(dir, "suricata.rules.bak", NULL);
    g_assert_true(g_file_set_contents(rules_path, "old-rules\n", -1, NULL));
                                                             
                         
    g_assert_cmpint(g_mkdir(bak_path, 0700), ==, 0);

    PcvSuricataRulesHooks hooks = { t3_mock_download, t3_mock_validate, t3_mock_reload };
    GError *err = NULL;
    gboolean ok = pcv_suricata_rules_update_at(dir, "https://example.test/emerging.rules",
                                                "alice", &hooks, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);

    gchar *content = NULL;
    g_assert_true(g_file_get_contents(rules_path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "old-rules\n");                     
    g_free(content);
    g_assert_false(g_file_test(tmp_path, G_FILE_TEST_EXISTS));                

    g_remove(rules_path);
    g_rmdir(bak_path);                     
    g_rmdir(dir);
    g_free(rules_path); g_free(tmp_path); g_free(bak_path); g_free(dir);
}

                                       
static void
test_rules_update_first_install_success_without_backup(void)
{
    t3_reset_hooks();

    gchar *dir = g_dir_make_tmp("pcv-suricata-rules-XXXXXX", NULL);
    gchar *rules_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *tmp_path = g_build_filename(dir, "suricata.rules.tmp", NULL);
    gchar *bak_path = g_build_filename(dir, "suricata.rules.bak", NULL);
                                                         

    PcvSuricataRulesHooks hooks = { t3_mock_download, t3_mock_validate, t3_mock_reload };
    GError *err = NULL;
    gboolean ok = pcv_suricata_rules_update_at(dir, "http://example.test/emerging.rules",
                                                NULL, &hooks, &err);
    g_assert_true(ok);
    g_assert_no_error(err);

    gchar *content = NULL;
    g_assert_true(g_file_get_contents(rules_path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "new-rule-content\n");
    g_free(content);
    g_assert_false(g_file_test(bak_path, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(tmp_path, G_FILE_TEST_EXISTS));

    g_remove(rules_path);
    g_rmdir(dir);
    g_free(rules_path); g_free(tmp_path); g_free(bak_path); g_free(dir);
}

                             
static void
test_rules_update_download_failure_keeps_existing_rules(void)
{
    t3_reset_hooks();
    g_t3_download_ok = FALSE;

    gchar *dir = g_dir_make_tmp("pcv-suricata-rules-XXXXXX", NULL);
    gchar *rules_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *tmp_path = g_build_filename(dir, "suricata.rules.tmp", NULL);
    g_assert_true(g_file_set_contents(rules_path, "old-rules\n", -1, NULL));

    PcvSuricataRulesHooks hooks = { t3_mock_download, t3_mock_validate, t3_mock_reload };
    GError *err = NULL;
    gboolean ok = pcv_suricata_rules_update_at(dir, "http://example.test/emerging.rules",
                                                "alice", &hooks, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);

    gchar *content = NULL;
    g_assert_true(g_file_get_contents(rules_path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "old-rules\n");
    g_free(content);
    g_assert_false(g_file_test(tmp_path, G_FILE_TEST_EXISTS));

    g_remove(rules_path);
    g_rmdir(dir);
    g_free(rules_path); g_free(tmp_path); g_free(dir);
}

                                                                 
static void
test_rules_update_rejects_unsupported_scheme(void)
{
    t3_reset_hooks();

    gchar *dir = g_dir_make_tmp("pcv-suricata-rules-XXXXXX", NULL);

    PcvSuricataRulesHooks hooks = { t3_mock_download, t3_mock_validate, t3_mock_reload };
    GError *err = NULL;
    gboolean ok = pcv_suricata_rules_update_at(dir, "file:///etc/passwd",
                                                "alice", &hooks, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
    g_assert_cmpint(g_t3_download_call_count, ==, 0);

    g_rmdir(dir);
    g_free(dir);
}

                                                                  

static void
test_policy_roundtrip_full_and_single(void)
{
    PcvSuricataPolicy *p = pcv_suricata_policy_new();
    gboolean inspect = TRUE;
    GError *err = NULL;
    g_assert_true(pcv_suricata_policy_apply(p, "acme", &inspect, "strict", "enforce", &err));
    g_assert_no_error(err);

    gchar *dir = g_dir_make_tmp("pcv-suricata-policy-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *path = g_build_filename(dir, "suricata_policy.json", NULL);
    gchar *tmp_path = g_build_filename(dir, "suricata_policy.json.tmp", NULL);

    g_assert_true(pcv_suricata_policy_save_file(p, path, &err));
    g_assert_no_error(err);
                         
    g_assert_false(g_file_test(tmp_path, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
    pcv_suricata_policy_free(p);

                         
    PcvSuricataPolicy *loaded = pcv_suricata_policy_load_file(path);
    JsonObject *full = pcv_suricata_policy_to_json(loaded, NULL);
    g_assert_cmpstr(json_object_get_string_member(full, "auto_isolate"), ==, "enforce");
    JsonObject *tenants = json_object_get_object_member(full, "tenants");
    JsonObject *acme = json_object_get_object_member(tenants, "acme");
    g_assert_true(json_object_get_boolean_member(acme, "inspect"));
    g_assert_cmpstr(json_object_get_string_member(acme, "profile"), ==, "strict");
    json_object_unref(full);

                     
    JsonObject *single = pcv_suricata_policy_to_json(loaded, "acme");
    g_assert_cmpstr(json_object_get_string_member(single, "tenant"), ==, "acme");
    g_assert_true(json_object_get_boolean_member(single, "inspect"));
    g_assert_cmpstr(json_object_get_string_member(single, "profile"), ==, "strict");
    json_object_unref(single);

                                                         
    JsonObject *unknown = pcv_suricata_policy_to_json(loaded, "ghost");
    g_assert_cmpstr(json_object_get_string_member(unknown, "tenant"), ==, "ghost");
    g_assert_true(json_object_get_boolean_member(unknown, "inspect"));
    g_assert_cmpstr(json_object_get_string_member(unknown, "profile"), ==, "default");
    json_object_unref(unknown);

    pcv_suricata_policy_free(loaded);
    g_remove(path);
    g_rmdir(dir);
    g_free(path); g_free(tmp_path); g_free(dir);
}

static void
test_policy_partial_update_preserves_untouched_fields(void)
{
    PcvSuricataPolicy *p = pcv_suricata_policy_new();
    gboolean inspect_true = TRUE;
    GError *err = NULL;
                                                                      
    g_assert_true(pcv_suricata_policy_apply(p, "acme", &inspect_true, "strict", "enforce", &err));

                                                         
    g_assert_true(pcv_suricata_policy_apply(p, NULL, NULL, NULL, "dry_run", &err));
    g_assert_no_error(err);
    JsonObject *j1 = pcv_suricata_policy_to_json(p, NULL);
    g_assert_cmpstr(json_object_get_string_member(j1, "auto_isolate"), ==, "dry_run");
    JsonObject *acme1 = json_object_get_object_member(
        json_object_get_object_member(j1, "tenants"), "acme");
    g_assert_true(json_object_get_boolean_member(acme1, "inspect"));               
    g_assert_cmpstr(json_object_get_string_member(acme1, "profile"), ==, "strict");
    json_object_unref(j1);

                                                   
    g_assert_true(pcv_suricata_policy_apply(p, "acme", NULL, "lax", NULL, &err));
    g_assert_no_error(err);
    JsonObject *single = pcv_suricata_policy_to_json(p, "acme");
    g_assert_true(json_object_get_boolean_member(single, "inspect"));                   
    g_assert_cmpstr(json_object_get_string_member(single, "profile"), ==, "lax");
    json_object_unref(single);

    pcv_suricata_policy_free(p);
}

static void
test_policy_apply_rejects_invalid_and_no_op(void)
{
    PcvSuricataPolicy *p = pcv_suricata_policy_new();
    GError *err = NULL;

                                    
    g_assert_false(pcv_suricata_policy_apply(p, NULL, NULL, NULL, "bogus", &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

                                                 
    gboolean inspect = TRUE;
    g_assert_false(pcv_suricata_policy_apply(p, NULL, &inspect, NULL, NULL, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

                                   
    g_assert_false(pcv_suricata_policy_apply(p, NULL, NULL, NULL, NULL, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);

    pcv_suricata_policy_free(p);
}

static void
test_policy_load_corrupt_falls_back_to_defaults(void)
{
    gchar *dir = g_dir_make_tmp("pcv-suricata-policy-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, "suricata_policy.json", NULL);
    g_assert_true(g_file_set_contents(path, "{ this is not valid json ]]", -1, NULL));

    PcvSuricataPolicy *p = pcv_suricata_policy_load_file(path);
    g_assert_nonnull(p);                   
    JsonObject *full = pcv_suricata_policy_to_json(p, NULL);
    g_assert_cmpstr(json_object_get_string_member(full, "auto_isolate"), ==, "dry_run");          
    JsonObject *tenants = json_object_get_object_member(full, "tenants");
    g_assert_cmpuint(json_object_get_size(tenants), ==, 0);              
    json_object_unref(full);
    pcv_suricata_policy_free(p);

                                                 
    PcvSuricataPolicy *p2 = pcv_suricata_policy_new();
    GError *err = NULL;
    g_assert_true(pcv_suricata_policy_apply(p2, "newco", NULL, NULL, NULL, &err));
    JsonObject *s = pcv_suricata_policy_to_json(p2, "newco");
    g_assert_true(json_object_get_boolean_member(s, "inspect"));
    g_assert_cmpstr(json_object_get_string_member(s, "profile"), ==, "default");
    json_object_unref(s);
    pcv_suricata_policy_free(p2);

    g_remove(path);
    g_rmdir(dir);
    g_free(path); g_free(dir);
}

                                        

static void
test_isolate_decide_dry_run(void)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_assert_cmpint(pcv_suricata_isolate_decide(FALSE, "192.0.2.7", set), ==,
                    PCV_SURICATA_ISOLATE_DRY_RUN);
    g_hash_table_destroy(set);
}

static void
test_isolate_decide_enforce(void)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_assert_cmpint(pcv_suricata_isolate_decide(TRUE, "192.0.2.7", set), ==,
                    PCV_SURICATA_ISOLATE_ENFORCE);
    g_hash_table_destroy(set);
}

static void
test_isolate_decide_already_actioned_skips(void)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add(set, g_strdup("192.0.2.7"));
                                                
    g_assert_cmpint(pcv_suricata_isolate_decide(TRUE, "192.0.2.7", set), ==,
                    PCV_SURICATA_ISOLATE_SKIP_ACTIONED);
    g_hash_table_destroy(set);
}

static void
test_isolate_decide_ipv6_and_placeholder_skip_invalid(void)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
                                                           
    g_assert_cmpint(pcv_suricata_isolate_decide(TRUE, "2001:db8::1", set), ==,
                    PCV_SURICATA_ISOLATE_SKIP_INVALID);
    g_assert_cmpint(pcv_suricata_isolate_decide(TRUE, "?", set), ==,
                    PCV_SURICATA_ISOLATE_SKIP_INVALID);
    g_assert_cmpint(pcv_suricata_isolate_decide(TRUE, "10.0.0.999", set), ==,
                    PCV_SURICATA_ISOLATE_SKIP_INVALID);
    g_hash_table_destroy(set);
}

static void
test_isolate_decide_cap_exceeded_skips_new_ip(void)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (gint i = 0; i < PCV_SURICATA_ISOLATE_CAP; i++) {
        g_hash_table_add(set, g_strdup_printf("10.%d.%d.%d",
                                              (i >> 16) & 0xff, (i >> 8) & 0xff, i & 0xff));
    }
                                    
    g_assert_cmpint(pcv_suricata_isolate_decide(TRUE, "192.0.2.250", set), ==,
                    PCV_SURICATA_ISOLATE_SKIP_CAP);
                                                   
    g_assert_cmpint(pcv_suricata_isolate_decide(TRUE, "10.0.0.0", set), ==,
                    PCV_SURICATA_ISOLATE_SKIP_ACTIONED);
    g_hash_table_destroy(set);
}

                                                     

static void
test_target_src_ip_normal(void)
{
    gchar out[64];
    g_assert_true(pcv_suricata_target_src_ip("192.0.2.7->10.0.0.5", out, sizeof out));
    g_assert_cmpstr(out, ==, "192.0.2.7");
}

static void
test_target_src_ip_placeholder(void)
{
    gchar out[64];
    g_assert_true(pcv_suricata_target_src_ip("?->10.0.0.5", out, sizeof out));
    g_assert_cmpstr(out, ==, "?");
}

static void
test_target_src_ip_no_separator(void)
{
    gchar out[64];
    g_assert_false(pcv_suricata_target_src_ip("no-arrow-here", out, sizeof out));
    g_assert_false(pcv_suricata_target_src_ip("", out, sizeof out));
}

                                                

static void
test_rules_count_cache_recompute_on_change_hit_when_unchanged(void)
{
    gchar *dir = g_dir_make_tmp("pcv-suricata-rulecount-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, "suricata.rules", NULL);
    const gchar *content =
        "# comment header\n"
        "alert tcp any any -> any any (msg:\"a\"; sid:1;)\n"
        "\n"
        "   # indented comment\n"
        "alert udp any any -> any any (msg:\"b\"; sid:2;)\n"
        "alert ip any any -> any any (msg:\"c\"; sid:3;)\n";
    g_assert_true(g_file_set_contents(path, content, -1, NULL));

    PcvSuricataRulesCountCache cache = {0};
    gboolean recomputed = FALSE;

    guint64 n = pcv_suricata_rules_count_cached(path, &cache, &recomputed);
    g_assert_cmpuint(n, ==, 3);                            
    g_assert_true(recomputed);

                                 
    n = pcv_suricata_rules_count_cached(path, &cache, &recomputed);
    g_assert_cmpuint(n, ==, 3);
    g_assert_false(recomputed);

                                      
    gchar *content2 = g_strconcat(content,
        "alert tcp any any -> any any (msg:\"d\"; sid:4;)\n", NULL);
    g_assert_true(g_file_set_contents(path, content2, -1, NULL));
    n = pcv_suricata_rules_count_cached(path, &cache, &recomputed);
    g_assert_cmpuint(n, ==, 4);
    g_assert_true(recomputed);
    g_free(content2);

    g_remove(path);
    g_rmdir(dir);
    g_free(path); g_free(dir);
}

static void
test_rules_count_cache_missing_file_is_zero(void)
{
    PcvSuricataRulesCountCache cache = {0};
    gboolean recomputed = FALSE;
    guint64 n = pcv_suricata_rules_count_cached(
        "/nonexistent/pcv-suricata-nope.rules", &cache, &recomputed);
    g_assert_cmpuint(n, ==, 0);
}

                                                                     
                                                            
  
                                                          
                                                           
                                                  
                                                    
                                                                        

                                              
static JsonArray *
_sid_arr(const gint64 *vals, guint n)
{
    JsonArray *a = json_array_new();
    for (guint i = 0; i < n; i++)
        json_array_add_int_element(a, vals[i]);
    return a;
}

                      
static gboolean
_has_sid(GHashTable *h, guint sid)
{
    return h && g_hash_table_contains(h, GUINT_TO_POINTER(sid));
}

                                                                         
static void
test_policy_drop_sids_roundtrip(void)
{
    PcvSuricataPolicy *p = pcv_suricata_policy_new();
    GError *err = NULL;

                                   
    g_assert_cmpuint(g_hash_table_size(pcv_suricata_policy_drop_sids(p)), ==, 0);

    const gint64 two[] = { 2100498, 2100500 };
    JsonArray *a = _sid_arr(two, 2);
    g_assert_true(pcv_suricata_policy_set_drop_sids(p, a, TRUE, &err));
    g_assert_no_error(err);
    json_array_unref(a);
    g_assert_cmpuint(g_hash_table_size(pcv_suricata_policy_drop_sids(p)), ==, 2);

                        
    a = _sid_arr(two, 1);
    g_assert_true(pcv_suricata_policy_set_drop_sids(p, a, TRUE, &err));
    json_array_unref(a);
    g_assert_cmpuint(g_hash_table_size(pcv_suricata_policy_drop_sids(p)), ==, 2);

    gchar *dir = g_dir_make_tmp("pcv-suricata-dropsids-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *path = g_build_filename(dir, "suricata_policy.json", NULL);
    g_assert_true(pcv_suricata_policy_save_file(p, path, &err));
    g_assert_no_error(err);
    pcv_suricata_policy_free(p);

                                    
    PcvSuricataPolicy *loaded = pcv_suricata_policy_load_file(path);
    GHashTable *set = pcv_suricata_policy_drop_sids(loaded);
    g_assert_cmpuint(g_hash_table_size(set), ==, 2);
    g_assert_true(_has_sid(set, 2100498));
    g_assert_true(_has_sid(set, 2100500));

                          
    JsonObject *full = pcv_suricata_policy_to_json(loaded, NULL);
    JsonArray *ser = json_object_get_array_member(full, "drop_sids");
    g_assert_cmpuint(json_array_get_length(ser), ==, 2);
    g_assert_cmpint(json_array_get_int_element(ser, 0), ==, 2100498);
    g_assert_cmpint(json_array_get_int_element(ser, 1), ==, 2100500);
    json_object_unref(full);

                                             
    const gint64 rm[] = { 2100498, 999999 };
    a = _sid_arr(rm, 2);
    g_assert_true(pcv_suricata_policy_set_drop_sids(loaded, a, FALSE, &err));
    g_assert_no_error(err);
    json_array_unref(a);
    set = pcv_suricata_policy_drop_sids(loaded);
    g_assert_cmpuint(g_hash_table_size(set), ==, 1);
    g_assert_true(_has_sid(set, 2100500));
    pcv_suricata_policy_free(loaded);

                                                        
    gchar *legacy = g_build_filename(dir, "legacy.json", NULL);
    g_assert_true(g_file_set_contents(legacy, "{\"auto_isolate\":\"enforce\"}", -1, NULL));
    PcvSuricataPolicy *old = pcv_suricata_policy_load_file(legacy);
    g_assert_nonnull(pcv_suricata_policy_drop_sids(old));
    g_assert_cmpuint(g_hash_table_size(pcv_suricata_policy_drop_sids(old)), ==, 0);
    g_assert_true(old->enforce);                      
    pcv_suricata_policy_free(old);

    g_remove(legacy);
    g_remove(path);
    g_rmdir(dir);
    g_free(legacy); g_free(path); g_free(dir);
}

                                                            
static void
test_policy_drop_sids_invalid(void)
{
    PcvSuricataPolicy *p = pcv_suricata_policy_new();
    GError *err = NULL;

                                     
    const gint64 base[] = { 4242 };
    JsonArray *a = _sid_arr(base, 1);
    g_assert_true(pcv_suricata_policy_set_drop_sids(p, a, TRUE, &err));
    json_array_unref(a);

                                                       
    a = json_array_new();
    json_array_add_int_element(a, 7777);
    json_array_add_string_element(a, "2100498");
    g_assert_false(pcv_suricata_policy_set_drop_sids(p, a, TRUE, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    json_array_unref(a);
    g_assert_cmpuint(g_hash_table_size(pcv_suricata_policy_drop_sids(p)), ==, 1);
    g_assert_false(_has_sid(p->drop_sids, 7777));

            
    const gint64 neg[] = { -1 };
    a = _sid_arr(neg, 1);
    g_assert_false(pcv_suricata_policy_set_drop_sids(p, a, TRUE, &err));
    g_assert_nonnull(err); g_clear_error(&err);
    json_array_unref(a);

                                     
    const gint64 zero[] = { 0 };
    a = _sid_arr(zero, 1);
    g_assert_false(pcv_suricata_policy_set_drop_sids(p, a, TRUE, &err));
    g_assert_nonnull(err); g_clear_error(&err);
    json_array_unref(a);

                     
    const gint64 huge[] = { (gint64)G_MAXUINT + 1 };
    a = _sid_arr(huge, 1);
    g_assert_false(pcv_suricata_policy_set_drop_sids(p, a, TRUE, &err));
    g_assert_nonnull(err); g_clear_error(&err);
    json_array_unref(a);

                                          
    a = json_array_new();
    g_assert_false(pcv_suricata_policy_set_drop_sids(p, a, TRUE, &err));
    g_assert_nonnull(err); g_clear_error(&err);
    json_array_unref(a);

                 
    g_assert_false(pcv_suricata_policy_set_drop_sids(p, NULL, TRUE, &err));
    g_assert_nonnull(err); g_clear_error(&err);

                                         
    g_assert_cmpuint(g_hash_table_size(pcv_suricata_policy_drop_sids(p)), ==, 1);
    g_assert_true(_has_sid(p->drop_sids, 4242));
    pcv_suricata_policy_free(p);
}

                                                                    
  
                                                  
                                                       
                                                         
                                                       
                                                             
static void
test_rbac_ips_drop_levels(void)
{
                                                
    pcv_dispatcher_init_policy_map();

    static const gchar *METHODS[] = {
        "suricata.ips.drop.list",
        "suricata.ips.drop.add",
        "suricata.ips.drop.remove",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(METHODS); i++) {
        g_assert_false(pcv_dispatcher_check_rbac(METHODS[i], 0));               
        g_assert_false(pcv_dispatcher_check_rbac(METHODS[i], 1));                 
        g_assert_true(pcv_dispatcher_check_rbac(METHODS[i], 2));               
    }
}

                                                              
  
                                                         
                                                       
                    
                                                        
                                                                    
                                                        
                                                                    
                                                                 
                                                               
                                                                 
                                                          
static void
test_rbac_ips_drop_rest_gate_agreement(void)
{
    gchar *dir = g_dir_make_tmp("pcv_suricata_rbac_XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *db = g_build_filename(dir, "rbac.db", NULL);
    pcv_rbac_init(db);

    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("ips_viewer", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_rbac_user_create("ips_oper", "pw", PCV_ROLE_OPERATOR, NULL, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_rbac_user_create("ips_admin", "pw", PCV_ROLE_ADMIN, NULL, &err));
    g_assert_no_error(err);

    pcv_dispatcher_init_policy_map();

                                                                 
                             
    g_assert_false(pcv_rbac_check_permission("ips_viewer", "suricata.ips.drop.add"));
    g_assert_false(pcv_rbac_check_permission("ips_oper",   "suricata.ips.drop.add"));
    g_assert_true (pcv_rbac_check_permission("ips_admin",  "suricata.ips.drop.add"));
    g_assert_false(pcv_rbac_check_permission("ips_viewer", "suricata.ips.drop.remove"));
    g_assert_true (pcv_rbac_check_permission("ips_admin",  "suricata.ips.drop.remove"));

                                                                  
                                                   
    g_assert_true(pcv_rbac_check_permission("ips_viewer", "suricata.ips.drop.list"));
    g_assert_false(pcv_dispatcher_check_rbac("suricata.ips.drop.list", PCV_ROLE_VIEWER));

                                                        
    g_assert_true(pcv_rbac_check_permission("ips_admin", "suricata.ips.drop.list"));
    g_assert_true(pcv_dispatcher_check_rbac("suricata.ips.drop.list",   PCV_ROLE_ADMIN));
    g_assert_true(pcv_dispatcher_check_rbac("suricata.ips.drop.add",    PCV_ROLE_ADMIN));
    g_assert_true(pcv_dispatcher_check_rbac("suricata.ips.drop.remove", PCV_ROLE_ADMIN));

    pcv_rbac_shutdown();

    gchar *wal = g_strconcat(db, "-wal", NULL);
    gchar *shm = g_strconcat(db, "-shm", NULL);
    g_remove(wal); g_remove(shm); g_remove(db); g_rmdir(dir);
    g_free(wal); g_free(shm); g_free(db); g_free(dir);
}

void
test_suricata_register(void)
{
    g_test_add_func("/suricata/state_str", test_state_str_mapping);
    g_test_add_func("/suricata/state_from_output/active", test_state_from_output_active);
    g_test_add_func("/suricata/state_from_output/failed", test_state_from_output_failed);
    g_test_add_func("/suricata/state_from_output/inactive", test_state_from_output_inactive);
    g_test_add_func("/suricata/state_from_output/activating", test_state_from_output_activating_is_inactive);
    g_test_add_func("/suricata/state_from_output/null", test_state_from_output_null_is_inactive);
    g_test_add_func("/suricata/state_from_output/empty", test_state_from_output_empty_is_inactive);
    g_test_add_func("/suricata/state_from_output/whitespace", test_state_from_output_whitespace_is_inactive);

    g_test_add_func("/suricata/eve/severity_crit", test_eve_severity_crit);
    g_test_add_func("/suricata/eve/severity_warn", test_eve_severity_warn);
    g_test_add_func("/suricata/eve/severity_info_default", test_eve_severity_info_default);
    g_test_add_func("/suricata/eve/non_alert_rejected", test_eve_non_alert_rejected);
    g_test_add_func("/suricata/eve/coalesce_fields", test_eve_coalesce_fields);
    g_test_add_func("/suricata/eve/coalesce_key_stable", test_eve_coalesce_key_stable_for_repeated_alert);
    g_test_add_func("/suricata/eve/coalesce_key_differs_by_signature", test_eve_coalesce_key_differs_by_signature);
    g_test_add_func("/suricata/eve/coalesce_key_differs_by_dest", test_eve_coalesce_key_differs_by_dest);
    g_test_add_func("/suricata/eve/missing_fields_default", test_eve_missing_fields_default_to_placeholder);
    g_test_add_func("/suricata/eve/malformed_scalar_fields_safe",
                     test_eve_malformed_scalar_fields_no_critical_and_safe_defaults);
    g_test_add_func("/suricata/eve/malformed_signature_id_safe",
                     test_eve_malformed_signature_id_no_critical_and_safe_default);
    g_test_add_func("/suricata/rate/burst_then_exhausted", test_rate_allow_burst_capacity_then_exhausted);
    g_test_add_func("/suricata/rate/refills_after_time", test_rate_allow_refills_after_elapsed_time);

    g_test_add_func("/suricata/line_feed/accumulates_until_newline", test_line_feed_accumulates_until_newline);
    g_test_add_func("/suricata/line_feed/cap_exceeded_discards_then_recovers",
                     test_line_feed_cap_exceeded_discards_then_recovers);

    g_test_add_func("/suricata/policy/network_threat_passthrough",
                     test_policy_network_threat_prefilled_action_passes_through);
    g_test_add_func("/suricata/policy/network_threat_empty_falls_back",
                     test_policy_network_threat_empty_action_falls_back_to_manual_runbook);
    g_test_add_func("/suricata/policy/auth_bruteforce_regression",
                     test_policy_auth_bruteforce_still_recommends_block_ip);

    g_test_add_func("/suricata/rules_update/validate_failure_keeps_existing",
                     test_rules_update_validate_failure_keeps_existing_rules);
    g_test_add_func("/suricata/rules_update/reload_failure_restores_backup",
                     test_rules_update_reload_failure_restores_backup);
    g_test_add_func("/suricata/rules_update/bak_copy_failure_keeps_existing",
                     test_rules_update_bak_copy_failure_keeps_existing_rules);
    g_test_add_func("/suricata/rules_update/success_replaces_and_keeps_backup",
                     test_rules_update_success_replaces_rules_and_keeps_backup);
    g_test_add_func("/suricata/rules_update/fixes_mode_0644",
                     test_rules_update_fixes_mode_0644);
    g_test_add_func("/suricata/rules_update/first_install_without_backup",
                     test_rules_update_first_install_success_without_backup);
    g_test_add_func("/suricata/rules_update/download_failure_keeps_existing",
                     test_rules_update_download_failure_keeps_existing_rules);
    g_test_add_func("/suricata/rules_update/rejects_unsupported_scheme",
                     test_rules_update_rejects_unsupported_scheme);

                                   
    g_test_add_func("/suricata/policy/roundtrip_full_and_single",
                     test_policy_roundtrip_full_and_single);
    g_test_add_func("/suricata/policy/partial_update_preserves",
                     test_policy_partial_update_preserves_untouched_fields);
    g_test_add_func("/suricata/policy/apply_rejects_invalid",
                     test_policy_apply_rejects_invalid_and_no_op);
    g_test_add_func("/suricata/policy/load_corrupt_defaults",
                     test_policy_load_corrupt_falls_back_to_defaults);

                                   
    g_test_add_func("/suricata/isolate/decide_dry_run", test_isolate_decide_dry_run);
    g_test_add_func("/suricata/isolate/decide_enforce", test_isolate_decide_enforce);
    g_test_add_func("/suricata/isolate/decide_already_actioned",
                     test_isolate_decide_already_actioned_skips);
    g_test_add_func("/suricata/isolate/decide_ipv6_placeholder_invalid",
                     test_isolate_decide_ipv6_and_placeholder_skip_invalid);
    g_test_add_func("/suricata/isolate/decide_cap_exceeded",
                     test_isolate_decide_cap_exceeded_skips_new_ip);
    g_test_add_func("/suricata/isolate/target_src_normal", test_target_src_ip_normal);
    g_test_add_func("/suricata/isolate/target_src_placeholder", test_target_src_ip_placeholder);
    g_test_add_func("/suricata/isolate/target_src_no_separator", test_target_src_ip_no_separator);

                                
    g_test_add_func("/suricata/rules_count/recompute_and_cache_hit",
                     test_rules_count_cache_recompute_on_change_hit_when_unchanged);
    g_test_add_func("/suricata/rules_count/missing_file_zero",
                     test_rules_count_cache_missing_file_is_zero);

                                                             
    g_test_add_func("/suricata/policy_drop_sids_roundtrip",
                     test_policy_drop_sids_roundtrip);
    g_test_add_func("/suricata/policy_drop_sids_invalid",
                     test_policy_drop_sids_invalid);
    g_test_add_func("/suricata/rbac_ips_drop_levels",
                     test_rbac_ips_drop_levels);
    g_test_add_func("/suricata/rbac_ips_drop_rest_gate_agreement",
                     test_rbac_ips_drop_rest_gate_agreement);
}
