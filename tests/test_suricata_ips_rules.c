                                                                                         
                                                                                   
                                                                    
                                                                       
                                
                                  
  
                                                            
  
                                                                     
                                                     
  
                                                                         
                                                                  
                                                               
                                                  
                                                                         
                                                                 
   
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>                                          
#include <utime.h>                                                          
#include "modules/security/pcv_suricata_ips_rules.h"

                                          
static GHashTable *_sids(guint a, guint b) {
    GHashTable *h = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_add(h, GUINT_TO_POINTER(a));
    if (b) g_hash_table_add(h, GUINT_TO_POINTER(b));
    return h;
}

static void test_transform_whitelist_only(void) {
    const gchar *src =
        "# comment sid:2100498; not a rule\n"
        "alert tcp any any -> any any (msg:\"A\"; sid:2100498; rev:7;)\n"
        "alert tcp any any -> any any (msg:\"B\"; sid:2100499; rev:1;)\n"
        "drop tcp any any -> any any (msg:\"C\"; sid:2100500; rev:1;)\n"
        "\n";
    GHashTable *h = _sids(2100498, 2100500);                              
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 1);                                      
    g_assert_nonnull(strstr(out, "drop tcp any any -> any any (msg:\"A\""));
    g_assert_nonnull(strstr(out, "alert tcp any any -> any any (msg:\"B\""));              
    g_assert_nonnull(strstr(out, "# comment sid:2100498; not a rule"));                   
    g_assert_cmpuint(strlen(out), ==, strlen(src) - 1);                               
    g_free(out); g_hash_table_destroy(h);
}

static void test_transform_empty_set_is_identity(void) {
    const gchar *src = "alert tcp any any -> any any (sid:1; rev:1;)\n";
    GHashTable *h = g_hash_table_new(g_direct_hash, g_direct_equal);
    guint n = 99;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 0);
    g_assert_cmpstr(out, ==, src);                                        
    g_free(out); g_hash_table_destroy(h);
}

static void test_transform_no_crlf_damage(void) {
    const gchar *src = "alert tcp any any -> any any (sid:7; rev:1;)\r\n";
    GHashTable *h = _sids(7, 0); guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_nonnull(strstr(out, "\r\n"));                          
    g_free(out); g_hash_table_destroy(h);
}

                                                                
                                                  
                                    
static void test_transform_sid_substring_not_matched(void) {
    const gchar *src =
        "alert tcp any any -> any any (msg:\"D\"; sid:21004980; rev:1;)\n";
    GHashTable *h = _sids(2100498, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 0);
    g_assert_cmpstr(out, ==, src);                                     
    g_free(out); g_hash_table_destroy(h);
}

                                                       
                                                      
                                                 
                                                    
                                                
                                                               
static void test_transform_msg_field_sid_text_not_false_positive(void) {
    const gchar *src =
        "alert tcp any any -> any any "
        "(msg:\"related to sid:2100498;\"; sid:9988771; rev:1;)\n";
    GHashTable *h = _sids(2100498, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 0);
    g_assert_cmpstr(out, ==, src);                                         
    g_free(out); g_hash_table_destroy(h);
}

                                                           
                                               
static void test_transform_msg_field_sid_text_real_sid_still_matches(void) {
    const gchar *src =
        "alert tcp any any -> any any "
        "(msg:\"related to sid:2100498;\"; sid:9988771; rev:1;)\n";
    GHashTable *h = _sids(9988771, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_nonnull(strstr(out, "drop tcp any any -> any any"));
    g_free(out); g_hash_table_destroy(h);
}

                                                        
                                                
                                          
static void test_transform_reference_field_does_not_shadow_real_sid(void) {
    const gchar *src =
        "alert tcp any any -> any any "
        "(msg:\"E\"; reference:url,example.com/sid:77; sid:2100498; rev:1;)\n";
    GHashTable *h = _sids(2100498, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_nonnull(strstr(out, "drop tcp any any -> any any"));
    g_free(out); g_hash_table_destroy(h);
}

                                                          
static void test_transform_null_src_returns_null(void) {
    GHashTable *h = _sids(1, 0);
    guint n = 99;
    gchar *out = pcv_suricata_ips_rules_transform(NULL, h, &n);
    g_assert_null(out);
    g_hash_table_destroy(h);
}

                                                        
                                                            
                                                                
                                                         
                                              
                     
static void test_transform_null_drop_sids_no_crash(void) {
    const gchar *src = "alert tcp any any -> any any (sid:1; rev:1;)\n";
    guint n = 99;
    gchar *out = pcv_suricata_ips_rules_transform(src, NULL, &n);
    g_assert_cmpuint(n, ==, 0);
    g_assert_cmpstr(out, ==, src);
    g_free(out);
}

                                                           
                                                   
                                                 
                                          
                                               
                                                     
static void test_transform_semicolon_inside_quoted_value_not_matched(void) {
    const gchar *src =
        "alert tcp any any -> any any "
        "(msg:\"a; sid:2100498;\"; sid:9988771; rev:1;)\n";
    GHashTable *h = _sids(2100498, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 0);
    g_assert_cmpstr(out, ==, src);
    g_free(out); g_hash_table_destroy(h);
}

                                               
                                      
static void test_transform_semicolon_inside_quoted_value_real_sid_matches(void) {
    const gchar *src =
        "alert tcp any any -> any any "
        "(msg:\"a; sid:2100498;\"; sid:9988771; rev:1;)\n";
    GHashTable *h = _sids(9988771, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_nonnull(strstr(out, "drop tcp any any -> any any"));
    g_free(out); g_hash_table_destroy(h);
}

                                                     
                                                  
                                                    
                                                  
static void test_transform_malformed_sid_option_skipped_continues(void) {
    const gchar *src =
        "alert tcp any any -> any any "
        "(msg:\"x\"; sid:; sid:2100498; rev:1;)\n";
    GHashTable *h = _sids(2100498, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_nonnull(strstr(out, "drop tcp any any -> any any"));
    g_free(out); g_hash_table_destroy(h);
}

                                             
                                                    
                                        
static void test_transform_quoted_value_escaped_quote_not_closing(void) {
    const gchar *src =
        "alert tcp any any -> any any "
        "(msg:\"quote \\\" and ; inside\"; sid:2100498; rev:1;)\n";
    GHashTable *h = _sids(2100498, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_nonnull(strstr(out, "drop tcp any any -> any any"));
    g_free(out); g_hash_table_destroy(h);
}

                                                 
                          
static void test_transform_flag_option_before_sid_option(void) {
    const gchar *src =
        "alert tcp any any -> any any (noalert; sid:2100498; rev:1;)\n";
    GHashTable *h = _sids(2100498, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_nonnull(strstr(out, "drop tcp any any -> any any"));
    g_free(out); g_hash_table_destroy(h);
}

                                                   
                    
static void test_transform_no_paren_line_unchanged(void) {
    const gchar *src = "alert tcp any any -> any any sid:2100498; rev:1;\n";
    GHashTable *h = _sids(2100498, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 0);
    g_assert_cmpstr(out, ==, src);
    g_free(out); g_hash_table_destroy(h);
}

                                              
                                           
static void test_transform_unterminated_quote_no_crash(void) {
    const gchar *src =
        "alert tcp any any -> any any "
        "(msg:\"unterminated sid:2100498; rev:1;\n";
    GHashTable *h = _sids(2100498, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 0);
    g_assert_cmpstr(out, ==, src);
    g_free(out); g_hash_table_destroy(h);
}

                                                  
                                                    
static void test_transform_missing_closing_paren_still_matches(void) {
    const gchar *src =
        "alert tcp any any -> any any (sid:2100498; rev:1;\n";
    GHashTable *h = _sids(2100498, 0);
    guint n = 0;
    gchar *out = pcv_suricata_ips_rules_transform(src, h, &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_nonnull(strstr(out, "drop tcp any any -> any any"));
    g_free(out); g_hash_table_destroy(h);
}

                                                             
  
                                                      
                                                
                                                        
                   

#define T2_RELOAD_SEQ_MAX 4

static gboolean g_t2_validate_ok = TRUE;                        
static gint     g_t2_validate_calls = 0;                          
static gboolean g_t2_reload_seq[T2_RELOAD_SEQ_MAX];                        
static gint     g_t2_reload_calls = 0;                          

static void _t2_reset(void) {
    g_t2_validate_ok = TRUE;
    g_t2_validate_calls = 0;
    g_t2_reload_calls = 0;
    for (guint i = 0; i < G_N_ELEMENTS(g_t2_reload_seq); i++)
        g_t2_reload_seq[i] = TRUE;
}

static gboolean _t2_mock_validate(const gchar *rules_path, GError **error) {
    g_t2_validate_calls++;
    g_assert_nonnull(rules_path);
                                                    
                                                   
    g_assert_true(g_str_has_suffix(rules_path, ".tmp"));
    if (!g_t2_validate_ok) {
        g_set_error(error, g_quark_from_static_string("t2-mock"), 1, "mock validate failure");
        return FALSE;
    }
    return TRUE;
}

static gboolean _t2_mock_reload(GError **error) {
    gint idx = g_t2_reload_calls++;
    gboolean ok = (idx < T2_RELOAD_SEQ_MAX) ? g_t2_reload_seq[idx] : TRUE;
    if (!ok) {
        g_set_error(error, g_quark_from_static_string("t2-mock"), 2, "mock reload failure");
    }
    return ok;
}

                                                
static const gchar *T2_SRC_RULES =
    "# comment\n"
    "alert tcp any any -> any any (msg:\"A\"; sid:2100498; rev:7;)\n"
    "alert tcp any any -> any any (msg:\"B\"; sid:2100499; rev:1;)\n";

                                   
static guint32 _t2_mode(const gchar *path) {
    GStatBuf st;
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    return (guint32)(st.st_mode & 07777);
}

                                                         
                                      
  
                                                      
                                                           
                                                           
                                               
                         
                                                         
                                                
                                     
static void test_apply_happy(void) {
    _t2_reset();
    gchar *dir = g_dir_make_tmp("pcv-ips-rules-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *src_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *out_path = g_build_filename(dir, "suricata-ips.rules", NULL);
    gchar *tmp_path = g_strdup_printf("%s.tmp", out_path);
    gchar *bak_path = g_strdup_printf("%s.bak", out_path);
    g_assert_true(g_file_set_contents(src_path, T2_SRC_RULES, -1, NULL));
    g_assert_true(g_file_set_contents(out_path, "old-derived\n", -1, NULL));
    g_assert_cmpint(g_chmod(out_path, 0644), ==, 0);                         

    GHashTable *h = _sids(2100498, 0);
    PcvIpsRulesHooks hooks = { _t2_mock_validate, _t2_mock_reload };
    GError *err = NULL;
    mode_t old_umask = umask(0077);                        
    gboolean ok = pcv_suricata_ips_rules_apply_at(src_path, out_path, h, "tester", &hooks, &err);
    umask(old_umask);
    g_assert_no_error(err);
    g_assert_true(ok);

                                                  
                                
    guint n = 0;
    gchar *expected = pcv_suricata_ips_rules_transform(T2_SRC_RULES, h, &n);
    g_assert_cmpuint(n, ==, 1);
    gchar *content = NULL;
    g_assert_true(g_file_get_contents(out_path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, expected);
    g_assert_nonnull(strstr(content, "drop tcp any any -> any any (msg:\"A\""));
    g_assert_nonnull(strstr(content, "alert tcp any any -> any any (msg:\"B\""));              
    g_free(content); g_free(expected);

                                                                       
    g_assert_cmphex(_t2_mode(out_path), ==, 0644);

                                                
                                                       
    g_assert_true(g_file_test(bak_path, G_FILE_TEST_EXISTS));
    gchar *bak_content = NULL;
    g_assert_true(g_file_get_contents(bak_path, &bak_content, NULL, NULL));
    g_assert_cmpstr(bak_content, ==, "old-derived\n");
    g_free(bak_content);
    g_assert_cmphex(_t2_mode(bak_path), ==, 0644);

    g_assert_cmpint(g_t2_validate_calls, ==, 1);
    g_assert_cmpint(g_t2_reload_calls, ==, 1);
    g_assert_false(g_file_test(tmp_path, G_FILE_TEST_EXISTS));                        

                                    
    gchar *src_after = NULL;
    g_assert_true(g_file_get_contents(src_path, &src_after, NULL, NULL));
    g_assert_cmpstr(src_after, ==, T2_SRC_RULES);
    g_free(src_after);

    g_remove(src_path); g_remove(out_path); g_remove(bak_path); g_rmdir(dir);
    g_free(src_path); g_free(out_path); g_free(tmp_path); g_free(bak_path); g_free(dir);
    g_hash_table_destroy(h);
}

                                                                             
static void test_apply_validate_fail_rollback(void) {
    _t2_reset();
    g_t2_validate_ok = FALSE;
    gchar *dir = g_dir_make_tmp("pcv-ips-rules-XXXXXX", NULL);
    gchar *src_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *out_path = g_build_filename(dir, "suricata-ips.rules", NULL);
    gchar *tmp_path = g_strdup_printf("%s.tmp", out_path);
    gchar *bak_path = g_strdup_printf("%s.bak", out_path);
    g_assert_true(g_file_set_contents(src_path, T2_SRC_RULES, -1, NULL));
    g_assert_true(g_file_set_contents(out_path, "old-derived\n", -1, NULL));

    GHashTable *h = _sids(2100498, 0);
    PcvIpsRulesHooks hooks = { _t2_mock_validate, _t2_mock_reload };
    GError *err = NULL;
    gboolean ok = pcv_suricata_ips_rules_apply_at(src_path, out_path, h, NULL, &hooks, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);

    gchar *content = NULL;
    g_assert_true(g_file_get_contents(out_path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "old-derived\n");                
    g_free(content);
    g_assert_cmpint(g_t2_validate_calls, ==, 1);
    g_assert_cmpint(g_t2_reload_calls, ==, 0);                             
    g_assert_false(g_file_test(tmp_path, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(bak_path, G_FILE_TEST_EXISTS));                     

    g_remove(src_path); g_remove(out_path); g_rmdir(dir);
    g_free(src_path); g_free(out_path); g_free(tmp_path); g_free(bak_path); g_free(dir);
    g_hash_table_destroy(h);
}

                                                       
static void test_apply_reload_fail_rollback(void) {
    _t2_reset();
    g_t2_reload_seq[0] = FALSE;                     
    g_t2_reload_seq[1] = TRUE;                     
    gchar *dir = g_dir_make_tmp("pcv-ips-rules-XXXXXX", NULL);
    gchar *src_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *out_path = g_build_filename(dir, "suricata-ips.rules", NULL);
    gchar *tmp_path = g_strdup_printf("%s.tmp", out_path);
    gchar *bak_path = g_strdup_printf("%s.bak", out_path);
    g_assert_true(g_file_set_contents(src_path, T2_SRC_RULES, -1, NULL));
    g_assert_true(g_file_set_contents(out_path, "old-derived\n", -1, NULL));
    g_assert_cmpint(g_chmod(out_path, 0644), ==, 0);

    GHashTable *h = _sids(2100498, 0);
    PcvIpsRulesHooks hooks = { _t2_mock_validate, _t2_mock_reload };
    GError *err = NULL;
    mode_t old_umask = umask(0077);                        
    gboolean ok = pcv_suricata_ips_rules_apply_at(src_path, out_path, h, "tester", &hooks, &err);
    umask(old_umask);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);

    gchar *content = NULL;
    g_assert_true(g_file_get_contents(out_path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "old-derived\n");                   
    g_free(content);
                                                         
                                                         
    g_assert_cmphex(_t2_mode(out_path), ==, 0644);
    g_assert_cmpint(g_t2_reload_calls, ==, 2);                        
    g_assert_false(g_file_test(tmp_path, G_FILE_TEST_EXISTS));
                                                                
    g_assert_false(g_file_test(bak_path, G_FILE_TEST_EXISTS));

    g_remove(src_path); g_remove(out_path); g_rmdir(dir);
    g_free(src_path); g_free(out_path); g_free(tmp_path); g_free(bak_path); g_free(dir);
    g_hash_table_destroy(h);
}

                                                      
static void test_apply_missing_src(void) {
    _t2_reset();
    gchar *dir = g_dir_make_tmp("pcv-ips-rules-XXXXXX", NULL);
    gchar *src_path = g_build_filename(dir, "no-such.rules", NULL);
    gchar *out_path = g_build_filename(dir, "suricata-ips.rules", NULL);
    gchar *tmp_path = g_strdup_printf("%s.tmp", out_path);
    g_assert_true(g_file_set_contents(out_path, "old-derived\n", -1, NULL));

    GHashTable *h = _sids(2100498, 0);
    PcvIpsRulesHooks hooks = { _t2_mock_validate, _t2_mock_reload };
    GError *err = NULL;
    gboolean ok = pcv_suricata_ips_rules_apply_at(src_path, out_path, h, NULL, &hooks, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);

    gchar *content = NULL;
    g_assert_true(g_file_get_contents(out_path, &content, NULL, NULL));
    g_assert_cmpstr(content, ==, "old-derived\n");
    g_free(content);
    g_assert_cmpint(g_t2_validate_calls, ==, 0);
    g_assert_cmpint(g_t2_reload_calls, ==, 0);
    g_assert_false(g_file_test(tmp_path, G_FILE_TEST_EXISTS));

    g_remove(out_path); g_rmdir(dir);
    g_free(src_path); g_free(out_path); g_free(tmp_path); g_free(dir);
    g_hash_table_destroy(h);
}

                                                                
                                                           
                                            
static void test_apply_first_install_inactive(void) {
    _t2_reset();                                                           
    gchar *dir = g_dir_make_tmp("pcv-ips-rules-XXXXXX", NULL);
    gchar *src_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *out_path = g_build_filename(dir, "suricata-ips.rules", NULL);
    gchar *bak_path = g_strdup_printf("%s.bak", out_path);
    g_assert_true(g_file_set_contents(src_path, T2_SRC_RULES, -1, NULL));
    g_assert_false(g_file_test(out_path, G_FILE_TEST_EXISTS));                 

    GHashTable *h = _sids(2100498, 0);
    PcvIpsRulesHooks hooks = { _t2_mock_validate, _t2_mock_reload };
    GError *err = NULL;
    mode_t old_umask = umask(0077);                                  
    gboolean ok = pcv_suricata_ips_rules_apply_at(src_path, out_path, h, NULL, &hooks, &err);
    umask(old_umask);
    g_assert_no_error(err);
    g_assert_true(ok);
    g_assert_true(g_file_test(out_path, G_FILE_TEST_EXISTS));
                                                         
    g_assert_cmphex(_t2_mode(out_path), ==, 0644);
    g_assert_false(g_file_test(bak_path, G_FILE_TEST_EXISTS));                   
    g_assert_cmpint(g_t2_reload_calls, ==, 1);

    gchar *content = NULL;
    g_assert_true(g_file_get_contents(out_path, &content, NULL, NULL));
    g_assert_nonnull(strstr(content, "drop tcp any any -> any any (msg:\"A\""));
    g_free(content);

    g_remove(src_path); g_remove(out_path); g_rmdir(dir);
    g_free(src_path); g_free(out_path); g_free(bak_path); g_free(dir);
    g_hash_table_destroy(h);
}

                                            
  
                                               
                                                          
                                                    
  
                            
                                               
                                 
                                                    
                                                      
static void test_rules_stale_detects_external_src_change(void) {
    gchar *dir = g_dir_make_tmp("pcv-ips-stale-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *src_path = g_build_filename(dir, "suricata.rules", NULL);
    gchar *out_path = g_build_filename(dir, "suricata-ips.rules", NULL);

                            
    g_assert_true(g_file_set_contents(src_path, T2_SRC_RULES, -1, NULL));
    g_assert_false(pcv_suricata_ips_rules_stale_at(src_path, out_path));

                                                   
                                         
    g_assert_true(g_file_set_contents(out_path, "derived\n", -1, NULL));
    GStatBuf st;
    g_assert_cmpint(g_stat(src_path, &st), ==, 0);
    struct utimbuf newer = { .actime = st.st_mtime + 10, .modtime = st.st_mtime + 10 };
    g_assert_cmpint(g_utime(out_path, &newer), ==, 0);
    g_assert_false(pcv_suricata_ips_rules_stale_at(src_path, out_path));

                                          
    struct utimbuf newest = { .actime = st.st_mtime + 20, .modtime = st.st_mtime + 20 };
    g_assert_cmpint(g_utime(src_path, &newest), ==, 0);
    g_assert_true(pcv_suricata_ips_rules_stale_at(src_path, out_path));

                                 
    g_remove(src_path);
    g_assert_false(pcv_suricata_ips_rules_stale_at(src_path, out_path));

    g_remove(out_path); g_rmdir(dir);
    g_free(src_path); g_free(out_path); g_free(dir);
}

void test_suricata_ips_rules_register(void) {
    g_test_add_func("/suricata_ips_rules/transform_whitelist_only", test_transform_whitelist_only);
    g_test_add_func("/suricata_ips_rules/transform_empty_identity", test_transform_empty_set_is_identity);
    g_test_add_func("/suricata_ips_rules/transform_crlf", test_transform_no_crlf_damage);
    g_test_add_func("/suricata_ips_rules/transform_sid_substring", test_transform_sid_substring_not_matched);
    g_test_add_func("/suricata_ips_rules/transform_msg_field_no_false_positive",
                    test_transform_msg_field_sid_text_not_false_positive);
    g_test_add_func("/suricata_ips_rules/transform_msg_field_real_sid_matches",
                    test_transform_msg_field_sid_text_real_sid_still_matches);
    g_test_add_func("/suricata_ips_rules/transform_reference_field_no_shadow",
                    test_transform_reference_field_does_not_shadow_real_sid);
    g_test_add_func("/suricata_ips_rules/transform_null_src", test_transform_null_src_returns_null);
    g_test_add_func("/suricata_ips_rules/transform_null_drop_sids", test_transform_null_drop_sids_no_crash);
    g_test_add_func("/suricata_ips_rules/transform_semicolon_in_quote_not_matched",
                    test_transform_semicolon_inside_quoted_value_not_matched);
    g_test_add_func("/suricata_ips_rules/transform_semicolon_in_quote_real_sid_matches",
                    test_transform_semicolon_inside_quoted_value_real_sid_matches);
    g_test_add_func("/suricata_ips_rules/transform_malformed_sid_skipped_continues",
                    test_transform_malformed_sid_option_skipped_continues);
    g_test_add_func("/suricata_ips_rules/transform_escaped_quote_not_closing",
                    test_transform_quoted_value_escaped_quote_not_closing);
    g_test_add_func("/suricata_ips_rules/transform_flag_option_before_sid",
                    test_transform_flag_option_before_sid_option);
    g_test_add_func("/suricata_ips_rules/transform_no_paren_line_unchanged",
                    test_transform_no_paren_line_unchanged);
    g_test_add_func("/suricata_ips_rules/transform_unterminated_quote_no_crash",
                    test_transform_unterminated_quote_no_crash);
    g_test_add_func("/suricata_ips_rules/transform_missing_closing_paren_still_matches",
                    test_transform_missing_closing_paren_still_matches);
                                         
    g_test_add_func("/suricata_ips_rules/apply_happy", test_apply_happy);
    g_test_add_func("/suricata_ips_rules/apply_validate_fail_rollback",
                    test_apply_validate_fail_rollback);
    g_test_add_func("/suricata_ips_rules/apply_reload_fail_rollback",
                    test_apply_reload_fail_rollback);
    g_test_add_func("/suricata_ips_rules/apply_missing_src", test_apply_missing_src);
    g_test_add_func("/suricata_ips_rules/apply_first_install_inactive",
                    test_apply_first_install_inactive);
    g_test_add_func("/suricata_ips_rules/stale_detects_external_src_change",
                    test_rules_stale_detects_external_src_change);
}
