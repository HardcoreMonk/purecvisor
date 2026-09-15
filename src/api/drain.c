   
                

  
                           


                                                                   


  
                                                 

                                    





   

#include "drain.h"

#include <glib.h>
#include <glib-unix.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

                                                       

typedef struct {
    volatile gint  inflight;                                    
    volatile gint  shutdown_flag;                                 
    GMutex         mutex;
    GCond          cond;                                            
    GThread       *drain_thread;                                   
    GMainLoop     *loop;                                              
    guint          timeout_sec;
    gint           work;
    gboolean       terminating;
    gboolean       sealed;
    guint          poll_source;
    gboolean       initialized;
} DrainState;

static DrainState g_drain = { 0 };

                                                           

                                              
void
pcv_drain_init(void)
{

    g_assert(g_drain.drain_thread == NULL);
    g_atomic_int_set(&g_drain.inflight,      0);
    g_atomic_int_set(&g_drain.shutdown_flag, 0);
    g_drain.drain_thread = NULL;
    g_drain.loop         = NULL;
    g_drain.timeout_sec  = 30;
    g_drain.terminating  = FALSE;
    g_drain.sealed       = FALSE;
    g_drain.initialized  = TRUE;
    g_message("[drain] Initialized.");
}

   
                                               
  
                                               
  
                                                    
  
                              
           
                                                                 
                                                                  
                                                          
                                                         
                                             
                                        
                                 
   
gboolean
pcv_drain_inc(void)
{
                                                
                                        
    g_mutex_lock(&g_drain.mutex);

    if (g_atomic_int_get(&g_drain.shutdown_flag)) {
        g_mutex_unlock(&g_drain.mutex);
        return FALSE;
    }

    g_atomic_int_inc(&g_drain.inflight);
    g_mutex_unlock(&g_drain.mutex);
    return TRUE;
}

   
                                              
  
                                               
  
                                       
                                           
  
                      
                                          
                                                                    
                                                      
   
void
pcv_drain_dec(void)
{
                                         
    if (g_atomic_int_dec_and_test(&g_drain.inflight)) {
        g_mutex_lock(&g_drain.mutex);
        g_cond_broadcast(&g_drain.cond);
        g_mutex_unlock(&g_drain.mutex);
    }
}

                                                    
                                                
                                                      
gboolean
pcv_drain_is_shutdown(void)
{
    return g_atomic_int_get(&g_drain.shutdown_flag) != 0;
}

                                                      
                                              
                                                  
gint
pcv_drain_get_inflight(void)
{
    return g_atomic_int_get(&g_drain.inflight);
}






void
pcv_drain_work_acquire(void)
{
    g_mutex_lock(&g_drain.mutex);
    if (g_drain.sealed) {
        g_mutex_unlock(&g_drain.mutex);
        g_printerr("[drain] Work submitted after shutdown seal; refusing unsafe teardown.\n");
        _exit(EXIT_FAILURE);
    }
    g_drain.work++;
    g_mutex_unlock(&g_drain.mutex);
}

void
pcv_drain_work_release(void)
{
    g_mutex_lock(&g_drain.mutex);
    g_assert(g_drain.work > 0);
    g_drain.work--;
    g_cond_broadcast(&g_drain.cond);
    g_mutex_unlock(&g_drain.mutex);
}

static void
_task_lifetime_end(gpointer unused)
{
    (void)unused;
    pcv_drain_work_release();
}

void
pcv_drain_track_task(GTask *task)
{
    GQuark key = g_quark_from_static_string("pcv-drain-task-lifetime");

    if (g_object_get_qdata(G_OBJECT(task), key))
        return;
    pcv_drain_work_acquire();
    g_object_set_qdata_full(G_OBJECT(task), key, GINT_TO_POINTER(1), _task_lifetime_end);
}

GTask *
pcv_drain_task_new(gpointer source, GCancellable *cancel,
                   GAsyncReadyCallback callback, gpointer data)
{
    GTask *task = g_task_new(source, cancel, callback, data);
    pcv_drain_track_task(task);
    return task;
}

gint
pcv_drain_get_work(void)
{
    g_mutex_lock(&g_drain.mutex);
    gint work = g_drain.work;
    g_mutex_unlock(&g_drain.mutex);
    return work;
}

gboolean
pcv_drain_is_terminating(void)
{
    g_mutex_lock(&g_drain.mutex);
    gboolean terminating = g_drain.terminating;
    g_mutex_unlock(&g_drain.mutex);
    return terminating;
}



gboolean
pcv_drain_admit_rest(void)
{
    g_mutex_lock(&g_drain.mutex);
    gboolean admitted = !g_drain.terminating;
    if (admitted)
        g_drain.work++;
    g_mutex_unlock(&g_drain.mutex);
    return admitted;
}

typedef struct {
    GSourceFunc callback;
    gpointer data;
    GDestroyNotify destroy;
} DrainSource;

static gboolean
_drain_source_dispatch(gpointer data)
{
    DrainSource *source = data;
    return source->callback(source->data);
}

static void
_drain_source_free(gpointer data)
{
    DrainSource *source = data;
    if (source->destroy)
        source->destroy(source->data);
    g_free(source);
    pcv_drain_work_release();
}

void
pcv_drain_idle(GMainContext *context, GSourceFunc callback,
               gpointer data, GDestroyNotify destroy)
{
    DrainSource *entry = g_new0(DrainSource, 1);
    *entry = (DrainSource){callback, data, destroy};
    pcv_drain_work_acquire();
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, _drain_source_dispatch, entry, _drain_source_free);
    g_source_attach(source, context);
    g_source_unref(source);
}

void
pcv_drain_invoke(GMainContext *context, GSourceFunc callback, gpointer data)
{
    DrainSource *entry = g_new0(DrainSource, 1);
    *entry = (DrainSource){callback, data, NULL};
    pcv_drain_work_acquire();
    g_main_context_invoke_full(context, G_PRIORITY_DEFAULT, _drain_source_dispatch,
                               entry, _drain_source_free);
}


guint
pcv_drain_timeout(guint milliseconds, GSourceFunc callback,
                  gpointer data, GDestroyNotify destroy)
{
    DrainSource *entry = g_new0(DrainSource, 1);
    *entry = (DrainSource){callback, data, destroy};
    pcv_drain_work_acquire();
    return g_timeout_add_full(G_PRIORITY_DEFAULT, milliseconds,
                              _drain_source_dispatch, entry, _drain_source_free);
}

                                                     
  
                                                     
                                     
                                                
                                                              

                                                         
void
pcv_drain_notify_stopping(void)
{
    const gchar *notify_socket = g_getenv("NOTIFY_SOCKET");
    if (!notify_socket || notify_socket[0] == '\0') {
        g_debug("[drain] NOTIFY_SOCKET not set — skipping sd_notify.");
        return;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        g_warning("[drain] sd_notify: socket() failed: %s", g_strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

                                                      
                                                        
                                                             
    if (notify_socket[0] == '@') {
                                            
        addr.sun_path[0] = '\0';
        strncpy(addr.sun_path + 1, notify_socket + 1,
                sizeof(addr.sun_path) - 2);
    } else {
        strncpy(addr.sun_path, notify_socket,
                sizeof(addr.sun_path) - 1);
    }

                                                             
                                                     
    socklen_t addrlen = offsetof(struct sockaddr_un, sun_path)
                        + (notify_socket[0] == '@'
                               ? strlen(notify_socket)                    
                               : strlen(notify_socket) + 1);

    const char *msg = "STOPPING=1\n";
    if (sendto(fd, msg, strlen(msg), MSG_NOSIGNAL,
               (struct sockaddr *)&addr, addrlen) < 0) {
        g_warning("[drain] sd_notify STOPPING=1 failed: %s", g_strerror(errno));
    } else {
        g_message("[drain] sd_notify STOPPING=1 sent.");
    }

    close(fd);
}

                                                        

                                                         
void
pcv_drain_notify_ready(void)
{
    const gchar *notify_socket = g_getenv("NOTIFY_SOCKET");
    if (!notify_socket || notify_socket[0] == '\0') {
        g_debug("[drain] NOTIFY_SOCKET not set — skipping sd_notify READY.");
        return;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        g_warning("[drain] sd_notify READY: socket() failed: %s", g_strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (notify_socket[0] == '@') {
        addr.sun_path[0] = '\0';
        strncpy(addr.sun_path + 1, notify_socket + 1,
                sizeof(addr.sun_path) - 2);
    } else {
        strncpy(addr.sun_path, notify_socket,
                sizeof(addr.sun_path) - 1);
    }

    socklen_t addrlen = offsetof(struct sockaddr_un, sun_path)
                        + (notify_socket[0] == '@'
                               ? strlen(notify_socket)
                               : strlen(notify_socket) + 1);

    const char *msg = "READY=1\n";
    if (sendto(fd, msg, strlen(msg), MSG_NOSIGNAL,
               (struct sockaddr *)&addr, addrlen) < 0) {
        g_warning("[drain] sd_notify READY=1 failed: %s", g_strerror(errno));
    } else {
        g_message("[drain] sd_notify READY=1 sent.");
    }

    close(fd);
}

                                                         

                                          
void
pcv_drain_notify_watchdog(void)
{
    const gchar *notify_socket = g_getenv("NOTIFY_SOCKET");
    if (!notify_socket || notify_socket[0] == '\0')
        return;

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (notify_socket[0] == '@') {
        addr.sun_path[0] = '\0';
        strncpy(addr.sun_path + 1, notify_socket + 1,
                sizeof(addr.sun_path) - 2);
    } else {
        strncpy(addr.sun_path, notify_socket,
                sizeof(addr.sun_path) - 1);
    }

    socklen_t addrlen = offsetof(struct sockaddr_un, sun_path)
                        + (notify_socket[0] == '@'
                               ? strlen(notify_socket)
                               : strlen(notify_socket) + 1);

    const char *msg = "WATCHDOG=1\n";
    (void)sendto(fd, msg, strlen(msg), MSG_NOSIGNAL,
                 (struct sockaddr *)&addr, addrlen);
    close(fd);
}

                                                       
guint64
pcv_drain_get_watchdog_usec(void)
{
    const gchar *val = g_getenv("WATCHDOG_USEC");
    if (!val || val[0] == '\0')
        return 0;
    guint64 usec = (guint64)g_ascii_strtoull(val, NULL, 10);
    return usec;
}




static gboolean
_drain_poll(gpointer unused)
{
    (void)unused;
    g_mutex_lock(&g_drain.mutex);
    if (g_atomic_int_get(&g_drain.inflight) != 0 || g_drain.work != 0) {
        g_mutex_unlock(&g_drain.mutex);
        return G_SOURCE_CONTINUE;
    }
    g_drain.sealed = TRUE;
    g_drain.poll_source = 0;
    g_cond_broadcast(&g_drain.cond);
    g_main_loop_quit(g_drain.loop);
    g_mutex_unlock(&g_drain.mutex);
    g_message("[drain] All requests drained; tasks and completion sources finalized. Quitting main loop.");
    return G_SOURCE_REMOVE;
}




static gpointer
_drain_thread_func(gpointer unused)
{
    (void)unused;
    g_mutex_lock(&g_drain.mutex);
    gint64 deadline = g_get_monotonic_time()
                      + (gint64)g_drain.timeout_sec * G_TIME_SPAN_SECOND;
    while (!g_drain.sealed) {
        if (!g_cond_wait_until(&g_drain.cond, &g_drain.mutex, deadline) &&
            !g_drain.sealed) {
            gint requests = g_atomic_int_get(&g_drain.inflight);
            gint work = g_drain.work;
            g_mutex_unlock(&g_drain.mutex);
            g_printerr("[drain] Timeout after %us: requests=%d work=%d; exiting unsuccessfully without unsafe cleanup.\n",
                       g_drain.timeout_sec, requests, work);
            _exit(EXIT_FAILURE);
        }
    }
    g_mutex_unlock(&g_drain.mutex);
    return NULL;
}


void
pcv_drain_begin(GMainLoop *loop, guint timeout_sec)
{
    g_mutex_lock(&g_drain.mutex);

    g_atomic_int_set(&g_drain.shutdown_flag, 1);
    if (!loop || g_drain.terminating) {
        g_mutex_unlock(&g_drain.mutex);
        return;
    }
    g_drain.terminating = TRUE;
    g_drain.loop = loop;
    g_drain.timeout_sec = timeout_sec;
    GSource *source = g_timeout_source_new(20);
    g_source_set_priority(source, G_PRIORITY_LOW);
    g_source_set_callback(source, _drain_poll, NULL, NULL);
    g_drain.poll_source = g_source_attach(source, g_main_loop_get_context(loop));
    g_source_unref(source);
    g_drain.drain_thread = g_thread_new("drain-deadline", _drain_thread_func, NULL);
    g_mutex_unlock(&g_drain.mutex);
    pcv_drain_notify_stopping();
    g_message("[drain] Waiting for %d in-flight request(s) and %d asynchronous work item(s) (timeout: %us)",
              pcv_drain_get_inflight(), pcv_drain_get_work(), timeout_sec);
}



void
pcv_drain_cancel(void)
{
    g_mutex_lock(&g_drain.mutex);
    if (!g_drain.terminating) {
        g_atomic_int_set(&g_drain.shutdown_flag, 0);
        g_drain.sealed = FALSE;
    }
    g_mutex_unlock(&g_drain.mutex);
}



void
pcv_drain_shutdown(void)
{
    if (g_drain.drain_thread) {
        g_thread_join(g_drain.drain_thread);
        g_drain.drain_thread = NULL;
    }
    g_assert_cmpint(pcv_drain_get_work(), ==, 0);
    g_drain.initialized = FALSE;
    g_message("[drain] Resources released.");
}
