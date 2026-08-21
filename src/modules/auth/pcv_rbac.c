   
                   
                                                                  
  
                           
                                                   
                                                    
                                        
  
          
                                               
                                                    
                                                
                                               
                                                
                        
  
         
                                                                        
                                                           
        
                                                              
                                                       
                                                                            
         
                                                     
        
                                                            
                                                                       
                                                   
  
          
                                          
                                                   
                                                 
                                          
  
            
                   
                                                                           
                                     
                                                                          
                               
                                                                   
                                
                                                     
  
           
                                             
               
                                         
                                                                     
                                                       
                                                                
                                                        
  
                                     
                                                                          
                                                                 
                                                                  
                                                               
                                                       
  
            
                                               
                                                
                                              
                                                 
  
          
                                       
                                                  
                                        
                                                   
                                                     
                                               
                                                                    
  
           
                                                
                                             
  
         
                                                                     
                                                   
                                                       
                                                      
   

#include "pcv_rbac.h"
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

                                                         
#define ACCESS_TOKEN_EXPIRY   900                                     
#define REFRESH_TOKEN_EXPIRY  604800          
#define REFRESH_TOKEN_BYTES   32                                   

                                                                 
               
                                                                    

static sqlite3 *g_rbac_db    = nullptr;                                    
static GMutex   g_rbac_mutex;                                   

                                                                
static void _ensure_apikey_table(void);
static void _migrate_api_keys_schema(void);
                                                                     
static void _migrate_apikey_columns(void);
                                                             
static void _migrate_freeze_apikey_effective_roles(void);
                                                                    
                                                       
                                                
static gboolean _totp_delete_rows(const gchar *username);

                                                                        
                                                     
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

                                                                 
                                  
  
                                                
                                                
                                                         
  
                                                   
                
                                               
                                                       
                                                      
                                               
                                                                    

typedef struct {
    gint    attempts;                     
    gint    lockout_count;                         
    gint64  locked_until;                                            
} LoginAttemptInfo;

static GHashTable *g_login_attempts = nullptr;                                    
static GMutex      g_attempts_mu;

                                                                     
                                                                   
                                                
static GHashTable *g_ip_attempts = nullptr;                                        

                                                         
                                                                      
                                                                    
static GHashTable *g_totp_attempts = nullptr;                                               

#define BRUTE_MAX_ATTEMPTS  5
static const gint  BRUTE_BACKOFF_SEC[] = { 30, 60, 300, 3600 };
#define BRUTE_BACKOFF_STAGES  4

#define BRUTE_IP_MAX_ATTEMPTS  20
#define BRUTE_IP_LOCKOUT_SEC   300

   
                                                      
                                          
                                                                    
                                              
                                                   
                                        
   
static void
_brute_ensure_init(void)
{
    if (!g_login_attempts) {                                  
        g_mutex_init(&g_attempts_mu);
        g_login_attempts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, g_free);                            
        g_ip_attempts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
        g_totp_attempts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, g_free);                        
    }
}

   
                                                           
                                                
                                                             
                                     
   
static LoginAttemptInfo *
_brute_get_info(const gchar *username)
{
    _brute_ensure_init();
    LoginAttemptInfo *info = g_hash_table_lookup(g_login_attempts, username);
    if (!info) {
                                                            
        info = g_new0(LoginAttemptInfo, 1);
        g_hash_table_insert(g_login_attempts, g_strdup(username), info);
    }
    return info;
}

   
                                                            
                                                      
                                                         
                                                    
   
static gboolean
_brute_check_locked(const gchar *username)
{
    LoginAttemptInfo *info = _brute_get_info(username);
    if (info->locked_until <= 0) return FALSE;                    
    gint64 now = g_get_monotonic_time();                                       
    if (now >= info->locked_until) {
                                                          
                                                     
                                       
        info->locked_until = 0;
        info->attempts = 0;
        return FALSE;
    }
    return TRUE;                                        
}

   
                                                                     
                                                      
                                                                     
   
static void
_brute_record_failure(const gchar *username)
{
    LoginAttemptInfo *info = _brute_get_info(username);
    info->attempts++;                     

    if (info->attempts >= BRUTE_MAX_ATTEMPTS) {                      
                                                              
                                                               
        gint stage = info->lockout_count;
        if (stage >= BRUTE_BACKOFF_STAGES)
            stage = BRUTE_BACKOFF_STAGES - 1;                          
        gint backoff_sec = BRUTE_BACKOFF_SEC[stage];                            

                                                       
        info->locked_until = g_get_monotonic_time() + (gint64)backoff_sec * G_USEC_PER_SEC;
        info->lockout_count++;                            

        PCV_LOG_WARN(RBAC_LOG_DOM,
            "Account '%s' locked for %d seconds (attempt %d, lockout #%d)",
            username, backoff_sec, info->attempts, info->lockout_count);
    }
}

   
                                                                  
                                                    
   
static void
_brute_record_success(const gchar *username)
{
    LoginAttemptInfo *info = _brute_get_info(username);
    info->attempts = 0;                     
    info->locked_until = 0;              
    info->lockout_count = 0;                             
}

                                                                     
                                                 
                                                              
                                        

   
                                                                     
                                       
   
static LoginAttemptInfo *
_brute_ip_get_info(const gchar *ip)
{
    LoginAttemptInfo *info = g_hash_table_lookup(g_ip_attempts, ip);
    if (!info) {
        info = g_new0(LoginAttemptInfo, 1);
        g_hash_table_insert(g_ip_attempts, g_strdup(ip), info);
    }
    return info;
}

   
                                                                         
                                                  
                                                      
   
static gboolean
_brute_ip_check_locked(const gchar *ip)
{
    if (!ip || !*ip) return FALSE;                         
    LoginAttemptInfo *info = _brute_ip_get_info(ip);
    if (info->locked_until <= 0) return FALSE;
    gint64 now = g_get_monotonic_time();
    if (now >= info->locked_until) {
                                                      
                                                       
        info->locked_until = 0;
        info->attempts = 0;
        return FALSE;
    }
    return TRUE;
}

   
                                                                             
                                                
   
static void
_brute_ip_record_failure(const gchar *ip)
{
    if (!ip || !*ip) return;                         
    LoginAttemptInfo *info = _brute_ip_get_info(ip);
    info->attempts++;
    if (info->attempts >= BRUTE_IP_MAX_ATTEMPTS) {                       
        info->locked_until = g_get_monotonic_time()
                             + (gint64)BRUTE_IP_LOCKOUT_SEC * G_USEC_PER_SEC;              
        PCV_LOG_WARN(RBAC_LOG_DOM,
            "IP '%s' locked for %d seconds (%d failed attempts)",
            ip, BRUTE_IP_LOCKOUT_SEC, info->attempts);
    }
}

   
                                                                        
                                          
   
static void
_brute_ip_record_success(const gchar *ip)
{
    if (!ip || !*ip) return;
    LoginAttemptInfo *info = _brute_ip_get_info(ip);
    info->attempts = 0;
    info->locked_until = 0;
}

                                                                
                                                       
                                                  
                                                   

   
                                                                 
                                                  
                                                                        
   
static LoginAttemptInfo *
_totp_brute_get_info(const gchar *username)
{
    _brute_ensure_init();
    LoginAttemptInfo *info = g_hash_table_lookup(g_totp_attempts, username);
    if (!info) {
        info = g_new0(LoginAttemptInfo, 1);
        g_hash_table_insert(g_totp_attempts, g_strdup(username), info);
    }
    return info;
}

   
                                                                             
                                                          
   
static gboolean
_totp_brute_check_locked(const gchar *username)
{
    LoginAttemptInfo *info = _totp_brute_get_info(username);
    if (info->locked_until <= 0) return FALSE;
    gint64 now = g_get_monotonic_time();
    if (now >= info->locked_until) {
        info->locked_until = 0;
        info->attempts = 0;
        return FALSE;
    }
    return TRUE;
}

   
                                                                          
                                                     
                                        
   
static void
_totp_brute_record_failure(const gchar *username)
{
    LoginAttemptInfo *info = _totp_brute_get_info(username);
    info->attempts++;

    if (info->attempts >= BRUTE_MAX_ATTEMPTS) {
        gint stage = info->lockout_count;
        if (stage >= BRUTE_BACKOFF_STAGES)
            stage = BRUTE_BACKOFF_STAGES - 1;                          
        gint backoff_sec = BRUTE_BACKOFF_SEC[stage];

        info->locked_until = g_get_monotonic_time() + (gint64)backoff_sec * G_USEC_PER_SEC;
        info->lockout_count++;

        PCV_LOG_WARN(RBAC_LOG_DOM,
            "TOTP verification for '%s' locked for %d seconds (attempt %d, lockout #%d)",
            username, backoff_sec, info->attempts, info->lockout_count);
    }
}

   
                                                                       
                                                            
   
static void
_totp_brute_record_success(const gchar *username)
{
    LoginAttemptInfo *info = _totp_brute_get_info(username);
    info->attempts = 0;
    info->locked_until = 0;
    info->lockout_count = 0;
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
                                                                     
    PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
                       (const guchar *)salt, (int)strlen(salt),
                       iter, EVP_sha256(), 32, dk);

                              
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
                                                 
        gchar *iter_str = g_strndup(rest, (gsize)(colon - rest));                       
        gint64 v = g_ascii_strtoll(iter_str, NULL, 10);                        
        g_free(iter_str);
        if (v <= 0) return FALSE;                                  
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
_ensure_table(void)
{
    const gchar *sql_users =
        "CREATE TABLE IF NOT EXISTS users ("
        "  username      TEXT PRIMARY KEY NOT NULL,"
        "  password_hash TEXT NOT NULL,"
        "  salt          TEXT NOT NULL,"
        "  role          INTEGER NOT NULL DEFAULT 0,"
        "  tenant        TEXT"
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
        "  revoked            INTEGER NOT NULL DEFAULT 0"
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

                                                                
                                                  
                                                        
                                                                 
                                                    
      
                                                   
                                     
    if (g_strcmp0(method, "pool.conninfo") == 0 ||
        g_strcmp0(method, "dpdk.status") == 0 ||
        g_strcmp0(method, "dpdk.list") == 0 ||
        g_strcmp0(method, "dpdk.hugepage.info") == 0 ||
        g_strcmp0(method, "sriov.status") == 0 ||
        g_strcmp0(method, "sriov.list") == 0)
    {
        return PCV_ROLE_VIEWER;
    }

                                                  
                                                             
                                                        
                                                                  
                                                     
                                                               
      
                                                    
                                                    
                            
    if (g_strcmp0(method, "alert.history") == 0 ||
        g_strcmp0(method, "healing.pending") == 0 ||
        g_strcmp0(method, "healing.history") == 0 ||
        g_strcmp0(method, "agent.history") == 0 ||
        g_strcmp0(method, "config.history") == 0 ||
        g_strcmp0(method, "storage.pool.forecast") == 0)
    {
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

                                 
    if (g_strcmp0(method, "iso.list") == 0        ||
        g_strcmp0(method, "vm.limit") == 0        ||
        g_strcmp0(method, "ovn.status") == 0      ||
        g_strcmp0(method, "vpc.list") == 0        ||
        g_strcmp0(method, "vpc.get") == 0         ||
        g_strcmp0(method, "vpc.subnet.list") == 0 ||
        g_strcmp0(method, "vpc.attachment.list") == 0 ||
        g_strcmp0(method, "vpc.service.list") == 0 ||
        g_strcmp0(method, "vpc.status") == 0      ||
        g_strcmp0(method, "vm.import.status") == 0  ||
        g_strcmp0(method, "vm.export.status") == 0  ||
        g_strcmp0(method, "cloud.jobs.list") == 0)
    {
        return PCV_ROLE_VIEWER;
    }

                                                       
                                              
      
                                                               
                                           
                                                                          
                                                            
                                      
                                                            
                                                                           
                                                                
                                                            
                                                          
                                                                    
                                                             
    if (g_strcmp0(method, "push.subscribe") == 0 ||
        g_strcmp0(method, "push.unsubscribe") == 0 ||
        g_strcmp0(method, "push.mine") == 0)
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

                                                                       
                                                     
                                                      
                                                          
                                                               
      
                                                    
                                                              
                                                                
                                                       
                                                           
                                                           
               
      
                                                
                                                     
                   
    if (g_strcmp0(method, "auth.totp.status") == 0 ||
        g_strcmp0(method, "auth.totp.disable") == 0 ||
        g_strcmp0(method, "auth.totp.recovery.regenerate") == 0)
    {
        return PCV_ROLE_VIEWER;
    }

                                                                
                                                          
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
        g_strcmp0(method, "sriov.detach") == 0)
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
        g_strcmp0(method, "backup.replicate") == 0   ||
        g_strcmp0(method, "vm.import.ec2") == 0      ||
        g_strcmp0(method, "vm.export.ec2") == 0      ||
        g_strcmp0(method, "cloud.job.cancel") == 0)
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

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "INSERT INTO users (username, password_hash, salt, role, tenant) "
        "VALUES (?, ?, ?, ?, ?);",
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

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    g_free(salt);                                   
    g_free(hash);

    if (rc != SQLITE_DONE) {
                                                                
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "User '%s' already exists or DB error: %s",
                    username, sqlite3_errmsg(g_rbac_db));
        pcv_audit_log(NULL, "auth.user.create", username, "failed", -1, 0, NULL);
        return FALSE;
    }

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.user.create", username,
                  "User created (role=%s, tenant=%s)",
                  pcv_rbac_role_to_str(role), tenant ? tenant : "global");
    pcv_audit_log(NULL, "auth.user.create", username, "ok", 0, 0, NULL);
    return TRUE;
}

   
                        
                        
  
                                
                                                  
                                                   
               
  
                                                
                                                  
                 
  
                                                                                     
                              
   
PcvUserExistence
pcv_rbac_user_exists(const gchar *username)
{
    if (!username || !*username) return PCV_USER_ABSENT;

    g_mutex_lock(&g_rbac_mutex);
    if (!g_rbac_db) {
        g_mutex_unlock(&g_rbac_mutex);
        return PCV_USER_UNKNOWN;
    }

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
                                "SELECT 1 FROM users WHERE username = ? LIMIT 1;",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return PCV_USER_UNKNOWN;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (rc == SQLITE_ROW)  return PCV_USER_PRESENT;                  
    if (rc == SQLITE_DONE) return PCV_USER_ABSENT;                   
    return PCV_USER_UNKNOWN;                                    
}

   
                        
                        
                       
  
                        
                                                            
  
                                                
                                           
  
                                                                                    
                                          
                     
  
                                                                  
                                                        
                                                                   
   
gboolean
pcv_rbac_user_delete(const gchar *username, GError **error)
{
    g_return_val_if_fail(username && *username, FALSE);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "DELETE FROM users WHERE username = ?;",
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

    if (rc != SQLITE_DONE) {
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error: %s", sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }

    if (changes == 0) {
                                                              
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "User '%s' not found", username);
        return FALSE;
    }

                                                  
                                                   
                                                         
    if (!_totp_delete_rows(username)) {
        PCV_LOG_WARN(RBAC_LOG_DOM,
            "Failed to cascade-delete TOTP rows for deleted user '%s' — "
            "orphaned user_totp/totp_recovery_codes rows may remain", username);
    }

    g_mutex_unlock(&g_rbac_mutex);

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.user.delete", username,
                  "User deleted");
    pcv_audit_log(NULL, "auth.user.delete", username, "ok", 0, 0, NULL);
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
                       GError     **error)
{
    g_return_val_if_fail(username && *username, FALSE);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE users SET role = ? WHERE username = ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }

    sqlite3_bind_int (stmt, 1, (int)role);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_rbac_db);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error: %s", sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }

    if (changes == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "User '%s' not found", username);
        return FALSE;
    }

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.role.set", username,
                  "Role changed to %s", pcv_rbac_role_to_str(role));
    {
        gchar *detail = g_strdup_printf("role changed to %s", pcv_rbac_role_to_str(role));
        pcv_audit_log(NULL, "auth.role.change", username, detail, 0, 0, NULL);
        g_free(detail);
    }
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

                                                            
                                             
    GError *verr = nullptr;
    gchar *t = pcv_rbac_authenticate(username, old_password, &verr);
    if (!t) {
        if (verr) g_error_free(verr);                                
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Current password is incorrect");
        pcv_audit_log(username, "auth.password.change", username,
                      "fail", 401, 0, NULL);
        return FALSE;
    }
    g_free(t);                             

    gchar *salt = _generate_salt();
    gchar *hash = _hash_password(salt, new_password);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE users SET password_hash = ?, salt = ? WHERE username = ?;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        g_free(salt); g_free(hash);
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, hash,     -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, salt,     -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_rbac_db);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    g_free(salt);
    g_free(hash);

    if (rc != SQLITE_DONE || changes == 0) {
                                                          
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to update password for '%s'", username);
        pcv_audit_log(username, "auth.password.change", username,
                      "fail", 500, 0, NULL);
        return FALSE;
    }

                                                    
    pcv_rbac_revoke_session(username, NULL);

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.password.change", username,
                  "Password changed (sessions revoked)");
    pcv_audit_log(username, "auth.password.change", username, "ok", 0, 0, NULL);
    return TRUE;
}

                                                                 
                                 
                                                                    

   
                         
                    
                     
                                 
  
                                       
                                          
                           
  
                                                      
                                                       
                                                     
                                                     
                                          
  
                                          
                                                               
  
                                                
                                                        
  
                                                                                   
                                                                     
                                                   
                                                              
   

gchar *
pcv_rbac_authenticate(const gchar *username,
                      const gchar *password,
                      GError     **error)
{
    g_return_val_if_fail(username && *username, NULL);
    g_return_val_if_fail(password && *password, NULL);

                                                               
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    if (_brute_check_locked(username)) {
                                                       
        LoginAttemptInfo *info = g_hash_table_lookup(g_login_attempts, username);
        gint remain = info ? (gint)((info->locked_until - g_get_monotonic_time()) / G_USEC_PER_SEC) : 0;
        g_mutex_unlock(&g_attempts_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Account locked — retry after %d seconds", remain > 0 ? remain : 1);
        return NULL;
    }
    g_mutex_unlock(&g_attempts_mu);                                   

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT password_hash, salt FROM users WHERE username = ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);

    if (rc != SQLITE_ROW) {
                                                              
                                                               
        sqlite3_finalize(stmt);
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid credentials");
        pcv_audit_log(username, "auth.login.failed", username, "user not found", 404, 0, NULL);
        return NULL;
    }

                                                                             
    const gchar *stored_hash = (const gchar *)sqlite3_column_text(stmt, 0);
    const gchar *stored_salt = (const gchar *)sqlite3_column_text(stmt, 1);

                                   
                                                 
                                     
                                                      
    gboolean match = FALSE;
    if (g_str_has_prefix(stored_hash, "pbkdf2:")) {
                                                   
                                                         
        gint         stored_iter = 0;
        const gchar *stored_hex  = NULL;
                                                       
                                                        
        if (_pbkdf2_parse(stored_hash, &stored_iter, &stored_hex) &&
            strlen(stored_hex) >= 64) {
            gchar *cand_hex = _pbkdf2_hex(stored_salt, password, stored_iter);                   
                                                           
            match = (CRYPTO_memcmp(stored_hex, cand_hex, 64) == 0);
            g_free(cand_hex);
        }

                                                       
                                                              
        if (match && stored_iter < _pbkdf2_target_iterations()) {
            gchar *new_hash = _hash_password_pbkdf2(stored_salt, password);
            if (new_hash) {
                sqlite3_stmt *upd = nullptr;
                if (sqlite3_prepare_v2(g_rbac_db,
                        "UPDATE users SET password_hash=? WHERE username=?",
                        -1, &upd, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(upd, 1, new_hash, -1, SQLITE_STATIC);
                    sqlite3_bind_text(upd, 2, username, -1, SQLITE_STATIC);
                    if (sqlite3_step(upd) == SQLITE_DONE) {
                        PCV_LOG_INFO(RBAC_LOG_DOM,
                            "Rehashed password to PBKDF2 %d iterations for user '%s'",
                            _pbkdf2_target_iterations(), username);
                    }
                    sqlite3_finalize(upd);
                }
                g_free(new_hash);
            }
        }
    } else {
                                           
                                                    
        gchar *legacy_hash = _hash_password_legacy(stored_salt, password);
        match = (strlen(stored_hash) >= 64) &&
                (CRYPTO_memcmp(stored_hash, legacy_hash, 64) == 0);                
        g_free(legacy_hash);

                                                               
        if (match) {
            PCV_LOG_INFO(RBAC_LOG_DOM,
                "Legacy SHA256 hash accepted for '%s' — will auto-migrate",
                username);
        }

                                            
                                                        
                                            
        if (match) {
            gchar *new_hash = _hash_password_pbkdf2(stored_salt, password);
            if (new_hash) {
                sqlite3_stmt *upd = nullptr;                                    
                if (sqlite3_prepare_v2(g_rbac_db,
                        "UPDATE users SET password_hash=? WHERE username=?",
                        -1, &upd, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(upd, 1, new_hash, -1, SQLITE_STATIC);
                    sqlite3_bind_text(upd, 2, username, -1, SQLITE_STATIC);
                    if (sqlite3_step(upd) == SQLITE_DONE) {
                        PCV_LOG_INFO(RBAC_LOG_DOM,
                                     "Migrated password hash to PBKDF2 for user '%s'", username);
                    }
                    sqlite3_finalize(upd);
                }
                g_free(new_hash);
                                                          
            }
        }
    }

    sqlite3_finalize(stmt);                                               
    g_mutex_unlock(&g_rbac_mutex);

    if (!match) {
                                                                 
        g_mutex_lock(&g_attempts_mu);
        _brute_record_failure(username);
        g_mutex_unlock(&g_attempts_mu);

        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid credentials");                       
        PCV_LOG_WARN(RBAC_LOG_DOM,
                     "Authentication failed for user '%s'", username);
        pcv_audit_log(username, "auth.login.failed", username, "invalid credentials", 401, 0, NULL);
        return NULL;
    }

                                                          
    g_mutex_lock(&g_attempts_mu);
    _brute_record_success(username);
    g_mutex_unlock(&g_attempts_mu);

                                                               
    gchar *token = pcv_jwt_sign(username, 0, error);
    if (token) {
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "User '%s' authenticated successfully", username);
        pcv_audit_log(username, "auth.login", username, "ok", 0, 0, NULL);
    }
    return token;
}

                                                                 
                                      
                                                                    

   
                           
  
                                               
                                                              
  
                                                           
                                           
  
                                                    
   
static gchar *
_generate_refresh_token(void)
{
    guchar raw[REFRESH_TOKEN_BYTES];
    _fill_random_bytes(raw, sizeof(raw));                                

    GString *hex = g_string_sized_new(REFRESH_TOKEN_BYTES * 2 + 1);
    for (gsize i = 0; i < sizeof(raw); i++)
        g_string_append_printf(hex, "%02x", raw[i]);

    return g_string_free(hex, FALSE);
}

   
                       
                                          
  
                                          
                                      
  
                                                    
                                             
  
                                                          
   
static gchar *
_hash_refresh_token(const gchar *token)
{
    GChecksum *cksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(cksum, (const guchar *)token, (gssize)strlen(token));
    gchar *hex = g_strdup(g_checksum_get_string(cksum));
    g_checksum_free(cksum);
    return hex;
}

   
                  
                         
                                                          
  
                                 
                                  
  
                                                  
                         
  
                                                               
                                 
   
static gboolean
_store_session(const gchar *username, const gchar *token_hash)
{
    sqlite3_stmt *stmt = nullptr;
                                                                  
    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;

    int rc = sqlite3_prepare_v2(g_rbac_db,
        "INSERT INTO sessions (username, refresh_token_hash, created_at, expires_at, revoked) "
        "VALUES (?, ?, ?, ?, 0);",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to prepare session INSERT: %s",
                      sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }

    sqlite3_bind_text (stmt, 1, username,   -1, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 2, token_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, now);                                        
    sqlite3_bind_int64(stmt, 4, now + REFRESH_TOKEN_EXPIRY);                          

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to store session: %s",
                      sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }

    return TRUE;
}

                                                                 
                                                                   
                                                                    

   
                           
  
                                                                          
                                                         
                                                      
  
                                                
                                                         
  
                                                                
                                                                         
                                                                
                                                                   
                                                 
   
gboolean
pcv_rbac_password_check(const gchar *username,
                        const gchar *password,
                        GError     **error)
{
    g_return_val_if_fail(username && *username, FALSE);
    g_return_val_if_fail(password && *password, FALSE);

                           
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    if (_brute_check_locked(username)) {
        LoginAttemptInfo *info = g_hash_table_lookup(g_login_attempts, username);
        gint remain = info ? (gint)((info->locked_until - g_get_monotonic_time()) / G_USEC_PER_SEC) : 0;
        g_mutex_unlock(&g_attempts_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Account locked — retry after %d seconds", remain > 0 ? remain : 1);
        return FALSE;
    }
    g_mutex_unlock(&g_attempts_mu);

                                                       
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

    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid credentials");
        pcv_audit_log(username, "auth.login.failed", username, "user not found", 404, 0, NULL);
        return FALSE;
    }

    const gchar *stored_hash = (const gchar *)sqlite3_column_text(stmt, 0);
    const gchar *stored_salt = (const gchar *)sqlite3_column_text(stmt, 1);

                                    
                                                                    
                                                                     
                                                               
                                                                    
                                                           
                                                                    
    gboolean match = FALSE;
    if (g_str_has_prefix(stored_hash, "pbkdf2:")) {
        gchar *pbkdf2_hash = _hash_password_pbkdf2(stored_salt, password);                   
        const gchar *stored_hex = stored_hash + 7;                               
        const gchar *computed_hex = pbkdf2_hash + 7;
        match = (strlen(stored_hex) >= 64) &&
                (CRYPTO_memcmp(stored_hex, computed_hex, 64) == 0);                    
        g_free(pbkdf2_hash);
    } else {
        gchar *legacy_hash = _hash_password_legacy(stored_salt, password);
        match = (strlen(stored_hash) >= 64) &&
                (CRYPTO_memcmp(stored_hash, legacy_hash, 64) == 0);
        g_free(legacy_hash);

                                              
        if (match) {
            gchar *new_hash = _hash_password_pbkdf2(stored_salt, password);
            if (new_hash) {
                sqlite3_stmt *upd = nullptr;
                if (sqlite3_prepare_v2(g_rbac_db,
                        "UPDATE users SET password_hash=? WHERE username=?",
                        -1, &upd, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(upd, 1, new_hash, -1, SQLITE_STATIC);
                    sqlite3_bind_text(upd, 2, username, -1, SQLITE_STATIC);
                    if (sqlite3_step(upd) == SQLITE_DONE) {
                        PCV_LOG_INFO(RBAC_LOG_DOM,
                                     "Migrated password hash to PBKDF2 for user '%s'", username);
                    }
                    sqlite3_finalize(upd);
                }
                g_free(new_hash);
            }
        }
    }

    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (!match) {
                                         
        g_mutex_lock(&g_attempts_mu);
        _brute_record_failure(username);
        g_mutex_unlock(&g_attempts_mu);

        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid credentials");
        PCV_LOG_WARN(RBAC_LOG_DOM,
                     "Authentication failed for user '%s'", username);
        pcv_audit_log(username, "auth.login.failed", username, "invalid credentials", 401, 0, NULL);
        return FALSE;
    }

                                                                 
                                                                     
    g_mutex_lock(&g_attempts_mu);
    _brute_record_success(username);
    g_mutex_unlock(&g_attempts_mu);

    return TRUE;
}

   
                         
  
                                                            
                                                       
                                                                           
                                                               
  
                                                
                                                      
                                     
  
                                                      
  
                                                                                    
                                                                          
                                                            
                                                             
                                                 
                                                 
                       
                                                                   
                                                          
   
gchar *
pcv_rbac_issue_tokens(const gchar *username,
                      gchar      **out_refresh_token,
                      GError     **error)
{
    g_return_val_if_fail(username && *username, NULL);

    g_mutex_lock(&g_rbac_mutex);

                                                         
                                                                    
                                                                  
                                                   
                                           
                                                                    
                                                      
                                                             
                                                   
    sqlite3_stmt *chk = nullptr;
    int chk_rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT 1 FROM users WHERE username = ?;",
        -1, &chk, NULL);

    if (chk_rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        return NULL;
    }

    sqlite3_bind_text(chk, 1, username, -1, SQLITE_STATIC);
    chk_rc = sqlite3_step(chk);
    sqlite3_finalize(chk);

    if (chk_rc != SQLITE_ROW) {
                                                               
                                                        
                                                             
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "User '%s' no longer exists", username);
        return NULL;
    }

                                                        
                                                                 
    gchar *refresh = nullptr;
    if (out_refresh_token) {
        refresh = _generate_refresh_token();
        gchar *hash = _hash_refresh_token(refresh);                    

        if (!_store_session(username, hash)) {
            g_free(hash);
            g_free(refresh);
            g_mutex_unlock(&g_rbac_mutex);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to create session");
            return NULL;
        }
        g_free(hash);
    }

    g_mutex_unlock(&g_rbac_mutex);

                                                               
    gchar *token = pcv_jwt_sign(username, ACCESS_TOKEN_EXPIRY, error);
    if (token) {
        if (out_refresh_token)
            *out_refresh_token = refresh;
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "User '%s' authenticated (v2, refresh=%s)",
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
    if (!pcv_rbac_password_check(username, password, error))
        return NULL;

    return pcv_rbac_issue_tokens(username, out_refresh_token, error);
}

                                                                 
                                                  
                                                                    

   
                          
  
                                                               
                                                    
  
                                                   
                                                   
  
                                                              
                                                                
                                                          
                                                            
   
gchar *
pcv_rbac_refresh_token(const gchar *refresh_token,
                       gchar      **out_new_refresh,
                       GError     **error)
{
    g_return_val_if_fail(refresh_token && *refresh_token, NULL);

    gchar *token_hash = _hash_refresh_token(refresh_token);                           
    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;                        

    g_mutex_lock(&g_rbac_mutex);

                                                      
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT id, username FROM sessions "
        "WHERE refresh_token_hash = ? AND revoked = 0 AND expires_at > ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        g_free(token_hash);
        return NULL;
    }

    sqlite3_bind_text (stmt, 1, token_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);
    rc = sqlite3_step(stmt);

    if (rc != SQLITE_ROW) {
                                                                
        sqlite3_finalize(stmt);
        g_mutex_unlock(&g_rbac_mutex);
        g_free(token_hash);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid or expired refresh token");
        PCV_LOG_WARN(RBAC_LOG_DOM, "Refresh token validation failed");
        pcv_audit_log(NULL, "auth.token.refresh", "unknown", "invalid token", 401, 0, NULL);
        return NULL;
    }

    gint64 session_id = sqlite3_column_int64(stmt, 0);                                
    gchar *username = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));                    
    sqlite3_finalize(stmt);

                                                      
                                                           
                                
    stmt = nullptr;
    rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE sessions SET revoked = 1 WHERE id = ?;",
        -1, &stmt, NULL);

    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, session_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

                                                            
    gchar *new_refresh = nullptr;
    if (out_new_refresh) {
        new_refresh = _generate_refresh_token();
        gchar *new_hash = _hash_refresh_token(new_refresh);

        if (!_store_session(username, new_hash)) {
            g_free(new_hash);
            g_free(new_refresh);
            g_free(username);
            g_mutex_unlock(&g_rbac_mutex);
            g_free(token_hash);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to rotate refresh token");
            return NULL;
        }
        g_free(new_hash);
    }

    g_mutex_unlock(&g_rbac_mutex);
    g_free(token_hash);

                                                              
    gchar *access_token = pcv_jwt_sign(username, ACCESS_TOKEN_EXPIRY, error);
    if (access_token) {
        if (out_new_refresh)
            *out_new_refresh = new_refresh;
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "Token refreshed for user '%s'", username);
        pcv_audit_log(username, "auth.token.refresh", username, "ok", 0, 0, NULL);
    } else {
        g_free(new_refresh);
    }

    g_free(username);
    return access_token;
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

   
                                 
                               
                                       
  
                       
                                              
  
                                                      
                                                  
  
                                                        
                           
   
gboolean
pcv_rbac_revoke_session_by_id(const gchar *username, gint64 session_id)
{
    if (!g_rbac_db || !username) return FALSE;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
                                                                  
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE sessions SET revoked=1 WHERE id=? AND username=? AND revoked=0",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }

    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_rbac_db);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    gboolean ok = (rc == SQLITE_DONE && changes > 0);
    if (ok) {
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "Revoked session %ld for user '%s'", (long)session_id, username);
        pcv_audit_log(username, "auth.session.revoke", username, "ok", 0, 0, NULL);
    }
    return ok;
}

                                                                 
                     
                                                                    

   
                             
                        
                                            
  
                                              
                                               
  
                                  
                                 
  
                                                     
                                                    
                                                      
  
                                                           
                                                   
   

gboolean
pcv_rbac_check_permission(const gchar *username,
                          const gchar *method)
{
    if (!username || !method) return FALSE;                   

    PcvRole user_role = pcv_rbac_get_role(username);                       
    PcvRole min_role  = _method_min_role(method);                          

    return (user_role >= min_role);                          
}

   
                     
                        
  
                       
                                                  
  
                                                     
                                                            
  
                                                              
   
PcvRole
pcv_rbac_get_role(const gchar *username)
{
                                      
    if (!username || !*username) return PCV_ROLE_VIEWER;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT role FROM users WHERE username = ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return PCV_ROLE_VIEWER;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);

    PcvRole role = PCV_ROLE_VIEWER;
    if (rc == SQLITE_ROW)
        role = (PcvRole)sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    return role;
}

   
                       
                        
  
                     
                                                
  
                                                    
                                                
  
             
                                         
                                            
                                       
  
                                                                   
                                                         
   
const gchar *
pcv_rbac_get_tenant(const gchar *username)
{
                                                      
                                                      
                                                     
    static __thread gchar t_tenant[256];

    if (!username || !*username) return NULL;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT tenant FROM users WHERE username = ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);

    const gchar *result = nullptr;
    if (rc == SQLITE_ROW) {
        const gchar *val = (const gchar *)sqlite3_column_text(stmt, 0);
        if (val) {
            g_strlcpy(t_tenant, val, sizeof(t_tenant));
            result = t_tenant;
        }
    }

    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    return result;
}

                                                                 
                                    
                                                                    

   
                        
                     
  
                                  
                         
  
                                                                         
  
                                                                
   

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
        "SELECT client_name, role FROM api_keys "
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
    sqlite3_finalize(stmt);
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

                                                                 
                        
                                                                    

   
                                                          
                                      
                                                   
   
gboolean
pcv_rbac_is_locked(const gchar *username)
{
    if (!username || !*username) return FALSE;
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    gboolean locked = _brute_check_locked(username);
    g_mutex_unlock(&g_attempts_mu);
    return locked;
}

   
                                                         
                                                    
                                                  
   
gint
pcv_rbac_get_remaining_lockout(const gchar *username)
{
    if (!username || !*username) return 0;
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    LoginAttemptInfo *info = g_hash_table_lookup(g_login_attempts, username);
    gint remaining = 0;
    if (info && info->locked_until > 0) {
        gint64 now = g_get_monotonic_time();
        if (now < info->locked_until)                             
            remaining = (gint)((info->locked_until - now) / G_USEC_PER_SEC);
    }
    g_mutex_unlock(&g_attempts_mu);
    return remaining;
}

                                              

   
                                                           
                                             
                                                                
   
gint
pcv_rbac_get_ip_remaining_lockout(const gchar *ip)
{
    if (!ip || !*ip) return 0;
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
                                                   
    gboolean locked = _brute_ip_check_locked(ip);
    gint remaining = 0;
    if (locked) {
        LoginAttemptInfo *info = g_hash_table_lookup(g_ip_attempts, ip);
        if (info && info->locked_until > 0) {
            gint64 now = g_get_monotonic_time();
            if (now < info->locked_until)
                remaining = (gint)((info->locked_until - now) / G_USEC_PER_SEC);
        }
    }
    g_mutex_unlock(&g_attempts_mu);
    return remaining;
}

   
                                                                 
                                                    
                                                
   
void
pcv_rbac_ip_record_auth_failure(const gchar *ip)
{
    if (!ip || !*ip) return;
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    _brute_ip_record_failure(ip);
    g_mutex_unlock(&g_attempts_mu);
}

   
                                                                   
                                          
   
void
pcv_rbac_ip_record_auth_success(const gchar *ip)
{
    if (!ip || !*ip) return;
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    _brute_ip_record_success(ip);
    g_mutex_unlock(&g_attempts_mu);
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

   
                                                  
                                                    
                                              
                                                                
   
gint
pcv_rbac_apikey_validate(const gchar *api_key)
{
    if (!api_key || !g_str_has_prefix(api_key, "pcv_")) return -1;               

    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, api_key, -1);            
    gint role = -1;
    gint64 now_epoch = g_get_real_time() / G_USEC_PER_SEC;              

                                                                  
    g_mutex_lock(&g_rbac_mutex);
    _ensure_apikey_table();
    sqlite3_stmt *stmt = nullptr;
                                                                
    if (sqlite3_prepare_v2(g_rbac_db,
        "SELECT role FROM api_keys WHERE key_hash = ? AND revoked = 0 "
        "AND (expires_at = 0 OR expires_at > ?)", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, now_epoch);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            role = sqlite3_column_int(stmt, 0);
    }
    if (stmt) sqlite3_finalize(stmt);

                                                                      
    if (role >= 0) {
        sqlite3_stmt *upd = nullptr;
        if (sqlite3_prepare_v2(g_rbac_db,
            "UPDATE api_keys SET last_used_at=datetime('now') WHERE key_hash=?",
            -1, &upd, NULL) == SQLITE_OK) {
            sqlite3_bind_text(upd, 1, hash, -1, SQLITE_TRANSIENT);
            sqlite3_step(upd);
            sqlite3_finalize(upd);
        }
    }
    g_mutex_unlock(&g_rbac_mutex);
    g_free(hash);
    return role;
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

                                                                 
                            
                                                               
  
                                                  
                                                             
                                                       
                                                        
                                                                    

   
                     
                    
  
                                                              
                                                 
                                       
  
                                                    
                                                                       
           
  
                                                                    
   
static gboolean
_totp_delete_rows(const gchar *username)
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
pcv_rbac_totp_enroll(const gchar *username, GError **error)
{
    g_return_val_if_fail(username && *username, NULL);

    g_mutex_lock(&g_rbac_mutex);

                                                             
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

                                                 
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    if (_totp_brute_check_locked(username)) {
        LoginAttemptInfo *info = g_hash_table_lookup(g_totp_attempts, username);
        gint remain = info ? (gint)((info->locked_until - g_get_monotonic_time()) / G_USEC_PER_SEC) : 0;
        g_mutex_unlock(&g_attempts_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "TOTP locked — retry after %d seconds", remain > 0 ? remain : 1);
        return FALSE;
    }
    g_mutex_unlock(&g_attempts_mu);                                   

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

                                                       
                                                          
                                                
    g_mutex_lock(&g_attempts_mu);
    if (success) _totp_brute_record_success(username);
    else if (!consume_failed) _totp_brute_record_failure(username);
    g_mutex_unlock(&g_attempts_mu);

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
    gboolean ok = _totp_delete_rows(username);
    g_mutex_unlock(&g_rbac_mutex);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error disabling TOTP: %s", sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }
    return TRUE;                                                     
}

   
                                                                   
   
gint
pcv_rbac_totp_get_remaining_lockout(const gchar *username)
{
    if (!username || !*username) return 0;
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    LoginAttemptInfo *info = g_hash_table_lookup(g_totp_attempts, username);
    gint remaining = 0;
    if (info && info->locked_until > 0) {
        gint64 now = g_get_monotonic_time();
        if (now < info->locked_until)
            remaining = (gint)((info->locked_until - now) / G_USEC_PER_SEC);
    }
    g_mutex_unlock(&g_attempts_mu);
    return remaining;
}
