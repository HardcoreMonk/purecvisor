   
                       
                                                                              
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                         
   

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
#include "modules/dispatcher/handler_push.h"
#include "modules/dispatcher/handler_security_group.h"
#include "modules/dispatcher/handler_jobs.h"
#include "api/dispatcher_internal.h"


                                                                      
                                             
void handle_jobs_list(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    gint lim = (params && json_object_has_member(params, "limit"))
        ? (gint)json_object_get_int_member(params, "limit") : 50;
    JsonArray *arr = pcv_job_list(lim);
    gint pg_off = 0, pg_lim = 0;
    pcv_disp_get_pagination_params(params, &pg_off, &pg_lim);
    JsonNode *node = pcv_disp_paginate_array(arr, pg_off, pg_lim > 0 ? pg_lim : lim);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                      
                                                              
void handle_jobs_get(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *job_id = (params && json_object_has_member(params, "job_id"))
        ? json_object_get_string_member(params, "job_id") : NULL;
    if (!job_id) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing param: job_id");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    JsonObject *obj = pcv_job_get(job_id);
    if (!obj) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "Job not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                      
                                                          
void handle_jobs_cancel(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *job_id = (params && json_object_has_member(params, "job_id"))
        ? json_object_get_string_member(params, "job_id") : NULL;
    if (!job_id) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing param: job_id");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    gboolean ok = pcv_job_cancel(job_id);
    if (!ok) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Cannot cancel: job not found or already finished");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                        
                                                
                                                       
                                            
                                                
  
                                                                     
                                                                          
void handle_prometheus_sd(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    gint port = pcv_config_get_rest_port();

                 
    gchar hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0)
        g_strlcpy(hostname, "localhost", sizeof(hostname));

    gchar *target = g_strdup_printf("%s:%d", hostname, port);

    JsonObject *labels = json_object_new();
    json_object_set_string_member(labels, "job", "purecvisor");
    json_object_set_string_member(labels, "__metrics_path__", "/api/v1/metrics");

    JsonArray *targets = json_array_new();
    json_array_add_string_element(targets, target);
    g_free(target);

    JsonObject *entry = json_object_new();
    json_object_set_array_member(entry, "targets", targets);
    json_object_set_object_member(entry, "labels", labels);

    JsonArray *result = json_array_new();
    json_array_add_object_element(result, entry);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}



                                                                        
                                    
  
                                                              
                                    
  
          
                                         
                                                 
                                                   
                        
                                                                          
void handle_webhook_dlq_list(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *dlq = pcv_alert_engine_dlq_list();
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, dlq);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                 
                                              
  
                                         
                                                                 
                                                 
                                                                     
                                                     
                                                            
                                                 
                                                                   
                                                  
                                                                             
                                               
                                                   
                                                             
                                               
                                                 
         
                                                                          
                                                          
typedef struct {
    UdsServer         *server;
    GSocketConnection *connection;
    gchar             *rpc_id;
} DlqRetryCtx;

static void _dlq_retry_ctx_free(gpointer data)
{
    DlqRetryCtx *ctx = data;
    if (!ctx) return;
    g_free(ctx->rpc_id);
    if (ctx->server)     g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

                                                                              
                                                                      
static void _dlq_retry_worker(GTask *task, gpointer src, gpointer data, GCancellable *cancellable)
{
    (void)src; (void)data; (void)cancellable;
    g_task_return_pointer(task, pcv_alert_engine_dlq_retry(), (GDestroyNotify)json_object_unref);
}

                                                           
                                                           
                                                       
static void _dlq_retry_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    DlqRetryCtx *ctx = data;
    JsonObject *result = g_task_propagate_pointer(G_TASK(res), NULL);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result ? result : json_object_new());
    gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, node);
    pure_uds_server_send_response(ctx->server, ctx->connection, resp); g_free(resp);
                                                                   
                                                                     
                                                          
    pcv_audit_log(pcv_drain_task_actor(res), "webhook.dlq.retry", "dlq", "ok", 0, 0, "local");
                                                                       
}

void handle_webhook_dlq_retry(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    (void)params;
                                                                      
                                                                   
    DlqRetryCtx *ctx = g_new0(DlqRetryCtx, 1);
    ctx->server     = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
    ctx->rpc_id     = g_strdup(rpc_id);
    GTask *task = pcv_drain_task_new(NULL, NULL, _dlq_retry_done, ctx);
    g_task_set_task_data(task, ctx, _dlq_retry_ctx_free);
    g_task_run_in_thread(task, _dlq_retry_worker);
    g_object_unref(task);
}
