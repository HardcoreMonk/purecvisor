   
                           
                                                                                                            
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                                
   

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
#include "modules/dispatcher/vm_delete_status.h"
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
#include "api/dispatcher_internal.h"


                                                                     
                                         
  
                
  
                                       
                                                              
                                                                       
  
                                                        
                                             
  
                                
                                                       
                                                            
                             
                                   
                                                
                                                         
                       
  
               
                                    
                                     
                                                           
                                                             
                                   
  
                            
                                                
                                                  
  
                                            
                                        
                                                                        

                                                                      
                                                      
  
                                                             
                                                                                      
                                                                        
                                      
                                                                         
                                                                                      
                                                                          
void handle_vm_delete_status(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!vm || !pcv_validate_vm_name(vm)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid param: name (alphanumeric, -, _ only)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }
    JsonObject *job = NULL;
    if (!pcv_job_find_latest("vm.delete", vm, &job)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR, "Job ledger unavailable");
        pure_uds_server_send_response(server, connection, e);
        g_free(e);
        return;
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, pcv_vm_delete_status_build(vm, job));
    if (job) json_object_unref(job);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                       
                                         
  
                                                     
                                                          
                                                                    
                                                                        
                                                           
                                                                          
void handle_vm_resize_disk(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    gint new_size_gb = json_object_has_member(params, "new_size_gb")
        ? json_object_get_int_member(params, "new_size_gb") : 0;
    const gchar *target = json_object_has_member(params, "target")
        ? json_object_get_string_member(params, "target") : NULL;

    if (!vm_name || new_size_gb <= 0) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid params: name, new_size_gb (>0) required");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
    } else {
                                                                              
                                                                         
                                                                
                                                                          
        gchar *lock_err = NULL;
        PcvVmLock *op_lock = pcv_vm_lock_acquire(vm_name, VM_OP_TUNING, &lock_err);
        if (!op_lock) {
            gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                lock_err ? lock_err : "VM busy (operation in progress)");
            pure_uds_server_send_response(server, connection, err_resp);
            g_free(err_resp); g_free(lock_err);
            return;
        }
        JsonNode *accepted = json_node_new(JSON_NODE_VALUE);
        json_node_set_string(accepted, "resize accepted");
        gchar *resp = pure_rpc_build_success_response(rpc_id, accepted);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        purecvisor_vm_resize_disk(vm_name, new_size_gb, target, op_lock             );
    }
}

                                                                   
                                                                                      
                                                                             
                                                                            
                                                                        
                                                                        
                                                                             

                                    
static void _vm_batch_start_done(const gchar *vm_id, gboolean ok, const GError *error, gpointer user_data)
{
    (void)vm_id; (void)ok; (void)error;                                              
    pcv_vm_batch_queue_item_done((PcvVmBatchQueue *)user_data);
}

static gboolean _vm_batch_start_submit(const gchar *vm, const gchar *actor, PcvVmBatchQueue *queue,
                                       gpointer user_data, gchar **reason)
{
    (void)user_data;
    PcvVmStartRequest req = { .vm_id = vm, .numa_node = 0, .vcpu_count = 0,
                              .bridge_name = NULL, .origin = "vm.batch", .actor = actor };
    return pcv_vm_start_submit(&req, _vm_batch_start_done, queue, reason) == PCV_VM_START_SUBMITTED;
}

static void _vm_batch_start_report(const gchar *vm, const gchar *actor, const gchar *reason, gpointer user_data)
{
    (void)user_data;
    pcv_vm_start_report_rejected(vm, "vm.batch", reason, actor);
}

                                                                   
static void _vm_batch_stop_done(const gchar *vm_id, gboolean ok, const GError *error, gpointer user_data)
{
    (void)vm_id; (void)ok; (void)error;                                               
    pcv_vm_batch_queue_item_done((PcvVmBatchQueue *)user_data);
}

static gboolean _vm_batch_stop_submit(const gchar *vm, const gchar *actor, PcvVmBatchQueue *queue,
                                      gpointer user_data, gchar **reason)
{
    return pcv_vm_graceful_stop_submit((PureCVisorVmManager *)user_data, vm, "vm.batch", actor,
                                       _vm_batch_stop_done, queue, reason);
}

static void _vm_batch_stop_report(const gchar *vm, const gchar *actor, const gchar *reason, gpointer user_data)
{
    (void)user_data;
    pcv_vm_stop_report_rejected(vm, "vm.batch", reason, actor);
}

static void _vm_batch_reject(JsonArray *rejected, const gchar *vm, const gchar *reason)
{
    JsonObject *rej = json_object_new();
    json_object_set_string_member(rej, "vm", vm ? vm : "");
    json_object_set_string_member(rej, "reason", reason);
    json_array_add_object_element(rejected, rej);
}

                                                                
  
                                                              
                                                                                                  
                                        
   
void handle_vm_batch(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *action = params ? json_object_get_string_member_with_default(params, "action", NULL) : NULL;
    JsonArray *vms = (params && json_object_has_member(params, "vms"))
        ? json_object_get_array_member(params, "vms") : NULL;
    if (!action || !vms) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "action and vms[] required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    if (!pcv_vm_batch_action_is_whitelisted(action)) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "unsupported batch action");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    gboolean is_start = g_strcmp0(action, "start") == 0;
    PureCVisorVmManager *mgr = pcv_disp_vm_manager;
    if (!is_start && !mgr) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "vm manager unavailable");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "libvirt unavailable");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    JsonArray *accepted = json_array_new();
    JsonArray *rejected = json_array_new();
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *queued = g_ptr_array_new_with_free_func(g_free);
    guint len = json_array_get_length(vms);
    for (guint i = 0; i < len && i < 100; i++) {
        const gchar *vm = json_array_get_string_element(vms, i);
        const gchar *why = pcv_vm_batch_name_reject_reason(vm, seen);
        if (!why) {
            virDomainPtr dom = virDomainLookupByName(conn, vm);                            
            if (dom) virDomainFree(dom); else why = "VM not found";
        }
        if (why) { _vm_batch_reject(rejected, vm, why); continue; }
        g_ptr_array_add(queued, g_strdup(vm));
        json_array_add_string_element(accepted, vm);
    }
    virt_conn_pool_release(conn);
    g_hash_table_unref(seen);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "action", action);
    json_object_set_array_member(res, "accepted", accepted);                     
    json_object_set_array_member(res, "rejected", rejected);                     
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);

    if (queued->len > 0) {
        PcvVmBatchQueueOps ops;
        if (is_start) {
            ops = (PcvVmBatchQueueOps){ _vm_batch_start_submit, _vm_batch_start_report,
                                        pcv_drain_is_terminating, NULL, 1, NULL, pcv_rpc_completion_actor() };
        } else {
            guint pool_max = 0;
            virt_conn_pool_stats(NULL, NULL, &pool_max);
            guint stop_k = pcv_vm_batch_stop_concurrency(pool_max);
                                                                       
            ops = (PcvVmBatchQueueOps){ _vm_batch_stop_submit, _vm_batch_stop_report,
                                        pcv_drain_is_terminating, mgr, stop_k, pcv_vm_batch_stop_slots(stop_k),
                                        pcv_rpc_completion_actor() };
        }
        pcv_vm_batch_queue_run((const gchar *const *)queued->pdata, queued->len, &ops);
    }
    g_ptr_array_unref(queued);
}

                                                                       
                                                                      
                                                   
                                      
                                                                          
void handle_vm_event_webhook_list(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection)
{
    JsonObject *result = json_object_new();

    const gchar *url = pcv_config_get_string("alert", "webhook_url", "");
    const gchar *fmt = pcv_config_get_string("alert", "webhook_format", "generic");
    const gchar *enabled_str = pcv_config_get_string("alert", "enabled", "false");
    gboolean enabled = (g_ascii_strcasecmp(enabled_str, "true") == 0);

    JsonArray *webhooks = json_array_new();
    if (url && *url) {
        JsonObject *wh = json_object_new();
        json_object_set_string_member(wh, "url", url);
                                                                     
        pcv_disp_webhook_member_for_caller(wh, "url", params, connection);
        json_object_set_string_member(wh, "format", fmt);
        json_object_set_boolean_member(wh, "enabled", enabled);
        json_object_set_string_member(wh, "scope", "vm.events");
        json_array_add_object_element(webhooks, wh);
    }

    json_object_set_array_member(result, "webhooks", webhooks);
    json_object_set_int_member(result, "count",
        (gint64)json_array_get_length(webhooks));

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                       
                                                                
                                               
                                                          
                                   
                                                                          

                                                                                    
                                                                               

                                                          
                                                        
                            
                                   
                                                        
                                                    
                                           
void handle_vm_autostart(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!name || !*name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    gboolean has_enable  = params && json_object_has_member(params, "enable");
    gboolean want_enable = has_enable && json_object_get_boolean_member(params, "enable");

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT, "libvirt connection unavailable");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    virDomainPtr dom = virDomainLookupByName(conn, name);
    if (!dom) {
        virResetLastError();
        dom = virDomainLookupByUUIDString(conn, name);
    }
    if (!dom) {
        virt_conn_pool_release(conn);
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_NOT_FOUND, "VM not found");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    if (has_enable) {
        if (virDomainSetAutostart(dom, want_enable ? 1 : 0) < 0) {
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "failed to set autostart");
            pure_uds_server_send_response(server, connection, r); g_free(r); return;
        }
    }

    int autostart = 0;
    if (virDomainGetAutostart(dom, &autostart) < 0) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "failed to read autostart");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "name", name);
    json_object_set_boolean_member(res, "autostart", autostart ? TRUE : FALSE);
    json_object_set_string_member(res, "action", has_enable ? "set" : "get");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                            
                                                      
                                     
                   
                                                                 
                                                              
void handle_vm_sla_report(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!name || !*name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    JsonObject *sla = pcv_alert_get_sla(name);
    json_object_set_string_member(sla, "vm", name);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, sla);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                         
                                                      
                                                  
                                               
                                                    
void handle_vm_schedule_list(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "not_available");
    json_object_set_string_member(res, "reason",
        "VM start/stop schedule backend not implemented");
    json_object_set_array_member(res, "schedules", json_array_new());
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                         
                                                      
                                            
                                              
                                              
                            
void handle_vm_schedule_set(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!name || !*name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "not_available");
    json_object_set_boolean_member(res, "applied", FALSE);
    json_object_set_string_member(res, "name", name);
    json_object_set_string_member(res, "reason",
        "VM start/stop schedule backend not implemented");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                            
                                                          
               
                                                  
void handle_vm_billing_report(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "not_available");
    json_object_set_string_member(res, "reason",
        "resource metering/billing backend not implemented");
    json_object_set_array_member(res, "records", json_array_new());
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                           
                                                            
                                                        
                                                                         
                                                                           
                                                                
void handle_vm_numa_info(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *result = cpu_allocator_get_numa_info(global_allocator);                   
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}
