                                                                             
                                 
                                                                                           
  
                           
              
                                                                                    
                                                                      
                                                                                            
                                                                                   
                                                                                                    
                                                                   
                                                                  
                                                                              
                                                                                
                                                       
                                                                               
                                                                                 
  
                                                                      
                                                                     
                                                              
                                                                                
#include "pcv_mainloop_guard.h"
#include "modules/daemons/prometheus_exporter.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pcv_config.h"
#include "pcv_log.h"

#define GUARD_LOG_DOM            "mainloop_guard"
#define GUARD_POLL_MS            1000                             
#define GUARD_RPC_NAME_MAX_LEN   96                                  
#define GUARD_RPC_INTERN_CAP     1024                               

                        
                                         
static gint64
_guard_real_now_us(void)
{
    return g_get_monotonic_time();
}

                                                        
                                            
static void
_guard_real_exit_now(gint code)
{
    _exit(code);
}

static const PcvMainloopGuardOps k_guard_real_ops = { _guard_real_now_us, _guard_real_exit_now };
static _Atomic(const PcvMainloopGuardOps *) g_ops = &k_guard_real_ops;

                                                                    
static int                   g_pipe_r = -1;
static volatile sig_atomic_t g_pipe_w = -1;
                                                                           
static pid_t                 g_owner_pid;

static GMainContext *g_ctx;                                                          
static GSourceFunc   g_on_signal;                                          
static GSource      *g_heartbeat;                                

static _Atomic gint64           g_last_beat_us;                                  
static _Atomic gint             g_armed;                             
static _Atomic gint64           g_deadline_us;                             
static _Atomic gint             g_picked_up;                                  
static _Atomic(const gchar *)   g_current_method;                                       
static _Atomic gint64           g_current_since_us;                   
static atomic_uint              g_interned_count;                               
static gint                     g_deadline_timeout_s;                                  
static PcvStallState            g_stall;                                      

              
static inline const PcvMainloopGuardOps *
_guard_ops(void)
{
    return atomic_load(&g_ops);
}

                                           
                                                                                   
                                                                                        
                                                                              
                                                                                
                                                                                          
                                                                                
                                                                         
                                                                 
                                                          
                                                                          
                                                             
                                                                 
                                                                  
                                    
static void
_guard_sig_handler(int signo)
{
    const int saved_errno = errno;
    if (g_owner_pid > 0 && getpid() != g_owner_pid) {
        signal(signo, SIG_DFL);
        raise(signo);
        errno = saved_errno;
        return;
    }
    const unsigned char b = (unsigned char)signo;
    ssize_t n = write(g_pipe_w, &b, 1);
    (void)n;
    errno = saved_errno;
}

                                                                    
                                                                  
                                                    
static gboolean
_guard_open_pipe(void)
{
    if (g_pipe_r >= 0) {
        unsigned char junk[64];
        while (read(g_pipe_r, junk, sizeof junk) > 0)
            ;
        return TRUE;
    }
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) return FALSE;
    g_pipe_r = fds[0];
    g_pipe_w = fds[1];
    return TRUE;
}

                                                           
                                                         
static gboolean
_guard_heartbeat(gpointer unused)
{
    (void)unused;
    atomic_store(&g_last_beat_us, _guard_ops()->now_us());
    return G_SOURCE_CONTINUE;
}

                                                                              
                                                                               
                                                           
static void
_guard_attach(GMainLoop *loop, GSourceFunc on_signal)
{
    GMainContext *ctx = loop ? g_main_loop_get_context(loop) : g_main_context_default();
    if (g_heartbeat) {
        g_source_destroy(g_heartbeat);
        g_source_unref(g_heartbeat);
        g_heartbeat = NULL;
    }
    if (g_ctx) g_main_context_unref(g_ctx);
    g_ctx = g_main_context_ref(ctx);
    g_on_signal = on_signal;
    atomic_store(&g_last_beat_us, _guard_ops()->now_us());
    g_heartbeat = g_timeout_source_new_seconds(1);
    g_source_set_callback(g_heartbeat, _guard_heartbeat, NULL, NULL);
    g_source_set_static_name(g_heartbeat, "pcv-mainloop-heartbeat");
    g_source_attach(g_heartbeat, g_ctx);
}

                                                                             
                                                                         
                                                                        
                                                             
static void
_guard_dispatch_on_signal(void)
{
    if (!g_on_signal || !g_ctx) return;
    GSource *src = g_idle_source_new();
    g_source_set_priority(src, G_PRIORITY_HIGH);
    g_source_set_callback(src, g_on_signal, NULL, NULL);
    g_source_set_static_name(src, "pcv-stop-signal");
    g_source_attach(src, g_ctx);
    g_source_unref(src);
}

                                                          
                                                                          
                                                                     
static void
_guard_on_signal_byte(gint signo)
{
    if (atomic_exchange(&g_armed, 1) != 0) {
        PCV_LOG_INFO(GUARD_LOG_DOM, "signal %d received again during shutdown; ignored", signo);
        return;
    }
    g_deadline_timeout_s = pcv_config_get_drain_timeout();
    atomic_store(&g_deadline_us,
                 _guard_ops()->now_us() + (gint64)g_deadline_timeout_s * G_USEC_PER_SEC);
    PCV_LOG_INFO(GUARD_LOG_DOM,
                 "stop signal %d received; graceful shutdown handed to main loop (pickup deadline %d s)",
                 signo, g_deadline_timeout_s);
    _guard_dispatch_on_signal();
}

                                                                                   
                                           
static void
_guard_drain_pipe(void)
{
    if (g_pipe_r < 0) return;
    for (;;) {
        unsigned char buf[16];
        ssize_t n = read(g_pipe_r, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) _guard_on_signal_byte(buf[i]);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return;                               
    }
}

                                                                                                 
                                                                                         
                                                                                      
                                                                                                                    
                                                                      
                                                                                    
                                               
                                                    
static gboolean
_guard_check_deadline(gint64 now)
{
    if (!atomic_load(&g_armed) || atomic_load(&g_picked_up)) return FALSE;
    if (!pcv_mainloop_deadline_expired(atomic_load(&g_deadline_us), now)) return FALSE;
    const gchar *method = atomic_load(&g_current_method);
    if (method) {
        const long long running_s = (now - atomic_load(&g_current_since_us)) / G_USEC_PER_SEC;
        PCV_LOG_ERROR(GUARD_LOG_DOM,
                      "stop signal not handled by main loop within %d s (current RPC: %s, running %lld s); "
                      "exiting unsuccessfully", g_deadline_timeout_s, method, running_s);
    } else {
        PCV_LOG_ERROR(GUARD_LOG_DOM,
                      "stop signal not handled by main loop within %d s (current RPC: none); "
                      "exiting unsuccessfully", g_deadline_timeout_s);
    }
    _guard_ops()->exit_now(EXIT_FAILURE);
    return TRUE;
}

                                                                                       
                                                          
static void
_guard_report_stall(gint64 now)
{
    const gint64 beat = atomic_load(&g_last_beat_us);
    const PcvStallVerdict v = pcv_mainloop_stall_classify(&g_stall, now, beat);
    if (v == PCV_STALL_NONE) return;
    if (v == PCV_STALL_RECOVERED) {
        PCV_LOG_INFO(GUARD_LOG_DOM, "main loop recovered after %lld s stall",
                     (long long)((beat - g_stall.stall_start_us) / G_USEC_PER_SEC));
        return;
    }
    const long long age_s = (now - beat) / G_USEC_PER_SEC;
    const gchar *method = atomic_load(&g_current_method);
    const long long running_s = method ? (now - atomic_load(&g_current_since_us)) / G_USEC_PER_SEC : 0;
                                                                                                
                                                   
    pcv_prom_inc("purecvisor_mainloop_stalls_total", "level", v == PCV_STALL_WARN ? "warn" : "crit");
    if (v == PCV_STALL_WARN) {
        if (method)
            PCV_LOG_WARN(GUARD_LOG_DOM, "main loop stalled for %lld s (current RPC: %s, running %lld s)",
                         age_s, method, running_s);
        else
            PCV_LOG_WARN(GUARD_LOG_DOM, "main loop stalled for %lld s (current RPC: none)", age_s);
    } else {
        if (method)
            PCV_LOG_ERROR(GUARD_LOG_DOM, "main loop unresponsive for %lld s (current RPC: %s, running %lld s)",
                          age_s, method, running_s);
        else
            PCV_LOG_ERROR(GUARD_LOG_DOM, "main loop unresponsive for %lld s (current RPC: none)", age_s);
    }
}

                                                                 
                                                         
static void
_guard_iteration(void)
{
    _guard_drain_pipe();
    const gint64 now = _guard_ops()->now_us();
    if (_guard_check_deadline(now)) return;
    if (atomic_load(&g_picked_up)) return;                                            
    _guard_report_stall(now);
}

                                                                                      
                                                                      
                                                   
static gpointer
_guard_thread_main(gpointer unused)
{
    (void)unused;
    for (;;) {
        struct pollfd pfd = { .fd = g_pipe_r, .events = POLLIN, .revents = 0 };
        (void)poll(&pfd, 1, GUARD_POLL_MS);                                  
        _guard_iteration();
    }
    return NULL;
}

                                                                            
                                                                  
                                                               
                                                             
static const gchar *
_guard_intern_bounded(const gchar *method)
{
    if (!method) return "(null)";
    GQuark q = g_quark_try_string(method);
    if (q) return g_quark_to_string(q);
    if (strnlen(method, GUARD_RPC_NAME_MAX_LEN + 1) > GUARD_RPC_NAME_MAX_LEN) return "(other)";
    if (atomic_load(&g_interned_count) >= GUARD_RPC_INTERN_CAP) return "(other)";
    atomic_fetch_add(&g_interned_count, 1);
    return g_intern_string(method);
}

                                                                               
                                               
PcvStallVerdict
pcv_mainloop_stall_classify(PcvStallState *st, gint64 now_us, gint64 last_beat_us)
{
    const gint64 age = now_us - last_beat_us;
    if (age >= PCV_MAINLOOP_STALL_WARN_US) {
        if (!st->warned && !st->critical) st->stall_start_us = last_beat_us;
        if (age >= PCV_MAINLOOP_STALL_CRIT_US && !st->critical) {
            st->critical = TRUE;
            st->warned = TRUE;
            return PCV_STALL_CRIT;
        }
        if (!st->warned) {
            st->warned = TRUE;
            return PCV_STALL_WARN;
        }
        return PCV_STALL_NONE;
    }
    if (st->warned || st->critical) {
        st->warned = FALSE;
        st->critical = FALSE;
        return PCV_STALL_RECOVERED;                                                
    }
    return PCV_STALL_NONE;
}

                                                     
                                 
gboolean
pcv_mainloop_deadline_expired(gint64 deadline_us, gint64 now_us)
{
    return deadline_us > 0 && now_us >= deadline_us;
}

                                                                                    
                                                                             
                                                                  
void
pcv_mainloop_guard_start(GMainLoop *loop, GSourceFunc on_signal)
{
    g_owner_pid = getpid();                                           
    _guard_attach(loop, on_signal);
    GError *err = NULL;
    GThread *th = NULL;
    if (!_guard_open_pipe()) {
        PCV_LOG_WARN(GUARD_LOG_DOM, "stop-signal pipe unavailable (%s); SIGINT/SIGTERM stay on GLib sources "
                     "without pickup deadline", g_strerror(errno));
    } else if (!(th = g_thread_try_new("mainloop-guard", _guard_thread_main, NULL, &err))) {
        PCV_LOG_WARN(GUARD_LOG_DOM, "mainloop-guard thread unavailable (%s); SIGINT/SIGTERM stay on GLib "
                     "sources without pickup deadline", err ? err->message : "unknown");
        g_clear_error(&err);
    }
    if (!th) {
        g_unix_signal_add(SIGINT, on_signal, NULL);
        g_unix_signal_add(SIGTERM, on_signal, NULL);
        return;
    }
    g_thread_unref(th);                                  

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = _guard_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    PCV_LOG_INFO(GUARD_LOG_DOM, "SIGINT/SIGTERM are received by mainloop-guard thread outside the main loop "
                 "(stall warn %d s, crit %d s)",
                 (gint)(PCV_MAINLOOP_STALL_WARN_US / G_USEC_PER_SEC),
                 (gint)(PCV_MAINLOOP_STALL_CRIT_US / G_USEC_PER_SEC));
}

                                                
                                    
void
pcv_mainloop_guard_signal_picked_up(void)
{
    atomic_store(&g_picked_up, 1);
}

                                                                              
                                                        
                               
void
pcv_mainloop_guard_rpc_enter(const gchar *method)
{
    atomic_store(&g_current_since_us, _guard_ops()->now_us());
    atomic_store(&g_current_method, _guard_intern_bounded(method));
}

                                         
                      
void
pcv_mainloop_guard_rpc_leave(void)
{
    atomic_store(&g_current_method, NULL);
}

                                                
                                        
                                                            
void
pcv_mainloop_guard_set_ops_for_test(const PcvMainloopGuardOps *ops)
{
    atomic_store(&g_ops, ops ? ops : &k_guard_real_ops);
    atomic_store(&g_armed, 0);
    atomic_store(&g_deadline_us, 0);
    atomic_store(&g_picked_up, 0);
    atomic_store(&g_current_method, NULL);
    atomic_store(&g_current_since_us, 0);
    atomic_store(&g_last_beat_us, _guard_ops()->now_us());
    g_deadline_timeout_s = 0;
    memset(&g_stall, 0, sizeof g_stall);
}

                                             
                                   
                                                            
void
pcv_mainloop_guard_tick_for_test(void)
{
    _guard_iteration();
}

                                                                
                                          
                                                            
void
pcv_mainloop_guard_start_for_test(GMainLoop *loop, GSourceFunc on_signal)
{
    g_owner_pid = getpid();                                                  
    _guard_attach(loop, on_signal);
    (void)_guard_open_pipe();
}

                                                              
                                         
                                                            
void
pcv_mainloop_guard_raise_for_test(gint signo)
{
    _guard_sig_handler(signo);
}
