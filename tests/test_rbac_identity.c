                                                                                                        
                                                                                         
                                                                                               
                                                                                    
                    
                                                                      
                                              
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

#include "../src/modules/auth/pcv_rbac.h"
#include "../src/utils/pcv_jwt.h"
#include "../src/utils/pcv_secure.h"
#include "modules/dispatcher/handler_auth.h"

                                                                         
void pcv_rbac_identity_last_migration_for_test(gint *filled, gint *revoked, gint *downgraded);
                                                                       
const gchar *pcv_test_ws_closed_subject(gint *calls);

static gchar *g_dir = NULL;
static gchar *g_db = NULL;

static void
id_setup(void)
{
    g_dir = g_dir_make_tmp("pcv-rbac-identity-XXXXXX", NULL);
    g_db = g_build_filename(g_dir, "rbac.db", NULL);
    pcv_jwt_init("rbac-identity-test-secret-32-bytes!!");
    pcv_rbac_init(g_db);
}

static void
id_teardown(void)
{
    pcv_rbac_shutdown();
    pcv_jwt_shutdown();
    gchar *wal = g_strconcat(g_db, "-wal", NULL), *shm = g_strconcat(g_db, "-shm", NULL);
    g_unlink(g_db); g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm);
    g_rmdir(g_dir);
    g_clear_pointer(&g_db, g_free);
    g_clear_pointer(&g_dir, g_free);
}

                                                     
static void
raw_exec(const gchar *sql)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(g_db, &db), ==, SQLITE_OK);
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) g_error("raw_exec failed: %s", err ? err : "?");
    sqlite3_close(db);
}

                                   
static gint64
raw_int(const gchar *sql, const gchar *arg)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_open(g_db, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_prepare_v2(db, sql, -1, &st, NULL), ==, SQLITE_OK);
    if (arg) sqlite3_bind_text(st, 1, arg, -1, SQLITE_TRANSIENT);
    gint64 v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

                                          
static gchar *
user_nonce(const gchar *user)
{
    gchar *n = NULL;
    g_assert_cmpint(pcv_rbac_lookup_subject(user, NULL, &n), ==, PCV_USER_PRESENT);
    return n;
}

static gboolean
is_hex32(const gchar *s)
{
    if (!s || strlen(s) != 32) return FALSE;
    for (const gchar *p = s; *p; p++) if (!g_ascii_isxdigit(*p)) return FALSE;
    return TRUE;
}

                                                                         
static gchar *
login(const gchar *user, gchar **out_access)
{
    GError *err = NULL;
    gchar *refresh = NULL;
    g_autofree gchar *idn = user_nonce(user);
    gchar *access = pcv_rbac_issue_tokens(user, idn, &refresh, &err);
    g_assert_no_error(err);
    g_assert_nonnull(access);
    g_assert_nonnull(refresh);
    if (out_access) *out_access = access; else g_free(access);
    return refresh;
}

                                                                                            
static void
test_identity_lookup_three_states(void)
{
    id_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("op1", "pw", PCV_ROLE_OPERATOR, NULL, &err));
    g_assert_no_error(err);
    PcvRole role = PCV_ROLE_ADMIN;
    gchar *nonce = NULL;
    g_assert_cmpint(pcv_rbac_lookup_subject("op1", &role, &nonce), ==, PCV_USER_PRESENT);
    g_assert_cmpint(role, ==, PCV_ROLE_OPERATOR);
    g_assert_true(is_hex32(nonce));
    g_free(nonce);
    nonce = (gchar *)0x1;
    g_assert_cmpint(pcv_rbac_lookup_subject("nobody", &role, &nonce), ==, PCV_USER_ABSENT);
    g_assert_null(nonce);
    g_assert_cmpint(role, ==, PCV_ROLE_VIEWER);
    g_assert_cmpint(pcv_rbac_lookup_subject("", NULL, NULL), ==, PCV_USER_ABSENT);
    g_assert_cmpint(pcv_rbac_user_exists("op1"), ==, PCV_USER_PRESENT);
    raw_exec("DROP TABLE users;");
    g_assert_cmpint(pcv_rbac_lookup_subject("op1", &role, &nonce), ==, PCV_USER_UNKNOWN);
    g_assert_null(nonce);
    g_assert_cmpint(pcv_rbac_user_exists("op1"), ==, PCV_USER_UNKNOWN);
    id_teardown();
}

                                                             
static void
test_identity_create_and_recreate_nonce(void)
{
    id_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("alice", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_assert_true(pcv_rbac_user_create("bob", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_autofree gchar *a1 = user_nonce("alice");
    g_autofree gchar *b1 = user_nonce("bob");
    g_assert_true(is_hex32(a1));
    g_assert_cmpstr(a1, !=, b1);
    g_assert_true(pcv_rbac_user_delete("alice", &err));
    g_assert_no_error(err);
    g_assert_true(pcv_rbac_user_create("alice", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_autofree gchar *a2 = user_nonce("alice");
    g_assert_true(is_hex32(a2));
    g_assert_cmpstr(a1, !=, a2);
    id_teardown();
}

                                                                          
static void
test_identity_delete_revokes_sessions_and_totp(void)
{
    id_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("carol", "pw", PCV_ROLE_OPERATOR, NULL, &err));
    g_assert_true(pcv_rbac_user_create("dave", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_autofree gchar *r1 = login("carol", NULL);
    g_autofree gchar *r2 = login("carol", NULL);
    g_autofree gchar *rd = login("dave", NULL);
    g_autofree gchar *cn = user_nonce("carol");
    gchar *secret = pcv_rbac_totp_enroll("carol", cn, &err);
    g_assert_no_error(err);
    g_assert_nonnull(secret);
    g_free(secret);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM user_totp WHERE username=?", "carol"), ==, 1);

    g_assert_true(pcv_rbac_user_delete("carol", &err));
    g_assert_no_error(err);
    g_assert_cmpint(pcv_rbac_user_exists("carol"), ==, PCV_USER_ABSENT);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM user_totp WHERE username=?", "carol"), ==, 0);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND revoked=0", "carol"), ==, 0);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND revoked=1", "carol"), ==, 2);
    gchar *nr = NULL;
    g_assert_null(pcv_rbac_refresh_token(r1, &nr, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&err);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND revoked=0", "dave"), ==, 1);
    gchar *access = pcv_rbac_refresh_token(rd, &nr, &err);                      
    g_assert_no_error(err);
    g_assert_nonnull(access);
    g_free(access); g_free(nr);
    id_teardown();
}

                                                                     
static void
test_identity_delete_rolls_back_on_failure(void)
{
    id_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("erin", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_autofree gchar *r = login("erin", NULL);
    raw_exec("DROP TABLE totp_recovery_codes;");
    g_assert_false(pcv_rbac_user_delete("erin", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&err);
    g_assert_cmpint(pcv_rbac_user_exists("erin"), ==, PCV_USER_PRESENT);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND revoked=0", "erin"), ==, 1);
    g_assert_false(pcv_rbac_user_delete("no-such-user", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);
    id_teardown();
}

                                                                      
                                                   
static void
test_identity_issue_tokens_requires_nonce(void)
{
    id_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("frank", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_autofree gchar *n = user_nonce("frank");
    const gchar *bad[] = { NULL, "", "ffffffffffffffffffffffffffffffff", "short" };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        gchar *refresh = NULL;
        g_assert_null(pcv_rbac_issue_tokens("frank", bad[i], &refresh, &err));
        g_assert_null(refresh);
        g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_clear_error(&err);
    }
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=?", "frank"), ==, 0);

    gchar *access = NULL;
    g_autofree gchar *refresh = login("frank", &access);
    g_autofree gchar *claim = pcv_jwt_identity_nonce(access);
    g_assert_cmpstr(claim, ==, n);
    gchar *sub = pcv_jwt_verify(access, &err);                                   
    g_assert_no_error(err);
    g_assert_cmpstr(sub, ==, "frank");
    g_free(sub); g_free(access);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND identity_nonce=(SELECT identity_nonce FROM users WHERE username='frank')", "frank"), ==, 1);

                                                   
    raw_exec("UPDATE users SET identity_nonce='' WHERE username='frank';");
    g_assert_null(pcv_rbac_issue_tokens("frank", "", NULL, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);
    id_teardown();
}

                                                                                   
static void
test_identity_refresh_rotates_with_nonce(void)
{
    id_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("gina", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_autofree gchar *n = user_nonce("gina");
    g_autofree gchar *r = login("gina", NULL);
    gchar *nr = NULL;
    gchar *access = pcv_rbac_refresh_token(r, &nr, &err);
    g_assert_no_error(err);
    g_assert_nonnull(access);
    g_autofree gchar *claim = pcv_jwt_identity_nonce(access);
    g_assert_cmpstr(claim, ==, n);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND revoked=0 AND identity_nonce<>''", "gina"), ==, 1);
    g_free(access);
    access = pcv_rbac_refresh_token(nr, NULL, &err);                                   
    g_assert_no_error(err);
    g_assert_nonnull(access);
    g_free(access); g_free(nr);
    id_teardown();
}

                                                                            
                                                              
static void
test_identity_refresh_rejects_identity_change(void)
{
    id_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("hank", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_autofree gchar *r_mismatch = login("hank", NULL);
    g_autofree gchar *r_empty = login("hank", NULL);
    g_autofree gchar *r_absent = login("hank", NULL);

    raw_exec("UPDATE users SET identity_nonce='00000000000000000000000000000000' WHERE username='hank';");
    gchar *nr = NULL;
    g_assert_null(pcv_rbac_refresh_token(r_mismatch, &nr, &err));
    g_assert_null(nr);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_cmpstr(err->message, ==, "Invalid or expired refresh token");
    g_clear_error(&err);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND revoked=0", "hank"), ==, 2);

                                                                       
    raw_exec("UPDATE sessions SET identity_nonce='' WHERE revoked=0;");
    raw_exec("UPDATE users SET identity_nonce='' WHERE username='hank';");
    g_assert_null(pcv_rbac_refresh_token(r_empty, &nr, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&err);

    raw_exec("DELETE FROM users WHERE username='hank';");
    g_assert_null(pcv_rbac_refresh_token(r_absent, &nr, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&err);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND revoked=0", "hank"), ==, 0);
    id_teardown();
}

                                                                                           
static void
test_identity_totp_enroll_requires_nonce(void)
{
    id_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("ivy", "pw", PCV_ROLE_VIEWER, NULL, &err));
    gchar *secret = pcv_rbac_totp_enroll("ghost", "0123456789abcdef0123456789abcdef", &err);
    g_assert_null(secret);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&err);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM user_totp WHERE username=?", "ghost"), ==, 0);
    const gchar *bad[] = { NULL, "", "ffffffffffffffffffffffffffffffff" };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_assert_null(pcv_rbac_totp_enroll("ivy", bad[i], &err));
        g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
        g_clear_error(&err);
    }
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM user_totp WHERE username=?", "ivy"), ==, 0);
    g_autofree gchar *n = user_nonce("ivy");
    secret = pcv_rbac_totp_enroll("ivy", n, &err);
    g_assert_no_error(err);
    g_assert_nonnull(secret);
    g_free(secret);
    id_teardown();
}

                                                                                 
static void
make_old_schema(void)
{
    pcv_rbac_shutdown();
    raw_exec("ALTER TABLE users DROP COLUMN identity_nonce;"
             "ALTER TABLE sessions DROP COLUMN identity_nonce;"
             "PRAGMA user_version=1;"
             "INSERT INTO users (username, password_hash, salt, role) VALUES"
             " ('olduser','h','s',0), ('opuser','h','s',1), ('admuser','h','s',2);"
             "INSERT INTO sessions (username, refresh_token_hash, created_at, expires_at, revoked) VALUES"
             " ('olduser','hash-a',0,9999999999,0), ('opuser','hash-b',0,9999999999,0);"
             "INSERT INTO api_keys (key_hash, client_name, role) VALUES"
             " ('k1','ci-pipeline',1), ('k2','opuser',2), ('k3','admuser',0), ('k4','olduser',1);");
}

                                                                                 
                                                                    
static void
test_identity_migrate_old_schema(void)
{
    id_setup();
    make_old_schema();
    pcv_rbac_init(g_db);
    gint filled = -2, revoked = -2, downgraded = -2;
    pcv_rbac_identity_last_migration_for_test(&filled, &revoked, &downgraded);
    g_assert_cmpint(filled, ==, 3);
    g_assert_cmpint(revoked, ==, 2);
    g_assert_cmpint(downgraded, ==, 3);
    g_autofree gchar *n1 = user_nonce("olduser");
    g_autofree gchar *n2 = user_nonce("opuser");
    g_autofree gchar *n3 = user_nonce("admuser");
    g_assert_true(is_hex32(n1) && is_hex32(n2) && is_hex32(n3));
    g_assert_cmpstr(n1, !=, n2);
    g_assert_cmpstr(n2, !=, n3);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE revoked=0", NULL), ==, 0);
    g_assert_cmpint(raw_int("SELECT role FROM api_keys WHERE key_hash=?", "k1"), ==, 0);                      
    g_assert_cmpint(raw_int("SELECT role FROM api_keys WHERE key_hash=?", "k2"), ==, 1);                 
    g_assert_cmpint(raw_int("SELECT role FROM api_keys WHERE key_hash=?", "k3"), ==, 0);                          
    g_assert_cmpint(raw_int("SELECT role FROM api_keys WHERE key_hash=?", "k4"), ==, 0);
    g_assert_cmpint(raw_int("PRAGMA user_version", NULL), ==, 2);
                                                              
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("fresh", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
    g_autofree gchar *nf = user_nonce("fresh");
    g_assert_true(is_hex32(nf));
    id_teardown();
}

                                                                            
                                                                          
static void
test_identity_migrate_rerun_semantics(void)
{
    id_setup();
    make_old_schema();
    pcv_rbac_init(g_db);
    g_autofree gchar *n1 = user_nonce("olduser");
    g_autofree gchar *live = login("opuser", NULL);
    gchar *key = NULL;
    GError *err = NULL;
    g_assert_true(pcv_rbac_apikey_create("ci-after-upgrade", PCV_ROLE_OPERATOR, NULL, 0, &key, &err));
    g_assert_no_error(err);
    g_free(key);
    pcv_rbac_shutdown();
                                                    
    raw_exec("INSERT INTO users (username, password_hash, salt, role) VALUES ('rollback-user','h','s',0);"
             "INSERT INTO sessions (username, refresh_token_hash, created_at, expires_at, revoked)"
             " VALUES ('rollback-user','hash-r',0,9999999999,0);");

    pcv_rbac_init(g_db);
    gint filled = -2, revoked = -2, downgraded = -2;
    pcv_rbac_identity_last_migration_for_test(&filled, &revoked, &downgraded);
    g_assert_cmpint(filled, ==, 1);
    g_assert_cmpint(revoked, ==, 1);
    g_assert_cmpint(downgraded, ==, 0);
    g_autofree gchar *n1b = user_nonce("olduser");
    g_assert_cmpstr(n1, ==, n1b);
    g_autofree gchar *nr = user_nonce("rollback-user");
    g_assert_true(is_hex32(nr));
    g_assert_cmpint(raw_int("SELECT role FROM api_keys WHERE client_name=?", "ci-after-upgrade"), ==, 1);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND revoked=0", "rollback-user"), ==, 0);
    gchar *access = pcv_rbac_refresh_token(live, NULL, &err);                              
    g_assert_no_error(err);
    g_assert_nonnull(access);
    g_free(access);
    id_teardown();
}

                                                                                          
                                   
static void
test_identity_subject_check_states(void)
{
    id_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("kate", "pw", PCV_ROLE_OPERATOR, NULL, &err));
    g_autofree gchar *n = user_nonce("kate");
    PcvRole role = PCV_ROLE_VIEWER;
    g_assert_cmpint(pcv_rbac_subject_check("kate", n, &role), ==, PCV_SUBJECT_OK);
    g_assert_cmpint(role, ==, PCV_ROLE_OPERATOR);
    g_assert_cmpint(pcv_rbac_subject_check("ghost", n, NULL), ==, PCV_SUBJECT_REJECT);
    g_assert_cmpint(pcv_rbac_subject_check("kate", "ffffffffffffffffffffffffffffffff", NULL), ==, PCV_SUBJECT_REJECT);
    g_assert_cmpint(pcv_rbac_subject_check("kate", NULL, NULL), ==, PCV_SUBJECT_REJECT);
    g_assert_cmpint(pcv_rbac_subject_check("kate", "", NULL), ==, PCV_SUBJECT_REJECT);
    g_assert_cmpint(pcv_rbac_subject_check(NULL, n, NULL), ==, PCV_SUBJECT_REJECT);
    raw_exec("DROP TABLE users;");
    g_assert_cmpint(pcv_rbac_subject_check("kate", n, NULL), ==, PCV_SUBJECT_UNAVAILABLE);
    id_teardown();
}

                                                                                     
                                                                               
static void
test_identity_subject_check_token_paths(void)
{
    id_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("liam", "pw", PCV_ROLE_ADMIN, NULL, &err));
    g_autofree gchar *n = user_nonce("liam");
    gchar *access = NULL;
    g_autofree gchar *refresh = login("liam", &access);
    PcvRole role = PCV_ROLE_VIEWER;
    g_assert_cmpint(pcv_rbac_subject_check_token("liam", access, &role), ==, PCV_SUBJECT_OK);
    g_assert_cmpint(role, ==, PCV_ROLE_ADMIN);
    g_autofree gchar *bearer = g_strconcat("Bearer ", access, NULL);
    g_assert_cmpint(pcv_rbac_subject_check_token("liam", bearer, NULL), ==, PCV_SUBJECT_OK);

    gchar *rotated = pcv_rbac_refresh_token(refresh, NULL, &err);                           
    g_assert_no_error(err);
    g_assert_cmpint(pcv_rbac_subject_check_token("liam", rotated, NULL), ==, PCV_SUBJECT_OK);

                                                                               
    gchar *pending = pcv_jwt_sign_scoped("liam", 180, "10.0.0.7", "totp", n, &err);
    g_assert_no_error(err);
    g_assert_cmpint(pcv_rbac_subject_check_token("liam", pending, NULL), ==, PCV_SUBJECT_OK);
    g_autofree gchar *pend_claim = pcv_jwt_identity_nonce(pending);
    gchar *via_verify = pcv_rbac_issue_tokens("liam", pend_claim, NULL, &err);
    g_assert_no_error(err);
    g_assert_cmpint(pcv_rbac_subject_check_token("liam", via_verify, NULL), ==, PCV_SUBJECT_OK);

    gchar *legacy = pcv_jwt_sign("liam", 900, &err);                                   
    g_assert_cmpint(pcv_rbac_subject_check_token("liam", legacy, NULL), ==, PCV_SUBJECT_REJECT);
    g_assert_cmpint(pcv_rbac_subject_check_token("liam", "not-a-jwt", NULL), ==, PCV_SUBJECT_REJECT);

                                                                                        
    g_assert_true(pcv_rbac_user_delete("liam", &err));
    g_assert_cmpint(pcv_rbac_subject_check_token("liam", access, NULL), ==, PCV_SUBJECT_REJECT);
    g_assert_true(pcv_rbac_user_create("liam", "pw2", PCV_ROLE_ADMIN, NULL, &err));
    g_assert_cmpint(pcv_rbac_subject_check_token("liam", access, NULL), ==, PCV_SUBJECT_REJECT);
    g_assert_cmpint(pcv_rbac_subject_check_token("liam", rotated, NULL), ==, PCV_SUBJECT_REJECT);
    g_assert_cmpint(pcv_rbac_subject_check_token("liam", pending, NULL), ==, PCV_SUBJECT_REJECT);
    g_assert_null(pcv_rbac_issue_tokens("liam", pend_claim, NULL, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);
    g_free(access); g_free(rotated); g_free(pending); g_free(via_verify); g_free(legacy);
    id_teardown();
}

                                                                                        
                                                                                     
static void
test_identity_apikey_stored_role_and_absent_permission(void)
{
    id_setup();
    GError *err = NULL;
    gchar *key = NULL;
    g_assert_true(pcv_rbac_apikey_create("ci-pipeline", PCV_ROLE_OPERATOR, NULL, 0, &key, &err));
    g_assert_no_error(err);
    PcvRole stored = PCV_ROLE_VIEWER;
    gchar *client = pcv_rbac_verify_api_key(key, &stored, &err);
    g_assert_no_error(err);
    g_assert_cmpstr(client, ==, "ci-pipeline");
    g_assert_cmpint(stored, ==, PCV_ROLE_OPERATOR);
    g_assert_true(pcv_rbac_role_permits(stored, "vm.start"));
    g_assert_false(pcv_rbac_check_permission(client, "vm.start"));                           
    g_assert_false(pcv_rbac_check_permission("ghost", "vm.list"));                        
    g_assert_true(pcv_rbac_role_permits(PCV_ROLE_VIEWER, "vm.list"));
    g_assert_false(pcv_rbac_role_permits(PCV_ROLE_VIEWER, "vm.start"));
    g_assert_false(pcv_rbac_role_permits((PcvRole)3, "vm.list"));
    g_assert_false(pcv_rbac_role_permits(PCV_ROLE_ADMIN, NULL));
    g_assert_true(pcv_rbac_user_create("mona", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_assert_true(pcv_rbac_check_permission("mona", "vm.list"));
    g_assert_false(pcv_rbac_check_permission("mona", "vm.start"));
    g_free(client);
    pcv_secure_free_str(&key);
    id_teardown();
}

                                                                                    
                                                     
static void
test_identity_delete_handler_closes_subject_connections(void)
{
    id_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("nina", "pw", PCV_ROLE_OPERATOR, NULL, &err));
    g_autofree gchar *r = login("nina", NULL);
    gint before = 0;
    (void)pcv_test_ws_closed_subject(&before);

    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "username", "nina");
    handle_auth_user_delete(params, "1", NULL, NULL);
    json_object_unref(params);
    gint after = 0;
    g_assert_cmpstr(pcv_test_ws_closed_subject(&after), ==, "nina");
    g_assert_cmpint(after, ==, before + 1);
    g_assert_cmpint(pcv_rbac_user_exists("nina"), ==, PCV_USER_ABSENT);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND revoked=0", "nina"), ==, 0);

    params = json_object_new();
    json_object_set_string_member(params, "username", "nobody");
    handle_auth_user_delete(params, "2", NULL, NULL);
    json_object_unref(params);
    gint again = 0;
    (void)pcv_test_ws_closed_subject(&again);
    g_assert_cmpint(again, ==, after);
    id_teardown();
}

void
test_rbac_identity_register(void)
{
    g_test_add_func("/rbac/identity/lookup_three_states", test_identity_lookup_three_states);
    g_test_add_func("/rbac/identity/create_and_recreate_nonce", test_identity_create_and_recreate_nonce);
    g_test_add_func("/rbac/identity/delete_revokes_sessions_and_totp", test_identity_delete_revokes_sessions_and_totp);
    g_test_add_func("/rbac/identity/delete_rolls_back_on_failure", test_identity_delete_rolls_back_on_failure);
    g_test_add_func("/rbac/identity/issue_tokens_requires_nonce", test_identity_issue_tokens_requires_nonce);
    g_test_add_func("/rbac/identity/refresh_rotates_with_nonce", test_identity_refresh_rotates_with_nonce);
    g_test_add_func("/rbac/identity/refresh_rejects_identity_change", test_identity_refresh_rejects_identity_change);
    g_test_add_func("/rbac/identity/totp_enroll_requires_nonce", test_identity_totp_enroll_requires_nonce);
    g_test_add_func("/rbac/identity/migrate_old_schema", test_identity_migrate_old_schema);
    g_test_add_func("/rbac/identity/migrate_rerun_semantics", test_identity_migrate_rerun_semantics);
    g_test_add_func("/rbac/identity/subject_check_states", test_identity_subject_check_states);
    g_test_add_func("/rbac/identity/subject_check_token_paths", test_identity_subject_check_token_paths);
    g_test_add_func("/rbac/identity/apikey_stored_role_and_absent_permission",
                    test_identity_apikey_stored_role_and_absent_permission);
    g_test_add_func("/rbac/identity/delete_handler_closes_subject_connections",
                    test_identity_delete_handler_closes_subject_connections);
}
