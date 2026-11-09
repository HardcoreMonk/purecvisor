                                                                             
                                                    
                                                                                             
  
                           
                                                                                         
                                                                                      
                                                                                               
                                                                                           
                                                           
                                                                             
                                                                                      
                                                                             
                                                                                      
                                                                                          
                                                                   
                                                     
  
                                                                    
                                                                    
                                                    
                                                                                
#include "handler_container_health.h"

#include <string.h>

#include "rpc_utils.h"
#include "api/drain.h"
#include "api/uds_server.h"
#include "modules/core/vm_state.h"
#include "modules/dispatcher/handler_container_mode.h"
#include "modules/lxc/lxc_driver.h"
#include "modules/lxc/lxc_ns_exec.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_validate.h"
#include "utils/pcv_worker_pool.h"

#define HEALTH_LOG_DOM               "container_health"
#define MAX_HEALTH_PROBES            32                                       
#define HEALTH_TIMEOUT_MIN_SEC       1                                    
#define HEALTH_TIMEOUT_MAX_SEC       30                                   
#define HEALTH_LXC_INFO_TIMEOUT_SEC  5                                    
#define HEALTH_STOP_GRACE_ARG        "20"                                                        
#define HEALTH_STOP_TIMEOUT_SEC      30                                 
#define HEALTH_TARGET_MAX_BYTES      255                                                   

                                           
typedef struct {
    gchar    name[PCV_MAX_VM_NAME + 1];                         
    gchar    probe_type[8];                                   
    gchar    target[256];                                                                  
    gint     timeout_sec;                                          
    gint     interval_sec;                             
    gint     failure_threshold;                   
    gboolean auto_restart;                                 
                
    gint     consecutive_failures;                  
    gboolean healthy;                            
    gint     restart_count;                       
    gint64   last_check_time;                                       
    guint64  gen;                                                
    gboolean inflight;                                        
} ContainerHealthProbe;

                                           
typedef enum {
    HEALTH_RESTART_NONE = 0,                                             
    HEALTH_RESTART_BUSY,                                                       
    HEALTH_RESTART_DENIED,                                               
    HEALTH_RESTART_REFUSED,                                         
    HEALTH_RESTART_DONE,                                          
} HealthRestart;

                                           
typedef enum {
    HEALTH_DISPATCH_POOL,                    
    HEALTH_DISPATCH_INLINE,                                         
    HEALTH_DISPATCH_MARK_ONLY,                                                            
} HealthDispatch;

                                                                           
static ContainerHealthProbe g_health_probes[MAX_HEALTH_PROBES];                             
static gint     g_n_health_probes = 0;                    
static GMutex   g_health_mu;                             
static guint    g_health_timer_id = 0;                                 
static guint64  g_health_gen_seq = 0;                                                 
static guint    g_health_running = 0;                                              

                                                         
static const PcvHealthOps *G_health_ops_test;

                                                                                
gint
pcv_container_health_clamp_timeout(gint requested)
{
    return CLAMP(requested, HEALTH_TIMEOUT_MIN_SEC, HEALTH_TIMEOUT_MAX_SEC);
}

                                                                                      

                                                                                             
                                                             
static gboolean
_health_init_pid(const gchar *name, gchar pid[16], GError **error)
{
    const gchar *argv[] = {"lxc-info", "-P", PCV_LXC_PATH, "-n", name, "-p", "-H", NULL};
    const PcvSpawnBound bound = {
        .profile           = PCV_CHILD_CAP_RUNTIME,
        .timeout_sec       = HEALTH_LXC_INFO_TIMEOUT_SEC,
        .max_output_bytes  = PCV_LXC_NS_OUTPUT_MAX,
        .cancellable       = NULL,
        .should_abort      = pcv_drain_is_terminating,                                   
        .new_process_group = TRUE,
    };
    g_autofree gchar *out = NULL;
    gint status = -1;
    if (!pcv_spawn_sync_bounded(argv, &bound, &out, NULL, &status, error))
        return FALSE;                                           
    const gchar *p = g_strstrip(out);
    gsize len = strlen(p);
    if (status != 0 || len == 0 || len >= 16 || strspn(p, "0123456789") != len) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: not running (lxc-info exit %d)", name, status);
        return FALSE;                                        
    }
    g_strlcpy(pid, p, 16);
    return TRUE;
}

                                                                                                       
                                                                                         
                                             
                                                                      
static gboolean
_real_probe(const PcvHealthProbeSpec *spec, GError **error)
{
    gchar pid[16];
    if (!_health_init_pid(spec->name, pid, error))
        return FALSE;
    gchar tmo[16];                           
    g_snprintf(tmo, sizeof tmo, "%d", spec->timeout_sec);
    gint status = -1;
    gboolean ran = FALSE;
    if (g_strcmp0(spec->type, "exec") == 0) {
        const gchar *cmd[] = {spec->target, NULL};
        g_auto(GStrv) argv = pcv_lxc_attach_argv(PCV_LXC_PATH, spec->name, cmd);
        if (!argv) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s: empty exec target", spec->name);
            return FALSE;
        }
        ran = pcv_lxc_ns_run((const gchar * const *)argv, (guint)spec->timeout_sec, PCV_LXC_NS_OUTPUT_MAX,
                             NULL, NULL, &status, error);
    } else {
        const gboolean http = g_strcmp0(spec->type, "http") == 0;
        const gchar *tcp_argv[]  = {"nsenter", "-t", pid, "-n", "--",
                                    "nc", "-z", "-w", tmo, "localhost", spec->target, NULL};
        const gchar *http_argv[] = {"nsenter", "-t", pid, "-n", "--",
                                    "curl", "-sf", "--max-time", tmo, spec->target, NULL};
        const PcvSpawnBound bound = {
            .profile           = PCV_CHILD_CAP_RUNTIME,                                                       
            .timeout_sec       = (guint)spec->timeout_sec + 1,
            .max_output_bytes  = 0,                                                  
            .cancellable       = NULL,
            .should_abort      = pcv_drain_is_terminating,
            .new_process_group = TRUE,
        };
        ran = pcv_spawn_sync_bounded(http ? http_argv : tcp_argv, &bound, NULL, NULL, &status, error);
    }
    if (!ran)
        return FALSE;
    if (status != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s %s probe exit %d", spec->name, spec->type, status);
        return FALSE;
    }
    return TRUE;
}

                                                                                                
                                                           
static gpointer
_real_lock(const gchar *name)
{
    gchar *err = NULL;
    PcvVmLock *lock = pcv_vm_lock_acquire(name, VM_OP_STARTING, &err);
    if (!lock)
        PCV_LOG_DEBUG(HEALTH_LOG_DOM, "health auto-restart of '%s' skipped: %s", name, err ? err : "busy");
    g_free(err);
    return lock;
}

                                              
static void
_real_unlock(gpointer lock)
{
    pcv_vm_lock_release(lock);
}

                                                                                      
                                           
static gboolean
_real_renew(gpointer lock)
{
    return pcv_vm_lock_renew(lock);
}

                                                                                            
                                                               
static gint
_real_stop(const gchar *name)
{
    const gchar *argv[] = {"lxc-stop", "-P", PCV_LXC_PATH, "-n", name, "-t", HEALTH_STOP_GRACE_ARG, NULL};
    const PcvSpawnBound bound = {
        .profile           = PCV_CHILD_CAP_RUNTIME,
        .timeout_sec       = HEALTH_STOP_TIMEOUT_SEC,
        .max_output_bytes  = 0,
        .cancellable       = NULL,
        .should_abort      = pcv_drain_is_terminating,
        .new_process_group = TRUE,
    };
    GError *err = NULL;
    gint status = -1;
    if (!pcv_spawn_sync_bounded(argv, &bound, NULL, NULL, &status, &err)) {
        PCV_LOG_WARN(HEALTH_LOG_DOM, "health auto-restart of '%s': lxc-stop not confirmed: %s",
                     name, err ? err->message : "unknown");
        g_clear_error(&err);
        return -1;
    }
    return status;
}

                                                                                              
                                                              
                                                          
static gboolean
_real_start(const gchar *name)
{
    GError *err = NULL;
    const gboolean ok = pcv_lxc_start_sync(name, &err);
    if (!ok)
        PCV_LOG_WARN(HEALTH_LOG_DOM, "health auto-restart of '%s': start failed: %s",
                     name, err ? err->message : "unknown error");
    g_clear_error(&err);
    return ok;
}

                                                                              
static const PcvHealthOps G_health_real_ops = {
    .probe           = _real_probe,
    .lock            = _real_lock,
    .unlock          = _real_unlock,
    .restart_allowed = pcv_container_volume_restart_allowed,
    .stop            = _real_stop,
    .after_stop      = pcv_container_volume_restart_after_stop,
    .start           = _real_start,
    .renew           = _real_renew,
    .is_terminating  = pcv_drain_is_terminating,
    .is_draining     = pcv_drain_is_shutdown,
};

                                                                     
static const PcvHealthOps *
_health_ops(void)
{
    const PcvHealthOps *ops = g_atomic_pointer_get(&G_health_ops_test);
    return ops ? ops : &G_health_real_ops;
}

                                                                            

                                                         
static gint
_health_find(const gchar *ctr_name)
{
    for (gint i = 0; i < g_n_health_probes; i++) {
        if (g_strcmp0(g_health_probes[i].name, ctr_name) == 0)
            return i;
    }
    return -1;
}

                                                                     
                                             
                                                               
static gboolean
_health_apply(const gchar *name, guint64 gen, gboolean healthy)
{
    gboolean restart = FALSE;
    g_mutex_lock(&g_health_mu);
    gint idx = _health_find(name);
    if (idx >= 0 && g_health_probes[idx].gen == gen) {
        ContainerHealthProbe *p = &g_health_probes[idx];
        if (healthy) {
            p->consecutive_failures = 0;                      
            p->healthy = TRUE;
        } else if (++p->consecutive_failures >= p->failure_threshold) {
            p->healthy = FALSE;                            
            restart = p->auto_restart;
        }
    }
    g_mutex_unlock(&g_health_mu);
    return restart;
}

                                                                                          
                                                                            
                         
                                                                           
static HealthRestart
_health_restart(const PcvHealthOps *ops, const gchar *name)
{
    gpointer lock = ops->lock(name);
    if (!lock)
        return HEALTH_RESTART_BUSY;                                              
    if (!ops->restart_allowed(name)) {                                                    
        ops->unlock(lock);
        return HEALTH_RESTART_DENIED;
    }
    PCV_LOG_INFO(HEALTH_LOG_DOM, "[HealthCheck] %s unhealthy, restarting", name);
    HealthRestart r = HEALTH_RESTART_REFUSED;
    gint stop_rc = ops->stop(name);                             
    (void)ops->renew(lock);                                                          
    if (ops->after_stop(name, stop_rc)) {                                
        if (ops->is_terminating()) {                                                                             
            PCV_LOG_WARN(HEALTH_LOG_DOM, "health auto-restart of '%s': daemon is shutting down — left stopped", name);
        } else {
            (void)ops->start(name);                                                    
            r = HEALTH_RESTART_DONE;
        }
    }
    ops->unlock(lock);
    return r;
}

                                                                                  
                                                              
                                                       
static void
_health_finish(const gchar *name, guint64 gen, HealthRestart r)
{
    g_mutex_lock(&g_health_mu);
    if (g_health_running > 0)
        g_health_running--;
    gint idx = _health_find(name);
    if (idx >= 0 && g_health_probes[idx].gen == gen) {
        ContainerHealthProbe *p = &g_health_probes[idx];
        p->inflight = FALSE;
        if (r == HEALTH_RESTART_DONE)
            p->restart_count++;
        if (r == HEALTH_RESTART_DONE || r == HEALTH_RESTART_DENIED || r == HEALTH_RESTART_REFUSED)
            p->consecutive_failures = 0;                                        
    }
    g_mutex_unlock(&g_health_mu);
}

                                                                           
                                                                
static void
_health_complete(const gchar *name, guint64 gen, gboolean healthy, gboolean cancelled)
{
    const PcvHealthOps *ops = _health_ops();
    HealthRestart r = HEALTH_RESTART_NONE;
    if (!cancelled && _health_apply(name, gen, healthy) && !ops->is_terminating())
        r = _health_restart(ops, name);
    _health_finish(name, gen, r);
}

                                                                                           
                                                               
static void
_health_run(const PcvHealthProbeSpec *spec)
{
    const PcvHealthOps *ops = _health_ops();
    gboolean healthy = FALSE;
    gboolean cancelled = ops->is_terminating();
    if (!cancelled) {
        GError *err = NULL;
        healthy = ops->probe(spec, &err);
        cancelled = !healthy && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        if (!healthy && !cancelled)
            PCV_LOG_DEBUG(HEALTH_LOG_DOM, "probe %s (%s) failed: %s", spec->name, spec->type,
                          err ? err->message : "unknown");
        g_clear_error(&err);
    }
    _health_complete(spec->name, spec->gen, healthy, cancelled);
}

                                                                                               
static void
_health_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
    (void)source; (void)cancellable;
    _health_run(task_data);
    g_task_return_boolean(task, TRUE);
}

                                                                                             
                                                                             
                                                             
                                                                           
static guint
_health_tick_core(gint64 now, HealthDispatch mode, gboolean *cont, guint *stopped_src)
{
                                                                                           
                                                                              
                                                                                     
                                                                       
    const PcvHealthOps *ops = _health_ops();
    const gboolean terminating = ops->is_terminating();                                
    const gboolean draining = ops->is_draining();                                               
    PcvHealthProbeSpec *due[PCV_HEALTH_MAX_INFLIGHT];
    guint n_due = 0;

    *cont = TRUE;
    *stopped_src = 0;
    g_mutex_lock(&g_health_mu);
    if (terminating || g_n_health_probes == 0) {
        *stopped_src = g_health_timer_id;                      
        g_health_timer_id = 0;
        *cont = FALSE;
        g_mutex_unlock(&g_health_mu);
        return 0;
    }
    if (draining) {
                                                                                           
                                                                                
                                                             
        g_mutex_unlock(&g_health_mu);
        return 0;
    }
                                                                                       
                                                                                    
    gint cand[MAX_HEALTH_PROBES];
    gint n_cand = 0;
    for (gint i = 0; i < g_n_health_probes; i++) {
        const ContainerHealthProbe *p = &g_health_probes[i];
        if (p->inflight || (now - p->last_check_time) < (gint64)p->interval_sec * G_USEC_PER_SEC)
            continue;                                                           
        gint j = n_cand++;                                          
        while (j > 0 && g_health_probes[cand[j - 1]].last_check_time > p->last_check_time) {
            cand[j] = cand[j - 1];
            j--;
        }
        cand[j] = i;
    }
    for (gint k = 0; k < n_cand && g_health_running < PCV_HEALTH_MAX_INFLIGHT; k++) {
        ContainerHealthProbe *p = &g_health_probes[cand[k]];
        p->last_check_time = now;                                                           
        p->inflight = TRUE;
        g_health_running++;
        PcvHealthProbeSpec *s = g_new0(PcvHealthProbeSpec, 1);
        g_strlcpy(s->name, p->name, sizeof s->name);
        g_strlcpy(s->type, p->probe_type, sizeof s->type);
        g_strlcpy(s->target, p->target, sizeof s->target);
        s->timeout_sec = pcv_container_health_clamp_timeout(p->timeout_sec);
        s->gen = p->gen;
        due[n_due++] = s;
    }
    g_mutex_unlock(&g_health_mu);

    for (guint i = 0; i < n_due; i++) {
        switch (mode) {
        case HEALTH_DISPATCH_POOL: {
            GTask *t = pcv_drain_task_new(NULL, NULL, NULL, NULL);                            
            g_task_set_task_data(t, due[i], g_free);
            pcv_worker_pool_push(t, _health_worker);
            g_object_unref(t);                      
            break;
        }
        case HEALTH_DISPATCH_INLINE:
            _health_run(due[i]);
            g_free(due[i]);
            break;
        case HEALTH_DISPATCH_MARK_ONLY:
            g_free(due[i]);
            break;
        }
    }
    return n_due;
}

                                                     
                                                             
static gboolean
_health_tick_cb(gpointer user_data)
{
    (void)user_data;
    gboolean cont = TRUE;
    guint self_id = 0;                                                     
    (void)_health_tick_core(g_get_monotonic_time(), HEALTH_DISPATCH_POOL, &cont, &self_id);
    return cont ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

                                                                        

                                                                         
static void
_health_send_success(const gchar *rpc_id, UdsServer *server, GSocketConnection *conn)
{
    JsonObject *r = json_object_new();
    json_object_set_boolean_member(r, "success", TRUE);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, r);
    gchar *rsp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, conn, rsp);
    g_free(rsp);
}

                                                              
static void
_health_send_error(const gchar *rpc_id, UdsServer *server, GSocketConnection *conn,
                   PureRpcErrorCode code, const gchar *message)
{
    gchar *e = pure_rpc_build_error_response(rpc_id, code, message);
    pure_uds_server_send_response(server, conn, e);
    g_free(e);
}

                                                                            
                                                                                      
                                                          
static const gchar *
_health_target_error(const gchar *type, const gchar *target)
{
    gsize len = strlen(target);
    if (len > HEALTH_TARGET_MAX_BYTES)
        return "target too long (max 255 bytes)";
    if (g_strcmp0(type, "tcp") == 0) {
        gint64 port = 0;
        gboolean ok = len > 0 && len <= 5;
        for (gsize i = 0; ok && i < len; i++) {
            ok = g_ascii_isdigit(target[i]);
            if (ok) port = port * 10 + (target[i] - '0');
        }
        if (!ok || port < 1 || port > 65535)
            return "Invalid tcp target: decimal port 1-65535";
        return NULL;
    }
    gboolean clean = TRUE;                             
    for (gsize i = 0; i < len; i++)
        if ((guchar)target[i] <= ' ' || (guchar)target[i] == 0x7f) clean = FALSE;
    if (g_strcmp0(type, "http") == 0) {
        if (!clean || !(g_str_has_prefix(target, "http://") || g_str_has_prefix(target, "https://")))
            return "Invalid http target: http:// or https:// URL without spaces or control characters";
        return NULL;
    }
    gboolean no_ctl = TRUE;                                                                      
    gboolean has_space = FALSE;
    for (gsize i = 0; i < len; i++) {
        if ((guchar)target[i] < ' ' || (guchar)target[i] == 0x7f) no_ctl = FALSE;
        if (target[i] == ' ') has_space = TRUE;
    }
    if (len == 0 || target[0] == '-' || !no_ctl)
        return "Invalid exec target: non-empty, not starting with '-', no control characters";
    if (has_space)                                                                                   
        return "Invalid exec target: a single program path without spaces (arguments are not supported)";
    return NULL;
}

                                            
                                                             
void
handle_container_health_set(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *conn)
{
                                                                                     
    if (!json_object_has_member(params, "name") || !json_object_has_member(params, "type") ||
        !json_object_has_member(params, "target")) {
        _health_send_error(rpc_id, server, conn, PURE_RPC_ERR_INVALID_PARAMS,
                           "Required: name, type (tcp/http/exec), target");
        return;
    }
    const gchar *cname  = pcv_rpc_params_get_string(params, "name");
    const gchar *type   = pcv_rpc_params_get_string(params, "type");
    const gchar *target = pcv_rpc_params_get_string(params, "target");

    if (!cname || !type || !target) {
        _health_send_error(rpc_id, server, conn, PURE_RPC_ERR_INVALID_PARAMS,
                           "Required: name, type (tcp/http/exec), target");
        return;
    }
    if (!pcv_validate_vm_name(cname)) {
        _health_send_error(rpc_id, server, conn, PURE_RPC_ERR_INVALID_PARAMS,
                           "Invalid container name: 1-64 chars [a-zA-Z0-9_-]");
        return;
    }
    if (g_strcmp0(type, "tcp") != 0 && g_strcmp0(type, "http") != 0 &&
        g_strcmp0(type, "exec") != 0) {
        _health_send_error(rpc_id, server, conn, PURE_RPC_ERR_INVALID_PARAMS, "type must be tcp, http, or exec");
        return;
    }
    const gchar *target_err = _health_target_error(type, target);
    if (target_err) {
        _health_send_error(rpc_id, server, conn, PURE_RPC_ERR_INVALID_PARAMS, target_err);
        return;
    }

    g_mutex_lock(&g_health_mu);
                                                              
    gint idx = _health_find(cname);
    if (idx < 0) {
        if (g_n_health_probes >= MAX_HEALTH_PROBES) {                           
            g_mutex_unlock(&g_health_mu);
            _health_send_error(rpc_id, server, conn, PURE_RPC_ERR_ZFS_OPERATION, "Max health probes reached (32)");
            return;
        }
        idx = g_n_health_probes++;                                
    }
    ContainerHealthProbe *p = &g_health_probes[idx];
    memset(p, 0, sizeof(*p));                                                                  
    g_strlcpy(p->name, cname, sizeof(p->name));
    g_strlcpy(p->probe_type, type, sizeof(p->probe_type));
    g_strlcpy(p->target, target, sizeof(p->target));
    p->timeout_sec = json_object_has_member(params, "timeout_sec")
        ? (gint)json_object_get_int_member(params, "timeout_sec") : 5;
    p->interval_sec = json_object_has_member(params, "interval_sec")
        ? (gint)json_object_get_int_member(params, "interval_sec") : 30;
    if (p->interval_sec < 5) p->interval_sec = 5;                                   
    p->failure_threshold = json_object_has_member(params, "failure_threshold")
        ? (gint)json_object_get_int_member(params, "failure_threshold") : 3;
    if (p->failure_threshold < 1) p->failure_threshold = 1;                                
    p->auto_restart = json_object_has_member(params, "auto_restart")
        ? json_object_get_boolean_member(params, "auto_restart") : FALSE;
    p->consecutive_failures = 0;                                                 
    p->healthy = TRUE;                                              
    p->last_check_time = g_get_monotonic_time();
    p->gen = ++g_health_gen_seq;                                             
    p->inflight = FALSE;                                                           

                                                            
    if (g_health_timer_id == 0)
        g_health_timer_id = g_timeout_add_seconds(1, _health_tick_cb, NULL);

    g_mutex_unlock(&g_health_mu);                                   
    _health_send_success(rpc_id, server, conn);
}

                                            
                                                                           
void
handle_container_health_get(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *conn)
{
    const gchar *cname = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    JsonNode *n = NULL;

    g_mutex_lock(&g_health_mu);
    if (cname) {                            
        gint idx = _health_find(cname);
        if (idx < 0) {
            g_mutex_unlock(&g_health_mu);
            _health_send_error(rpc_id, server, conn, PURE_RPC_ERR_ZFS_OPERATION, "No health probe for this container");
            return;
        }
        ContainerHealthProbe *p = &g_health_probes[idx];
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "name", p->name);
        json_object_set_string_member(obj, "type", p->probe_type);
        json_object_set_string_member(obj, "target", p->target);
        json_object_set_int_member(obj, "interval_sec", p->interval_sec);
        json_object_set_int_member(obj, "failure_threshold", p->failure_threshold);
        json_object_set_boolean_member(obj, "healthy", p->healthy);
        json_object_set_int_member(obj, "consecutive_failures", p->consecutive_failures);
        json_object_set_int_member(obj, "restart_count", p->restart_count);
        json_object_set_boolean_member(obj, "auto_restart", p->auto_restart);
        n = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(n, obj);
    } else {                               
        JsonArray *arr = json_array_new();
        for (gint i = 0; i < g_n_health_probes; i++) {
            ContainerHealthProbe *p = &g_health_probes[i];
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "name", p->name);
            json_object_set_string_member(obj, "type", p->probe_type);
            json_object_set_boolean_member(obj, "healthy", p->healthy);
            json_object_set_int_member(obj, "consecutive_failures", p->consecutive_failures);
            json_object_set_int_member(obj, "restart_count", p->restart_count);
            json_array_add_object_element(arr, obj);
        }
        n = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(n, arr);
    }
    g_mutex_unlock(&g_health_mu);

    gchar *rsp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, conn, rsp);
    g_free(rsp);
}

                                                
                                                                         
void
handle_container_health_delete(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *conn)
{
    const gchar *cname = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!cname) {
        _health_send_error(rpc_id, server, conn, PURE_RPC_ERR_INVALID_PARAMS, "Required: name");
        return;
    }

    g_mutex_lock(&g_health_mu);
    gint idx = _health_find(cname);
    if (idx >= 0) {
                                                                                  
        if (idx < g_n_health_probes - 1)
            g_health_probes[idx] = g_health_probes[g_n_health_probes - 1];
        g_n_health_probes--;
        g_health_gen_seq++;                                
    }
    g_mutex_unlock(&g_health_mu);
    _health_send_success(rpc_id, server, conn);                     
}

                                                                           

                                                                                       
                                     
void
pcv_container_health_set_ops_for_test(const PcvHealthOps *ops)
{
    g_atomic_pointer_set(&G_health_ops_test, ops);
}

                                                                                
                                     
guint
pcv_container_health_tick_for_test(gint64 now_us, gboolean run_inline, gboolean *source_continue)
{
    gboolean cont = TRUE;
    guint stopped = 0;
    guint n = _health_tick_core(now_us, run_inline ? HEALTH_DISPATCH_INLINE : HEALTH_DISPATCH_MARK_ONLY,
                                &cont, &stopped);
    if (stopped)
        g_source_remove(stopped);                                      
    if (source_continue)
        *source_continue = cont;
    return n;
}

                                                                       
                                          
void
pcv_container_health_complete_for_test(const gchar *name, guint64 gen, gboolean healthy, gboolean cancelled)
{
    _health_complete(name, gen, healthy, cancelled);
}

                                                                           
gboolean
pcv_container_health_timer_active_for_test(void)
{
    g_mutex_lock(&g_health_mu);
    gboolean active = g_health_timer_id != 0;
    g_mutex_unlock(&g_health_mu);
    return active;
}

                                                                              
guint64
pcv_container_health_gen_for_test(const gchar *name)
{
    g_mutex_lock(&g_health_mu);
    gint idx = _health_find(name);
    guint64 gen = idx >= 0 ? g_health_probes[idx].gen : 0;
    g_mutex_unlock(&g_health_mu);
    return gen;
}

                                                                                   
                           
void
pcv_container_health_reset_for_test(void)
{
    g_mutex_lock(&g_health_mu);
    guint id = g_health_timer_id;
    g_health_timer_id = 0;
    g_n_health_probes = 0;
    g_health_running = 0;
    memset(g_health_probes, 0, sizeof g_health_probes);
    g_mutex_unlock(&g_health_mu);
    if (id)
        g_source_remove(id);
}
