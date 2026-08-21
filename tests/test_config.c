                                                                                    
                                                                                             
                                                                       
                                                                        
                      
                      
  
                                                              
  
                 
                                                   
                                                       
                                                
  
                                    
  
                                                        
                                          
                                               
  
         
                                             
                                                                
   

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include "utils/pcv_config.h"

                                      
                                                    

                                                       

static void test_defaults(void) {
                          
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();

    g_assert_cmpstr(pcv_config_get_socket_path(), ==, PCV_DEFAULT_SOCKET_PATH);
    g_assert_cmpstr(pcv_config_get_libvirt_uri(),  ==, PCV_DEFAULT_LIBVIRT_URI);
    g_assert_cmpint(pcv_config_get_pool_max_conn(),==, PCV_DEFAULT_POOL_MAX_CONN);
    g_assert_cmpint(pcv_config_get_drain_timeout(),==, PCV_DEFAULT_DRAIN_TIMEOUT);
    g_assert_cmpstr(pcv_config_get_db_path(),      ==, PCV_DEFAULT_DB_PATH);
    g_assert_cmpstr(pcv_config_get_log_level(),    ==, PCV_DEFAULT_LOG_LEVEL);

    pcv_config_shutdown();
}

                                                              
                                                             
                                                                    
                                                    
                                                              
#define RESTORE_CFG_GUARD() \
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE)

                                              
                                            
static void test_rest_port_missing_config_default(void) {
    gchar *tmpdir = g_dir_make_tmp("pcv-cfg-missing-rest-XXXXXX", NULL);
    gchar *cfgpath = g_build_filename(tmpdir, "missing-daemon.conf", NULL);

    g_assert_false(g_file_test(cfgpath, G_FILE_TEST_EXISTS));
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init();
    g_assert_cmpint(PCV_DEFAULT_REST_PORT, ==, 80);
    g_assert_cmpint(pcv_config_get_rest_port(), ==, PCV_DEFAULT_REST_PORT);
    pcv_config_shutdown();

    RESTORE_CFG_GUARD();
    g_remove(cfgpath);
    g_rmdir(tmpdir);
    g_free(cfgpath);
    g_free(tmpdir);
}

                                                  

static void test_env_override_socket(void) {
    g_setenv("PURECVISOR_SOCKET_PATH", "/tmp/test.sock", TRUE);
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_socket_path(), ==, "/tmp/test.sock");
                  
    g_assert_cmpstr(pcv_config_get_libvirt_uri(), ==, PCV_DEFAULT_LIBVIRT_URI);
    pcv_config_shutdown();

    g_unsetenv("PURECVISOR_SOCKET_PATH");
}

                                                  

static void test_env_override_uri(void) {
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_setenv("PURECVISOR_LIBVIRT_URI", "test:///default", TRUE);
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_libvirt_uri(), ==, "test:///default");
    pcv_config_shutdown();

    g_unsetenv("PURECVISOR_LIBVIRT_URI");
}

                                                

static void test_env_override_pool_int(void) {
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_setenv("PURECVISOR_POOL_MAX_CONN", "4", TRUE);
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();
    g_assert_cmpint(pcv_config_get_pool_max_conn(), ==, 4);
    pcv_config_shutdown();

    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
}

                                            

static void test_env_invalid_int_fallback(void) {
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_setenv("PURECVISOR_POOL_MAX_CONN", "not-a-number", TRUE);
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();
    g_assert_cmpint(pcv_config_get_pool_max_conn(), ==, PCV_DEFAULT_POOL_MAX_CONN);
    pcv_config_shutdown();

    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
}

                                                          

static void test_dump_no_crash(void) {
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");

    pcv_config_init();
    pcv_config_dump();                       
    pcv_config_shutdown();
}

                                                         

static void clear_env(void) {
    g_unsetenv("PURECVISOR_SOCKET_PATH");
    g_unsetenv("PURECVISOR_LIBVIRT_URI");
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
    g_unsetenv("PURECVISOR_DB_PATH");
    g_unsetenv("PURECVISOR_LOG_LEVEL");
}

static void test_storage_getters(void) {
    clear_env();
    pcv_config_init();
                                       
    g_assert_nonnull(pcv_config_get_zvol_pool());
    g_assert_nonnull(pcv_config_get_container_pool());
    g_assert_nonnull(pcv_config_get_container_path());
    g_assert_nonnull(pcv_config_get_image_dir());
    g_assert_nonnull(pcv_config_get_iso_dirs());
    g_assert_nonnull(pcv_config_get_ssh_user());
    pcv_config_shutdown();
}

static void test_get_string_default(void) {
    clear_env();
    pcv_config_init();
                        
    const gchar *v = pcv_config_get_string("nonexistent", "key", "fallback");
    g_assert_cmpstr(v, ==, "fallback");
                              
    v = pcv_config_get_string(NULL, NULL, "default");
    g_assert_nonnull(v);
    pcv_config_shutdown();
}

static void test_get_int_default(void) {
    clear_env();
    pcv_config_init();
    gint v = pcv_config_get_int("nonexistent", "key", 42);
    g_assert_cmpint(v, ==, 42);
                                                                 
    (void)pcv_config_get_int(NULL, NULL, 99);
    pcv_config_shutdown();
}

static void test_db_path_env_override(void) {
    clear_env();
    g_setenv("PURECVISOR_DB_PATH", "/tmp/test-vm-state.db", TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_db_path(), ==, "/tmp/test-vm-state.db");
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_DB_PATH");
}

static void test_log_level_env_override(void) {
    clear_env();
    g_setenv("PURECVISOR_LOG_LEVEL", "debug", TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_log_level(), ==, "debug");
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_LOG_LEVEL");
}

static void test_drain_timeout_env(void) {
    clear_env();
    g_setenv("PURECVISOR_DRAIN_TIMEOUT", "60", TRUE);
    pcv_config_init();
    g_assert_cmpint(pcv_config_get_drain_timeout(), ==, 60);
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_DRAIN_TIMEOUT");
}

static void test_init_shutdown_cycle(void) {
    clear_env();
                              
    for (int i = 0; i < 3; i++) {
        pcv_config_init();
        pcv_config_shutdown();
    }
}

                                                        

static void test_parse_keyfile(void) {
    clear_env();
    gchar *tmpdir = g_dir_make_tmp("pcv-cfg-XXXXXX", NULL);
    gchar *cfgpath = g_build_filename(tmpdir, "daemon.conf", NULL);
    const gchar *content =
        "[daemon]\n"
        "socket_path=/tmp/parsed.sock\n"
        "pool_max_conn=16\n"
        "drain_timeout=45\n"
        "[storage]\n"
        "zvol_pool=parsed_pool/vms\n"
        "image_dir=/tmp/imgs\n"
        "[logging]\n"
        "level=debug\n";
    g_file_set_contents(cfgpath, content, -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_socket_path(), ==, "/tmp/parsed.sock");
    g_assert_cmpint(pcv_config_get_pool_max_conn(), ==, 16);
    g_assert_cmpint(pcv_config_get_drain_timeout(), ==, 45);
    g_assert_cmpstr(pcv_config_get_zvol_pool(), ==, "parsed_pool/vms");
    g_assert_cmpstr(pcv_config_get_image_dir(), ==, "/tmp/imgs");
                        
    g_assert_cmpstr(pcv_config_get_string("daemon", "socket_path", "x"), ==, "/tmp/parsed.sock");
    g_assert_cmpint(pcv_config_get_int("daemon", "pool_max_conn", 0), ==, 16);
    pcv_config_shutdown();

    RESTORE_CFG_GUARD();
    g_unlink(cfgpath); g_rmdir(tmpdir);
    g_free(cfgpath); g_free(tmpdir);
}

static void test_dup_raw_value_distinguishes_empty_and_missing(void) {
    clear_env();
    gchar *tmpdir = g_dir_make_tmp("pcv-cfg-raw-XXXXXX", NULL);
    gchar *cfgpath = g_build_filename(tmpdir, "daemon.conf", NULL);
    const gchar *content =
        "[raw]\n"
        "empty=\n"
        "spaces=   \n"
        "value=81oops\n";
    g_assert_true(g_file_set_contents(cfgpath, content, -1, NULL));
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init();
    gchar *empty = pcv_config_dup_raw_value("raw", "empty");
    gchar *spaces = pcv_config_dup_raw_value("raw", "spaces");
    gchar *value = pcv_config_dup_raw_value("raw", "value");
    gchar *second = pcv_config_dup_raw_value("raw", "value");
    gchar *missing = pcv_config_dup_raw_value("raw", "missing");

    g_assert_nonnull(empty);
    g_assert_cmpstr(empty, ==, "");
    g_assert_nonnull(spaces);
    g_assert_cmpstr(value, ==, "81oops");
    g_assert_cmpstr(second, ==, "81oops");
    g_assert_true(value != second);
    value[0] = '9';
    g_assert_cmpstr(second, ==, "81oops");
    g_assert_null(missing);

    g_free(empty);
    g_free(spaces);
    g_free(value);
    g_free(second);
    pcv_config_shutdown();
    RESTORE_CFG_GUARD();
    g_unlink(cfgpath);
    g_rmdir(tmpdir);
    g_free(cfgpath);
    g_free(tmpdir);
}

                                                                       
static void test_secret_from_env(void) {
    clear_env();
    g_setenv("PCV_SECRET_TESTGROUP_APIKEY", "supersecret123", TRUE);
    pcv_config_init();
    gchar *v = pcv_config_get_secret("testgroup", "apikey", "default");
    g_assert_cmpstr(v, ==, "supersecret123");
    g_free(v);
    pcv_config_shutdown();
    g_unsetenv("PCV_SECRET_TESTGROUP_APIKEY");
}

static void test_secret_fallback(void) {
    clear_env();
    pcv_config_init();
                                        
    gchar *v = pcv_config_get_secret("nogroup", "nokey", "fallback-value");
    g_assert_cmpstr(v, ==, "fallback-value");
    g_free(v);
    pcv_config_shutdown();
}

static void test_secret_plaintext_from_keyfile(void) {
    clear_env();
    gchar *tmpdir = g_dir_make_tmp("pcv-cfg-sec-XXXXXX", NULL);
    gchar *cfgpath = g_build_filename(tmpdir, "daemon.conf", NULL);
    g_file_set_contents(cfgpath,
        "[secrets]\nplain_pw=mypassword\n", -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);
    pcv_config_init();
    gchar *v = pcv_config_get_secret("secrets", "plain_pw", "x");
    g_assert_cmpstr(v, ==, "mypassword");
    g_free(v);
    pcv_config_shutdown();
    RESTORE_CFG_GUARD();
    g_unlink(cfgpath); g_rmdir(tmpdir);
    g_free(cfgpath); g_free(tmpdir);
}

                                                                                      
static void test_encrypt_value_returns_enc_prefix(void) {
    clear_env();
    pcv_config_init();
    gchar *enc = pcv_config_encrypt_value("hello");
    if (enc) {
                                                             
        g_assert_true(g_str_has_prefix(enc, "ENC2:"));
        g_free(enc);
    }
                    
    g_assert_null(pcv_config_encrypt_value(NULL));
    pcv_config_shutdown();
}

                                                             
                                                               
                                                          
static void test_encrypt_random_nonce_and_roundtrip(void) {
    clear_env();
    pcv_config_init();

    const gchar *pt = "tenant-wg-privkey-P";
    gchar *e1 = pcv_config_encrypt_value(pt);
    if (!e1) {
                                                             
        g_test_skip("machine-id 미접근으로 암호화 불가");
        pcv_config_shutdown();
        return;
    }
    g_assert_true(g_str_has_prefix(e1, "ENC2:"));

    gchar *e2 = pcv_config_encrypt_value(pt);
    g_assert_nonnull(e2);
    g_assert_true(g_str_has_prefix(e2, "ENC2:"));
                                                                  
    g_assert_cmpstr(e1, !=, e2);

                                                             
    gchar *d1 = pcv_config_decrypt_value(e1);
    gchar *d2 = pcv_config_decrypt_value(e2);
    g_assert_cmpstr(d1, ==, pt);
    g_assert_cmpstr(d2, ==, pt);

    g_free(e1); g_free(e2); g_free(d1); g_free(d2);
    pcv_config_shutdown();
}

                                                               
                                                        
                    
static gchar *
make_legacy_enc_blob(const gchar *plaintext) {
    gchar *machine_id = NULL;
    gsize mid_len = 0;
    if (!g_file_get_contents("/etc/machine-id", &machine_id, &mid_len, NULL))
        return NULL;
    g_strstrip(machine_id);
    mid_len = strlen(machine_id);
    if (mid_len == 0) { g_free(machine_id); return NULL; }

    guchar key[32], iv[12];
    static const guchar salt[] = "purecvisor-config-v1";
    if (PKCS5_PBKDF2_HMAC(machine_id, (int)mid_len, salt, sizeof(salt) - 1,
                          100000, EVP_sha256(), 32, key) != 1) {
        g_free(machine_id);
        return NULL;
    }
    guchar sha_buf[SHA256_DIGEST_LENGTH];
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md, EVP_sha256(), NULL);
    EVP_DigestUpdate(md, machine_id, mid_len);
    EVP_DigestUpdate(md, "iv-derivation", 13);
    EVP_DigestFinal_ex(md, sha_buf, NULL);
    EVP_MD_CTX_free(md);
    memcpy(iv, sha_buf, 12);
    g_free(machine_id);

    int pt_len = (int)strlen(plaintext);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return NULL;
    guchar *buf = g_malloc0((gsize)16 + (gsize)pt_len + 16);                    
    guchar *ct = buf + 16;
    int out_len = 0, final_len = 0;
    guchar tag[16];
    gboolean ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, iv) == 1 &&
        EVP_EncryptUpdate(ctx, ct, &out_len, (const guchar *)plaintext, pt_len) == 1 &&
        EVP_EncryptFinal_ex(ctx, ct + out_len, &final_len) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) { g_free(buf); return NULL; }
    memcpy(buf, tag, 16);
    gsize total = 16 + (gsize)(out_len + final_len);
    gchar *b64 = g_base64_encode(buf, total);
    g_free(buf);
    gchar *result = g_strdup_printf("ENC:%s", b64);
    g_free(b64);
    return result;
}

                                                  
                                                             
static void test_decrypt_legacy_enc_prefix(void) {
    clear_env();
    pcv_config_init();

    gchar *legacy = make_legacy_enc_blob("legacy-secret-42");
    if (!legacy) {
        g_test_skip("machine-id 미접근으로 레거시 blob 생성 불가");
        pcv_config_shutdown();
        return;
    }
    g_assert_true(g_str_has_prefix(legacy, "ENC:"));
    g_assert_false(g_str_has_prefix(legacy, "ENC2:"));

    gchar *dec = pcv_config_decrypt_value(legacy);
    g_assert_cmpstr(dec, ==, "legacy-secret-42");

    g_free(legacy); g_free(dec);
    pcv_config_shutdown();
}

static void test_reload_no_crash(void) {
    clear_env();
    pcv_config_init();
    pcv_config_reload();                          
    pcv_config_shutdown();
}

                                                      

static void test_env_override_jwt_secret(void) {
    clear_env();
    g_unsetenv("PURECVISOR_JWT_SECRET");
    g_setenv("PURECVISOR_JWT_SECRET", "my-test-jwt-secret-key", TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_jwt_secret(), ==, "my-test-jwt-secret-key");
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_JWT_SECRET");
}

                                                      

static void test_env_override_admin_user(void) {
    clear_env();
    g_unsetenv("PURECVISOR_ADMIN_USER");
    g_setenv("PURECVISOR_ADMIN_USER", "testadmin", TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_admin_user(), ==, "testadmin");
                       
    g_assert_cmpstr(pcv_config_get_socket_path(), ==, PCV_DEFAULT_SOCKET_PATH);
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_ADMIN_USER");
}

                                                      

static void test_env_override_pool_max_conn(void) {
    clear_env();
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
    g_setenv("PURECVISOR_POOL_MAX_CONN", "8", TRUE);
    pcv_config_init();
    g_assert_cmpint(pcv_config_get_pool_max_conn(), ==, 8);
    pcv_config_shutdown();
    g_unsetenv("PURECVISOR_POOL_MAX_CONN");
}

static void test_parse_invalid_keyfile(void) {
    clear_env();
    gchar *tmpdir = g_dir_make_tmp("pcv-cfg-bad-XXXXXX", NULL);
    gchar *cfgpath = g_build_filename(tmpdir, "daemon.conf", NULL);
                                         
    g_file_set_contents(cfgpath, "this is not valid ini\n=key\n[unclosed\n", -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init();                                
    g_assert_nonnull(pcv_config_get_socket_path());
    pcv_config_shutdown();

    RESTORE_CFG_GUARD();
    g_unlink(cfgpath); g_rmdir(tmpdir);
    g_free(cfgpath); g_free(tmpdir);
}

                                                        

static void test_allow_core_dumps_default_false(void) {
    g_unsetenv("PURECVISOR_ALLOW_CORE_DUMPS");
    pcv_config_init();
    g_assert_false(pcv_config_get_allow_core_dumps());                 
}
static void test_allow_core_dumps_env_true(void) {
    g_setenv("PURECVISOR_ALLOW_CORE_DUMPS", "true", TRUE);
    pcv_config_init();
    g_assert_true(pcv_config_get_allow_core_dumps());                     
    g_setenv("PURECVISOR_ALLOW_CORE_DUMPS", "0", TRUE);
    pcv_config_init();
    g_assert_false(pcv_config_get_allow_core_dumps());                   
    g_unsetenv("PURECVISOR_ALLOW_CORE_DUMPS");
}

                                                           

static void test_ips_defaults(void) {
    g_unsetenv("PURECVISOR_IPS_ENABLED");
    g_unsetenv("PURECVISOR_IPS_QUEUE_NUM");
    g_unsetenv("PURECVISOR_IPS_FAIL_OPEN");
    g_unsetenv("PURECVISOR_IPS_MODE");
    RESTORE_CFG_GUARD();

    pcv_config_init();
    g_assert_false(pcv_config_get_ips_enabled());
    g_assert_cmpint(pcv_config_get_ips_queue_num(), ==, 0);
    g_assert_true(pcv_config_get_ips_fail_open());
    g_assert_cmpstr(pcv_config_get_ips_mode(), ==, "detect");
    pcv_config_shutdown();
}

static void test_ips_env_override(void) {
    g_setenv("PURECVISOR_IPS_ENABLED", "true", TRUE);
    g_setenv("PURECVISOR_IPS_QUEUE_NUM", "5", TRUE);
    g_setenv("PURECVISOR_IPS_FAIL_OPEN", "false", TRUE);
    RESTORE_CFG_GUARD();

    pcv_config_init();
    g_assert_true(pcv_config_get_ips_enabled());
    g_assert_cmpint(pcv_config_get_ips_queue_num(), ==, 5);
    g_assert_false(pcv_config_get_ips_fail_open());
    pcv_config_shutdown();

    g_unsetenv("PURECVISOR_IPS_ENABLED");
    g_unsetenv("PURECVISOR_IPS_QUEUE_NUM");
    g_unsetenv("PURECVISOR_IPS_FAIL_OPEN");
}

static void test_ips_file_section(void) {
    g_unsetenv("PURECVISOR_IPS_ENABLED");
    g_unsetenv("PURECVISOR_IPS_QUEUE_NUM");
    g_unsetenv("PURECVISOR_IPS_FAIL_OPEN");
    g_unsetenv("PURECVISOR_IPS_MODE");

    gchar *cfgpath = g_build_filename(g_get_tmp_dir(), "pcv_ips_test.conf", NULL);
    g_file_set_contents(cfgpath,
        "[ips]\nenabled = true\nqueue_num = 3\nfail_open = false\nmode = detect\n",
        -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init();
    g_assert_true(pcv_config_get_ips_enabled());
    g_assert_cmpint(pcv_config_get_ips_queue_num(), ==, 3);
    g_assert_false(pcv_config_get_ips_fail_open());
    g_assert_cmpstr(pcv_config_get_ips_mode(), ==, "detect");
    pcv_config_shutdown();

    RESTORE_CFG_GUARD();
    g_remove(cfgpath);
    g_free(cfgpath);
}

                                                                
                                                  
static void test_ips_no_cross_section_bleed(void) {
    g_unsetenv("PURECVISOR_IPS_ENABLED");
    g_unsetenv("PURECVISOR_IPS_QUEUE_NUM");
    g_unsetenv("PURECVISOR_IPS_FAIL_OPEN");
    g_unsetenv("PURECVISOR_IPS_MODE");

    gchar *cfgpath = g_build_filename(g_get_tmp_dir(), "pcv_ips_bleed.conf", NULL);
    g_file_set_contents(cfgpath,
        "[tls]\nenabled = true\n[alert]\nenabled = true\n",
        -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init();
    g_assert_false(pcv_config_get_ips_enabled());                                    
    pcv_config_shutdown();

    RESTORE_CFG_GUARD();
    g_remove(cfgpath);
    g_free(cfgpath);
}

                                                           

                                                 
static void webpush_unset_env(void) {
    g_unsetenv("PURECVISOR_WEBPUSH_ENABLED");
    g_unsetenv("PURECVISOR_WEBPUSH_MIN_SEVERITY");
    g_unsetenv("PURECVISOR_WEBPUSH_CONTACT");
}

                                                                
                                           
static void test_webpush_defaults(void) {
    webpush_unset_env();
    RESTORE_CFG_GUARD();

    pcv_config_init();
    g_assert_true(pcv_config_get_webpush_enabled());
    g_assert_cmpstr(pcv_config_get_webpush_min_severity(), ==, "warn");
    g_assert_cmpstr(pcv_config_get_webpush_contact(), ==, "");
    pcv_config_shutdown();
}

                                
static void test_webpush_parse(void) {
    webpush_unset_env();

    gchar *cfgpath = g_build_filename(g_get_tmp_dir(), "pcv_webpush_test.conf", NULL);
    g_file_set_contents(cfgpath,
        "[webpush]\nenabled = false\nmin_severity = crit\ncontact = mailto:a@b\n",
        -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init();
    g_assert_false(pcv_config_get_webpush_enabled());
    g_assert_cmpstr(pcv_config_get_webpush_min_severity(), ==, "crit");
    g_assert_cmpstr(pcv_config_get_webpush_contact(), ==, "mailto:a@b");
    pcv_config_shutdown();

    RESTORE_CFG_GUARD();
    g_remove(cfgpath);
    g_free(cfgpath);
}

                                                   
                                                         
                                  
static void test_webpush_min_severity_invalid(void) {
    webpush_unset_env();

    gchar *cfgpath = g_build_filename(g_get_tmp_dir(), "pcv_webpush_bogus.conf", NULL);
    g_file_set_contents(cfgpath, "[webpush]\nmin_severity = bogus\n", -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_webpush_min_severity(), ==, "warn");
    pcv_config_shutdown();

    RESTORE_CFG_GUARD();
    g_remove(cfgpath);
    g_free(cfgpath);
}

                                                    
                                                                 
                                                   
                                                              
                
static void test_webpush_contact_no_scheme_dropped(void) {
    webpush_unset_env();

    gchar *cfgpath = g_build_filename(g_get_tmp_dir(), "pcv_webpush_contact_bad.conf", NULL);
    g_file_set_contents(cfgpath, "[webpush]\ncontact = admin@example.com\n", -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_webpush_contact(), ==, "");
    pcv_config_shutdown();

    RESTORE_CFG_GUARD();
    g_remove(cfgpath);
    g_free(cfgpath);
}

                                                     
                                 
static void test_webpush_contact_valid_schemes_kept(void) {
    const gchar *cases[] = { "mailto:ops@example.com", "https://example.com/contact" };

    for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
        webpush_unset_env();
        gchar *cfgpath = g_build_filename(g_get_tmp_dir(), "pcv_webpush_contact_ok.conf", NULL);
        gchar *body = g_strdup_printf("[webpush]\ncontact = %s\n", cases[i]);
        g_file_set_contents(cfgpath, body, -1, NULL);
        g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

        pcv_config_init();
        g_assert_cmpstr(pcv_config_get_webpush_contact(), ==, cases[i]);
        pcv_config_shutdown();

        RESTORE_CFG_GUARD();
        g_remove(cfgpath);
        g_free(cfgpath);
        g_free(body);
    }
}

                                                                        
                                                
static void test_webpush_no_cross_section_bleed(void) {
    webpush_unset_env();

    gchar *cfgpath = g_build_filename(g_get_tmp_dir(), "pcv_webpush_bleed.conf", NULL);
    g_file_set_contents(cfgpath,
        "[tls]\nenabled = false\n[alert]\nenabled = false\n", -1, NULL);
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);

    pcv_config_init();
    g_assert_true(pcv_config_get_webpush_enabled());                   
    pcv_config_shutdown();

    RESTORE_CFG_GUARD();
    g_remove(cfgpath);
    g_free(cfgpath);
}

                                                        

static gchar *
init_transport_config(const gchar *content, gchar **tmpdir_out)
{
    gchar *tmpdir = g_dir_make_tmp("pcv-cfg-transport-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    gchar *cfgpath = g_build_filename(tmpdir, "daemon.conf", NULL);
    g_assert_true(g_file_set_contents(cfgpath, content, -1, NULL));
    g_setenv("PCV_CONFIG_PATH", cfgpath, TRUE);
    pcv_config_init();
    *tmpdir_out = tmpdir;
    return cfgpath;
}

static void
shutdown_transport_config(gchar *cfgpath, gchar *tmpdir)
{
    pcv_config_shutdown();
    RESTORE_CFG_GUARD();
    g_assert_cmpint(g_remove(cfgpath), ==, 0);
    g_assert_cmpint(g_rmdir(tmpdir), ==, 0);
    g_free(cfgpath);
    g_free(tmpdir);
}

static void test_https_before_init_defaults_true(void) {
    gboolean enabled = FALSE;
    GError *error = NULL;

    pcv_config_shutdown();
    g_assert_true(pcv_config_get_https_enabled(&enabled, &error));
    g_assert_no_error(error);
    g_assert_true(enabled);
}

static void test_https_missing_defaults_true(void) {
    gchar *tmpdir = NULL;
    gchar *cfgpath = init_transport_config("[server]\nbind_plaintext=all\n",
                                           &tmpdir);
    gboolean enabled = FALSE;
    GError *error = NULL;

    g_assert_true(pcv_config_get_https_enabled(&enabled, &error));
    g_assert_no_error(error);
    g_assert_true(enabled);
    g_assert_true(pcv_config_validate_transport(&error));
    g_assert_no_error(error);

    shutdown_transport_config(cfgpath, tmpdir);
}

static void test_https_explicit_false(void) {
    gchar *tmpdir = NULL;
    gchar *cfgpath = init_transport_config(
        "[tls]\nhttps_enabled=false\n[server]\nbind_plaintext=loopback\n",
        &tmpdir);
    gboolean enabled = TRUE;
    GError *error = NULL;

    g_assert_true(pcv_config_get_https_enabled(&enabled, &error));
    g_assert_no_error(error);
    g_assert_false(enabled);

    shutdown_transport_config(cfgpath, tmpdir);
}

static void test_https_explicit_true(void) {
    gchar *tmpdir = NULL;
    gchar *cfgpath = init_transport_config(
        "[tls]\nhttps_enabled=true\n[server]\nbind_plaintext=all\n",
        &tmpdir);
    gboolean enabled = FALSE;
    GError *error = NULL;

    g_assert_true(pcv_config_get_https_enabled(&enabled, &error));
    g_assert_no_error(error);
    g_assert_true(enabled);
    g_assert_true(pcv_config_validate_transport(&error));
    g_assert_no_error(error);

    shutdown_transport_config(cfgpath, tmpdir);
}

static void test_https_invalid_boolean_rejected(void) {
    gchar *tmpdir = NULL;
    gchar *cfgpath = init_transport_config(
        "[tls]\nhttps_enabled=distinctive-invalid-secret-like-value\n",
        &tmpdir);
    gboolean enabled = TRUE;
    GError *error = NULL;

    g_assert_false(pcv_config_get_https_enabled(&enabled, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_null(strstr(error->message,
                         "distinctive-invalid-secret-like-value"));
    g_clear_error(&error);

    shutdown_transport_config(cfgpath, tmpdir);
}

static void test_https_external_tls_accepts_loopback(void) {
    gchar *tmpdir = NULL;
    gchar *cfgpath = init_transport_config(
        "[tls]\nhttps_enabled=false\n[server]\nbind_plaintext=loopback\n",
        &tmpdir);
    GError *error = NULL;

    g_assert_true(pcv_config_validate_transport(&error));
    g_assert_no_error(error);

    shutdown_transport_config(cfgpath, tmpdir);
}

static void test_https_external_tls_rejects_missing_bind_mode(void) {
    gchar *tmpdir = NULL;
    gchar *cfgpath = init_transport_config(
        "[tls]\nhttps_enabled=false\n",
        &tmpdir);
    GError *error = NULL;

    g_assert_false(pcv_config_validate_transport(&error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

    shutdown_transport_config(cfgpath, tmpdir);
}

static void test_https_external_tls_rejects_empty_bind_mode(void) {
    gchar *tmpdir = NULL;
    gchar *cfgpath = init_transport_config(
        "[tls]\nhttps_enabled=false\n[server]\nbind_plaintext=\n",
        &tmpdir);
    GError *error = NULL;

    g_assert_false(pcv_config_validate_transport(&error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

    shutdown_transport_config(cfgpath, tmpdir);
}

static void test_https_external_tls_rejects_non_loopback(void) {
    const gchar *modes[] = { "all", "external", NULL };

    for (guint i = 0; modes[i] != NULL; i++) {
        gchar *tmpdir = NULL;
        gchar *content = g_strdup_printf(
            "[tls]\nhttps_enabled=false\n[server]\nbind_plaintext=%s\n",
            modes[i]);
        gchar *cfgpath = init_transport_config(content, &tmpdir);
        GError *error = NULL;

        g_assert_false(pcv_config_validate_transport(&error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);

        shutdown_transport_config(cfgpath, tmpdir);
        g_free(content);
    }
}

typedef struct {
    const gchar *cfgpath;
    gint stop;
    gint reload_failed;
    GMutex mutex;
    GCond first_reload;
    guint successful_reloads;
} TransportReloadRace;

static gpointer
reload_rejected_transport_configs(gpointer data)
{
    TransportReloadRace *race = data;
    const gchar *configs[] = {
        "[tls]\nhttps_enabled=false\n[server]\nbind_plaintext=all\n",
        "[tls]\nhttps_enabled=distinctive-invalid-reload-value\n"
        "[server]\nbind_plaintext=loopback\n"
    };
    guint index = 0;

    while (!g_atomic_int_get(&race->stop)) {
        if (!g_file_set_contents(race->cfgpath, configs[index], -1, NULL) ||
            !pcv_config_reload()) {
            g_atomic_int_set(&race->reload_failed, TRUE);
            g_mutex_lock(&race->mutex);
            g_cond_signal(&race->first_reload);
            g_mutex_unlock(&race->mutex);
            break;
        }
        g_mutex_lock(&race->mutex);
        race->successful_reloads++;
        g_cond_signal(&race->first_reload);
        g_mutex_unlock(&race->mutex);
        index ^= 1;
    }
    return NULL;
}

                                                       
                                                        
static void test_https_validation_uses_single_reload_snapshot(void) {
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }

    gchar *tmpdir = NULL;
    gchar *cfgpath = init_transport_config(
        "[tls]\nhttps_enabled=false\n[server]\nbind_plaintext=all\n",
        &tmpdir);
    TransportReloadRace race = { .cfgpath = cfgpath };
    g_mutex_init(&race.mutex);
    g_cond_init(&race.first_reload);
    GThread *reloader = g_thread_new("transport-reload",
                                     reload_rejected_transport_configs,
                                     &race);
    gboolean accepted = FALSE;

    g_mutex_lock(&race.mutex);
    while (race.successful_reloads == 0 &&
           !g_atomic_int_get(&race.reload_failed))
        g_cond_wait(&race.first_reload, &race.mutex);
    g_assert_cmpuint(race.successful_reloads, >, 0);
    g_mutex_unlock(&race.mutex);

    for (guint i = 0; i < 200000 && !accepted; i++) {
        GError *error = NULL;
        accepted = pcv_config_validate_transport(&error);
        if (!accepted)
            g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
    }

    g_atomic_int_set(&race.stop, TRUE);
    g_thread_join(reloader);
    g_assert_false(g_atomic_int_get(&race.reload_failed));
    g_mutex_lock(&race.mutex);
    g_assert_cmpuint(race.successful_reloads, >, 0);
    g_mutex_unlock(&race.mutex);
    g_assert_false(accepted);
    g_cond_clear(&race.first_reload);
    g_mutex_clear(&race.mutex);

    shutdown_transport_config(cfgpath, tmpdir);
}

                                                        

void test_config_register(void) {
    g_test_add_func("/config/defaults",                 test_defaults);
    g_test_add_func("/config/rest_port_missing_config_default",
                    test_rest_port_missing_config_default);
    g_test_add_func("/config/env_override_socket",      test_env_override_socket);
    g_test_add_func("/config/env_override_uri",         test_env_override_uri);
    g_test_add_func("/config/env_override_pool_int",    test_env_override_pool_int);
    g_test_add_func("/config/env_invalid_int_fallback", test_env_invalid_int_fallback);
    g_test_add_func("/config/dump_no_crash",            test_dump_no_crash);
    g_test_add_func("/config/storage_getters",          test_storage_getters);
    g_test_add_func("/config/get_string_default",       test_get_string_default);
    g_test_add_func("/config/get_int_default",          test_get_int_default);
    g_test_add_func("/config/db_path_env_override",     test_db_path_env_override);
    g_test_add_func("/config/log_level_env_override",   test_log_level_env_override);
    g_test_add_func("/config/drain_timeout_env",        test_drain_timeout_env);
    g_test_add_func("/config/init_shutdown_cycle",      test_init_shutdown_cycle);
    g_test_add_func("/config/parse_keyfile",            test_parse_keyfile);
    g_test_add_func("/config/dup_raw_value_distinguishes_empty_and_missing",
                    test_dup_raw_value_distinguishes_empty_and_missing);
    g_test_add_func("/config/parse_invalid_keyfile",    test_parse_invalid_keyfile);
    g_test_add_func("/config/secret_from_env",          test_secret_from_env);
    g_test_add_func("/config/secret_fallback",          test_secret_fallback);
    g_test_add_func("/config/secret_plaintext_from_keyfile", test_secret_plaintext_from_keyfile);
    g_test_add_func("/config/encrypt_value_returns_enc_prefix", test_encrypt_value_returns_enc_prefix);
    g_test_add_func("/config/encrypt_random_nonce_and_roundtrip", test_encrypt_random_nonce_and_roundtrip);
    g_test_add_func("/config/decrypt_legacy_enc_prefix", test_decrypt_legacy_enc_prefix);
    g_test_add_func("/config/reload_no_crash",          test_reload_no_crash);
    g_test_add_func("/config/env_override_jwt_secret",  test_env_override_jwt_secret);
    g_test_add_func("/config/env_override_admin_user",  test_env_override_admin_user);
    g_test_add_func("/config/env_override_pool_max_conn", test_env_override_pool_max_conn);
    g_test_add_func("/config/allow_core_dumps_default_false", test_allow_core_dumps_default_false);
    g_test_add_func("/config/allow_core_dumps_env_true", test_allow_core_dumps_env_true);
    g_test_add_func("/config/ips_defaults",             test_ips_defaults);
    g_test_add_func("/config/ips_env_override",         test_ips_env_override);
    g_test_add_func("/config/ips_file_section",         test_ips_file_section);
    g_test_add_func("/config/ips_no_cross_section_bleed", test_ips_no_cross_section_bleed);
    g_test_add_func("/config/webpush_defaults",         test_webpush_defaults);
    g_test_add_func("/config/webpush_parse",            test_webpush_parse);
    g_test_add_func("/config/webpush_min_severity_invalid",
                    test_webpush_min_severity_invalid);
    g_test_add_func("/config/webpush_contact_no_scheme_dropped",
                    test_webpush_contact_no_scheme_dropped);
    g_test_add_func("/config/webpush_contact_valid_schemes_kept",
                    test_webpush_contact_valid_schemes_kept);
    g_test_add_func("/config/webpush_no_cross_section_bleed",
                    test_webpush_no_cross_section_bleed);
    g_test_add_func("/config/https/before_init_defaults_true",
                    test_https_before_init_defaults_true);
    g_test_add_func("/config/https/missing_defaults_true", test_https_missing_defaults_true);
    g_test_add_func("/config/https/explicit_false", test_https_explicit_false);
    g_test_add_func("/config/https/explicit_true", test_https_explicit_true);
    g_test_add_func("/config/https/invalid_boolean_rejected",
                    test_https_invalid_boolean_rejected);
    g_test_add_func("/config/https/external_tls_accepts_loopback",
                    test_https_external_tls_accepts_loopback);
    g_test_add_func("/config/https/external_tls_rejects_missing_bind_mode",
                    test_https_external_tls_rejects_missing_bind_mode);
    g_test_add_func("/config/https/external_tls_rejects_empty_bind_mode",
                    test_https_external_tls_rejects_empty_bind_mode);
    g_test_add_func("/config/https/external_tls_rejects_non_loopback",
                    test_https_external_tls_rejects_non_loopback);
    g_test_add_func("/config/https/validation_uses_single_reload_snapshot",
                    test_https_validation_uses_single_reload_snapshot);
}
