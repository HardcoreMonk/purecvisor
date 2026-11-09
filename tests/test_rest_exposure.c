                                                                                                                          
                                                                                               
                                                                                          
                                                                                
                                                         
#include <glib.h>

#include "../src/api/rest_server.h"
#include "../src/api/rest_auth.h"
#include "../src/utils/pcv_config.h"
#include <glib/gstdio.h>

static void
test_collector_auth_mode_parse(void)
{
    gboolean ok = FALSE;
    g_assert_cmpint(pcv_rest_collector_auth_mode_parse("loopback", &ok), ==, PCV_COLLECTOR_AUTH_LOOPBACK);
    g_assert_true(ok);
    g_assert_cmpint(pcv_rest_collector_auth_mode_parse(" Required ", &ok), ==, PCV_COLLECTOR_AUTH_REQUIRED);
    g_assert_true(ok);
    g_assert_cmpint(pcv_rest_collector_auth_mode_parse("NONE", &ok), ==, PCV_COLLECTOR_AUTH_NONE);
    g_assert_true(ok);
                                                       
    g_assert_cmpint(pcv_rest_collector_auth_mode_parse("oops", &ok), ==, PCV_COLLECTOR_AUTH_REQUIRED);
    g_assert_false(ok);
    g_assert_cmpint(pcv_rest_collector_auth_mode_parse("", &ok), ==, PCV_COLLECTOR_AUTH_REQUIRED);
    g_assert_false(ok);
    g_assert_cmpint(pcv_rest_collector_auth_mode_parse(NULL, &ok), ==, PCV_COLLECTOR_AUTH_REQUIRED);
    g_assert_false(ok);
    g_assert_cmpint(pcv_rest_collector_auth_mode_parse("nonee", NULL), ==, PCV_COLLECTOR_AUTH_REQUIRED);
}

static void
test_collector_needs_auth_table(void)
{
    g_assert_false(pcv_rest_collector_needs_auth(PCV_COLLECTOR_AUTH_LOOPBACK, TRUE));
    g_assert_true(pcv_rest_collector_needs_auth(PCV_COLLECTOR_AUTH_LOOPBACK, FALSE));                          
    g_assert_true(pcv_rest_collector_needs_auth(PCV_COLLECTOR_AUTH_REQUIRED, TRUE));
    g_assert_true(pcv_rest_collector_needs_auth(PCV_COLLECTOR_AUTH_REQUIRED, FALSE));
    g_assert_false(pcv_rest_collector_needs_auth(PCV_COLLECTOR_AUTH_NONE, TRUE));
    g_assert_false(pcv_rest_collector_needs_auth(PCV_COLLECTOR_AUTH_NONE, FALSE));
}

                                                                                        
static void
test_bearer_api_key_parse(void)
{
    g_assert_cmpstr(pcv_rest_bearer_api_key("Bearer pcv_abc"), ==, "pcv_abc");
    g_assert_cmpstr(pcv_rest_bearer_api_key("bearer   pcv_abc"), ==, "pcv_abc");                                
    g_assert_cmpstr(pcv_rest_bearer_api_key("  Bearer\tpcv_abc"), ==, "pcv_abc");
    g_assert_null(pcv_rest_bearer_api_key("Bearer eyJhbGciOiJIUzI1NiJ9.e30.x"));                           
    g_assert_null(pcv_rest_bearer_api_key("Bearer PCV_abc"));                                             
    g_assert_null(pcv_rest_bearer_api_key("Basic pcv_abc"));
    g_assert_null(pcv_rest_bearer_api_key("Bearerpcv_abc"));
    g_assert_null(pcv_rest_bearer_api_key("Bearer "));
    g_assert_null(pcv_rest_bearer_api_key("pcv_abc"));
    g_assert_null(pcv_rest_bearer_api_key(""));
    g_assert_null(pcv_rest_bearer_api_key(NULL));
}

                                                                                                     
                                                      
static void
_assert_metrics_auth_config(const gchar *conf, PcvCollectorAuthMode want, gboolean want_recognized)
{
    g_autofree gchar *dir = g_dir_make_tmp("pcv-metrics-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree gchar *path = g_build_filename(dir, "daemon.conf", NULL);
    g_assert_true(g_file_set_contents(path, conf, -1, NULL));
    g_setenv("PCV_CONFIG_PATH", path, TRUE);
    pcv_config_init();
    gboolean recognized = TRUE;
    g_assert_cmpint(pcv_rest_collector_auth_mode_from_config(&recognized), ==, want);
    g_assert_cmpint(recognized, ==, want_recognized);
    pcv_config_shutdown();
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);                                      
    g_assert_cmpint(g_remove(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void
test_collector_auth_config_path(void)
{
    _assert_metrics_auth_config("[metrics]\n", PCV_COLLECTOR_AUTH_LOOPBACK, TRUE);                             
    _assert_metrics_auth_config("[server]\n", PCV_COLLECTOR_AUTH_LOOPBACK, TRUE);                    
    _assert_metrics_auth_config("[metrics]\nauth =\n", PCV_COLLECTOR_AUTH_REQUIRED, FALSE);
    _assert_metrics_auth_config("[metrics]\nauth =   \n", PCV_COLLECTOR_AUTH_REQUIRED, FALSE);
    _assert_metrics_auth_config("[metrics]\nauth = none\n", PCV_COLLECTOR_AUTH_NONE, TRUE);
    _assert_metrics_auth_config("[metrics]\nauth = oops\n", PCV_COLLECTOR_AUTH_REQUIRED, FALSE);
}

void
test_rest_exposure_register(void)
{
    g_test_add_func("/rest_exposure/collector/mode_parse",  test_collector_auth_mode_parse);
    g_test_add_func("/rest_exposure/collector/needs_auth",  test_collector_needs_auth_table);
    g_test_add_func("/rest_exposure/collector/config_path", test_collector_auth_config_path);
    g_test_add_func("/rest_exposure/bearer_api_key_parse", test_bearer_api_key_parse);
}
