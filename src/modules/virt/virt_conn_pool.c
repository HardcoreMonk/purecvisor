   
                         
                                          
  
                           
                                                   
                                                    
                                        
  
                                                     
                                                   
                                                    
                                                   
                                       
  
                   
                                                                           
                                                                   
  
                
                                             
                                    
                              
                                      
                                                   
  
                   
                                                       
                                                            
                                                       
  
                 
                                                  
                                                        
  
                
                                                          
   
                                    
  
                               
  
          
                                  
                       
                                                          
                                                    
  
              
                                             
                      
  
                   
                                   
                  
   

#include "virt_conn_pool.h"
#include "circuit_breaker.h"
#include "pcv_virt_open.h"
#include "../../api/drain.h"
#include "../../utils/pcv_log.h"
#include <glib.h>
#include <libvirt/libvirt.h>
#include <stdatomic.h>

#define POOL_URI "qemu:///system"
                  
#define POOL_EXHAUST_WAIT_SEC 10
                                                                                    
#define POOL_TERMINATING_BOUND_SEC 3
                                                                       
                                                                                        
#define POOL_DOOR_STALE_SEC 10

                                                       

typedef struct {
    GQueue  *idle;                                         
    GMutex   mutex;
    GCond    cond;                                     
    guint    max_size;                                     
    guint    total;                                 
    gboolean shutdown;                                   
    gdouble  wait_total_us;                        
    guint64  wait_count;                                
} ConnPool;

static ConnPool g_pool = { 0 };

                                                        

                                                                               
                                                                  
static virConnectPtr
pool_open_bounded(const gchar *uri, guint timeout_sec, GError **error)
{
    return pcv_virt_open_bounded(uri, FALSE, timeout_sec, error);
}

                
                                                   
static const PcvConnPoolOps G_real_pool_ops = {
    .open        = pool_open_bounded,
    .close       = virConnectClose,
    .is_alive    = virConnectIsAlive,
    .get_version = virConnectGetVersion,
};

                                          
static const PcvConnPoolOps *G_pool_ops_test = NULL;

static const PcvConnPoolOps *
pool_ops(void)
{
    const PcvConnPoolOps *t = g_atomic_pointer_get(&G_pool_ops_test);
    return t ? t : &G_real_pool_ops;
}

                                                                      
                                            
                                                            
void
virt_conn_pool_set_ops_for_test(const PcvConnPoolOps *ops)
{
    g_atomic_pointer_set(&G_pool_ops_test, ops);
}

                                                         
static gboolean (*G_pool_terminating_fn)(void) = NULL;

                                       
                                                                             
static gboolean
pool_terminating(void)
{
    gboolean (*fn)(void) = g_atomic_pointer_get(&G_pool_terminating_fn);
    return fn ? fn() : pcv_drain_is_terminating();
}

                                                                                  
                              
                                                            
void
virt_conn_pool_set_terminating_fn_for_test(gboolean (*fn)(void))
{
    g_atomic_pointer_set(&G_pool_terminating_fn, fn);
}

                                                                                  
                                                             
                                                            
gboolean
virt_conn_pool_lock_is_free_for_test(void)
{
    if (!g_mutex_trylock(&g_pool.mutex))
        return FALSE;
    g_mutex_unlock(&g_pool.mutex);
    return TRUE;
}

                                                       
                                                                                   
                                                                 
                                        
                                                                           
static _Atomic gint64 G_door_last_ok_us;                          
static atomic_int     G_door_watching;                                              
static atomic_int     G_door_warned;                                     
static _Atomic(gint64 (*)(void)) G_door_now_fn;                               

                                       
static gint64
door_now(void)
{
    gint64 (*fn)(void) = atomic_load(&G_door_now_fn);
    return fn ? fn() : g_get_monotonic_time();
}

                                    
static gboolean
door_stale(gint64 now)
{
    return now - atomic_load(&G_door_last_ok_us) >= POOL_DOOR_STALE_SEC * G_USEC_PER_SEC;
}

                                                                                                             
                                                                       
static gboolean
door_refuses(void)
{
    if (!atomic_load(&G_door_watching) || !g_main_context_is_owner(NULL))
        return FALSE;
    gint64 now = door_now();
    if (!door_stale(now))
        return FALSE;
    if (!atomic_exchange(&G_door_warned, 1))
        PCV_LOG_WARN("conn_pool", "libvirtd not answering for %lld s; refusing libvirt calls on the main loop",
                     (long long)((now - atomic_load(&G_door_last_ok_us)) / G_USEC_PER_SEC));
    return TRUE;
}

                                                                  
                                                              
void
virt_conn_pool_note_libvirtd_ok(void)
{
    atomic_store(&G_door_last_ok_us, door_now());
    atomic_store(&G_door_watching, 1);
    if (atomic_exchange(&G_door_warned, 0))
        PCV_LOG_INFO("conn_pool", "libvirtd answering again; main loop libvirt calls resume");
}

                                                           
                                                                  
                                                                 
void
virt_conn_pool_note_coverage_end(void)
{
    if (atomic_load(&G_door_watching) && !door_stale(door_now()))
        atomic_store(&G_door_watching, 0);
}

                                                                         
                                    
                                                            
void
virt_conn_pool_set_door_now_for_test(gint64 (*now)(void))
{
    atomic_store(&G_door_now_fn, now);
}

                                                         

                                                                
                                                                    
                                                                   
const gchar *
virt_conn_pool_uri(void)
{
    const gchar *uri_env = g_getenv("PCV_LIBVIRT_URI");
    return (uri_env && *uri_env) ? uri_env : POOL_URI;
}

                                                 
                                                  
                                                    
static virConnectPtr pool_new_conn(void) {
                                        
                                                  
                                                    
    if (cb_is_open()) {
        PCV_LOG_WARN("conn_pool",
                     "Circuit OPEN — libvirt connection blocked (state=%s, failures=%d)",
                     cb_get_state_str(), cb_get_failure_count());
        return NULL;
    }

                                                                                  
                                                                             
    GError *err = NULL;
    guint timeout_sec = pool_terminating() ? POOL_TERMINATING_BOUND_SEC : PCV_VIRT_OPEN_TIMEOUT_SEC;
    virConnectPtr conn = pool_ops()->open(virt_conn_pool_uri(), timeout_sec, &err);
    if (!conn) {
        PCV_LOG_WARN("conn_pool", "libvirt connection open failed: %s", err ? err->message : "unknown");
        g_clear_error(&err);
        cb_record_failure();
        return NULL;
    }

    cb_record_success();
    return conn;
}

                                                                                           
                                                                                
                                                               
                                                                     
                                                        

   
                                                  
  
                                                             
                                                            
                                                             
                    
  
                                                                       
                                                                             
                                                                        
                                                                                  
                                                         
                                                  
                                                                 
   
static gboolean
_pool_idle_reaper(gpointer user_data)
{
    (void)user_data;
    g_mutex_lock(&g_pool.mutex);

    if (g_pool.shutdown) {
        g_mutex_unlock(&g_pool.mutex);
        return G_SOURCE_REMOVE;
    }

    GPtrArray *dead = g_ptr_array_new();
    guint len = g_queue_get_length(g_pool.idle);
    for (guint i = 0; i < len; i++) {
        virConnectPtr conn = g_queue_pop_head(g_pool.idle);
        if (!conn) continue;

        if (pool_ops()->is_alive(conn) == 1) {
            g_queue_push_tail(g_pool.idle, conn);
        } else {
            PCV_LOG_INFO("conn_pool", "Reaping stale idle connection (slot %u/%u)", i, len);
            g_ptr_array_add(dead, conn);
        }
    }
    g_mutex_unlock(&g_pool.mutex);

    if (dead->len == 0) {
        g_ptr_array_free(dead, TRUE);
        return G_SOURCE_CONTINUE;
    }

                                                                       
                                             
    for (guint i = 0; i < dead->len; i++)
        pool_ops()->close(g_ptr_array_index(dead, i));

    g_mutex_lock(&g_pool.mutex);
    g_pool.total -= dead->len;
    g_cond_broadcast(&g_pool.cond);
    g_mutex_unlock(&g_pool.mutex);

    g_ptr_array_free(dead, TRUE);
    return G_SOURCE_CONTINUE;
}

                                                         

   
                                                      
  
                                                
                                         
  
                                
                                                
                                          
                         
                                                                           
                                                 
  
                                       
   
void virt_conn_pool_init(guint max_size) {
    cb_init();                           
    g_mutex_init(&g_pool.mutex);
    g_cond_init(&g_pool.cond);
    g_pool.idle     = g_queue_new();
    g_pool.max_size = (max_size > 0) ? max_size : 8;
    g_pool.total    = 0;
    g_pool.shutdown = FALSE;
    g_pool.wait_total_us = 0.0;                                                  
    g_pool.wait_count    = 0;
    atomic_store(&G_door_watching, 0);                                            
    atomic_store(&G_door_warned, 0);

                                  
    virConnectPtr conn = pool_new_conn();
    if (conn) {
        g_queue_push_tail(g_pool.idle, conn);
        g_pool.total = 1;
    }

                                
    g_timeout_add_seconds(30, _pool_idle_reaper, NULL);

    PCV_LOG_INFO("conn_pool", "Initialized (max=%u, pre-opened=%u)",
              g_pool.max_size, g_pool.total);
}

   
                                               
  
                                              
                                                  
                                                    
  
                                                         
                                                                        
                                                                 
                                                                     
                                         
                                                        
                                                  
                                                          
                                                       
  
                                                                               
   
                                                                                      
                                                                 
                                                                    
                                                            
                                       
static virConnectPtr _pool_acquire(gboolean wait_when_exhausted) {
    if (door_refuses())
        return NULL;                                                                               

    g_mutex_lock(&g_pool.mutex);

    while (!g_pool.shutdown) {

                                                         
                                                                        
                                                            
                                                                       
                                                     
        if (!g_queue_is_empty(g_pool.idle)) {
            virConnectPtr conn = g_queue_pop_head(g_pool.idle);
            g_mutex_unlock(&g_pool.mutex);

                                                                                             
                                                                                
                                                                          
                                                                 
            if (conn && pool_ops()->is_alive(conn) == 1) {
                unsigned long ver = 0;
                if (pool_ops()->get_version(conn, &ver) >= 0) {
                    cb_record_success();
                    return conn;
                }
            }
                                                            
            if (conn) {
                PCV_LOG_WARN("conn_pool",
                             "Stale connection detected during acquire — reconnecting...");
                pool_ops()->close(conn);
            }
            virConnectPtr fresh = pool_new_conn();
            if (fresh)
                return fresh;
                                                                                     
            g_mutex_lock(&g_pool.mutex);
            g_pool.total--;
            g_cond_signal(&g_pool.cond);
            g_mutex_unlock(&g_pool.mutex);
            return NULL;
        }

                                            
                                                        
                                                        
                                      
                                                         
                                                           
                                                            
        if (g_pool.total < g_pool.max_size) {
            g_pool.total++;
            g_mutex_unlock(&g_pool.mutex);

            virConnectPtr conn = pool_new_conn();
            if (!conn) {
                g_mutex_lock(&g_pool.mutex);
                g_pool.total--;
                g_cond_signal(&g_pool.cond);                                                
                g_mutex_unlock(&g_pool.mutex);
                return NULL;
            }
            return conn;
        }

                                               
                                                                          
                                                         
                                                    
                                                                
        if (!wait_when_exhausted) {
                                                                      
            g_mutex_unlock(&g_pool.mutex);
            return NULL;
        }
        gint64 wait_start = g_get_monotonic_time();
        gint64 wait_sec = pool_terminating() ? POOL_TERMINATING_BOUND_SEC : POOL_EXHAUST_WAIT_SEC;
        gint64 deadline = wait_start + wait_sec * G_TIME_SPAN_SECOND;
        if (!g_cond_wait_until(&g_pool.cond, &g_pool.mutex, deadline)) {
                                 
            gint64 waited = g_get_monotonic_time() - wait_start;
            g_pool.wait_total_us += (gdouble)waited;
            g_pool.wait_count++;
            PCV_LOG_WARN("conn_pool", "acquire() timed out waiting for a connection");
            g_mutex_unlock(&g_pool.mutex);
            return NULL;
        }
                                 
        {
            gint64 waited = g_get_monotonic_time() - wait_start;
            g_pool.wait_total_us += (gdouble)waited;
            g_pool.wait_count++;
        }
    }

                    
    g_mutex_unlock(&g_pool.mutex);
    return NULL;
}

                                                                       
                                                
virConnectPtr virt_conn_pool_acquire(void) {
    return _pool_acquire(TRUE);
}

   
                                                        
  
                                                        
                                                            
  
                                                                                       
                                                             
                                     
                                                                         
                                                               
                                                                              
  
                                                               
   
virConnectPtr virt_conn_pool_try_acquire(void) {
    return _pool_acquire(FALSE);
}

   
                                             
  
                                                    
  
                                                       
                                             
                                               
                                 
                                    
  
                                         
   
void virt_conn_pool_release(virConnectPtr conn) {
    if (!conn) return;

    g_mutex_lock(&g_pool.mutex);

    if (g_pool.shutdown) {
                          
        g_pool.total--;
        g_mutex_unlock(&g_pool.mutex);
        pool_ops()->close(conn);
        return;
    }

                                               
                                                                
    g_queue_push_tail(g_pool.idle, conn);
    g_cond_signal(&g_pool.cond);

    g_mutex_unlock(&g_pool.mutex);
}

                                                       
                                                                                        
                                                                                  
                                                          
static gpointer
_pool_close_idle(gpointer data)
{
    GPtrArray *conns = data;
    for (guint i = 0; i < conns->len; i++)
        pool_ops()->close(g_ptr_array_index(conns, i));
    g_ptr_array_free(conns, TRUE);
    return NULL;
}

   
                                                     
                                                 
                                                        
                                                                       
   
void virt_conn_pool_shutdown(void) {
    GPtrArray *conns = g_ptr_array_new();
    g_mutex_lock(&g_pool.mutex);
    g_pool.shutdown = TRUE;
                                                            
                                                             
    g_cond_broadcast(&g_pool.cond);
    if (g_pool.idle) {
        virConnectPtr conn;
        while ((conn = g_queue_pop_head(g_pool.idle)) != NULL) {
            g_ptr_array_add(conns, conn);
            g_pool.total--;
        }
    }
    g_mutex_unlock(&g_pool.mutex);

                                                                   
    pcv_virt_cleanup_bounded("conn pool", _pool_close_idle, conns, PCV_VIRT_CLEANUP_TIMEOUT_SEC);

    if (g_pool.idle)
        g_queue_free(g_pool.idle);
    g_pool.idle = NULL;
    g_mutex_clear(&g_pool.mutex);
    g_cond_clear(&g_pool.cond);

    cb_shutdown();                             
    PCV_LOG_INFO("conn_pool", "Shutdown complete.");
}

   
                                                            
                                                   
   
void virt_conn_pool_stats(guint *out_idle, guint *out_total, guint *out_max) {
    g_mutex_lock(&g_pool.mutex);
    if (out_idle)  *out_idle  = g_queue_get_length(g_pool.idle);
    if (out_total) *out_total = g_pool.total;
    if (out_max)   *out_max   = g_pool.max_size;
    g_mutex_unlock(&g_pool.mutex);
}

   
                                                          
                                                        
   
gdouble virt_conn_pool_wait_avg_seconds(void) {
    g_mutex_lock(&g_pool.mutex);
                                                           
    gdouble avg = (g_pool.wait_count > 0)
                  ? (g_pool.wait_total_us / (gdouble)g_pool.wait_count) / 1e6
                  : 0.0;
    g_mutex_unlock(&g_pool.mutex);
    return avg;
}

                                                             
                                              
                                                            
void
virt_conn_pool_reap_for_test(void)
{
    (void)_pool_idle_reaper(NULL);
}
