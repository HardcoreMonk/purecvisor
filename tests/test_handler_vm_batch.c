                                                                            
                                                                                    
                                       
                                                                     
                       
   
                                
                                                           
  
                                                                      
                                                                
  
                                                                               
                                          
                                                                               
                                                        
                                                       
                           
                                                               
                                                                   
                                          
                                                             
                                              
  
                                                                 
                                                              
                                                                  
                                               
                                                                 
                                                            
                          
  
                                                         
                     
                                                                           
                                                            
                                       
  
                                         
                                                       
                                                                               
   
#include <glib.h>
#include "../src/api/vm_batch_policy.h"

  
                                                                
                                                           
   
typedef enum { BATCH_ACCEPTED, BATCH_REJECTED } BatchVmDecision;
static BatchVmDecision batch_vm_decision(gboolean vm_exists) {
    return vm_exists ? BATCH_ACCEPTED : BATCH_REJECTED;
}

                                                         
static void test_nonwhitelist_action_rejected(void) {
                                                                 
    g_assert_false(pcv_vm_batch_action_is_whitelisted("reboot"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted("pause"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted("resume"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted("delete"));                    
    g_assert_false(pcv_vm_batch_action_is_whitelisted("bogus"));
    g_assert_false(pcv_vm_batch_action_is_whitelisted(NULL));
}

                                               
static void test_whitelist_action_allowed(void) {
    g_assert_true(pcv_vm_batch_action_is_whitelisted("start"));
    g_assert_true(pcv_vm_batch_action_is_whitelisted("stop"));
}

                                             
static void test_missing_vm_rejected(void) {
    g_assert_cmpint(batch_vm_decision(FALSE), ==, BATCH_REJECTED);
    g_assert_cmpint(batch_vm_decision(TRUE),  ==, BATCH_ACCEPTED);
}

static void test_name_reject_reason(void) {
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_assert_cmpstr(pcv_vm_batch_name_reject_reason(NULL, seen), ==, "empty vm name");
    g_assert_cmpstr(pcv_vm_batch_name_reject_reason("", seen), ==, "empty vm name");
    g_assert_null(pcv_vm_batch_name_reject_reason("vm-a", seen));
    g_assert_null(pcv_vm_batch_name_reject_reason("vm-b", seen));
    g_assert_cmpstr(pcv_vm_batch_name_reject_reason("vm-a", seen), ==, "duplicate");
    g_assert_cmpuint(g_hash_table_size(seen), ==, 2);
    g_hash_table_unref(seen);
}

static void test_stop_concurrency(void)
{
    g_assert_cmpuint(pcv_vm_batch_stop_concurrency(0), ==, 1);
    g_assert_cmpuint(pcv_vm_batch_stop_concurrency(1), ==, 1);
    g_assert_cmpuint(pcv_vm_batch_stop_concurrency(2), ==, 1);
    g_assert_cmpuint(pcv_vm_batch_stop_concurrency(5), ==, 4);
    g_assert_cmpuint(pcv_vm_batch_stop_concurrency(8), ==, 4);
    g_assert_cmpuint(pcv_vm_batch_stop_concurrency(64), ==, 4);
}

void test_handler_vm_batch_register(void) {
    g_test_add_func("/handler_vm_batch/nonwhitelist_action_rejected",
                    test_nonwhitelist_action_rejected);
    g_test_add_func("/handler_vm_batch/whitelist_action_allowed",
                    test_whitelist_action_allowed);
    g_test_add_func("/handler_vm_batch/missing_vm_rejected",
                    test_missing_vm_rejected);
    g_test_add_func("/handler_vm_batch/stop_concurrency", test_stop_concurrency);
    g_test_add_func("/handler_vm_batch/name_reject_reason",
                    test_name_reject_reason);
}
