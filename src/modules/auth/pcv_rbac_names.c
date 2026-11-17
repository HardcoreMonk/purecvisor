   
                         
                                                                                     
                                            
  
                           
                                                                              
                                                                                                       
                                                                                               
                                                                                               
                                                                                                
                                                                              
                                                                                               
                                                                                              
                                                                          
                                                                                                   
                                                       
                                                                             
                                                                                    
  
                                                                               
                                                              
   
#include "pcv_rbac_internal.h"
#include "pcv_principal_names.h"
#include "../resource/resource_registry.h"                                          
#include "../../utils/pcv_log.h"

#include <gio/gio.h>
#include <sqlite3.h>
#include <string.h>

#define RBAC_LOG_DOM "pcv_rbac"
                                                    
#define N_SQL(x) "trim(" x ", " PCV_PRINCIPAL_TRIM_SQL ")"

                                                                     
static gint g_last_counts[7] = { -1, -1, -1, -1, -1, -1, -1 };

                                   
static gint
_count_int(sqlite3 *db, const gchar *sql)
{
    sqlite3_stmt *st = nullptr;
    gint v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

                                                                                                   
static gint
_count_names(sqlite3 *db, const gchar *sql, gboolean (*pred)(const gchar *))
{
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    gint n = 0;
    while (sqlite3_step(st) == SQLITE_ROW)
        if (pred((const gchar *)sqlite3_column_text(st, 0)))
            n++;
    sqlite3_finalize(st);
    return n;
}

static gboolean
_reserved_n(const gchar *name)
{
    g_autofree gchar *n = pcv_principal_name_normalize(name);
    return n && pcv_principal_name_is_reserved(n);
}

static gboolean
_outside_ra7(const gchar *name)
{
    return !pcv_res_name_is_valid(name);
}

                                                                                                       
                                                                
                                                                         
static void
_startup_reconcile_and_count(sqlite3 *db)
{
    gint w6 = -1;
    if (sqlite3_exec(db, "DELETE FROM deleted_user_names WHERE EXISTS (SELECT 1 FROM users WHERE "
                         N_SQL("users.username") " = " N_SQL("deleted_user_names.username") ");",
                     NULL, NULL, NULL) == SQLITE_OK)
        w6 = sqlite3_changes(db);
    gint w[7] = {
        _count_int(db, "SELECT COUNT(*) FROM users u WHERE EXISTS (SELECT 1 FROM api_keys k WHERE "
                       N_SQL("k.client_name") " = " N_SQL("u.username") ");"),
        _count_names(db, "SELECT username FROM users;", _reserved_n),
        _count_names(db, "SELECT client_name FROM api_keys;", _reserved_n),
        _count_names(db, "SELECT client_name FROM api_keys;", _outside_ra7),
        _count_int(db, "SELECT COUNT(*) FROM deleted_user_names d WHERE EXISTS (SELECT 1 FROM api_keys k WHERE "
                       N_SQL("k.client_name") " = " N_SQL("d.username") ");"),
        w6,
        _count_int(db, "SELECT COUNT(*) FROM deleted_user_names;"),
    };
    memcpy(g_last_counts, w, sizeof(w));
    gboolean overlap = FALSE;
    for (gint i = 0; i < 6; i++)
        if (w[i] != 0)
            overlap = TRUE;
    if (overlap)
        PCV_LOG_WARN(RBAC_LOG_DOM, "rbac principal names: same-name user/key pairs=%d, reserved users=%d, reserved keys=%d, "
                     "non-RA7 key names=%d, tombstone/key overlaps=%d, tombstones reconciled=%d, tombstones=%d",
                     w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
    else
        PCV_LOG_INFO(RBAC_LOG_DOM, "rbac principal names: same-name user/key pairs=%d, reserved users=%d, reserved keys=%d, "
                     "non-RA7 key names=%d, tombstone/key overlaps=%d, tombstones reconciled=%d, tombstones=%d",
                     w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
}

                                                          
void
pcv_rbac_names_startup_counts_for_test(gint out[7])
{
    memcpy(out, g_last_counts, sizeof(g_last_counts));
}

                                                                  
void
pcv_rbac_names_migrate_locked(sqlite3 *db)
{
    if (!db)
        return;
    if (sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS deleted_user_names ("
            "  username   TEXT PRIMARY KEY NOT NULL,"
            "  deleted_at TEXT NOT NULL DEFAULT (datetime('now'))"
            ");", NULL, NULL, NULL) != SQLITE_OK)
                                                                  
        PCV_LOG_ERROR(RBAC_LOG_DOM, "principal names: tombstone table ensure failed: %s", sqlite3_errmsg(db));
    _startup_reconcile_and_count(db);
}

                                                                           
gboolean
pcv_rbac_names_tombstone_locked(sqlite3 *db, const gchar *username)
{
    sqlite3_stmt *stmt = nullptr;
    if (!db || sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO deleted_user_names (username, deleted_at) VALUES (?1, datetime('now'));",
            -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

                                                                                  
                                     
                                                                         
static gboolean
_reuse_begin_locked(sqlite3 *db, const gchar *username, gint *out_removed, GError **error)
{
    *out_removed = 0;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", sqlite3_errmsg(db));
        return FALSE;
    }
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "DELETE FROM deleted_user_names WHERE " N_SQL("username") " = " N_SQL("?1") ";", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        *out_removed = sqlite3_changes(db);
        sqlite3_finalize(stmt);
    }
    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return FALSE;
    }
    return TRUE;
}

                                                                                          
                                                                                             
                                                                          
static void
_refusal_locked(sqlite3 *db, const char *sql, const gchar *name, gint other_code, const gchar *other_msg,
                const gchar *deleted_msg, GError **error)
{
    sqlite3_stmt *stmt = nullptr;
    gint other = 0, deleted = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            other = sqlite3_column_int(stmt, 0);
            deleted = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }
    if (other)
        g_set_error_literal(error, PCV_PRINCIPAL_ERROR, other_code, other_msg);
    else if (deleted)
        g_set_error_literal(error, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_DELETED, deleted_msg);
    else
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "name check failed: %s", sqlite3_errmsg(db));
}

                                                                                         
gboolean
pcv_rbac_names_insert_user_locked(sqlite3 *db, const gchar *username, const gchar *hash, const gchar *salt,
                                  PcvRole role, const gchar *tenant, const gchar *nonce,
                                  gboolean reuse_deleted_name, gboolean *out_reused, GError **error)
{
    if (out_reused)
        *out_reused = FALSE;
    g_autofree gchar *norm = pcv_principal_name_normalize(username);
    if (pcv_principal_name_is_reserved(norm)) {
        g_set_error_literal(error, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_RESERVED, "username is reserved");
        return FALSE;
    }
    if (!db) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB prepare failed: %s", "RBAC DB not initialized");
        return FALSE;
    }

    gint removed = 0;                                                             
    if (reuse_deleted_name && !_reuse_begin_locked(db, username, &removed, error))
        return FALSE;
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO users (username, password_hash, salt, role, tenant, identity_nonce) "
        "SELECT ?1, ?2, ?3, ?4, ?5, ?6 "
        "WHERE NOT EXISTS (SELECT 1 FROM api_keys WHERE " N_SQL("client_name") " = " N_SQL("?1") ") "
        "AND NOT EXISTS (SELECT 1 FROM deleted_user_names WHERE " N_SQL("username") " = " N_SQL("?1") ");",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB prepare failed: %s", sqlite3_errmsg(db));
        if (reuse_deleted_name)
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return FALSE;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, salt, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, (int)role);
    if (tenant)
        sqlite3_bind_text(stmt, 5, tenant, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);                                                      
    sqlite3_bind_text(stmt, 6, nonce, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
                                                                                           
                                                                                                        
        gint ext = sqlite3_extended_errcode(db);
        if (ext == SQLITE_CONSTRAINT_PRIMARYKEY || ext == SQLITE_CONSTRAINT_UNIQUE)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "User '%s' already exists or DB error: %s",
                        username, sqlite3_errmsg(db));
        else
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        if (reuse_deleted_name)
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);                    
        return FALSE;
    }
    sqlite3_finalize(stmt);
    gboolean inserted = sqlite3_changes(db) > 0;
    if (reuse_deleted_name && inserted && sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return FALSE;
    }
    if (inserted) {
        if (out_reused)
            *out_reused = removed > 0;
        return TRUE;
    }
    if (reuse_deleted_name)
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);                                              
                                          
    _refusal_locked(db,
        "SELECT EXISTS (SELECT 1 FROM api_keys WHERE " N_SQL("client_name") " = " N_SQL("?1") "), "
        "EXISTS (SELECT 1 FROM deleted_user_names WHERE " N_SQL("username") " = " N_SQL("?1") ");",
        username, PCV_PRINCIPAL_ERROR_KEY_NAME, "username is already used by an API key",
        "username belongs to a deleted account — set reuse_deleted_name to recreate it with that account's ownership records",
        error);
    return FALSE;
}

                                                                                          
                                                                  
gboolean
pcv_rbac_names_insert_apikey_locked(sqlite3 *db, const gchar *key_hash, const gchar *client_name, PcvRole role,
                                    const gchar *description, gint64 expires_at, GError **error)
{
    g_autofree gchar *norm = pcv_principal_name_normalize(client_name);
    if (pcv_principal_name_is_reserved(norm)) {
        g_set_error_literal(error, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_RESERVED, "name is reserved");
        return FALSE;
    }
    sqlite3_stmt *stmt = nullptr;
    int rc = db ? sqlite3_prepare_v2(db,
        "INSERT INTO api_keys (key_hash, client_name, role, description, expires_at) "
        "SELECT ?1, ?2, ?3, ?4, ?5 "
        "WHERE NOT EXISTS (SELECT 1 FROM users WHERE " N_SQL("username") " = " N_SQL("?2") ") "
        "AND NOT EXISTS (SELECT 1 FROM deleted_user_names WHERE " N_SQL("username") " = " N_SQL("?2") ");",
        -1, &stmt, NULL) : SQLITE_MISUSE;
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, client_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, (int)role);
        sqlite3_bind_text(stmt, 4, description ? description : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, expires_at > 0 ? expires_at : 0);
        rc = sqlite3_step(stmt);
    }
    if (stmt)
        sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create API key");                  
        return FALSE;
    }
    if (sqlite3_changes(db) > 0)
        return TRUE;
                                            
    _refusal_locked(db,
        "SELECT EXISTS (SELECT 1 FROM users WHERE " N_SQL("username") " = " N_SQL("?1") "), "
        "EXISTS (SELECT 1 FROM deleted_user_names WHERE " N_SQL("username") " = " N_SQL("?1") ");",
        client_name, PCV_PRINCIPAL_ERROR_USER_NAME, "name is already used by a user account",
        "name belongs to a deleted account", error);
    return FALSE;
}

                                                                                
                                                   
gboolean
pcv_rbac_deleted_name_add(const gchar *username, gboolean *out_added, GError **error)
{
    if (out_added)
        *out_added = FALSE;
    g_autofree gchar *norm = pcv_principal_name_normalize(username);
    if (!norm || !*norm) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "username must be a non-empty string");
        return FALSE;
    }
    if (pcv_principal_name_is_reserved(norm)) {
        g_set_error_literal(error, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_RESERVED, "name is reserved");
        return FALSE;
    }
    sqlite3 *db = pcv_rbac_db_lock();
    if (!db || sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", db ? sqlite3_errmsg(db) : "RBAC DB not initialized");
        pcv_rbac_db_unlock();
        return FALSE;
    }
                                                   
    sqlite3_stmt *stmt = nullptr;
    gint user = -1, key = -1;
    if (sqlite3_prepare_v2(db,
            "SELECT EXISTS (SELECT 1 FROM users WHERE " N_SQL("username") " = " N_SQL("?1") "), "
            "EXISTS (SELECT 1 FROM api_keys WHERE " N_SQL("client_name") " = " N_SQL("?1") ");", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            user = sqlite3_column_int(stmt, 0);
            key = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }
    gboolean ok = FALSE;
    if (user < 0)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", sqlite3_errmsg(db));
    else if (user)
        g_set_error_literal(error, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_EXISTING_USER,
                            "username belongs to an existing account — delete the account instead");
    else if (key)
        g_set_error_literal(error, PCV_PRINCIPAL_ERROR, PCV_PRINCIPAL_ERROR_KEY_NAME, "name is already used by an API key");
    else if (sqlite3_prepare_v2(db,
                 "INSERT OR IGNORE INTO deleted_user_names (username) SELECT ?1 "
                 "WHERE NOT EXISTS (SELECT 1 FROM deleted_user_names WHERE " N_SQL("username") " = " N_SQL("?1") ");",
                 -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        gboolean added = ok && sqlite3_changes(db) > 0;
        sqlite3_finalize(stmt);
        ok = ok && sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK;
        if (ok && out_added)
            *out_added = added;
        if (!ok)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", sqlite3_errmsg(db));
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", sqlite3_errmsg(db));
    }
    if (!ok)
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    pcv_rbac_db_unlock();
    return ok;
}

                                                           
                                                   
gboolean
pcv_rbac_user_create(const gchar *username, const gchar *password, PcvRole role, const gchar *tenant, GError **error)
{
    return pcv_rbac_user_create_ex(username, password, role, tenant, PCV_RBAC_CREATE_DEFAULT, NULL, error);
}

                                                                                   
                                                     
void
pcv_rbac_names_log_bootstrap_refusal(const gchar *admin_user, const GError *err)
{
    if (err && err->domain == PCV_PRINCIPAL_ERROR) {
        PCV_LOG_WARN(RBAC_LOG_DOM,
                     "Bootstrap admin user '%s' not created: the name is reserved, used by an API key, or belongs to a "
                     "deleted account — create an admin with another name over the local UDS (pcvctl auth create <name> "
                     "<password> admin) or recreate it explicitly (pcvctl auth create ... --reuse-deleted-name)",
                     admin_user);
        return;
    }
    PCV_LOG_ERROR(RBAC_LOG_DOM, "Failed to auto-create admin user: %s", err ? err->message : "unknown");
}
