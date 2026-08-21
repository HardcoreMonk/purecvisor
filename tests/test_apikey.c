                                                                                            
                                                                                            
                                                                                 
                                                              
                                    
                      
  
                                                              
                
  
                                          
                                                                
                                                                        
                                                     
  
                          
                                                                    
                                                                      
                                                                       
                                                                   
  
                                     
                                                                                  
                                                                
                                                                
                                               
                                    
   
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include "../src/modules/auth/pcv_rbac.h"                                          

                                                                           
#define APIKEY_CANONICAL_SCHEMA \
    "CREATE TABLE api_keys (" \
    "  key_hash     TEXT PRIMARY KEY," \
    "  client_name  TEXT NOT NULL," \
    "  role         INTEGER NOT NULL DEFAULT 1," \
    "  description  TEXT NOT NULL DEFAULT ''," \
    "  created_at   TEXT NOT NULL DEFAULT (datetime('now'))," \
    "  last_used_at TEXT," \
    "  expires_at   INTEGER NOT NULL DEFAULT 0," \
    "  revoked      INTEGER NOT NULL DEFAULT 0" \
    ")"

                                                                      
#define APIKEY_LEGACY_SCHEMA2 \
    "CREATE TABLE api_keys (" \
    "  key_hash     TEXT PRIMARY KEY," \
    "  client_name  TEXT NOT NULL," \
    "  role         INTEGER NOT NULL DEFAULT 1," \
    "  created_at   TEXT NOT NULL DEFAULT (datetime('now'))," \
    "  last_used_at TEXT," \
    "  revoked      INTEGER NOT NULL DEFAULT 0" \
    ")"

                                                         
#define APIKEY_VERIFY_SQL \
    "SELECT client_name FROM api_keys " \
    "WHERE key_hash = ? AND revoked = 0 " \
    "AND (expires_at = 0 OR expires_at > ?)"

                                                                       
                                                                
#define APIKEY_VERIFY_ROLE_SQL \
    "SELECT client_name, role FROM api_keys " \
    "WHERE key_hash = ? AND revoked = 0 " \
    "AND (expires_at = 0 OR expires_at > ?)"

                                                                     
#define USERS_MIN_SCHEMA \
    "CREATE TABLE users (" \
    "  username TEXT PRIMARY KEY NOT NULL," \
    "  role     INTEGER NOT NULL DEFAULT 0" \
    ")"

                                                   
#define APIKEY_ALTER_DESC "ALTER TABLE api_keys ADD COLUMN description TEXT NOT NULL DEFAULT ''"
#define APIKEY_ALTER_EXP  "ALTER TABLE api_keys ADD COLUMN expires_at  INTEGER NOT NULL DEFAULT 0"

                                                               

static sqlite3 *
_open_with(const char *schema_sql)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(":memory:", &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, schema_sql, NULL, NULL, NULL), ==, SQLITE_OK);
    return db;
}

static void
_insert_key(sqlite3 *db, const char *hash, const char *client,
            gint64 expires_at, int revoked)
{
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "INSERT INTO api_keys (key_hash, client_name, role, description, "
        "expires_at, revoked) VALUES (?,?,1,'',?,?)", -1, &st, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, client, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, expires_at);
    sqlite3_bind_int(st, 4, revoked);
    g_assert_cmpint(sqlite3_step(st), ==, SQLITE_DONE);
    sqlite3_finalize(st);
}

                                                                    
static gchar *
_verify(sqlite3 *db, const char *hash, gint64 now_epoch)
{
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, APIKEY_VERIFY_SQL, -1, &st, NULL),
                    ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now_epoch);
    gchar *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = g_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

static gboolean
_has_column(sqlite3 *db, const char *col)
{
    sqlite3_stmt *st = NULL;
    gboolean found = FALSE;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(api_keys)", -1, &st, NULL)
        == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 1);
            if (name && g_strcmp0(name, col) == 0) { found = TRUE; break; }
        }
    }
    sqlite3_finalize(st);
    return found;
}

                                                             

                                 
static void
test_apikey_expiry_never(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_never", "grafana", 0, 0);

    gchar *who = _verify(db, "hash_never", now);
    g_assert_nonnull(who);
    g_assert_cmpstr(who, ==, "grafana");
    g_free(who);
    sqlite3_close(db);
}

                
static void
test_apikey_expiry_future_accepted(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_future", "ci-bot", now + 3600, 0);

    gchar *who = _verify(db, "hash_future", now);
    g_assert_nonnull(who);
    g_assert_cmpstr(who, ==, "ci-bot");
    g_free(who);
    sqlite3_close(db);
}

                                 
static void
test_apikey_expiry_past_rejected(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_past", "expired-bot", now - 3600, 0);

    gchar *who = _verify(db, "hash_past", now);
    g_assert_null(who);                   
    sqlite3_close(db);
}

                                              
static void
test_apikey_expiry_boundary_now_rejected(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_boundary", "edge-bot", now, 0);

    gchar *who = _verify(db, "hash_boundary", now);
    g_assert_null(who);                                 
    sqlite3_close(db);
}

                                
static void
test_apikey_revoked_rejected(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_revoked", "old-bot", now + 100000, 1);

    gchar *who = _verify(db, "hash_revoked", now);
    g_assert_null(who);
    sqlite3_close(db);
}

                                                      

                                                                    
static void
test_apikey_migrate_columns_idempotent(void)
{
    sqlite3 *db = _open_with(APIKEY_LEGACY_SCHEMA2);

    g_assert_false(_has_column(db, "description"));
    g_assert_false(_has_column(db, "expires_at"));

                             
    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_DESC, NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_EXP,  NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_true(_has_column(db, "description"));
    g_assert_true(_has_column(db, "expires_at"));

                                                                    
    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_DESC, NULL, NULL, NULL), !=, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_EXP,  NULL, NULL, NULL), !=, SQLITE_OK);

                                   
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_migrated", "post-migrate", now + 3600, 0);
    gchar *who = _verify(db, "hash_migrated", now);
    g_assert_cmpstr(who, ==, "post-migrate");
    g_free(who);

    sqlite3_close(db);
}

                                                                   

                                                         
                                                                             
                                                                               
                                                                      
                                                                                         
static void
test_apikey_stored_role_enforced(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

                                           
    g_assert_cmpint(sqlite3_exec(db, USERS_MIN_SCHEMA, NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db,
        "INSERT INTO users (username, role) VALUES ('root', 2)",
        NULL, NULL, NULL), ==, SQLITE_OK);

                                                                                   
    g_assert_cmpint(sqlite3_exec(db,
        "INSERT INTO api_keys (key_hash, client_name, role, description, expires_at, revoked) "
        "VALUES ('hash_root', 'root', 0, '', 0, 0)", NULL, NULL, NULL), ==, SQLITE_OK);

                                                  
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, APIKEY_VERIFY_ROLE_SQL, -1, &st, NULL),
                    ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, "hash_root", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now);
    g_assert_cmpint(sqlite3_step(st), ==, SQLITE_ROW);
    const char *who = (const char *)sqlite3_column_text(st, 0);
    gint stored_role = sqlite3_column_int(st, 1);
    g_assert_cmpstr(who, ==, "root");
    sqlite3_finalize(st);

                                                  
    sqlite3_stmt *lu = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "SELECT role FROM users WHERE username = ?", -1, &lu, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(lu, 1, "root", -1, SQLITE_STATIC);
    g_assert_cmpint(sqlite3_step(lu), ==, SQLITE_ROW);
    gint live_role = sqlite3_column_int(lu, 0);
    sqlite3_finalize(lu);

                                                            
    g_assert_cmpint(stored_role, ==, 0);                        
    g_assert_cmpint(live_role,   ==, 2);                       
    g_assert_cmpint(stored_role, !=, live_role);

                                                           
    const gint admin_min = 2;
    g_assert_false(stored_role >= admin_min);                                              
    g_assert_true (live_role  >= admin_min);                                              

    sqlite3_close(db);
}

                                                               
  
                                                                   
                                                                  
                                                       
                                                                  
                                                                          
                                                     
                                                      
typedef enum { CREATE_OK, CREATE_RANGE, CREATE_EXCEEDS } ApikeyCreateVerdict;

                                                              
static ApikeyCreateVerdict
_apikey_create_bound(gint role, gint caller_role)
{
    const gint VIEWER = 0, ADMIN = 2;                
    if (role < VIEWER || role > ADMIN) return CREATE_RANGE;
    if (role > caller_role)            return CREATE_EXCEEDS;
    return CREATE_OK;
}

                                        
static void
test_apikey_rolecap_admin_grants_all(void)
{
    g_assert_cmpint(_apikey_create_bound(0, 2), ==, CREATE_OK);
    g_assert_cmpint(_apikey_create_bound(1, 2), ==, CREATE_OK);
    g_assert_cmpint(_apikey_create_bound(2, 2), ==, CREATE_OK);
}

                                                         
static void
test_apikey_rolecap_nonadmin_bounded(void)
{
                                                             
    g_assert_cmpint(_apikey_create_bound(2, 1), ==, CREATE_EXCEEDS);
    g_assert_cmpint(_apikey_create_bound(1, 1), ==, CREATE_OK);
    g_assert_cmpint(_apikey_create_bound(0, 1), ==, CREATE_OK);
                                                      
    g_assert_cmpint(_apikey_create_bound(2, 0), ==, CREATE_EXCEEDS);
    g_assert_cmpint(_apikey_create_bound(1, 0), ==, CREATE_EXCEEDS);
    g_assert_cmpint(_apikey_create_bound(0, 0), ==, CREATE_OK);
}

                                              
static void
test_apikey_rolecap_range_first(void)
{
    g_assert_cmpint(_apikey_create_bound(3, 2),  ==, CREATE_RANGE);
    g_assert_cmpint(_apikey_create_bound(-1, 2), ==, CREATE_RANGE);
    g_assert_cmpint(_apikey_create_bound(99, 2), ==, CREATE_RANGE);
}

                                                                         
                                                    
  
                                                                         
                                                             
                                                                      
                                      
  
                                                                          
                                                  
                                                         
                                                                 
                                                                            

static gchar *
_m2_tmp(const gchar *tag)
{
    return g_strdup_printf("%s/pcv-apikey-m2-%s-%u.db",
                           g_get_tmp_dir(), tag, g_random_int());
}

static void
_m2_clean(const gchar *path)
{
    gchar *wal = g_strdup_printf("%s-wal", path);
    gchar *shm = g_strdup_printf("%s-shm", path);
    g_unlink(path); g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm);
}

static gboolean
_m2_col(const gchar *path, const gchar *col)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) { if (db) sqlite3_close(db); return FALSE; }
    sqlite3_stmt *st = NULL;
    gboolean found = FALSE;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(api_keys)", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *n = (const char *)sqlite3_column_text(st, 1);
            if (n && g_strcmp0(n, col) == 0) { found = TRUE; break; }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return found;
}

static gboolean
_m2_table_exists(const gchar *path, const gchar *table)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) { if (db) sqlite3_close(db); return FALSE; }
    sqlite3_stmt *st = NULL;
    gboolean exists = FALSE;
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?;",
        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) exists = sqlite3_column_int(st, 0) != 0;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return exists;
}

                                                                        
                                                                  
static void
_m2_precreate_schema1(const gchar *path, gboolean with_row)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db,
        "CREATE TABLE api_keys ("
        "  key_hash    TEXT PRIMARY KEY,"
        "  username    TEXT NOT NULL,"
        "  description TEXT DEFAULT '',"
        "  created_at  INTEGER NOT NULL,"
        "  expires_at  INTEGER NOT NULL,"
        "  revoked     INTEGER DEFAULT 0"
        ");", NULL, NULL, NULL), ==, SQLITE_OK);
    if (with_row)
        g_assert_cmpint(sqlite3_exec(db,
            "INSERT INTO api_keys (key_hash, username, description, created_at, expires_at, revoked) "
            "VALUES ('legacyhash1', 'olduser', 'legacy key', 1700000000, 1999999999, 0);",
            NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(db);
}

                                                        
static gchar *
_m2_client_name(const gchar *path, const gchar *key_hash)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) { if (db) sqlite3_close(db); return NULL; }
    sqlite3_stmt *st = NULL;
    gchar *out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT client_name FROM api_keys WHERE key_hash = ?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key_hash, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            out = g_strdup((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

                                                                     
static gboolean
_m2_get_expires(const gchar *path, const gchar *key_hash, gint64 *val, gboolean *is_null)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) { if (db) sqlite3_close(db); return FALSE; }
    sqlite3_stmt *st = NULL;
    gboolean ok = FALSE;
    if (sqlite3_prepare_v2(db, "SELECT expires_at FROM api_keys WHERE key_hash = ?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key_hash, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            ok = TRUE;
            if (is_null) *is_null = (sqlite3_column_type(st, 0) == SQLITE_NULL);
            if (val)     *val     = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}

                                                                           
static void
test_apikey_real_roundtrip(void)
{
    gchar *path = _m2_tmp("roundtrip");
    _m2_clean(path);
    pcv_rbac_init(path);

    gchar *out_key = NULL;
    GError *err = NULL;
    g_assert_true(pcv_rbac_apikey_create("ci-bot", PCV_ROLE_OPERATOR, NULL, 0, &out_key, &err));
    g_assert_no_error(err);
    g_assert_nonnull(out_key);
    g_assert_cmpuint(strlen(out_key), ==, 68);
    g_assert_true(g_str_has_prefix(out_key, "pcv_"));

                                               
    g_assert_cmpint(pcv_rbac_apikey_validate(out_key), ==, (gint)PCV_ROLE_OPERATOR);

                             
    gchar *bogus = g_strdup(out_key);
    bogus[10] = (bogus[10] == 'a') ? 'b' : 'a';
    g_assert_cmpint(pcv_rbac_apikey_validate(bogus), ==, -1);
    g_free(bogus);

                   
    JsonArray *arr = pcv_rbac_apikey_list();
    g_assert_nonnull(arr);
    g_assert_cmpuint(json_array_get_length(arr), ==, 1);
    JsonObject *item = json_array_get_object_element(arr, 0);
    g_assert_cmpstr(json_object_get_string_member(item, "client_name"), ==, "ci-bot");
    json_array_unref(arr);

                     
    GError *rerr = NULL;
    g_assert_true(pcv_rbac_apikey_revoke("ci-bot", &rerr));
    g_assert_no_error(rerr);
    g_assert_cmpint(pcv_rbac_apikey_validate(out_key), ==, -1);

    g_free(out_key);
    pcv_rbac_shutdown();
    _m2_clean(path);
    g_free(path);
}

                                                                     
static void
test_apikey_real_migrate_schema1_empty(void)
{
    gchar *path = _m2_tmp("s1-empty");
    _m2_clean(path);
    _m2_precreate_schema1(path, FALSE);
    g_assert_false(_m2_col(path, "client_name"));                                 

    pcv_rbac_init(path);                                                                       

    g_assert_true(_m2_col(path, "client_name"));
    g_assert_true(_m2_col(path, "role"));
    g_assert_false(_m2_table_exists(path, "api_keys_new"));

                                                         
    gchar *out_key = NULL;
    GError *err = NULL;
    g_assert_true(pcv_rbac_apikey_create("post-migration", PCV_ROLE_ADMIN, NULL, 0, &out_key, &err));
    g_assert_no_error(err);
    g_assert_cmpint(pcv_rbac_apikey_validate(out_key), ==, (gint)PCV_ROLE_ADMIN);

    g_free(out_key);
    pcv_rbac_shutdown();
    _m2_clean(path);
    g_free(path);
}

                                                                                        
static void
test_apikey_real_migrate_schema1_rows(void)
{
    gchar *path = _m2_tmp("s1-rows");
    _m2_clean(path);
    _m2_precreate_schema1(path, TRUE);

    pcv_rbac_init(path);                                                

    g_assert_true(_m2_col(path, "client_name"));
    g_assert_false(_m2_table_exists(path, "api_keys_new"));

                                                             
                                                                          
    gchar *cn = _m2_client_name(path, "legacyhash1");
    g_assert_cmpstr(cn, ==, "olduser");
    g_free(cn);

                                 
    gchar *out_key = NULL;
    GError *err = NULL;
    g_assert_true(pcv_rbac_apikey_create("after-migration", PCV_ROLE_VIEWER, NULL, 0, &out_key, &err));
    g_assert_no_error(err);
    g_assert_nonnull(out_key);

    g_free(out_key);
    pcv_rbac_shutdown();
    _m2_clean(path);
    g_free(path);
}

                                                              
static void
test_apikey_real_migration_idempotent(void)
{
    gchar *path = _m2_tmp("idem");
    _m2_clean(path);

    pcv_rbac_init(path);
    gchar *out_key = NULL;
    GError *err = NULL;
    g_assert_true(pcv_rbac_apikey_create("stable-client", PCV_ROLE_OPERATOR, NULL, 0, &out_key, &err));
    g_assert_no_error(err);
    pcv_rbac_shutdown();

                                                                             
                                                  
    pcv_rbac_init(path);
    g_assert_cmpint(pcv_rbac_apikey_validate(out_key), ==, (gint)PCV_ROLE_OPERATOR);
    g_assert_false(_m2_table_exists(path, "api_keys_new"));

    g_free(out_key);
    pcv_rbac_shutdown();
    _m2_clean(path);
    g_free(path);
}

                                                                               
  
                                                                 
                                                                    
                                                               
                                                                       
                                                                          
static void
test_apikey_real_legacy_null_expires_normalized(void)
{
    gchar *path = _m2_tmp("null-exp");
    _m2_clean(path);

                                                                                 
                                                                    
    const gchar *plain = "pcv_0000000000000000000000000000000000000000000000000000000000000000";
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, plain, -1);
    {
        sqlite3 *db = NULL;
        g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
        g_assert_cmpint(sqlite3_exec(db,
            "CREATE TABLE api_keys ("
            "  key_hash     TEXT PRIMARY KEY,"
            "  username     TEXT,"
            "  client_name  TEXT,"
            "  description  TEXT DEFAULT '',"
            "  role         INTEGER NOT NULL DEFAULT 1,"
            "  created_at   TEXT NOT NULL DEFAULT (datetime('now')),"
            "  expires_at   INTEGER,"                                       
            "  last_used_at TEXT,"
            "  revoked      INTEGER NOT NULL DEFAULT 0"
            ");", NULL, NULL, NULL), ==, SQLITE_OK);
        sqlite3_stmt *st = NULL;
        g_assert_cmpint(sqlite3_prepare_v2(db,
            "INSERT INTO api_keys (key_hash, client_name, role, expires_at, revoked) "
            "VALUES (?, 'null-exp-bot', 1, NULL, 0)", -1, &st, NULL), ==, SQLITE_OK);
        sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC);
        g_assert_cmpint(sqlite3_step(st), ==, SQLITE_DONE);
        sqlite3_finalize(st);
        sqlite3_close(db);
    }

                                      
    gint64 v = -1; gboolean is_null = FALSE;
    g_assert_true(_m2_get_expires(path, hash, &v, &is_null));
    g_assert_true(is_null);

    pcv_rbac_init(path);                                                

                                                 
    v = -1; is_null = TRUE;
    g_assert_true(_m2_get_expires(path, hash, &v, &is_null));
    g_assert_false(is_null);
    g_assert_cmpint(v, ==, 0);

                                                           
                                                                         
    g_assert_cmpint(pcv_rbac_apikey_validate(plain), !=, -1);

    g_free(hash);
    pcv_rbac_shutdown();
    _m2_clean(path);
    g_free(path);
}

void
test_apikey_register(void)
{
    g_test_add_func("/apikey/expiry/never_expires",       test_apikey_expiry_never);
    g_test_add_func("/apikey/expiry/future_accepted",     test_apikey_expiry_future_accepted);
    g_test_add_func("/apikey/expiry/past_rejected",       test_apikey_expiry_past_rejected);
    g_test_add_func("/apikey/expiry/boundary_now_rejected", test_apikey_expiry_boundary_now_rejected);
    g_test_add_func("/apikey/revoked_rejected",           test_apikey_revoked_rejected);
    g_test_add_func("/apikey/migrate/columns_idempotent", test_apikey_migrate_columns_idempotent);
    g_test_add_func("/apikey/sec3/stored_role_enforced",  test_apikey_stored_role_enforced);
    g_test_add_func("/apikey/rolecap/admin_grants_all",   test_apikey_rolecap_admin_grants_all);
    g_test_add_func("/apikey/rolecap/nonadmin_bounded",   test_apikey_rolecap_nonadmin_bounded);
    g_test_add_func("/apikey/rolecap/range_first",        test_apikey_rolecap_range_first);

                                                                        
    g_test_add_func("/apikey/real/roundtrip",                    test_apikey_real_roundtrip);
    g_test_add_func("/apikey/real/migrate_schema1_empty",        test_apikey_real_migrate_schema1_empty);
    g_test_add_func("/apikey/real/migrate_schema1_rows",         test_apikey_real_migrate_schema1_rows);
    g_test_add_func("/apikey/real/migration_idempotent",         test_apikey_real_migration_idempotent);
    g_test_add_func("/apikey/real/legacy_null_expires_normalized", test_apikey_real_legacy_null_expires_normalized);
}
