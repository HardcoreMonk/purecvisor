   
                     
                                                           
  
                           
                                                   
                                                    
                                        
  
          
                                               
                                                 
                                                
                                               
                                                
                                               
                                               
                                     
  
           
                                                                     
                                                 
  
                                                         
  
         
                                                                           
                                                                     
                                                                         
                                                                 
                                                        
                                                                
                                                                  
  
          
                                                                
                                                               
                                                        
                                                                   
  
                                  
                                                                     
                                                                              
  
             
                                              
                                                 
                                                    
                                                             
                                                    
  
                         
                               
                                         
                                            
                                                          
                                       
                                                            
  
            
                                                                 
  
                                             
                                             
  
           
                                           
                                             
                                                     
   

#include "dispatcher.h"
#include "../modules/dispatcher/rpc_offload.h"                            
#include "uds_server.h"
#include "snapshot_verify_probe.h"                                                  
#include "ova_import_xml.h"                                                
#include "vm_batch_policy.h"                                                           
#include "vm_batch_queue.h"                                 
#include "daemon_config_policy.h"                                                   
#include "bootstrap/pcv_bootstrap.h"
#include "../modules/virt/vm_manager.h"
#include "../modules/virt/cancellable_map.h"                                     
#include "../modules/virt/vm_clone_plan.h"
#include "../modules/daemons/prometheus_exporter.h"
#include "../modules/daemons/pcv_webpush.h"                                  
#include "../modules/audit/pcv_audit.h"
#include "../modules/plugin/pcv_plugin_manager.h"
#include "../modules/core/cpu_allocator.h"                                                             
#include "../utils/pcv_secure.h"                              
#include "purecvisor/version.h"
#include <json-glib/json-glib.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "../modules/dispatcher/handler_snapshot.h"
#include "../modules/dispatcher/rpc_utils.h"
#include "../modules/dispatcher/rpc_completion.h"                                  
#include "../utils/pcv_mainloop_guard.h"                                   
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
#include "modules/dispatcher/handler_container_health.h"                                        
#include "modules/dispatcher/handler_overlay.h"
#include "modules/dispatcher/handler_accel.h"
#include "modules/accel/gpu_manager.h"
#include "modules/dispatcher/handler_template.h"
#include "modules/dispatcher/handler_auth.h"
#include "modules/dispatcher/handler_resource.h"                                            
#include "modules/dispatcher/handler_host_device.h"
#include "modules/dispatcher/handler_backup.h"
#include "modules/dispatcher/handler_replication_hostkey.h"
#include "modules/dispatcher/handler_security.h"
#include "modules/dispatcher/handler_tenant_overlay.h"
#include "modules/dispatcher/handler_vpc.h"
#include "modules/daemons/alert_engine.h"
#include "modules/daemons/process_monitor.h"
#include "modules/daemons/update_check.h"
#include "../modules/virt/virt_conn_pool.h"
#include "../utils/pcv_spawn.h"
#include "../utils/pcv_config.h"
#include "../utils/pcv_jwt.h"                                                 
#include "../modules/auth/pcv_rbac.h"
#include "utils/pcv_config.h"
#include "drain.h"
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
#include "modules/core/cpu_allocator.h"                                               
#include "modules/ai/workload_predict.h"                                
#include "utils/pcv_spawn.h"
#include "utils/pcv_config.h"
#include "utils/pcv_worker_pool.h"
#include "../utils/pcv_log.h"
#include "../utils/pcv_validate.h"
#include "utils/pcv_job_queue.h"
#include "modules/core/vm_state.h"                                                 
#include "ws_server.h"
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
#include "modules/dispatcher/handler_push.h"
#include "modules/dispatcher/handler_security_group.h"
#include "modules/dispatcher/handler_jobs.h"
#include "modules/dispatcher/handler_host_query.h"
#include "modules/dispatcher/handler_snapshot.h"

                                                                  
                                                                                  
                                                                            
                                                            
                                                           
                                                                        

                                                             
                                        
                               
                                                                        
                                                
void handle_vm_limit_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
                      
void handle_network_list_request    (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_network_host_info_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_network_info_request    (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_network_mode_set_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);


                                                       
void handle_vm_vnc_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_device_nic_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_device_nic_attach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_device_nic_detach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_vm_eject_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                               
                              
                                                               
                                                      
                                                                        
                                                                     
typedef void (*PcvDispatchHandler)(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);

static GHashTable *g_rpc_routes = nullptr;                                          
                                                           
static GHashTable *g_async_methods = nullptr;

                                     
                                               
                                                        
                                                                      
                                                                               
gboolean pcv_dispatcher_is_async_method(const gchar *method);
gboolean pcv_dispatcher_is_async_method(const gchar *method) {
    return method && g_async_methods && g_hash_table_contains(g_async_methods, method);
}

                                                            
                                                 
                                   
                                                          
typedef gboolean (*PcvDispatchHook)(const gchar *method, JsonObject *params,
                                     const gchar *rpc_id, gpointer user_data);

typedef struct {
    PcvDispatchHook hook;
    gpointer        user_data;
} _HookEntry;

static GPtrArray *g_pre_hooks = nullptr;                      

                                                               
                                                        
                                         
                                                                       
                                                                   
void
pcv_dispatcher_register_pre_hook(PcvDispatchHook hook, gpointer user_data)
{
    if (!g_pre_hooks)
        g_pre_hooks = g_ptr_array_new_with_free_func(g_free);
    _HookEntry *entry = g_new0(_HookEntry, 1);
    entry->hook = hook;
    entry->user_data = user_data;
    g_ptr_array_add(g_pre_hooks, entry);
}

                                             
                                                       
                                                     
static gboolean
_run_pre_hooks(const gchar *method, JsonObject *params, const gchar *rpc_id)
{
    if (!g_pre_hooks) return TRUE;
    for (guint i = 0; i < g_pre_hooks->len; i++) {
        _HookEntry *entry = g_ptr_array_index(g_pre_hooks, i);
        if (!entry->hook(method, params, rpc_id, entry->user_data))
            return FALSE;                 
    }
    return TRUE;
}

                                                    
static void dispatcher_init_routes(void);

                                                               
                                                                                       
                                          
                                              
                                                                        
G_DEFINE_TYPE(PureCVisorDispatcher, purecvisor_dispatcher, G_TYPE_OBJECT)

   
                               
  
                                                 
                                            
                                                      
   
typedef struct {
    PureCVisorDispatcher *dispatcher;
    gint request_id;
    UdsServer *server;
    GSocketConnection *connection;
} DispatcherRequestContext;

   
                                                   
  
                                                 
                                              
  
                                                         
                                           
                                        
   
static void dispatcher_request_context_free(DispatcherRequestContext *ctx) {
    if (ctx->dispatcher) g_object_unref(ctx->dispatcher);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}




                              

   
                                        
                   
  
                          
                                                        
                                            
                                                          
  
                                                    
   
PureCVisorVmManager *
purecvisor_dispatcher_get_vm_manager(PureCVisorDispatcher *self)
{
    g_return_val_if_fail(PURECVISOR_IS_DISPATCHER(self), NULL);
    return self->vm_manager;
}

                                                               

                                                        
                                                              
                                                                    
                                                                
                                             
  
                                                   
                                                       
                                                                    
                                                                     
                                                    
                                    
PureCVisorVmManager *pcv_disp_vm_manager = NULL;

                                                     
                                                  
                                                                              
static void purecvisor_dispatcher_finalize(GObject *object) {
    PureCVisorDispatcher *self = PURECVISOR_DISPATCHER(object);
    if (pcv_disp_vm_manager == self->vm_manager) pcv_disp_vm_manager = NULL;
    if (self->vm_manager) g_object_unref(self->vm_manager);
    G_OBJECT_CLASS(purecvisor_dispatcher_parent_class)->finalize(object);
}

                                           
                                                        
static void purecvisor_dispatcher_class_init(PureCVisorDispatcherClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = purecvisor_dispatcher_finalize;
}

                                                                              
                                              
static void purecvisor_dispatcher_init(PureCVisorDispatcher *self) {
    self->vm_manager = purecvisor_vm_manager_new();
    pcv_disp_vm_manager = self->vm_manager;                            
}

                        
                                                    
                                                                   
PureCVisorDispatcher *purecvisor_dispatcher_new(void) {
    PureCVisorDispatcher *d = g_object_new(PURECVISOR_TYPE_DISPATCHER, NULL);
    dispatcher_init_routes();                           
    return d;
}

                                                                      
                                                    
static void _handle_daemon_version(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "version", PCV_PRODUCT_VERSION);
    json_object_set_string_member(obj, "edition", "single");
    json_object_set_int_member(obj, "rpc_methods", (gint64)g_hash_table_size(g_rpc_routes));
    json_object_set_string_member(obj, "build_date", "2026-03-31");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                     
                   
                                                                        
   
                              
                          
                                                     
   
void dispatcher_shutdown_routes(void)
{
    if (g_rpc_routes) {
        g_hash_table_destroy(g_rpc_routes);
        g_rpc_routes = nullptr;
    }
                             
    if (g_pre_hooks) {
        g_ptr_array_free(g_pre_hooks, TRUE);
        g_pre_hooks = nullptr;
    }
}

   
                                                         
  
                                                        
                                        
  
         
                                               
                                          
                                
                                      
                                                         
                                                       
                                    
  
             
                                                                           
                                             
  
                                
                                               
                                             
                                              
   
                                                                                   
                                                                                    
static const gchar *
_dispatch_deny_audit_target(const gchar *method, JsonObject *params)
{
    return pcv_disp_container_method_requires_owner_scope(method)
        ? pcv_disp_container_owner_scope_target_from_params(method, params)
        : pcv_disp_vm_owner_scope_target_from_params(method, params);
}

                                                         
                                      
static void
_dispatch_deny(const gchar *method, JsonObject *params, const gchar *rpc_id, UdsServer *server,
               GSocketConnection *connection, const gchar *actor, const gchar *message)
{
    const gchar *target = _dispatch_deny_audit_target(method, params);
    gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_FORBIDDEN, message ? message : "Permission denied");
    pure_uds_server_send_response(server, connection, err);
    g_free(err);
    pcv_audit_log(actor, method, target ? target : "", "denied", PURE_RPC_ERR_FORBIDDEN, 0, "rbac");
}

                                                                                  
                                                                                             
                               
                                                                                 
static gboolean
_dispatch_role_check(const gchar *method, JsonObject *params, const gchar *rpc_id, UdsServer *server,
                     GSocketConnection *connection, const gchar *actor)
{
    gint caller_role = pcv_disp_dispatcher_caller_role(params, connection);
    if (!pcv_dispatcher_check_rbac(method, caller_role)) {
        _dispatch_deny(method, params, rpc_id, server, connection, actor,
                       "Permission denied: insufficient role for this method");
        return FALSE;
    }
    const gchar *alias_error = NULL;
    if (!pcv_dispatcher_vm_target_aliases_valid(method, params, &alias_error)) {
                                                                                        
                                                                                           
                           
        const gchar *target = _dispatch_deny_audit_target(method, params);
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, alias_error);
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        pcv_audit_log(actor, method, target ? target : "", "fail", PURE_RPC_ERR_INVALID_PARAMS, 0, "rbac");
        return FALSE;
    }
    return TRUE;
}

                                                                               
                                                                              
                                                                   
static gboolean
_dispatch_owner_check(const gchar *method, JsonObject *params, const gchar *rpc_id, UdsServer *server,
                      GSocketConnection *connection, const gchar *actor)
{
    gint caller_role = pcv_disp_dispatcher_caller_role(params, connection);
    if (caller_role != PCV_ROLE_OPERATOR)
        return TRUE;
    gchar *deny_message = NULL;
    gboolean allowed = TRUE;
    if (pcv_disp_vm_method_requires_owner_scope(method)) {
        allowed = pcv_disp_vm_owner_scoped_method_allowed(method, params, connection, caller_role, &deny_message);
    } else if (pcv_disp_container_method_requires_owner_scope(method)) {
                                                                                        
                             
        allowed = pcv_disp_container_owner_scoped_method_allowed(method, params, connection, caller_role, &deny_message);
    }
    if (!allowed)
        _dispatch_deny(method, params, rpc_id, server, connection, actor, deny_message);
    g_free(deny_message);
    return allowed;
}

                                                                                                      
                                                                                
                                              
static void
_rpc_finish(const gchar *method, const gchar *rpc_id, const gchar *actor, const gchar *target,
            gint64 start_us, PcvRpcCompletionResult completion)
{
    gdouble dur_ms = (gdouble)(g_get_monotonic_time() - start_us) / 1000.0;
    gboolean is_async = pcv_dispatcher_is_async_method(method);
                                                                                                               
                                                                                                    
                                  
    gboolean success = pcv_rpc_completion_dispatch_succeeded(completion, is_async);
    gint error_code = success ? 0 : (completion.response_observed ? completion.error_code : PURE_RPC_ERR_INTERNAL_ERROR);
    pcv_prom_rpc_end(method, success, dur_ms);
                                                                                             
                                                                
    if (pcv_rpc_completion_should_audit(completion, is_async))
        pcv_audit_log(actor, method, target, success ? "ok" : "fail", error_code, (gint64)dur_ms, "local");
                                                            
    if (dur_ms > 1000.0)
        g_warning("[dispatcher] SLOW RPC: method=%s id=%s took %.0fms", method, rpc_id ? rpc_id : "(null)", dur_ms);
}

   
                                                     
  
                                                           
                                                      
                                                             
                                                          
                                          
  
                                   
                                                 
                                           
                                                               
                              
  
           
                                                 
                                                                     
                                                                        
  
            
                                                            
                                                                                      
                                                                      
                                           
                                                                   
  
                                               
                                        
                                             
                                                          
   
void purecvisor_dispatcher_dispatch(PureCVisorDispatcher *self,
                                   UdsServer *server,
                                   GSocketConnection *connection,
                                   const gchar *request_json) {
                                                       
                                                       
                                
                                                             
                                                                  
                                                            
                                                       
                                                       
    JsonParser *parser = nullptr;
    GError *err = nullptr;
    if (!pcv_rpc_parse_guarded(request_json, -1, &parser, &err)) {
                                                                              
                                                                 
                                                                
        gboolean bad_req = err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        gchar *derr = pure_rpc_build_error_response(NULL,
            bad_req ? PURE_RPC_ERR_INVALID_REQUEST : PURE_RPC_ERR_PARSE_ERROR,
            bad_req ? "Invalid Request" : "Parse error");
        pure_uds_server_send_response(server, connection, derr);
        g_free(derr);
        if (err) g_error_free(err);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
                                                                  
                                                                
                                                                              
                                                             
    JsonObject *obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                      ? json_node_get_object(root) : nullptr;
    const gchar *method = obj ? json_object_get_string_member(obj, "method") : nullptr;
    if (!obj || !method) {
        gchar *ierr = pure_rpc_build_error_response(NULL, PURE_RPC_ERR_INVALID_REQUEST,
            "Invalid Request: root must be an object with a string 'method'");
        pure_uds_server_send_response(server, connection, ierr);
        g_free(ierr);
        g_object_unref(parser);
        return;
    }
    
                                                      
      
                                                
                                            
                                   
                                                      
                                                          
                                                                        
    gint id = -1;
    gchar *rpc_id_str = nullptr;
    
    if (json_object_has_member(obj, "id")) {
        JsonNode *id_node = json_object_get_member(obj, "id");
        if (json_node_get_value_type(id_node) == G_TYPE_STRING) {
            rpc_id_str = g_strdup(json_node_get_string(id_node));
            id = 0;                                      
        } else {
            id = json_node_get_int(id_node);
            rpc_id_str = g_strdup_printf("%d", id);
        }
    }

    JsonObject *params = nullptr;
    if (json_object_has_member(obj, "params")) {
        params = json_object_get_object_member(obj, "params");
    }

                                                            
                                                         
                                            
                                
    if (params) {
        if (!json_object_has_member(params, "vm_id") &&
             json_object_has_member(params, "name")) {
            const gchar *alias = json_object_get_string_member(params, "name");
            if (alias) json_object_set_string_member(params, "vm_id", alias);
        } else if (!json_object_has_member(params, "name") &&
                    json_object_has_member(params, "vm_id")) {
            const gchar *alias = json_object_get_string_member(params, "vm_id");
            if (alias) json_object_set_string_member(params, "name", alias);
        }
    }

                                                             
                                                    
                                                        
                      
                                                                      
    gchar *dispatch_req_id = nullptr;
    {
        const gchar *existing = pcv_log_req_id_get();
        if (!existing || g_strcmp0(existing, "-") == 0) {
            dispatch_req_id = pcv_generate_request_id();
            pcv_log_req_id_set(dispatch_req_id);
        }
    }
    PCV_LOG_INFO("dispatcher", "[%s] method=%s id=%s",
                 pcv_log_req_id_get(), method ? method : "(null)",
                 rpc_id_str ? rpc_id_str : "-");

                              
                                                           
                                                               
                           
    DispatcherRequestContext *ctx = g_new0(DispatcherRequestContext, 1);
    ctx->dispatcher = g_object_ref(self);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
    ctx->request_id = id;

                                                          
                                          
                                                            
                                                                        
    gint64 _rpc_start_us = g_get_monotonic_time();
    pcv_prom_rpc_start(method);

                                                                  
                                                                   
    const gchar *_audit_sub = pcv_disp_dispatcher_caller_subject(params, connection);
    const gchar *_audit_target = pcv_disp_dispatcher_audit_target_from_params(method, params);
                                                                                        
    gchar *audit_username = g_strdup((_audit_sub && *_audit_sub) ? _audit_sub : PCV_AUDIT_ACTOR_LOCAL);
    gchar *audit_target = g_strdup((_audit_target && *_audit_target) ? _audit_target : "");
    pcv_rpc_completion_begin(method);
    pcv_rpc_completion_set_actor(audit_username);
                                                                                  
                                                                         
                                                                 
    pcv_mainloop_guard_rpc_enter(method);

                                                               
                                                 
                                            
    if (!_run_pre_hooks(method, params, rpc_id_str)) {
        gchar *err = pure_rpc_build_error_response(rpc_id_str, PURE_RPC_ERR_ZFS_OPERATION,
            "Request rejected by pre-dispatch hook");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        dispatcher_request_context_free(ctx);
        goto rpc_done;
    }

                                                                     
                                       
      
                                                      
                                                  
                                                      
                                                                        

                                                                                   
                                                                            
    if (!_dispatch_role_check(method, params, rpc_id_str, server, connection, audit_username)) {
        dispatcher_request_context_free(ctx);
        goto rpc_done;
    }
    {
        PcvDispatchHandler offload = pcv_rpc_offload_lane_of(method) >= 0
            ? (PcvDispatchHandler)g_hash_table_lookup(g_rpc_routes, method) : NULL;
        if (offload) {
            const PcvRpcOffloadRequest oreq = {
                .method = method, .rpc_id = rpc_id_str, .req_id = pcv_log_req_id_get(), .actor = audit_username,
                .target = audit_target, .start_us = _rpc_start_us, .handler = offload,
                .owner_check = _dispatch_owner_check, .finish = _rpc_finish, .parser = parser, .params = params,
                .server = server, .connection = connection };
            if (!pcv_rpc_offload_submit(&oreq)) {
                gchar *busy = g_strdup_printf("%s: server busy", method);
                gchar *err = pure_rpc_build_error_response(rpc_id_str, PURE_RPC_ERR_BUSY, busy);
                pure_uds_server_send_response(server, connection, err);
                g_free(err);
                g_free(busy);
                dispatcher_request_context_free(ctx);
                goto rpc_done;
            }
                                                                       
            dispatcher_request_context_free(ctx);
            (void)pcv_rpc_completion_finish();
            goto rpc_cleanup;
        }
    }
    if (!_dispatch_owner_check(method, params, rpc_id_str, server, connection, audit_username)) {
        dispatcher_request_context_free(ctx);
        goto rpc_done;
    }

                               
                                                                    
                                              
                                      
    if (g_strcmp0(method, "vm.create") == 0) {
                                                                               
                                                   
        {
            virConnectPtr qconn = virt_conn_pool_acquire();
            if (qconn) {
                int num = virConnectNumOfDefinedDomains(qconn)
                        + virConnectNumOfDomains(qconn);
                virt_conn_pool_release(qconn);
                if (num >= 200) {
                    gchar *err = pure_rpc_build_error_response(rpc_id_str, PURE_RPC_ERR_ZFS_OPERATION,
                        "VM quota exceeded: maximum 200 VMs per node");
                    pure_uds_server_send_response(server, connection, err);
                    g_free(err);
                    dispatcher_request_context_free(ctx);
                    goto rpc_done;
                }
            }
        }
        handle_vm_create(self, params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    } else {
                                                                  
                                        
                                                            
        PcvDispatchHandler handler = (PcvDispatchHandler)g_hash_table_lookup(g_rpc_routes, method);
        if (handler) {
            handler(params, rpc_id_str, server, connection);
            dispatcher_request_context_free(ctx);
        } else if (pcv_plugin_has_handler(method)) {
                                                    
            pcv_plugin_dispatch(method, params, rpc_id_str, server, connection);
            dispatcher_request_context_free(ctx);
        } else {
                                                          
            gchar *err = pure_rpc_build_error_response(rpc_id_str, PURE_RPC_ERR_METHOD_NOT_FOUND,
                "Method not found");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            dispatcher_request_context_free(ctx);
        }
    }
    
    

rpc_done:
    _rpc_finish(method, rpc_id_str, audit_username, audit_target, _rpc_start_us, pcv_rpc_completion_finish());
rpc_cleanup:
    pcv_mainloop_guard_rpc_leave();                                      

                                        
      
                            
                                                                  
                                              
                                                              
                                                               
    g_free(rpc_id_str);
    g_free(dispatch_req_id);
    g_free(audit_username);
    g_free(audit_target);
    pcv_log_req_id_set(NULL);
    g_object_unref(parser);
}

                                                                     
                                       
  
                                                        
                                                     
                                                        
                                          
                                                                 
  
                                 
                                             
                                                   
                          
                                                                        
static void dispatcher_init_routes(void)
{
    if (g_rpc_routes) return;                       

                                                                           
                                                                   
                                                                            
                                                                           
                                                                
                                             
    pcv_dispatcher_init_policy_map();

      
                                      
      
                                 
                                                
                                              
                                                     
      
                
                                                   
                                    
                                                     
      
              
                                                  
                                                       
                                   
       
    g_rpc_routes = g_hash_table_new(g_str_hash, g_str_equal);

                                                               
                                                             
                                                                
                                             
                                                                   
    if (!g_async_methods) {
        g_async_methods = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
                                                             
                                                                
    static const char *_async_method_names[] = {
                                 
        "vm.start",                                                            
        "vm.create",                                                                 
        "vm.delete",                                                                   
        "vm.stop", "vm.pause", "vm.resume", "vm.limit",                                                  
                                            
        "vm.list",                                                                
        "vm.metrics",                                                                
        "vm.inspect",                                                                     
        "webhook.dlq.retry",                                                    
        "dpdk.bridge.create",                                                 
        "dpdk.bridge.delete",                                                 
        "vm.guest.ping",                                                          
        "vm.guest.exec",                                                          
        "vm.guest.shutdown",                                                          
        "vm.snapshot.create",                                                                          
        "vm.snapshot.list",                                                                          
        "vm.snapshot.delete",                                                                          
        "vm.snapshot.delete_all",                                                                               
        "vm.snapshot.rollback",                                                        
        "vm.export.ova",                                                             
                                                                   
        "backup.restore",                                                          
        "backup.replicate",                                                  
        "backup.replication.hostkey.scan",                                                               
        "backup.replication.hostkey.trust",         
        "backup.incremental",                                                          
        "container.create",                                                   
        "container.start",                                                   
        "container.stop",                                                   
        "container.clone",                                                                 
        "container.destroy",                                                   
        "vm.disk.live_resize",                                                           
        "vm.resize_disk",                                             
        "vm.clone",                                                           
        "vm.import.ova",                                                      
        "security.action.approve",                                                          
        "suricata.rules.update",                                                                              
        "suricata.ips.enable",                                                                       
        "suricata.ips.disable",                                                                      
                                                                      
                                                               
                                                          
        "suricata.ips.drop.add",                                                                   
        "suricata.ips.drop.remove",                                                                
                                                                  
                                                           
        "backup.snapshot.verify",                                                          
                                                                                   
                                                                          
        "vpc.create", "vpc.delete", "vpc.egress.set",
        "vpc.subnet.create", "vpc.subnet.delete",
        "vpc.attachment.create", "vpc.attachment.delete",
        "vpc.service.publish", "vpc.service.unpublish",
        "vpc.reconcile",
                                                                                             
                                                                                           
        "vm.set_memory", "vm.set_vcpu",                                                    
        "network.create", "network.delete",                                                    
        "container.exec",                                                                                  
        "container.snapshot.create", "container.snapshot.rollback",
        "container.snapshot.delete", "container.snapshot.list",                                             
        "vm.guest.fsinfo",                                                                     
        "vm.guest.agent.status",                                                                                   
        "container.nic.list", "container.nic.attach",                                                            
        "container.nic.detach", "container.set_bandwidth",                                          
        "storage.pool.list", "storage.zvol.list",                                                             
        "container.list", "network.list",                                           
        "storage.zvol.create", "storage.zvol.delete",                                                                  
        NULL
    };
    for (int _i = 0; _async_method_names[_i]; _i++) {
        g_hash_table_add(g_async_methods, g_strdup(_async_method_names[_i]));
    }
    pcv_bootstrap_register_async_methods(g_async_methods);

                                                        
    g_hash_table_insert(g_rpc_routes, "vm.start",           (gpointer)handle_vm_start_request);
    g_hash_table_insert(g_rpc_routes, "vm.stop",            (gpointer)handle_vm_stop_request);
    g_hash_table_insert(g_rpc_routes, "vm.pause",           (gpointer)handle_vm_pause_request);
    g_hash_table_insert(g_rpc_routes, "vm.resume",          (gpointer)handle_vm_resume_request);
    g_hash_table_insert(g_rpc_routes, "vm.guest.ping",      (gpointer)handle_vm_guest_ping_request);
    g_hash_table_insert(g_rpc_routes, "vm.guest.agent.status", (gpointer)handle_vm_guest_agent_status_request);
    g_hash_table_insert(g_rpc_routes, "vm.guest.agent.ensure_channel", (gpointer)handle_vm_guest_agent_ensure_channel_request);
    g_hash_table_insert(g_rpc_routes, "vm.guest.fsinfo",    (gpointer)handle_vm_guest_fsinfo_request);
                                                                                            
    g_hash_table_insert(g_rpc_routes, "vm.guest.exec",      (gpointer)handle_vm_guest_exec_request);
    g_hash_table_insert(g_rpc_routes, "vm.guest.shutdown",  (gpointer)handle_vm_guest_shutdown_request);
    g_hash_table_insert(g_rpc_routes, "vm.delete",          (gpointer)handle_vm_delete_request);
    g_hash_table_insert(g_rpc_routes, "vm.delete.status",   (gpointer)handle_vm_delete_status);
    g_hash_table_insert(g_rpc_routes, "vm.list",            (gpointer)handle_vm_list_request);
    g_hash_table_insert(g_rpc_routes, "vm.limit",           (gpointer)handle_vm_limit_request);
    g_hash_table_insert(g_rpc_routes, "vm.metrics",         (gpointer)handle_vm_metrics_request);
    g_hash_table_insert(g_rpc_routes, "vm.inspect",         (gpointer)handle_vm_inspect_request);
    g_hash_table_insert(g_rpc_routes, "vm.rename",          (gpointer)handle_vm_rename_request);
    g_hash_table_insert(g_rpc_routes, "vm.vnc",             (gpointer)handle_vm_vnc_request);
    g_hash_table_insert(g_rpc_routes, "get_vnc_info",       (gpointer)handle_vnc_request);
    g_hash_table_insert(g_rpc_routes, "vm.mount_iso",       (gpointer)handle_vm_mount_iso);
    g_hash_table_insert(g_rpc_routes, "vm.eject",           (gpointer)handle_vm_eject_iso);
    g_hash_table_insert(g_rpc_routes, "vm.resize_disk",     (gpointer)handle_vm_resize_disk);
    g_hash_table_insert(g_rpc_routes, "vm.clone",           (gpointer)handle_vm_clone);
    g_hash_table_insert(g_rpc_routes, "vm.set_bandwidth",   (gpointer)handle_vm_set_bandwidth);

                                                                   
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.create",     (gpointer)handle_vm_snapshot_create);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.list",       (gpointer)handle_vm_snapshot_list);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.rollback",   (gpointer)handle_vm_snapshot_rollback);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.delete",     (gpointer)handle_vm_snapshot_delete);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.delete_all", (gpointer)handle_vm_snapshot_delete_all);

                                                                  
    g_hash_table_insert(g_rpc_routes, "vm.set_memory",       (gpointer)handle_vm_set_memory_request);
    g_hash_table_insert(g_rpc_routes, "vm.set_vcpu",         (gpointer)handle_vm_set_vcpu_request);
    g_hash_table_insert(g_rpc_routes, "vm.pin_vcpu",         (gpointer)handle_vm_pin_vcpu);
    g_hash_table_insert(g_rpc_routes, "vm.memory.stats",     (gpointer)handle_vm_memory_stats_request);
    g_hash_table_insert(g_rpc_routes, "vm.cpu.stats",        (gpointer)handle_vm_cpu_stats_request);
    g_hash_table_insert(g_rpc_routes, "vm.disk.live_resize", (gpointer)handle_vm_disk_live_resize_request);

                                                              
    g_hash_table_insert(g_rpc_routes, "device.disk.attach",  (gpointer)handle_device_disk_attach);
    g_hash_table_insert(g_rpc_routes, "device.disk.detach",  (gpointer)handle_device_disk_detach);
    g_hash_table_insert(g_rpc_routes, "device.nic.list",     (gpointer)handle_device_nic_list);
    g_hash_table_insert(g_rpc_routes, "device.nic.attach",   (gpointer)handle_device_nic_attach);
    g_hash_table_insert(g_rpc_routes, "device.nic.detach",   (gpointer)handle_device_nic_detach);
    g_hash_table_insert(g_rpc_routes, "vm.usb.attach",       (gpointer)handle_vm_usb_attach);
    g_hash_table_insert(g_rpc_routes, "vm.usb.detach",       (gpointer)handle_vm_usb_detach);
    g_hash_table_insert(g_rpc_routes, "vm.usb.list",         (gpointer)handle_vm_usb_list);

                                                                  
    g_hash_table_insert(g_rpc_routes, "network.create",         (gpointer)handle_network_create_request);
    g_hash_table_insert(g_rpc_routes, "network.delete",         (gpointer)handle_network_delete_request);
    g_hash_table_insert(g_rpc_routes, "network.list",           (gpointer)handle_network_list_request);
    g_hash_table_insert(g_rpc_routes, "network.host.info",      (gpointer)handle_network_host_info_request);
    g_hash_table_insert(g_rpc_routes, "network.info",           (gpointer)handle_network_info_request);
    g_hash_table_insert(g_rpc_routes, "network.mode_set",       (gpointer)handle_network_mode_set_request);
    g_hash_table_insert(g_rpc_routes, "network.bind_phys",      (gpointer)handle_network_bind_phys_request);
    g_hash_table_insert(g_rpc_routes, "network.dhcp_toggle",    (gpointer)handle_network_dhcp_toggle_request);
    g_hash_table_insert(g_rpc_routes, "network.ovs.create",     (gpointer)handle_network_ovs_create_request);
    g_hash_table_insert(g_rpc_routes, "network.ovs.delete",     (gpointer)handle_network_ovs_delete_request);
    g_hash_table_insert(g_rpc_routes, "network.ovs.vxlan.add",  (gpointer)handle_network_ovs_vxlan_add_request);
    g_hash_table_insert(g_rpc_routes, "network.ovs.vxlan.del",  (gpointer)handle_network_ovs_vxlan_del_request);

                                                                  
    g_hash_table_insert(g_rpc_routes, "storage.pool.list",     (gpointer)handle_storage_pool_list_request);
    g_hash_table_insert(g_rpc_routes, "storage.zvol.list",     (gpointer)handle_storage_zvol_list_request);
    g_hash_table_insert(g_rpc_routes, "storage.zvol.create",   (gpointer)handle_storage_zvol_create_request);
    g_hash_table_insert(g_rpc_routes, "storage.zvol.delete",   (gpointer)handle_storage_zvol_delete_request);
    g_hash_table_insert(g_rpc_routes, "storage.pool.create",   (gpointer)handle_storage_pool_create_request);
    g_hash_table_insert(g_rpc_routes, "storage.pool.destroy",  (gpointer)handle_storage_pool_destroy_request);
    g_hash_table_insert(g_rpc_routes, "storage.pool.scrub",    (gpointer)handle_storage_pool_scrub_request);
    g_hash_table_insert(g_rpc_routes, "storage.pool.health",   (gpointer)handle_storage_pool_health);
    g_hash_table_insert(g_rpc_routes, "storage.pool.forecast", (gpointer)handle_storage_pool_forecast);

                                                                  
    g_hash_table_insert(g_rpc_routes, "container.create",           (gpointer)handle_container_create);
    g_hash_table_insert(g_rpc_routes, "container.destroy",          (gpointer)handle_container_destroy);
    g_hash_table_insert(g_rpc_routes, "container.start",            (gpointer)handle_container_start);
    g_hash_table_insert(g_rpc_routes, "container.stop",             (gpointer)handle_container_stop);
    g_hash_table_insert(g_rpc_routes, "container.list",             (gpointer)handle_container_list);
    g_hash_table_insert(g_rpc_routes, "container.metrics",          (gpointer)handle_container_metrics);
    g_hash_table_insert(g_rpc_routes, "container.exec",             (gpointer)handle_container_exec);
                                                                    
                                                                        
                                                                    
                                                              
                                               
                                                    
    g_hash_table_insert(g_rpc_routes, "container.snapshot.create",  (gpointer)handle_container_snapshot_create);
    g_hash_table_insert(g_rpc_routes, "container.snapshot.rollback",(gpointer)handle_container_snapshot_rollback);
    g_hash_table_insert(g_rpc_routes, "container.snapshot.delete",  (gpointer)handle_container_snapshot_delete);
    g_hash_table_insert(g_rpc_routes, "container.snapshot.list",    (gpointer)handle_container_snapshot_list);
    g_hash_table_insert(g_rpc_routes, "container.set_limits",       (gpointer)handle_container_set_limits);
    g_hash_table_insert(g_rpc_routes, "container.nic.list",         (gpointer)handle_container_nic_list);
    g_hash_table_insert(g_rpc_routes, "container.nic.attach",       (gpointer)handle_container_nic_attach);
    g_hash_table_insert(g_rpc_routes, "container.nic.detach",       (gpointer)handle_container_nic_detach);
    g_hash_table_insert(g_rpc_routes, "container.set_bandwidth",    (gpointer)handle_container_set_bandwidth);
    g_hash_table_insert(g_rpc_routes, "container.logs",            (gpointer)handle_container_logs);
    g_hash_table_insert(g_rpc_routes, "container.volume.attach",   (gpointer)handle_container_volume_attach);
    g_hash_table_insert(g_rpc_routes, "container.volume.detach",   (gpointer)handle_container_volume_detach);
    g_hash_table_insert(g_rpc_routes, "container.volume.list",     (gpointer)handle_container_volume_list);
    g_hash_table_insert(g_rpc_routes, "container.env.set",         (gpointer)handle_container_env_set);
    g_hash_table_insert(g_rpc_routes, "container.env.list",        (gpointer)handle_container_env_list);
    g_hash_table_insert(g_rpc_routes, "container.env.delete",      (gpointer)handle_container_env_delete);
    g_hash_table_insert(g_rpc_routes, "container.health.set",      (gpointer)handle_container_health_set);
    g_hash_table_insert(g_rpc_routes, "container.health.get",      (gpointer)handle_container_health_get);
    g_hash_table_insert(g_rpc_routes, "container.health.delete",   (gpointer)handle_container_health_delete);

                                                             
    g_hash_table_insert(g_rpc_routes, "monitor.metrics",     (gpointer)handle_monitor_metrics);
    g_hash_table_insert(g_rpc_routes, "monitor.fleet",       (gpointer)handle_monitor_fleet_rpc);
    g_hash_table_insert(g_rpc_routes, "monitor.processes",   (gpointer)handle_monitor_processes);
    g_hash_table_insert(g_rpc_routes, "alert.history",       (gpointer)handle_alert_history);
    g_hash_table_insert(g_rpc_routes, "alert.ack",           (gpointer)handle_alert_ack);              
    g_hash_table_insert(g_rpc_routes, "alert.config.get",    (gpointer)handle_alert_config_get);
    g_hash_table_insert(g_rpc_routes, "alert.config.set",    (gpointer)handle_alert_config_set);
    g_hash_table_insert(g_rpc_routes, "alert.config.reload", (gpointer)handle_alert_config_reload);

                                                              
                                                         
                                                               
    g_hash_table_insert(g_rpc_routes, "push.vapid.get",      (gpointer)handle_push_vapid_get);
    g_hash_table_insert(g_rpc_routes, "push.subscribe",      (gpointer)handle_push_subscribe);
    g_hash_table_insert(g_rpc_routes, "push.unsubscribe",    (gpointer)handle_push_unsubscribe);
    g_hash_table_insert(g_rpc_routes, "push.mine",           (gpointer)handle_push_mine);
    g_hash_table_insert(g_rpc_routes, "push.list",           (gpointer)handle_push_list);
    g_hash_table_insert(g_rpc_routes, "push.vapid.rotate",   (gpointer)handle_push_vapid_rotate);
    g_hash_table_insert(g_rpc_routes, "push.test",           (gpointer)handle_push_test);

                                                                      
    g_hash_table_insert(g_rpc_routes, "agent.config.get",    (gpointer)handle_agent_config_get);
    g_hash_table_insert(g_rpc_routes, "agent.config.set",    (gpointer)handle_agent_config_set);
    g_hash_table_insert(g_rpc_routes, "agent.history",       (gpointer)handle_agent_history);
    g_hash_table_insert(g_rpc_routes, "healing.history",     (gpointer)handle_healing_history);
    g_hash_table_insert(g_rpc_routes, "healing.pending",     (gpointer)handle_healing_pending);              
    g_hash_table_insert(g_rpc_routes, "healing.status",      (gpointer)handle_healing_status);                               
    g_hash_table_insert(g_rpc_routes, "healing.set_mode",    (gpointer)handle_healing_set_mode);               
    g_hash_table_insert(g_rpc_routes, "anomaly.reset_baseline", (gpointer)handle_anomaly_reset_baseline);           
    g_hash_table_insert(g_rpc_routes, "agent.compare_manual",   (gpointer)handle_agent_compare_manual);           

                                                                  
    g_hash_table_insert(g_rpc_routes, "overlay.create",      (gpointer)handle_overlay_create);
    g_hash_table_insert(g_rpc_routes, "overlay.delete",      (gpointer)handle_overlay_delete);
    g_hash_table_insert(g_rpc_routes, "overlay.list",        (gpointer)handle_overlay_list);
    g_hash_table_insert(g_rpc_routes, "overlay.info",        (gpointer)handle_overlay_info);
    g_hash_table_insert(g_rpc_routes, "overlay.add_peer",    (gpointer)handle_overlay_add_peer);
    g_hash_table_insert(g_rpc_routes, "overlay.remove_peer", (gpointer)handle_overlay_remove_peer);

                                                                      
    g_hash_table_insert(g_rpc_routes, "iscsi.target.create", (gpointer)handle_iscsi_target_create);
    g_hash_table_insert(g_rpc_routes, "iscsi.target.delete", (gpointer)handle_iscsi_target_delete);
    g_hash_table_insert(g_rpc_routes, "iscsi.target.list",   (gpointer)handle_iscsi_target_list);
    g_hash_table_insert(g_rpc_routes, "iscsi.connect",       (gpointer)handle_iscsi_connect);
    g_hash_table_insert(g_rpc_routes, "iscsi.disconnect",    (gpointer)handle_iscsi_disconnect);
    g_hash_table_insert(g_rpc_routes, "iso.list",            (gpointer)handle_iso_list);

                                                                      
    g_hash_table_insert(g_rpc_routes, "ovn.switch.create",   (gpointer)handle_ovn_switch_create);
    g_hash_table_insert(g_rpc_routes, "ovn.switch.delete",   (gpointer)handle_ovn_switch_delete);
    g_hash_table_insert(g_rpc_routes, "ovn.switch.list",     (gpointer)handle_ovn_switch_list);
    g_hash_table_insert(g_rpc_routes, "ovn.switch.detail",   (gpointer)handle_ovn_switch_detail);
    g_hash_table_insert(g_rpc_routes, "ovn.port.add",        (gpointer)handle_ovn_port_add);
    g_hash_table_insert(g_rpc_routes, "ovn.port.remove",     (gpointer)handle_ovn_port_remove);
    g_hash_table_insert(g_rpc_routes, "ovn.acl.add",         (gpointer)handle_ovn_acl_add);
    g_hash_table_insert(g_rpc_routes, "ovn.acl.list",        (gpointer)handle_ovn_acl_list);
    g_hash_table_insert(g_rpc_routes, "ovn.router.create",   (gpointer)handle_ovn_router_create);
    g_hash_table_insert(g_rpc_routes, "ovn.router.delete",   (gpointer)handle_ovn_router_delete);
    g_hash_table_insert(g_rpc_routes, "ovn.router.list",     (gpointer)handle_ovn_router_list);
    g_hash_table_insert(g_rpc_routes, "ovn.router.detail",   (gpointer)handle_ovn_router_detail);
    g_hash_table_insert(g_rpc_routes, "ovn.router.add_port", (gpointer)handle_ovn_router_add_port);
    g_hash_table_insert(g_rpc_routes, "ovn.dhcp.enable",     (gpointer)handle_ovn_dhcp_enable);
    g_hash_table_insert(g_rpc_routes, "ovn.nat.add",         (gpointer)handle_ovn_nat_add);
    g_hash_table_insert(g_rpc_routes, "ovn.nat.list",        (gpointer)handle_ovn_nat_list);
    g_hash_table_insert(g_rpc_routes, "ovn.tenant.create",   (gpointer)handle_ovn_tenant_create);
    g_hash_table_insert(g_rpc_routes, "ovn.status",          (gpointer)handle_ovn_status);

                                                                      
    g_hash_table_insert(g_rpc_routes, "dpdk.status",         (gpointer)handle_dpdk_status);
    g_hash_table_insert(g_rpc_routes, "dpdk.bind",           (gpointer)handle_dpdk_bind);
    g_hash_table_insert(g_rpc_routes, "dpdk.unbind",         (gpointer)handle_dpdk_unbind);
    g_hash_table_insert(g_rpc_routes, "dpdk.list",           (gpointer)handle_dpdk_list);
    g_hash_table_insert(g_rpc_routes, "dpdk.bridge.create",  (gpointer)handle_dpdk_bridge_create);
    g_hash_table_insert(g_rpc_routes, "dpdk.bridge.delete",  (gpointer)handle_dpdk_bridge_delete);
    g_hash_table_insert(g_rpc_routes, "dpdk.hugepage.info",  (gpointer)handle_dpdk_hugepage_info);

                                                                      
    g_hash_table_insert(g_rpc_routes, "sriov.status",        (gpointer)handle_sriov_status);
    g_hash_table_insert(g_rpc_routes, "sriov.enable",        (gpointer)handle_sriov_enable);
    g_hash_table_insert(g_rpc_routes, "sriov.disable",       (gpointer)handle_sriov_disable);
    g_hash_table_insert(g_rpc_routes, "sriov.list",          (gpointer)handle_sriov_list);
    g_hash_table_insert(g_rpc_routes, "sriov.set",           (gpointer)handle_sriov_set);
    g_hash_table_insert(g_rpc_routes, "sriov.attach",        (gpointer)handle_sriov_attach);
    g_hash_table_insert(g_rpc_routes, "sriov.detach",        (gpointer)handle_sriov_detach);

                                                                      
    g_hash_table_insert(g_rpc_routes, "auth.user.create",    (gpointer)handle_auth_user_create);
    g_hash_table_insert(g_rpc_routes, "auth.user.list",      (gpointer)handle_auth_user_list);
    g_hash_table_insert(g_rpc_routes, "auth.user.delete",    (gpointer)handle_auth_user_delete);
    g_hash_table_insert(g_rpc_routes, "auth.role.set",       (gpointer)handle_auth_role_set);
    g_hash_table_insert(g_rpc_routes, "auth.user.tenant.set", (gpointer)handle_auth_user_tenant_set);                    
    g_hash_table_insert(g_rpc_routes, "resource.register",          (gpointer)handle_resource_register);                    
    g_hash_table_insert(g_rpc_routes, "resource.unregister",        (gpointer)handle_resource_unregister);
    g_hash_table_insert(g_rpc_routes, "resource.grant",             (gpointer)handle_resource_grant);
    g_hash_table_insert(g_rpc_routes, "resource.revoke",            (gpointer)handle_resource_revoke);
    g_hash_table_insert(g_rpc_routes, "resource.list",              (gpointer)handle_resource_list);
    g_hash_table_insert(g_rpc_routes, "resource.unregistered.list", (gpointer)handle_resource_unregistered_list);
    g_hash_table_insert(g_rpc_routes, "resource.import",            (gpointer)handle_resource_import);

                                                                   
    g_hash_table_insert(g_rpc_routes, "auth.totp.status",              (gpointer)handle_auth_totp_status);
    g_hash_table_insert(g_rpc_routes, "auth.totp.disable",             (gpointer)handle_auth_totp_disable);
    g_hash_table_insert(g_rpc_routes, "auth.totp.reset",               (gpointer)handle_auth_totp_reset);
    g_hash_table_insert(g_rpc_routes, "auth.totp.recovery.regenerate", (gpointer)handle_auth_totp_recovery_regenerate);

                                                                   
    g_hash_table_insert(g_rpc_routes, "template.list",       (gpointer)handle_template_list);
    g_hash_table_insert(g_rpc_routes, "template.get",        (gpointer)handle_template_get);
    g_hash_table_insert(g_rpc_routes, "template.create",     (gpointer)handle_template_create);
    g_hash_table_insert(g_rpc_routes, "template.delete",     (gpointer)handle_template_delete);
    g_hash_table_insert(g_rpc_routes, "template.history",    (gpointer)handle_template_history);

                                                                    
    g_hash_table_insert(g_rpc_routes, "backup.policy.set",   (gpointer)handle_backup_policy_set);
    g_hash_table_insert(g_rpc_routes, "backup.policy.list",  (gpointer)handle_backup_policy_list);
    g_hash_table_insert(g_rpc_routes, "backup.policy.delete",(gpointer)handle_backup_policy_delete);
    g_hash_table_insert(g_rpc_routes, "backup.history",      (gpointer)handle_backup_history);
    g_hash_table_insert(g_rpc_routes, "backup.restore",      (gpointer)handle_backup_restore);
    g_hash_table_insert(g_rpc_routes, "backup.incremental",  (gpointer)handle_backup_incremental);
    g_hash_table_insert(g_rpc_routes, "backup.verify",       (gpointer)handle_backup_verify);
    g_hash_table_insert(g_rpc_routes, "backup.replicate",    (gpointer)handle_backup_replicate);
                                                                         
    g_hash_table_insert(g_rpc_routes, "backup.replication.hostkey.scan",   (gpointer)handle_backup_replication_hostkey_scan);
    g_hash_table_insert(g_rpc_routes, "backup.replication.hostkey.trust",  (gpointer)handle_backup_replication_hostkey_trust);
    g_hash_table_insert(g_rpc_routes, "backup.replication.hostkey.list",   (gpointer)handle_backup_replication_hostkey_list);
    g_hash_table_insert(g_rpc_routes, "backup.replication.hostkey.remove", (gpointer)handle_backup_replication_hostkey_remove);

                                                                       
    g_hash_table_insert(g_rpc_routes, "security.event.list",       (gpointer)handle_security_event_list);
    g_hash_table_insert(g_rpc_routes, "security.event.get",        (gpointer)handle_security_event_get);
    g_hash_table_insert(g_rpc_routes, "security.event.close",      (gpointer)handle_security_event_close);
    g_hash_table_insert(g_rpc_routes, "security.action.pending",   (gpointer)handle_security_action_pending);
    g_hash_table_insert(g_rpc_routes, "security.action.approve",   (gpointer)handle_security_action_approve);
    g_hash_table_insert(g_rpc_routes, "security.action.dismiss",   (gpointer)handle_security_action_dismiss);
    g_hash_table_insert(g_rpc_routes, "security.baseline.status",  (gpointer)handle_security_baseline_status);
    g_hash_table_insert(g_rpc_routes, "security.baseline.refresh", (gpointer)handle_security_baseline_refresh);
    g_hash_table_insert(g_rpc_routes, "security.config.get",       (gpointer)handle_security_config_get);
    g_hash_table_insert(g_rpc_routes, "security.config.set",       (gpointer)handle_security_config_set);

                                                                
    g_hash_table_insert(g_rpc_routes, "snapshot.schedule.status", (gpointer)handle_snapshot_schedule_status);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.schedule.set",    (gpointer)handle_snapshot_schedule_set);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.schedule.list",   (gpointer)handle_snapshot_schedule_list);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.schedule.delete", (gpointer)handle_snapshot_schedule_delete);

                                                                  
    g_hash_table_insert(g_rpc_routes, "vm.blkio.set",     (gpointer)handle_vm_blkio_set);
    g_hash_table_insert(g_rpc_routes, "vm.blkio.get",     (gpointer)handle_vm_blkio_get);


                                                                    
    g_hash_table_insert(g_rpc_routes, "daemon.version",      (gpointer)_handle_daemon_version);
    g_hash_table_insert(g_rpc_routes, "daemon.update_check", (gpointer)handle_daemon_update_check);
    g_hash_table_insert(g_rpc_routes, "node.drain",          (gpointer)handle_node_drain);
    g_hash_table_insert(g_rpc_routes, "node.resume",         (gpointer)handle_node_resume);
    g_hash_table_insert(g_rpc_routes, "quota.get",           (gpointer)handle_quota_get);

    pcv_bootstrap_register_rpc_routes(g_rpc_routes);

                                                                      
    g_hash_table_insert(g_rpc_routes, "gpu.metrics",         (gpointer)handle_gpu_metrics);
    g_hash_table_insert(g_rpc_routes, "gpu.list",            (gpointer)handle_gpu_list);
    g_hash_table_insert(g_rpc_routes, "device.gpu.attach",   (gpointer)handle_device_gpu_attach);
    g_hash_table_insert(g_rpc_routes, "device.gpu.detach",   (gpointer)handle_device_gpu_detach);

                                                                      
    g_hash_table_insert(g_rpc_routes, "host.device.list",    (gpointer)handle_host_device_list);

    g_hash_table_insert(g_rpc_routes, "webhook.dlq.list",    (gpointer)handle_webhook_dlq_list);
    g_hash_table_insert(g_rpc_routes, "webhook.dlq.retry",   (gpointer)handle_webhook_dlq_retry);

                                                                  
    g_hash_table_insert(g_rpc_routes, "security_group.create",   (gpointer)handle_security_group_create);
    g_hash_table_insert(g_rpc_routes, "security_group.list",     (gpointer)handle_security_group_list);
    g_hash_table_insert(g_rpc_routes, "security_group.delete",   (gpointer)handle_security_group_delete);
    g_hash_table_insert(g_rpc_routes, "security_group.rule.add",    (gpointer)handle_security_group_rule_add);
    g_hash_table_insert(g_rpc_routes, "security_group.rule.remove", (gpointer)handle_security_group_rule_remove);
    g_hash_table_insert(g_rpc_routes, "ai.healing.approve",         (gpointer)handle_ai_healing_approve);
    g_hash_table_insert(g_rpc_routes, "ai.healing.reject",          (gpointer)handle_ai_healing_reject);
    g_hash_table_insert(g_rpc_routes, "vm.security_group.set",   (gpointer)handle_vm_security_group_set);
    g_hash_table_insert(g_rpc_routes, "security_group.detach",   (gpointer)handle_security_group_detach);

                                                              
    g_hash_table_insert(g_rpc_routes, "tenant_overlay.create",    (gpointer)handle_tenant_overlay_create);
    g_hash_table_insert(g_rpc_routes, "tenant_overlay.delete",    (gpointer)handle_tenant_overlay_delete);
    g_hash_table_insert(g_rpc_routes, "tenant_overlay.list",      (gpointer)handle_tenant_overlay_list);
    g_hash_table_insert(g_rpc_routes, "tenant_overlay.get",       (gpointer)handle_tenant_overlay_get);
    g_hash_table_insert(g_rpc_routes, "tenant_overlay.attach_vm", (gpointer)handle_tenant_overlay_attach_vm);
    g_hash_table_insert(g_rpc_routes, "tenant_overlay.detach_vm", (gpointer)handle_tenant_overlay_detach_vm);

                                                                          
    g_hash_table_insert(g_rpc_routes, "vpc.list",              (gpointer)handle_vpc_list);
    g_hash_table_insert(g_rpc_routes, "vpc.get",               (gpointer)handle_vpc_get);
    g_hash_table_insert(g_rpc_routes, "vpc.subnet.list",       (gpointer)handle_vpc_subnet_list);
    g_hash_table_insert(g_rpc_routes, "vpc.attachment.list",   (gpointer)handle_vpc_attachment_list);
    g_hash_table_insert(g_rpc_routes, "vpc.service.list",      (gpointer)handle_vpc_service_list);
    g_hash_table_insert(g_rpc_routes, "vpc.status",            (gpointer)handle_vpc_status);
    g_hash_table_insert(g_rpc_routes, "vpc.backend.list",      (gpointer)handle_vpc_backend_list);
    g_hash_table_insert(g_rpc_routes, "vpc.create",            (gpointer)handle_vpc_create);
    g_hash_table_insert(g_rpc_routes, "vpc.delete",            (gpointer)handle_vpc_delete);
    g_hash_table_insert(g_rpc_routes, "vpc.egress.set",        (gpointer)handle_vpc_egress_set);
    g_hash_table_insert(g_rpc_routes, "vpc.subnet.create",     (gpointer)handle_vpc_subnet_create);
    g_hash_table_insert(g_rpc_routes, "vpc.subnet.delete",     (gpointer)handle_vpc_subnet_delete);
    g_hash_table_insert(g_rpc_routes, "vpc.attachment.create", (gpointer)handle_vpc_attachment_create);
    g_hash_table_insert(g_rpc_routes, "vpc.attachment.delete", (gpointer)handle_vpc_attachment_delete);
    g_hash_table_insert(g_rpc_routes, "vpc.service.publish",   (gpointer)handle_vpc_service_publish);
    g_hash_table_insert(g_rpc_routes, "vpc.service.unpublish", (gpointer)handle_vpc_service_unpublish);
    g_hash_table_insert(g_rpc_routes, "vpc.reconcile",         (gpointer)handle_vpc_reconcile);
    g_hash_table_insert(g_rpc_routes, "vpc.subnet.confirm",    (gpointer)handle_vpc_subnet_confirm);
    g_hash_table_insert(g_rpc_routes, "vpc.pool.set",          (gpointer)handle_vpc_pool_set);
    g_hash_table_insert(g_rpc_routes, "vpc.pool.list",         (gpointer)handle_vpc_pool_list);

                                                                      
    g_hash_table_insert(g_rpc_routes, "config.history",      (gpointer)handle_config_history);
    g_hash_table_insert(g_rpc_routes, "config.backup",       (gpointer)handle_config_backup);
    g_hash_table_insert(g_rpc_routes, "daemon.config.get",   (gpointer)handle_daemon_config_get);
    g_hash_table_insert(g_rpc_routes, "daemon.config.set",   (gpointer)handle_daemon_config_set);
    g_hash_table_insert(g_rpc_routes, "audit.search",        (gpointer)handle_audit_search);

                                                                      
    g_hash_table_insert(g_rpc_routes, "network.qos.set",      (gpointer)handle_network_qos_set);
    g_hash_table_insert(g_rpc_routes, "network.qos.get",      (gpointer)handle_network_qos_get);
    g_hash_table_insert(g_rpc_routes, "network.qos.remove",   (gpointer)handle_network_qos_remove);

                                                                     
    g_hash_table_insert(g_rpc_routes, "qos.vm.set",           (gpointer)handle_qos_vm_set);
    g_hash_table_insert(g_rpc_routes, "qos.vm.get",           (gpointer)handle_qos_vm_get);
    g_hash_table_insert(g_rpc_routes, "qos.tenant.set",       (gpointer)handle_qos_tenant_set);
    g_hash_table_insert(g_rpc_routes, "qos.tenant.get",       (gpointer)handle_qos_tenant_get);
    g_hash_table_insert(g_rpc_routes, "qos.stats",            (gpointer)handle_qos_stats);
    g_hash_table_insert(g_rpc_routes, "qos.chaos.start",      (gpointer)handle_qos_chaos_start);
    g_hash_table_insert(g_rpc_routes, "qos.chaos.stop",       (gpointer)handle_qos_chaos_stop);
    g_hash_table_insert(g_rpc_routes, "qos.chaos.status",     (gpointer)handle_qos_chaos_status);

                                                                 
    g_hash_table_insert(g_rpc_routes, "debug.trace.start",   (gpointer)handle_debug_trace_start);
    g_hash_table_insert(g_rpc_routes, "debug.trace.stop",    (gpointer)handle_debug_trace_stop);
    g_hash_table_insert(g_rpc_routes, "debug.trace.status",  (gpointer)handle_debug_trace_status);
    g_hash_table_insert(g_rpc_routes, "debug.trace.list",    (gpointer)handle_debug_trace_list);
    g_hash_table_insert(g_rpc_routes, "debug.trace.report",  (gpointer)handle_debug_trace_report);

                                                                   
    g_hash_table_insert(g_rpc_routes, "suricata.policy.set",   (gpointer)handle_suricata_policy_set);
    g_hash_table_insert(g_rpc_routes, "suricata.policy.get",   (gpointer)handle_suricata_policy_get);
    g_hash_table_insert(g_rpc_routes, "suricata.rules.update", (gpointer)handle_suricata_rules_update);
    g_hash_table_insert(g_rpc_routes, "suricata.status",       (gpointer)handle_suricata_status);
    g_hash_table_insert(g_rpc_routes, "suricata.ips.status",  (gpointer)handle_suricata_ips_status);
    g_hash_table_insert(g_rpc_routes, "suricata.ips.enable",  (gpointer)handle_suricata_ips_enable);
    g_hash_table_insert(g_rpc_routes, "suricata.ips.disable", (gpointer)handle_suricata_ips_disable);
    g_hash_table_insert(g_rpc_routes, "suricata.ips.drop.list",   (gpointer)handle_suricata_ips_drop_list);
    g_hash_table_insert(g_rpc_routes, "suricata.ips.drop.add",    (gpointer)handle_suricata_ips_drop_add);
    g_hash_table_insert(g_rpc_routes, "suricata.ips.drop.remove", (gpointer)handle_suricata_ips_drop_remove);

                                                                      
    g_hash_table_insert(g_rpc_routes, "vm.import.ova",        (gpointer)handle_vm_import_ova);
    g_hash_table_insert(g_rpc_routes, "vm.export.ova",        (gpointer)handle_vm_export_ova);
                                                                                                         
                                                               
                                                                  
                                       
    g_hash_table_insert(g_rpc_routes, "jobs.list",            (gpointer)handle_jobs_list);
    g_hash_table_insert(g_rpc_routes, "jobs.get",             (gpointer)handle_jobs_get);
    g_hash_table_insert(g_rpc_routes, "jobs.status",          (gpointer)handle_jobs_get);                      
    g_hash_table_insert(g_rpc_routes, "jobs.cancel",          (gpointer)handle_jobs_cancel);
    g_hash_table_insert(g_rpc_routes, "prometheus.sd",        (gpointer)handle_prometheus_sd);
    g_hash_table_insert(g_rpc_routes, "vm.event.webhook.list",(gpointer)handle_vm_event_webhook_list);
    g_hash_table_insert(g_rpc_routes, "alert.action.list",    (gpointer)handle_alert_action_list);

                                                                  
                             
                                                                     

                                                                 
    g_hash_table_insert(g_rpc_routes, "auth.apikey.create",   (gpointer)handle_apikey_create);
    g_hash_table_insert(g_rpc_routes, "auth.apikey.list",     (gpointer)handle_apikey_list);
    g_hash_table_insert(g_rpc_routes, "auth.apikey.revoke",   (gpointer)handle_apikey_revoke);
    g_hash_table_insert(g_rpc_routes, "auth.session.revoke",  (gpointer)handle_session_revoke);
    g_hash_table_insert(g_rpc_routes, "auth.user.sessions.revoke", (gpointer)handle_user_sessions_revoke);

                                                                 
    g_hash_table_insert(g_rpc_routes, "vm.batch",             (gpointer)handle_vm_batch);
    g_hash_table_insert(g_rpc_routes, "pool.conninfo",        (gpointer)handle_pool_conninfo);

                                                                 
    g_hash_table_insert(g_rpc_routes, "config.reload",        (gpointer)handle_config_reload);
    g_hash_table_insert(g_rpc_routes, "health.deep",          (gpointer)handle_health_deep);
    g_hash_table_insert(g_rpc_routes, "backup.snapshot.verify",(gpointer)handle_snapshot_verify);
    g_hash_table_insert(g_rpc_routes, "alert.silence",        (gpointer)handle_alert_silence);
    g_hash_table_insert(g_rpc_routes, "alert.silence.list",   (gpointer)handle_alert_silence_list);
    g_hash_table_insert(g_rpc_routes, "alert.dlq.list",       (gpointer)handle_alert_dlq_list);
                                                                 
                                                
    g_hash_table_insert(g_rpc_routes, "db.migration.status",  (gpointer)handle_db_migration_status);

                                                               
    g_hash_table_insert(g_rpc_routes, "container.clone",           (gpointer)handle_container_clone);
    g_hash_table_insert(g_rpc_routes, "container.memory.stats",    (gpointer)handle_container_memory_stats);
    g_hash_table_insert(g_rpc_routes, "container.health.check",    (gpointer)handle_container_health_check);

                                                             
    g_hash_table_insert(g_rpc_routes, "vm.numa.info",        (gpointer)handle_vm_numa_info);
    g_hash_table_insert(g_rpc_routes, "vm.autostart",        (gpointer)handle_vm_autostart);
    g_hash_table_insert(g_rpc_routes, "vm.sla.report",       (gpointer)handle_vm_sla_report);
    g_hash_table_insert(g_rpc_routes, "capacity.forecast",   (gpointer)handle_capacity_forecast);
    g_hash_table_insert(g_rpc_routes, "vm.schedule.list",    (gpointer)handle_vm_schedule_list);
    g_hash_table_insert(g_rpc_routes, "vm.schedule.set",     (gpointer)handle_vm_schedule_set);
    g_hash_table_insert(g_rpc_routes, "vm.billing.report",   (gpointer)handle_vm_billing_report);

    g_message("[DISPATCHER] Route table initialized: %u methods registered",
              g_hash_table_size(g_rpc_routes));
}

