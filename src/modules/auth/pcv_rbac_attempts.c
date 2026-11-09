   
                            
                                                                
  
                           
                                                                                             
                                                                                     
                                                                                                    
                                          
                                                                              
                                                                                         
  
                                                                        
   
#include "pcv_rbac.h"
#include "pcv_rbac_internal.h"
#include "../../utils/pcv_log.h"

#include <glib.h>
#include <string.h>

#define RBAC_LOG_DOM "pcv_rbac"

                                                                 
                                  
  
                                                
                                                
                                                         
  
                                                   
              
                                                                              
                                                           
                                                       
                                                      
                                               
                                                                    

typedef struct {
    gint    attempts;                     
    gint    lockout_count;                         
    gint64  locked_until;                                            
    gint64  last_failure;                                                                  
} LoginAttemptInfo;

static GHashTable *g_login_attempts = nullptr;                                    
static GMutex      g_attempts_mu;

                                                                      
                                                                                          
                                                  
                                                                                 
                                                                                                
#define BRUTE_LOGIN_CARDS_MAX 4096
#define BRUTE_LOGIN_KEY_MAX   256
static gint64 (*g_attempts_now_fn)(void) = g_get_monotonic_time;
static guint   g_login_cards_max = BRUTE_LOGIN_CARDS_MAX;
static gint64  g_login_full_warned_at = 0;                                               

                                                                     
                                                                   
                                                
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

   
                                                                            
                                               
                                                
   
static gchar *
_login_card_key(const gchar *username)
{
    if (strlen(username) <= BRUTE_LOGIN_KEY_MAX) return g_strdup(username);
    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, username, -1);
}

   
                                                                
                                                        
                                     
   
static LoginAttemptInfo *
_login_card_find(const gchar *username)
{
    g_autofree gchar *key = _login_card_key(username);
    return g_hash_table_lookup(g_login_attempts, key);
}

   
                                                                
                                                                           
                                                                             
                                
                                                 
                                                                                 
   
static gboolean
_cards_make_room(GHashTable *table, gint64 now, const gchar *what)
{
    if (g_hash_table_size(table) < g_login_cards_max)
        return TRUE;
    GHashTableIter it;
    gpointer key, val;
    gpointer oldest_key = NULL;
    gint64 oldest = G_MAXINT64;
    g_hash_table_iter_init(&it, table);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        const LoginAttemptInfo *info = val;
        if (now < info->locked_until) continue;                        
        if (info->last_failure < oldest) {
            oldest = info->last_failure;
            oldest_key = key;
        }
    }
    if (oldest_key && g_hash_table_remove(table, oldest_key))
        return TRUE;
    if (g_login_full_warned_at == 0 || now - g_login_full_warned_at >= 60 * G_USEC_PER_SEC) {
        g_login_full_warned_at = now;
        PCV_LOG_WARN(RBAC_LOG_DOM, "Login attempt table (%s) is full of locked entries (%u) — new entries are not tracked",
                     what, g_hash_table_size(table));
    }
    return FALSE;
}

   
                                                            
                                                      
                                                         
                                                                     
   
static gboolean
_brute_check_locked(const gchar *username)
{
    LoginAttemptInfo *info = _login_card_find(username);
    if (!info || info->locked_until <= 0) return FALSE;                         
    gint64 now = g_attempts_now_fn();                                          
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
    gint64 now = g_attempts_now_fn();
    LoginAttemptInfo *info = _login_card_find(username);
    if (!info) {
        if (!_cards_make_room(g_login_attempts, now, "name"))
            return;
        info = g_new0(LoginAttemptInfo, 1);
        g_hash_table_insert(g_login_attempts, _login_card_key(username), info);
    }
    info->attempts++;                     
    info->last_failure = now;

    if (info->attempts >= BRUTE_MAX_ATTEMPTS) {                      
                                                              
                                                               
        gint stage = info->lockout_count;
        if (stage >= BRUTE_BACKOFF_STAGES)
            stage = BRUTE_BACKOFF_STAGES - 1;                          
        gint backoff_sec = BRUTE_BACKOFF_SEC[stage];                            

                                                       
        info->locked_until = now + (gint64)backoff_sec * G_USEC_PER_SEC;
        info->lockout_count++;                            

        PCV_LOG_WARN(RBAC_LOG_DOM,
            "Account '%s' locked for %d seconds (attempt %d, lockout #%d)",
            username, backoff_sec, info->attempts, info->lockout_count);
    }
}

   
                                                                
                                                     
   
static void
_brute_record_success(const gchar *username)
{
    g_autofree gchar *key = _login_card_key(username);
    g_hash_table_remove(g_login_attempts, key);
}

                                                                     
                                                 
                                                              
                                        

   
                                                                         
                                                  
                                                      
   
static gboolean
_brute_ip_check_locked(const gchar *ip)
{
    if (!ip || !*ip) return FALSE;                         
    LoginAttemptInfo *info = g_hash_table_lookup(g_ip_attempts, ip);
    if (!info || info->locked_until <= 0) return FALSE;                        
    gint64 now = g_attempts_now_fn();
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
    gint64 now = g_attempts_now_fn();
    LoginAttemptInfo *info = g_hash_table_lookup(g_ip_attempts, ip);
    if (!info) {
        if (!_cards_make_room(g_ip_attempts, now, "IP"))
            return;
        info = g_new0(LoginAttemptInfo, 1);
        g_hash_table_insert(g_ip_attempts, g_strdup(ip), info);
    }
    info->attempts++;
    info->last_failure = now;
    if (info->attempts >= BRUTE_IP_MAX_ATTEMPTS) {                       
        info->locked_until = now + (gint64)BRUTE_IP_LOCKOUT_SEC * G_USEC_PER_SEC;              
        PCV_LOG_WARN(RBAC_LOG_DOM,
            "IP '%s' locked for %d seconds (%d failed attempts)",
            ip, BRUTE_IP_LOCKOUT_SEC, info->attempts);
    }
}

   
                                                                        
                                              
   
static void
_brute_ip_record_success(const gchar *ip)
{
    if (!ip || !*ip) return;
    g_hash_table_remove(g_ip_attempts, ip);
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

                                                           

                                                                           
static gint
_remaining_sec(const LoginAttemptInfo *info, gint64 now)
{
    gint remain = info ? (gint)((info->locked_until - now) / G_USEC_PER_SEC) : 0;
    return remain > 0 ? remain : 1;
}

                                                               
gboolean
pcv_rbac_attempts_login_locked(const gchar *username, gint *out_remain)
{
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    gboolean locked = _brute_check_locked(username);
    if (locked && out_remain)
        *out_remain = _remaining_sec(_login_card_find(username), g_attempts_now_fn());
    g_mutex_unlock(&g_attempts_mu);
    return locked;
}

                                                               
void
pcv_rbac_attempts_login_record(const gchar *username, gboolean success)
{
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    if (success) _brute_record_success(username);
    else _brute_record_failure(username);
    g_mutex_unlock(&g_attempts_mu);
}

                                                              
gboolean
pcv_rbac_attempts_totp_locked(const gchar *username, gint *out_remain)
{
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    gboolean locked = _totp_brute_check_locked(username);
    if (locked && out_remain)
        *out_remain = _remaining_sec(g_hash_table_lookup(g_totp_attempts, username), g_get_monotonic_time());
    g_mutex_unlock(&g_attempts_mu);
    return locked;
}

                                                              
void
pcv_rbac_attempts_totp_record(const gchar *username, gboolean success)
{
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    if (success) _totp_brute_record_success(username);
    else _totp_brute_record_failure(username);
    g_mutex_unlock(&g_attempts_mu);
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
    LoginAttemptInfo *info = _login_card_find(username);
    gint remaining = 0;
    if (info && info->locked_until > 0) {
        gint64 now = g_attempts_now_fn();
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
            gint64 now = g_attempts_now_fn();
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

                                                       

void pcv_rbac_attempts_reset_for_test(guint cards_max, gint64 (*now_fn)(void));
void pcv_rbac_attempts_login_stats_for_test(guint *cards, gsize *key_bytes);
gint pcv_rbac_attempts_login_card_for_test(const gchar *username);
void pcv_rbac_attempts_ip_stats_for_test(guint *cards);

                                                    
void
pcv_rbac_attempts_reset_for_test(guint cards_max, gint64 (*now_fn)(void))
{
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    g_hash_table_remove_all(g_login_attempts);
    g_hash_table_remove_all(g_ip_attempts);
    g_login_cards_max = cards_max ? cards_max : BRUTE_LOGIN_CARDS_MAX;
    g_attempts_now_fn = now_fn ? now_fn : g_get_monotonic_time;
    g_login_full_warned_at = 0;
    g_mutex_unlock(&g_attempts_mu);
}

                                     
void
pcv_rbac_attempts_login_stats_for_test(guint *cards, gsize *key_bytes)
{
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    *cards = g_hash_table_size(g_login_attempts);
    *key_bytes = 0;
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, g_login_attempts);
    while (g_hash_table_iter_next(&it, &key, NULL))
        *key_bytes += strlen(key);
    g_mutex_unlock(&g_attempts_mu);
}

                              
gint
pcv_rbac_attempts_login_card_for_test(const gchar *username)
{
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    const LoginAttemptInfo *info = _login_card_find(username);
    gint attempts = info ? info->attempts : -1;
    g_mutex_unlock(&g_attempts_mu);
    return attempts;
}

                 
void
pcv_rbac_attempts_ip_stats_for_test(guint *cards)
{
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    *cards = g_hash_table_size(g_ip_attempts);
    g_mutex_unlock(&g_attempts_mu);
}
