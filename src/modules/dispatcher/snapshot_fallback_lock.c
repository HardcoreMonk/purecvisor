                                                                             
                                                  
                                                                                  
  
                           
                                                                                               
                                                                               
  
                                                            
                                                                                
#include "snapshot_fallback_lock.h"

#define RENEW_TICK_US ((gint64)250 * G_TIME_SPAN_MILLISECOND)

struct PcvSnapshotFallbackLock {
    PcvVmLock *lock;
    GThread   *renew_thread;                    
    GMutex     mu;
    GCond      cond;
    gboolean   stop;                      
};

static gint64   (*g_now_fn)(void);                                                                   
static gboolean (*g_renew_fn)(const PcvVmLock *lock);                                             

                                       
static gint64
_now(void)
{
    return g_now_fn ? g_now_fn() : g_get_monotonic_time();
}

                                                                                          
static gpointer
_renew_worker(gpointer data)
{
    PcvSnapshotFallbackLock *h = data;
    gint64 last = _now();
    g_mutex_lock(&h->mu);
    while (!h->stop) {
        (void)g_cond_wait_until(&h->cond, &h->mu, g_get_monotonic_time() + RENEW_TICK_US);
        if (h->stop)
            break;
        if (_now() - last >= (gint64)PCV_SNAPSHOT_FALLBACK_RENEW_SEC * G_USEC_PER_SEC) {
            g_mutex_unlock(&h->mu);
            (void)(g_renew_fn ? g_renew_fn : pcv_vm_lock_renew)(h->lock);
            last = _now();
            g_mutex_lock(&h->mu);
        }
    }
    g_mutex_unlock(&h->mu);
    return NULL;
}

                                                                               
PcvSnapshotFallbackLock *
pcv_snapshot_fallback_lock_begin(const gchar *vm_id, gchar **err_msg)
{
    PcvVmLock *lock = pcv_vm_lock_acquire(vm_id, VM_OP_SNAPSHOT, err_msg);
    if (!lock)
        return NULL;
    PcvSnapshotFallbackLock *h = g_new0(PcvSnapshotFallbackLock, 1);
    h->lock = lock;
    g_mutex_init(&h->mu);
    g_cond_init(&h->cond);
    h->renew_thread = g_thread_try_new("snap-lock-renew", _renew_worker, h, NULL);                                 
    return h;
}

                                                                        
void
pcv_snapshot_fallback_lock_end(PcvSnapshotFallbackLock *h)
{
    if (!h)
        return;
    if (h->renew_thread) {
        g_mutex_lock(&h->mu);
        h->stop = TRUE;
        g_cond_signal(&h->cond);
        g_mutex_unlock(&h->mu);
        g_thread_join(h->renew_thread);
    }
    pcv_vm_lock_release(h->lock);
    g_mutex_clear(&h->mu);
    g_cond_clear(&h->cond);
    g_free(h);
}

                                                          
void
pcv_snapshot_fallback_lock_set_seam_for_test(gint64 (*now_fn)(void), gboolean (*renew_fn)(const PcvVmLock *lock))
{
    g_now_fn = now_fn;
    g_renew_fn = renew_fn;
}

                                                                                             
                                                                                           
                                                                                            
                        
                                                                                                      
                                                                                       
                                                                                             
struct PcvSnapshotLockKeeper {
    PcvVmLock *lock;
    guint      source_id;
};

static guint    g_keeper_interval_ms;                                               
static gboolean (*g_keeper_renew_fn)(const PcvVmLock *lock);                                          

                                                                                                
static gboolean
_keeper_tick(gpointer data)
{
    PcvSnapshotLockKeeper *k = data;
    (void)(g_keeper_renew_fn ? g_keeper_renew_fn : pcv_vm_lock_renew)(k->lock);
    return G_SOURCE_CONTINUE;
}

                                                                            
PcvSnapshotLockKeeper *
pcv_snapshot_lock_keeper_start(PcvVmLock *lock)
{
    if (!lock)
        return NULL;
    PcvSnapshotLockKeeper *k = g_new0(PcvSnapshotLockKeeper, 1);
    k->lock = lock;
    k->source_id = g_keeper_interval_ms
        ? g_timeout_add(g_keeper_interval_ms, _keeper_tick, k)
        : g_timeout_add_seconds(PCV_SNAPSHOT_FALLBACK_RENEW_SEC, _keeper_tick, k);
    return k;
}

                                                                                                      
void
pcv_snapshot_lock_keeper_stop_and_release(PcvSnapshotLockKeeper *k)
{
    if (!k)
        return;
    g_source_remove(k->source_id);
    pcv_vm_lock_release(k->lock);
    g_free(k);
}

                                                                  
void
pcv_snapshot_lock_keeper_set_seam_for_test(guint interval_ms, gboolean (*renew_fn)(const PcvVmLock *lock))
{
    g_keeper_interval_ms = interval_ms;
    g_keeper_renew_fn = renew_fn;
}
