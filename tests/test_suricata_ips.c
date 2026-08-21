                                                                                     
                                                                                                    
                                                                      
                                                                   
                                                
                            
  
                                                           
  
                                                                          
                                                     
                                                                             
                                            
  
                                                                  
                                             
                                                                                 
                                                        
                                                                  
                   
                                                              
                                                    
                                                             
                                                
                 
                                      
                                                            
  
                                                              
                                          
   
#include <glib.h>
#include <string.h>
#include <json-glib/json-glib.h>
#include "modules/security/pcv_suricata.h"
#include "modules/security/pcv_suricata_ips.h"
#include "modules/daemons/alert_engine.h"                            

static void test_queue_rule_argv_fail_open(void) {
    gchar **a = pcv_suricata_ips_queue_rule_argv(5, TRUE);
    g_assert_nonnull(a);
    g_assert_cmpint(g_strv_length(a), ==, 10);
    g_assert_cmpstr(a[0], ==, "nft");
    g_assert_cmpstr(a[1], ==, "add");
    g_assert_cmpstr(a[2], ==, "rule");
    g_assert_cmpstr(a[3], ==, "inet");
    g_assert_cmpstr(a[4], ==, "purecvisor");
    g_assert_cmpstr(a[5], ==, "ips");
    g_assert_cmpstr(a[6], ==, "queue");
    g_assert_cmpstr(a[7], ==, "num");
    g_assert_cmpstr(a[8], ==, "5");
    g_assert_cmpstr(a[9], ==, "bypass");
    g_strfreev(a);
}

static void test_queue_rule_argv_fail_closed_has_no_bypass(void) {
    gchar **a = pcv_suricata_ips_queue_rule_argv(0, FALSE);
    g_assert_nonnull(a);
    g_assert_cmpint(g_strv_length(a), ==, 9);                     
    g_assert_cmpstr(a[8], ==, "0");
    g_assert_null(a[9]);
    g_strfreev(a);
}

static void test_ips_status_cache_store_and_read(void) {
    PcvSuricataState s;
    pcv_suricata_ips_probe_cache_store(PCV_SURICATA_ACTIVE);
    pcv_suricata_ips_engine_status_cached(&s, NULL);
    g_assert_cmpint(s, ==, PCV_SURICATA_ACTIVE);

    pcv_suricata_ips_probe_cache_store(PCV_SURICATA_FAILED);
    pcv_suricata_ips_engine_status_cached(&s, NULL);
    g_assert_cmpint(s, ==, PCV_SURICATA_FAILED);
}

                                                                     
                                
  
                                                         
                                                 
                           
                                                                        

typedef struct {
    GMutex     mu;                                                      
    GPtrArray *events;                                            
    guint      q_insert;                                                
    guint      q_insert_bypass;                          
    guint      flush;                                      
    guint      restart;                                      
    guint      ready_calls;                             
    gboolean   fail_restart;                                              
    gboolean   fail_start;                                                               
    gboolean   fail_q_insert;                                  
    gboolean   probe_seen;                                                       
    gboolean   enabled_at_probe;                                                
    PcvSuricataState probe_state;                        
} IpsSpy;

static IpsSpy   G_spy = {0};                                             
static GThread *G_disable_thr = NULL;

                                                           
static gboolean _argv_has(const gchar * const *argv, const gchar *tok) {
    for (guint i = 0; argv[i] != NULL; i++)
        if (g_strcmp0(argv[i], tok) == 0) return TRUE;
    return FALSE;
}

                                                        
                                                                
static void _spy_push(const gchar *tok) {
    if (!G_spy.events) G_spy.events = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(G_spy.events, g_strdup(tok));
}

                                                  
                                                  
                    
static gboolean _spy_exec(const gchar * const *argv, GError **error) {
    gboolean ok = TRUE;
    g_mutex_lock(&G_spy.mu);
    if (_argv_has(argv, "is-active")) {
        _spy_push("is_active");
    } else if (_argv_has(argv, "restart")) {
        G_spy.restart++;
        _spy_push("restart");
        ok = !G_spy.fail_restart;
    } else if (_argv_has(argv, "start")) {
        _spy_push("start");
        ok = !G_spy.fail_start;
    } else if (_argv_has(argv, "stop")) {
        _spy_push("stop");
    } else if (_argv_has(argv, "queue") && _argv_has(argv, "num")) {
        G_spy.q_insert++;
        if (_argv_has(argv, "bypass")) G_spy.q_insert_bypass++;
        _spy_push("q_insert");                                                       
        if (_argv_has(argv, "bypass")) _spy_push("q_insert_bypass");
        ok = !G_spy.fail_q_insert;
    } else if (_argv_has(argv, "add") && _argv_has(argv, "table")) {
        _spy_push("ensure_table");                                                 
    } else if (_argv_has(argv, "add") && _argv_has(argv, "chain")) {
        _spy_push("ensure_chain");                                                       
    } else if (_argv_has(argv, "flush")) {
        G_spy.flush++;
        _spy_push("flush");
    }
    g_mutex_unlock(&G_spy.mu);
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "spy: 강제 실패 (%s)", argv[0] ? argv[0] : "?");
    return ok;
}

                                          
static PcvSuricataState _spy_probe(void) {
    g_mutex_lock(&G_spy.mu);
    PcvSuricataState s = G_spy.probe_state;
    g_mutex_unlock(&G_spy.mu);
    return s;
}

                              
static gboolean _spy_ready_true(GError **error) {
    (void)error;
    g_mutex_lock(&G_spy.mu); G_spy.ready_calls++; g_mutex_unlock(&G_spy.mu);
    return TRUE;
}

                                               
static gboolean _spy_ready_false(GError **error) {
    g_mutex_lock(&G_spy.mu); G_spy.ready_calls++; g_mutex_unlock(&G_spy.mu);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "spy: readiness 미도달");
    return FALSE;
}

                                  
static void _spy_reset(void) {
    g_mutex_lock(&G_spy.mu);
    G_spy.q_insert = G_spy.q_insert_bypass = 0;
    G_spy.flush = G_spy.restart = G_spy.ready_calls = 0;
    G_spy.fail_restart  = FALSE;
    G_spy.fail_start    = FALSE;
    G_spy.fail_q_insert = FALSE;
    G_spy.probe_seen       = FALSE;
    G_spy.enabled_at_probe = FALSE;
    G_spy.probe_state   = PCV_SURICATA_ACTIVE;
    if (G_spy.events) g_ptr_array_set_size(G_spy.events, 0);
    g_mutex_unlock(&G_spy.mu);
}

                                                
static void _spy_scenario(PcvSuricataState probe_state, gboolean fail_restart) {
    g_mutex_lock(&G_spy.mu);
    G_spy.probe_state  = probe_state;
    G_spy.fail_restart = fail_restart;
    g_mutex_unlock(&G_spy.mu);
}

                                       
static void _spy_set_fail_start(gboolean v) {
    g_mutex_lock(&G_spy.mu);
    G_spy.fail_start = v;
    g_mutex_unlock(&G_spy.mu);
}

                       
static guint _spy_get(const guint *field) {
    g_mutex_lock(&G_spy.mu);
    guint v = *field;
    g_mutex_unlock(&G_spy.mu);
    return v;
}

                                       
static void _spy_clear_events(void) {
    g_mutex_lock(&G_spy.mu);
    if (G_spy.events) g_ptr_array_set_size(G_spy.events, 0);
    g_mutex_unlock(&G_spy.mu);
}

                                   
static gint _spy_idx(const gchar *tok) {
    gint found = -1;
    g_mutex_lock(&G_spy.mu);
    if (G_spy.events) {
        for (guint i = 0; i < G_spy.events->len; i++) {
            if (g_strcmp0(g_ptr_array_index(G_spy.events, i), tok) == 0) {
                found = (gint)i;
                break;
            }
        }
    }
    g_mutex_unlock(&G_spy.mu);
    return found;
}

                                                   
static gchar *_spy_trace(void) {
    g_mutex_lock(&G_spy.mu);
    gchar *s;
    if (G_spy.events && G_spy.events->len) {
        g_ptr_array_add(G_spy.events, NULL);                                
        s = g_strjoinv(",", (gchar **)G_spy.events->pdata);
        g_ptr_array_remove_index(G_spy.events, G_spy.events->len - 1);
    } else {
        s = g_strdup("(empty)");
    }
    g_mutex_unlock(&G_spy.mu);
    return s;
}

                                  
                                                                       
                                               
static void _spy_assert_before(const gchar *a, const gchar *b) {
    gint ia = _spy_idx(a), ib = _spy_idx(b);
    gchar *tr = _spy_trace();
    g_test_message("순서 검사 '%s'(%d) < '%s'(%d) — trace=[%s]", a, ia, b, ib, tr);
    g_free(tr);
    g_assert_cmpint(ia, >=, 0);                      
    g_assert_cmpint(ib, >=, 0);                      
    g_assert_cmpint(ia, <, ib);               
}

                                                      
static void _spy_attach(PcvIpsReadyFn ready) {
    _spy_reset();
    pcv_suricata_ips_set_exec_hook(_spy_exec);
    pcv_suricata_ips_set_probe_hook(_spy_probe);
    pcv_suricata_ips_set_ready_hook(ready);
}

                                                            
                                                     
                                      
static void _spy_detach(void) {
    pcv_suricata_ips_set_ready_hook(NULL);
    _spy_scenario(PCV_SURICATA_ACTIVE, FALSE);                       
    g_mutex_lock(&G_spy.mu);
    G_spy.fail_q_insert = FALSE;
    G_spy.fail_start    = FALSE;
    g_mutex_unlock(&G_spy.mu);
    pcv_suricata_ips_disable(NULL);
    pcv_suricata_ips_set_probe_hook(NULL);
    pcv_suricata_ips_set_exec_hook(NULL);
}

                                       
static guint _degrade_alert_count(void) {
    JsonArray *a = pcv_alert_engine_get_history();
    guint n = 0;
    if (a) {
        guint len = json_array_get_length(a);
        for (guint i = 0; i < len; i++) {
            JsonObject *o = json_array_get_object_element(a, i);
            if (!o || !json_object_has_member(o, "message")) continue;
            const gchar *m = json_object_get_string_member(o, "message");
            if (m && strstr(m, "fail-open 강등")) n++;
        }
        json_array_unref(a);
    }
    return n;
}

                                           
static guint _nonenforce_alert_count(void) {
    JsonArray *a = pcv_alert_engine_get_history();
    guint n = 0;
    if (a) {
        guint len = json_array_get_length(a);
        for (guint i = 0; i < len; i++) {
            JsonObject *o = json_array_get_object_element(a, i);
            if (!o || !json_object_has_member(o, "message")) continue;
            const gchar *m = json_object_get_string_member(o, "message");
            if (m && strstr(m, "IPS 비집행")) n++;
        }
        json_array_unref(a);
    }
    return n;
}

                                             
                                                    
                                           
static guint _mitigation_alert_count(void) {
    JsonArray *a = pcv_alert_engine_get_history();
    guint n = 0;
    if (a) {
        guint len = json_array_get_length(a);
        for (guint i = 0; i < len; i++) {
            JsonObject *o = json_array_get_object_element(a, i);
            if (!o || !json_object_has_member(o, "message")) continue;
            const gchar *m = json_object_get_string_member(o, "message");
            if (m && strstr(m, "IPS 완화 미반영")) n++;
        }
        json_array_unref(a);
    }
    return n;
}

                                                  
                                                              
static void test_ips_failclosed_waits_ready(void) {
    _spy_attach(_spy_ready_false);

    GError *e = NULL;
    g_assert_false(pcv_suricata_ips_enable(3, FALSE, &e));
    g_assert_nonnull(e);                                           
    g_clear_error(&e);

    g_assert_cmpuint(_spy_get(&G_spy.ready_calls), ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert),    ==, 0);                 
    g_assert_false(pcv_suricata_ips_is_enabled());

    _spy_detach();
}

                                                                   
static void test_ips_failclosed_inserts_after_ready(void) {
    _spy_attach(_spy_ready_true);

    g_assert_true(pcv_suricata_ips_enable(3, FALSE, NULL));

    g_assert_cmpuint(_spy_get(&G_spy.ready_calls),     ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert),        ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 0);
    g_assert_true(pcv_suricata_ips_is_enabled());
    g_assert_false(pcv_suricata_ips_is_degraded());

    _spy_detach();
}

                                                            
                                                          
static void test_ips_escape_valve_degrades_after_3(void) {
    pcv_alert_engine_init();                                        
    guint alerts0 = _degrade_alert_count();

    _spy_attach(_spy_ready_true);
    g_assert_true(pcv_suricata_ips_enable(9, FALSE, NULL));

                                         
    _spy_reset();
    _spy_scenario(PCV_SURICATA_FAILED, TRUE);

    pcv_suricata_ips_health_tick();
    g_assert_false(pcv_suricata_ips_is_degraded());              
    pcv_suricata_ips_health_tick();
    g_assert_false(pcv_suricata_ips_is_degraded());              
    pcv_suricata_ips_health_tick();
    g_assert_true(pcv_suricata_ips_is_degraded());                    

    g_assert_cmpuint(_spy_get(&G_spy.restart),         ==, 3);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert),        ==, 1);               
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 1);                       
    g_assert_cmpuint(_degrade_alert_count() - alerts0, ==, 1);

                                                               
    pcv_suricata_ips_health_tick();
    pcv_suricata_ips_health_tick();
    g_assert_true(pcv_suricata_ips_is_degraded());
    g_assert_cmpuint(_spy_get(&G_spy.restart),         ==, 5);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert),        ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 1);
    g_assert_cmpuint(_degrade_alert_count() - alerts0, ==, 1);

    _spy_detach();
    g_assert_false(pcv_suricata_ips_is_degraded());                         
    pcv_alert_engine_shutdown();
}

                                                      
static void test_ips_escape_valve_counter_resets(void) {
    _spy_attach(_spy_ready_true);
    g_assert_true(pcv_suricata_ips_enable(9, FALSE, NULL));

    _spy_reset();
    _spy_scenario(PCV_SURICATA_FAILED, TRUE);
    pcv_suricata_ips_health_tick();                            
    pcv_suricata_ips_health_tick();                            
    g_assert_false(pcv_suricata_ips_is_degraded());

                                                          
    _spy_scenario(PCV_SURICATA_FAILED, FALSE);
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_spy_get(&G_spy.q_insert),        ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 0);

                                                
    _spy_scenario(PCV_SURICATA_FAILED, TRUE);
    pcv_suricata_ips_health_tick();
    pcv_suricata_ips_health_tick();
    g_assert_false(pcv_suricata_ips_is_degraded());
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 0);

    _spy_detach();
}

                                                         
                                               
static void test_ips_failopen_path_unchanged(void) {
    _spy_attach(_spy_ready_true);

    g_assert_true(pcv_suricata_ips_enable(4, TRUE, NULL));

    g_assert_cmpuint(_spy_get(&G_spy.ready_calls),     ==, 0);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert),        ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 1);

    _spy_detach();
}

                                                          
static gpointer _disable_thread_fn(gpointer d) {
    (void)d;
    pcv_suricata_ips_disable(NULL);
    return NULL;
}

                                                                  
                                                              
                                                     
static gboolean _spy_ready_racing_disable(GError **error) {
    (void)error;
    g_mutex_lock(&G_spy.mu); G_spy.ready_calls++; g_mutex_unlock(&G_spy.mu);
    G_disable_thr = g_thread_new("t4-disable-race", _disable_thread_fn, NULL);
                                                             
    for (int i = 0; i < 5000 && pcv_suricata_ips_is_enabled(); i++)
        g_usleep(1000);
    g_assert_false(pcv_suricata_ips_is_enabled());
    return TRUE;
}

                                                    
                                                      
static void test_ips_tick_skips_restore_after_disable(void) {
    _spy_attach(_spy_ready_true);
    g_assert_true(pcv_suricata_ips_enable(5, FALSE, NULL));

    _spy_reset();
    _spy_scenario(PCV_SURICATA_FAILED, FALSE);                            
    pcv_suricata_ips_set_ready_hook(_spy_ready_racing_disable);

    pcv_suricata_ips_health_tick();

    g_assert_nonnull(G_disable_thr);
    g_thread_join(G_disable_thr);
    G_disable_thr = NULL;

    g_assert_cmpuint(_spy_get(&G_spy.restart),  ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert), ==, 0);                        
    g_assert_false(pcv_suricata_ips_is_enabled());

    _spy_detach();
}

                                                        
                                                         
static void test_ips_nfqueue_bound_parse(void) {
                                 
    const gchar *one = "    0  12345     0 2 65531     0     0       42  1\n";
    g_assert_true (pcv_suricata_ips_nfqueue_bound(one, 0));
    g_assert_false(pcv_suricata_ips_nfqueue_bound(one, 1));

                          
    const gchar *two = "    1  111 0 2 0 0 0 1 1\n   12  222 0 2 0 0 0 2 1\n";
    g_assert_true (pcv_suricata_ips_nfqueue_bound(two, 1));
    g_assert_true (pcv_suricata_ips_nfqueue_bound(two, 12));
    g_assert_false(pcv_suricata_ips_nfqueue_bound(two, 2));

                                                   
    g_assert_false(pcv_suricata_ips_nfqueue_bound("",   0));
    g_assert_false(pcv_suricata_ips_nfqueue_bound(NULL, 0));

                                                            
                                                    
                                               
    g_assert_false(pcv_suricata_ips_nfqueue_bound("    7  99 0 2 0 0 0 5 1\n", 99));

                       
    g_assert_false(pcv_suricata_ips_nfqueue_bound("   12  1 0\n", 1));
    g_assert_false(pcv_suricata_ips_nfqueue_bound("    1  1 0\n", 12));

                                   
    g_assert_false(pcv_suricata_ips_nfqueue_bound("   3abc  1\n", 3));

                                         
    g_assert_true (pcv_suricata_ips_nfqueue_bound("    5  1 0 2 0 0 0 0 1", 5));
    g_assert_true (pcv_suricata_ips_nfqueue_bound("queue peer\n    3  1\n", 3));
    g_assert_false(pcv_suricata_ips_nfqueue_bound("queue peer\n", 0));
}

                                                       
                                                                  
                                               
static void test_ips_nfqueue_drops_parse(void) {
    guint64 qd = 12345, ud = 67890;                              

                                   
    const gchar *real = "    0 212663     0 2 65531     0     0        0  1\n";
    g_assert_true(pcv_suricata_ips_nfqueue_drops(real, 0, &qd, &ud));
    g_assert_cmpuint(qd, ==, 0);
    g_assert_cmpuint(ud, ==, 0);

                            
    const gchar *drop = "    3  555 10 2 65531 7 4 99 1\n";
    g_assert_true(pcv_suricata_ips_nfqueue_drops(drop, 3, &qd, &ud));
    g_assert_cmpuint(qd, ==, 7);
    g_assert_cmpuint(ud, ==, 4);

                                            
    const gchar *two = "    1  111 0 2 0 33 44 1 1\n   12  222 0 2 0 55 66 2 1\n";
    g_assert_true(pcv_suricata_ips_nfqueue_drops(two, 1, &qd, &ud));
    g_assert_cmpuint(qd, ==, 33); g_assert_cmpuint(ud, ==, 44);
    g_assert_true(pcv_suricata_ips_nfqueue_drops(two, 12, &qd, &ud));
    g_assert_cmpuint(qd, ==, 55); g_assert_cmpuint(ud, ==, 66);

                                                  
    g_assert_true(pcv_suricata_ips_nfqueue_drops("    5  1 0 2 0 8 9 0 1", 5, &qd, &ud));
    g_assert_cmpuint(qd, ==, 8); g_assert_cmpuint(ud, ==, 9);

                                                   
    const gchar *ext = "    2  1 2 2 3 11 22 5 1 999 888\n";
    g_assert_true(pcv_suricata_ips_nfqueue_drops(ext, 2, &qd, &ud));
    g_assert_cmpuint(qd, ==, 11); g_assert_cmpuint(ud, ==, 22);

                                
    g_assert_true(pcv_suricata_ips_nfqueue_drops("queue peer\n    3  1 0 2 0 2 3 0 1", 3, &qd, &ud));
    g_assert_cmpuint(qd, ==, 2); g_assert_cmpuint(ud, ==, 3);

                                                        
    g_assert_false(pcv_suricata_ips_nfqueue_drops("    7  1 0 2 0 5 6 7 1\n", 1, &qd, &ud));
                   
    g_assert_false(pcv_suricata_ips_nfqueue_drops("   12  1 0 2 0 5 6 7 1\n", 1, &qd, &ud));

                                             
    qd = 111; ud = 222;
    g_assert_false(pcv_suricata_ips_nfqueue_drops(two, 9, &qd, &ud));                
    g_assert_cmpuint(qd, ==, 111); g_assert_cmpuint(ud, ==, 222);
    g_assert_false(pcv_suricata_ips_nfqueue_drops("   4  1 2 3\n", 4, &qd, &ud));           
    g_assert_cmpuint(qd, ==, 111); g_assert_cmpuint(ud, ==, 222);
    g_assert_false(pcv_suricata_ips_nfqueue_drops("", 0, &qd, &ud));              
    g_assert_false(pcv_suricata_ips_nfqueue_drops(NULL, 0, &qd, &ud));            
    g_assert_cmpuint(qd, ==, 111); g_assert_cmpuint(ud, ==, 222);

                                             
    g_assert_true(pcv_suricata_ips_nfqueue_drops(drop, 3, NULL, &ud));
    g_assert_cmpuint(ud, ==, 4);
    g_assert_true(pcv_suricata_ips_nfqueue_drops(drop, 3, &qd, NULL));
    g_assert_cmpuint(qd, ==, 7);
}

                                                                       
                                                         
                                  
static void test_ips_probe_inactive_flushes_rule(void) {
    _spy_attach(_spy_ready_true);
    g_assert_true(pcv_suricata_ips_enable(3, FALSE, NULL));

    _spy_reset();
    _spy_scenario(PCV_SURICATA_INACTIVE, FALSE);
    pcv_suricata_ips_health_tick();

    g_assert_cmpint (_spy_idx("flush"),          >=, 0);                     
    g_assert_cmpuint(_spy_get(&G_spy.q_insert),  ==, 0);              
    g_assert_cmpuint(_spy_get(&G_spy.restart),   ==, 0);                        
    g_assert_false(pcv_suricata_ips_is_degraded());

                                                       
                                                     
    _spy_clear_events();
    _spy_scenario(PCV_SURICATA_ACTIVE, FALSE);
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_spy_get(&G_spy.q_insert),        ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 0);

    _spy_detach();
}

                                                           
static void test_ips_probe_absent_flushes_rule(void) {
    _spy_attach(_spy_ready_true);
    g_assert_true(pcv_suricata_ips_enable(3, FALSE, NULL));

    _spy_reset();
    _spy_scenario(PCV_SURICATA_ABSENT, FALSE);
    pcv_suricata_ips_health_tick();

    g_assert_cmpint (_spy_idx("flush"),         >=, 0);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert), ==, 0);
    g_assert_cmpuint(_spy_get(&G_spy.restart),  ==, 0);                      

    _spy_detach();
}

                                                                
                                                         
                          
static void test_ips_reconcile_restores_missing_rule(void) {
    _spy_attach(_spy_ready_true);
    g_assert_true(pcv_suricata_ips_enable(7, FALSE, NULL));

                                                           
    _spy_reset();
    _spy_scenario(PCV_SURICATA_FAILED, FALSE);
    pcv_suricata_ips_set_ready_hook(_spy_ready_false);
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_spy_get(&G_spy.restart),  ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert), ==, 0);
    g_assert_false(pcv_suricata_ips_is_degraded());

                                                      
    _spy_clear_events();
    _spy_scenario(PCV_SURICATA_ACTIVE, FALSE);
    pcv_suricata_ips_set_ready_hook(_spy_ready_true);
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_spy_get(&G_spy.q_insert),        ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 0);
    g_assert_cmpuint(_spy_get(&G_spy.restart),         ==, 1);                        

                                                       
    _spy_clear_events();
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_spy_get(&G_spy.q_insert), ==, 1);
    g_assert_cmpint (_spy_idx("flush"),         ==, -1);

    _spy_detach();
}

                                               
                                                    
static void test_ips_reconcile_failure_reaches_escape_valve(void) {
    pcv_alert_engine_init();
    guint alerts0 = _degrade_alert_count();

    _spy_attach(_spy_ready_true);
    g_assert_true(pcv_suricata_ips_enable(7, FALSE, NULL));

                                           
    _spy_reset();
    _spy_scenario(PCV_SURICATA_FAILED, FALSE);
    pcv_suricata_ips_set_ready_hook(_spy_ready_false);
    pcv_suricata_ips_health_tick();

                                                             
    _spy_scenario(PCV_SURICATA_ACTIVE, FALSE);
    pcv_suricata_ips_health_tick();                                 
    g_assert_false(pcv_suricata_ips_is_degraded());
    pcv_suricata_ips_health_tick();                                      
    g_assert_true(pcv_suricata_ips_is_degraded());

    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.restart),         ==, 1);                    
    g_assert_cmpuint(_degrade_alert_count() - alerts0,  ==, 1);

    _spy_detach();
    pcv_alert_engine_shutdown();
}

                                                   
                                                                
static void test_ips_order_tick_flush_before_restart(void) {
    _spy_attach(_spy_ready_true);
    g_assert_true(pcv_suricata_ips_enable(3, FALSE, NULL));

    _spy_reset();
    _spy_scenario(PCV_SURICATA_FAILED, FALSE);
    pcv_suricata_ips_health_tick();

    _spy_assert_before("flush", "restart");
    _spy_assert_before("restart", "q_insert");                  

    _spy_detach();
}

                                                              
                                                                
static void test_ips_order_enable_failclosed_flush_before_start(void) {
    _spy_attach(_spy_ready_true);
    _spy_clear_events();

    g_assert_true(pcv_suricata_ips_enable(3, FALSE, NULL));

    _spy_assert_before("flush", "start");
    _spy_assert_before("start", "q_insert");

    _spy_detach();
}

                                                           
                                                 
static void test_ips_order_enable_failopen_start_flush_insert(void) {
    _spy_attach(_spy_ready_true);
    _spy_clear_events();

    g_assert_true(pcv_suricata_ips_enable(4, TRUE, NULL));

    _spy_assert_before("start", "flush");
    _spy_assert_before("flush", "q_insert");
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 1);
    g_assert_cmpuint(_spy_get(&G_spy.ready_calls),     ==, 0);

    _spy_detach();
}

                                                               
                                                       
static void test_ips_degraded_tick_keeps_bypass_rule(void) {
    _spy_attach(_spy_ready_true);
    g_assert_true(pcv_suricata_ips_enable(9, FALSE, NULL));

    _spy_reset();
    _spy_scenario(PCV_SURICATA_FAILED, TRUE);
    pcv_suricata_ips_health_tick();
    pcv_suricata_ips_health_tick();
    pcv_suricata_ips_health_tick();
    g_assert_true(pcv_suricata_ips_is_degraded());

    _spy_clear_events();
    pcv_suricata_ips_health_tick();
    pcv_suricata_ips_health_tick();

    g_assert_cmpint(_spy_idx("flush"),    ==, -1);                           
    g_assert_cmpint(_spy_idx("q_insert"), ==, -1);                    
    g_assert_cmpint(_spy_idx("restart"),  >=,  0);                   

    _spy_detach();
}

                                                    
  
                                                                       
                                                                          
                                                            
                                                        
                                                             
static void test_ips_intent_rule_mismatch_still_flushes(void) {
    _spy_attach(_spy_ready_true);

                                                             
    g_assert_true(pcv_suricata_ips_enable(3, FALSE, NULL));
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 0);

                                                
                                                                
    _spy_reset();
    _spy_set_fail_start(TRUE);
    GError *e = NULL;
    g_assert_false(pcv_suricata_ips_enable(3, TRUE, &e));
    g_assert_nonnull(e);
    g_clear_error(&e);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert), ==, 0);               
    g_assert_true(pcv_suricata_ips_is_enabled());                             

                                                              
                                             
    _spy_set_fail_start(FALSE);
    _spy_clear_events();
    _spy_scenario(PCV_SURICATA_INACTIVE, FALSE);
    pcv_suricata_ips_health_tick();

    g_assert_cmpint (_spy_idx("flush"),         >=, 0);                 
    g_assert_cmpuint(_spy_get(&G_spy.q_insert), ==, 0);               

                                                
    _spy_clear_events();
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_spy_get(&G_spy.q_insert), ==, 0);

    _spy_detach();
}

                                                        
                                                              
static void test_ips_boot_flush_stale(void) {
    _spy_attach(_spy_ready_true);
    _spy_clear_events();

    pcv_suricata_ips_boot_flush_stale();

    g_assert_cmpint (_spy_idx("flush"),         >=, 0);                  
    g_assert_cmpuint(_spy_get(&G_spy.q_insert), ==, 0);                      
    g_assert_cmpint (_spy_idx("start"),         ==, -1);                    
                                                              
                                                                      
    g_assert_cmpint (_spy_idx("ensure_table"),  ==, -1);
    g_assert_cmpint (_spy_idx("ensure_chain"),  ==, -1);
    g_assert_false(pcv_suricata_ips_is_enabled());

    _spy_detach();
}

                                                                 
  
                                                         
                                                        
                                                                         
                                  
  
                        
                                          
                                  
                                                    
static void test_ips_nonenforce_alerts_after_3_ticks(void) {
    pcv_alert_engine_init();                                          
    guint alerts0 = _nonenforce_alert_count();

    _spy_attach(_spy_ready_true);
    g_assert_true(pcv_suricata_ips_enable(9, FALSE, NULL));                          

                                                       
    _spy_reset();
    _spy_scenario(PCV_SURICATA_INACTIVE, FALSE);

    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_nonenforce_alert_count() - alerts0, ==, 0);             
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_nonenforce_alert_count() - alerts0, ==, 0);             
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_nonenforce_alert_count() - alerts0, ==, 1);                    

                                                           
    g_assert_cmpuint(_spy_get(&G_spy.restart), ==, 0);

                                  
    pcv_suricata_ips_health_tick();
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_nonenforce_alert_count() - alerts0, ==, 1);

                                                               
    _spy_scenario(PCV_SURICATA_ACTIVE, FALSE);
    pcv_suricata_ips_health_tick();                                               
    g_assert_cmpuint(_nonenforce_alert_count() - alerts0, ==, 1);
    _spy_scenario(PCV_SURICATA_INACTIVE, FALSE);
    pcv_suricata_ips_health_tick();
    pcv_suricata_ips_health_tick();
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_nonenforce_alert_count() - alerts0, ==, 2);

    _spy_detach();
    pcv_alert_engine_shutdown();
}

                                                               
                                        
  
                                                          
                                 
                                                                                
                                                                
                                                            
                                                         
                                                                       
  
                                                           
                                                           
                
  
                
                                                 
                                                       
                                                        
                       
                                                        
                                                        
static void test_ips_mitigation_unapplied_alerts_after_3_ticks(void) {
    pcv_alert_engine_init();                                          
    guint mit0 = _mitigation_alert_count();
    guint non0 = _nonenforce_alert_count();

    _spy_attach(_spy_ready_true);

                                                         
    g_assert_true(pcv_suricata_ips_enable(9, FALSE, NULL));
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 0);

                                                               
                                              
    GError *e = NULL;
    _spy_set_fail_start(TRUE);
    g_assert_false(pcv_suricata_ips_enable(9, TRUE, &e));
    g_assert_nonnull(e);                                             
    g_clear_error(&e);
    g_assert_true(pcv_suricata_ips_is_enabled());                                   
    _spy_set_fail_start(FALSE);

                                                          
                                                          
                          
    _spy_reset();
    _spy_scenario(PCV_SURICATA_ACTIVE, FALSE);

    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_mitigation_alert_count() - mit0, ==, 0);             
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_mitigation_alert_count() - mit0, ==, 0);             
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_mitigation_alert_count() - mit0, ==, 1);                    

                                                                  
                                     
    g_assert_cmpuint(_nonenforce_alert_count() - non0, ==, 0);
                                                         
                                          
    g_assert_cmpuint(_spy_get(&G_spy.restart),  ==, 0);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert), ==, 0);
    g_assert_cmpuint(_spy_get(&G_spy.flush),    ==, 0);

                                    
    pcv_suricata_ips_health_tick();
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_mitigation_alert_count() - mit0, ==, 1);

                                                             
                                                               
    g_assert_true(pcv_suricata_ips_enable(9, TRUE, NULL));
    g_assert_cmpuint(_spy_get(&G_spy.q_insert_bypass), ==, 1);                      
    pcv_suricata_ips_health_tick();                                           
    g_assert_cmpuint(_mitigation_alert_count() - mit0, ==, 1);                       

                                                         
    g_assert_true(pcv_suricata_ips_enable(9, FALSE, NULL));                             
    _spy_set_fail_start(TRUE);
    g_assert_false(pcv_suricata_ips_enable(9, TRUE, &e));
    g_clear_error(&e);
    _spy_set_fail_start(FALSE);
    _spy_scenario(PCV_SURICATA_ACTIVE, FALSE);

    pcv_suricata_ips_health_tick();
    pcv_suricata_ips_health_tick();
    pcv_suricata_ips_health_tick();
    g_assert_cmpuint(_mitigation_alert_count() - mit0, ==, 2);

    _spy_detach();
    pcv_alert_engine_shutdown();
}

                                                                  
  
                                                                    
                                                                     
                                                                
  
                                                           
                                          
static void test_ips_boot_stops_orphan_unit(void) {
    _spy_attach(_spy_ready_true);

                                                      
    _spy_scenario(PCV_SURICATA_ACTIVE, FALSE);
    _spy_clear_events();
    pcv_suricata_ips_boot_stop_orphan_unit();
    g_assert_cmpint (_spy_idx("stop"),          >=, 0);
    g_assert_cmpuint(_spy_get(&G_spy.q_insert), ==, 0);
    g_assert_cmpint (_spy_idx("start"),         ==, -1);
    g_assert_false(pcv_suricata_ips_is_enabled());                         

                                                       
    _spy_scenario(PCV_SURICATA_INACTIVE, FALSE);
    _spy_clear_events();
    pcv_suricata_ips_boot_stop_orphan_unit();
    g_assert_cmpint(_spy_idx("stop"), ==, -1);

    _spy_detach();
}

                                                                 
  
                                                                  
                                                                    
                                                                           
                                                         
                                       
                                                                  
                                                                   
                                                       
static gboolean _spy_exec_stop_on_probe(const gchar * const *argv, GError **error) {
    gboolean ok = _spy_exec(argv, error);
    if (_argv_has(argv, "is-active")) {
        g_mutex_lock(&G_spy.mu);
        if (!G_spy.probe_seen) {
            G_spy.probe_seen       = TRUE;
            G_spy.enabled_at_probe = pcv_suricata_ips_is_enabled();
        }
        g_mutex_unlock(&G_spy.mu);
        pcv_suricata_ips_shutdown_signal();
    }
    return ok;
}

                                                           
  
                                                                  
                                                                   
                                                                       
                                                                    
                                    
  
                                                        
                                                            
                                                       
                                        
static void test_ips_wait_ready_aborts_on_stop_signal(void) {
    _spy_attach(NULL);                                                               
    pcv_suricata_ips_set_exec_hook(_spy_exec_stop_on_probe);

                                                                 
                                                                     
    gint64 t0 = g_get_monotonic_time();
    GError *e = NULL;
    g_assert_false(pcv_suricata_ips_enable(65001, FALSE, &e));
    gint64 el_ms = (g_get_monotonic_time() - t0) / 1000;
    g_test_message("실험군(대기 중 신호) enable %" G_GINT64_FORMAT "ms — 상한 60000ms", el_ms);

                                                        
                                                       
    g_assert_true(g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED));
    g_clear_error(&e);
    g_assert_cmpint(el_ms, <, 5000);                                                
    g_assert_cmpuint(_spy_get(&G_spy.q_insert), ==, 0);                           
    g_assert_false(pcv_suricata_ips_is_enabled());

                                                        
    g_mutex_lock(&G_spy.mu);
    gboolean probe_seen = G_spy.probe_seen, en_at_probe = G_spy.enabled_at_probe;
    g_mutex_unlock(&G_spy.mu);
    g_assert_true(probe_seen);                                         
    g_assert_false(en_at_probe);                                           

                                                        
                                                            
                                 
                                                        
                                              
    pcv_suricata_ips_set_exec_hook(_spy_exec);                                  
    t0 = g_get_monotonic_time();
    GError *e2 = NULL;
    g_assert_false(pcv_suricata_ips_wait_ready(1, &e2));
    gint64 ctl_ms = (g_get_monotonic_time() - t0) / 1000;
    g_test_message("통제군(신호 없음) 대기 %" G_GINT64_FORMAT "ms — 상한 1000ms 소진 기대", ctl_ms);
    g_assert_true(g_error_matches(e2, G_IO_ERROR, G_IO_ERROR_TIMED_OUT));
    g_clear_error(&e2);
    g_assert_cmpint(ctl_ms, >=, 900);

    _spy_detach();
}

void test_suricata_ips_register(void) {
    g_test_add_func("/suricata_ips/queue_rule_argv/fail_open",
                    test_queue_rule_argv_fail_open);
    g_test_add_func("/suricata_ips/queue_rule_argv/fail_closed_no_bypass",
                    test_queue_rule_argv_fail_closed_has_no_bypass);
    g_test_add_func("/suricata_ips/status_cache/store_and_read",
                    test_ips_status_cache_store_and_read);
                                   
    g_test_add_func("/suricata_ips/failclosed_waits_ready",
                    test_ips_failclosed_waits_ready);
    g_test_add_func("/suricata_ips/failclosed_inserts_after_ready",
                    test_ips_failclosed_inserts_after_ready);
    g_test_add_func("/suricata_ips/escape_valve_degrades_after_3",
                    test_ips_escape_valve_degrades_after_3);
    g_test_add_func("/suricata_ips/escape_valve_counter_resets",
                    test_ips_escape_valve_counter_resets);
    g_test_add_func("/suricata_ips/failopen_path_unchanged",
                    test_ips_failopen_path_unchanged);
    g_test_add_func("/suricata_ips/tick_skips_restore_after_disable",
                    test_ips_tick_skips_restore_after_disable);
                                                                            
                                                          
    g_test_add_func("/suricata_ips/nfqueue_bound/parse",
                    test_ips_nfqueue_bound_parse);
                                            
    g_test_add_func("/suricata_ips/nfqueue_drops/parse",
                    test_ips_nfqueue_drops_parse);
    g_test_add_func("/suricata_ips/probe_inactive_flushes_rule",
                    test_ips_probe_inactive_flushes_rule);
    g_test_add_func("/suricata_ips/probe_absent_flushes_rule",
                    test_ips_probe_absent_flushes_rule);
    g_test_add_func("/suricata_ips/reconcile_restores_missing_rule",
                    test_ips_reconcile_restores_missing_rule);
    g_test_add_func("/suricata_ips/reconcile_failure_reaches_escape_valve",
                    test_ips_reconcile_failure_reaches_escape_valve);
    g_test_add_func("/suricata_ips/order_tick_flush_before_restart",
                    test_ips_order_tick_flush_before_restart);
    g_test_add_func("/suricata_ips/order_enable_failclosed_flush_before_start",
                    test_ips_order_enable_failclosed_flush_before_start);
    g_test_add_func("/suricata_ips/order_enable_failopen_start_flush_insert",
                    test_ips_order_enable_failopen_start_flush_insert);
    g_test_add_func("/suricata_ips/degraded_tick_keeps_bypass_rule",
                    test_ips_degraded_tick_keeps_bypass_rule);
                                                         
    g_test_add_func("/suricata_ips/intent_rule_mismatch_still_flushes",
                    test_ips_intent_rule_mismatch_still_flushes);
    g_test_add_func("/suricata_ips/boot_flush_stale",
                    test_ips_boot_flush_stale);
    g_test_add_func("/suricata_ips/boot_stops_orphan_unit",
                    test_ips_boot_stops_orphan_unit);
    g_test_add_func("/suricata_ips/nonenforce_alerts_after_3_ticks",
                    test_ips_nonenforce_alerts_after_3_ticks);
                                                                      
    g_test_add_func("/suricata_ips/mitigation_unapplied_alerts_after_3_ticks",
                    test_ips_mitigation_unapplied_alerts_after_3_ticks);
                                                                  
    g_test_add_func("/suricata_ips/wait_ready_aborts_on_stop_signal",
                    test_ips_wait_ready_aborts_on_stop_signal);
}
