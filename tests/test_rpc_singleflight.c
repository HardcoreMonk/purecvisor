   
                                
                                                       
  
                           
                                                               
                                                                       
                                                     
                                            
  
                       
                                                        
            
   
#include <glib.h>

#include "modules/dispatcher/rpc_singleflight.h"

static void
test_first_join_owns_run(void)
{
    PcvSingleflight *sf = pcv_singleflight_new(g_free);
    g_assert_true(pcv_singleflight_join(sf, "k", g_strdup("a")));
    g_assert_false(pcv_singleflight_join(sf, "k", g_strdup("b")));
    GPtrArray *w = pcv_singleflight_complete(sf, "k");
    g_assert_nonnull(w);
    g_ptr_array_unref(w);
    pcv_singleflight_free(sf);
}

static void
test_all_waiters_get_one_result(void)
{
    PcvSingleflight *sf = pcv_singleflight_new(g_free);
    g_assert_true(pcv_singleflight_join(sf, "vm1", g_strdup("w1")));
    g_assert_false(pcv_singleflight_join(sf, "vm1", g_strdup("w2")));
    g_assert_false(pcv_singleflight_join(sf, "vm1", g_strdup("w3")));
    GPtrArray *w = pcv_singleflight_complete(sf, "vm1");
    g_assert_nonnull(w);
    g_assert_cmpuint(w->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(w, 0), ==, "w1");              
    g_assert_cmpstr(g_ptr_array_index(w, 2), ==, "w3");
    g_ptr_array_unref(w);
    g_assert_null(pcv_singleflight_complete(sf, "vm1"));                
    pcv_singleflight_free(sf);
}

static void
test_late_joiner_after_completion_starts_new(void)
{
    PcvSingleflight *sf = pcv_singleflight_new(g_free);
    g_assert_true(pcv_singleflight_join(sf, "k", g_strdup("a")));
    g_ptr_array_unref(pcv_singleflight_complete(sf, "k"));
    g_assert_true(pcv_singleflight_join(sf, "k", g_strdup("late")));             
    GPtrArray *w = pcv_singleflight_complete(sf, "k");
    g_assert_cmpuint(w->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(w, 0), ==, "late");
    g_ptr_array_unref(w);
    pcv_singleflight_free(sf);
}

static void
test_keys_independent(void)
{
    PcvSingleflight *sf = pcv_singleflight_new(g_free);
    g_assert_true(pcv_singleflight_join(sf, "a", g_strdup("1")));
    g_assert_true(pcv_singleflight_join(sf, "b", g_strdup("2")));
    GPtrArray *wa = pcv_singleflight_complete(sf, "a");
    g_assert_cmpuint(wa->len, ==, 1);
    g_ptr_array_unref(wa);
    g_assert_false(pcv_singleflight_join(sf, "b", g_strdup("3")));                     
    pcv_singleflight_free(sf);                     
}

                                                                                      
static void
test_waiter_count(void)
{
    PcvSingleflight *sf = pcv_singleflight_new(g_free);
    g_assert_cmpuint(pcv_singleflight_waiter_count(sf, "k"), ==, 0);
    g_assert_true(pcv_singleflight_join(sf, "k", g_strdup("a")));
    g_assert_false(pcv_singleflight_join(sf, "k", g_strdup("b")));
    g_assert_true(pcv_singleflight_join(sf, "other", g_strdup("c")));
    g_assert_cmpuint(pcv_singleflight_waiter_count(sf, "k"), ==, 2);
    g_assert_cmpuint(pcv_singleflight_waiter_count(sf, "other"), ==, 1);
    g_ptr_array_unref(pcv_singleflight_complete(sf, "k"));
    g_assert_cmpuint(pcv_singleflight_waiter_count(sf, "k"), ==, 0);
    pcv_singleflight_free(sf);
}

void
test_rpc_singleflight_register(void)
{
    g_test_add_func("/rpc_singleflight/first_join_owns_run", test_first_join_owns_run);
    g_test_add_func("/rpc_singleflight/all_waiters_get_one_result", test_all_waiters_get_one_result);
    g_test_add_func("/rpc_singleflight/late_joiner_after_completion_starts_new",
                    test_late_joiner_after_completion_starts_new);
    g_test_add_func("/rpc_singleflight/keys_independent", test_keys_independent);
    g_test_add_func("/rpc_singleflight/waiter_count", test_waiter_count);
}
