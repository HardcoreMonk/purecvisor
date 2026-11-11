                                                                             
                           
                                                                      
  
                           
                                                   
                                                    
                                        
  
                                                             
                                                                
                                                           
                                                                    
                                                    
                                                                     
                                                                         
                                                                                 
                                              
                                                                      
                                     
                                                                    
                                                                
                                                      
                                                   
                                                                                      
                                                                
  
                                                  
                                                     
                                    
  
                                                                         
                                                                     
                                                             
  
         
                                                        
                                                              
                                                                       
                                                                                          
                                                                                

#include "pcv_zfs_lock.h"
#include <string.h>
#include <gio/gio.h>                                            
#include "pcv_log.h"

                                                                        
                                                                                      
                                                          
                                                                                     
   
extern void pcv_prom_gauge_set_labels(const gchar *name, const gchar *labels, gdouble value);

#define ZFS_LOCK_DOM "zfs_lock"
                                                                     
                                                                         
#define ZFS_LOCK_MAX_POOLS     64
#define ZFS_LOCK_MAX_NAME      64
#define ZFS_LOCK_OVERFLOW_POOL "(other)"

                                                          
                                           
typedef struct {
    GMutex  mu;
    guint64 acquired;
} PoolLock;

static GHashTable *g_pool_mutexes = NULL;                                    
static GMutex      g_registry_mu;                                        
static gint        g_initialized = 0;
G_LOCK_DEFINE_STATIC(g_init_mu);
static gint        g_contentions = 0;                                     

                                                                       
                                                                               
static gchar *
_pool_head(const gchar *pool)
{
                                                                 
    if (!pool || !*pool) return g_strdup("");                                  
    return g_strndup(pool, strcspn(pool, "/@"));
}

                                                                            
                                                              
                                                     
        
                                                     
                                                         
                                                              
                                                   
void
pcv_zfs_pool_lock_init(void)
{
    if (g_atomic_int_get(&g_initialized)) return;
    G_LOCK(g_init_mu);
    if (g_atomic_int_get(&g_initialized)) {
        G_UNLOCK(g_init_mu);
        return;
    }
    g_mutex_init(&g_registry_mu);
    g_pool_mutexes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, NULL);
    g_atomic_int_set(&g_initialized, 1);
    G_UNLOCK(g_init_mu);
    PCV_LOG_INFO(ZFS_LOCK_DOM, "ZFS pool lock module initialized");
}

                                                  
void
pcv_zfs_pool_lock_shutdown(void)
{
    G_LOCK(g_init_mu);
    if (!g_atomic_int_get(&g_initialized)) { G_UNLOCK(g_init_mu); return; }
    g_mutex_lock(&g_registry_mu);
    if (g_pool_mutexes) {
                                                                                      
                                         
        GHashTableIter iter;
        gpointer k, v;
        g_hash_table_iter_init(&iter, g_pool_mutexes);
        while (g_hash_table_iter_next(&iter, &k, &v)) {
            PoolLock *entry = v;
            if (entry) { g_mutex_clear(&entry->mu); g_free(entry); }
        }
        g_hash_table_unref(g_pool_mutexes);
        g_pool_mutexes = NULL;
    }
    g_mutex_unlock(&g_registry_mu);
    g_mutex_clear(&g_registry_mu);
    g_atomic_int_set(&g_initialized, 0);
    G_UNLOCK(g_init_mu);
}

                                                               
                                                               
                                                  
                                        
static PoolLock *
_pool_entry_locked(gchar **head_inout)
{
    PoolLock *entry = g_hash_table_lookup(g_pool_mutexes, *head_inout);
    if (entry) return entry;
    if (strlen(*head_inout) > ZFS_LOCK_MAX_NAME ||
        g_hash_table_size(g_pool_mutexes) >= ZFS_LOCK_MAX_POOLS) {
        g_free(*head_inout);
        *head_inout = g_strdup(ZFS_LOCK_OVERFLOW_POOL);
        entry = g_hash_table_lookup(g_pool_mutexes, *head_inout);
        if (entry) return entry;
    }
    entry = g_new0(PoolLock, 1);
    g_mutex_init(&entry->mu);
    g_hash_table_insert(g_pool_mutexes, g_strdup(*head_inout), entry);                    
    return entry;
}

                                                          
                                            
static void
_pool_lock_acquired(PoolLock *entry, const gchar *head, gint64 waited_ms)
{
    gchar lbl[128];
    g_snprintf(lbl, sizeof(lbl), "pool=\"%s\"", head);                                             
    entry->acquired++;
    pcv_prom_gauge_set_labels("purecvisor_zfs_pool_lock_acquired_total", lbl, (gdouble)entry->acquired);
    pcv_prom_gauge_set_labels("purecvisor_zfs_pool_lock_wait_ms", lbl, (gdouble)waited_ms);
}

                                                                  
gboolean
pcv_zfs_pool_lock(const gchar *pool, const gchar *op,
                   gint timeout_ms, GError **error)
{
    if (!g_atomic_int_get(&g_initialized)) pcv_zfs_pool_lock_init();
    if (!pool || !*pool) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "pcv_zfs_pool_lock: pool is NULL/empty");
        return FALSE;
    }

    gchar *head = _pool_head(pool);                                   
    g_mutex_lock(&g_registry_mu);
    PoolLock *entry = _pool_entry_locked(&head);                                     
    g_mutex_unlock(&g_registry_mu);

                                                                  
    if (g_mutex_trylock(&entry->mu)) {
        _pool_lock_acquired(entry, head, 0);
        g_free(head);
        return TRUE;
    }

                                                          
                                                                                 
    const gint64 started_us = g_get_monotonic_time();
    gint64 deadline = started_us + (gint64)timeout_ms * 1000;                          
    gint64 poll_us = 20000;            
    while (g_get_monotonic_time() < deadline) {
        g_usleep((gulong)poll_us);                                         
        if (g_mutex_trylock(&entry->mu)) {                           
            const gint64 waited_ms = (g_get_monotonic_time() - started_us) / 1000;
            if (waited_ms >= 1000) {
                PCV_LOG_INFO(ZFS_LOCK_DOM,
                    "pool=%s op=%s acquired after %" G_GINT64_FORMAT "ms wait",
                    head, op ?: "?", waited_ms);
            }
            _pool_lock_acquired(entry, head, waited_ms);
            g_free(head);
            return TRUE;
        }
    }

                                                  
                                                             
    g_atomic_int_inc(&g_contentions);                                    
    pcv_prom_gauge_set_labels("purecvisor_zfs_pool_lock_contentions_total",
                                "", (gdouble)g_atomic_int_get(&g_contentions));
    PCV_LOG_WARN(ZFS_LOCK_DOM,
        "pool=%s op=%s timeout after %dms (another op holds the lock)",
        head, op ?: "?", timeout_ms);
                                                                               
                                                 
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                "Storage pool is busy, this zfs command did not run, try again: zfs pool lock timeout: pool=%s op=%s after %dms",
                head, op ?: "?", timeout_ms);
    g_free(head);
    return FALSE;
}

                                                
void
pcv_zfs_pool_unlock(const gchar *pool)
{
    if (!g_atomic_int_get(&g_initialized) || !pool || !*pool) return;
    gchar *head = _pool_head(pool);
                                                   
                                                                         
    g_mutex_lock(&g_registry_mu);
    PoolLock *entry = g_hash_table_lookup(g_pool_mutexes, head);
    if (!entry) entry = g_hash_table_lookup(g_pool_mutexes, ZFS_LOCK_OVERFLOW_POOL);
    g_mutex_unlock(&g_registry_mu);
    if (entry) g_mutex_unlock(&entry->mu);                         
    else PCV_LOG_WARN(ZFS_LOCK_DOM,                                      
        "pool=%s unlock called but no mutex registered", head);
    g_free(head);
}

                                                      
void
pcv_zfs_pool_get_stats(gint *registered, gint64 *contentions)
{
    if (registered) {                                  
        g_mutex_lock(&g_registry_mu);
        *registered = g_pool_mutexes ? (gint)g_hash_table_size(g_pool_mutexes) : 0;
        g_mutex_unlock(&g_registry_mu);
    }
    if (contentions) *contentions = (gint64)g_atomic_int_get(&g_contentions);
}
