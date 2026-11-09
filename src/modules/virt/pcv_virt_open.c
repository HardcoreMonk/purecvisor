                                   
  
                           
                               
                                                                                    
                                                                              
                                               
                                                                         
                                                               
                                                                      
  
                       
                                                                 
                          
   
#include "pcv_virt_open.h"
#include "../../utils/pcv_log.h"
#include <gio/gio.h>
#include <libvirt/virterror.h>

#define VIRT_OPEN_LOG_DOM "virt_open"
#define VIRT_OPEN_KEEPALIVE_INTERVAL 5
#define VIRT_OPEN_KEEPALIVE_COUNT    3u

typedef virConnectPtr (*VirtOpenFn)(const gchar *uri, gboolean read_only);
typedef int (*VirtKeepaliveFn)(virConnectPtr conn, int interval, unsigned int count);
typedef void (*VirtCloseFn)(virConnectPtr conn);

typedef struct {
    gchar          *uri;
    gboolean        read_only;
    VirtOpenFn      open_fn;                                                        
    VirtKeepaliveFn keepalive_fn;
    VirtCloseFn     close_fn;
    GMutex          mu;
    GCond           cond;
    gboolean        done;                     
    gboolean        abandoned;                
    virConnectPtr   conn;                                             
    gchar          *err;                                        
} OpenState;

                         
                             
static virConnectPtr
_real_open(const gchar *uri, gboolean read_only)
{
    return read_only ? virConnectOpenReadOnly(uri) : virConnectOpen(uri);
}

                                                          
static void
_real_close(virConnectPtr conn)
{
    (void)virConnectClose(conn);
}

static VirtOpenFn      g_open_fn;                             
static VirtKeepaliveFn g_keepalive_fn;                        
static VirtCloseFn     g_close_fn;                            
static gint            g_abandoned;                

                                                  
static void
_open_state_clear(gpointer p)
{
    OpenState *s = p;
    g_free(s->uri);
    g_free(s->err);
    g_mutex_clear(&s->mu);
    g_cond_clear(&s->cond);
}

                                                           
                                                                       
static void
_open_keepalive(OpenState *s, virConnectPtr conn)
{
    int rc = s->keepalive_fn(conn, VIRT_OPEN_KEEPALIVE_INTERVAL, VIRT_OPEN_KEEPALIVE_COUNT);
    if (rc < 0) {
        gchar *msg = g_utf8_make_valid(virGetLastErrorMessage(), -1);
        PCV_LOG_WARN(VIRT_OPEN_LOG_DOM, "libvirt keepalive could not be enabled: %s", msg);
        g_free(msg);
    } else if (rc == 1) {
        PCV_LOG_INFO(VIRT_OPEN_LOG_DOM, "libvirt peer does not support keepalive");
    }
}

                                          
                                                            
                                             
static gpointer
_open_thread(gpointer data)
{
    OpenState *s = data;
    virConnectPtr conn = s->open_fn(s->uri, s->read_only);
    gchar *err = NULL;
    if (conn)
        _open_keepalive(s, conn);
    else
        err = g_utf8_make_valid(virGetLastErrorMessage(), -1);

    g_mutex_lock(&s->mu);
    s->done = TRUE;
    s->conn = conn;
    s->err = err;
    gboolean abandoned = s->abandoned;
    g_cond_signal(&s->cond);
    g_mutex_unlock(&s->mu);

    if (abandoned) {
        if (conn)
            s->close_fn(conn);
        g_atomic_int_add(&g_abandoned, -1);
    }
    g_atomic_rc_box_release_full(s, _open_state_clear);
    return NULL;
}

          
                                                                
virConnectPtr
pcv_virt_open_bounded(const gchar *uri, gboolean read_only, guint timeout_sec, GError **error)
{
    if (g_atomic_int_get(&g_abandoned) >= PCV_VIRT_OPEN_ABANDONED_MAX) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "libvirt open refused: %d abandoned opens still waiting", PCV_VIRT_OPEN_ABANDONED_MAX);
        return NULL;
    }

    OpenState *s = g_atomic_rc_box_new0(OpenState);
    VirtOpenFn open_fn = g_atomic_pointer_get(&g_open_fn);
    VirtKeepaliveFn keepalive_fn = g_atomic_pointer_get(&g_keepalive_fn);
    VirtCloseFn close_fn = g_atomic_pointer_get(&g_close_fn);
    s->uri = g_strdup(uri);
    s->read_only = read_only;
    s->open_fn = open_fn ? open_fn : _real_open;
    s->keepalive_fn = keepalive_fn ? keepalive_fn : virConnectSetKeepAlive;
    s->close_fn = close_fn ? close_fn : _real_close;
    g_mutex_init(&s->mu);
    g_cond_init(&s->cond);

    g_atomic_rc_box_acquire(s);              
    GError *spawn_err = NULL;
    GThread *th = g_thread_try_new("virt-open", _open_thread, s, &spawn_err);
    if (!th) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "libvirt open helper thread: %s", spawn_err->message);
        g_error_free(spawn_err);
        g_atomic_rc_box_release_full(s, _open_state_clear);
        g_atomic_rc_box_release_full(s, _open_state_clear);
        return NULL;
    }
    g_thread_unref(th);

    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_sec * G_TIME_SPAN_SECOND;
    virConnectPtr conn = NULL;
    g_mutex_lock(&s->mu);
    while (!s->done && g_cond_wait_until(&s->cond, &s->mu, deadline))
        ;
    if (s->done) {
        conn = s->conn;
        if (!conn)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", s->err ? s->err : "libvirt open failed");
    } else {
        s->abandoned = TRUE;
        g_atomic_int_inc(&g_abandoned);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "libvirt open timed out after %u s", timeout_sec);
    }
    g_mutex_unlock(&s->mu);
    g_atomic_rc_box_release_full(s, _open_state_clear);
    return conn;
}

typedef struct {
    GThreadFunc fn;
    gpointer    data;
    GMutex      mu;
    GCond       cond;
    gboolean    done;              
} CleanupState;

                                         
static void
_cleanup_state_clear(gpointer p)
{
    CleanupState *s = p;
    g_mutex_clear(&s->mu);
    g_cond_clear(&s->cond);
}

                                                               
static gpointer
_cleanup_thread(gpointer data)
{
    CleanupState *s = data;
    (void)s->fn(s->data);
    g_mutex_lock(&s->mu);
    s->done = TRUE;
    g_cond_signal(&s->cond);
    g_mutex_unlock(&s->mu);
    g_atomic_rc_box_release_full(s, _cleanup_state_clear);
    return NULL;
}

          
                                                                        
gboolean
pcv_virt_cleanup_bounded(const gchar *what, GThreadFunc fn, gpointer data, guint timeout_sec)
{
    CleanupState *s = g_atomic_rc_box_new0(CleanupState);
    s->fn = fn;
    s->data = data;
    g_mutex_init(&s->mu);
    g_cond_init(&s->cond);

    g_atomic_rc_box_acquire(s);              
    GError *spawn_err = NULL;
    GThread *th = g_thread_try_new("virt-cleanup", _cleanup_thread, s, &spawn_err);
    if (!th) {
        PCV_LOG_WARN(VIRT_OPEN_LOG_DOM, "cleanup bounded: %s gave up after 0 s (helper thread: %s)", what,
                     spawn_err->message);
        g_error_free(spawn_err);
        g_atomic_rc_box_release_full(s, _cleanup_state_clear);
        g_atomic_rc_box_release_full(s, _cleanup_state_clear);
        return FALSE;
    }
    g_thread_unref(th);

    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_sec * G_TIME_SPAN_SECOND;
    g_mutex_lock(&s->mu);
    while (!s->done && g_cond_wait_until(&s->cond, &s->mu, deadline))
        ;
    gboolean done = s->done;
    g_mutex_unlock(&s->mu);
    g_atomic_rc_box_release_full(s, _cleanup_state_clear);

    if (done)
        PCV_LOG_INFO(VIRT_OPEN_LOG_DOM, "cleanup bounded: %s done", what);
    else
        PCV_LOG_WARN(VIRT_OPEN_LOG_DOM, "cleanup bounded: %s gave up after %u s", what, timeout_sec);
    return done;
}

typedef struct {
    virConnectPtr conn;
    VirtCloseFn   close_fn;                                             
} DetachedClose;

static GThreadPool *g_close_pool;                                            
static gint         g_close_pending;               

                                            
static void
_detached_close_run(gpointer data, gpointer unused)
{
    DetachedClose *d = data;
    (void)unused;
    d->close_fn(d->conn);
    g_atomic_int_add(&g_close_pending, -1);
    g_free(d);
}

                                                            
static gpointer
_detached_pool_init(gpointer unused)
{
    (void)unused;
    GError *err = NULL;
    g_close_pool = g_thread_pool_new(_detached_close_run, NULL, 1, FALSE, &err);
    if (!g_close_pool) {
        PCV_LOG_WARN(VIRT_OPEN_LOG_DOM, "detached close pool unavailable: %s", err->message);
        g_error_free(err);
    }
    return NULL;
}

          
                                                           
void
pcv_virt_close_detached(virConnectPtr conn)
{
    static GOnce once = G_ONCE_INIT;
    VirtCloseFn close_fn = g_atomic_pointer_get(&g_close_fn);
    if (!close_fn)
        close_fn = _real_close;
    g_once(&once, _detached_pool_init, NULL);
    if (!g_close_pool) {
        close_fn(conn);
        return;
    }
    DetachedClose *d = g_new0(DetachedClose, 1);
    d->conn = conn;
    d->close_fn = close_fn;
    g_atomic_int_inc(&g_close_pending);
    GError *err = NULL;
    if (!g_thread_pool_push(g_close_pool, d, &err)) {
                                                                                
                                                                                          
                                                                                            
        PCV_LOG_WARN(VIRT_OPEN_LOG_DOM, "detached close queued without a worker thread: %s", err->message);
        g_error_free(err);
    }
}

          
                                     
guint
pcv_virt_close_detached_pending(void)
{
    return (guint)g_atomic_int_get(&g_close_pending);
}

          
                             
                                                            
void
pcv_virt_open_set_fn_for_test(virConnectPtr (*open_fn)(const gchar *uri, gboolean read_only),
                              int (*keepalive_fn)(virConnectPtr conn, int interval, unsigned int count),
                              void (*close_fn)(virConnectPtr conn))
{
    g_atomic_pointer_set(&g_open_fn, open_fn);
    g_atomic_pointer_set(&g_keepalive_fn, keepalive_fn);
    g_atomic_pointer_set(&g_close_fn, close_fn);
}

          
                              
                                                            
guint
pcv_virt_open_abandoned_for_test(void)
{
    return (guint)g_atomic_int_get(&g_abandoned);
}
