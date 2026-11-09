                                                                                         
                                                                   
                                                                       
                                                                     
                         
  
                                                 
                                                     
  
                                          
                                                                      
                                                      
                                                        
                              
  
               
                                                                    
                                                             
  
                      
                                                
                                                                           
                                                               
                                                          
  
                                  
                                                       
   

#include <glib.h>
#include "modules/daemons/alert_dlq.h"
#include <json-glib/json-glib.h>

                                                               
static gchar *entry_of(const gchar *url, const gchar *payload) {
    return g_strdup_printf("%s|%s", url, payload);
}

                                          
static void assert_at(GPtrArray *a, guint i, const gchar *want) {
    g_assert_cmpuint(i, <, a->len);
    g_assert_cmpstr((const gchar *)g_ptr_array_index(a, i), ==, want);
}

                                                   
                                              
static void test_dlq_remove_by_value_survives_mutation(void) {
    pcv_alert_dlq_reset();

    gchar *e1 = entry_of("http://a", "1");
    gchar *e2 = entry_of("http://b", "2");
    gchar *e3 = entry_of("http://c", "3");
    gchar *e4 = entry_of("http://d", "4");

                        
    pcv_alert_dlq_store("http://a", "1");
    pcv_alert_dlq_store("http://b", "2");
    pcv_alert_dlq_store("http://c", "3");

                                                        
    GPtrArray *snap = pcv_alert_dlq_snapshot();
    g_assert_cmpuint(snap->len, ==, 3);

                                                               
    pcv_alert_dlq_store("http://d", "4");

                                                         
                                          
    GPtrArray *ok = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ok, g_strdup(e2));
    pcv_alert_dlq_remove_matching(ok);
    g_ptr_array_unref(ok);

                                               
                                                     
    GPtrArray *now = pcv_alert_dlq_snapshot();
    g_assert_cmpuint(now->len, ==, 3);
    assert_at(now, 0, e1);
    assert_at(now, 1, e3);
    assert_at(now, 2, e4);
    g_ptr_array_unref(now);

                                                    
                                                      
                                              
    g_assert_cmpuint(snap->len, ==, 3);
    assert_at(snap, 0, e1);
    assert_at(snap, 1, e2);
    assert_at(snap, 2, e3);
    g_ptr_array_unref(snap);

    g_free(e1); g_free(e2); g_free(e3); g_free(e4);
    pcv_alert_dlq_reset();
}

static gint g_fake_posts;
static gint64 g_fake_now_us;
static gint64 g_fake_step_us;
static gint g_term_after;

static gboolean fake_post_ok(const gchar *url, const gchar *payload) {
    (void)url; (void)payload;
    g_fake_posts++;
    g_fake_now_us += g_fake_step_us;
    return TRUE;
}
static gint64 fake_now(void) { return g_fake_now_us; }
static gboolean fake_terminating(void) { return g_term_after > 0 && g_fake_posts >= g_term_after; }

static void fake_setup(guint items, gint64 step_us, gint term_after) {
    pcv_alert_dlq_reset();
    for (guint i = 0; i < items; i++) {
        gchar *u = g_strdup_printf("http://h/%u", i);
        pcv_alert_dlq_store(u, "p");
        g_free(u);
    }
    g_fake_posts = 0; g_fake_now_us = 1000000; g_fake_step_us = step_us; g_term_after = term_after;
    pcv_alert_dlq_set_post_fn(fake_post_ok);
    pcv_alert_dlq_set_now_fn_for_test(fake_now);
    pcv_alert_dlq_set_terminating_fn_for_test(fake_terminating);
}
static void fake_teardown(void) {
    pcv_alert_dlq_set_post_fn(NULL);
    pcv_alert_dlq_set_now_fn_for_test(NULL);
    pcv_alert_dlq_set_terminating_fn_for_test(NULL);
    pcv_alert_dlq_reset();
}

static void test_dlq_retry_caps_items_at_100(void) {
    fake_setup(150, 0, 0);
    JsonObject *r = pcv_alert_dlq_retry();
    g_assert_cmpint(json_object_get_int_member(r, "retried"), ==, 100);
    g_assert_cmpint(json_object_get_int_member(r, "succeeded"), ==, 100);
    g_assert_cmpint(json_object_get_int_member(r, "remaining"), ==, 50);
    g_assert_true(json_object_get_boolean_member(r, "capped"));
    GPtrArray *left = pcv_alert_dlq_snapshot();
    g_assert_cmpuint(left->len, ==, 50);
    g_ptr_array_unref(left);
    json_object_unref(r);
    fake_teardown();
}

static void test_dlq_retry_caps_total_60s(void) {

    fake_setup(5, 30 * G_USEC_PER_SEC, 0);
    JsonObject *r = pcv_alert_dlq_retry();
    g_assert_cmpint(g_fake_posts, ==, 2);
    g_assert_cmpint(json_object_get_int_member(r, "retried"), ==, 2);
    g_assert_cmpint(json_object_get_int_member(r, "remaining"), ==, 3);
    g_assert_true(json_object_get_boolean_member(r, "capped"));
    json_object_unref(r);
    fake_teardown();
}

static void test_dlq_retry_result_keeps_old_fields(void) {
    fake_setup(3, 0, 0);
    JsonObject *r = pcv_alert_dlq_retry();
    g_assert_true(json_object_has_member(r, "retried"));
    g_assert_true(json_object_has_member(r, "succeeded"));
    g_assert_true(json_object_has_member(r, "failed"));
    g_assert_cmpint(json_object_get_int_member(r, "retried"), ==, 3);
    g_assert_cmpint(json_object_get_int_member(r, "succeeded"), ==, 3);
    g_assert_cmpint(json_object_get_int_member(r, "failed"), ==, 0);
    g_assert_cmpint(json_object_get_int_member(r, "remaining"), ==, 0);
    g_assert_false(json_object_get_boolean_member(r, "capped"));
    json_object_unref(r);
    fake_teardown();
}

static void test_dlq_retry_stops_when_terminating(void) {
    fake_setup(5, 0, 2);
    JsonObject *r = pcv_alert_dlq_retry();
    g_assert_cmpint(g_fake_posts, ==, 2);
    g_assert_cmpint(json_object_get_int_member(r, "retried"), ==, 2);
    g_assert_cmpint(json_object_get_int_member(r, "remaining"), ==, 3);
    g_assert_true(json_object_get_boolean_member(r, "capped"));
    json_object_unref(r);
    fake_teardown();
}

void test_alert_dlq_register(void) {
    g_test_add_func("/alert_dlq/remove_by_value_survives_mutation",
                    test_dlq_remove_by_value_survives_mutation);
    g_test_add_func("/alert_dlq/retry_caps_items_at_100", test_dlq_retry_caps_items_at_100);
    g_test_add_func("/alert_dlq/retry_caps_total_60s", test_dlq_retry_caps_total_60s);
    g_test_add_func("/alert_dlq/retry_result_keeps_old_fields", test_dlq_retry_result_keeps_old_fields);
    g_test_add_func("/alert_dlq/retry_stops_when_terminating", test_dlq_retry_stops_when_terminating);
}
