                                                                             
                                       
                                                                            
  
                           
                                                                                                        
                                                                                                  
                                        
                                                                                           
                                                                                          
                                                                                                                                                                
                                                                                       
               
                                                                                         
  
                                                                                      
                                     
                                                                                
#include "rpc_offload.h"

#include "api/drain.h"
#include "modules/dispatcher/poll_async.h"
#include "modules/dispatcher/rpc_utils.h"
#include "utils/pcv_log.h"

#define LOG_DOM "rpc_offload"
#define PCV_OFFLOAD_SHUTTING_DOWN_MSG "server is shutting down"                            

                                                                
static const struct { const gchar *method; PcvOffloadLane lane; } g_table[] = {
                                                              
    { "monitor.fleet", PCV_OFFLOAD_LANE_LIBVIRT_READ },
    { "quota.get", PCV_OFFLOAD_LANE_LIBVIRT_READ },
    { "vm.cpu.stats", PCV_OFFLOAD_LANE_LIBVIRT_READ },
    { "vm.memory.stats", PCV_OFFLOAD_LANE_LIBVIRT_READ },
    { "vm.blkio.get", PCV_OFFLOAD_LANE_LIBVIRT_READ },
    { "vm.usb.list", PCV_OFFLOAD_LANE_LIBVIRT_READ },
    { "device.nic.list", PCV_OFFLOAD_LANE_LIBVIRT_READ },
    { "vm.vnc", PCV_OFFLOAD_LANE_LIBVIRT_READ },
    { "get_vnc_info", PCV_OFFLOAD_LANE_LIBVIRT_READ },
    { "qos.vm.get", PCV_OFFLOAD_LANE_LIBVIRT_READ },
    { "resource.unregistered.list", PCV_OFFLOAD_LANE_LIBVIRT_READ },
                                                                
    { "dpdk.list", PCV_OFFLOAD_LANE_HOST_READ },
    { "dpdk.status", PCV_OFFLOAD_LANE_HOST_READ },
    { "gpu.list", PCV_OFFLOAD_LANE_HOST_READ },
    { "gpu.metrics", PCV_OFFLOAD_LANE_HOST_READ },
    { "sriov.list", PCV_OFFLOAD_LANE_HOST_READ },
    { "backup.history", PCV_OFFLOAD_LANE_HOST_READ },
    { "container.env.list", PCV_OFFLOAD_LANE_HOST_READ },
    { "container.volume.list", PCV_OFFLOAD_LANE_HOST_READ },
    { "network.host.info", PCV_OFFLOAD_LANE_HOST_READ },
    { "network.info", PCV_OFFLOAD_LANE_HOST_READ },
    { "network.qos.get", PCV_OFFLOAD_LANE_HOST_READ },
    { "storage.pool.health", PCV_OFFLOAD_LANE_HOST_READ },
                                                                                
    { "vm.snapshot.list", PCV_OFFLOAD_LANE_HOST_READ },
                                                                                            
    { "push.subscribe", PCV_OFFLOAD_LANE_HOST_READ },
                                                                                               
    { "security_group.create", PCV_OFFLOAD_LANE_NET_MUTATE },
    { "security_group.delete", PCV_OFFLOAD_LANE_NET_MUTATE },
    { "security_group.detach", PCV_OFFLOAD_LANE_NET_MUTATE },
    { "security_group.rule.add", PCV_OFFLOAD_LANE_NET_MUTATE },
    { "security_group.rule.remove", PCV_OFFLOAD_LANE_NET_MUTATE },
    { "vm.security_group.set", PCV_OFFLOAD_LANE_NET_MUTATE },
    { "dpdk.bind", PCV_OFFLOAD_LANE_NET_MUTATE },
    { "dpdk.unbind", PCV_OFFLOAD_LANE_NET_MUTATE },
                                                      
    { "storage.pool.create", PCV_OFFLOAD_LANE_STORAGE_MUTATE },
                                                                 
    { "vm.snapshot.create", PCV_OFFLOAD_LANE_STORAGE_MUTATE },
    { "vm.snapshot.delete", PCV_OFFLOAD_LANE_STORAGE_MUTATE },
    { "vm.snapshot.delete_all", PCV_OFFLOAD_LANE_STORAGE_MUTATE },
                                                                                                       
    { "container.volume.attach", PCV_OFFLOAD_LANE_CTR_MUTATE },
    { "container.volume.detach", PCV_OFFLOAD_LANE_CTR_MUTATE },
    { NULL, 0 },
};

                                            
static const struct { gint threads; guint queue_cap; gboolean stale_reject; } g_lane_conf[PCV_OFFLOAD_LANE_COUNT] = {
    [PCV_OFFLOAD_LANE_LIBVIRT_READ]   = { 2, 32, FALSE },
    [PCV_OFFLOAD_LANE_HOST_READ]      = { 2, 32, FALSE },
    [PCV_OFFLOAD_LANE_NET_MUTATE]     = { 1, 8, TRUE },
    [PCV_OFFLOAD_LANE_STORAGE_MUTATE] = { 1, 8, TRUE },
    [PCV_OFFLOAD_LANE_CTR_MUTATE]     = { 1, 8, TRUE },
};

                                                                
typedef struct {
    gchar                   *method, *rpc_id, *req_id, *actor, *target;
    gchar                   *response;                                  
    gint64                   start_us;
    gint64                   queued_us;                           
    PcvOffloadLane           lane;
    PcvRpcOffloadHandler     handler;
    PcvRpcOffloadOwnerCheck  owner_check;
    PcvRpcOffloadFinish      finish;
    JsonParser              *parser;                                      
    JsonObject              *params;
    UdsServer               *server;
    GSocketConnection       *connection;
    PcvRpcCompletionResult   completion;
} OffloadReq;

static GThreadPool *g_lanes[PCV_OFFLOAD_LANE_COUNT];
static GPrivate     g_capture = G_PRIVATE_INIT(NULL);
static GHashTable  *g_test_routes;                                                          
static gint64     (*g_now_fn)(void);                                                      
static gboolean   (*g_terminating_fn)(void);                                                  
static void       (*g_after_job_fn)(void);                                                   

                                                                           
static void
_req_free(gpointer data)
{
    OffloadReq *r = data;
    g_free(r->method);
    g_free(r->rpc_id);
    g_free(r->req_id);
    g_free(r->actor);
    g_free(r->target);
    g_free(r->response);
    g_clear_object(&r->parser);
    g_clear_object(&r->server);
    g_clear_object(&r->connection);
    g_free(r);
}

                                                         
gint
pcv_rpc_offload_lane_of(const gchar *method)
{
    if (!method)
        return -1;
    for (guint i = 0; g_table[i].method; i++) {
        if (g_strcmp0(g_table[i].method, method) == 0)
            return (gint)g_table[i].lane;
    }
    if (g_test_routes) {
        gpointer v = g_hash_table_lookup(g_test_routes, method);
        if (v)
            return GPOINTER_TO_INT(v) - 1;
    }
    return -1;
}

                                                                       
gboolean
pcv_rpc_offload_capture(const gchar *response)
{
    OffloadReq *r = g_private_get(&g_capture);
    if (!r)
        return FALSE;
    if (!r->response)
        r->response = g_strdup(response);
    else
        PCV_LOG_WARN(LOG_DOM, "%s: second response ignored", r->method);
    return TRUE;
}

                                                                                  
                                           
static void
_req_refuse(OffloadReq *r, PureRpcErrorCode code, const gchar *message)
{
    gchar *resp = pure_rpc_build_error_response(r->rpc_id, code, message);
    pcv_rpc_offload_capture(resp);
    g_free(resp);
}

                                                                                                
                                                                      
static void
_req_replace_with_error(OffloadReq *r, const gchar *message)
{
    g_clear_pointer(&r->response, g_free);
    _req_refuse(r, PURE_RPC_ERR_ZFS_OPERATION, message);
}

                                                                                                      
                                             
static void
_req_apply_host_outcome(OffloadReq *r, const PcvPollGuard *guard)
{
    if (pcv_poll_guard_host_aborted(guard)) {
        _req_replace_with_error(r, PCV_OFFLOAD_SHUTTING_DOWN_MSG);
    } else if (pcv_poll_guard_host_stalled(guard)) {
        gchar *msg = g_strdup_printf("%s: host command timed out", r->method);
        _req_replace_with_error(r, msg);
        g_free(msg);
    }
}

                                                                               
                                                  
static gboolean
_req_is_stale(const OffloadReq *r)
{
    if (!g_lane_conf[r->lane].stale_reject)
        return FALSE;
    gint64 now = g_now_fn ? g_now_fn() : g_get_monotonic_time();
    return now - r->queued_us > (gint64)PCV_OFFLOAD_MUTATE_MAX_WAIT_SEC * G_USEC_PER_SEC;
}

                                                                    
                                                                                                                                           
                                      
static void
_lane_worker(gpointer data, gpointer unused)
{
    (void)unused;
    GTask *task = data;
    OffloadReq *r = g_task_get_task_data(task);

    pcv_log_req_id_set(r->req_id);
    pcv_rpc_completion_begin(r->method);
    pcv_rpc_completion_set_actor(r->actor);
    g_private_set(&g_capture, r);

    if (g_terminating_fn ? g_terminating_fn() : pcv_drain_is_terminating()) {
        _req_refuse(r, PURE_RPC_ERR_ZFS_OPERATION, PCV_OFFLOAD_SHUTTING_DOWN_MSG);
    } else if (_req_is_stale(r)) {
        gchar *msg = g_strdup_printf("%s: queued too long", r->method);
        _req_refuse(r, PURE_RPC_ERR_BUSY, msg);
        g_free(msg);
    } else {
                                                                             
        PcvPollGuard *guard = r->lane == PCV_OFFLOAD_LANE_HOST_READ ? pcv_poll_guard_new(r->method) : NULL;
        if (guard)
            pcv_spawn_set_thread_override(pcv_poll_guard_spawn_override, guard);
        gboolean permitted = !r->owner_check ||
            r->owner_check(r->method, r->params, r->rpc_id, r->server, r->connection, r->actor);
        if (permitted)
            r->handler(r->params, r->rpc_id, r->server, r->connection);
        if (guard) {
            pcv_spawn_set_thread_override(NULL, NULL);
            if (permitted && r->response)
                _req_apply_host_outcome(r, guard);
            pcv_poll_guard_free(guard);
        }
    }
    if (g_after_job_fn)
        g_after_job_fn();

    g_private_set(&g_capture, NULL);
    r->completion = pcv_rpc_completion_finish();
    pcv_log_req_id_set(NULL);
    g_task_return_boolean(task, TRUE);
    g_object_unref(task);
}

                                                                                       
                                       
static void
_offload_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source; (void)user_data;
    OffloadReq *r = g_task_get_task_data(G_TASK(res));
    if (r->response)
        pure_uds_server_send_response(r->server, r->connection, r->response);
    r->finish(r->method, r->rpc_id, r->actor, r->target, r->start_us, r->completion);
}

                                                                     
gboolean
pcv_rpc_offload_submit(const PcvRpcOffloadRequest *req)
{
    g_return_val_if_fail(req && req->method && req->handler && req->finish, FALSE);
    gint lane = pcv_rpc_offload_lane_of(req->method);
    if (lane < 0 || lane >= PCV_OFFLOAD_LANE_COUNT)
        return FALSE;
    if (!g_lanes[lane]) {
        g_lanes[lane] = g_thread_pool_new(_lane_worker, NULL, g_lane_conf[lane].threads, FALSE, NULL);
        if (!g_lanes[lane])
            return FALSE;
    }
    if (g_thread_pool_unprocessed(g_lanes[lane]) >= g_lane_conf[lane].queue_cap)
        return FALSE;

    OffloadReq *r = g_new0(OffloadReq, 1);
    r->method = g_strdup(req->method);
    r->rpc_id = g_strdup(req->rpc_id);
    r->req_id = g_strdup(req->req_id);
    r->actor = g_strdup(req->actor);
    r->target = g_strdup(req->target);
    r->start_us = req->start_us;
    r->queued_us = g_now_fn ? g_now_fn() : g_get_monotonic_time();
    r->lane = (PcvOffloadLane)lane;
    r->handler = req->handler;
    r->owner_check = req->owner_check;
    r->finish = req->finish;
    r->parser = req->parser ? g_object_ref(req->parser) : NULL;
    r->params = req->params;
    r->server = req->server ? g_object_ref(req->server) : NULL;
    r->connection = req->connection ? g_object_ref(req->connection) : NULL;

    GTask *task = pcv_drain_task_new(NULL, NULL, _offload_done, NULL);
    g_task_set_task_data(task, r, _req_free);
    g_thread_pool_push(g_lanes[lane], task, NULL);
    return TRUE;
}

                                                                                                   
void
pcv_rpc_offload_shutdown(void)
{
    for (gint i = 0; i < PCV_OFFLOAD_LANE_COUNT; i++) {
        if (g_lanes[i]) {
            g_thread_pool_free(g_lanes[i], FALSE, TRUE);
            g_lanes[i] = NULL;
        }
    }
}

                                                                                
void
pcv_rpc_offload_add_for_test(const gchar *method, gint lane)
{
    if (lane < 0) {
        g_clear_pointer(&g_test_routes, g_hash_table_destroy);
        return;
    }
    if (!g_test_routes)
        g_test_routes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_insert(g_test_routes, g_strdup(method), GINT_TO_POINTER(lane + 1));
}

                                                   
void
pcv_rpc_offload_set_now_fn_for_test(gint64 (*fn)(void))
{
    g_now_fn = fn;
}

                                                              
void
pcv_rpc_offload_set_after_job_fn_for_test(void (*fn)(void))
{
    g_after_job_fn = fn;
}

                                                  
void
pcv_rpc_offload_set_terminating_fn_for_test(gboolean (*fn)(void))
{
    g_terminating_fn = fn;
}
