                                                                                          
                                                                                   
                                                                                      
                                  
                                                              
                           
                                             
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include "../src/modules/auth/pcv_rbac.h"
#include "../src/utils/pcv_jwt.h"

static gchar *g_ls_dir = NULL;
static gchar *g_ls_db  = NULL;

                                                                 
static void
ls_setup(void)
{
    g_ls_dir = g_dir_make_tmp("pcv-rbac-logout-XXXXXX", NULL);
    g_ls_db  = g_build_filename(g_ls_dir, "rbac.db", NULL);
    pcv_jwt_init("test-secret-key-for-logout-scope-32b");
    pcv_rbac_init(g_ls_db);
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("lsuser", "Str0ng!Passw0rd", PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_rbac_user_create("lsother", "Str0ng!Passw0rd", PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
}

static void
ls_teardown(void)
{
    pcv_rbac_shutdown();
    pcv_jwt_shutdown();
    gchar *wal = g_strconcat(g_ls_db, "-wal", NULL);
    gchar *shm = g_strconcat(g_ls_db, "-shm", NULL);
    g_unlink(g_ls_db); g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm);
    g_rmdir(g_ls_dir);
    g_clear_pointer(&g_ls_db, g_free);
    g_clear_pointer(&g_ls_dir, g_free);
}

                                                     
static gchar *
ls_login(const gchar *user)
{
    GError *err = NULL;
    gchar *refresh = NULL;
    g_autofree gchar *idn = NULL;                                                        
    g_assert_cmpint(pcv_rbac_lookup_subject(user, NULL, &idn), ==, PCV_USER_PRESENT);
    gchar *access = pcv_rbac_issue_tokens(user, idn, &refresh, &err);
    g_assert_no_error(err);
    g_assert_nonnull(access);
    g_assert_nonnull(refresh);
    g_free(access);
    return refresh;
}

                                                   
static gchar *
ls_rotate(const gchar *refresh)
{
    GError *err = NULL;
    gchar *next = NULL;
    gchar *access = pcv_rbac_refresh_token(refresh, &next, &err);
    g_clear_error(&err);
    if (!access) {
        g_free(next);
        return NULL;
    }
    g_free(access);
    return next;
}

                                                                    
static void
test_revokes_only_presented_session(void)
{
    ls_setup();
    gchar *r1 = ls_login("lsuser");
    gchar *r2 = ls_login("lsuser");
    gchar *owner = NULL;
    g_assert_cmpint(pcv_rbac_revoke_session_by_token(r1, "lsuser", &owner), ==, 1);
    g_assert_cmpstr(owner, ==, "lsuser");
    gchar *dead = ls_rotate(r1);
    g_assert_null(dead);
    gchar *alive = ls_rotate(r2);
    g_assert_nonnull(alive);
    g_free(owner); g_free(r1); g_free(r2); g_free(alive);
    ls_teardown();
}

                                              
static void
test_owner_mismatch_revokes_nothing(void)
{
    ls_setup();
    gchar *r = ls_login("lsother");
    gchar *owner = NULL;
    g_assert_cmpint(pcv_rbac_revoke_session_by_token(r, "lsuser", &owner), ==, 0);
    g_assert_null(owner);
    gchar *alive = ls_rotate(r);
    g_assert_nonnull(alive);
    g_free(r); g_free(alive);
    ls_teardown();
}

                                                           
static void
test_rotated_token_matches_nothing(void)
{
    ls_setup();
    gchar *r1 = ls_login("lsuser");
    gchar *r1b = ls_rotate(r1);
    g_assert_nonnull(r1b);
    g_assert_cmpint(pcv_rbac_revoke_session_by_token(r1, NULL, NULL), ==, 0);
    gchar *alive = ls_rotate(r1b);
    g_assert_nonnull(alive);
    g_free(r1); g_free(r1b); g_free(alive);
    ls_teardown();
}

                                                         
static void
test_expired_token_matches_nothing(void)
{
    ls_setup();
    gchar *r = ls_login("lsuser");
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(g_ls_db, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, "UPDATE sessions SET expires_at = 1 WHERE username = 'lsuser';",
                                 NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(db);
    g_assert_cmpint(pcv_rbac_revoke_session_by_token(r, NULL, NULL), ==, 0);
    g_free(r);
    ls_teardown();
}

                                                                              
static void
test_null_username_returns_owner(void)
{
    ls_setup();
    gchar *r = ls_login("lsother");
    gchar *owner = NULL;
    g_assert_cmpint(pcv_rbac_revoke_session_by_token(r, NULL, &owner), ==, 1);
    g_assert_cmpstr(owner, ==, "lsother");
    g_assert_cmpint(pcv_rbac_revoke_session_by_token("", NULL, NULL), ==, 0);
    g_assert_cmpint(pcv_rbac_revoke_session_by_token(NULL, NULL, NULL), ==, 0);
    g_free(owner); g_free(r);
    ls_teardown();
}

                                   
static void
test_no_db_is_error(void)
{
    ls_setup();
    gchar *r = ls_login("lsuser");
    ls_teardown();
    gchar *owner = (gchar *)"sentinel";
    g_assert_cmpint(pcv_rbac_revoke_session_by_token(r, NULL, &owner), ==, -1);
    g_assert_null(owner);
    g_free(r);
}

                                                                                
                                                                          
                                                                                   
static void
test_api_key_bearer_is_not_identity(void)
{
    ls_setup();
    gchar *r = ls_login("lsuser");
    gchar *hdr = g_strdup_printf("Bearer pcv_%08x%08x%08x%08x", g_random_int(), g_random_int(),
                                 g_random_int(), g_random_int());
    GError *err = NULL;
    gchar *sub = pcv_jwt_verify(hdr, &err);
    g_assert_null(sub);
    g_assert_nonnull(err);
    g_clear_error(&err);
    gchar *owner = NULL;
    g_assert_cmpint(pcv_rbac_revoke_session_by_token(NULL, sub, &owner), ==, 0);                   
    g_assert_null(owner);
    g_assert_cmpint(pcv_rbac_revoke_session_by_token(r, sub, &owner), ==, 1);                                    
    g_assert_cmpstr(owner, ==, "lsuser");
    gchar *dead = ls_rotate(r);
    g_assert_null(dead);
    g_free(owner); g_free(hdr); g_free(r);
    ls_teardown();
}

void
test_rbac_logout_scope_register(void)
{
    g_test_add_func("/rbac_logout_scope/revokes_only_presented_session", test_revokes_only_presented_session);
    g_test_add_func("/rbac_logout_scope/owner_mismatch_revokes_nothing", test_owner_mismatch_revokes_nothing);
    g_test_add_func("/rbac_logout_scope/rotated_token_matches_nothing",  test_rotated_token_matches_nothing);
    g_test_add_func("/rbac_logout_scope/expired_token_matches_nothing",  test_expired_token_matches_nothing);
    g_test_add_func("/rbac_logout_scope/null_username_returns_owner",    test_null_username_returns_owner);
    g_test_add_func("/rbac_logout_scope/no_db_is_error",                 test_no_db_is_error);
    g_test_add_func("/rbac_logout_scope/api_key_bearer_is_not_identity", test_api_key_bearer_is_not_identity);
}
