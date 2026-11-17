                                                                                                             
                                                                                             
                                                                                 
                                                                                    
                                                                                      
                                                                            
                                                            
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <sqlite3.h>
#include <string.h>

#include "../src/modules/auth/pcv_rbac.h"
#include "../src/modules/auth/pcv_rbac_internal.h"
#include "../src/modules/auth/pcv_principal_names.h"
#include "../src/utils/pcv_config.h"
#include "../src/utils/pcv_jwt.h"
#include "../src/utils/pcv_secure.h"

#define PW "Principal-Names-Pw-73!"

                                                            
void pcv_rbac_names_startup_counts_for_test(gint out[7]);

static gchar *g_dir = NULL;
static gchar *g_db = NULL;

static void
names_setup(void)
{
    g_dir = g_dir_make_tmp("pcv-rbac-names-XXXXXX", NULL);
    g_assert_nonnull(g_dir);
    g_db = g_build_filename(g_dir, "rbac.db", NULL);
    pcv_jwt_init("rbac-names-test-secret-32-bytes!!!!!");
    pcv_rbac_init(g_db);
}

static void
names_teardown(void)
{
    pcv_rbac_shutdown();
    pcv_jwt_shutdown();
    gchar *wal = g_strconcat(g_db, "-wal", NULL), *shm = g_strconcat(g_db, "-shm", NULL);
    g_autofree gchar *conf = g_build_filename(g_dir, "daemon.conf", NULL);
    g_unlink(g_db); g_unlink(wal); g_unlink(shm); g_unlink(conf);
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
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK)
        g_error("raw_exec failed: %s", err ? err : "?");
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

                         
static void
issue_key(const gchar *name)
{
    gchar *key = NULL;
    GError *err = NULL;
    g_assert_true(pcv_rbac_apikey_create(name, PCV_ROLE_VIEWER, NULL, 0, &key, &err));
    g_assert_no_error(err);
    pcv_secure_free_str(&key);
}

                                             
static void
assert_user_refused(const gchar *name, gint code, const gchar *msg)
{
    gint64 before = raw_int("SELECT COUNT(*) FROM users", NULL);
    GError *err = NULL;
    g_assert_false(pcv_rbac_user_create(name, PW, PCV_ROLE_VIEWER, NULL, &err));
    g_assert_error(err, PCV_PRINCIPAL_ERROR, code);
    g_assert_cmpstr(err->message, ==, msg);
    g_clear_error(&err);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM users", NULL), ==, before);
}

                                                     
static void
assert_key_refused(const gchar *name, gint code, const gchar *msg)
{
    gint64 before = raw_int("SELECT COUNT(*) FROM api_keys", NULL);
    gchar *key = NULL;
    GError *err = NULL;
    g_assert_false(pcv_rbac_apikey_create(name, PCV_ROLE_VIEWER, NULL, 0, &key, &err));
    g_assert_null(key);
    g_assert_error(err, PCV_PRINCIPAL_ERROR, code);
    g_assert_cmpstr(err->message, ==, msg);
    g_clear_error(&err);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM api_keys", NULL), ==, before);
}

#define MSG_USER_KEY      "username is already used by an API key"
#define MSG_USER_RESERVED "username is reserved"
#define MSG_USER_DELETED  "username belongs to a deleted account — set reuse_deleted_name to recreate it with that " \
                          "account's ownership records"
#define MSG_KEY_USER      "name is already used by a user account"
#define MSG_KEY_RESERVED  "name is reserved"
#define MSG_KEY_DELETED   "name belongs to a deleted account"

static const gchar *const k_reserved[] = { "grpc", "local-admin", "system", "ai-agent", "ai-ops", "-" };

                                                          
static void
test_user_rejects_key_name(void)
{
    names_setup();
    issue_key("k1");
    issue_key("k2");
    GError *err = NULL;
    g_assert_true(pcv_rbac_apikey_revoke("k2", &err));
    g_assert_no_error(err);
    raw_exec("INSERT INTO api_keys (key_hash, client_name, role) VALUES ('seeded-k3', ' k3 ', 0);");
    assert_user_refused("k1", PCV_PRINCIPAL_ERROR_KEY_NAME, MSG_USER_KEY);
    assert_user_refused("k2", PCV_PRINCIPAL_ERROR_KEY_NAME, MSG_USER_KEY);
    assert_user_refused("k3", PCV_PRINCIPAL_ERROR_KEY_NAME, MSG_USER_KEY);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM users", NULL), ==, 0);
    names_teardown();
}

                                                                             
static void
test_user_rejects_reserved(void)
{
    names_setup();
    for (gsize i = 0; i < G_N_ELEMENTS(k_reserved); i++)
        assert_user_refused(k_reserved[i], PCV_PRINCIPAL_ERROR_RESERVED, MSG_USER_RESERVED);
    assert_user_refused(" grpc", PCV_PRINCIPAL_ERROR_RESERVED, MSG_USER_RESERVED);
    assert_user_refused("system\n", PCV_PRINCIPAL_ERROR_RESERVED, MSG_USER_RESERVED);
    names_teardown();
}

                                                                        
static void
test_user_rejects_tombstone(void)
{
    names_setup();
    raw_exec("INSERT INTO deleted_user_names (username) VALUES (' bob'), ('dave');");
    assert_user_refused("bob", PCV_PRINCIPAL_ERROR_DELETED, MSG_USER_DELETED);
    raw_exec("INSERT INTO api_keys (key_hash, client_name, role) VALUES ('seeded-dave', 'dave', 0);");                                      
    assert_user_refused("dave", PCV_PRINCIPAL_ERROR_KEY_NAME, MSG_USER_KEY);
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("bobby", PW, PCV_ROLE_VIEWER, NULL, &err));                    
    g_assert_no_error(err);
    names_teardown();
}

                                                         
static void
test_key_rejects_user_name(void)
{
    names_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("alice", PW, PCV_ROLE_OPERATOR, NULL, &err));
    g_assert_no_error(err);
    raw_exec("INSERT INTO users (username, password_hash, salt, role) VALUES (' carl', 'h', 's', 0);");
    assert_key_refused("alice", PCV_PRINCIPAL_ERROR_USER_NAME, MSG_KEY_USER);
    assert_key_refused("carl", PCV_PRINCIPAL_ERROR_USER_NAME, MSG_KEY_USER);
    assert_key_refused("alice\t", PCV_PRINCIPAL_ERROR_USER_NAME, MSG_KEY_USER);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM api_keys", NULL), ==, 0);
    names_teardown();
}

                                                            
static void
test_key_rejects_reserved_and_tombstone(void)
{
    names_setup();
    for (gsize i = 0; i < G_N_ELEMENTS(k_reserved); i++)
        assert_key_refused(k_reserved[i], PCV_PRINCIPAL_ERROR_RESERVED, MSG_KEY_RESERVED);
    assert_key_refused(" local-admin ", PCV_PRINCIPAL_ERROR_RESERVED, MSG_KEY_RESERVED);
    raw_exec("INSERT INTO deleted_user_names (username) VALUES ('erin'), ('frank\r');");
    assert_key_refused("erin", PCV_PRINCIPAL_ERROR_DELETED, MSG_KEY_DELETED);
    assert_key_refused("frank", PCV_PRINCIPAL_ERROR_DELETED, MSG_KEY_DELETED);
    names_teardown();
}

                                              
static void
test_key_same_name_second_allowed(void)
{
    names_setup();
    issue_key("svc");
    issue_key("svc");
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM api_keys WHERE client_name = ?", "svc"), ==, 2);
    names_teardown();
}

                                                                  
static int
count_stmt(unsigned type, void *ctx, void *p, void *x)
{
    (void)p; (void)x;
    if (type == SQLITE_TRACE_STMT) (*(gint *)ctx)++;
    return 0;
}

static void
trace_on(gint *counter)
{
    sqlite3 *db = pcv_rbac_db_lock();
    g_assert_nonnull(db);
    *counter = 0;
    sqlite3_trace_v2(db, SQLITE_TRACE_STMT, count_stmt, counter);
    pcv_rbac_db_unlock();
}

static void
trace_off(void)
{
    sqlite3 *db = pcv_rbac_db_lock();
    sqlite3_trace_v2(db, 0, NULL, NULL);
    pcv_rbac_db_unlock();
}

                                                                                       
                                                                         
static void
test_success_statement_count(void)
{
    names_setup();
    gint n = 0;
    GError *err = NULL;
    trace_on(&n);
    g_assert_true(pcv_rbac_user_create("u1", PW, PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
    g_assert_cmpint(n, ==, 1);
    trace_on(&n);
    issue_key("svc");
    g_assert_cmpint(n, ==, 2);
    trace_on(&n);
    g_assert_false(pcv_rbac_user_create("svc", PW, PCV_ROLE_VIEWER, NULL, &err));
    g_assert_error(err, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_KEY_NAME);
    g_clear_error(&err);
    g_assert_cmpint(n, ==, 2);
    trace_off();
    names_teardown();
}

                                                
typedef struct {
    gint warns;
    gint crits;
    gchar *last;
} LogCap;

static void
cap_handler(const gchar *dom, GLogLevelFlags lvl, const gchar *msg, gpointer ud)
{
    (void)dom;
    LogCap *c = ud;
    if (lvl & G_LOG_LEVEL_WARNING) c->warns++;
    if (lvl & G_LOG_LEVEL_CRITICAL) c->crits++;
    g_free(c->last);
    c->last = g_strdup(msg);
}

static guint
cap_begin(LogCap *c)
{
    memset(c, 0, sizeof *c);
    return g_log_set_handler("pcv_rbac", G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_FATAL, cap_handler, c);
}

static void
cap_end(guint id, LogCap *c)
{
    g_log_remove_handler("pcv_rbac", id);
    g_clear_pointer(&c->last, g_free);
}

                                                                                 
static void
reboot_with_admin(const gchar *admin_user, LogCap *cap)
{
    pcv_rbac_shutdown();
    g_autofree gchar *conf = g_build_filename(g_dir, "daemon.conf", NULL);
    g_autofree gchar *body = g_strdup_printf("[daemon]\nadmin_user=%s\nadmin_password=Principal-Boot-Pw-73!\n", admin_user);
    g_assert_true(g_file_set_contents(conf, body, -1, NULL));
    g_setenv("PCV_CONFIG_PATH", conf, TRUE);
    pcv_config_init();
    guint id = cap_begin(cap);
    pcv_rbac_init(g_db);
    g_log_remove_handler("pcv_rbac", id);
}

                                                                                
static void
restore_config(void)
{
    pcv_config_shutdown();
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);
    pcv_config_init();
}

#define BOOT_WARN_PREFIX "not created: the name is reserved, used by an API key, or belongs to a deleted account"

                                                                                    
                    
static void
test_bootstrap_refuses_principal_names(void)
{
    names_setup();
    issue_key("bootkey");
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("bootgone", PW, PCV_ROLE_ADMIN, NULL, &err));                       
    g_assert_true(pcv_rbac_user_delete("bootgone", &err));
    g_assert_no_error(err);
    const gchar *const names[] = { "grpc", "bootkey", "bootgone" };
    for (gsize i = 0; i < G_N_ELEMENTS(names); i++) {
        LogCap cap;
        reboot_with_admin(names[i], &cap);
        g_assert_cmpint(raw_int("SELECT COUNT(*) FROM users", NULL), ==, 0);
        g_assert_cmpint(cap.crits, ==, 0);
        g_assert_cmpint(cap.warns, ==, 1);
        g_assert_nonnull(strstr(cap.last, BOOT_WARN_PREFIX));
        g_assert_nonnull(strstr(cap.last, "--reuse-deleted-name"));
        g_clear_pointer(&cap.last, g_free);
    }
                                                      
    LogCap ok_cap;
    reboot_with_admin("bootadmin", &ok_cap);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM users WHERE username = ?", "bootadmin"), ==, 1);
    g_assert_cmpint(ok_cap.warns + ok_cap.crits, ==, 0);
    g_clear_pointer(&ok_cap.last, g_free);
    restore_config();

                                                    
    LogCap cap;
    guint id = cap_begin(&cap);
    GError *in = g_error_new_literal(PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_DELETED, "x");
    pcv_rbac_names_log_bootstrap_refusal("someone", in);
    g_assert_cmpint(cap.warns, ==, 1);
    g_assert_cmpint(cap.crits, ==, 0);
    GError *out = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_EXISTS, "boom");
    pcv_rbac_names_log_bootstrap_refusal("someone", out);
    g_assert_cmpint(cap.crits, ==, 1);
    g_assert_nonnull(strstr(cap.last, "Failed to auto-create admin user: boom"));
    g_error_free(in);
    g_error_free(out);
    cap_end(id, &cap);
    names_teardown();
}

                                          
static gchar *
user_nonce(const gchar *user)
{
    gchar *n = NULL;
    g_assert_cmpint(pcv_rbac_lookup_subject(user, NULL, &n), ==, PCV_USER_PRESENT);
    return n;
}

                                                                  
static void
test_delete_writes_tombstone(void)
{
    names_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("gina", PW, PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names", NULL), ==, 0);
    g_assert_true(pcv_rbac_user_delete("gina", &err));
    g_assert_no_error(err);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names WHERE username = ?", "gina"), ==, 1);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names", NULL), ==, 1);
    assert_user_refused("gina", PCV_PRINCIPAL_ERROR_DELETED, MSG_USER_DELETED);
    assert_key_refused("gina", PCV_PRINCIPAL_ERROR_DELETED, MSG_KEY_DELETED);
                                      
    g_assert_false(pcv_rbac_user_delete("nobody", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&err);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names", NULL), ==, 1);
    names_teardown();
}

                                                                             
static void
test_delete_tombstone_failure_rolls_back(void)
{
    names_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("hana", PW, PCV_ROLE_VIEWER, NULL, &err));
    g_autofree gchar *n = user_nonce("hana");
    g_autofree gchar *refresh = NULL;
    g_autofree gchar *access = pcv_rbac_issue_tokens("hana", n, &refresh, &err);                      
    g_assert_no_error(err);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND revoked=0", "hana"), ==, 1);
    raw_exec("DROP TABLE deleted_user_names;");
    g_assert_false(pcv_rbac_user_delete("hana", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&err);
    g_assert_cmpint(pcv_rbac_user_exists("hana"), ==, PCV_USER_PRESENT);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM sessions WHERE username=? AND revoked=0", "hana"), ==, 1);
    names_teardown();

    names_setup();
    g_assert_true(pcv_rbac_user_create("ines", PW, PCV_ROLE_VIEWER, NULL, &err));
    raw_exec("DROP TABLE totp_recovery_codes;");
    g_assert_false(pcv_rbac_user_delete("ines", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&err);
    g_assert_cmpint(pcv_rbac_user_exists("ines"), ==, PCV_USER_PRESENT);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names", NULL), ==, 0);
    names_teardown();
}

                                                                                  
                                                              
static void
test_reuse_flag_scope(void)
{
    names_setup();
    GError *err = NULL;
    gboolean reused = FALSE;
    g_assert_true(pcv_rbac_user_create("jay", PW, PCV_ROLE_OPERATOR, NULL, &err));
    g_autofree gchar *n1 = user_nonce("jay");
    g_assert_true(pcv_rbac_user_delete("jay", &err));
    g_assert_no_error(err);
    gint stmts = 0;
    trace_on(&stmts);
    g_assert_true(pcv_rbac_user_create_ex("jay", PW, PCV_ROLE_OPERATOR, NULL, PCV_RBAC_CREATE_REUSE_DELETED_NAME,
                                          &reused, &err));
    trace_off();
    g_assert_no_error(err);
    g_assert_true(reused);
    g_assert_cmpint(stmts, ==, 4);                                                                  
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names", NULL), ==, 0);
    g_autofree gchar *n2 = user_nonce("jay");
    g_assert_cmpstr(n1, !=, n2);

    reused = TRUE;
    g_assert_true(pcv_rbac_user_create_ex("kim", PW, PCV_ROLE_VIEWER, NULL, PCV_RBAC_CREATE_REUSE_DELETED_NAME,
                                          &reused, &err));
    g_assert_no_error(err);
    g_assert_false(reused);

    g_assert_false(pcv_rbac_user_create_ex("system", PW, PCV_ROLE_VIEWER, NULL, PCV_RBAC_CREATE_REUSE_DELETED_NAME,
                                           &reused, &err));
    g_assert_error(err, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_RESERVED);
    g_clear_error(&err);

    raw_exec("INSERT INTO deleted_user_names (username) VALUES ('lee');"
             "INSERT INTO api_keys (key_hash, client_name, role) VALUES ('seeded-lee', 'lee', 0);");
    g_assert_false(pcv_rbac_user_create_ex("lee", PW, PCV_ROLE_VIEWER, NULL, PCV_RBAC_CREATE_REUSE_DELETED_NAME,
                                           &reused, &err));
    g_assert_error(err, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_KEY_NAME);
    g_clear_error(&err);
    g_assert_false(reused);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names WHERE username = ?", "lee"), ==, 1);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM users WHERE username = ?", "lee"), ==, 0);

                                                                      
    g_assert_false(pcv_rbac_user_create_ex("kim", PW, PCV_ROLE_VIEWER, NULL, PCV_RBAC_CREATE_REUSE_DELETED_NAME,
                                           &reused, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&err);
    names_teardown();
}

                                                                                            
                                                                                                 
static void
test_deleted_name_add(void)
{
    names_setup();
    GError *err = NULL;
    gboolean added = FALSE;
    g_assert_true(pcv_rbac_deleted_name_add("zed", &added, &err));
    g_assert_no_error(err);
    g_assert_true(added);
    added = TRUE;
    g_assert_true(pcv_rbac_deleted_name_add("zed", &added, &err));
    g_assert_no_error(err);
    g_assert_false(added);
    g_assert_true(pcv_rbac_deleted_name_add(" zed\t", &added, &err));                              
    g_assert_false(added);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names", NULL), ==, 1);

    g_assert_true(pcv_rbac_user_create("yan", PW, PCV_ROLE_VIEWER, NULL, &err));
    g_assert_false(pcv_rbac_deleted_name_add("yan", &added, &err));
    g_assert_error(err, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_EXISTING_USER);
    g_assert_cmpstr(err->message, ==, "username belongs to an existing account — delete the account instead");
    g_clear_error(&err);

    issue_key("kw");
    raw_exec("INSERT INTO api_keys (key_hash, client_name, role) VALUES ('seeded-kx', ' kx ', 0);");
    const gchar *const keys[] = { "kw", "kx" };
    for (gsize i = 0; i < G_N_ELEMENTS(keys); i++) {
        g_assert_false(pcv_rbac_deleted_name_add(keys[i], &added, &err));
        g_assert_error(err, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_KEY_NAME);
        g_assert_cmpstr(err->message, ==, "name is already used by an API key");
        g_clear_error(&err);
    }
    g_assert_false(pcv_rbac_deleted_name_add("grpc", &added, &err));
    g_assert_error(err, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_RESERVED);
    g_assert_cmpstr(err->message, ==, MSG_KEY_RESERVED);
    g_clear_error(&err);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names", NULL), ==, 1);

    assert_user_refused("zed", PCV_PRINCIPAL_ERROR_DELETED, MSG_USER_DELETED);
    assert_key_refused("zed", PCV_PRINCIPAL_ERROR_DELETED, MSG_KEY_DELETED);
    names_teardown();
}

                                                                                                          
static int
veto_commit(void *ctx)
{
    (*(gint *)ctx)++;
    return 1;
}

                                           
static gboolean
rbac_autocommit(void)
{
    sqlite3 *db = pcv_rbac_db_lock();
    gboolean on = db && sqlite3_get_autocommit(db) != 0;
    pcv_rbac_db_unlock();
    return on;
}

                                                                                              
                                                                            
static void
test_commit_failure_reported(void)
{
    names_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("pat", PW, PCV_ROLE_VIEWER, NULL, &err));
    g_assert_true(pcv_rbac_user_delete("pat", &err));
    g_assert_no_error(err);
    gint vetoes = 0;
    sqlite3 *db = pcv_rbac_db_lock();
    sqlite3_commit_hook(db, veto_commit, &vetoes);
    pcv_rbac_db_unlock();

    gboolean reused = TRUE;
    g_assert_false(pcv_rbac_user_create_ex("pat", PW, PCV_ROLE_VIEWER, NULL, PCV_RBAC_CREATE_REUSE_DELETED_NAME,
                                           &reused, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&err);
    g_assert_false(reused);
    gboolean added = TRUE;
    g_assert_false(pcv_rbac_deleted_name_add("zoe", &added, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&err);
    g_assert_false(added);
    g_assert_cmpint(vetoes, ==, 2);
    g_assert_true(rbac_autocommit());

    db = pcv_rbac_db_lock();
    sqlite3_commit_hook(db, NULL, NULL);
    pcv_rbac_db_unlock();
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names WHERE username = ?", "pat"), ==, 1);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM users WHERE username = ?", "pat"), ==, 0);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names WHERE username = ?", "zoe"), ==, 0);
    names_teardown();
}

                                                                                            
                                                                               
                                                                              
static void
test_commit_failure_leaves_no_open_transaction(void)
{
    names_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("quin", PW, PCV_ROLE_VIEWER, NULL, &err));
    g_assert_true(pcv_rbac_user_delete("quin", &err));
    g_assert_no_error(err);
    raw_exec("CREATE TABLE fk_parent (id INTEGER PRIMARY KEY);"
             "CREATE TABLE fk_child (pid INTEGER REFERENCES fk_parent(id) DEFERRABLE INITIALLY DEFERRED);"
             "CREATE TRIGGER fk_users AFTER INSERT ON users BEGIN INSERT INTO fk_child VALUES (999); END;"
             "CREATE TRIGGER fk_tomb AFTER INSERT ON deleted_user_names BEGIN INSERT INTO fk_child VALUES (999); END;");
    sqlite3 *db = pcv_rbac_db_lock();
    g_assert_cmpint(sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL), ==, SQLITE_OK);
    pcv_rbac_db_unlock();

    gboolean reused = TRUE;
    g_assert_false(pcv_rbac_user_create_ex("quin", PW, PCV_ROLE_VIEWER, NULL, PCV_RBAC_CREATE_REUSE_DELETED_NAME,
                                           &reused, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&err);
    g_assert_false(reused);
    g_assert_true(rbac_autocommit());
    gboolean added = TRUE;
    g_assert_false(pcv_rbac_deleted_name_add("rho", &added, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&err);
    g_assert_false(added);
    g_assert_true(rbac_autocommit());
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names WHERE username = ?", "quin"), ==, 1);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM users WHERE username = ?", "quin"), ==, 0);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names WHERE username = ?", "rho"), ==, 0);

                                                                             
    raw_exec("DROP TRIGGER fk_users; DROP TRIGGER fk_tomb;");
    g_assert_true(pcv_rbac_deleted_name_add("rho", &added, &err));
    g_assert_no_error(err);
    g_assert_true(added);
    db = pcv_rbac_db_lock();
    sqlite3_exec(db, "PRAGMA foreign_keys = OFF;", NULL, NULL, NULL);
    pcv_rbac_db_unlock();
    names_teardown();
}

                                                                                      
                                                                                            
                                                                                                   
static void
test_signup_conflict_classes(void)
{
    names_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("sora", PW, PCV_ROLE_VIEWER, NULL, &err));
    issue_key("skey");
    raw_exec("INSERT INTO deleted_user_names (username) VALUES ('sgone');");
    const gchar *const names[] = { "sora", "system", "skey", "sgone" };
    for (gsize i = 0; i < G_N_ELEMENTS(names); i++) {
        g_assert_false(pcv_rbac_user_create(names[i], PW, PCV_ROLE_VIEWER, NULL, &err));
        g_assert_nonnull(err);
        g_assert_true(pcv_principal_signup_name_conflict(err));
        g_clear_error(&err);
    }
                                                   
    sqlite3 *raw = NULL;
    g_assert_cmpint(sqlite3_open(g_db, &raw), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(raw, "INSERT INTO users (username, password_hash, salt, role) VALUES ('sora', 'h', 's', 0);",
                                 NULL, NULL, NULL), ==, SQLITE_CONSTRAINT);
    g_assert_cmpint(sqlite3_extended_errcode(raw), ==, SQLITE_CONSTRAINT_PRIMARYKEY);
    sqlite3_close(raw);

                                                                                       
    gint vetoes = 0;
    sqlite3 *db = pcv_rbac_db_lock();
    sqlite3_commit_hook(db, veto_commit, &vetoes);
    pcv_rbac_db_unlock();
    g_assert_false(pcv_rbac_user_create("sfresh", PW, PCV_ROLE_VIEWER, NULL, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_false(pcv_principal_signup_name_conflict(err));
    g_assert_null(strstr(err->message, "sfresh"));
    g_clear_error(&err);
    g_assert_cmpint(vetoes, ==, 1);
    db = pcv_rbac_db_lock();
    sqlite3_commit_hook(db, NULL, NULL);
    pcv_rbac_db_unlock();
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM users WHERE username = ?", "sfresh"), ==, 0);
    names_teardown();
}

                               

                                                                                  
static void
seed_legacy_key(const gchar *plain, const gchar *name)
{
    g_autofree gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, plain, -1);
    char *sql = sqlite3_mprintf("INSERT INTO api_keys (key_hash, client_name, role) VALUES (%Q, %Q, 0);", hash, name);
    raw_exec(sql);
    sqlite3_free(sql);
}

                                             
static gboolean
list_has_name(JsonArray *list, const gchar *name)
{
    for (guint i = 0; list && i < json_array_get_length(list); i++) {
        JsonObject *o = json_array_get_object_element(list, i);
        if (o && json_object_has_member(o, "client_name") && g_strcmp0(json_object_get_string_member(o, "client_name"), name) == 0)
            return TRUE;
    }
    return FALSE;
}

                                                                                        
static void
test_revoke_and_verify_nonconforming_key(void)
{
    names_setup();
    static const struct { const gchar *name; const gchar *plain; } k[] = {
        { " old key", "pcv_1111111111111111111111111111111111111111111111111111111111111111" },
        { "-",        "pcv_2222222222222222222222222222222222222222222222222222222222222222" },
        { "a/b",      "pcv_3333333333333333333333333333333333333333333333333333333333333333" },
        { "a/b",      "pcv_4444444444444444444444444444444444444444444444444444444444444444" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(k); i++)
        seed_legacy_key(k[i].plain, k[i].name);
    for (guint i = 0; i < G_N_ELEMENTS(k); i++) {
        GError *err = NULL;
        PcvRole role = PCV_ROLE_ADMIN;
        g_autofree gchar *who = pcv_rbac_verify_api_key(k[i].plain, &role, &err);
        g_assert_no_error(err);
        g_assert_cmpstr(who, ==, k[i].name);
        g_assert_cmpint(role, ==, PCV_ROLE_VIEWER);
    }
    JsonArray *list = pcv_rbac_apikey_list();
    g_assert_true(list_has_name(list, " old key"));
    g_assert_true(list_has_name(list, "-"));
    g_assert_true(list_has_name(list, "a/b"));
    if (list) json_array_unref(list);
    const gchar *revoke[] = { " old key", "-", "a/b" };
    for (guint i = 0; i < G_N_ELEMENTS(revoke); i++) {
        GError *err = NULL;
        g_assert_true(pcv_rbac_apikey_revoke(revoke[i], &err));
        g_assert_no_error(err);
    }
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM api_keys WHERE revoked = 0", NULL), ==, 0);
    for (guint i = 0; i < G_N_ELEMENTS(k); i++) {
        GError *err = NULL;
        g_autofree gchar *who = pcv_rbac_verify_api_key(k[i].plain, NULL, &err);
        g_assert_null(who);
        g_assert_error(err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
        g_error_free(err);
    }
    names_teardown();
}


                                                                                           
                                                                                                  
static void
test_startup_counts(void)
{
    names_setup();
    pcv_rbac_shutdown();
    raw_exec("INSERT INTO users (username, password_hash, salt, role) VALUES ('u1','h','s',0), ('grpc','h','s',0);"
             "INSERT INTO api_keys (key_hash, client_name, role) VALUES ('s-u1','u1',0), ('s-sys','system',0), ('s-dash','-',0),"
             " ('s-kx',' k x',0), ('s-d1',' d1',0);"                                    
             "INSERT INTO deleted_user_names (username) VALUES ('d1');");
    pcv_rbac_init(g_db);
    gint w[7];
    pcv_rbac_names_startup_counts_for_test(w);
    g_assert_cmpint(w[0], ==, 1);              
    g_assert_cmpint(w[1], ==, 1);                
    g_assert_cmpint(w[2], ==, 2);                    
    g_assert_cmpint(w[3], ==, 3);                          
    g_assert_cmpint(w[4], ==, 1);                           
    g_assert_cmpint(w[5], ==, 0);           
    g_assert_cmpint(w[6], ==, 1);           
    names_teardown();
}

                                                                                                    
static void
test_startup_reconcile_pair(void)
{
    names_setup();
    pcv_rbac_shutdown();
    raw_exec("INSERT INTO users (username, password_hash, salt, role) VALUES ('r1','h','s',0);"
             "INSERT INTO deleted_user_names (username) VALUES (' r1'), ('gone9');");
    pcv_rbac_init(g_db);
    gint w[7];
    pcv_rbac_names_startup_counts_for_test(w);
    g_assert_cmpint(w[5], ==, 1);
    g_assert_cmpint(w[6], ==, 1);                    
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM deleted_user_names WHERE trim(username) = 'r1'", NULL), ==, 0);
    g_assert_cmpint(raw_int("SELECT COUNT(*) FROM users WHERE username = 'r1'", NULL), ==, 1);
    names_teardown();
}

void
test_rbac_names_register(void)
{
    g_test_add_func("/rbac_names/user_rejects_key_name", test_user_rejects_key_name);
    g_test_add_func("/rbac_names/user_rejects_reserved", test_user_rejects_reserved);
    g_test_add_func("/rbac_names/user_rejects_tombstone", test_user_rejects_tombstone);
    g_test_add_func("/rbac_names/key_rejects_user_name", test_key_rejects_user_name);
    g_test_add_func("/rbac_names/key_rejects_reserved_and_tombstone", test_key_rejects_reserved_and_tombstone);
    g_test_add_func("/rbac_names/key_same_name_second_allowed", test_key_same_name_second_allowed);
    g_test_add_func("/rbac_names/success_statement_count", test_success_statement_count);
    g_test_add_func("/rbac_names/bootstrap_refuses_principal_names", test_bootstrap_refuses_principal_names);
    g_test_add_func("/rbac_names/delete_writes_tombstone", test_delete_writes_tombstone);
    g_test_add_func("/rbac_names/delete_tombstone_failure_rolls_back", test_delete_tombstone_failure_rolls_back);
    g_test_add_func("/rbac_names/reuse_flag_scope", test_reuse_flag_scope);
    g_test_add_func("/rbac_names/deleted_name_add", test_deleted_name_add);
    g_test_add_func("/rbac_names/commit_failure_reported", test_commit_failure_reported);
    g_test_add_func("/rbac_names/commit_failure_leaves_no_open_transaction", test_commit_failure_leaves_no_open_transaction);
    g_test_add_func("/rbac_names/signup_conflict_classes", test_signup_conflict_classes);
    g_test_add_func("/rbac_names/revoke_and_verify_nonconforming_key", test_revoke_and_verify_nonconforming_key);
    g_test_add_func("/rbac_names/startup_counts", test_startup_counts);
    g_test_add_func("/rbac_names/startup_reconcile_pair", test_startup_reconcile_pair);
}
