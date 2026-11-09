                                                                             
                                      
                                                                 
  
                           
                                                                                             
                                                                                   
                                                                                                  
                                                                                 
                                                    
                                                                             
                                                                                      
                                                             
                                                                
                                                                          
                                                                           
                                                                                      
                                                                             
                                                                     
                                                                
                                                                                          
                                                                            
                                                                             
                                                     
  
                                                                  
                                                                 
                                                          
                                                                                
#include "poll_async.h"
#include "rpc_singleflight.h"
#include "rpc_utils.h"

#include <string.h>

#include "api/drain.h"
#include "modules/audit/pcv_audit.h"
#include "modules/dispatcher/rpc_completion.h"
#include "utils/pcv_log.h"
#include "utils/pcv_proc_state.h"

#define LOG_DOM "poll_async"

                                                                               
#define POLL_CMD_TIMEOUT_S   10
#define POLL_CALL_BUDGET_S   20
#define POLL_MAX_OUTPUT      ((gsize)4 * 1024 * 1024)
#define POLL_MIN_REMAINING_US ((gint64)G_USEC_PER_SEC)

typedef gboolean (*PollSpawnFn)(const gchar * const *argv, const PcvSpawnBound *b, gchar **out,
                                gchar **err, gint *status, GError **error);
typedef gint64 (*PollNowFn)(void);

                                                                 
struct PcvPollGuard {
    const gchar *method;
    gint64       start_us;                                   
    gchar       *owned;                                                         
    gboolean     host_stalled;                                               
    gboolean     host_aborted;                                                        
};

                                                     
                                                             
typedef struct {
    gchar             *rpc_id;
    UdsServer         *server;               
    GSocketConnection *connection;           
    gchar             *actor;                                                       
} PollWaiter;

                                                 
typedef struct {
    gchar            *key;
    gchar            *method;
    JsonNode         *params;                                                               
    PcvPollComputeFn  compute;
} PollRun;

                                              
typedef struct {
    GPid   pid;
    gchar *argv0;
} PollStuck;

                                                           
static struct {
    GMutex      mutex;
    GHashTable *stuck;                                                  
    PollSpawnFn spawn_fn;                                      
    PollNowFn   now_fn;                                      
} g_pa;

                                                                                 
static PcvSingleflight *g_poll_sf;

                                                                                    
static GHashTable *g_poll_capped;

                                                                         
static void
_poll_waiter_free(gpointer data)
{
    PollWaiter *w = data;
    g_free(w->rpc_id);
    g_clear_object(&w->server);
    g_clear_object(&w->connection);
    g_free(w->actor);
    g_free(w);
}

                                                                          
static void
_poll_run_free(gpointer data)
{
    PollRun *run = data;
    g_free(run->key);
    g_free(run->method);
    if (run->params)
        json_node_unref(run->params);
    g_free(run);
}

static void
_poll_stuck_free(gpointer data)
{
    PollStuck *s = data;
    g_free(s->argv0);
    g_free(s);
}

                                                                    
                                                    
                                                           
static void
_poll_canon_append(GString *s, JsonNode *node)
{
    if (!node || JSON_NODE_HOLDS_NULL(node)) {
        g_string_append(s, "null");
    } else if (JSON_NODE_HOLDS_OBJECT(node)) {
        JsonObject *o = json_node_get_object(node);
        GList *keys = g_list_sort(json_object_get_members(o), (GCompareFunc)strcmp);
        g_string_append_c(s, '{');
        for (GList *l = keys; l; l = l->next) {
            JsonNode *k = json_node_init_string(json_node_alloc(), l->data);
            gchar *ks = json_to_string(k, FALSE);
            if (l != keys)
                g_string_append_c(s, ',');
            g_string_append(s, ks);
            g_string_append_c(s, ':');
            _poll_canon_append(s, json_object_get_member(o, l->data));
            g_free(ks);
            json_node_unref(k);
        }
        g_string_append_c(s, '}');
        g_list_free(keys);
    } else if (JSON_NODE_HOLDS_ARRAY(node)) {
        JsonArray *a = json_node_get_array(node);
        g_string_append_c(s, '[');
        for (guint i = 0; i < json_array_get_length(a); i++) {
            if (i)
                g_string_append_c(s, ',');
            _poll_canon_append(s, json_array_get_element(a, i));
        }
        g_string_append_c(s, ']');
    } else {
        gchar *v = json_to_string(node, FALSE);
        g_string_append(s, v);
        g_free(v);
    }
}

                                                                           
static gchar *
_poll_canon_params(JsonObject *params)
{
    GString *s = g_string_new(NULL);
    if (params) {
        JsonNode *n = json_node_init_object(json_node_alloc(), params);
        _poll_canon_append(s, n);
        json_node_unref(n);
    } else {
        g_string_append(s, "null");
    }
    return g_string_free(s, FALSE);
}

                                                                                                       
                                                                   
static PureRpcErrorCode
_poll_rpc_code(const GError *e)
{
    switch (e ? e->code : 0) {
    case PURE_RPC_ERR_PARSE_ERROR:
    case PURE_RPC_ERR_INVALID_REQUEST:
    case PURE_RPC_ERR_METHOD_NOT_FOUND:
    case PURE_RPC_ERR_INVALID_PARAMS:
    case PURE_RPC_ERR_INTERNAL_ERROR:
    case PURE_RPC_ERR_ZFS_OPERATION:
    case PURE_RPC_ERR_VM_NOT_FOUND:
    case PURE_RPC_ERR_CONFLICT:
    case PURE_RPC_ERR_TIMEOUT:
    case PURE_RPC_ERR_BUSY:
    case PURE_RPC_ERR_NOT_FOUND:
    case PURE_RPC_ERR_FORBIDDEN:
    case PURE_RPC_ERR_TOTP_REQUIRED:
    case PURE_RPC_ERR_TOTP_ENROLL_REQUIRED:
    case PURE_RPC_ERR_TOTP_INVALID_CODE:
    case PURE_RPC_ERR_TOTP_PENDING_EXPIRED:
    case PURE_RPC_ERR_TOTP_LOCKED:
    case PURE_RPC_ERR_HOSTKEY_UNTRUSTED:
    case PURE_RPC_ERR_HOSTKEY_MISMATCH:
        return (PureRpcErrorCode)e->code;
    default:
        return PURE_RPC_ERR_INTERNAL_ERROR;
    }
}

                                                         
static gint64
_poll_now(void)
{
    g_mutex_lock(&g_pa.mutex);
    PollNowFn fn = g_pa.now_fn;
    g_mutex_unlock(&g_pa.mutex);
    return fn ? fn() : g_get_monotonic_time();
}

                                                                                                
                                                                                    
static gboolean
_poll_storage_cmd(const gchar *argv0)
{
    const gchar *slash = strrchr(argv0, '/');
    const gchar *base = slash ? slash + 1 : argv0;
    return g_strcmp0(base, "zfs") == 0 || g_strcmp0(base, "zpool") == 0;
}

                                                                          
                                                                 
                                                         
static const gchar *
_poll_stuck_find_locked(const gchar *method)
{
    GPtrArray *arr = g_pa.stuck ? g_hash_table_lookup(g_pa.stuck, method) : NULL;
    if (!arr)
        return NULL;
    for (guint i = arr->len; i > 0; i--) {
        PollStuck *s = g_ptr_array_index(arr, i - 1);
        if (!pcv_proc_pid_alive(s->pid))
            g_ptr_array_remove_index(arr, i - 1);
    }
    if (arr->len == 0) {
        g_hash_table_remove(g_pa.stuck, method);
        return NULL;
    }
    return ((PollStuck *)g_ptr_array_index(arr, 0))->argv0;
}

                                                            
                                                                       
static void
_poll_stuck_record(const gchar *method, const gchar *argv0, GPid pid)
{
    PollStuck *s = g_new0(PollStuck, 1);
    s->pid = pid;
    s->argv0 = g_strdup(argv0);
    g_mutex_lock(&g_pa.mutex);
    if (!g_pa.stuck)
        g_pa.stuck = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)g_ptr_array_unref);
    GPtrArray *arr = g_hash_table_lookup(g_pa.stuck, method);
    if (!arr) {
        arr = g_ptr_array_new_with_free_func(_poll_stuck_free);
        g_hash_table_insert(g_pa.stuck, g_strdup(method), arr);
    }
    g_ptr_array_add(arr, s);
    g_mutex_unlock(&g_pa.mutex);
    PCV_LOG_WARN(LOG_DOM, "'%s'(pid %d)이 %s 에서 SIGKILL 뒤 회수되지 않았다 — 살아 있는 동안 이 메서드의 "
                 "새 명령을 띄우지 않는다", argv0, (gint)pid, method);
}

static gboolean _poll_guard_run(PcvPollGuard *g, const gchar * const *argv, gchar **out, gchar **err,
                                PcvChildCapabilityProfile profile, gboolean keep_output_on_exit, GError **error);

                                                             
                                                                       
                                                            
                                                                                           
                                                      
                                                              
gboolean
pcv_poll_guard_spawn(PcvPollGuard *g, const gchar * const *argv, gchar **out, gchar **err, GError **error)
{
    g_return_val_if_fail(argv && argv[0], FALSE);
    return _poll_guard_run(g, argv, out, err, _poll_storage_cmd(argv[0]) ? PCV_CHILD_CAP_STORAGE : PCV_CHILD_CAP_BASE,
                           FALSE, error);
}

                                                                                    
                                                                
gboolean
pcv_poll_guard_spawn_override(gpointer data, const gchar * const *argv, gchar **out, gchar **err, GError **error)
{
    return _poll_guard_run(data, argv, out, err, pcv_spawn_capability_profile_for_argv(argv), TRUE, error);
}

                                                                                      
gboolean
pcv_poll_guard_host_stalled(const PcvPollGuard *g)
{
    return g && g->host_stalled;
}

                                                                                     
gboolean
pcv_poll_guard_host_aborted(const PcvPollGuard *g)
{
    return g && g->host_aborted;
}

                                                                  
PcvPollGuard *
pcv_poll_guard_new(const gchar *method)
{
    PcvPollGuard *g = g_new0(PcvPollGuard, 1);
    g->owned = g_strdup(method);
    g->method = g->owned;
    g->start_us = _poll_now();
    return g;
}

void
pcv_poll_guard_free(PcvPollGuard *g)
{
    if (!g)
        return;
    g_free(g->owned);
    g_free(g);
}

                                                                                                  
static gboolean
_poll_guard_run(PcvPollGuard *g, const gchar * const *argv, gchar **out, gchar **err,
                PcvChildCapabilityProfile profile, gboolean keep_output_on_exit, GError **error)
{
    if (out) *out = NULL;
    if (err) *err = NULL;
    g_return_val_if_fail(g && g->method && argv && argv[0], FALSE);

    gint64 remaining = (gint64)POLL_CALL_BUDGET_S * G_USEC_PER_SEC - (_poll_now() - g->start_us);
    if (remaining < POLL_MIN_REMAINING_US) {
        g->host_stalled = TRUE;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "%s: not started — %ds call budget for %s exhausted", argv[0], POLL_CALL_BUDGET_S,
                    g->method);
        PCV_LOG_WARN(LOG_DOM, "poll '%s': not running '%s' (%s)", g->method, argv[0], "call budget exhausted");
        return FALSE;
    }
    guint timeout_sec = (guint)MIN((gint64)POLL_CMD_TIMEOUT_S, remaining / G_USEC_PER_SEC);

    g_mutex_lock(&g_pa.mutex);
    const gchar *stuck = _poll_stuck_find_locked(g->method);
    gchar *busy_reason = NULL;                                                     
    if (stuck) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY, "previous '%s' for %s is still running", stuck,
                    g->method);
        busy_reason = g_strdup_printf("previous '%s' still running", stuck);
    }
    PollSpawnFn spawn_fn = g_pa.spawn_fn;
    g_mutex_unlock(&g_pa.mutex);
    if (busy_reason) {                                 
        g->host_stalled = TRUE;
        PCV_LOG_WARN(LOG_DOM, "poll '%s': not running '%s' (%s)", g->method, argv[0], busy_reason);
        g_free(busy_reason);
        return FALSE;
    }

    GPid unreaped = 0;
    PcvSpawnBound b = { .profile = PCV_CHILD_CAP_BASE, .timeout_sec = timeout_sec,
                        .max_output_bytes = POLL_MAX_OUTPUT, .should_abort = pcv_drain_is_terminating,
                        .new_process_group = TRUE, .unreaped_pid_out = &unreaped };
    switch (profile) {                                                              
    case PCV_CHILD_CAP_STORAGE: b.profile = PCV_CHILD_CAP_STORAGE; break;
    case PCV_CHILD_CAP_RUNTIME: b.profile = PCV_CHILD_CAP_RUNTIME; break;
    case PCV_CHILD_CAP_DHCP:    b.profile = PCV_CHILD_CAP_DHCP;    break;
    case PCV_CHILD_CAP_SIGNAL:  b.profile = PCV_CHILD_CAP_SIGNAL;  break;
    default: break;
    }
    gchar *o = NULL, *e = NULL;
    gint status = -1;
    GError *spawn_err = NULL;                                             
    gboolean ok = (spawn_fn ? spawn_fn : pcv_spawn_sync_bounded)(argv, &b, out ? &o : NULL,
                                                                 err ? &e : NULL, &status, &spawn_err);
    if (unreaped > 0)
        _poll_stuck_record(g->method, argv[0], unreaped);
    if (spawn_err) {
        if (g_error_matches(spawn_err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
            g_error_matches(spawn_err, G_IO_ERROR, G_IO_ERROR_BUSY))
            g->host_stalled = TRUE;
                                                                                                              
                                                                 
        if (g_error_matches(spawn_err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g->host_aborted = TRUE;
        g_propagate_error(error, spawn_err);
    }
    gboolean exited_nonzero = ok && status != 0;
    if (exited_nonzero) {                                                            
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", argv[0],
                    (e && *e) ? g_strstrip(e) : "process exited with non-zero status");
        ok = FALSE;
    }
    if (ok || (exited_nonzero && keep_output_on_exit)) {
        if (out) *out = g_steal_pointer(&o);
        if (err) *err = g_steal_pointer(&e);
    }
    g_free(o);
    g_free(e);
    return ok;
}

                                                                               
                                                 
                                             
static void
_poll_async_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
    (void)source; (void)cancel;
    PollRun *run = task_data;
    PcvPollGuard guard = { .method = run->method, .start_us = _poll_now() };
    JsonObject *params = (run->params && JSON_NODE_HOLDS_OBJECT(run->params))
                         ? json_node_get_object(run->params) : NULL;
    GError *err = NULL;
    JsonNode *result = run->compute(params, &guard, &err);
    if (result) {
        g_clear_error(&err);
        g_task_return_pointer(task, result, (GDestroyNotify)json_node_unref);
        return;
    }
    if (!err)
        err = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "%s: poll compute returned no result", run->method);
    g_task_return_error(task, err);
}

                                                                
                                                                   
                                                           
                                                                
static void
_poll_async_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source; (void)user_data;
    PollRun *run = g_task_get_task_data(G_TASK(res));
    GError *err = NULL;
    JsonNode *result = g_task_propagate_pointer(G_TASK(res), &err);
    gchar *result_json = result ? json_to_string(result, FALSE) : NULL;
    gint code = result ? 0 : (gint)_poll_rpc_code(err);
    GPtrArray *waiters = g_poll_sf ? pcv_singleflight_complete(g_poll_sf, run->key) : NULL;
    if (g_poll_capped)
        g_hash_table_remove(g_poll_capped, run->key);                                
    for (guint i = 0; waiters && i < waiters->len; i++) {
        PollWaiter *w = g_ptr_array_index(waiters, i);
                                                                                                
        pcv_audit_log(w->actor, run->method, "", result ? "ok" : "fail", code, 0, "local");
        gchar *resp = result
            ? pure_rpc_build_success_response(w->rpc_id, json_from_string(result_json, NULL))
            : pure_rpc_build_error_response(w->rpc_id, (PureRpcErrorCode)code,
                                            err ? err->message : "poll failed");
        pure_uds_server_send_response(w->server, w->connection, resp);
        g_free(resp);
    }
    if (waiters)
        g_ptr_array_unref(waiters);
    g_free(result_json);
    if (result)
        json_node_unref(result);
    g_clear_error(&err);
}

                                                                               
                                            
                                                                       
static void
_poll_refuse_busy(const gchar *method, const gchar *key, const gchar *rpc_id, UdsServer *server,
                  GSocketConnection *connection, const gchar *actor)
{
    if (!g_poll_capped)
        g_poll_capped = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (g_hash_table_add(g_poll_capped, g_strdup(key)))                                
        PCV_LOG_WARN(LOG_DOM, "poll '%s' has %d waiters; refusing new requests until it completes", method,
                     PCV_POLL_MAX_WAITERS);
                                                                                            
    pcv_audit_log(actor, method, "", "fail", PURE_RPC_ERR_BUSY, 0, "local");
    gchar *msg = g_strdup_printf("%s is still running from an earlier request", method);
    gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY, msg);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(msg);
}

                                                             
                                                                                      
                                                                              
                                                                                
                                                      
                                                                                                
                                                         
void
pcv_poll_async_submit(const gchar *method, JsonObject *params, const gchar *rpc_id,
                      UdsServer *server, GSocketConnection *connection, PcvPollComputeFn compute)
{
    g_return_if_fail(method != NULL && compute != NULL);
    gchar *canon = _poll_canon_params(params);
    gchar *key = g_strdup_printf("%s\x1f%s", method, canon);
    const gchar *actor = pcv_rpc_completion_actor();
    if (!g_poll_sf)
        g_poll_sf = pcv_singleflight_new(_poll_waiter_free);
    if (pcv_singleflight_waiter_count(g_poll_sf, key) >= PCV_POLL_MAX_WAITERS) {                         
        _poll_refuse_busy(method, key, rpc_id, server, connection, actor ? actor : PCV_AUDIT_ACTOR_LOCAL);
        g_free(key);
        g_free(canon);
        return;
    }

    PollWaiter *w = g_new0(PollWaiter, 1);
    w->rpc_id = g_strdup(rpc_id);
    w->server = g_object_ref(server);
    w->connection = g_object_ref(connection);
    w->actor = g_strdup(actor ? actor : PCV_AUDIT_ACTOR_LOCAL);
    if (!pcv_singleflight_join(g_poll_sf, key, w)) {                              
        g_free(key);
        g_free(canon);
        return;
    }

    PollRun *run = g_new0(PollRun, 1);
    run->key = key;
    run->method = g_strdup(method);
    run->params = json_from_string(canon, NULL);
    run->compute = compute;
    g_free(canon);
    GTask *task = pcv_drain_task_new(NULL, NULL, _poll_async_done, NULL);
    g_task_set_task_data(task, run, _poll_run_free);
    g_task_run_in_thread(task, _poll_async_worker);
    g_object_unref(task);
}

                                                                                        
void
pcv_poll_async_set_spawn_fn_for_test(gboolean (*fn)(const gchar * const *argv, const PcvSpawnBound *b,
                                                    gchar **out, gchar **err, gint *status, GError **error))
{
    g_mutex_lock(&g_pa.mutex);
    g_pa.spawn_fn = fn;
    g_mutex_unlock(&g_pa.mutex);
}

                                                                                
void
pcv_poll_async_set_now_fn_for_test(gint64 (*fn)(void))
{
    g_mutex_lock(&g_pa.mutex);
    g_pa.now_fn = fn;
    g_mutex_unlock(&g_pa.mutex);
}
