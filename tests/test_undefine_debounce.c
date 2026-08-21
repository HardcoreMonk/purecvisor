                                                                                            
                                                                              
                                                                    
                                                                     
                                 
                                    
#include <glib.h>
#include "modules/daemons/pcv_undefine_debounce.h"

                                                                       
static void test_defined_within_window_cancels(void) {
    PcvUndefineDebounce *d = pcv_undefine_debounce_new();
    guint prev = pcv_undefine_debounce_note_undefined(d, "uuid-A", "vm-a", 111);
    g_assert_cmpuint(prev, ==, 0);                               
    guint tok = pcv_undefine_debounce_note_defined(d, "uuid-A");
    g_assert_cmpuint(tok, ==, 111);                                                
    gchar *name = pcv_undefine_debounce_take_expired(d, "uuid-A");
    g_assert_null(name);                                                       
    pcv_undefine_debounce_free(d);
}

                                                                 
static void test_no_defined_expires_to_cleanup(void) {
    PcvUndefineDebounce *d = pcv_undefine_debounce_new();
    pcv_undefine_debounce_note_undefined(d, "uuid-B", "vm-b", 222);
    gchar *name = pcv_undefine_debounce_take_expired(d, "uuid-B");
    g_assert_nonnull(name);
    g_assert_cmpstr(name, ==, "vm-b");                                 
    g_free(name);
                           
    g_assert_null(pcv_undefine_debounce_take_expired(d, "uuid-B"));
    pcv_undefine_debounce_free(d);
}

                               
static void test_other_uuid_defined_no_interference(void) {
    PcvUndefineDebounce *d = pcv_undefine_debounce_new();
    pcv_undefine_debounce_note_undefined(d, "uuid-C", "vm-c", 333);
    guint tok = pcv_undefine_debounce_note_defined(d, "uuid-OTHER");
    g_assert_cmpuint(tok, ==, 0);                                 
    gchar *name = pcv_undefine_debounce_take_expired(d, "uuid-C");
    g_assert_cmpstr(name, ==, "vm-c");                                     
    g_free(name);
    pcv_undefine_debounce_free(d);
}

                                                                 
static void test_double_undefined_returns_old_token(void) {
    PcvUndefineDebounce *d = pcv_undefine_debounce_new();
    pcv_undefine_debounce_note_undefined(d, "uuid-D", "vm-d", 444);
    guint old = pcv_undefine_debounce_note_undefined(d, "uuid-D", "vm-d", 555);
    g_assert_cmpuint(old, ==, 444);                                       
                                                   
    guint tok = pcv_undefine_debounce_note_defined(d, "uuid-D");
    g_assert_cmpuint(tok, ==, 555);
    pcv_undefine_debounce_free(d);
}

void test_undefine_debounce_register(void) {
    g_test_add_func("/undefine_debounce/defined_within_window_cancels", test_defined_within_window_cancels);
    g_test_add_func("/undefine_debounce/no_defined_expires_to_cleanup", test_no_defined_expires_to_cleanup);
    g_test_add_func("/undefine_debounce/other_uuid_no_interference", test_other_uuid_defined_no_interference);
    g_test_add_func("/undefine_debounce/double_undefined_returns_old_token", test_double_undefined_returns_old_token);
}
