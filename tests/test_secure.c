                                                                                              
                                                                             
                                       
                                                            
                                     
                         
#include <glib.h>
#include <string.h>
#include "utils/pcv_secure.h"
#include "utils/pcv_spawn.h"
static void test_wipe_zeroes_buffer(void) {
    gchar buf[64];
    memcpy(buf, "SECRETKEY0123456789", 20);
    pcv_secure_wipe(buf, sizeof buf);
    for (gsize i = 0; i < sizeof buf; i++) g_assert_cmpint(buf[i], ==, 0);                        
}
static void test_free_str_wipes_and_nulls(void) {
    gchar *s = g_strdup("wg-private-key-base64==");
    pcv_secure_free_str(&s);
    g_assert_null(s);                                
    pcv_secure_free_str(&s);                           
    gchar *n = NULL; pcv_secure_free_str(&n);                  
}
                                                                    
                                                                     
                                              
static void test_spawn_stdin_passthrough(void) {
    const gchar *argv[] = { "cat", NULL };                         
    gchar *out = NULL, *err = NULL; GError *e = NULL;
    gboolean ok = pcv_spawn_sync_stdin(argv, "secret-on-stdin", -1, &out, &err, &e);
    g_assert_true(ok); g_assert_cmpstr(out, ==, "secret-on-stdin");
    g_free(out); g_free(err);
}
void test_secure_register(void) {
    g_test_add_func("/secure/wipe_zeroes_buffer", test_wipe_zeroes_buffer);
    g_test_add_func("/secure/free_str_wipes_and_nulls", test_free_str_wipes_and_nulls);
    g_test_add_func("/secure/spawn_stdin_passthrough", test_spawn_stdin_passthrough);
}
