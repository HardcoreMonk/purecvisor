   
                   
                                                                              
  
                           
                                                   
                                                    
                                        
  
          
                                               
                                                    
                                                
                                               
                                                
                        
  
         
                                                                        
                                                           
        
                                                              
                                                       
                                                                            
         
                                                     
        
                                                            
                                                                                            
                                                   
  
          
                                          
                                                               
                                                 
                                          
  
            
                   
                                                                           
                                     
                                                                          
                               
                                                                   
                                
                                                     
  
           
                                             
               
                                         
                                                                                                      
                                                       
                                                                
                                                                          
  
                                     
                                                                          
                                                                 
                                                                  
                                                               
                                                       
  
            
                                               
                                                
                                              
                                                 
  
          
                                                                                                         
                                                               
                                                                    
                                                   
                                                     
                                               
                                                                    
  
           
                                                
                                             
  
         
                                                                     
                                                   
                                                                              
                                                      
   

#include "pcv_rbac.h"
#include "pcv_rbac_internal.h"                                                  
#include "../../utils/pcv_jwt.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"
#include "../../utils/pcv_secure.h"                                     
#include "../../utils/pcv_totp.h"                                                        
#include "../audit/pcv_audit.h"

#include <glib.h>
#include <gio/gio.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>

#define RBAC_LOG_DOM  "pcv_rbac"
#define SALT_LEN      16                                     

                                                                              
#define REFRESH_TOKEN_BYTES   32                                   
#define IDENTITY_NONCE_BYTES  16                                                          

                                                                 
               
                                                                    

static sqlite3 *g_rbac_db    = nullptr;                                    
static GMutex   g_rbac_mutex;                                   

                                                                
static void _ensure_apikey_table(void);
static void _migrate_api_keys_schema(void);
                                                                     
static void _migrate_apikey_columns(void);
                                                             
static void _migrate_freeze_apikey_effective_roles(void);
                                                                        
                                                     
#define PCV_RBAC_USER_VERSION_APIKEY_FREEZE 1

   
                      
              
              
  
                                                                 
  
                                                 
                                                    
                                               
  
                                                   
                                                      
                                                          
                                    
   
static void
_fill_random_bytes(guchar *buf, gsize len)
{
    g_return_if_fail(buf != NULL);                                  

                                                             
                                             
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        gboolean ok = fread(buf, 1, len, f) == len;                   
        fclose(f);
        if (ok) return;
    }

                                                                         
                                            
    if (RAND_bytes(buf, (int)len) == 1)
        return;

                                          
                                                
                                               
    g_error("pcv_rbac: secure RNG unavailable "
            "(/dev/urandom and OpenSSL RAND_bytes both failed) — refusing to "
            "generate predictable salt/token/API key");
}

                                                                 
                                
                                                                    

   
                  
  
                                               
                                                              
                             
  
                                                   
                                               
                        
  
               
                                    
                             
  
                                                
   
static gchar *
_generate_salt(void)
{
    guchar raw[SALT_LEN];
    _fill_random_bytes(raw, sizeof(raw));                                  

                                                    
    GString *hex = g_string_sized_new(SALT_LEN * 2 + 1);
    for (gsize i = 0; i < sizeof(raw); i++)
        g_string_append_printf(hex, "%02x", raw[i]);                     

    return g_string_free(hex, FALSE);                                              
}

   
                         
                                
                            
  
                                                   
                                                
  
                                                  
                                                  
                         
  
                                                                
  
                                                   
                                                        
                                                            
                                       
   
static gchar *
_hash_password_legacy(const gchar *salt, const gchar *password)
{
    gchar *input = g_strdup_printf("%s%s", salt, password);                          
    gsize  input_len = strlen(input);

    guchar digest[EVP_MAX_MD_SIZE];
    guint  digest_len = 0;

                                                             
                                                              
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx) {
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
        EVP_DigestUpdate(ctx, input, input_len);
        EVP_DigestFinal_ex(ctx, digest, &digest_len);
        EVP_MD_CTX_free(ctx);
    }
    g_free(input);                         

                                                     
    GString *hex = g_string_sized_new(digest_len * 2 + 1);
    for (guint i = 0; i < digest_len; i++)
        g_string_append_printf(hex, "%02x", digest[i]);

    return g_string_free(hex, FALSE);
}

                                   
                                                               
                                                                  
                                                      
#define PBKDF2_ITER_TARGET  600000
#define PBKDF2_ITER_LEGACY  100000

                                                                            
static void (*g_pbkdf2_probe)(gint iter, gboolean db_locked) = NULL;
void pcv_rbac_set_pbkdf2_probe_for_test(void (*probe)(gint iter, gboolean db_locked));
void
pcv_rbac_set_pbkdf2_probe_for_test(void (*probe)(gint iter, gboolean db_locked))
{
    g_pbkdf2_probe = probe;
}

   
                             
  
                                 
                                                                           
                            
  
                                                 
                                                
                                           
                                 
   
static gint
_pbkdf2_target_iterations(void)
{
    gint iter = pcv_config_get_int("auth", "pbkdf2_iterations", PBKDF2_ITER_TARGET);
    if (iter < PBKDF2_ITER_TARGET) iter = PBKDF2_ITER_TARGET;                               
    return iter;
}

   
               
                     
                     
                        
  
                                                             
                  
  
                                                
                                                          
  
                                                            
                                                    
   
static gchar *
_pbkdf2_hex(const gchar *salt, const gchar *password, gint iter)
{
    guchar dk[32];                                              
    if (G_UNLIKELY(g_pbkdf2_probe)) {
        gboolean free_now = g_mutex_trylock(&g_rbac_mutex);                                   
        if (free_now) g_mutex_unlock(&g_rbac_mutex);
        g_pbkdf2_probe(iter, !free_now);
    }
                                                                     
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
                         (const guchar *)salt, (int)strlen(salt),
                         iter, EVP_sha256(), 32, dk) != 1)
        return NULL;                                     

                              
    GString *hex = g_string_sized_new(64 + 1);
    for (int i = 0; i < 32; i++)
        g_string_append_printf(hex, "%02x", dk[i]);
    return g_string_free(hex, FALSE);
}

   
                 
                                                 
                           
                                                              
  
                                        
                                                    
                                                                         
  
                                               
                                              
                                           
  
                                                     
                                                      
  
                                                                  
                                            
                                                             
   
static gboolean
_pbkdf2_parse(const gchar *stored, gint *iter_out, const gchar **hex_out)
{
                                                               
    if (!stored || !g_str_has_prefix(stored, "pbkdf2:")) return FALSE;
    const gchar *rest  = stored + 7;                                   
    const gchar *colon = strchr(rest, ':');                                   
    if (colon) {
                                                 
                                                   
        if (colon == rest) return FALSE;
        for (const gchar *it = rest; it < colon; it++)
            if (!g_ascii_isdigit(*it)) return FALSE;
        gchar *end = NULL;
        gint64 v = g_ascii_strtoll(rest, &end, 10);
        if (end != colon || v <= 0 || v > G_MAXINT) return FALSE;
        *iter_out = (gint)v;
        *hex_out  = colon + 1;                           
    } else {
                                                               
        *iter_out = PBKDF2_ITER_LEGACY;
        *hex_out  = rest;
    }
    return TRUE;
}

   
                         
                                
                            
  
                                                             
                         
                                                           
  
               
                                                      
                                               
                                               
                            
  
                                               
                                                    
                
  
                                                    
   
static gchar *
_hash_password_pbkdf2(const gchar *salt, const gchar *password)
{
    gint   iter = _pbkdf2_target_iterations();                                 
    gchar *hex  = _pbkdf2_hex(salt, password, iter);                   
    if (!hex) return NULL;                     
    gchar *out  = g_strdup_printf("pbkdf2:%d:%s", iter, hex);                     
    g_free(hex);
    return out;
}

   
                  
                                
                            
  
                                  
                                             
  
                                                     
                                               
  
              
                                            
                                         
                                                
  
                                                                
   
static gchar *
_hash_password(const gchar *salt, const gchar *password)
{
    return _hash_password_pbkdf2(salt, password);                           
}

   
                                                     
                                            
                                                            
                                             
                                            
   
static gboolean
_password_matches(const gchar *stored, const gchar *salt, const gchar *password,
                  gboolean *rehash, gint *spent_iter)
{
    if (rehash) *rehash = FALSE;
    if (spent_iter) *spent_iter = 0;
    if (!stored || !salt || !password) return FALSE;
    const gchar *hex = stored;
    gint iter = 0;
    gboolean pbkdf2 = g_str_has_prefix(stored, "pbkdf2:");
    if (pbkdf2 && !_pbkdf2_parse(stored, &iter, &hex)) return FALSE;
                                                 
    if (strlen(hex) != 64) return FALSE;
    for (guint i = 0; i < 64; i++)
        if (!g_ascii_isxdigit(hex[i])) return FALSE;
    if (pbkdf2 && spent_iter) *spent_iter = iter;
    gchar *candidate = pbkdf2 ? _pbkdf2_hex(salt, password, iter)
                              : _hash_password_legacy(salt, password);
    gboolean match = candidate && CRYPTO_memcmp(hex, candidate, 64) == 0;
    g_free(candidate);
    if (match && rehash) *rehash = !pbkdf2 || iter < _pbkdf2_target_iterations();
    return match;
}

                                                                               
#define LOGIN_COST_SALT "00000000000000000000000000000000"

   
                                                                            
                                                      
                                                                                   
                                                                                
                          
                                                 
   
static void
_login_failure_cost(const gchar *password, gint spent_iter)
{
    gint rest = _pbkdf2_target_iterations() - spent_iter;
    if (rest <= 0) return;
    gchar *discard = _pbkdf2_hex(LOGIN_COST_SALT, password, rest);
    g_free(discard);
}

   
                                                                          
                                                                           
                                                         
                                                                                  
                                                                                        
                                                                              
                                                  
   
static gboolean
_login_commit(const gchar *username, const gchar *stored_hash, const gchar *stored_salt,
              const gchar *new_hash, PcvRole *out_role, gchar **out_nonce)
{
    gboolean same = FALSE;
    sqlite3_stmt *stmt = NULL;
    g_mutex_lock(&g_rbac_mutex);
    if (g_rbac_db && sqlite3_prepare_v2(g_rbac_db,
            "SELECT password_hash, salt FROM users WHERE username = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        same = sqlite3_step(stmt) == SQLITE_ROW
            && g_strcmp0((const gchar *)sqlite3_column_text(stmt, 0), stored_hash) == 0
            && g_strcmp0((const gchar *)sqlite3_column_text(stmt, 1), stored_salt) == 0;
    }
    sqlite3_finalize(stmt);
    if (same && new_hash) {
        sqlite3_stmt *upd = NULL;
        if (sqlite3_prepare_v2(g_rbac_db,
                "UPDATE users SET password_hash=? WHERE username=? AND password_hash=?", -1, &upd, NULL) == SQLITE_OK) {
            sqlite3_bind_text(upd, 1, new_hash, -1, SQLITE_STATIC);
            sqlite3_bind_text(upd, 2, username, -1, SQLITE_STATIC);
            sqlite3_bind_text(upd, 3, stored_hash, -1, SQLITE_STATIC);
            if (sqlite3_step(upd) == SQLITE_DONE && sqlite3_changes(g_rbac_db) == 1)
                PCV_LOG_INFO(RBAC_LOG_DOM, "Rehashed password to PBKDF2 %d iterations for '%s'",
                             _pbkdf2_target_iterations(), username);
        }
        sqlite3_finalize(upd);
    }
    if (same)
        (void)pcv_rbac_lookup_subject_locked(g_rbac_db, username, out_role, out_nonce);
    g_mutex_unlock(&g_rbac_mutex);
    return same;
}

   
                                                                                           
                                                                                              
                                                                                                      
                                                                                   
                                               
                                                                                                            
                                                                
   
static gboolean
_login_verify(const gchar *username, const gchar *password, PcvRole *out_role, gchar **out_nonce, GError **error)
{
    if (out_role) *out_role = PCV_ROLE_VIEWER;
    if (out_nonce) *out_nonce = NULL;

                                  
    gint remain = 0;
    if (pcv_rbac_attempts_login_locked(username, &remain)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Account locked — retry after %d seconds", remain);
        return FALSE;
    }

    g_mutex_lock(&g_rbac_mutex);
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT password_hash, salt FROM users WHERE username = ?;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    g_autofree gchar *stored_hash = NULL, *stored_salt = NULL;
    if (rc == SQLITE_ROW) {
        stored_hash = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
        stored_salt = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (rc != SQLITE_ROW) {
                                                                                 
                                                                                          
        _login_failure_cost(password, 0);
        if (rc == SQLITE_DONE)
            pcv_rbac_attempts_login_record(username, FALSE);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid credentials");
        pcv_audit_log(username, "auth.login.failed", username, "user not found", 404, 0, NULL);
        return FALSE;
    }

    gboolean rehash = FALSE;
    gint spent_iter = 0;
    gboolean match = _password_matches(stored_hash, stored_salt, password, &rehash, &spent_iter);
    if (match) {
                                                              
        g_autofree gchar *new_hash = rehash ? _hash_password_pbkdf2(stored_salt, password) : NULL;
        match = _login_commit(username, stored_hash, stored_salt, new_hash, out_role, out_nonce);
    } else {
        _login_failure_cost(password, spent_iter);
    }

    if (!match) {
                                                                 
        pcv_rbac_attempts_login_record(username, FALSE);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid credentials");                       
        PCV_LOG_WARN(RBAC_LOG_DOM,
                     "Authentication failed for user '%s'", username);
        pcv_audit_log(username, "auth.login.failed", username, "invalid credentials", 401, 0, NULL);
        return FALSE;
    }

                                                          
    pcv_rbac_attempts_login_record(username, TRUE);
    return TRUE;
}

                                                              
                                                                     
                                                                         
                                                 
                                                              
                                                                  

   
                 
  
                                      
                                    
  
                                                          
                                                    
                                 
  
                                                            
           
                                         
                                                             
                                          
                                                           
                                                                     
  
                                                                     
                                             
   
static gboolean
_ensure_table(void)
{
    const gchar *sql_users =
        "CREATE TABLE IF NOT EXISTS users ("
        "  username      TEXT PRIMARY KEY NOT NULL,"
        "  password_hash TEXT NOT NULL,"
        "  salt          TEXT NOT NULL,"
        "  role          INTEGER NOT NULL DEFAULT 0,"
        "  tenant        TEXT,"
        "  identity_nonce TEXT NOT NULL DEFAULT ''"                                       
        ");";

    gchar *errmsg = nullptr;
    int rc = sqlite3_exec(g_rbac_db, sql_users, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to create users table: %s",
                      errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return FALSE;
    }

                                                         
                                                          
    const gchar *sql_sessions =
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id                 INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username           TEXT NOT NULL,"
        "  refresh_token_hash TEXT NOT NULL UNIQUE,"
        "  created_at         INTEGER NOT NULL,"
        "  expires_at         INTEGER NOT NULL,"
        "  revoked            INTEGER NOT NULL DEFAULT 0,"
        "  identity_nonce     TEXT NOT NULL DEFAULT ''"                                 
        ");";

    errmsg = nullptr;
    rc = sqlite3_exec(g_rbac_db, sql_sessions, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to create sessions table: %s",
                      errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return FALSE;
    }

                                
    sqlite3_exec(g_rbac_db,
        "CREATE INDEX IF NOT EXISTS idx_sessions_hash "
        "ON sessions(refresh_token_hash);",
        NULL, NULL, NULL);
    sqlite3_exec(g_rbac_db,
        "CREATE INDEX IF NOT EXISTS idx_sessions_user "
        "ON sessions(username, revoked);",
        NULL, NULL, NULL);

                                                                    
                                                     
                                                                 
                             

                                                                                           
                                                            
                                                              
                                                              
    const gchar *sql_user_totp =
        "CREATE TABLE IF NOT EXISTS user_totp ("
        "  username    TEXT PRIMARY KEY NOT NULL,"
        "  secret      TEXT NOT NULL,"
        "  confirmed   INTEGER NOT NULL DEFAULT 0,"
        "  created_at  INTEGER NOT NULL,"
        "  last_step   INTEGER NOT NULL DEFAULT 0"
        ");";

    errmsg = nullptr;
    rc = sqlite3_exec(g_rbac_db, sql_user_totp, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to create user_totp table: %s",
                      errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return FALSE;
    }

                                                                
                                                            
    const gchar *sql_totp_recovery =
        "CREATE TABLE IF NOT EXISTS totp_recovery_codes ("
        "  username  TEXT NOT NULL,"
        "  code_hash TEXT NOT NULL,"
        "  used      INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (username, code_hash)"
        ");";

    errmsg = nullptr;
    rc = sqlite3_exec(g_rbac_db, sql_totp_recovery, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to create totp_recovery_codes table: %s",
                      errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return FALSE;
    }

    return TRUE;
}

   
                      
  
                                
                                                    
                                                
  
                                                  
                                                  
                                                 
                       
  
                                                               
   
static void
_ensure_admin_user(void)
{
    const gchar *admin_user = pcv_config_get_admin_user();
    const gchar *admin_pass = pcv_config_get_admin_password();

    if (!admin_user || !*admin_user) admin_user = "admin";                        
    if (!admin_pass || !*admin_pass) {
                                                               
        PCV_LOG_WARN(RBAC_LOG_DOM,
                     "Bootstrap admin user '%s' not auto-created because admin_password is unset",
                     admin_user);
        return;
    }

                                                         
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
                                "SELECT username FROM users WHERE username = ?;",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) return;                                       

    sqlite3_bind_text(stmt, 1, admin_user, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_ROW) {
                                                         
        PCV_LOG_DEBUG(RBAC_LOG_DOM,
                      "Admin user '%s' already exists in RBAC DB", admin_user);
        return;
    }

                           
    GError *err = nullptr;
    gboolean ok = pcv_rbac_user_create(admin_user, admin_pass,
                                       PCV_ROLE_ADMIN, NULL, &err);
                                                                 
    pcv_audit_log("-", "auth.user.create", admin_user, ok ? "ok" : "fail", ok ? 0 : -1, 0, "bootstrap");
    if (ok) {
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "Auto-created admin user '%s' (ADMIN role)", admin_user);
    } else {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to auto-create admin user: %s",
                      err ? err->message : "unknown");
        if (err) g_error_free(err);
    }
}

                                                                 
                                 
                                                                    

   
                    
                                                  
  
                                         
                                                     
  
                                                    
                                                        
                                                   
                                     
  
                                                      
                                                       
                                                                
                                        
  
               
                                                               
                                                              
                                                              
  
                                                       
   
static PcvRole
_method_min_role(const gchar *method)
{
                                                              
    if (!method || !*method) return PCV_ROLE_ADMIN;

                                                                
                                                                         
                                                          
                                                                                      
      
                                                              
    static const gchar *const k_viewer_exact[] = {
                                                                 
                                                                                  
                                                    
        "pool.conninfo", "dpdk.status", "dpdk.list", "dpdk.hugepage.info", "sriov.status", "sriov.list",
                                                                   
                                                                                        
                                                                        
        "alert.history", "healing.pending", "healing.history", "healing.status",
        "agent.history", "config.history", "storage.pool.forecast",
                                                                                 
                                                                                 
                                                                      
        "iso.list", "vm.limit", "ovn.status", "vpc.list", "vpc.get", "vpc.subnet.list",
        "vpc.attachment.list", "vpc.service.list", "vpc.status", "vm.delete.status", "vm.guest.agent.status",
                                                                               
                                                                                      
                                                                                         
                                                                     
        "security.action.pending", "security.baseline.status", "capacity.forecast",
        "daemon.update_check", "vm.billing.report", "vm.guest.fsinfo", "vm.sla.report",
                                                                      
                                                                        
                                                                                      
                                                                                  
                                                                                  
                                                                           
        "push.subscribe", "push.unsubscribe", "push.mine",
                                                                             
                                                                     
                                                                                   
                                                                                      
                                                                         
                                                                                      
                                                                  
                                                           
        "auth.totp.status", "auth.totp.disable", "auth.totp.recovery.regenerate",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(k_viewer_exact); i++) {
        if (g_strcmp0(method, k_viewer_exact[i]) == 0)
            return PCV_ROLE_VIEWER;
    }

                                                        
    if (g_str_has_suffix(method, ".list") ||
        g_str_has_suffix(method, ".get")  ||
        g_str_has_suffix(method, ".metrics") ||
        g_str_has_suffix(method, ".info"))
    {
        return PCV_ROLE_VIEWER;
    }

                                              
    if (g_str_has_prefix(method, "monitor.") ||
        g_str_has_prefix(method, "telemetry."))
    {
        return PCV_ROLE_VIEWER;
    }
#if PCV_CLUSTER_ENABLED
    if (g_strcmp0(method, "cluster.status") == 0 ||
        g_strcmp0(method, "cluster.role") == 0   ||
        g_strcmp0(method, "storage.replicate.status") == 0)
    {
        return PCV_ROLE_VIEWER;
    }
#endif

                                                                
                                                                   
                                                           
    if (g_strcmp0(method, "vm.inspect") == 0)
        return PCV_ROLE_ADMIN;
                                                          
    if (g_str_has_prefix(method, "auth."))
        return PCV_ROLE_ADMIN;

                                                                      
                                                                   
                                                  
                                                             
      
                                                 
                                                       
    if (g_strcmp0(method, "dpdk.set") == 0 ||
        g_strcmp0(method, "dpdk.bind") == 0 ||
        g_strcmp0(method, "dpdk.unbind") == 0 ||
        g_strcmp0(method, "dpdk.bridge.create") == 0 ||
        g_strcmp0(method, "dpdk.bridge.delete") == 0 ||
        g_strcmp0(method, "sriov.enable") == 0 ||
        g_strcmp0(method, "sriov.disable") == 0 ||
        g_strcmp0(method, "sriov.set") == 0 ||
        g_strcmp0(method, "sriov.attach") == 0 ||
        g_strcmp0(method, "sriov.detach") == 0 ||
        g_strcmp0(method, "device.gpu.attach") == 0 ||
        g_strcmp0(method, "device.gpu.detach") == 0)
    {
        return PCV_ROLE_ADMIN;
    }

                                                          
                                                                          
    if (g_strcmp0(method, "container.destroy") == 0  ||
        g_strcmp0(method, "storage.zvol.delete") == 0 ||
        g_strcmp0(method, "network.delete") == 0     ||
        g_strcmp0(method, "overlay.delete") == 0     ||
        g_strcmp0(method, "iscsi.target.delete") == 0 ||
        g_strcmp0(method, "ovn.switch.delete") == 0  ||
        g_strcmp0(method, "ovn.port.remove") == 0    ||
        g_strcmp0(method, "vpc.delete") == 0         ||
        g_strcmp0(method, "vpc.reconcile") == 0      ||
        g_strcmp0(method, "backup.replicate") == 0)
    {
        return PCV_ROLE_ADMIN;
    }
                                                         
                                                           
                   
#if PCV_CLUSTER_ENABLED
    if (g_strcmp0(method, "cluster.failover.test") == 0 ||
        g_strcmp0(method, "cluster.peer.set") == 0 ||
        g_strcmp0(method, "storage.replicate.trigger") == 0)
    {
        return PCV_ROLE_ADMIN;
    }
#endif

                                                                                       
                                                                                     
    if (g_strcmp0(method, "security.action.dismiss") == 0)
        return PCV_ROLE_OPERATOR;

                                                                  
                                                                               
                                                                       
                                                              
                                                                               
    if (g_str_has_prefix(method, "vm.") ||
        g_strcmp0(method, "get_vnc_info") == 0 ||
        g_str_has_prefix(method, "container.") ||
        g_str_has_prefix(method, "network.") ||
        g_str_has_prefix(method, "storage.") ||
        g_str_has_prefix(method, "device.") ||
        g_str_has_prefix(method, "overlay.") ||
        g_str_has_prefix(method, "iscsi.") ||
        g_str_has_prefix(method, "ovn.") ||
        g_str_has_prefix(method, "dpdk.") ||
        g_str_has_prefix(method, "sriov.") ||
        g_str_has_prefix(method, "backup.") ||
        g_str_has_prefix(method, "template.") ||
        g_str_has_prefix(method, "agent.") ||
        g_str_has_prefix(method, "alert."))
    {
        return PCV_ROLE_OPERATOR;
    }
    if (g_str_has_prefix(method, "vpc."))
        return PCV_ROLE_OPERATOR;
#if PCV_CLUSTER_ENABLED
    if (g_str_has_prefix(method, "cluster."))
    {
        return PCV_ROLE_OPERATOR;
    }
#endif

                                                  
                                                   
                                                  
    return PCV_ROLE_ADMIN;
}

                                                                 
                                 
                                                                    

   
                         
  
                                                         
                                                   
  
                                                       
                                         
                                                          
                
   
static void
_ensure_quota_columns(void)
{
    if (!g_rbac_db) return;                        
                                                        
    sqlite3_exec(g_rbac_db,
        "ALTER TABLE users ADD COLUMN quota_vm_count INTEGER DEFAULT 0",
        NULL, NULL, NULL);
    sqlite3_exec(g_rbac_db,
        "ALTER TABLE users ADD COLUMN quota_storage_gb INTEGER DEFAULT 0",
        NULL, NULL, NULL);
}

   
                        
                             
                                       
  
                     
                                   
  
                                                
                        
  
                                                
                                                                     
                                                       
                                 
   
gboolean
pcv_rbac_check_quota(const gchar *username, gint current_vm_count)
{
    if (!g_rbac_db || !username) return TRUE;                        

    g_mutex_lock(&g_rbac_mutex);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_rbac_db,
        "SELECT quota_vm_count FROM users WHERE username=?",
        -1, &stmt, NULL) != SQLITE_OK || !stmt) {
        g_mutex_unlock(&g_rbac_mutex);
        return TRUE;                               
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    gint quota = 0;                                      
    if (sqlite3_step(stmt) == SQLITE_ROW)
        quota = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (quota <= 0) return TRUE;                     
    return current_vm_count < quota;                               
}

   
                      
                       
                                   
                                     
  
                      
  
                                                     
                                               
  
                                                                 
                                                             
                           
   
gboolean
pcv_rbac_set_quota(const gchar *username, gint vm_count, gint storage_gb)
{
    if (!g_rbac_db || !username) return FALSE;

    g_mutex_lock(&g_rbac_mutex);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_rbac_db,
        "UPDATE users SET quota_vm_count=?, quota_storage_gb=? WHERE username=?",
        -1, &stmt, NULL) != SQLITE_OK || !stmt) {
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }
    sqlite3_bind_int(stmt, 1, vm_count);
    sqlite3_bind_int(stmt, 2, storage_gb);
    sqlite3_bind_text(stmt, 3, username, -1, SQLITE_STATIC);
    gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (ok) {
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "Quota set for '%s': vm_count=%d, storage_gb=%d",
                     username, vm_count, storage_gb);
    }
    return ok;
}

                                                                 
                        
                                                                    

   
                            
  
                                                                         
                                                     
                                                          
                                                         
  
                                                       
                                                   
                                             
  
      
                                                                   
                                                                
                      
                                                                    
                                                                                   
                                                                                
                                                                          
                                                   
   
static void
_migrate_api_keys_schema(void)
{
    if (!g_rbac_db) return;

                                           
                                                          
                                                                   
    gboolean table_exists    = FALSE;
    gboolean has_client_name = FALSE;
    sqlite3_stmt *pragma = nullptr;
    if (sqlite3_prepare_v2(g_rbac_db,
            "PRAGMA table_info(api_keys);", -1, &pragma, NULL) == SQLITE_OK) {
        while (sqlite3_step(pragma) == SQLITE_ROW) {
            table_exists = TRUE;                       
            const char *col = (const char *)sqlite3_column_text(pragma, 1);                   
            if (col && g_strcmp0(col, "client_name") == 0)
                has_client_name = TRUE;                       
        }
        sqlite3_finalize(pragma);
    }

                                                               
                                      
    if (!table_exists || has_client_name) {
        _ensure_apikey_table();
        return;
    }

                                                      
    gint64 row_count = 0;
    sqlite3_stmt *cnt = nullptr;
    if (sqlite3_prepare_v2(g_rbac_db,
            "SELECT COUNT(*) FROM api_keys;", -1, &cnt, NULL) == SQLITE_OK) {
        if (sqlite3_step(cnt) == SQLITE_ROW)
            row_count = sqlite3_column_int64(cnt, 0);
        sqlite3_finalize(cnt);
    }

    gchar *errmsg = nullptr;
    if (row_count == 0) {
                                                               
        int rc = sqlite3_exec(g_rbac_db, "DROP TABLE api_keys;",
                              NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            PCV_LOG_ERROR(RBAC_LOG_DOM,
                          "api_keys schema#2 migration: DROP failed: %s",
                          errmsg ? errmsg : "unknown");
            sqlite3_free(errmsg);
            return;
        }
        _ensure_apikey_table();
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "api_keys migrated to canonical schema#2 "
                     "(empty legacy schema#1 table dropped)");
        return;
    }

                           
                                                              
                                                   
                                                                            
                                                    
    const char *migrate_sql =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE api_keys_new ("
        "  key_hash     TEXT PRIMARY KEY,"
        "  client_name  TEXT NOT NULL,"
        "  role         INTEGER NOT NULL DEFAULT 1,"
        "  created_at   TEXT NOT NULL DEFAULT (datetime('now')),"
        "  last_used_at TEXT,"
        "  revoked      INTEGER NOT NULL DEFAULT 0"
        ");"
        "INSERT INTO api_keys_new (key_hash, client_name, role, revoked) "
        "  SELECT key_hash, username, 1, COALESCE(revoked, 0) FROM api_keys;"
        "DROP TABLE api_keys;"
        "ALTER TABLE api_keys_new RENAME TO api_keys;"
        "COMMIT;";
    int rc = sqlite3_exec(g_rbac_db, migrate_sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
                                                               
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "api_keys schema#2 migration (table rewrite) failed: %s",
                      errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        sqlite3_exec(g_rbac_db, "ROLLBACK;", NULL, NULL, NULL);
        return;
    }
    PCV_LOG_INFO(RBAC_LOG_DOM,
                 "api_keys migrated to canonical schema#2 "
                 "(%lld legacy row(s) preserved, role defaulted to OPERATOR)",
                 (long long)row_count);
}

                                          
  
                                                     
                                                 
                                                  
                                                    
                                                     
                                                   
  
                                                                      
                                                          
                                                             
                                                     
                     
                                                                            
                                                                        
  
                          
                                                               
                                                             
                                                
                                                                
                                                       
  
                                                                            
                                                                 
                                         
static void
_migrate_freeze_apikey_effective_roles(void)
{
    if (!g_rbac_db) return;

                                             
                                                                    
                                                       
                                                            
                                                     
                                            
    gint64 uv = 0;                                                     
    gboolean uv_known = FALSE;                              
    sqlite3_stmt *q = nullptr;
    int uv_prep_rc = sqlite3_prepare_v2(g_rbac_db, "PRAGMA user_version;", -1, &q, NULL);
    if (uv_prep_rc == SQLITE_OK) {
        int uv_step_rc = sqlite3_step(q);
        if (uv_step_rc == SQLITE_ROW) {
            uv = sqlite3_column_int64(q, 0);
            uv_known = TRUE;                         
        } else {
                                                                      
            PCV_LOG_ERROR(RBAC_LOG_DOM,
                          "SEC-3 PRAGMA user_version step failed (rc=%d) — "
                          "apikey freeze-effective migration skipped this boot, "
                          "will retry next boot", uv_step_rc);
        }
        sqlite3_finalize(q);
    } else {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "SEC-3 PRAGMA user_version prepare failed (rc=%d): %s — "
                      "apikey freeze-effective migration skipped this boot, "
                      "will retry next boot",
                      uv_prep_rc, sqlite3_errmsg(g_rbac_db));
    }
    if (!uv_known)
        return;                                                      
    if (uv >= PCV_RBAC_USER_VERSION_APIKEY_FREEZE)
        return;                                                

                                                         
                                                               
    char *errmsg = nullptr;
    int rc = sqlite3_exec(g_rbac_db,
        "UPDATE api_keys SET role = "
        "COALESCE((SELECT role FROM users WHERE users.username = api_keys.client_name), 0);",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "SEC-3 apikey freeze-effective migration failed: %s",
                      errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
                                                                
        return;
    }

                                                            
                                       
                                                             
                                                       
                                                        
                                                       
                                                      
                                                
    int uv_write_rc = sqlite3_exec(g_rbac_db,
        "PRAGMA user_version = " G_STRINGIFY(PCV_RBAC_USER_VERSION_APIKEY_FREEZE) ";",
        NULL, NULL, NULL);
    if (uv_write_rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "SEC-3 PRAGMA user_version write failed (rc=%d): %s — "
                      "migration UPDATE already applied but marker not persisted, "
                      "will retry next boot",
                      uv_write_rc, sqlite3_errmsg(g_rbac_db));
        return;
    }
    PCV_LOG_INFO(RBAC_LOG_DOM,
                 "SEC-3 apikey freeze-effective migration applied "
                 "(existing keys' stored role pinned to current effective role; user_version=%d)",
                 PCV_RBAC_USER_VERSION_APIKEY_FREEZE);
}

                                                                   
                                                                       
                                                         
static gboolean g_totp_required_roles[3];

   
                              
  
                                                                        
                                                         
                                                  
                                                
                                                              
  
                                                     
                                                  
                                                
                                
  
                                           
   
static void
_totp_parse_required_roles(void)
{
    g_totp_required_roles[PCV_ROLE_VIEWER]   = FALSE;
    g_totp_required_roles[PCV_ROLE_OPERATOR] = FALSE;
    g_totp_required_roles[PCV_ROLE_ADMIN]    = FALSE;

                                                                            
    const gchar *raw = pcv_config_get_string("auth", "require_totp_roles", "");
    if (!raw || !*raw) return;                        

    gchar **tokens = g_strsplit(raw, ",", -1);
    for (gchar **t = tokens; *t; t++) {
        gchar *tok = g_strstrip(*t);                         
        if (!*tok) continue;                                      

                                                                
                                                             
                                                    
        gboolean known = g_ascii_strcasecmp(tok, "admin")    == 0 ||
                         g_ascii_strcasecmp(tok, "operator") == 0 ||
                         g_ascii_strcasecmp(tok, "viewer")   == 0;
        if (!known) {
            g_critical("[%s] FATAL: unrecognized role '%s' in [auth] require_totp_roles "
                       "— refusing silent no-op, daemon cannot start", RBAC_LOG_DOM, tok);
            g_strfreev(tokens);
            exit(1);
        }

        PcvRole role = pcv_rbac_str_to_role(tok);                            
        g_totp_required_roles[role] = TRUE;
    }
    g_strfreev(tokens);
}

   
                                                                
                                                  
                                                          
   
gboolean
pcv_rbac_totp_role_required(gint role)
{
    if (role < 0 || role >= (gint)G_N_ELEMENTS(g_totp_required_roles))
        return FALSE;                                        
    return g_totp_required_roles[role];
}

   
                 
                                                                 
  
                          
                  
                                
                                    
                                      
  
                                                       
                                                     
                                
  
                               
                                      
                          
  
                                                                
                                                                   
                                                           
   
void
pcv_rbac_init(const gchar *db_path)
{
    g_mutex_init(&g_rbac_mutex);

    const gchar *path = db_path ? db_path : "/var/lib/purecvisor/rbac.db";

    int rc = sqlite3_open(path, &g_rbac_db);
    if (rc != SQLITE_OK) {
                                                                    
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Cannot open RBAC DB '%s': %s",
                      path, sqlite3_errmsg(g_rbac_db));
        return;
    }

                                                
    sqlite3_exec(g_rbac_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    if (!_ensure_table()) return;                                      

                                                                  
                                                        
    _totp_parse_required_roles();

                                                                   
                              
    _migrate_api_keys_schema();

                                                                         
                                                                     
    _migrate_apikey_columns();

                                                      
                                                                                       
    _migrate_freeze_apikey_effective_roles();

                                                                                     
    pcv_rbac_identity_migrate();

    _ensure_admin_user();

                                   
    _ensure_quota_columns();

    PCV_LOG_INFO(RBAC_LOG_DOM, "RBAC module initialized (DB: %s)", path);
}

   
                     
  
                            
                                 
  
                                                    
                                                         
   
void
pcv_rbac_shutdown(void)
{
    g_mutex_lock(&g_rbac_mutex);
    if (g_rbac_db) {
        sqlite3_close(g_rbac_db);
        g_rbac_db = nullptr;
    }
    g_mutex_unlock(&g_rbac_mutex);
    g_mutex_clear(&g_rbac_mutex);

    PCV_LOG_INFO(RBAC_LOG_DOM, "RBAC module shut down");
}

                                                                 
                        
                                                                    

   
                        
                                       
                                                       
                                                                  
                                                                
                                         
  
                         
                                                            
  
                                                 
                                                    
                               
  
                                           
  
                                                                               
                                                                   
                                            
   

gboolean
pcv_rbac_user_create(const gchar *username,
                     const gchar *password,
                     PcvRole      role,
                     const gchar *tenant,
                     GError     **error)
{
    g_return_val_if_fail(username && *username, FALSE);                 
    g_return_val_if_fail(password && *password, FALSE);                  

    gchar *salt = _generate_salt();                               
    gchar *hash = _hash_password(salt, password);                   
    g_autofree gchar *nonce = pcv_rbac_identity_nonce_new();                           

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "INSERT INTO users (username, password_hash, salt, role, tenant, identity_nonce) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        g_free(salt);
        g_free(hash);
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash,     -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, salt,     -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 4, (int)role);
    if (tenant)
        sqlite3_bind_text(stmt, 5, tenant, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);                                                      
    sqlite3_bind_text(stmt, 6, nonce, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    g_free(salt);                                   
    g_free(hash);

    if (rc != SQLITE_DONE) {
                                                                
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "User '%s' already exists or DB error: %s",
                    username, sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.user.create", username,
                  "User created (role=%s, tenant=%s)",
                  pcv_rbac_role_to_str(role), tenant ? tenant : "global");
    return TRUE;
}

                                                                                                                    

   
                      
  
                                                   
                                                          
                                     
  
                                                 
                                             
  
                                                                
                                            
                              
   
GPtrArray *
pcv_rbac_user_list(void)
{
                                                                       
    GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)pcv_user_free);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT u.username, u.role, u.tenant, "
        "       CASE WHEN t.username IS NULL THEN 0 ELSE 1 END, "
        "       COALESCE(t.confirmed, 0) "
        "FROM users AS u "
        "LEFT JOIN user_totp AS t ON t.username = u.username "
        "ORDER BY u.username;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return arr;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {                             
        PcvUser *u = g_new0(PcvUser, 1);
        u->username = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));             
        u->role     = (PcvRole)sqlite3_column_int(stmt, 1);
        const gchar *t = (const gchar *)sqlite3_column_text(stmt, 2);
        u->tenant   = t ? g_strdup(t) : NULL;                                                      
        u->totp_enrolled  = sqlite3_column_int(stmt, 3) != 0;
        u->totp_confirmed = sqlite3_column_int(stmt, 4) != 0;
        g_ptr_array_add(arr, u);
    }

    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    return arr;
}

   
                          
                    
                                
                       
  
                     
                               
  
                                                           
                                                               
  
                                                                                    
                                                                                  
                                                                                          
                     
   
gboolean
pcv_rbac_user_set_role(const gchar *username,
                       PcvRole      role,
                       PcvRole     *out_previous,
                       GError     **error)
{
    g_return_val_if_fail(username && *username, FALSE);

    g_mutex_lock(&g_rbac_mutex);

                                                                
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db, "SELECT role FROM users WHERE username = ?;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    PcvRole previous = (rc == SQLITE_ROW) ? (PcvRole)sqlite3_column_int(stmt, 0) : PCV_ROLE_VIEWER;
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW) {
        if (rc == SQLITE_DONE)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "User '%s' not found", username);
        else
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DB error: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }

    stmt = nullptr;
    rc = sqlite3_prepare_v2(g_rbac_db, "UPDATE users SET role = ? WHERE username = ?;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }

    sqlite3_bind_int (stmt, 1, (int)role);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }
    g_mutex_unlock(&g_rbac_mutex);

    if (out_previous) *out_previous = previous;
    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.role.set", username, "Role changed from %s to %s",
                  pcv_rbac_role_to_str(previous), pcv_rbac_role_to_str(role));
    return TRUE;
}

   
                            
                                                    
  
                                                
                                                        
                                         
  
                                                         
                                                             
                                                             
                                                               
   
gboolean
pcv_rbac_change_password(const gchar *username,
                         const gchar *old_password,
                         const gchar *new_password,
                         GError     **error)
{
    g_return_val_if_fail(username && *username, FALSE);
    g_return_val_if_fail(old_password && *old_password, FALSE);
    g_return_val_if_fail(new_password && *new_password, FALSE);

                                                  
    if (strlen(new_password) < 8) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "New password must be at least 8 characters");
        return FALSE;
    }
                                              
    if (g_strcmp0(old_password, new_password) == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "New password must differ from current password");
        return FALSE;
    }

                                                          
    if (pcv_rbac_attempts_login_locked(username, NULL)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Account locked");
        return FALSE;
    }

    gchar *salt = _generate_salt();
    gchar *hash = _hash_password(salt, new_password);
    if (!hash) {
        g_free(salt);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Password hashing failed");
        return FALSE;
    }
    gboolean ok = FALSE, transaction = FALSE, bad_password = FALSE;
    sqlite3_stmt *stmt = NULL;
    g_mutex_lock(&g_rbac_mutex);
    if (!g_rbac_db || sqlite3_exec(g_rbac_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        goto db_failure;
    transaction = TRUE;

                                                       
    if (sqlite3_prepare_v2(g_rbac_db,
            "SELECT password_hash, salt FROM users WHERE username=?", -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC) != SQLITE_OK)
        goto db_failure;
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto db_failure;
    if (rc == SQLITE_DONE || !_password_matches(
            (const gchar *)sqlite3_column_text(stmt, 0),
            (const gchar *)sqlite3_column_text(stmt, 1), old_password, NULL, NULL)) {
        bad_password = TRUE;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Current password is incorrect");
        goto done;
    }
    sqlite3_finalize(stmt); stmt = NULL;

    if (sqlite3_prepare_v2(g_rbac_db,
            "UPDATE users SET password_hash=?, salt=? WHERE username=?", -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, salt, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 3, username, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(g_rbac_db) != 1)
        goto db_failure;
    sqlite3_finalize(stmt); stmt = NULL;

                                                              
    if (sqlite3_prepare_v2(g_rbac_db,
            "UPDATE sessions SET revoked=1 WHERE username=? AND revoked=0", -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE)
        goto db_failure;
    sqlite3_finalize(stmt); stmt = NULL;
    if (sqlite3_exec(g_rbac_db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto db_failure;
    transaction = FALSE;
    ok = TRUE;
    goto done;

db_failure:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Password change transaction failed: %s",
                g_rbac_db ? sqlite3_errmsg(g_rbac_db) : "RBAC unavailable");
done:
    sqlite3_finalize(stmt);
    if (transaction && sqlite3_exec(g_rbac_db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK)
        PCV_LOG_WARN(RBAC_LOG_DOM, "Password change rollback failed: %s", sqlite3_errmsg(g_rbac_db));
    g_mutex_unlock(&g_rbac_mutex);
    g_free(salt); g_free(hash);

    if (ok || bad_password)
        pcv_rbac_attempts_login_record(username, ok);
                                                           
    if (ok) PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.password.change", username,
                          "Password changed (refresh sessions revoked)");
    return ok;
}

                                                                 
                                 
                                                                    

   
                         
                    
                     
                                 
  
                                       
                                          
                           
  
                                                      
                                                       
                                                     
                                                     
                                                
                             
  
                                          
                                                               
  
                                                                                
                                                                              
  
                                                                                   
                                          
                                                              
   

                                                           
                                                 
gchar *
pcv_rbac_authenticate(const gchar *username,
                      const gchar *password,
                      GError     **error)
{
    g_return_val_if_fail(username && *username, NULL);
    g_return_val_if_fail(password && *password, NULL);

    if (!_login_verify(username, password, NULL, NULL, error))
        return NULL;

                                                               
    gchar *token = pcv_jwt_sign(username, 0, error);
    if (token) {
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "User '%s' authenticated successfully", username);
        pcv_audit_log(username, "auth.login", username, "ok", 0, 0, NULL);
    }
    return token;
}

                                                                 
                                      
                                                                    

                                                                                                  
                                                                                                     
gchar *
pcv_rbac_identity_nonce_new(void)
{
    guchar raw[IDENTITY_NONCE_BYTES];
    _fill_random_bytes(raw, sizeof(raw));                                   
    gchar *hex = g_malloc(IDENTITY_NONCE_BYTES * 2 + 1);
    for (gsize i = 0; i < sizeof(raw); i++)
        g_snprintf(hex + i * 2, 3, "%02x", raw[i]);
    return hex;
}

   
                                                                                   
  
                                               
                                                              
  
                                                           
                                           
  
                                                    
   
gchar *
pcv_rbac_refresh_token_new(void)
{
    guchar raw[REFRESH_TOKEN_BYTES];
    _fill_random_bytes(raw, sizeof(raw));                                

    GString *hex = g_string_sized_new(REFRESH_TOKEN_BYTES * 2 + 1);
    for (gsize i = 0; i < sizeof(raw); i++)
        g_string_append_printf(hex, "%02x", raw[i]);

    return g_string_free(hex, FALSE);
}

   
                                                                                
                                          
  
                                          
                                      
  
                                                    
                                             
  
                                                          
   
gchar *
pcv_rbac_refresh_token_hash(const gchar *token)
{
    GChecksum *cksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(cksum, (const guchar *)token, (gssize)strlen(token));
    gchar *hex = g_strdup(g_checksum_get_string(cksum));
    g_checksum_free(cksum);
    return hex;
}

                                                                 
                                                                   
                                                                    

   
                           
  
                                                                          
                                                         
                                                      
  
                                                
                                                         
  
                                                                                                 
                                                 
   
                                                                                                               
gboolean
pcv_rbac_password_check(const gchar *username,
                        const gchar *password,
                        GError     **error)
{
    g_return_val_if_fail(username && *username, FALSE);
    g_return_val_if_fail(password && *password, FALSE);
    return _login_verify(username, password, NULL, NULL, error);
}

                                                                     
gboolean
pcv_rbac_password_check_subject(const gchar *username,
                                const gchar *password,
                                PcvRole     *out_role,
                                gchar      **out_identity_nonce,
                                GError     **error)
{
    g_return_val_if_fail(username && *username, FALSE);
    g_return_val_if_fail(password && *password, FALSE);
    return _login_verify(username, password, out_role, out_identity_nonce, error);
}

                                                                                                                            
                                                                 
                                                  
                                                                    

                                                                                                 

   
                           
  
                            
  
                                                    
                                         
  
                                                                        
                                                           
   
gboolean
pcv_rbac_revoke_session(const gchar *username, GError **error)
{
    g_return_val_if_fail(username && *username, FALSE);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE sessions SET revoked = 1 WHERE username = ? AND revoked = 0;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_rbac_db);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error: %s", sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.session.revoke", username,
                  "Revoked %d active session(s)", changes);
    pcv_audit_log(username, "auth.session.revoke", username, "ok", 0, 0, NULL);
    return TRUE;
}

   
                                     
  
                                       
                                                
  
                                                  
                        
  
                                                     
   
gint
pcv_rbac_cleanup_expired_sessions(void)
{
    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "DELETE FROM sessions WHERE expires_at < ? OR revoked = 1;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, now);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_rbac_db);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (changes > 0) {
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "Cleaned up %d expired/revoked session(s)", changes);
    }

    return changes;
}

                                                                 
                                            
                                                                    

   
                          
                         
  
                                             
  
                                                    
                              
  
                                                                         
                              
   
JsonArray *
pcv_rbac_list_sessions(const gchar *username)
{
    JsonArray *arr = json_array_new();
    if (!g_rbac_db || !username) return arr;

    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT id, created_at, expires_at FROM sessions "
        "WHERE username=? AND revoked=0 AND expires_at > ?";
    if (sqlite3_prepare_v2(g_rbac_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, now);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            JsonObject *s = json_object_new();
            json_object_set_int_member(s, "id", sqlite3_column_int64(stmt, 0));
            json_object_set_int_member(s, "created_at", sqlite3_column_int64(stmt, 1));
            json_object_set_int_member(s, "expires_at", sqlite3_column_int64(stmt, 2));
            json_array_add_object_element(arr, s);
        }
        sqlite3_finalize(stmt);
    }

    g_mutex_unlock(&g_rbac_mutex);
    return arr;
}

   
                                    
                                                                              
                                                                             
                                                               
                                                                                
                                                                                 
   
gint
pcv_rbac_revoke_session_by_token(const gchar *refresh_token, const gchar *username,
                                 gchar **out_username)
{
    if (out_username) *out_username = NULL;
    if (!refresh_token || !*refresh_token) return 0;
    gchar *token_hash = pcv_rbac_refresh_token_hash(refresh_token);
    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;
    gint result = -1;
    gchar *owner = nullptr;
    sqlite3_stmt *stmt = nullptr;

    g_mutex_lock(&g_rbac_mutex);
    if (g_rbac_db && sqlite3_prepare_v2(g_rbac_db,
            "SELECT id, username FROM sessions WHERE refresh_token_hash = ? AND revoked = 0 "
            "AND expires_at > ? AND (?3 IS NULL OR username = ?3);", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, token_hash, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, now);
        sqlite3_bind_text(stmt, 3, username, -1, SQLITE_STATIC);                                   
        int rc = sqlite3_step(stmt);
        result = rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
        gint64 id = result == 1 ? sqlite3_column_int64(stmt, 0) : 0;
        if (result == 1) owner = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
        sqlite3_finalize(stmt);
        if (result == 1) {
            result = -1;
            if (sqlite3_prepare_v2(g_rbac_db, "UPDATE sessions SET revoked = 1 WHERE id = ?;", -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, id);
                result = sqlite3_step(stmt) == SQLITE_DONE ? 1 : -1;
                sqlite3_finalize(stmt);
            }
        }
    }
    g_mutex_unlock(&g_rbac_mutex);
    if (result == 1 && out_username) *out_username = g_steal_pointer(&owner);
    g_free(owner);
    g_free(token_hash);
    return result;
}

                                                                 
                     
                                                                    

   
                             
                        
                                            
  
                                              
                                               
  
                                  
                                 
  
                                                     
                                                    
                                                      
  
                                                           
                                                         
   

gboolean
pcv_rbac_check_permission(const gchar *username,
                          const gchar *method)
{
    if (!username || !method) return FALSE;                   

                                                                                   
    PcvRole user_role = PCV_ROLE_VIEWER;
    if (pcv_rbac_lookup_subject(username, &user_role, NULL) != PCV_USER_PRESENT) return FALSE;
    return pcv_rbac_role_permits(user_role, method);
}

                                                                                  
gboolean
pcv_rbac_role_permits(PcvRole role, const gchar *method)
{
    if (!method || role < PCV_ROLE_VIEWER || role > PCV_ROLE_ADMIN) return FALSE;                      
    return role >= _method_min_role(method);                          
}

                                                                                               
                                                            

                                                                                                          
                                                                             
                                                                       
sqlite3 *
pcv_rbac_db_lock(void)
{
    g_mutex_lock(&g_rbac_mutex);
    return g_rbac_db;
}

void
pcv_rbac_db_unlock(void) { g_mutex_unlock(&g_rbac_mutex); }

                                                                 
                                    
                                                                    

   
                        
                     
  
                                  
                         
  
                                                                         
  
                                                                
   

const gchar *
pcv_rbac_role_to_str(PcvRole role)
{
    switch (role) {
    case PCV_ROLE_VIEWER:   return "viewer";
    case PCV_ROLE_OPERATOR: return "operator";
    case PCV_ROLE_ADMIN:    return "admin";
    default:                return "unknown";
    }
}

   
                        
                                                         
  
                            
                                            
  
                                                     
                                                          
  
                                                          
   
PcvRole
pcv_rbac_str_to_role(const gchar *str)
{
    if (!str) return PCV_ROLE_VIEWER;                      
    if (g_ascii_strcasecmp(str, "admin") == 0)    return PCV_ROLE_ADMIN;
    if (g_ascii_strcasecmp(str, "operator") == 0) return PCV_ROLE_OPERATOR;
    if (g_ascii_strcasecmp(str, "viewer") == 0)   return PCV_ROLE_VIEWER;
    return PCV_ROLE_VIEWER;
}

                                                                 
                                       
                                                                    

   
               
                                         
  
                                                       
                                                                    
  
                                                                      
   
static gchar *
_sha256_hex(const gchar *data, gsize len)
{
    guchar digest[EVP_MAX_MD_SIZE];
    guint  digest_len = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx) {
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
        EVP_DigestUpdate(ctx, data, len);
        EVP_DigestFinal_ex(ctx, digest, &digest_len);
        EVP_MD_CTX_free(ctx);
    }

    GString *hex = g_string_sized_new(digest_len * 2 + 1);
    for (guint i = 0; i < digest_len; i++)
        g_string_append_printf(hex, "%02x", digest[i]);

    return g_string_free(hex, FALSE);
}

                                                                      
                                                                    
                                                                           
                                                            
                                                             
                                                              
  
                                                                      
                                                               
                                                                                     
   
                                                  
                                                    
                                                     
                                                   
                                      
                                                                    
                                                                                 
                                                                                 
                                                                         
   
gchar *
pcv_rbac_verify_api_key(const gchar *api_key, PcvRole *out_role, GError **error)
{
    if (out_role) *out_role = PCV_ROLE_VIEWER;                               

                                                                       
    if (!api_key || strlen(api_key) != 68 ||
        strncmp(api_key, "pcv_", 4) != 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid API key format");
        return NULL;
    }

    gchar *key_hash = _sha256_hex(api_key, strlen(api_key));                      
    gint64 now_epoch = g_get_real_time() / G_USEC_PER_SEC;                        

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
                                                                
                                                                                        
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT client_name, role, (last_used_at IS NULL OR last_used_at NOT BETWEEN "
        "datetime('now','-59 seconds') AND datetime('now')) FROM api_keys "
        "WHERE key_hash = ? AND revoked = 0 "
        "AND (expires_at = 0 OR expires_at > ?);",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed");
        g_free(key_hash);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, key_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now_epoch);

    gchar *client_name = nullptr;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
                                                                  
        client_name = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
        if (out_role) *out_role = (PcvRole)sqlite3_column_int(stmt, 1);
    }
    gboolean touch = (rc == SQLITE_ROW) && sqlite3_column_int(stmt, 2) != 0;
    sqlite3_finalize(stmt);
                                                                            
                                                            
                                                                 
    if (touch) {
        sqlite3_stmt *upd = nullptr;
        if (sqlite3_prepare_v2(g_rbac_db, "UPDATE api_keys SET last_used_at=datetime('now') WHERE key_hash=?",
                               -1, &upd, NULL) == SQLITE_OK) {
            sqlite3_bind_text(upd, 1, key_hash, -1, SQLITE_STATIC);
            if (sqlite3_step(upd) != SQLITE_DONE)
                PCV_LOG_WARN(RBAC_LOG_DOM, "API key last_used_at update failed: %s", sqlite3_errmsg(g_rbac_db));
        }
        sqlite3_finalize(upd);
    }
    g_mutex_unlock(&g_rbac_mutex);
    g_free(key_hash);

    if (!client_name) {
                                                      
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "API key invalid or revoked");
    }
    return client_name;
}

                                       
                                                                                       
                                                                 
                                                                   
                         

   
                 
                                
  
                                                         
                                                              
  
                                                            
   
void
pcv_user_free(PcvUser *u)
{
    if (!u) return;
    g_free(u->username);
    g_free(u->tenant);
    g_free(u);
}

                                                                 
                                 
  
                                                                    
                                                             
                                                         
                                                           
                                                      
        
                                                                    

   
                                                                
                                                   
                                                              
                                                                           
   
static void _ensure_apikey_table(void) {
                                                                        
                                                                        
    const char *sql =
        "CREATE TABLE IF NOT EXISTS api_keys ("
        "  key_hash     TEXT PRIMARY KEY,"
        "  client_name  TEXT NOT NULL,"
        "  role         INTEGER NOT NULL DEFAULT 1,"
        "  description  TEXT NOT NULL DEFAULT '',"
        "  created_at   TEXT NOT NULL DEFAULT (datetime('now')),"
        "  last_used_at TEXT,"
        "  expires_at   INTEGER NOT NULL DEFAULT 0,"                        
        "  revoked      INTEGER NOT NULL DEFAULT 0"
        ")";
    sqlite3_exec(g_rbac_db, sql, NULL, NULL, NULL);
}

                           
  
                                                                       
                                                             
                                                           
  
                                                                 
                                                          
                                                          
static void _migrate_apikey_columns(void)
{
    if (!g_rbac_db) return;
    const char *alters[] = {
        "ALTER TABLE api_keys ADD COLUMN description TEXT NOT NULL DEFAULT ''",
        "ALTER TABLE api_keys ADD COLUMN expires_at  INTEGER NOT NULL DEFAULT 0",
    };
    for (guint i = 0; i < G_N_ELEMENTS(alters); i++) {
        char *errmsg = nullptr;
                                     
        sqlite3_exec(g_rbac_db, alters[i], NULL, NULL, &errmsg);
        sqlite3_free(errmsg);
    }
                                                                           
                                                         
                                                              
                                                                   
                                                               
    {
        char *errmsg = nullptr;
        sqlite3_exec(g_rbac_db,
            "UPDATE api_keys SET expires_at = 0 WHERE expires_at IS NULL",
            NULL, NULL, &errmsg);
        sqlite3_free(errmsg);
    }
}

   
                                        
                                                   
                                                
                                         
                                                            
                                                                    
                                                                  
                  
  
                                                      
                                                    
                                                     
                                                                            
   
gboolean
pcv_rbac_apikey_create(const gchar *client_name, PcvRole role,
                       const gchar *description, gint64 expires_at,
                       gchar **out_key, GError **error)
{
                                                                     
    guint8 raw[32];
    _fill_random_bytes(raw, sizeof(raw));                           

    GString *plaintext_key = g_string_new("pcv_");
    for (int i = 0; i < 32; i++) g_string_append_printf(plaintext_key, "%02x", raw[i]);
    pcv_secure_wipe(raw, sizeof raw);                                                

                                                          
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, plaintext_key->str, -1);

                                                              
                                                         
                                     
    g_mutex_lock(&g_rbac_mutex);
    _ensure_apikey_table();
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "INSERT INTO api_keys (key_hash, client_name, role, description, expires_at) "
        "VALUES (?, ?, ?, ?, ?)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, client_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, (int)role);
        sqlite3_bind_text(stmt, 4, description ? description : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, expires_at > 0 ? expires_at : 0);
        rc = sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (rc != SQLITE_DONE) {
                                                              
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create API key");
        g_free(hash);
                                                               
        pcv_secure_wipe(plaintext_key->str, plaintext_key->len);
        g_string_free(plaintext_key, TRUE);
        return FALSE;
    }

                                                                                 
                                                                      
    *out_key = g_string_free(plaintext_key, FALSE);
    g_free(hash);
    return TRUE;
}

   
                                                 
                                                    
                                        
                                                                      
   
JsonArray *
pcv_rbac_apikey_list(void)
{
    JsonArray *arr = json_array_new();

                                                                  
    g_mutex_lock(&g_rbac_mutex);
    _ensure_apikey_table();
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_rbac_db,
        "SELECT client_name, role, description, created_at, last_used_at, "
        "expires_at, revoked FROM api_keys ORDER BY created_at DESC",
        -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "client_name", (const char *)sqlite3_column_text(stmt, 0));
            json_object_set_int_member(obj, "role", sqlite3_column_int(stmt, 1));
            const char *desc = (const char *)sqlite3_column_text(stmt, 2);
            json_object_set_string_member(obj, "description", desc ? desc : "");
            json_object_set_string_member(obj, "created_at", (const char *)sqlite3_column_text(stmt, 3));
            const char *lu = (const char *)sqlite3_column_text(stmt, 4);
            if (lu) json_object_set_string_member(obj, "last_used_at", lu);
                                              
            json_object_set_int_member(obj, "expires_at", sqlite3_column_int64(stmt, 5));
            json_object_set_boolean_member(obj, "revoked", sqlite3_column_int(stmt, 6) != 0);
            json_array_add_object_element(arr, obj);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);
    return arr;
}

   
                                      
                                                        
                                            
                                                                  
                                                             
   
gboolean
pcv_rbac_apikey_revoke(const gchar *client_name, GError **error)
{
                                                                  
    g_mutex_lock(&g_rbac_mutex);
    _ensure_apikey_table();
                                                                        
    sqlite3_stmt *upd = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE api_keys SET revoked=1 WHERE client_name=?",
        -1, &upd, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(upd, 1, client_name, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(upd);
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
        sqlite3_finalize(upd);
    }
    g_mutex_unlock(&g_rbac_mutex);
    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Revoke failed: %s", sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }
    return TRUE;
}

                                                                 
                                    
  
                                                     
                                            
                                                                    

static GHashTable *g_perm_cache    = nullptr;                                                
static GHashTable *g_perm_cache_ts = nullptr;                                           
static GMutex      g_perm_cache_mu;
#define PERM_CACHE_TTL_SEC 60

   
                                                      
                                                     
                                             
   
void pcv_rbac_perm_cache_init(void) {
    g_mutex_init(&g_perm_cache_mu);
                                                                        
    g_perm_cache    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
                                                     
    g_perm_cache_ts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

   
                                                           
                                                   
                                               
                                                            
                                            
   
void pcv_rbac_perm_cache_invalidate(const gchar *username) {
    if (!g_perm_cache) return;
    g_mutex_lock(&g_perm_cache_mu);
                            
                                                      
                                                           
                                                          
    GHashTableIter iter;
    gpointer key, value;
    GList *to_remove = nullptr;
    g_hash_table_iter_init(&iter, g_perm_cache);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (g_str_has_prefix((const gchar *)key, username))
            to_remove = g_list_prepend(to_remove, g_strdup((const gchar *)key));               
    }
                                              
    for (GList *l = to_remove; l; l = l->next) {
        g_hash_table_remove(g_perm_cache, l->data);
        g_hash_table_remove(g_perm_cache_ts, l->data);
    }
    g_list_free_full(to_remove, g_free);
    g_mutex_unlock(&g_perm_cache_mu);
}

   
                                                     
                                                          
                                                           
                                                          
   
gint pcv_rbac_perm_cache_check(const gchar *username, const gchar *method) {
    if (!g_perm_cache || !username || !method) return -1;                       
                                                                              
    gchar key[192];
    g_snprintf(key, sizeof(key), "%s:%s", username, method);                           
    gint result = -1;

    g_mutex_lock(&g_perm_cache_mu);
    gpointer val = g_hash_table_lookup(g_perm_cache, key);
    gint64 *ts = g_hash_table_lookup(g_perm_cache_ts, key);
    if (val && ts) {                             
        gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
        if (now - *ts < PERM_CACHE_TTL_SEC)
            result = GPOINTER_TO_INT(val);                                       
        else {
                                                               
            g_hash_table_remove(g_perm_cache, key);
            g_hash_table_remove(g_perm_cache_ts, key);
        }
    }
    g_mutex_unlock(&g_perm_cache_mu);
                                                         
    return result;
}

   
                                                               
                                                      
                                                                   
   
void pcv_rbac_perm_cache_set(const gchar *username, const gchar *method, gboolean allowed) {
    if (!g_perm_cache || !username || !method) return;
                                                                                 
    gchar key[192];
    g_snprintf(key, sizeof(key), "%s:%s", username, method);
    gint64 *ts = g_new(gint64, 1);                                           
    *ts = g_get_monotonic_time() / G_USEC_PER_SEC;

    g_mutex_lock(&g_perm_cache_mu);
                                                        
    g_hash_table_replace(g_perm_cache, g_strdup(key), GINT_TO_POINTER(allowed ? 1 : 0));
    g_hash_table_replace(g_perm_cache_ts, g_strdup(key), ts);
    g_mutex_unlock(&g_perm_cache_mu);
}

                                                                 
                                      
                                                                    

static GHashTable *g_user_rate = nullptr;                                        
static GMutex      g_user_rate_mu;
#define USER_RATE_LIMIT  100                 
#define USER_RATE_WINDOW  60                

typedef struct { gint count; gint64 window_start; } UserRateInfo;

   
                                                        
                                                      
                                                      
  
                                                                     
                                                                    
                                                    
                           
   
gboolean
pcv_rbac_check_user_rate(const gchar *username)
{
    if (!username) return TRUE;                        
    if (!g_user_rate) {                     
        g_mutex_init(&g_user_rate_mu);
        g_user_rate = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    }
    g_mutex_lock(&g_user_rate_mu);
    UserRateInfo *info = g_hash_table_lookup(g_user_rate, username);
    gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;                       
    if (!info) {
                                              
        info = g_new0(UserRateInfo, 1);
        info->window_start = now;
        g_hash_table_insert(g_user_rate, g_strdup(username), info);
    }
    if (now - info->window_start >= USER_RATE_WINDOW) {
                                          
        info->count = 0;
        info->window_start = now;
    }
    info->count++;                 
    gboolean allowed = (info->count <= USER_RATE_LIMIT);                   
    g_mutex_unlock(&g_user_rate_mu);
    return allowed;
}

                                                                 
                            
                                                               
  
                                                  
                                                             
                                                       
                                                        
                                                                    

   
                                                                                   
                    
  
                                                              
                                                 
                                       
  
                                                    
                                                                       
           
  
                                                                    
   
gboolean
pcv_rbac_totp_delete_rows_locked(const gchar *username)
{
    sqlite3_stmt *stmt = nullptr;

    int rc = sqlite3_prepare_v2(g_rbac_db,
        "DELETE FROM user_totp WHERE username = ?;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return FALSE;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return FALSE;

    stmt = nullptr;
    rc = sqlite3_prepare_v2(g_rbac_db,
        "DELETE FROM totp_recovery_codes WHERE username = ?;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return FALSE;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

   
                                                    
   
gboolean
pcv_rbac_totp_status(const gchar *username, PcvTotpStatus *out)
{
    g_return_val_if_fail(username && *username && out, FALSE);

    out->enrolled = FALSE;
    out->confirmed = FALSE;
    out->recovery_remaining = 0;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT confirmed FROM user_totp WHERE username = ?;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->enrolled  = TRUE;
        out->confirmed = sqlite3_column_int(stmt, 0) != 0;
    }
    sqlite3_finalize(stmt);

    if (out->enrolled) {
        sqlite3_stmt *cstmt = nullptr;
        rc = sqlite3_prepare_v2(g_rbac_db,
            "SELECT COUNT(*) FROM totp_recovery_codes WHERE username = ? AND used = 0;",
            -1, &cstmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(cstmt, 1, username, -1, SQLITE_STATIC);
            if (sqlite3_step(cstmt) == SQLITE_ROW)
                out->recovery_remaining = sqlite3_column_int(cstmt, 0);
            sqlite3_finalize(cstmt);
        }
    }

    g_mutex_unlock(&g_rbac_mutex);
    return TRUE;
}

   
                                                    
   
gchar *
pcv_rbac_totp_enroll(const gchar *username, const gchar *identity_nonce, GError **error)
{
    g_return_val_if_fail(username && *username, NULL);

    g_mutex_lock(&g_rbac_mutex);

                                                                                            
    PcvSubjectCheck who = pcv_rbac_identity_check_locked(g_rbac_db, username, identity_nonce, NULL);
    if (who != PCV_SUBJECT_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, who == PCV_SUBJECT_REJECT ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                    who == PCV_SUBJECT_REJECT ? "Invalid token" : "Authentication store unavailable");
        return NULL;
    }

                                                             
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT confirmed FROM user_totp WHERE username = ?;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        return NULL;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    gboolean exists    = (rc == SQLITE_ROW);
    gboolean confirmed = exists && sqlite3_column_int(stmt, 0) != 0;
    sqlite3_finalize(stmt);

    if (confirmed) {
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "TOTP already confirmed for '%s' — disable first", username);
        return NULL;
    }

    gchar *secret = pcv_totp_generate_secret();                                
    gint64 now    = g_get_real_time() / G_USEC_PER_SEC;

    sqlite3_stmt *wstmt = nullptr;
    if (exists) {
                                                             
        rc = sqlite3_prepare_v2(g_rbac_db,
            "UPDATE user_totp SET secret=?, created_at=?, last_step=0 WHERE username=?;",
            -1, &wstmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text (wstmt, 1, secret, -1, SQLITE_STATIC);
            sqlite3_bind_int64(wstmt, 2, now);
            sqlite3_bind_text (wstmt, 3, username, -1, SQLITE_STATIC);
        }
    } else {
        rc = sqlite3_prepare_v2(g_rbac_db,
            "INSERT INTO user_totp (username, secret, confirmed, created_at, last_step) "
            "VALUES (?, ?, 0, ?, 0);",
            -1, &wstmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text (wstmt, 1, username, -1, SQLITE_STATIC);
            sqlite3_bind_text (wstmt, 2, secret, -1, SQLITE_STATIC);
            sqlite3_bind_int64(wstmt, 3, now);
        }
    }

    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        pcv_secure_free_str(&secret);                                   
        return NULL;
    }

    rc = sqlite3_step(wstmt);
    sqlite3_finalize(wstmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error: %s", sqlite3_errmsg(g_rbac_db));
        pcv_secure_free_str(&secret);                                   
        return NULL;
    }

    return secret;                                    
}

   
                                                         
  
                                                                 
                                                               
   
gboolean
pcv_rbac_totp_verify_code(const gchar *username, const gchar *code,
                          gboolean allow_recovery,
                          gboolean *out_first_confirm, GError **error)
{
    g_return_val_if_fail(username && *username && code, FALSE);
    if (out_first_confirm) *out_first_confirm = FALSE;

                                                 
    gint remain = 0;
    if (pcv_rbac_attempts_totp_locked(username, &remain)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "TOTP locked — retry after %d seconds", remain);
        return FALSE;
    }

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT secret, confirmed, last_step FROM user_totp WHERE username = ?;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
                                                                       
                                                                      
        sqlite3_finalize(stmt);
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "TOTP not enrolled for '%s'", username);
        return FALSE;
    }

    gchar  *secret_dup = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    gboolean was_confirmed = sqlite3_column_int(stmt, 1) != 0;
    gint64  last_step  = sqlite3_column_int64(stmt, 2);
    sqlite3_finalize(stmt);                                   

    gboolean success     = FALSE;
    gboolean via_recovery = FALSE;
    gint64   matched_step = 0;
                                                                
                                                             
                                                             
                                                           
                                                     
                                      
    gboolean consume_failed = FALSE;

                                                                         
    if (allow_recovery && strlen(code) == 11 && code[5] == '-') {
        gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, code, -1);

        sqlite3_stmt *rstmt = nullptr;
        rc = sqlite3_prepare_v2(g_rbac_db,
            "SELECT 1 FROM totp_recovery_codes "
            "WHERE username = ? AND code_hash = ? AND used = 0;",
            -1, &rstmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(rstmt, 1, username, -1, SQLITE_STATIC);
            sqlite3_bind_text(rstmt, 2, hash, -1, SQLITE_STATIC);
            if (sqlite3_step(rstmt) == SQLITE_ROW) {
                success = TRUE;
                via_recovery = TRUE;
            }
            sqlite3_finalize(rstmt);
        }

        if (success) {
                                                        
                                                           
                                                            
            sqlite3_stmt *ustmt = nullptr;
            gboolean consume_ok = FALSE;
            rc = sqlite3_prepare_v2(g_rbac_db,
                "UPDATE totp_recovery_codes SET used = 1 "
                "WHERE username = ? AND code_hash = ?;",
                -1, &ustmt, NULL);
            if (rc == SQLITE_OK) {
                sqlite3_bind_text(ustmt, 1, username, -1, SQLITE_STATIC);
                sqlite3_bind_text(ustmt, 2, hash, -1, SQLITE_STATIC);
                consume_ok = (sqlite3_step(ustmt) == SQLITE_DONE);
                sqlite3_finalize(ustmt);
            }
            if (!consume_ok) {
                PCV_LOG_ERROR(RBAC_LOG_DOM,
                    "TOTP recovery code consume(used=1) UPDATE failed for '%s': %s — "
                    "rejecting match to avoid unlimited-reuse window (fail-closed)",
                    username, sqlite3_errmsg(g_rbac_db));
                success = FALSE;
                consume_failed = TRUE;
            }
        }
        g_free(hash);
    } else {
                                                                     
                                                       
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        gint64 out_step = 0;
        if (pcv_totp_check(secret_dup, code, now, last_step, &out_step)) {
            success = TRUE;
            matched_step = out_step;
        }
    }

    if (success && !via_recovery) {
                                                             
                                                          
                                                               
        sqlite3_stmt *ustmt = nullptr;
        gboolean consume_ok = FALSE;
        const gchar *sql = was_confirmed
            ? "UPDATE user_totp SET last_step = ? WHERE username = ?;"
            : "UPDATE user_totp SET last_step = ?, confirmed = 1 WHERE username = ?;";
        rc = sqlite3_prepare_v2(g_rbac_db, sql, -1, &ustmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(ustmt, 1, matched_step);
            sqlite3_bind_text (ustmt, 2, username, -1, SQLITE_STATIC);
            consume_ok = (sqlite3_step(ustmt) == SQLITE_DONE);
            sqlite3_finalize(ustmt);
        }
        if (consume_ok) {
                                                                
                                                              
                                                  
            if (!was_confirmed && out_first_confirm) *out_first_confirm = TRUE;
        } else {
            PCV_LOG_ERROR(RBAC_LOG_DOM,
                "TOTP last_step/confirmed UPDATE failed for '%s': %s — "
                "rejecting match to avoid replay window (fail-closed)",
                username, sqlite3_errmsg(g_rbac_db));
            success = FALSE;
            consume_failed = TRUE;
        }
    }

    g_mutex_unlock(&g_rbac_mutex);
    pcv_secure_free_str(&secret_dup);                                

                                                       
                                                          
                                                
    if (success || !consume_failed)
        pcv_rbac_attempts_totp_record(username, success);

    if (!success) {
        if (consume_failed) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "TOTP code matched but failed to persist consumption "
                        "(DB error) — rejected to avoid replay/reuse window");
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "Invalid TOTP code");
        }
        return FALSE;
    }
    return TRUE;
}

   
                                                               
   
GPtrArray *
pcv_rbac_totp_generate_recovery(const gchar *username, GError **error)
{
    g_return_val_if_fail(username && *username, NULL);

                                                         
                           
    GPtrArray *codes  = g_ptr_array_new_with_free_func(g_free);                          
    GPtrArray *hashes = g_ptr_array_new_with_free_func(g_free);              

    for (int i = 0; i < 10; i++) {
        guchar raw[5];
        _fill_random_bytes(raw, sizeof raw);                                 

        GString *hex = g_string_sized_new(10 + 1);
        for (int j = 0; j < 5; j++)
            g_string_append_printf(hex, "%02x", raw[j]);                       

        gchar *plain = g_strdup_printf("%.5s-%.5s", hex->str, hex->str + 5);                    
        g_string_free(hex, TRUE);

        gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, plain, -1);

        g_ptr_array_add(codes, plain);
        g_ptr_array_add(hashes, hash);
    }

    g_mutex_lock(&g_rbac_mutex);

                                            
    sqlite3_stmt *dstmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "DELETE FROM totp_recovery_codes WHERE username = ?;", -1, &dstmt, NULL);
    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_ptr_array_unref(codes);
        g_ptr_array_unref(hashes);
        return NULL;
    }
    sqlite3_bind_text(dstmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(dstmt);
    sqlite3_finalize(dstmt);
    if (rc != SQLITE_DONE) {
                                                                      
                                                         
                                                       
                                                                      
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error clearing old recovery codes: %s", sqlite3_errmsg(g_rbac_db));
        g_ptr_array_unref(codes);
        g_ptr_array_unref(hashes);
        return NULL;
    }

    gboolean ok = TRUE;
    for (guint i = 0; i < hashes->len; i++) {
        sqlite3_stmt *istmt = nullptr;
        rc = sqlite3_prepare_v2(g_rbac_db,
            "INSERT INTO totp_recovery_codes (username, code_hash, used) VALUES (?, ?, 0);",
            -1, &istmt, NULL);
        if (rc != SQLITE_OK) { ok = FALSE; break; }
        sqlite3_bind_text(istmt, 1, username, -1, SQLITE_STATIC);
        sqlite3_bind_text(istmt, 2, (const gchar *)g_ptr_array_index(hashes, i), -1, SQLITE_STATIC);
        rc = sqlite3_step(istmt);
        sqlite3_finalize(istmt);
        if (rc != SQLITE_DONE) { ok = FALSE; break; }
    }

    g_mutex_unlock(&g_rbac_mutex);
    g_ptr_array_unref(hashes);                               

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error inserting recovery codes: %s", sqlite3_errmsg(g_rbac_db));
        g_ptr_array_unref(codes);
        return NULL;
    }

    return codes;                                                               
}

   
                                                     
   
gboolean
pcv_rbac_totp_disable(const gchar *username, GError **error)
{
    g_return_val_if_fail(username && *username, FALSE);

    g_mutex_lock(&g_rbac_mutex);
    gboolean ok = pcv_rbac_totp_delete_rows_locked(username);
    g_mutex_unlock(&g_rbac_mutex);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error disabling TOTP: %s", sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }
    return TRUE;                                                     
}
