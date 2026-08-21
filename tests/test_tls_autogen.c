                                                                                  
                                                                                            
                                                                                   
                                                            
                                     
                           
  
                                               
                                                            
  
                                                          
                                                             
                                                 
                           
   
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <sys/stat.h>

#include "../src/utils/pcv_tls.h"
#include "../src/api/rest_server.h"
#include "../src/utils/pcv_spawn.h"

static void test_autogen_creates_valid_cert(void)
{
                                                           
                                                         
                                                        
                          
    pcv_spawn_launcher_init();

    gchar *dir = g_dir_make_tmp("pcvtls-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *cert = g_build_filename(dir, "node.crt", NULL);
    gchar *key  = g_build_filename(dir, "node.key", NULL);

    GError *err = NULL;
    g_assert_true(pcv_tls_autogen_selfsigned(cert, key, &err));
    g_assert_no_error(err);
    g_assert_true(g_file_test(cert, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(key,  G_FILE_TEST_EXISTS));

                  
    struct stat st;
    g_assert_cmpint(stat(key, &st), ==, 0);
    g_assert_cmpint((int)(st.st_mode & 0777), ==, 0600);

                                               
    GTlsCertificate *c = g_tls_certificate_new_from_files(cert, key, &err);
    g_assert_no_error(err);
    g_assert_nonnull(c);
    g_object_unref(c);

                            
    gint64 days = pcv_tls_cert_expiry_days_path(cert);
    g_assert_cmpint(days, >, 3600);

                     
    gchar *marker = g_strdup_printf("%s.autogen", cert);
    g_assert_true(g_file_test(marker, G_FILE_TEST_EXISTS));

                                                     
                                                                     
                                        
    gchar *cert_tmp   = g_strdup_printf("%s.tmp", cert);
    gchar *key_tmp    = g_strdup_printf("%s.tmp", key);
    gchar *marker_tmp = g_strdup_printf("%s.tmp", marker);
    g_assert_false(g_file_test(cert_tmp,   G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(key_tmp,    G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(marker_tmp, G_FILE_TEST_EXISTS));

                                                
                              
    g_unlink(cert_tmp);
    g_unlink(key_tmp);
    g_unlink(marker_tmp);
    g_unlink(marker);
    g_unlink(cert);
    g_unlink(key);
    g_rmdir(dir);

    g_free(cert_tmp); g_free(key_tmp); g_free(marker_tmp);
    g_free(marker); g_free(cert); g_free(key); g_free(dir);
}

static void test_expiry_days_path_missing(void)
{
    g_assert_cmpint(pcv_tls_cert_expiry_days_path("/nonexistent/x.crt"), ==, -1);
}

                                                   
static guint tls_init_call_count;

static void
record_tls_initialization(void)
{
    tls_init_call_count++;
}

static void
test_tls_disabled_skips_internal_initialization(void)
{
    tls_init_call_count = 0;

    PcvRestTransportPlan external = pcv_rest_transport_plan(
        pcv_rest_tls_mode_from_config(FALSE), "loopback");
    pcv_rest_transport_initialize(&external, record_tls_initialization);
    g_assert_cmpuint(tls_init_call_count, ==, 0);

    PcvRestTransportPlan internal = pcv_rest_transport_plan(
        pcv_rest_tls_mode_from_config(TRUE), "loopback");
    pcv_rest_transport_initialize(&internal, record_tls_initialization);
    g_assert_cmpuint(tls_init_call_count, ==, 1);
}

                                                        
                                                   
                                                
                                                     
static void test_cert_key_pair_ok(void)
{
    pcv_spawn_launcher_init();

    gchar *dir = g_dir_make_tmp("pcvtls-pair-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *certA = g_build_filename(dir, "a.crt", NULL);
    gchar *keyA  = g_build_filename(dir, "a.key", NULL);
    gchar *certB = g_build_filename(dir, "b.crt", NULL);
    gchar *keyB  = g_build_filename(dir, "b.key", NULL);

    GError *err = NULL;
    g_assert_true(pcv_tls_autogen_selfsigned(certA, keyA, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_tls_autogen_selfsigned(certB, keyB, &err));
    g_assert_no_error(err);

    g_assert_true(pcv_tls_cert_key_pair_ok(certA, keyA));
    g_assert_true(pcv_tls_cert_key_pair_ok(certB, keyB));
    g_assert_false(pcv_tls_cert_key_pair_ok(certA, keyB));
    g_assert_false(pcv_tls_cert_key_pair_ok(certB, keyA));

            
    gchar *markerA = g_strdup_printf("%s.autogen", certA);
    gchar *markerB = g_strdup_printf("%s.autogen", certB);
    g_unlink(certA); g_unlink(keyA); g_unlink(markerA);
    g_unlink(certB); g_unlink(keyB); g_unlink(markerB);
    g_rmdir(dir);

    g_free(markerA); g_free(markerB);
    g_free(certA); g_free(keyA); g_free(certB); g_free(keyB); g_free(dir);
}

void test_tls_autogen_register(void)
{
    g_test_add_func("/tls_autogen/creates_valid_cert", test_autogen_creates_valid_cert);
    g_test_add_func("/tls_autogen/expiry_days_path_missing", test_expiry_days_path_missing);
    g_test_add_func("/tls_autogen/cert_key_pair_ok", test_cert_key_pair_ok);
    g_test_add_func("/tls/disabled",
                    test_tls_disabled_skips_internal_initialization);
}
