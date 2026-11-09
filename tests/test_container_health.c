                                                                                                       
                                                                                           
                                   
                                                                                     
                                                  
                                                                              
                                                                         
                                                          
                                                                                   
                                                                         
                                                                                   
                                                                                        
                                                             
                                                               
                                                                     
                                                                   
                                 
  
                                         
   
#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "modules/dispatcher/handler_container_health.h"

                                                                    
void pcv_test_uds_response_reset(void);
const gchar *pcv_test_last_uds_response(void);

#define HC_INTERVAL_SEC 5                                                    

                                                  
static GString  *G_rec;                                                           
static gboolean  G_probe_ok;                                 
static gboolean  G_probe_cancel;                                                
static guint     G_probe_calls;                     
static gboolean  G_lock_ok;                                         
static gboolean  G_allowed;                                
static gint      G_stop_rc;                                     
static gint      G_after_rc;                                   
static gboolean  G_terminating;                           
static gboolean  G_draining;                                                   
static gboolean  G_term_after_stop;                                                              
static GString  *G_probed;                                            
static gint64    G_base;                                                   

                                                           
static void
_rec(const gchar *what)
{
    if (G_rec->len)
        g_string_append_c(G_rec, ',');
    g_string_append(G_rec, what);
}

                                
static gboolean
f_probe(const PcvHealthProbeSpec *spec, GError **error)
{
    g_assert_nonnull(spec);
    G_probe_calls++;
    if (G_probed->len)
        g_string_append_c(G_probed, ',');
    g_string_append(G_probed, spec->name);
    if (G_probe_cancel) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "fake probe cancelled");
        return FALSE;
    }
    if (!G_probe_ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fake probe failed");
    return G_probe_ok;
}

static gpointer f_lock(const gchar *name)              { (void)name; _rec("lock"); return G_lock_ok ? GINT_TO_POINTER(1) : NULL; }
static void     f_unlock(gpointer lock)                { g_assert_nonnull(lock); _rec("unlock"); }
static gboolean f_restart_allowed(const gchar *name)   { (void)name; _rec("restart_allowed"); return G_allowed; }
static gint     f_stop(const gchar *name)              { (void)name; _rec("stop"); if (G_term_after_stop) G_terminating = TRUE; return G_stop_rc; }
static gboolean f_start(const gchar *name)             { (void)name; _rec("start"); return TRUE; }
static gboolean f_renew(gpointer lock)                 { g_assert_nonnull(lock); _rec("renew"); return TRUE; }
static gboolean f_is_terminating(void)                 { return G_terminating; }
static gboolean f_is_draining(void)                    { return G_draining; }

                                     
static gboolean
f_after_stop(const gchar *name, gint stop_rc)
{
    (void)name;
    _rec("after_stop");
    G_after_rc = stop_rc;
    return stop_rc == 0 || stop_rc == 2;
}

static const PcvHealthOps G_fake_ops = {
    .probe           = f_probe,
    .lock            = f_lock,
    .unlock          = f_unlock,
    .restart_allowed = f_restart_allowed,
    .stop            = f_stop,
    .after_stop      = f_after_stop,
    .start           = f_start,
    .renew           = f_renew,
    .is_terminating  = f_is_terminating,
    .is_draining     = f_is_draining,
};

                                                                      
static void
_fx_setup(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    pcv_container_health_reset_for_test();
    pcv_container_health_set_ops_for_test(&G_fake_ops);
    G_rec = g_string_new(NULL);
    G_probed = g_string_new(NULL);
    G_probe_ok = TRUE;
    G_probe_cancel = FALSE;
    G_probe_calls = 0;
    G_lock_ok = TRUE;
    G_allowed = TRUE;
    G_stop_rc = 0;
    G_after_rc = 12345;
    G_terminating = FALSE;
    G_draining = FALSE;
    G_term_after_stop = FALSE;
    G_base = g_get_monotonic_time() + (gint64)3600 * G_USEC_PER_SEC;
}

                                                                
static void
_fx_teardown(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    pcv_container_health_reset_for_test();
    pcv_container_health_set_ops_for_test(NULL);
    g_string_free(G_rec, TRUE);
    G_rec = NULL;
    g_string_free(G_probed, TRUE);
    G_probed = NULL;
}

                              
static gint64
T(gint k)
{
    return G_base + (gint64)k * HC_INTERVAL_SEC * G_USEC_PER_SEC;
}

                                                                                        
static JsonParser *
_parse(JsonObject **result, gint64 *err_code)
{
    JsonParser *p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, pcv_test_last_uds_response(), -1, NULL));
    JsonObject *root = json_node_get_object(json_parser_get_root(p));
    *result = NULL;
    *err_code = 0;
    if (json_object_has_member(root, "error"))
        *err_code = json_object_get_int_member(json_object_get_object_member(root, "error"), "code");
    else if (json_object_has_member(root, "result") &&
             JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "result")))
        *result = json_object_get_object_member(root, "result");
    return p;
}

                                                             
static void
_set(const gchar *name, gint threshold, gboolean auto_restart)
{
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", name);
    json_object_set_string_member(params, "type", "tcp");
    json_object_set_string_member(params, "target", "80");
    json_object_set_int_member(params, "interval_sec", HC_INTERVAL_SEC);
    json_object_set_int_member(params, "failure_threshold", threshold);
    json_object_set_boolean_member(params, "auto_restart", auto_restart);
    pcv_test_uds_response_reset();
    handle_container_health_set(params, "1", NULL, NULL);
    json_object_unref(params);
    JsonObject *res; gint64 code;
    JsonParser *p = _parse(&res, &code);
    g_assert_cmpint(code, ==, 0);
    g_assert_nonnull(res);
    g_assert_true(json_object_get_boolean_member(res, "success"));
    g_object_unref(p);
}

                                                 
static void
_delete(const gchar *name)
{
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", name);
    pcv_test_uds_response_reset();
    handle_container_health_delete(params, "1", NULL, NULL);
    json_object_unref(params);
    JsonObject *res; gint64 code;
    JsonParser *p = _parse(&res, &code);
    g_assert_cmpint(code, ==, 0);
    g_assert_nonnull(res);
    g_object_unref(p);
}

                                                                                        
static gboolean
_get(const gchar *name, gint *failures, gint *restarts, gboolean *healthy, gint64 *code)
{
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", name);
    pcv_test_uds_response_reset();
    handle_container_health_get(params, "1", NULL, NULL);
    json_object_unref(params);
    JsonObject *res;
    JsonParser *p = _parse(&res, code);
    gboolean found = res != NULL;
    if (found) {
        *failures = (gint)json_object_get_int_member(res, "consecutive_failures");
        *restarts = (gint)json_object_get_int_member(res, "restart_count");
        *healthy  = json_object_get_boolean_member(res, "healthy");
    }
    g_object_unref(p);
    return found;
}

                                  
static gint
_failures(const gchar *name)
{
    gint f = -1, r = -1; gboolean h = FALSE; gint64 code = 0;
    g_assert_true(_get(name, &f, &r, &h, &code));
    return f;
}

                                
static gint
_restarts(const gchar *name)
{
    gint f = -1, r = -1; gboolean h = FALSE; gint64 code = 0;
    g_assert_true(_get(name, &f, &r, &h, &code));
    return r;
}

                                               
static void
test_clamp_timeout(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    g_assert_cmpint(pcv_container_health_clamp_timeout(0), ==, 1);
    g_assert_cmpint(pcv_container_health_clamp_timeout(-5), ==, 1);
    g_assert_cmpint(pcv_container_health_clamp_timeout(1), ==, 1);
    g_assert_cmpint(pcv_container_health_clamp_timeout(5), ==, 5);
    g_assert_cmpint(pcv_container_health_clamp_timeout(30), ==, 30);
    g_assert_cmpint(pcv_container_health_clamp_timeout(9999), ==, 30);
}

                                                                                  
static void
test_inflight_skips_redispatch(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    _set("c1", 3, FALSE);
    guint64 gen = pcv_container_health_gen_for_test("c1");
    g_assert_cmpuint(gen, >, 0);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, 1);
    g_assert_true(cont);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(1), FALSE, &cont), ==, 0);                   
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(2), FALSE, &cont), ==, 0);
    g_assert_true(cont);
    g_assert_cmpuint(G_probe_calls, ==, 0);                                                                          
    pcv_container_health_complete_for_test("c1", gen, TRUE, FALSE);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(3), FALSE, &cont), ==, 1);                   
}

                                                               
static void
test_max_inflight_cap(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    const gchar *names[] = { "c0", "c1", "c2", "c3", "c4", "c5" };
    guint64 gens[G_N_ELEMENTS(names)];
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        _set(names[i], 1, TRUE);
        gens[i] = pcv_container_health_gen_for_test(names[i]);
    }
    G_probe_ok = FALSE;
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, PCV_HEALTH_MAX_INFLIGHT);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, 0);              
    for (guint i = 0; i < G_N_ELEMENTS(names); i++)
        g_assert_cmpint(_failures(names[i]), ==, 0);                                                      
    g_assert_cmpstr(G_rec->str, ==, "");
                                                     
    _delete("c0");
    pcv_container_health_complete_for_test("c0", gens[0], FALSE, FALSE);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, 1);
    pcv_container_health_complete_for_test("c1", gens[1], TRUE, FALSE);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, 1);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, 0);                  
    g_assert_cmpint(_failures("c1"), ==, 0);
    g_assert_cmpstr(G_rec->str, ==, "");
}

                                                                           
                                                                  
static void
test_fair_dispatch_under_cap(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    const gchar *names[] = { "c0", "c1", "c2", "c3", "c4" };
    guint64 gens[G_N_ELEMENTS(names)];
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        _set(names[i], 3, FALSE);
        gens[i] = pcv_container_health_gen_for_test(names[i]);
    }
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, PCV_HEALTH_MAX_INFLIGHT);
    pcv_container_health_complete_for_test("c0", gens[0], TRUE, FALSE);                                          
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(1), TRUE, &cont), ==, 1);                            
    g_assert_cmpstr(G_probed->str, ==, "c4");                                                                  
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(1), TRUE, &cont), ==, 1);                    
    g_assert_cmpstr(G_probed->str, ==, "c4,c0");
}

                                                       
static void
test_result_dropped_after_delete(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    _set("c1", 1, TRUE);
    guint64 gen = pcv_container_health_gen_for_test("c1");
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, 1);
    _delete("c1");
    pcv_container_health_complete_for_test("c1", gen, FALSE, FALSE);
    gint f = -1, r = -1; gboolean h = FALSE; gint64 code = 0;
    g_assert_false(_get("c1", &f, &r, &h, &code));
    g_assert_cmpint(code, ==, -32000);
    g_assert_cmpstr(G_rec->str, ==, "");
}

                                                  
static void
test_result_dropped_after_reset(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    _set("c1", 3, FALSE);
    guint64 gen1 = pcv_container_health_gen_for_test("c1");
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, 1);
    _set("c1", 3, FALSE);
    guint64 gen2 = pcv_container_health_gen_for_test("c1");
    g_assert_cmpuint(gen2, >, gen1);
    pcv_container_health_complete_for_test("c1", gen1, FALSE, FALSE);
    gint f = -1, r = -1; gboolean h = FALSE; gint64 code = 0;
    g_assert_true(_get("c1", &f, &r, &h, &code));
    g_assert_cmpint(f, ==, 0);
    g_assert_true(h);
}

                                                          
static void
test_swap_remove_does_not_misapply(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    _set("a", 1, FALSE);
    guint64 gen_a = pcv_container_health_gen_for_test("a");
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, 1);                
    _set("b", 1, FALSE);                                                                           
    _delete("a");                                                                                       
    pcv_container_health_complete_for_test("a", gen_a, FALSE, FALSE);
    gint f = -1, r = -1; gboolean h = FALSE; gint64 code = 0;
    g_assert_true(_get("b", &f, &r, &h, &code));
    g_assert_cmpint(f, ==, 0);
    g_assert_true(h);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(1), FALSE, &cont), ==, 1);                     
}

                                                                
static void
test_restart_order_and_lock(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    G_probe_ok = FALSE;
    _set("c1", 1, TRUE);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), TRUE, &cont), ==, 1);
    g_assert_cmpuint(G_probe_calls, ==, 1);
    g_assert_cmpstr(G_rec->str, ==, "lock,restart_allowed,stop,renew,after_stop,start,unlock");
    g_assert_cmpint(G_after_rc, ==, 0);
    g_assert_cmpint(_restarts("c1"), ==, 1);
    g_assert_cmpint(_failures("c1"), ==, 0);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(1), FALSE, &cont), ==, 1);                     
}

                                                                    
static void
test_restart_denied_resets_failures(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    G_probe_ok = FALSE;
    G_allowed = FALSE;
    _set("c1", 1, TRUE);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), TRUE, &cont), ==, 1);
    g_assert_cmpstr(G_rec->str, ==, "lock,restart_allowed,unlock");
    g_assert_cmpint(_failures("c1"), ==, 0);
    g_assert_cmpint(_restarts("c1"), ==, 0);
}

                                             
static void
test_cancelled_result_discarded(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    _set("c1", 2, TRUE);
    guint64 gen = pcv_container_health_gen_for_test("c1");
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, 1);
    pcv_container_health_complete_for_test("c1", gen, FALSE, FALSE);                              
    g_assert_cmpint(_failures("c1"), ==, 1);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(1), FALSE, &cont), ==, 1);
    pcv_container_health_complete_for_test("c1", gen, FALSE, TRUE);                                      
    g_assert_cmpint(_failures("c1"), ==, 1);
    g_assert_cmpstr(G_rec->str, ==, "");
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(2), FALSE, &cont), ==, 1);               
    pcv_container_health_complete_for_test("c1", gen, TRUE, FALSE);
                                                  
    G_probe_cancel = TRUE;
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(3), TRUE, &cont), ==, 1);
    g_assert_cmpuint(G_probe_calls, ==, 1);
    g_assert_cmpint(_failures("c1"), ==, 0);
    g_assert_cmpstr(G_rec->str, ==, "");
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(4), FALSE, &cont), ==, 1);
}

                                                                    
static void
test_redispatch_after_reset(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    _set("c1", 3, FALSE);
    guint64 gen1 = pcv_container_health_gen_for_test("c1");
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, 1);
    _set("c1", 3, FALSE);
    guint64 gen2 = pcv_container_health_gen_for_test("c1");
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(1), FALSE, &cont), ==, 1);                         
    pcv_container_health_complete_for_test("c1", gen1, FALSE, FALSE);                                   
    g_assert_cmpint(_failures("c1"), ==, 0);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(2), FALSE, &cont), ==, 0);                       
    pcv_container_health_complete_for_test("c1", gen2, FALSE, FALSE);
    g_assert_cmpint(_failures("c1"), ==, 1);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(3), FALSE, &cont), ==, 1);
}

                                                              
static void
test_restart_skipped_when_locked(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    G_probe_ok = FALSE;
    G_lock_ok = FALSE;
    _set("c1", 1, TRUE);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), TRUE, &cont), ==, 1);
    g_assert_cmpstr(G_rec->str, ==, "lock");
    g_assert_cmpint(_failures("c1"), ==, 1);
    g_assert_cmpint(_restarts("c1"), ==, 0);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(1), TRUE, &cont), ==, 1);
    g_assert_cmpstr(G_rec->str, ==, "lock,lock");
    g_assert_cmpint(_failures("c1"), ==, 2);
}

                                                       
static void
test_stop_timeout_no_start(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    G_probe_ok = FALSE;
    G_stop_rc = -1;
    _set("c1", 1, TRUE);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), TRUE, &cont), ==, 1);
    g_assert_cmpstr(G_rec->str, ==, "lock,restart_allowed,stop,renew,after_stop,unlock");
    g_assert_cmpint(G_after_rc, ==, -1);
    g_assert_cmpint(_restarts("c1"), ==, 0);
}

                                                                                
                                              
static void
test_terminating_after_stop_skips_start(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    G_probe_ok = FALSE;
    G_term_after_stop = TRUE;
    _set("c1", 1, TRUE);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), TRUE, &cont), ==, 1);
    g_assert_cmpstr(G_rec->str, ==, "lock,restart_allowed,stop,renew,after_stop,unlock");
    g_assert_cmpint(G_after_rc, ==, 0);
    g_assert_cmpint(_restarts("c1"), ==, 0);
}

                                                           
static void
test_terminating_stops_dispatch(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = TRUE;
    _set("c1", 1, TRUE);
    g_assert_true(pcv_container_health_timer_active_for_test());
    G_terminating = TRUE;
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), TRUE, &cont), ==, 0);
    g_assert_false(cont);
    g_assert_cmpuint(G_probe_calls, ==, 0);
    g_assert_false(pcv_container_health_timer_active_for_test());
}

                                                                                     
                                                                               
                                                                    
static void
test_draining_defers_dispatch(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    _set("c1", 1, TRUE);
    G_draining = TRUE;
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), TRUE, &cont), ==, 0);
    g_assert_true(cont);
    g_assert_cmpuint(G_probe_calls, ==, 0);
    g_assert_true(pcv_container_health_timer_active_for_test());
    G_draining = FALSE;
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), TRUE, &cont), ==, 1);                             
    g_assert_true(cont);
    g_assert_cmpuint(G_probe_calls, ==, 1);
    G_draining = TRUE;                                                                                           
    G_terminating = TRUE;
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(1), TRUE, &cont), ==, 0);
    g_assert_false(cont);
    g_assert_cmpuint(G_probe_calls, ==, 1);
    g_assert_false(pcv_container_health_timer_active_for_test());
}

                                      
static void
test_timer_stops_when_empty(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = TRUE;
    g_assert_false(pcv_container_health_timer_active_for_test());
    _set("c1", 3, FALSE);
    g_assert_true(pcv_container_health_timer_active_for_test());
    _delete("c1");
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), FALSE, &cont), ==, 0);
    g_assert_false(cont);
    g_assert_false(pcv_container_health_timer_active_for_test());
    _set("c1", 3, FALSE);
    g_assert_true(pcv_container_health_timer_active_for_test());
}

                                                                                      
static gint64
_set_raw(JsonNode *name, const gchar *type, const gchar *target, gint threshold)
{
    JsonObject *params = json_object_new();
    if (name)
        json_object_set_member(params, "name", name);
    json_object_set_string_member(params, "type", type);
    json_object_set_string_member(params, "target", target);
    if (threshold != G_MININT)
        json_object_set_int_member(params, "failure_threshold", threshold);
    pcv_test_uds_response_reset();
    handle_container_health_set(params, "1", NULL, NULL);
    json_object_unref(params);
    JsonObject *res; gint64 code;
    JsonParser *p = _parse(&res, &code);
    g_object_unref(p);
    return code;
}

static gint64
_set_name(const gchar *name, const gchar *type, const gchar *target)
{
    return _set_raw(json_node_init_string(json_node_alloc(), name), type, target, G_MININT);
}

                                     
static void
test_set_rejects_invalid_name(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gchar *long65 = g_strnfill(65, 'a');
    g_assert_cmpint(_set_name("bad name", "tcp", "80"), ==, -32602);
    g_assert_cmpint(_set_name("../x", "tcp", "80"), ==, -32602);
    g_assert_cmpint(_set_name(long65, "tcp", "80"), ==, -32602);
    g_free(long65);
    g_assert_cmpint(_set_raw(json_node_init_int(json_node_alloc(), 5), "tcp", "80", G_MININT), ==, -32602);
    g_assert_cmpint(_set_raw(NULL, "tcp", "80", G_MININT), ==, -32602);
    g_assert_cmpint(_set_name("ok_name-1", "tcp", "80"), ==, 0);
}

                             
static void
test_set_validates_target_by_type(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    static const struct { const gchar *type, *target; gboolean ok; } rows[] = {
        {"tcp", "80", TRUE}, {"tcp", "65535", TRUE},
        {"tcp", "0", FALSE}, {"tcp", "65536", FALSE}, {"tcp", "80a", FALSE},
        {"tcp", "127.0.0.1:80", FALSE}, {"tcp", "", FALSE},
        {"http", "http://x/h", TRUE}, {"http", "https://x", TRUE},
        {"http", "-o/tmp/x", FALSE}, {"http", "ftp://x", FALSE},
        {"http", "http://a b", FALSE}, {"http", "http://a\nb", FALSE},
        {"exec", "/bin/check", TRUE},
        {"exec", "-x", FALSE}, {"exec", "", FALSE}, {"exec", "a\tb", FALSE},
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        gint64 code = _set_name("c1", rows[i].type, rows[i].target);
        if (rows[i].ok) {
            g_assert_cmpint(code, ==, 0);
        } else {
            g_assert_cmpint(code, ==, -32602);
            g_assert_nonnull(strstr(pcv_test_last_uds_response(), "Invalid "));
        }
    }
}

                                                                                                 
                          
static void
test_set_rejects_exec_target_with_space(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    static const gchar *bad[] = { "/bin/check arg", " /bin/check", "/bin/check ", "/bin/check  " };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_assert_cmpint(_set_name("c1", "exec", bad[i]), ==, -32602);
        g_assert_nonnull(strstr(pcv_test_last_uds_response(),
            "Invalid exec target: a single program path without spaces (arguments are not supported)"));
    }
    gint f, r; gboolean h; gint64 code = 0;
    g_assert_false(_get("c1", &f, &r, &h, &code));                       
    g_assert_cmpint(_set_name("c1", "exec", "/bin/check"), ==, 0);
    g_assert_true(_get("c1", &f, &r, &h, &code));
}

                                          
static void
test_set_rejects_long_target(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gchar *t256 = g_strnfill(256, 'a');
    gchar *t255 = g_strnfill(255, 'a');
    g_assert_cmpint(_set_name("c1", "exec", t256), ==, -32602);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "target too long (max 255 bytes)"));
    g_assert_cmpint(_set_name("c1", "exec", t255), ==, 0);
    g_free(t256);
    g_free(t255);
}

                                                    
static void
test_set_resets_runtime_state(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gboolean cont = FALSE;
    G_probe_ok = FALSE;
    _set("A", 1, TRUE);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(0), TRUE, &cont), ==, 1);
    g_assert_cmpint(_restarts("A"), ==, 1);
    _delete("A");
    _set("B", 3, FALSE);
    g_assert_cmpint(_restarts("B"), ==, 0);
    _set("A", 1, TRUE);                                 
    g_assert_cmpint(_restarts("A"), ==, 0);
    g_assert_cmpuint(pcv_container_health_tick_for_test(T(1), TRUE, &cont), >=, 1);
    g_assert_cmpint(_restarts("A"), ==, 1);
    _set("A", 3, FALSE);                
    g_assert_cmpint(_restarts("A"), ==, 0);
    g_assert_cmpint(_failures("A"), ==, 0);
}

                                    
static void
test_failure_threshold_floor(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    g_assert_cmpint(_set_raw(json_node_init_string(json_node_alloc(), "z0"), "tcp", "80", 0), ==, 0);
    g_assert_cmpint(_set_raw(json_node_init_string(json_node_alloc(), "z3"), "tcp", "80", -3), ==, 0);
    for (guint i = 0; i < 2; i++) {
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "name", i ? "z3" : "z0");
        pcv_test_uds_response_reset();
        handle_container_health_get(params, "1", NULL, NULL);
        json_object_unref(params);
        JsonObject *res; gint64 code;
        JsonParser *p = _parse(&res, &code);
        g_assert_nonnull(res);
        g_assert_cmpint(json_object_get_int_member(res, "failure_threshold"), ==, 1);
        g_object_unref(p);
    }
}

                             
static void
test_set_keeps_64_char_name(gpointer *fx, gconstpointer data)
{
    (void)fx; (void)data;
    gchar *n64 = g_strnfill(64, 'n');
    g_assert_cmpint(_set_name(n64, "tcp", "80"), ==, 0);
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", n64);
    pcv_test_uds_response_reset();
    handle_container_health_get(params, "1", NULL, NULL);
    json_object_unref(params);
    JsonObject *res; gint64 code;
    JsonParser *p = _parse(&res, &code);
    g_assert_cmpint(code, ==, 0);
    g_assert_nonnull(res);
    g_assert_cmpstr(json_object_get_string_member(res, "name"), ==, n64);
    g_object_unref(p);
    g_free(n64);
}

#define HC_ADD(name, fn) \
    g_test_add("/container_health/" name, gpointer, NULL, _fx_setup, fn, _fx_teardown)

void
test_container_health_register(void)
{
    HC_ADD("clamp_timeout", test_clamp_timeout);
    HC_ADD("inflight_skips_redispatch", test_inflight_skips_redispatch);
    HC_ADD("max_inflight_cap", test_max_inflight_cap);
    HC_ADD("fair_dispatch_under_cap", test_fair_dispatch_under_cap);
    HC_ADD("result_dropped_after_delete", test_result_dropped_after_delete);
    HC_ADD("result_dropped_after_reset", test_result_dropped_after_reset);
    HC_ADD("swap_remove_does_not_misapply", test_swap_remove_does_not_misapply);
    HC_ADD("restart_order_and_lock", test_restart_order_and_lock);
    HC_ADD("restart_denied_resets_failures", test_restart_denied_resets_failures);
    HC_ADD("cancelled_result_discarded", test_cancelled_result_discarded);
    HC_ADD("redispatch_after_reset", test_redispatch_after_reset);
    HC_ADD("restart_skipped_when_locked", test_restart_skipped_when_locked);
    HC_ADD("stop_timeout_no_start", test_stop_timeout_no_start);
    HC_ADD("terminating_after_stop_skips_start", test_terminating_after_stop_skips_start);
    HC_ADD("terminating_stops_dispatch", test_terminating_stops_dispatch);
    HC_ADD("draining_defers_dispatch", test_draining_defers_dispatch);
    HC_ADD("timer_stops_when_empty", test_timer_stops_when_empty);
    HC_ADD("set_rejects_invalid_name", test_set_rejects_invalid_name);
    HC_ADD("set_validates_target_by_type", test_set_validates_target_by_type);
    HC_ADD("set_rejects_long_target", test_set_rejects_long_target);
    HC_ADD("set_rejects_exec_target_with_space", test_set_rejects_exec_target_with_space);
    HC_ADD("set_resets_runtime_state", test_set_resets_runtime_state);
    HC_ADD("failure_threshold_floor", test_failure_threshold_floor);
    HC_ADD("set_keeps_64_char_name", test_set_keeps_64_char_name);
}
