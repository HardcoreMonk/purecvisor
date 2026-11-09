   
                    
                                                           
  
                           
                                                   
                                                    
                                        
  
                                                                     
                                                                 
                        
  
          
                                                 
                                                           
                                                  
                                                    
                                               
  
        
                                                         
                                      
                                                                       
                                                       
         
                                                         
                                                       
                                                       
            
                                                          
                                                          
                                                  
  
            
                                                                 
        
                                                                                    
                                                                       
                                                                     
                                                                   
                                                                       
                                                                    
                                                                    
                  
        
                                                                 
  
                                       
                             
                                                   
                                                                              
  
              
                                                                     
                                                          
                                      
  
                         
                                                           
  
             
                      
                                    
                       
                                                    
                                                      
  
                      
                                              
                                          
                                               
                                               
  
                           
                                             
                                    
                                 
  
                               
                                              
                                                       
                                
                                                 
                                                                  
                             
                                                               
  
               
                                                
                                                              
                                      
  
         
                                                         
                                                       
                                                             
                                            
                                                            
   

#include "api/drain.h"
#include "pcv_spawn.h"
#include "pcv_spawn_env.h"
#include "pcv_log.h"

#include <gio/gio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

                                                       
#define SPAWN_LOG_DOM "pcv_spawn"

                                                                            
                                      
                                                                            
                                                       
                                           
                                        
   
static GSubprocessLauncher *g_launcher = NULL;
                                                                     
                                                               
static GMutex g_spawn_launcher_mu;

                                                             
static const gchar *
_spawn_basename(const gchar *path)
{
    const gchar *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path;
}

                                                                 
                                                                              
static gboolean
_spawn_profile_path_is_trusted(const gchar *path)
{
    if (!path || !*path)
        return FALSE;
    if (!strchr(path, '/'))
        return TRUE;                                                                            

    static const gchar *prefixes[] = { "/usr/sbin/", "/usr/bin/", "/sbin/", "/bin/" };
    for (guint i = 0; i < G_N_ELEMENTS(prefixes); i++) {
        if (!g_str_has_prefix(path, prefixes[i]))
            continue;
        const gchar *name = path + strlen(prefixes[i]);
        return *name != '\0' && strchr(name, '/') == NULL;
    }
    return FALSE;
}

static gboolean
_spawn_name_in(const gchar *name, const gchar * const *allowlist)
{
    for (guint i = 0; allowlist[i]; i++) {
        if (g_str_equal(name, allowlist[i]))
            return TRUE;
    }
    return FALSE;
}

                                                              
                                                               
static gboolean
_spawn_env_key_is_reserved(const gchar *key)
{
    static const gchar * const reserved[] = {
        "PATH", "HOME", "LANG",
        "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT",
        "GCONV_PATH", "BASH_ENV", "ENV", NULL
    };
    return _spawn_name_in(key, reserved);
}

static gboolean
_spawn_env_key_is_valid(const gchar *key)
{
    if (!key || !(g_ascii_isalpha(*key) || *key == '_'))
        return FALSE;
    for (const gchar *p = key + 1; *p; p++) {
        if (!(g_ascii_isalnum(*p) || *p == '_'))
            return FALSE;
    }
    return TRUE;
}

PcvChildCapabilityProfile
pcv_spawn_capability_profile_for_argv(const gchar * const *argv)
{
    if (!argv || !argv[0])
        return PCV_CHILD_CAP_BASE;

    if (!_spawn_profile_path_is_trusted(argv[0]))
        return PCV_CHILD_CAP_BASE;

    const gchar *base = _spawn_basename(argv[0]);
    static const gchar * const runtime_names[] = {
        "lxc-attach", "lxc-checkpoint", "lxc-create", "lxc-destroy",
        "lxc-info", "lxc-start", "lxc-stop",
        "virt-customize", "virt-filesystems", "virt-install", "virt-sysprep",
        "guestfish", "guestmount", "qemu-nbd", "qemu-system-x86_64",
        "qemu-system-aarch64", "kvm", NULL
    };
    if (_spawn_name_in(base, runtime_names))
        return PCV_CHILD_CAP_RUNTIME;

    if (g_str_equal(base, "dnsmasq"))
        return PCV_CHILD_CAP_DHCP;
    if (g_str_equal(base, "ip") && argv[1] && argv[2] && argv[3] && argv[4] &&
        g_str_equal(argv[1], "netns") && g_str_equal(argv[2], "exec") &&
        _spawn_profile_path_is_trusted(argv[4]) &&
        g_str_equal(_spawn_basename(argv[4]), "dnsmasq"))
        return PCV_CHILD_CAP_DHCP;

    if (g_str_equal(base, "kill") || g_str_equal(base, "pkill") ||
        g_str_equal(base, "fuser"))
        return PCV_CHILD_CAP_SIGNAL;
    if (g_str_equal(base, "zfs") || g_str_equal(base, "zpool") ||
        g_str_equal(base, "tar") || g_str_equal(base, "cp") ||
        g_str_equal(base, "qemu-img"))
        return PCV_CHILD_CAP_STORAGE;

    return PCV_CHILD_CAP_BASE;
}

                                                                                
#define SPAWN_SETUP_PROFILE_MASK 0xff
#define SPAWN_SETUP_CAPS         0x100

                                                                   
                                                               
                                                                  
static void
_spawn_group_child_setup(gpointer user_data)
{
    gint v = GPOINTER_TO_INT(user_data);
    if (setpgid(0, 0) != 0)
        _exit(126);
    if (v & SPAWN_SETUP_CAPS)
        pcv_privdrop_child_setup(GINT_TO_POINTER(v & SPAWN_SETUP_PROFILE_MASK));
}

                                                               
                                                                     
                                                               
static void
_launcher_set_capability_profile(GSubprocessLauncher *launcher,
                                 PcvChildCapabilityProfile profile,
                                 gboolean new_group)
{
    if (new_group) {
        gint v = (gint)profile |
                 (pcv_privdrop_child_profiles_enabled() ? SPAWN_SETUP_CAPS : 0);
        g_subprocess_launcher_set_child_setup(
            launcher, _spawn_group_child_setup, GINT_TO_POINTER(v), NULL);
    } else if (pcv_privdrop_child_profiles_enabled()) {
        g_subprocess_launcher_set_child_setup(
            launcher, pcv_privdrop_child_setup, GINT_TO_POINTER((gint)profile), NULL);
    } else {
        g_subprocess_launcher_set_child_setup(launcher, NULL, NULL, NULL);
    }
}

                                                                            
                                   
                                                                               

   
                                                              
  
                                                  
                                                       
  
                                                               
                                                             
  
                                                            
  
              
                                         
                                                  
                 
                                           
                   
                                             
             
                                               
  
                           
                                       
                                         
                                            
  
             
                                  
                                            
                                  
   
void
pcv_spawn_launcher_init(void)
{
    if (G_UNLIKELY(g_launcher != NULL)) {
        PCV_LOG_WARN(SPAWN_LOG_DOM, "pcv_spawn_launcher_init() called twice — ignored");
        return;
    }

      
                                                          
                                                                        
                                            
       
    g_launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);

                                                                         
                 
      
                                                     
                                            
                                
                                    
                                                                            
    pcv_spawn_env_apply(g_launcher);                                         

                                                                         
                        
      
                                     
                             
                                                                            
    g_subprocess_launcher_set_cwd(g_launcher, "/");

    PCV_LOG_INFO(SPAWN_LOG_DOM,
                 "GSubprocessLauncher initialized "
                 "(cwd=/, PATH=/usr/sbin:..., LANG=C.UTF-8)");
}

   
                                               
  
                                           
  
                                                                   
                                                                                  
  
                       
                                                            
                                                
   
void
pcv_spawn_launcher_shutdown(void)
{
                                                               
                                                        
                                                            
    g_mutex_lock(&g_spawn_launcher_mu);
    gboolean had_launcher = (g_launcher != NULL);
    g_clear_object(&g_launcher);
    g_mutex_unlock(&g_spawn_launcher_mu);
    if (had_launcher)
        PCV_LOG_INFO(SPAWN_LOG_DOM, "GSubprocessLauncher shutdown.");
}

                                                                            
                                    
                                                                            
                                                          
                                                               
  
                                   
                                                                       
                                               
  
                                                 
                                        
                                                                                     
                                                                        
                                                                         
   
static GSubprocess *
_spawn_newv_with_profile(const gchar * const *argv,
                         GSubprocessFlags flags,
                         PcvChildCapabilityProfile profile,
                         gboolean new_group,
                         GError **error)
{
    g_return_val_if_fail(argv && argv[0], NULL);
    g_return_val_if_fail(profile >= PCV_CHILD_CAP_BASE &&
                         profile < PCV_CHILD_CAP_N_PROFILES, NULL);

                                                               
                                             
    g_mutex_lock(&g_spawn_launcher_mu);
    if (g_launcher != NULL) {
          
                                                     
          
                         
                                                        
                                                             
                                                                    
                                       
                                    
           
                                                                  
                                                           
                                                            
                                            
        g_subprocess_launcher_set_flags(g_launcher, flags);
        _launcher_set_capability_profile(g_launcher, profile, new_group);
        GSubprocess *proc = g_subprocess_launcher_spawnv(g_launcher, argv, error);
        g_mutex_unlock(&g_spawn_launcher_mu);
        return proc;
    }
    g_mutex_unlock(&g_spawn_launcher_mu);

                                                           
                                                       
    PCV_LOG_WARN(SPAWN_LOG_DOM,
                 "launcher not initialized, using one-shot protected launcher");
    GSubprocessLauncher *fallback = g_subprocess_launcher_new(flags);
    pcv_spawn_env_apply(fallback);
    g_subprocess_launcher_set_cwd(fallback, "/");
    _launcher_set_capability_profile(fallback, profile, new_group);
    GSubprocess *proc = g_subprocess_launcher_spawnv(fallback, argv, error);
    g_object_unref(fallback);
    return proc;
}

GSubprocess *
pcv_spawn_newv_profile(const gchar * const *argv,
                       GSubprocessFlags flags,
                       PcvChildCapabilityProfile profile,
                       GError **error)
{
    return _spawn_newv_with_profile(argv, flags, profile, FALSE, error);
}

GSubprocess *
pcv_spawn_newv(const gchar * const *argv,
               GSubprocessFlags flags,
               GError **error)
{
    return _spawn_newv_with_profile(
        argv, flags, pcv_spawn_capability_profile_for_argv(argv), FALSE, error);
}

                                                                            
         
                                                                               

                                                            
                                             
                                                      
                                
                                                                           
                                                                     
                                                        
                                                                            
static gboolean
_spawn_exit_failed(const gchar *arg0, gchar *captured_stderr, GError **error)
{
    if (error && !*error) {
        const gchar *msg = (captured_stderr && *captured_stderr)
                           ? g_strstrip(captured_stderr)
                           : "process exited with non-zero status";
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", arg0, msg);
    }
    return FALSE;
}

static gboolean
_spawn_judge_exit(GSubprocess *proc, const gchar *arg0,
                  gchar *captured_stderr, GError **error)
{
    return g_subprocess_get_successful(proc) ||
           _spawn_exit_failed(arg0, captured_stderr, error);
}

                                                              
                                                       
typedef struct {
    gboolean      done;
    gboolean      timed_out;
    GAsyncResult *res;                             
} SpawnWaitState;

                                                     
                                                                      
static void
_spawn_communicate_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void)src;
    SpawnWaitState *st = user_data;
    st->res  = g_object_ref(res);
    st->done = TRUE;
}

                                                              
typedef struct {
    GSubprocess    *proc;
    GCancellable   *cancel;
    SpawnWaitState *st;
} SpawnTimeoutCtx;

                                                
                                                    
static gboolean
_spawn_timeout_cb(gpointer user_data)
{
    SpawnTimeoutCtx *tc = user_data;
    tc->st->timed_out = TRUE;
    g_cancellable_cancel(tc->cancel);                                           
    g_subprocess_force_exit(tc->proc);                                     
    return G_SOURCE_REMOVE;                       
}

                                                                            
                                                  
                                                        
                                                      
                                                                          
                                                                            
                                                           
                                                        
                                                                               
typedef enum { BOUND_RUNNING, BOUND_TIMEOUT, BOUND_CANCELLED, BOUND_ABORTED, BOUND_TOO_LARGE } BoundStop;
typedef struct BoundRun BoundRun;
                                                        
typedef struct {
    BoundRun *run; GInputStream *in; GByteArray *buf; gboolean eof; guint8 chunk[8192];
} BoundReader;
struct BoundRun {
    const PcvSpawnBound *b;
    GSubprocess  *proc;
    pid_t         pgid;                                                          
    GCancellable *io_cancel;                              
    GCancellable *wait_cancel;                                                      
    gsize         total;                                    
    guint         pending;                           
    gboolean      exited;                                     
    gboolean      reap_late;                             
    BoundStop     stop;                        
    GError       *read_err;                                 
    BoundReader   rd[2];
};

                                                        
                                                                              
                                                                                  
                                                            
                                                                        
                                                                       
                                                                      
static void
_bound_stop(BoundRun *r, BoundStop why)
{
    if (r->stop != BOUND_RUNNING)
        return;
    r->stop = why;
    if (r->pgid > 1)
        (void)kill(-r->pgid, SIGKILL);
    if (g_subprocess_get_identifier(r->proc) != NULL)
        g_subprocess_force_exit(r->proc);
    g_cancellable_cancel(r->io_cancel);
}

static void _bound_read_done(GObject *src, GAsyncResult *res, gpointer user_data);

static void
_bound_read_next(BoundReader *rd)
{
    rd->run->pending++;
    g_input_stream_read_async(rd->in, rd->chunk, sizeof rd->chunk, G_PRIORITY_DEFAULT,
                              rd->run->io_cancel, _bound_read_done, rd);
}

                                                                   
static void
_bound_read_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    BoundReader *rd = user_data;
    BoundRun *r = rd->run;
    GError *e = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &e);
    r->pending--;
    if (n > 0) {
        g_byte_array_append(rd->buf, rd->chunk, (guint)n);
        r->total += (gsize)n;
        if (r->b->max_output_bytes > 0 && r->total > r->b->max_output_bytes)
            _bound_stop(r, BOUND_TOO_LARGE);
        if (r->stop == BOUND_RUNNING) {
            _bound_read_next(rd);
            return;
        }
    } else if (n < 0 && r->stop == BOUND_RUNNING) {
        r->read_err = g_steal_pointer(&e);
        _bound_stop(r, BOUND_CANCELLED);
    }
    g_clear_error(&e);
    rd->eof = TRUE;
}

                                                                        
static void
_bound_wait_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    BoundRun *r = user_data;
    r->pending--;
    r->exited = g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
}

                                                    
static gboolean
_bound_reap_late_cb(gpointer user_data)
{
    ((BoundRun *)user_data)->reap_late = TRUE;
    return G_SOURCE_REMOVE;
}

static gboolean
_bound_timeout_cb(gpointer user_data)
{
    _bound_stop(user_data, BOUND_TIMEOUT);
    return G_SOURCE_REMOVE;
}

                                                                
static gboolean
_bound_abort_poll_cb(gpointer user_data)
{
    BoundRun *r = user_data;
    if (r->b->should_abort())
        _bound_stop(r, BOUND_ABORTED);
    return G_SOURCE_CONTINUE;
}

static gboolean
_bound_cancelled_cb(GCancellable *cancellable, gpointer user_data)
{
    (void)cancellable;
    _bound_stop(user_data, BOUND_CANCELLED);
    return G_SOURCE_REMOVE;
}

                                                                                            
                                                           
                                                                             
                                                                                  
                                                                      
                                                                                      
                                                                                
                                                                                   
                                      
static gboolean
_spawn_bounded_core(const gchar * const *argv, const PcvSpawnBound *b, gchar **stdout_out,
                    gchar **stderr_out, gint *exit_status, gboolean strict, GError **error)
{
    if ((b->cancellable && g_cancellable_is_cancelled(b->cancellable)) ||
        (b->should_abort && b->should_abort())) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s: cancelled", argv[0]);
        return FALSE;
    }
    GError *spawn_err = NULL;
    GSubprocess *proc = _spawn_newv_with_profile(
        argv, (stdout_out ? G_SUBPROCESS_FLAGS_STDOUT_PIPE : G_SUBPROCESS_FLAGS_STDOUT_SILENCE) |
              (stderr_out ? G_SUBPROCESS_FLAGS_STDERR_PIPE : G_SUBPROCESS_FLAGS_STDERR_SILENCE),
        b->profile, b->new_process_group, &spawn_err);
    if (!proc) {
        if (strict && spawn_err) {
            spawn_err->domain = G_IO_ERROR;
            spawn_err->code = G_IO_ERROR_FAILED;
        }
        g_propagate_error(error, spawn_err);
        return FALSE;
    }

    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    const gchar *ident = g_subprocess_get_identifier(proc);
    pid_t child = ident ? (pid_t)g_ascii_strtoll(ident, NULL, 10) : 0;                               
    BoundRun r = { .b = b, .proc = proc, .io_cancel = g_cancellable_new(),
                   .wait_cancel = g_cancellable_new(),
                   .pgid = b->new_process_group ? child : 0 };
    GInputStream *ins[2] = { stdout_out ? g_subprocess_get_stdout_pipe(proc) : NULL,
                             stderr_out ? g_subprocess_get_stderr_pipe(proc) : NULL };
    for (int i = 0; i < 2; i++) {
        r.rd[i] = (BoundReader){ .run = &r, .in = ins[i], .buf = g_byte_array_new(),
                                 .eof = (ins[i] == NULL) };
        if (ins[i])
            _bound_read_next(&r.rd[i]);
    }
    r.pending++;
    g_subprocess_wait_async(proc, r.wait_cancel, _bound_wait_done, &r);
    GSource *srcs[3] = { g_timeout_source_new_seconds(b->timeout_sec),
                         b->should_abort ? g_timeout_source_new(1000) : NULL,
                         b->cancellable ? g_cancellable_source_new(b->cancellable) : NULL };
    g_source_set_callback(srcs[0], _bound_timeout_cb, &r, NULL);
    if (srcs[1])
        g_source_set_callback(srcs[1], _bound_abort_poll_cb, &r, NULL);
    if (srcs[2])
        g_source_set_callback(srcs[2], G_SOURCE_FUNC(_bound_cancelled_cb), &r, NULL);
    for (int i = 0; i < 3; i++)
        if (srcs[i])
            g_source_attach(srcs[i], ctx);

    while (r.stop == BOUND_RUNNING && !(r.exited && r.rd[0].eof && r.rd[1].eof))
        g_main_context_iteration(ctx, TRUE);
    if (strict && r.stop != BOUND_RUNNING && !r.exited) {                             
        GSource *reap = g_timeout_source_new(PCV_SPAWN_BOUNDED_REAP_WAIT_SEC * 1000);
        g_source_set_callback(reap, _bound_reap_late_cb, &r, NULL);
        g_source_attach(reap, ctx);
        while (!r.exited && !r.reap_late)
            g_main_context_iteration(ctx, TRUE);
        g_source_destroy(reap);
        g_source_unref(reap);
        if (!r.exited) {
            PCV_LOG_WARN(SPAWN_LOG_DOM, "'%s' SIGKILL 뒤 %d초 안에 회수되지 않음 — D 상태? 회수는 "
                         "GLib child watch에 맡긴다", argv[0], PCV_SPAWN_BOUNDED_REAP_WAIT_SEC);
            if (b->unreaped_pid_out)                                                 
                *b->unreaped_pid_out = child;
        }
    }
    g_cancellable_cancel(r.wait_cancel);                                    
    while (r.pending > 0)                                              
        g_main_context_iteration(ctx, TRUE);
    for (int i = 0; i < 3; i++)
        if (srcs[i]) {
            g_source_destroy(srcs[i]);
            g_source_unref(srcs[i]);
        }
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    g_object_unref(r.io_cancel);
    g_object_unref(r.wait_cancel);

    if (r.read_err) {
        if (strict) {
            r.read_err->domain = G_IO_ERROR;
            r.read_err->code = G_IO_ERROR_FAILED;
        }
        g_propagate_error(error, g_steal_pointer(&r.read_err));
    } else if (r.stop == BOUND_TIMEOUT) {
        PCV_LOG_WARN(SPAWN_LOG_DOM, "'%s' %us 타임아웃 — 자식 SIGKILL", argv[0], b->timeout_sec);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "%s: timed out after %us", argv[0], b->timeout_sec);
    } else if (r.stop == BOUND_TOO_LARGE) {
        PCV_LOG_WARN(SPAWN_LOG_DOM, "'%s' 출력 %zu바이트 초과 — 자식 SIGKILL", argv[0],
                     b->max_output_bytes);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                    "%s: output exceeds %zu bytes", argv[0], b->max_output_bytes);
    } else if (r.stop != BOUND_RUNNING) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s: %s", argv[0],
                    r.stop == BOUND_ABORTED ? "aborted" : "cancelled");
    }
    gboolean ok = (r.stop == BOUND_RUNNING);
    if (ok && !g_subprocess_get_if_exited(proc) && strict) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: terminated by signal %d", argv[0],
                    g_subprocess_get_if_signaled(proc) ? g_subprocess_get_term_sig(proc) : 0);
        ok = FALSE;
    }
    gchar *outs[2];
    for (int i = 0; i < 2; i++) {
        const gchar *end = NULL;
        g_byte_array_append(r.rd[i].buf, (const guint8 *)"", 1);
        outs[i] = (gchar *)g_byte_array_free(r.rd[i].buf, FALSE);
        if (ok && ins[i] && !g_utf8_validate(outs[i], -1, &end)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid UTF-8 in child %s at offset %lu",
                        i == 0 ? "stdout" : "stderr", (unsigned long)(end - outs[i]));
            ok = FALSE;
        }
    }
    if (ok) {
        *exit_status = g_subprocess_get_if_exited(proc) ? g_subprocess_get_exit_status(proc) : -1;
        if (stdout_out) *stdout_out = g_steal_pointer(&outs[0]);
        if (stderr_out) *stderr_out = g_steal_pointer(&outs[1]);
    }
    g_free(outs[0]);
    g_free(outs[1]);
    g_object_unref(proc);
    return ok;
}

   
                                                                     
                                                          
                                                                                
                                                                        
   
gboolean
pcv_spawn_sync_bounded(const gchar * const *argv, const PcvSpawnBound *bound,
                       gchar **stdout_out, gchar **stderr_out,
                       gint *exit_status, GError **error)
{
    gint status = -1;
    if (exit_status) *exit_status = -1;
    if (stdout_out) *stdout_out = NULL;
    if (stderr_out) *stderr_out = NULL;
    if (bound && bound->unreaped_pid_out) *bound->unreaped_pid_out = 0;                          
    g_return_val_if_fail(argv && argv[0] && bound && bound->timeout_sec > 0, FALSE);
    g_return_val_if_fail(bound->profile >= PCV_CHILD_CAP_BASE &&
                         bound->profile < PCV_CHILD_CAP_N_PROFILES, FALSE);
    gboolean ok = _spawn_bounded_core(argv, bound, stdout_out, stderr_out, &status, TRUE, error);
    if (ok && exit_status) *exit_status = status;
    return ok;
}

   
                                                  
  
                                                    
                                                 
  
                                      
                                                                             
                                                                             
                                                                
                                                            
                                                                               
  
                                                       
                                                              
   
static gboolean
_spawn_sync_timeout_with_profile(const gchar * const *argv,
                                 gchar **stdout_out,
                                 gchar **stderr_out,
                                 guint timeout_sec,
                                 PcvChildCapabilityProfile profile,
                                 GError **error)
{
    g_return_val_if_fail(argv && argv[0], FALSE);

                                                                       
                                                                  
    if (timeout_sec > 0) {
        PcvSpawnBound b = { .profile = profile, .timeout_sec = timeout_sec };
        gint status = -1;
        if (!_spawn_bounded_core(argv, &b, stdout_out, stderr_out, &status, FALSE, error))
            return FALSE;
        return status == 0 ||
               _spawn_exit_failed(argv[0], stderr_out ? *stderr_out : NULL, error);
    }

                                                                
    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_NONE;
    if (stdout_out) flags |= G_SUBPROCESS_FLAGS_STDOUT_PIPE;
    else            flags |= G_SUBPROCESS_FLAGS_STDOUT_SILENCE;
    if (stderr_out) flags |= G_SUBPROCESS_FLAGS_STDERR_PIPE;
    else            flags |= G_SUBPROCESS_FLAGS_STDERR_SILENCE;

    GSubprocess *proc = pcv_spawn_newv_profile(argv, flags, profile, error);
    if (!proc)
        return FALSE;

                                                            
    gchar *captured_out = NULL, *captured_err = NULL;
    if (!g_subprocess_communicate_utf8(proc, NULL, NULL,
                                       stdout_out ? &captured_out : NULL,
                                       stderr_out ? &captured_err : NULL,
                                       error)) {
        g_object_unref(proc);
        return FALSE;
    }
    if (stdout_out) *stdout_out = captured_out;
    if (stderr_out) *stderr_out = captured_err;
    gboolean ok = _spawn_judge_exit(proc, argv[0],
                                    stderr_out ? *stderr_out : NULL, error);
    g_object_unref(proc);
    return ok;
}

                                                                 
static __thread PcvSpawnOverride t_spawn_override;
static __thread gpointer         t_spawn_override_data;

                                                                                    
void
pcv_spawn_set_thread_override(PcvSpawnOverride fn, gpointer data)
{
    t_spawn_override = fn;
    t_spawn_override_data = data;
}

gboolean
pcv_spawn_sync_timeout(const gchar * const *argv,
                       gchar              **stdout_out,
                       gchar              **stderr_out,
                       guint                timeout_sec,
                       GError             **error)
{
    if (timeout_sec == 0 && t_spawn_override)                                                         
        return t_spawn_override(t_spawn_override_data, argv, stdout_out, stderr_out, error);
    return _spawn_sync_timeout_with_profile(
        argv, stdout_out, stderr_out, timeout_sec,
        pcv_spawn_capability_profile_for_argv(argv), error);
}

   
                                                          
                               
                                                            
  
                                                         
                                                              
                                              
                                                          
  
                                      
                                                                             
                                                                             
                                                                            
                                               
                               
  
                                                              
                                                                   
                            
   
gboolean
pcv_spawn_sync(const gchar * const *argv,
               gchar              **stdout_out,
               gchar              **stderr_out,
               GError             **error)
{
    return pcv_spawn_sync_timeout(argv, stdout_out, stderr_out, 0, error);
}

gboolean
pcv_spawn_sync_profile(const gchar * const *argv,
                       PcvChildCapabilityProfile profile,
                       gchar **stdout_out,
                       gchar **stderr_out,
                       GError **error)
{
    return _spawn_sync_timeout_with_profile(
        argv, stdout_out, stderr_out, 0, profile, error);
}

   
                                                       
  
                                                              
                                                                
  
                                                                           
                                                                 
                                                                
                                    
   
gboolean
pcv_spawn_sync_status(const gchar * const *argv,
                      gchar              **stdout_out,
                      gchar              **stderr_out,
                      gint                *exit_status,
                      GError             **error)
{
    g_return_val_if_fail(argv && argv[0] && exit_status, FALSE);
    *exit_status = -1;

    GSubprocessFlags flags =
        (stdout_out ? G_SUBPROCESS_FLAGS_STDOUT_PIPE : G_SUBPROCESS_FLAGS_STDOUT_SILENCE) |
        (stderr_out ? G_SUBPROCESS_FLAGS_STDERR_PIPE : G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    GSubprocess *proc = pcv_spawn_newv(argv, flags, error);
    if (!proc)
        return FALSE;

    gchar *captured_out = NULL, *captured_err = NULL;
    gboolean ok = g_subprocess_communicate_utf8(proc, NULL, NULL,
                                                stdout_out ? &captured_out : NULL,
                                                stderr_out ? &captured_err : NULL,
                                                error);
    if (ok && !g_subprocess_get_if_exited(proc)) {
                                                             
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: terminated by signal %d", argv[0],
                    g_subprocess_get_if_signaled(proc) ? g_subprocess_get_term_sig(proc) : 0);
        ok = FALSE;
    }
    if (ok) {
        *exit_status = g_subprocess_get_exit_status(proc);
        if (stdout_out) *stdout_out = captured_out ? captured_out : g_strdup("");
        if (stderr_out) *stderr_out = captured_err ? captured_err : g_strdup("");
    } else {
        g_free(captured_out);
        g_free(captured_err);
    }
    g_object_unref(proc);
    return ok;
}

   
                                                     
  
                                               
                                                  
  
                                                         
                                             
                                                                   
                                    
  
                                                                    
                                                                     
                                                                         
                                                   
                                                      
                                                               
                                                      
                                                                
   
gboolean
pcv_spawn_sync_env(const gchar * const *argv,
                   const gchar * const *envp,
                   gchar              **stdout_out,
                   gchar              **stderr_out,
                   GError             **error)
{
    g_return_val_if_fail(argv && argv[0], FALSE);

                                                   
                                                             
    for (const gchar * const *e = envp; e && *e; e++) {
        const gchar *eq = strchr(*e, '=');
        if (!eq || eq == *e) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "child environment entry must be KEY=VALUE");
            return FALSE;
        }
        gchar *key = g_strndup(*e, (gsize)(eq - *e));
        gboolean valid = _spawn_env_key_is_valid(key);
        gboolean reserved = valid && _spawn_env_key_is_reserved(key);
        if (!valid || reserved) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "child environment key is not allowed: %s", key);
            g_free(key);
            return FALSE;
        }
        g_free(key);
    }

    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_NONE;
    if (stdout_out) flags |= G_SUBPROCESS_FLAGS_STDOUT_PIPE;
    else            flags |= G_SUBPROCESS_FLAGS_STDOUT_SILENCE;
    if (stderr_out) flags |= G_SUBPROCESS_FLAGS_STDERR_PIPE;
    else            flags |= G_SUBPROCESS_FLAGS_STDERR_SILENCE;

                                                
                                                 
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(flags);

                                                         
    pcv_spawn_env_apply(launcher);
    g_subprocess_launcher_set_cwd(launcher, "/");

                                                        
    for (const gchar * const *e = envp; e && *e; e++) {
        const gchar *eq = strchr(*e, '=');
        gchar *key = g_strndup(*e, (gsize)(eq - *e));
        g_subprocess_launcher_setenv(launcher, key, eq + 1, TRUE);
        g_free(key);
    }

    _launcher_set_capability_profile(
        launcher, pcv_spawn_capability_profile_for_argv(argv), FALSE);
    GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, argv, error);
    if (!proc) {
        g_object_unref(launcher);
        return FALSE;
    }

    gchar *captured_out = NULL, *captured_err = NULL;
    if (!g_subprocess_communicate_utf8(proc, NULL, NULL,
                                       stdout_out ? &captured_out : NULL,
                                       stderr_out ? &captured_err : NULL,
                                       error)) {
        g_object_unref(proc);
        g_object_unref(launcher);
        return FALSE;
    }
    if (stdout_out) *stdout_out = captured_out;
    if (stderr_out) *stderr_out = captured_err;

    gboolean ok = _spawn_judge_exit(proc, argv[0],
                                    stderr_out ? *stderr_out : NULL, error);
    g_object_unref(proc);
    g_object_unref(launcher);
    return ok;
}

                                                         
                                                    
                                                                 
                                                   
static gchar *
_read_stream_to_string(GInputStream *stream)
{
    if (!stream)
        return g_strdup("");

    GString *buf = g_string_new(NULL);
    guint8 chunk[4096];
    GError *local_error = NULL;

    for (;;) {
        gssize n = g_input_stream_read(stream, chunk, sizeof(chunk),
                                       NULL, &local_error);
        if (n > 0) {                                                
            g_string_append_len(buf, (const gchar *)chunk, n);
            continue;
        }
        if (n < 0) {                                                           
            PCV_LOG_WARN(SPAWN_LOG_DOM, "stream read failed: %s",
                         local_error ? local_error->message : "unknown");
            g_clear_error(&local_error);
        }
        break;
    }

    return g_string_free(buf, FALSE);
}

                                                                     
                                                  
                      
                                                              
static gchar *
_combine_pipe_stderr(const gchar *producer_err, const gchar *consumer_err)
{
    gboolean has_producer = producer_err && *producer_err;
    gboolean has_consumer = consumer_err && *consumer_err;

    if (has_producer && has_consumer)
        return g_strdup_printf("producer: %s\nconsumer: %s",
                               producer_err, consumer_err);
    if (has_producer)
        return g_strdup(producer_err);
    if (has_consumer)
        return g_strdup(consumer_err);
    return g_strdup("");
}

   
                       
                                              
  
                                                  
                                                         
                                                      
  
                                                                                     
                                                                           
                                                                    
  
           
                                                      
                                                         
                                                         
                                             
   
gboolean
pcv_spawn_pipe_sync(const gchar * const *producer_argv,
                    const gchar * const *consumer_argv,
                    gchar              **consumer_stdout_out,
                    gchar              **combined_stderr_out,
                    GError             **error)
{
    g_return_val_if_fail(producer_argv && producer_argv[0], FALSE);
    g_return_val_if_fail(consumer_argv && consumer_argv[0], FALSE);

    if (consumer_stdout_out)
        *consumer_stdout_out = NULL;
    if (combined_stderr_out)
        *combined_stderr_out = NULL;

    GSubprocess *producer = NULL;
    GSubprocess *consumer = NULL;
    gchar *producer_err = NULL;
    gchar *consumer_err = NULL;
    gchar *consumer_out = NULL;
    gboolean ok = FALSE;

    producer = pcv_spawn_newv(producer_argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        error);
    if (!producer)
        goto cleanup;

    GSubprocessFlags consumer_flags =
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
    consumer_flags |= consumer_stdout_out ? G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                          : G_SUBPROCESS_FLAGS_STDOUT_SILENCE;
    consumer = pcv_spawn_newv(consumer_argv, consumer_flags, error);
    if (!consumer) {
        g_subprocess_force_exit(producer);
        (void)g_subprocess_wait(producer, NULL, NULL);
        goto cleanup;
    }

    GInputStream *producer_stdout = g_subprocess_get_stdout_pipe(producer);
    GOutputStream *consumer_stdin = g_subprocess_get_stdin_pipe(consumer);

                                                               
                                                           
                                                              
    struct sigaction old_pipe_action;
    struct sigaction ignore_pipe_action = {0};
    ignore_pipe_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe_action.sa_mask);
    gboolean restore_sigpipe =
        sigaction(SIGPIPE, &ignore_pipe_action, &old_pipe_action) == 0;

                                                         
                                                                        
    gboolean spliced = g_output_stream_splice(
        consumer_stdin,
        producer_stdout,
        G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
        G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
        NULL,
        error);

    if (restore_sigpipe)
        sigaction(SIGPIPE, &old_pipe_action, NULL);

                                                                  
                                          
    if (!spliced) {
        g_subprocess_force_exit(producer);
        g_subprocess_force_exit(consumer);
        goto collect_output;
    }

collect_output:
    if (consumer_stdout_out)
        consumer_out = _read_stream_to_string(g_subprocess_get_stdout_pipe(consumer));
    producer_err = _read_stream_to_string(g_subprocess_get_stderr_pipe(producer));
    consumer_err = _read_stream_to_string(g_subprocess_get_stderr_pipe(consumer));

    GError *producer_wait_error = NULL;
    GError *consumer_wait_error = NULL;
    gboolean producer_ok = g_subprocess_wait_check(producer, NULL,
                                                   &producer_wait_error);
    gboolean consumer_ok = g_subprocess_wait_check(consumer, NULL,
                                                   &consumer_wait_error);

    ok = producer_ok && consumer_ok;
    if (!ok && error && !*error) {
        gchar *combined = _combine_pipe_stderr(producer_err, consumer_err);
        const gchar *detail = combined && *combined ? combined
                                                    : "pipeline exited with non-zero status";
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "%s -> %s failed: %s",
                    producer_argv[0], consumer_argv[0], detail);
        g_free(combined);
    }

    g_clear_error(&producer_wait_error);
    g_clear_error(&consumer_wait_error);

cleanup:
    if (combined_stderr_out)
        *combined_stderr_out = _combine_pipe_stderr(producer_err, consumer_err);
    if (consumer_stdout_out)
        *consumer_stdout_out = g_steal_pointer(&consumer_out);

    g_free(producer_err);
    g_free(consumer_err);
    g_free(consumer_out);
    g_clear_object(&producer);
    g_clear_object(&consumer);
    return ok;
}

   
                        
                                                     
  
                                                     
                                                       
  
                                                               
                                                                                      
                                                                            
  
           
                                                                       
                                                                  
                                                               
                                                   
  
                                                                      
                                                      
  
                                                   
                                                                
                                                 
                                                                     
                                                                
                                                             
   
#define SPAWN_STDIN_TIMEOUT_SEC 30

gboolean
pcv_spawn_sync_stdin(const gchar * const *argv,
                     const gchar          *input,
                     gssize                input_len,
                     gchar               **stdout_out,
                     gchar               **stderr_out,
                     GError              **error)
{
    g_return_val_if_fail(argv && argv[0], FALSE);

    if (stdout_out) *stdout_out = NULL;
    if (stderr_out) *stderr_out = NULL;

    GSubprocess *proc = pcv_spawn_newv(argv,
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
    if (!proc)
        return FALSE;

                                                                     
    gsize len = !input ? 0 : ((input_len < 0) ? strlen(input) : (gsize)input_len);
    GBytes *in = g_bytes_new(input ? input : "", len);

                                                                           
                                                     
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GCancellable *cancel = g_cancellable_new();
    SpawnWaitState st = { FALSE, FALSE, NULL };

    g_subprocess_communicate_async(proc, in, cancel, _spawn_communicate_done, &st);

    SpawnTimeoutCtx tc = { proc, cancel, &st };
    GSource *tsrc = g_timeout_source_new_seconds(SPAWN_STDIN_TIMEOUT_SEC);
    g_source_set_callback(tsrc, _spawn_timeout_cb, &tc, NULL);
    g_source_attach(tsrc, ctx);

    while (!st.done)
        g_main_context_iteration(ctx, TRUE);

    GBytes *out = NULL, *err = NULL;
    GError *fin_err = NULL;
    g_subprocess_communicate_finish(proc, st.res, &out, &err, &fin_err);

    g_clear_object(&st.res);
    g_source_destroy(tsrc);
    g_source_unref(tsrc);
    g_object_unref(cancel);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    g_bytes_unref(in);

    if (st.timed_out) {
        if (out) g_bytes_unref(out);
        if (err) g_bytes_unref(err);
        g_clear_error(&fin_err);
        g_object_unref(proc);
        PCV_LOG_WARN(SPAWN_LOG_DOM, "'%s' stdin %us 타임아웃 — 자식 SIGKILL",
                     argv[0], SPAWN_STDIN_TIMEOUT_SEC);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "%s: stdin communicate %us 초과", argv[0], SPAWN_STDIN_TIMEOUT_SEC);
        return FALSE;
    }

    if (fin_err) {
        if (out) g_bytes_unref(out);
        if (err) g_bytes_unref(err);
        g_object_unref(proc);
        g_propagate_error(error, fin_err);
        return FALSE;
    }

    if (out && stdout_out) {
        gsize n; const gchar *d = g_bytes_get_data(out, &n);
        *stdout_out = g_strndup(d, n);
    }
    if (err && stderr_out) {
        gsize n; const gchar *d = g_bytes_get_data(err, &n);
        *stderr_out = g_strndup(d, n);
    }
    if (out) g_bytes_unref(out);
    if (err) g_bytes_unref(err);

    gboolean ok = _spawn_judge_exit(proc, argv[0], stderr_out ? *stderr_out : NULL, error);
    g_object_unref(proc);
    return ok;
}

   
                                               
                                                    
                                                 
                           
                                                                                
  
                        
                                  
  
        
                                             
                                                            
                              
  
          
                                                 
               
                                                      
                                             
  
                         
                                   
                                          
                                        
  
          
                                   
                                         
   
void
pcv_spawn_fire(const gchar * const *argv)
{
    g_return_if_fail(argv && argv[0]);

    GError     *error = NULL;
    GSubprocess *proc = pcv_spawn_newv(
        argv,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &error);

    if (!proc) {
        PCV_LOG_WARN(SPAWN_LOG_DOM, "fire failed [%s]: %s",
                     argv[0], error->message);
        g_error_free(error);
        return;
    }

                                                     
    g_object_unref(proc);
}
