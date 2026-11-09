   
                            
                                                                                    
  
                           
                                                                       
                                                                                                      
                                                                                             
                                                                                                                 
                                                                                           
                                                                                                           
                                                                                
                                                                                                  
                                                                                             
                                                                                                        
                                                                                                         
                                                                          
                                                                       
  
                                                                       
                                                                  
                                         
   
#include "pcv_rbac.h"
#include "pcv_rbac_internal.h"
#include "../../utils/pcv_jwt.h"
#include "../../utils/pcv_log.h"
#include "../audit/pcv_audit.h"

#include <glib.h>
#include <gio/gio.h>
#include <sqlite3.h>
#include <openssl/crypto.h>
#include <string.h>

#define RBAC_LOG_DOM "pcv_rbac"

                                                                                                          
#define PCV_RBAC_USER_VERSION_APIKEY_STORED_ROLE 2

                                                     
static gint g_last_filled_users = -1, g_last_revoked_sessions = -1, g_last_downgraded_keys = -1;

                                                  
                                                                      
                                                                             
static gboolean
_identity_nonce_equal(const gchar *a, const gchar *b)
{
    if (!a || !b || !*a || !*b) return FALSE;
    gsize la = strlen(a), lb = strlen(b);
    if (la != lb) return FALSE;                                          
    return CRYPTO_memcmp(a, b, la) == 0;
}

                                                                                         
                                                                 
                                                                                               
static PcvUserExistence
_lookup_locked(sqlite3 *db, const gchar *username, PcvRole *out_role, gchar **out_nonce)
{
    if (out_role) *out_role = PCV_ROLE_VIEWER;
    if (out_nonce) *out_nonce = NULL;
    if (!username || !*username) return PCV_USER_ABSENT;
    if (!db) return PCV_USER_UNKNOWN;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT role, identity_nonce FROM users WHERE username = ? LIMIT 1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return PCV_USER_UNKNOWN;                                         
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    PcvUserExistence result = PCV_USER_UNKNOWN;
    if (rc == SQLITE_ROW) {
        gint role = sqlite3_column_int(stmt, 0);
        if (out_role)                                                        
            *out_role = (role >= PCV_ROLE_VIEWER && role <= PCV_ROLE_ADMIN) ? (PcvRole)role : PCV_ROLE_VIEWER;
        if (out_nonce) {
            const unsigned char *n = sqlite3_column_text(stmt, 1);
            *out_nonce = g_strdup(n ? (const gchar *)n : "");
        }
        result = PCV_USER_PRESENT;
    } else if (rc == SQLITE_DONE) {
        result = PCV_USER_ABSENT;
    }
    sqlite3_finalize(stmt);
    return result;
}

                                                                          
PcvUserExistence
pcv_rbac_lookup_subject_locked(sqlite3 *db, const gchar *username, PcvRole *out_role, gchar **out_nonce)
{
    return _lookup_locked(db, username, out_role, out_nonce);
}

                                                                        
PcvSubjectCheck
pcv_rbac_identity_check_locked(sqlite3 *db, const gchar *username,
                               const gchar *identity_nonce, PcvRole *out_role)
{
    PcvRole role = PCV_ROLE_VIEWER;
    gchar *stored = NULL;
    PcvUserExistence ex = _lookup_locked(db, username, &role, &stored);
                                                                                     
                                                                                           
    PcvSubjectCheck verdict = (ex == PCV_USER_UNKNOWN) ? PCV_SUBJECT_UNAVAILABLE
        : (ex == PCV_USER_PRESENT && _identity_nonce_equal(stored, identity_nonce)) ? PCV_SUBJECT_OK
        : PCV_SUBJECT_REJECT;
    if (verdict == PCV_SUBJECT_OK && out_role) *out_role = role;
    g_free(stored);
    return verdict;
}

                                                                       
PcvSubjectCheck
pcv_rbac_subject_check(const gchar *username, const gchar *identity_nonce, PcvRole *out_role)
{
    sqlite3 *db = pcv_rbac_db_lock();
    PcvSubjectCheck verdict = pcv_rbac_identity_check_locked(db, username, identity_nonce, out_role);
    pcv_rbac_db_unlock();
    return verdict;
}

                                                                         
PcvSubjectCheck
pcv_rbac_subject_check_token(const gchar *subject, const gchar *verified_token, PcvRole *out_role)
{
    g_autofree gchar *claim = pcv_jwt_identity_nonce(verified_token);                               
    return pcv_rbac_subject_check(subject, claim, out_role);
}

                                                                         
PcvUserExistence
pcv_rbac_lookup_subject(const gchar *username, PcvRole *out_role, gchar **out_identity_nonce)
{
    if (out_role) *out_role = PCV_ROLE_VIEWER;
    if (out_identity_nonce) *out_identity_nonce = NULL;
    if (!username || !*username) return PCV_USER_ABSENT;
    sqlite3 *db = pcv_rbac_db_lock();
    PcvUserExistence ex = _lookup_locked(db, username, out_role, out_identity_nonce);
    pcv_rbac_db_unlock();
    return ex;
}

   
                        
                        
  
                                                                         
                                                                            
                                                             
  
                                                                          
  
                                                                                                        
   
PcvUserExistence
pcv_rbac_user_exists(const gchar *username)
{
    return pcv_rbac_lookup_subject(username, NULL, NULL);
}

                                                                 
                                   
static void
_rollback(sqlite3 *db)
{
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
}

   
                        
                        
                       
  
                                                                                                
                                                                                             
                                                                        
                                                                  
  
                                                                    
                                                                     
  
                                                                                         
                                                             
   
gboolean
pcv_rbac_user_delete(const gchar *username, GError **error)
{
    g_return_val_if_fail(username && *username, FALSE);

    sqlite3 *db = pcv_rbac_db_lock();
    if (!db || sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s",
                    db ? sqlite3_errmsg(db) : "RBAC DB not initialized");
        pcv_rbac_db_unlock();
        return FALSE;
    }

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, "DELETE FROM users WHERE username = ?;", -1, &stmt, NULL);
    int changes = 0;
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        changes = sqlite3_changes(db);                              
        sqlite3_finalize(stmt);
    }
    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", sqlite3_errmsg(db));
        _rollback(db);
        pcv_rbac_db_unlock();
        return FALSE;
    }
    if (changes == 0) {
                                                             
        _rollback(db);
        pcv_rbac_db_unlock();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "User '%s' not found", username);
        return FALSE;
    }

                                                                    
    if (!pcv_rbac_totp_delete_rows_locked(username)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", sqlite3_errmsg(db));
        _rollback(db);
        pcv_rbac_db_unlock();
        return FALSE;
    }

                                                             
    int revoked = 0;
    stmt = nullptr;
    rc = sqlite3_prepare_v2(db, "UPDATE sessions SET revoked = 1 WHERE username = ? AND revoked = 0;",
                            -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        revoked = sqlite3_changes(db);
        sqlite3_finalize(stmt);
    }
    if (rc != SQLITE_DONE || sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", sqlite3_errmsg(db));
        _rollback(db);
        pcv_rbac_db_unlock();
        return FALSE;
    }
    pcv_rbac_db_unlock();

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.user.delete", username,
                  "User deleted (refresh sessions revoked: %d)", revoked);
    return TRUE;                                                                             
}

                                                                              
                                                                      
                                                                                   
static gboolean
_store_session(sqlite3 *db, const gchar *username, const gchar *token_hash, const gchar *identity_nonce)
{
    sqlite3_stmt *stmt = nullptr;
    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;                    

    if (sqlite3_prepare_v2(db,
            "INSERT INTO sessions (username, refresh_token_hash, created_at, expires_at, revoked, identity_nonce) "
            "VALUES (?, ?, ?, ?, 0, ?);", -1, &stmt, NULL) != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM, "Failed to prepare session INSERT: %s", sqlite3_errmsg(db));
        return FALSE;
    }
    sqlite3_bind_text (stmt, 1, username,   -1, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 2, token_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_bind_int64(stmt, 4, now + PCV_RBAC_REFRESH_TOKEN_EXPIRY);
    sqlite3_bind_text (stmt, 5, identity_nonce, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        PCV_LOG_ERROR(RBAC_LOG_DOM, "Failed to store session: %s", sqlite3_errmsg(db));
        return FALSE;
    }
    return TRUE;
}

   
                         
  
                                                                                
                                                                                 
                                                                                                  
                                             
  
                                                                     
                                                       
  
                                                                                            
                                                                   
                                                                                   
                                                                  
                                                                
                                                                                                     
   
gchar *
pcv_rbac_issue_tokens(const gchar *username,
                      const gchar *expected_identity_nonce,
                      gchar      **out_refresh_token,
                      GError     **error)
{
    g_return_val_if_fail(username && *username, NULL);

    sqlite3 *db = pcv_rbac_db_lock();
    PcvSubjectCheck who = pcv_rbac_identity_check_locked(db, username, expected_identity_nonce, NULL);
    if (who != PCV_SUBJECT_OK) {
                                                                             
        pcv_rbac_db_unlock();
        if (who == PCV_SUBJECT_UNAVAILABLE)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "User lookup failed");
        else
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "User '%s' no longer exists", username);
        return NULL;
    }

    gchar *refresh = nullptr;
    if (out_refresh_token) {
        refresh = pcv_rbac_refresh_token_new();
        gchar *hash = pcv_rbac_refresh_token_hash(refresh);                   
        gboolean stored = _store_session(db, username, hash, expected_identity_nonce);
        g_free(hash);
        if (!stored) {
            g_free(refresh);
            pcv_rbac_db_unlock();
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create session");
            return NULL;
        }
    }
    pcv_rbac_db_unlock();

                                                         
    gchar *token = pcv_jwt_sign_scoped(username, PCV_RBAC_ACCESS_TOKEN_EXPIRY, NULL, NULL,
                                       expected_identity_nonce, error);
    if (token) {
        if (out_refresh_token)
            *out_refresh_token = refresh;
        PCV_LOG_INFO(RBAC_LOG_DOM, "User '%s' authenticated (v2, refresh=%s)",
                     username, out_refresh_token ? "yes" : "no");
        pcv_audit_log(username, "auth.login", username, "ok", 0, 0, NULL);
    } else {
        g_free(refresh);
    }
    return token;
}

   
                            
  
                                                                                             
                                                                                               
                                                     
                                                                                    
                                                                  
                                                                                                     
   
                                                      
                                                                
gchar *
pcv_rbac_authenticate_v2(const gchar *username,
                         const gchar *password,
                         gchar      **out_refresh_token,
                         GError     **error)
{
                                                                                   
    g_autofree gchar *nonce = NULL;
    if (!pcv_rbac_password_check_subject(username, password, NULL, &nonce, error))
        return NULL;
    if (!nonce) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "User lookup failed");
        return NULL;
    }
    return pcv_rbac_issue_tokens(username, nonce, out_refresh_token, error);
}

   
                          
  
                                                                                      
                                                                         
                                                                                                 
                                                                         
  
                                                                 
                                                             
  
                                                                                                
                                                                                        
                                                                          
   
gchar *
pcv_rbac_refresh_token(const gchar *refresh_token,
                       gchar      **out_new_refresh,
                       GError     **error)
{
    g_return_val_if_fail(refresh_token && *refresh_token, NULL);

    gchar *token_hash = pcv_rbac_refresh_token_hash(refresh_token);                           
    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;

    sqlite3 *db = pcv_rbac_db_lock();
    sqlite3_stmt *stmt = nullptr;
    int rc = db ? sqlite3_prepare_v2(db,
        "SELECT id, username, identity_nonce FROM sessions "
        "WHERE refresh_token_hash = ? AND revoked = 0 AND expires_at > ?;",
        -1, &stmt, NULL) : SQLITE_MISUSE;
    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB prepare failed: %s",
                    db ? sqlite3_errmsg(db) : "RBAC DB not initialized");
        pcv_rbac_db_unlock();
        g_free(token_hash);
        return NULL;
    }
    sqlite3_bind_text (stmt, 1, token_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);
    rc = sqlite3_step(stmt);
    g_free(token_hash);

    if (rc != SQLITE_ROW) {
                                                                
        sqlite3_finalize(stmt);
        pcv_rbac_db_unlock();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Invalid or expired refresh token");
        PCV_LOG_WARN(RBAC_LOG_DOM, "Refresh token validation failed");
        pcv_audit_log("-", "auth.token.refresh", "unknown", "invalid token", 401, 0, NULL);                                       
        return NULL;
    }

    gint64 session_id = sqlite3_column_int64(stmt, 0);                 
    g_autofree gchar *username = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
    const unsigned char *sn = sqlite3_column_text(stmt, 2);
    g_autofree gchar *session_nonce = g_strdup(sn ? (const gchar *)sn : "");
    sqlite3_finalize(stmt);

                                                                    
    PcvSubjectCheck who = pcv_rbac_identity_check_locked(db, username, session_nonce, NULL);
    if (who == PCV_SUBJECT_UNAVAILABLE) {
        pcv_rbac_db_unlock();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Authentication store unavailable");
        return NULL;
    }

                                                                        
    stmt = nullptr;
    if (sqlite3_prepare_v2(db, "UPDATE sessions SET revoked = 1 WHERE id = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, session_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (who == PCV_SUBJECT_REJECT) {
        pcv_rbac_db_unlock();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Invalid or expired refresh token");
        PCV_LOG_WARN(RBAC_LOG_DOM, "Refresh token rejected: session subject no longer valid (session revoked)");
        pcv_audit_log("-", "auth.token.refresh", "unknown", "invalid token", 401, 0, NULL);                                       
        return NULL;
    }

    gchar *new_refresh = nullptr;
    if (out_new_refresh) {
        new_refresh = pcv_rbac_refresh_token_new();
        gchar *new_hash = pcv_rbac_refresh_token_hash(new_refresh);
        gboolean stored = _store_session(db, username, new_hash, session_nonce);
        g_free(new_hash);
        if (!stored) {
            g_free(new_refresh);
            pcv_rbac_db_unlock();
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to rotate refresh token");
            return NULL;
        }
    }
    pcv_rbac_db_unlock();

    gchar *access_token = pcv_jwt_sign_scoped(username, PCV_RBAC_ACCESS_TOKEN_EXPIRY, NULL, NULL,
                                              session_nonce, error);
    if (access_token) {
        if (out_new_refresh)
            *out_new_refresh = new_refresh;
        PCV_LOG_INFO(RBAC_LOG_DOM, "Token refreshed for user '%s'", username);
        pcv_audit_log(username, "auth.token.refresh", username, "ok", 0, 0, NULL);
    } else {
        g_free(new_refresh);
    }
    return access_token;
}

                                                                                        
                                                       
                                                                                      
                                                      
static void
_ensure_nonce_column(sqlite3 *db, const gchar *table)
{
    g_autofree gchar *info_sql = g_strdup_printf("PRAGMA table_info(%s);", table);
    sqlite3_stmt *stmt = nullptr;
    gboolean present = FALSE;
    if (sqlite3_prepare_v2(db, info_sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(stmt, 1);
            if (name && strcmp((const gchar *)name, "identity_nonce") == 0) present = TRUE;
        }
        sqlite3_finalize(stmt);
    }
    if (present) return;
    g_autofree gchar *alter = g_strdup_printf(
        "ALTER TABLE %s ADD COLUMN identity_nonce TEXT NOT NULL DEFAULT '';", table);
    if (sqlite3_exec(db, alter, NULL, NULL, NULL) != SQLITE_OK)
        PCV_LOG_ERROR(RBAC_LOG_DOM, "identity migration: cannot add %s.identity_nonce: %s",
                      table, sqlite3_errmsg(db));
}

                                                                                      
                                                                      
                                                     
                                                                                  
                                                                                     
static gboolean
_fill_and_revoke_nonceless(sqlite3 *db, gint *filled, gint *revoked)
{
    *filled = 0; *revoked = 0;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) return FALSE;

    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    sqlite3_stmt *stmt = nullptr;
    gboolean ok = sqlite3_prepare_v2(db, "SELECT username FROM users WHERE identity_nonce = '';",
                                     -1, &stmt, NULL) == SQLITE_OK;
    if (ok) {
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            g_ptr_array_add(names, g_strdup((const gchar *)sqlite3_column_text(stmt, 0)));
        ok = (rc == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    for (guint i = 0; ok && i < names->len; i++) {
        g_autofree gchar *nonce = pcv_rbac_identity_nonce_new();                                        
        stmt = nullptr;
        ok = sqlite3_prepare_v2(db, "UPDATE users SET identity_nonce = ? WHERE username = ? AND identity_nonce = '';",
                                -1, &stmt, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(stmt, 1, nonce, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, g_ptr_array_index(names, i), -1, SQLITE_STATIC);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
            *filled += sqlite3_changes(db);
            sqlite3_finalize(stmt);
        }
    }
    g_ptr_array_unref(names);
    if (ok) {
        ok = sqlite3_exec(db, "UPDATE sessions SET revoked = 1 WHERE revoked = 0 AND identity_nonce = '';",
                          NULL, NULL, NULL) == SQLITE_OK;
        *revoked = sqlite3_changes(db);
    }
    if (ok) ok = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK;
    if (!ok) { _rollback(db); *filled = 0; *revoked = 0; }
    return ok;
}

                                                                                                  
                                                                      
                                                                        
                                                                         
                                                                             
                                                                      
static gint
_downgrade_apikey_roles_once(sqlite3 *db)
{
    sqlite3_stmt *stmt = nullptr;
    gint64 uv = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) uv = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (uv < 0) {
        PCV_LOG_ERROR(RBAC_LOG_DOM, "identity migration: PRAGMA user_version unreadable — api key downgrade skipped, retry next boot");
        return -1;
    }
    if (uv >= PCV_RBAC_USER_VERSION_APIKEY_STORED_ROLE)
        return 0;                                             

    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) return -1;
    gint lowered = -1;
    if (sqlite3_exec(db,
            "UPDATE api_keys SET role = "
            "COALESCE((SELECT role FROM users WHERE users.username = api_keys.client_name), 0) "
            "WHERE role > COALESCE((SELECT role FROM users WHERE users.username = api_keys.client_name), 0);",
            NULL, NULL, NULL) == SQLITE_OK) {
        lowered = sqlite3_changes(db);
        if (sqlite3_exec(db, "PRAGMA user_version = "
                         G_STRINGIFY(PCV_RBAC_USER_VERSION_APIKEY_STORED_ROLE) ";", NULL, NULL, NULL) != SQLITE_OK
            || sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
            lowered = -1;
    }
    if (lowered < 0) {
        PCV_LOG_ERROR(RBAC_LOG_DOM, "identity migration: api key downgrade failed: %s — retry next boot",
                      sqlite3_errmsg(db));
        _rollback(db);
    }
    return lowered;
}

                                                                              
void
pcv_rbac_identity_migrate(void)
{
    sqlite3 *db = pcv_rbac_db_lock();
    if (!db) { pcv_rbac_db_unlock(); return; }
    _ensure_nonce_column(db, "users");
    _ensure_nonce_column(db, "sessions");
    gint filled = 0, revoked = 0;
    if (!_fill_and_revoke_nonceless(db, &filled, &revoked)) {
        PCV_LOG_ERROR(RBAC_LOG_DOM, "identity migration: nonce fill/session revoke failed: %s — retry next boot",
                      sqlite3_errmsg(db));
        filled = revoked = -1;
    }
    gint downgraded = _downgrade_apikey_roles_once(db);
    pcv_rbac_db_unlock();

    g_last_filled_users = filled;
    g_last_revoked_sessions = revoked;
    g_last_downgraded_keys = downgraded;
    PCV_LOG_INFO(RBAC_LOG_DOM, "rbac identity migration: users nonce filled=%d, nonceless refresh sessions revoked=%d, "
                 "api keys downgraded=%d", filled, revoked, downgraded);
}

                                                                                 
                                                          
void
pcv_rbac_identity_last_migration_for_test(gint *filled, gint *revoked, gint *downgraded)
{
    if (filled) *filled = g_last_filled_users;
    if (revoked) *revoked = g_last_revoked_sessions;
    if (downgraded) *downgraded = g_last_downgraded_keys;
}
