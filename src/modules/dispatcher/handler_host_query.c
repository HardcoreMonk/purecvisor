   
                             
                                                                      
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                          
   

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
#include "modules/dispatcher/handler_host_query.h"
#include "api/dispatcher_internal.h"


                                   
                                                           
                                   
void handle_pool_conninfo(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    guint idle = 0, total = 0, max = 0;
    virt_conn_pool_stats(&idle, &total, &max);
    JsonObject *res = json_object_new();
    json_object_set_int_member(res, "idle", (gint64)idle);
    json_object_set_int_member(res, "total", (gint64)total);
    json_object_set_int_member(res, "max", (gint64)max);
    json_object_set_double_member(res, "wait_avg_sec", virt_conn_pool_wait_avg_seconds());
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

                                                       
                                                       
                                               
                                                             
                                                
                                  
void handle_capacity_forecast(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = json_object_new();
    gchar *fjson = pcv_predict_get_forecast_json();

    JsonNode *farr = NULL;
    if (fjson && *fjson) {
        JsonParser *p = json_parser_new();
                                                         
        if (json_parser_load_from_data(p, fjson, -1, NULL)) {
            JsonNode *root = json_parser_get_root(p);
            if (root && JSON_NODE_HOLDS_ARRAY(root))
                farr = json_node_copy(root);
        }
        g_object_unref(p);
    }
    g_free(fjson);

    if (farr) {
        json_object_set_member(res, "forecasts", farr);                        
        json_object_set_string_member(res, "source", "workload_predict");
    } else {
        json_object_set_array_member(res, "forecasts", json_array_new());
        json_object_set_string_member(res, "note", "no prediction samples yet");
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                        
                                 
  
                                                         
                                   
  
            
                                                   
                                       
                                            
                                                     
                                                   
  
          
                                                   
                                                                          
void handle_quota_get(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *obj = json_object_new();
    json_object_set_int_member(obj, "max_vms_per_node", 200);
    json_object_set_int_member(obj, "max_snapshots_per_vm", 50);
    json_object_set_int_member(obj, "max_disk_gb", 2048);
    json_object_set_boolean_member(obj, "maintenance_mode", FALSE);
    {
        virConnectPtr qconn = virt_conn_pool_acquire();
        if (qconn) {
            int num = virConnectNumOfDefinedDomains(qconn) + virConnectNumOfDomains(qconn);
            virt_conn_pool_release(qconn);
            json_object_set_int_member(obj, "current_vms", num);
        } else {
            json_object_set_int_member(obj, "current_vms", -1);
        }
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                      
                                                   
                                       
void handle_audit_search(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection)
{
    if (json_object_has_member(params, "invalid_query")) {                                                         
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid audit search query (from_ts, to_ts: YYYY-MM-DD HH:MM:SS UTC; username, method; limit >= 1; offset >= 0)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    const gchar *from = json_object_has_member(params, "from_ts")
        ? json_object_get_string_member(params, "from_ts") : NULL;
    const gchar *to = json_object_has_member(params, "to_ts")
        ? json_object_get_string_member(params, "to_ts") : NULL;
    const gchar *user = json_object_has_member(params, "username")
        ? json_object_get_string_member(params, "username") : NULL;
    const gchar *meth = json_object_has_member(params, "method")
        ? json_object_get_string_member(params, "method") : NULL;
    gint lim = json_object_has_member(params, "limit")
        ? (gint)json_object_get_int_member(params, "limit") : 100;
    gint pg_offset = json_object_has_member(params, "offset")
        ? (gint)json_object_get_int_member(params, "offset") : 0;
                                                                               
                                                                                              
                                                                    
    gint64 fetch = (gint64)lim + MAX(pg_offset, 0) + 1;
    JsonArray *results = pcv_audit_search(from, to, user, meth, lim > 0 ? (gint)MIN(fetch, G_MAXINT) : lim);
    JsonNode *node = pcv_disp_paginate_array(results, pg_offset, lim);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}
