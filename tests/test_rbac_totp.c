                                                                                       
                                                                                                 
                                                               
                                                                
                                 
                         
  
                                                           
                                                              
                                                                       
  
                 
                                                                  
                                                            
                                                   
                                                         
                                                                        
                                                                 
                                                                      
                                                
  
                                                    
                                                
                                                
                                                             
                            
  
                                                 
                                                                                
                                                
  
                                       
  
                                                
   
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../src/modules/auth/pcv_rbac.h"
#include "../src/utils/pcv_secure.h"
#include "../src/utils/pcv_totp.h"
#include "../src/utils/pcv_jwt.h"

                                                                      
                                                              
                                                             
void     pcv_dispatcher_init_policy_map(void);
gboolean pcv_dispatcher_check_rbac(const gchar *method, gint caller_role);

                                                              
static gchar *g_tmpdir = NULL;
static gchar *g_dbpath = NULL;

                                                        
static void
totp_setup_first(gpointer *fixture __attribute__((unused)),
                 gconstpointer data __attribute__((unused)))
{
    g_tmpdir = g_dir_make_tmp("pcv-rbac-totp-XXXXXX", NULL);
    g_dbpath = g_build_filename(g_tmpdir, "rbac.db", NULL);
    pcv_rbac_init(g_dbpath);

    GError *err = NULL;
    gboolean ok = pcv_rbac_user_create("tuser", "pw", PCV_ROLE_VIEWER, NULL, &err);
    g_assert_true(ok);
    g_assert_no_error(err);
}

                                                     
static void
totp_noop(gpointer *fixture __attribute__((unused)),
          gconstpointer data __attribute__((unused)))
{
}

                                                        
static void
totp_teardown_last(gpointer *fixture __attribute__((unused)),
                   gconstpointer data __attribute__((unused)))
{
    pcv_rbac_shutdown();
    if (g_dbpath) {
        gchar *wal = g_strconcat(g_dbpath, "-wal", NULL);
        gchar *shm = g_strconcat(g_dbpath, "-shm", NULL);
        g_unlink(g_dbpath); g_unlink(wal); g_unlink(shm);
        g_free(wal); g_free(shm); g_free(g_dbpath); g_dbpath = NULL;
    }
    if (g_tmpdir) { g_rmdir(g_tmpdir); g_free(g_tmpdir); g_tmpdir = NULL; }
}

                                                           

                                                     
static void
test_totp_enroll_confirm_flow(gpointer *f, gconstpointer d)
{
    (void)f; (void)d;
    GError *err = NULL;
    PcvTotpStatus st;
                
    g_assert_true(pcv_rbac_totp_status("tuser", &st));
    g_assert_false(st.enrolled);
                      
    gchar *b32 = pcv_rbac_totp_enroll("tuser", &err);
    g_assert_no_error(err); g_assert_nonnull(b32);
    pcv_rbac_totp_status("tuser", &st);
    g_assert_true(st.enrolled); g_assert_false(st.confirmed);
                             
    guchar key[64]; gsize klen; pcv_totp_base32_decode(b32, key, &klen);
    gchar code[8];
    g_snprintf(code, sizeof code, "%06u",
               pcv_totp_code_at(key, klen, g_get_real_time()/G_USEC_PER_SEC/30, 6));
    gboolean first = FALSE;
    g_assert_true(pcv_rbac_totp_verify_code("tuser", code, FALSE, &first, &err));
    g_assert_true(first);
    pcv_rbac_totp_status("tuser", &st);
    g_assert_true(st.confirmed);
                                
    g_assert_false(pcv_rbac_totp_verify_code("tuser", code, FALSE, &first, &err));
    g_clear_error(&err);
    g_free(b32);
}

                                                             
static void
test_totp_recovery_codes(gpointer *f, gconstpointer d)
{
    (void)f; (void)d;
    GError *err = NULL;
    GPtrArray *codes = pcv_rbac_totp_generate_recovery("tuser", &err);
    g_assert_no_error(err);
    g_assert_cmpuint(codes->len, ==, 10);
    const gchar *c0 = g_ptr_array_index(codes, 0);
    g_assert_cmpuint(strlen(c0), ==, 11);                             
    gboolean first = FALSE;
                        
    g_assert_true(pcv_rbac_totp_verify_code("tuser", c0, TRUE, &first, &err));
                      
    g_assert_false(pcv_rbac_totp_verify_code("tuser", c0, TRUE, &first, &err));
    g_clear_error(&err);
                                            
    const gchar *c1 = g_ptr_array_index(codes, 1);
    g_assert_false(pcv_rbac_totp_verify_code("tuser", c1, FALSE, &first, &err));
    g_clear_error(&err);
    PcvTotpStatus st; pcv_rbac_totp_status("tuser", &st);
    g_assert_cmpint(st.recovery_remaining, ==, 9);
    g_ptr_array_unref(codes);
}

                                                      
static void
test_totp_brute_lockout(gpointer *f, gconstpointer d)
{
    (void)f; (void)d;
    GError *err = NULL;
    gboolean first;
    for (int i = 0; i < 5; i++) {
        g_assert_false(pcv_rbac_totp_verify_code("tuser", "000000", FALSE, &first, &err));
        g_clear_error(&err);
    }
                                                                
    g_assert_true(pcv_rbac_totp_get_remaining_lockout("tuser") > 0);
    g_assert_cmpint(pcv_rbac_totp_get_remaining_lockout("tuser"), >, 0);
}

                                                          
static void
test_totp_disable(gpointer *f, gconstpointer d)
{
    (void)f; (void)d;
    GError *err = NULL;
    g_assert_true(pcv_rbac_totp_disable("tuser", &err));
    PcvTotpStatus st; pcv_rbac_totp_status("tuser", &st);
    g_assert_false(st.enrolled);
    g_assert_cmpint(st.recovery_remaining, ==, 0);
}

                                                                           
                                                        
                                                               
                                                               
static gchar *g_pc_tmpdir = NULL;
static gchar *g_pc_dbpath = NULL;

static void
pc_setup(void)
{
    g_pc_tmpdir = g_dir_make_tmp("pcv-rbac-passcheck-XXXXXX", NULL);
    g_pc_dbpath = g_build_filename(g_pc_tmpdir, "rbac.db", NULL);
    pcv_jwt_init("test-secret-key-for-password-check-32b");
    pcv_rbac_init(g_pc_dbpath);
}

static void
pc_teardown(void)
{
    pcv_rbac_shutdown();
    pcv_jwt_shutdown();
    if (g_pc_dbpath) {
        gchar *wal = g_strconcat(g_pc_dbpath, "-wal", NULL);
        gchar *shm = g_strconcat(g_pc_dbpath, "-shm", NULL);
        g_unlink(g_pc_dbpath); g_unlink(wal); g_unlink(shm);
        g_free(wal); g_free(shm); g_free(g_pc_dbpath); g_pc_dbpath = NULL;
    }
    if (g_pc_tmpdir) { g_rmdir(g_pc_tmpdir); g_free(g_pc_tmpdir); g_pc_tmpdir = NULL; }
}

                                                
                                                        
                                               
static void
test_user_list_includes_totp_state(void)
{
    pc_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("list_totp_user", "pw",
                                       PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_rbac_user_create("list_plain_user", "pw",
                                       PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_rbac_user_create("list_pending_user", "pw",
                                       PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);

    gchar *b32 = pcv_rbac_totp_enroll("list_totp_user", &err);
    g_assert_nonnull(b32);
    g_assert_no_error(err);
    guchar key[64];
    gsize klen = 0;
    g_assert_true(pcv_totp_base32_decode(b32, key, &klen));
    gchar code[8];
    g_snprintf(code, sizeof code, "%06u",
               pcv_totp_code_at(key, klen,
                   g_get_real_time() / G_USEC_PER_SEC / 30, 6));
    gboolean first = FALSE;
    g_assert_true(pcv_rbac_totp_verify_code(
        "list_totp_user", code, FALSE, &first, &err));
    g_assert_no_error(err);
    gchar *pending_b32 = pcv_rbac_totp_enroll("list_pending_user", &err);
    g_assert_nonnull(pending_b32);
    g_assert_no_error(err);

    GPtrArray *users = pcv_rbac_user_list();
    PcvUser *found = NULL;
    PcvUser *plain = NULL;
    PcvUser *pending = NULL;
    for (guint i = 0; i < users->len; i++) {
        PcvUser *u = g_ptr_array_index(users, i);
        if (g_strcmp0(u->username, "list_totp_user") == 0) {
            found = u;
        } else if (g_strcmp0(u->username, "list_plain_user") == 0) {
            plain = u;
        } else if (g_strcmp0(u->username, "list_pending_user") == 0) {
            pending = u;
        }
    }

    g_assert_nonnull(found);
    g_assert_true(found->totp_enrolled);
    g_assert_true(found->totp_confirmed);
    g_assert_nonnull(plain);
    g_assert_false(plain->totp_enrolled);
    g_assert_false(plain->totp_confirmed);
    g_assert_nonnull(pending);
    g_assert_true(pending->totp_enrolled);
    g_assert_false(pending->totp_confirmed);

    g_ptr_array_unref(users);
    pcv_secure_wipe(key, sizeof key);
    pcv_secure_free_str(&b32);
    pcv_secure_free_str(&pending_b32);
    pc_teardown();
}

                                                                 
                                                                  
                                                             
static void
test_password_check_success_and_lockout(void)
{
    pc_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("pcuser", "Str0ng!Passw0rd",
                                       PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);

                                                
    g_assert_true(pcv_rbac_password_check("pcuser", "Str0ng!Passw0rd", &err));
    g_assert_no_error(err);

                                                             
                     
    for (int i = 0; i < 5; i++) {
        g_assert_false(pcv_rbac_password_check("pcuser", "wrong-password", &err));
        g_clear_error(&err);
    }
    g_assert_true(pcv_rbac_is_locked("pcuser"));
    g_assert_cmpint(pcv_rbac_get_remaining_lockout("pcuser"), >, 0);

    pc_teardown();
}

                                                          
                                                                  
                                                           
                                                      
static void
test_issue_tokens_stores_session_for_refresh(void)
{
    pc_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("ituser", "Str0ng!Passw0rd",
                                       PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_rbac_password_check("ituser", "Str0ng!Passw0rd", &err));
    g_assert_no_error(err);

    gchar *refresh = NULL;
    gchar *access = pcv_rbac_issue_tokens("ituser", &refresh, &err);
    g_assert_nonnull(access);
    g_assert_nonnull(refresh);
    g_assert_no_error(err);

                                                  
    gchar *new_refresh = NULL;
    gchar *new_access = pcv_rbac_refresh_token(refresh, &new_refresh, &err);
    g_assert_nonnull(new_access);
    g_assert_no_error(err);

    g_free(access); g_free(refresh); g_free(new_access); g_free(new_refresh);
    pc_teardown();
}

                                                                   
                                                                     
                                                        
                                                             
                                                  
static void
test_issue_tokens_rejects_nonexistent_user(void)
{
    pc_setup();
    GError *err = NULL;

    gchar *refresh = NULL;
    gchar *access = pcv_rbac_issue_tokens("ghost-user", &refresh, &err);
    g_assert_null(access);
    g_assert_null(refresh);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);

                                                 
    g_assert_false(pcv_rbac_is_locked("ghost-user"));

    pc_teardown();
}

                                                                       
                                                  
                                                       
                                                          
                                                            
                                                                         
                                                 
                                                               
                                                   
          
static void
test_totp_rbac_policy(void)
{
                                                       
    pcv_dispatcher_init_policy_map();

    static const struct { const gchar *method; gint min_role; } TBL[] = {
        { "auth.totp.status",              0 },                      
        { "auth.totp.disable",             0 },                               
        { "auth.totp.recovery.regenerate", 0 },                                   
        { "auth.totp.reset",               2 },                         
    };

    for (gsize i = 0; i < G_N_ELEMENTS(TBL); i++) {
        for (gint role = 0; role <= 2; role++) {
            gboolean expect = (role >= TBL[i].min_role);
            g_assert_cmpint(pcv_dispatcher_check_rbac(TBL[i].method, role),
                            ==, expect);
        }
    }

                                                          
    g_assert_false(pcv_dispatcher_check_rbac("auth.totp.reset", 1));
}

                                                                 
                                                        
                                                     
                                                         
static void
test_totp_rbac_rest_gate_agreement(void)
{
    pc_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("totp_viewer", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);

                                                         
    g_assert_true(pcv_rbac_check_permission("totp_viewer", "auth.totp.status"));
    g_assert_true(pcv_rbac_check_permission("totp_viewer", "auth.totp.disable"));
    g_assert_true(pcv_rbac_check_permission("totp_viewer", "auth.totp.recovery.regenerate"));

                                                                    
    g_assert_false(pcv_rbac_check_permission("totp_viewer", "auth.totp.reset"));

                                                             
    pcv_dispatcher_init_policy_map();
    g_assert_true(pcv_dispatcher_check_rbac("auth.totp.status", PCV_ROLE_VIEWER));
    g_assert_true(pcv_dispatcher_check_rbac("auth.totp.disable", PCV_ROLE_VIEWER));
    g_assert_true(pcv_dispatcher_check_rbac("auth.totp.recovery.regenerate", PCV_ROLE_VIEWER));
    g_assert_false(pcv_dispatcher_check_rbac("auth.totp.reset", PCV_ROLE_VIEWER));

    pc_teardown();
}

void
test_rbac_totp_register(void)
{
    g_test_add("/rbac_totp/enroll_confirm_flow", gpointer, NULL,
               totp_setup_first, test_totp_enroll_confirm_flow, totp_noop);
    g_test_add("/rbac_totp/recovery_codes", gpointer, NULL,
               totp_noop, test_totp_recovery_codes, totp_noop);
    g_test_add("/rbac_totp/brute_lockout", gpointer, NULL,
               totp_noop, test_totp_brute_lockout, totp_noop);
    g_test_add("/rbac_totp/disable", gpointer, NULL,
               totp_noop, test_totp_disable, totp_teardown_last);

                                            
    g_test_add_func("/rbac_totp/user_list_includes_totp_state",
                    test_user_list_includes_totp_state);

                                                                      
    g_test_add_func("/rbac_totp/password_check/success_and_lockout",
                    test_password_check_success_and_lockout);
    g_test_add_func("/rbac_totp/issue_tokens/stores_session_for_refresh",
                    test_issue_tokens_stores_session_for_refresh);
    g_test_add_func("/rbac_totp/issue_tokens/rejects_nonexistent_user",
                    test_issue_tokens_rejects_nonexistent_user);

                                                                         
    g_test_add_func("/rbac_totp/rbac_policy", test_totp_rbac_policy);
    g_test_add_func("/rbac_totp/rbac_rest_gate_agreement",
                    test_totp_rbac_rest_gate_agreement);
}
