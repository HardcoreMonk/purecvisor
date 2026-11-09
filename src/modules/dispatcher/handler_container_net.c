   
                                
                                                                                                                
  
                           
                                                                           
                                                                      
                                                                
                                                   
                                                                                                   
                                                                            
  
                                                     
   

#include "api/dispatcher.h"
#include "api/uds_server.h"
#include "api/snapshot_verify_probe.h"
#include "api/ova_import_xml.h"
#include "api/vm_batch_policy.h"
#include "api/vm_batch_queue.h"
#include "api/daemon_config_policy.h"
#include "bootstrap/pcv_bootstrap.h"
#include "modules/virt/vm_manager.h"
#include "modules/virt/cancellable_map.h"
#include "modules/virt/vm_clone_plan.h"
#include "modules/daemons/prometheus_exporter.h"
#include "modules/daemons/pcv_webpush.h"
#include "modules/audit/pcv_audit.h"
#include "modules/plugin/pcv_plugin_manager.h"
#include "modules/core/cpu_allocator.h"
#include "utils/pcv_secure.h"
#include "purecvisor/version.h"
#include <json-glib/json-glib.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "modules/dispatcher/handler_snapshot.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/handler_vnc.h"
#include "modules/dispatcher/handler_vm_start.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"
#include "modules/dispatcher/handler_vm_inspect.h"
#include "modules/dispatcher/handler_vm_hotplug.h"
#include "modules/network/network_manager.h"
#include "modules/network/vpc/vpc_manager.h"
#include "modules/network/pcv_qos.h"
#include "modules/network/pcv_qos_chaos.h"
#include "modules/daemons/pcv_trace.h"
#include "modules/security/pcv_suricata.h"
#include "modules/security/pcv_suricata_rules.h"
#include "modules/security/pcv_suricata_ips.h"
#include "modules/security/pcv_suricata_ips_rules.h"
#include "modules/dispatcher/handler_storage.h"
#include "modules/dispatcher/handler_monitor.h"
#include "modules/dispatcher/handler_container.h"
#include "modules/dispatcher/handler_container_mode.h"                                                                        
#include "modules/resource/resource_gate.h"                                                                       
#include "modules/dispatcher/handler_overlay.h"
#include "modules/dispatcher/handler_accel.h"
#include "modules/accel/gpu_manager.h"
#include "modules/dispatcher/handler_template.h"
#include "modules/dispatcher/handler_auth.h"
#include "modules/dispatcher/handler_backup.h"
#include "modules/dispatcher/handler_security.h"
#include "modules/dispatcher/handler_tenant_overlay.h"
#include "modules/dispatcher/handler_vpc.h"
#include "modules/daemons/alert_engine.h"
#include "modules/daemons/process_monitor.h"
#include "modules/daemons/update_check.h"
#include "modules/virt/virt_conn_pool.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_config.h"
#include "utils/pcv_jwt.h"
#include "modules/auth/pcv_rbac.h"
#include "api/drain.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "modules/lxc/lxc_driver.h"
#include "modules/storage/zfs_driver.h"
#include "modules/backup/backup_scheduler.h"
#include "modules/ai/workload_predict.h"
#include "utils/pcv_worker_pool.h"
#include "utils/pcv_log.h"
#include "utils/pcv_validate.h"
#include "utils/pcv_job_queue.h"
#include "modules/core/vm_state.h"
#include "api/ws_server.h"
#include <sqlite3.h>
#include <errno.h>
#include "api/dispatcher_policy.h"
#include "api/dispatcher_internal.h"
#include "modules/dispatcher/dispatcher_shared.h"
#include "api/dispatcher_owner_scope.h"
#include "modules/dispatcher/handler_vm_ova.h"
#include "modules/dispatcher/handler_vm_clone.h"
#include "modules/dispatcher/handler_vm_create.h"
#include "modules/dispatcher/handler_vm_admin.h"
#include "modules/dispatcher/handler_suricata.h"
#include "modules/dispatcher/handler_alert.h"
#include "modules/dispatcher/handler_daemon.h"
#include "modules/dispatcher/handler_qos.h"
#include "modules/dispatcher/handler_container_net.h"
#include "api/dispatcher_internal.h"


                          

                                                                                                               
                                                                                             
                                                                                                     
                                                                                
#define CTR_CLONE_LOCK_RENEW_INTERVAL_US   (60 * G_USEC_PER_SEC)

typedef struct {
    gchar     *source;
    gchar     *dest;
    PcvVmLock *source_lock;                                                               
    PcvVmLock *target_lock;                                                                       
    GThread   *lock_renew_thread;                                                          
    GMutex     lock_renew_mu;                                                
    GCond      lock_renew_cond;                             
    gboolean   lock_renew_stop;                                                  
} ContainerCloneAuditCtx;

                                                                                               
                                                       
static void
_container_clone_audit_ctx_free(ContainerCloneAuditCtx *ctx)
{
    if (!ctx) return;
    g_mutex_clear(&ctx->lock_renew_mu);
    g_cond_clear(&ctx->lock_renew_cond);
    g_free(ctx->source);
    g_free(ctx->dest);
    g_free(ctx);
}

                                                                                                  
                                                                                                      
                                                                                                             
                                      
                                                                                                       
                                                                                           
                   
                                   
                                                                                   
static gpointer
_ctr_clone_lock_renew_worker(gpointer data)
{
    ContainerCloneAuditCtx *ctx = data;
    g_mutex_lock(&ctx->lock_renew_mu);
    while (!ctx->lock_renew_stop) {                     
        const gint64 deadline = g_get_monotonic_time() + CTR_CLONE_LOCK_RENEW_INTERVAL_US;
        gboolean signaled = TRUE;
        while (!ctx->lock_renew_stop && signaled)                                                          
            signaled = g_cond_wait_until(&ctx->lock_renew_cond, &ctx->lock_renew_mu, deadline);
        if (ctx->lock_renew_stop) break;
        g_mutex_unlock(&ctx->lock_renew_mu);                                           
        (void)pcv_vm_lock_renew(ctx->source_lock);                                      
        (void)pcv_vm_lock_renew(ctx->target_lock);
        g_mutex_lock(&ctx->lock_renew_mu);
    }
    g_mutex_unlock(&ctx->lock_renew_mu);
    return NULL;
}

                                                                                                              
                                                                                               
                                                                                 
                                         
                                                                                           
                                                                                              
                                                    
                                                                          
static void
_ctr_clone_release_locks(gpointer data)
{
    ContainerCloneAuditCtx *ctx = data;
    g_mutex_lock(&ctx->lock_renew_mu);
    ctx->lock_renew_stop = TRUE;                                           
    g_cond_signal(&ctx->lock_renew_cond);
    g_mutex_unlock(&ctx->lock_renew_mu);
    if (ctx->lock_renew_thread) {                       
        g_thread_join(ctx->lock_renew_thread);
        ctx->lock_renew_thread = NULL;
    }
    pcv_vm_lock_release(ctx->source_lock);                                 
    ctx->source_lock = NULL;
    pcv_vm_lock_release(ctx->target_lock);
    ctx->target_lock = NULL;
}

                                                                                                        
                                                                                              
                  
                                                                   
                                                                                                    
static void
_on_container_clone_done(GObject *src __attribute__((unused)),
                         GAsyncResult *res,
                         gpointer user_data)
{
    ContainerCloneAuditCtx *ctx = user_data;
    GError *error = NULL;
    gboolean ok = pcv_lxc_clone_finish(res, &error);
    gchar *target = g_strdup_printf("%s:%s", ctx->source, ctx->dest);
    gchar *job_id = g_strdup_printf("container.clone:%s", target);

    pcv_audit_log(pcv_drain_task_actor(res), "container.clone", target,
                  ok ? "ok" : "fail", ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_ws_broadcast_job_complete(job_id, "container.clone",
                                  ok ? "completed" : "failed",
                                  ok ? NULL : (error ? error->message : "container clone failed"));

    if (error) g_error_free(error);
    g_free(job_id);
    g_free(target);
    _container_clone_audit_ctx_free(ctx);                                 
}

                                                                                                                     
                                                                                                                        
                                                                               
                                                                  
static gboolean
_clone_bridge_allowed(const gchar *bridge, gpointer data)
{
    if (pcv_res_check(data, PCV_RES_BRIDGE, bridge, NULL, NULL) == PCV_RES_ALLOW)
        return TRUE;
    pcv_res_audit_denied("container.clone", "network_bridge", bridge);
    return FALSE;
}

                                                                                                         
                                                                                                           
                                                                                                  
                               
                                                                                                         
                                                                                                          
                                                                                                      
                                                                              
                                                                              
                                                                                 
               
void handle_container_clone(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    const gchar *src = params ? json_object_get_string_member_with_default(params, "source", NULL) : NULL;
    const gchar *dst = params ? json_object_get_string_member_with_default(params, "dest", NULL) : NULL;
    if (!src || !dst) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "source and dest required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    if (!pcv_validate_vm_name(src) || !pcv_validate_new_container_name(dst) || g_strcmp0(src, dst) == 0) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "invalid source or dest container name");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
                                                                                          
                                                                   
    if (pcv_disp_dispatcher_caller_role(params, connection) < PCV_ROLE_ADMIN && pcv_lxc_container_privileged(src)) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_FORBIDDEN,
            "cloning a privileged container requires admin role");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
                                                                                  
    gchar *lock_err = NULL;
    PcvVmLock *src_lock = pcv_vm_lock_acquire(src, VM_OP_SNAPSHOT, &lock_err);
    if (!src_lock) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            lock_err ? lock_err : "Source container busy (another operation in progress)");
        pure_uds_server_send_response(server, connection, r); g_free(r); g_free(lock_err); return;
    }
    PcvVmLock *dst_lock = pcv_vm_lock_acquire(dst, VM_OP_CREATING, &lock_err);
    if (!dst_lock) {
        pcv_vm_lock_release(src_lock);
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            lock_err ? lock_err : "Target container name busy (another operation in progress)");
        pure_uds_server_send_response(server, connection, r); g_free(r); g_free(lock_err); return;
    }
                                                                                                     
    gchar *config_path = g_build_filename(PCV_LXC_PATH, src, "config", NULL);
    gchar *config_text = NULL;
    if (!g_file_get_contents(config_path, &config_text, NULL, NULL))
        config_text = NULL;
    gint volume_entries = -1;
    gchar *gate_msg = NULL;
    const gint caller_role = pcv_disp_dispatcher_caller_role(params, connection);                                         
    PcvResCaller res_caller = { 0 };                                                             
    pcv_res_caller_init(&res_caller, params, connection);
    const gboolean gate_ok = pcv_container_clone_volume_gate(caller_role, config_text, &volume_entries, &gate_msg) &&
                             pcv_container_clone_bridge_gate(caller_role, config_text, _clone_bridge_allowed, &res_caller, NULL, &gate_msg);
    pcv_res_caller_clear(&res_caller);
    g_free(config_text);
    g_free(config_path);
    if (!gate_ok) {
        pcv_vm_lock_release(src_lock);
        pcv_vm_lock_release(dst_lock);
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_FORBIDDEN, gate_msg);
        pure_uds_server_send_response(server, connection, r); g_free(r); g_free(gate_msg); return;
    }
    ContainerCloneAuditCtx *ctx = g_new0(ContainerCloneAuditCtx, 1);
    ctx->source = g_strdup(src);
    ctx->dest = g_strdup(dst);
    ctx->source_lock = src_lock;                                         
    ctx->target_lock = dst_lock;
    g_mutex_init(&ctx->lock_renew_mu);                                
    g_cond_init(&ctx->lock_renew_cond);
                                                                                                    
    ctx->lock_renew_thread = g_thread_try_new("ctr-clone-renew", _ctr_clone_lock_renew_worker, ctx, NULL);
    if (!ctx->lock_renew_thread)
        PCV_LOG_WARN("container", "clone '%s' -> '%s': lock renew thread not started; locks may expire during a long copy",
                     src, dst);
                                               
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "source", src);
    json_object_set_string_member(res, "dest", dst);
    json_object_set_string_member(res, "status", "accepted");
    if (volume_entries >= 0)                                                                  
        json_object_set_int_member(res, "volume_entries", volume_entries);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
                                                     
                                                                                       
                                                               
    pcv_lxc_clone_async(src, dst, json_object_get_string_member_with_default(params, "_pcv_caller_sub", NULL),
                        _ctr_clone_release_locks, ctx, NULL, _on_container_clone_done, ctx);
}

                                                      
                                                          
void handle_container_memory_stats(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
                                  
    gchar *path = g_strdup_printf("/sys/fs/cgroup/lxc.payload.%s/memory.stat", name);
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "container", name);
    gchar *contents = nullptr;
    if (g_file_get_contents(path, &contents, NULL, NULL) && contents) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i] && lines[i][0]; i++) {
            gchar **kv = g_strsplit(lines[i], " ", 2);
            if (kv[0] && kv[1])
                json_object_set_int_member(res, kv[0], g_ascii_strtoll(kv[1], NULL, 10));
            g_strfreev(kv);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(path);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

                                                
                                                          
void handle_container_health_check(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!name || !pcv_validate_vm_name(name)) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
                                                                    
                                                              
    const gchar *argv[] = {"lxc-info", "-P", PCV_LXC_PATH, "-n", name, "-sH", NULL};
    gchar *out = nullptr;
    gchar *stderr_out = nullptr;
    GError *probe_error = nullptr;
    gboolean probe_ok = pcv_spawn_sync(argv, &out, &stderr_out, &probe_error);
    gboolean running = probe_ok && out && strstr(out, "RUNNING");
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "container", name);
    json_object_set_string_member(res, "state", running ? "healthy" : "unhealthy");
    json_object_set_boolean_member(res, "running", running);
    if (!probe_ok) {
        const gchar *detail = probe_error && probe_error->message
            ? probe_error->message
            : (stderr_out && *stderr_out ? stderr_out : "lxc-info failed");
        json_object_set_string_member(res, "detail", detail);
    }
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r);
    g_free(r); g_free(out); g_free(stderr_out); g_clear_error(&probe_error);
}

                                                                        
                                     
  
                                                        
                                                    
  
              
                                                             
                                   
  
                  
                                                                      
                                                  
                                                            
  
         
                       
                                             
                                       
                                                                        
void handle_container_set_limits(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *ctr_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    gint cpu_pct = json_object_has_member(params, "cpu_percent")
        ? (gint)json_object_get_int_member(params, "cpu_percent") : 0;
    gint mem_mb = json_object_has_member(params, "memory_mb")
        ? (gint)json_object_get_int_member(params, "memory_mb") : 0;
    gint cpu_wt = json_object_has_member(params, "cpu_weight")
        ? (gint)json_object_get_int_member(params, "cpu_weight") : 0;
    gint mem_low = json_object_has_member(params, "memory_low_mb")
        ? (gint)json_object_get_int_member(params, "memory_low_mb") : 0;
    gint mem_high = json_object_has_member(params, "memory_high_mb")
        ? (gint)json_object_get_int_member(params, "memory_high_mb") : 0;
    gint64 io_rbps = json_object_has_member(params, "io_read_bps")
        ? json_object_get_int_member(params, "io_read_bps") : 0;
    gint pids = json_object_has_member(params, "pids_max")
        ? (gint)json_object_get_int_member(params, "pids_max") : 0;

    if (!ctr_name) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required parameter: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (cpu_pct <= 0 && mem_mb <= 0 && cpu_wt <= 0 && mem_low <= 0 &&
        mem_high <= 0 && io_rbps <= 0 && pids <= 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "At least one limit parameter must be > 0");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (cpu_wt < 0 || cpu_wt > 10000) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "cpu_weight must be 1-10000");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (mem_low < 0 || mem_high < 0 || io_rbps < 0 || pids < 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Limit values must be >= 0");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    if (!pcv_validate_vm_name(ctr_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid container name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
                                                                                             
                                                                                      
    gchar *lock_err = nullptr;
    PcvVmLock *op_lock = pcv_vm_lock_acquire(ctr_name, VM_OP_TUNING, &lock_err);
    if (!op_lock) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY, lock_err ? lock_err : "Container is busy");
        pure_uds_server_send_response(server, connection, e); g_free(e); g_free(lock_err);
        return;
    }

    GError *cg_err = nullptr;
    const gboolean limits_ok = pcv_lxc_set_resource_limits(ctr_name, cpu_pct, mem_mb, cpu_wt,
                                                           mem_low, mem_high, io_rbps, pids, &cg_err);
    gchar *state = limits_ok ? pcv_lxc_get_state(ctr_name) : nullptr;                                             
    pcv_vm_lock_release(op_lock);
    if (limits_ok) {
        const gchar *applied = (state && g_strcmp0(state, "RUNNING") == 0)
            ? "live" : "config";
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "container", ctr_name);
        if (cpu_pct > 0)
            json_object_set_int_member(obj, "cpu_percent", cpu_pct);
        if (mem_mb > 0)
            json_object_set_int_member(obj, "memory_mb", mem_mb);
        if (cpu_wt > 0)
            json_object_set_int_member(obj, "cpu_weight", cpu_wt);
        if (mem_low > 0)
            json_object_set_int_member(obj, "memory_low_mb", mem_low);
        if (mem_high > 0)
            json_object_set_int_member(obj, "memory_high_mb", mem_high);
        if (io_rbps > 0)
            json_object_set_int_member(obj, "io_read_bps", io_rbps);
        if (pids > 0)
            json_object_set_int_member(obj, "pids_max", pids);
        json_object_set_string_member(obj, "applied", applied);
        g_free(state);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            cg_err ? cg_err->message : "Failed to set resource limits");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        if (cg_err) g_error_free(cg_err);
    }
}

                                                                                     
                           
                                                                           
                                                                  
                                                                                               
                                                                              
                                                                             
                                                                                
                                                        
                                                                                    
                                                                  
                                                                                     
                                               
                                                            
                                                                 
typedef enum { CNET_NIC_LIST, CNET_NIC_ATTACH, CNET_NIC_DETACH, CNET_SET_BANDWIDTH } CnetKind;

typedef struct {
    UdsServer         *server;
    GSocketConnection *connection;
    gchar             *rpc_id;
    CnetKind           kind;
    gchar             *name;                        
    gchar             *nic;                                                                   
    gchar             *bridge;                               
    gchar             *hwaddr;                           
    guint              in_kbps;
    guint              out_kbps;
                                                  
    JsonNode          *result;                                        
    gint               code;                                      
    gchar             *message;               
} CnetCtx;

                                                                                   
                                           
static void
_cnet_ctx_free(gpointer data)
{
    CnetCtx *ctx = data;
    if (!ctx) return;
    g_free(ctx->rpc_id);
    g_free(ctx->name);
    g_free(ctx->nic);
    g_free(ctx->bridge);
    g_free(ctx->hwaddr);
    if (ctx->result) json_node_unref(ctx->result);
    g_free(ctx->message);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

                                                       
                                        
static void
_cnet_fail(CnetCtx *ctx, gint code, const GError *e, const gchar *fallback)
{
    ctx->code = code;
    ctx->message = g_strdup(e ? e->message : fallback);
}

                                                                    
static void
_cnet_object_result(CnetCtx *ctx, JsonObject *obj)
{
    ctx->result = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(ctx->result, obj);
}

                                                                      
                                                                             
static void
_cnet_nic_list(CnetCtx *ctx)
{
    GError *e = NULL;
    GPtrArray *nics = pcv_lxc_nic_list(ctx->name, &e);
    JsonArray *arr = json_array_new();
    for (guint i = 0; nics && i < nics->len; i++) {
        PcvLxcNicInfo *ni = g_ptr_array_index(nics, i);
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "name",   ni->name   ?: "");
        json_object_set_string_member(o, "type",   ni->type   ?: "veth");
        json_object_set_string_member(o, "bridge", ni->bridge ?: "");
        json_object_set_string_member(o, "hwaddr", ni->hwaddr ?: "");
        json_object_set_string_member(o, "ipv4",   ni->ipv4   ?: "");
        json_object_set_boolean_member(o, "ipv4_unavailable", ni->ipv4_unavailable);
        json_array_add_object_element(arr, o);
    }
    if (nics) g_ptr_array_unref(nics);
    g_clear_error(&e);
    ctx->result = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(ctx->result, arr);
}

                                                                               
static void
_cnet_nic_attach(CnetCtx *ctx)
{
    GError *e = NULL;
    if (!pcv_lxc_nic_attach(ctx->name, ctx->bridge, ctx->hwaddr, &e)) {
        _cnet_fail(ctx, PURE_RPC_ERR_ZFS_OPERATION, e, "NIC attach failed");
        g_clear_error(&e);
        return;
    }
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "status", "attached");
    json_object_set_string_member(obj, "container", ctx->name);
    json_object_set_string_member(obj, "bridge", ctx->bridge ?: PCV_LXC_DEFAULT_BRIDGE);
    _cnet_object_result(ctx, obj);
}

                                                                                      
static void
_cnet_nic_detach(CnetCtx *ctx)
{
    GError *e = NULL;
    gchar *warning = NULL;
    if (!pcv_lxc_nic_detach(ctx->name, ctx->nic, &warning, &e)) {
        _cnet_fail(ctx, PURE_RPC_ERR_ZFS_OPERATION, e, "NIC detach failed");
        g_clear_error(&e);
        return;
    }
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "status", "detached");
    json_object_set_string_member(obj, "nic", ctx->nic);
    if (warning)
        json_object_set_string_member(obj, "warning", warning);
    g_free(warning);
    _cnet_object_result(ctx, obj);
}

                                                                             
static void
_cnet_set_bandwidth(CnetCtx *ctx)
{
    GError *e = NULL;
    if (!pcv_lxc_set_bandwidth(ctx->name, ctx->nic, ctx->in_kbps, ctx->out_kbps, &e)) {
        _cnet_fail(ctx, PURE_RPC_ERR_ZFS_OPERATION, e, "Bandwidth set failed");
        g_clear_error(&e);
        return;
    }
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "status", "applied");
    json_object_set_string_member(obj, "container", ctx->name);
    json_object_set_int_member(obj, "inbound_kbps", ctx->in_kbps);
    json_object_set_int_member(obj, "outbound_kbps", ctx->out_kbps);
    _cnet_object_result(ctx, obj);
}

                                                                       
                                                               
static void
_cnet_worker(GTask *task, gpointer src, gpointer data, GCancellable *cancellable)
{
    (void)src; (void)cancellable;
    CnetCtx *ctx = data;
    PcvVmLock *lock = NULL;
    if (ctx->kind != CNET_NIC_LIST) {
        gchar *lock_err = NULL;
        lock = pcv_vm_lock_acquire(ctx->name, VM_OP_TUNING, &lock_err);
        if (!lock) {
            ctx->code = PURE_RPC_ERR_BUSY;
            ctx->message = lock_err ? lock_err : g_strdup("Container is busy");
            g_task_return_boolean(task, FALSE);
            return;
        }
    }
    switch (ctx->kind) {
    case CNET_NIC_LIST:      _cnet_nic_list(ctx);      break;
    case CNET_NIC_ATTACH:    _cnet_nic_attach(ctx);    break;
    case CNET_NIC_DETACH:    _cnet_nic_detach(ctx);    break;
    case CNET_SET_BANDWIDTH: _cnet_set_bandwidth(ctx); break;
    }
    g_clear_pointer(&lock, pcv_vm_lock_release);
    g_task_return_boolean(task, ctx->code == 0);
}

                                                                             
                                                    
static void
_cnet_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    CnetCtx *ctx = data;
    (void)g_task_propagate_boolean(G_TASK(res), NULL);
    gchar *r = ctx->code ? pure_rpc_build_error_response(ctx->rpc_id, ctx->code, ctx->message)
                         : pure_rpc_build_success_response(ctx->rpc_id, g_steal_pointer(&ctx->result));
    pure_uds_server_send_response(ctx->server, ctx->connection, r);
    g_free(r);
    const gchar *outcome = ctx->code ? "fail" : "ok";
    switch (ctx->kind) {
    case CNET_NIC_LIST:
        pcv_audit_log(pcv_drain_task_actor(res), "container.nic.list", ctx->name, outcome, ctx->code, 0, "local");
        break;
    case CNET_NIC_ATTACH:
        pcv_audit_log(pcv_drain_task_actor(res), "container.nic.attach", ctx->name, outcome, ctx->code, 0, "local");
        break;
    case CNET_NIC_DETACH:
        pcv_audit_log(pcv_drain_task_actor(res), "container.nic.detach", ctx->name, outcome, ctx->code, 0, "local");
        break;
    case CNET_SET_BANDWIDTH:
        pcv_audit_log(pcv_drain_task_actor(res), "container.set_bandwidth", ctx->name, outcome, ctx->code, 0, "local");
        break;
    }
}

                                                               
                                                          
static void
_cnet_start(CnetCtx *ctx, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
    ctx->rpc_id = g_strdup(rpc_id);
    GTask *task = pcv_drain_task_new(NULL, NULL, _cnet_done, ctx);
    g_task_set_task_data(task, ctx, _cnet_ctx_free);
    pcv_worker_pool_push(task, _cnet_worker);
    g_object_unref(task);
}

                                         
static CnetCtx *
_cnet_new(CnetKind kind, const gchar *name)
{
    CnetCtx *ctx = g_new0(CnetCtx, 1);
    ctx->kind = kind;
    ctx->name = g_strdup(name);
    return ctx;
}

                                                                      
                                                                  
                                               
                                                                   
void handle_container_nic_list(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    const gchar *ctr_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
                                                                                       
    if (!ctr_name || !pcv_validate_vm_name(ctr_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid or missing parameter: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    _cnet_start(_cnet_new(CNET_NIC_LIST, ctr_name), rpc_id, server, connection);
}

                                                                      
                                                                   
                                                                            
                                                                             
                                                                                                                   
                                                      
void handle_container_nic_attach(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *ctr_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *bridge = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    const gchar *mac = pcv_rpc_params_get_string(params, "hwaddr");                                             
                                                                   
                                                                  
                                                    
    if (!ctr_name || !pcv_validate_vm_name(ctr_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid or missing parameter: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else if (bridge && !pcv_validate_bridge_name(bridge)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid parameter: bridge");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else if (json_object_has_member(params, "hwaddr") && (!mac || !pcv_validate_mac(mac))) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid parameter: hwaddr (xx:xx:xx:xx:xx:xx)");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else if (pcv_vpc_bridge_is_managed(bridge ?: PCV_LXC_DEFAULT_BRIDGE)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "VPC-managed bridges cannot be attached through container.nic.attach");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else {
        CnetCtx *ctx = _cnet_new(CNET_NIC_ATTACH, ctr_name);
        ctx->bridge = g_strdup(bridge);
        ctx->hwaddr = g_strdup(mac);
        _cnet_start(ctx, rpc_id, server, connection);
    }
}

                                                                      
                                                                
                                                                                    
                                                                 
void handle_container_nic_detach(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *ctr_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *nic_name = json_object_has_member(params, "nic_name")
        ? json_object_get_string_member(params, "nic_name") : NULL;
                                                                      
                                                                 
                                      
    if (!ctr_name || !pcv_validate_vm_name(ctr_name) ||
        !nic_name || !pcv_validate_iface_name(nic_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid or missing parameters: name, nic_name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    CnetCtx *ctx = _cnet_new(CNET_NIC_DETACH, ctr_name);
    ctx->nic = g_strdup(nic_name);
    _cnet_start(ctx, rpc_id, server, connection);
}

                                                                      
                                                                    
                                                                  
void handle_container_set_bandwidth(JsonObject *params, const gchar *rpc_id,
                                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *ctr_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *nic = json_object_has_member(params, "nic_name")
        ? json_object_get_string_member(params, "nic_name") : NULL;
    guint in_kbps = json_object_has_member(params, "inbound_kbps")
        ? (guint)json_object_get_int_member(params, "inbound_kbps") : 0;
    guint out_kbps = json_object_has_member(params, "outbound_kbps")
        ? (guint)json_object_get_int_member(params, "outbound_kbps") : 0;
                                                                  
                                                             
    if (!ctr_name || !pcv_validate_vm_name(ctr_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid or missing parameter: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else if (nic && !pcv_validate_iface_name(nic)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid parameter: nic_name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else {
        CnetCtx *ctx = _cnet_new(CNET_SET_BANDWIDTH, ctr_name);
        ctx->nic = g_strdup(nic);
        ctx->in_kbps = in_kbps;
        ctx->out_kbps = out_kbps;
        _cnet_start(ctx, rpc_id, server, connection);
    }
}
