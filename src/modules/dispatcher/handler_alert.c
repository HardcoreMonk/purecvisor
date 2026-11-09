   
                        
                                                                              
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                 
   

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
#include "api/dispatcher_internal.h"


                                                                       
                             
  
                                                         
                                            
                                            
                                                                          
void handle_alert_history(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    JsonArray *arr = pcv_alert_engine_get_history();
    gint pg_off = 0, pg_lim = 0;
    pcv_disp_get_pagination_params(params, &pg_off, &pg_lim);
    JsonNode *node = pcv_disp_paginate_array(arr, pg_off, pg_lim);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                       
                      
                                                       
                                                                     
                                                                                       
                                                                          
void handle_alert_config_get(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    JsonObject *cfg = pcv_alert_engine_get_config();
    pcv_disp_webhook_member_for_caller(cfg, "webhook_url", params, connection);
    pcv_disp_webhook_member_for_caller(cfg, "webhook_crit_url", params, connection);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, cfg);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                       
                                              
                                                 
                                                       
                                          
                                                                          
void handle_alert_config_set(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    JsonNode *expected_node = params
        ? json_object_get_member(params, "expected_revision")
        : NULL;
    if (!expected_node
        || !JSON_NODE_HOLDS_VALUE(expected_node)
        || json_node_get_value_type(expected_node) != G_TYPE_INT64
        || json_node_get_int(expected_node) < 1) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "expected_revision must be a positive integer");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(pcv_rpc_completion_actor(), "alert.config.set", "config", "fail", PURE_RPC_ERR_INVALID_PARAMS, 0, "local");
        pcv_disp_wipe_json_string_member(params, "webhook_secret");
        return;
    }

    JsonObject *patch = pcv_alert_config_patch_from_rpc(params);

    PcvAlertConfigSetResult result = pcv_alert_engine_set_config(
        patch, json_node_get_int(expected_node));
    pcv_disp_wipe_json_string_member(patch, "webhook_secret");
    pcv_disp_wipe_json_string_member(params, "webhook_secret");
    json_object_unref(patch);
    if (result == PCV_ALERT_CONFIG_SET_OK) {
        JsonObject *cfg = pcv_alert_engine_get_config();
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, cfg);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(pcv_rpc_completion_actor(), "alert.config.set", "config", "ok", 0, 0, "local");
    } else if (result == PCV_ALERT_CONFIG_SET_CONFLICT) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_CONFLICT,
            "Alert config revision conflict");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(pcv_rpc_completion_actor(), "alert.config.set", "config", "fail", PURE_RPC_ERR_CONFLICT, 0, "local");
    } else {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid alert config");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(pcv_rpc_completion_actor(), "alert.config.set", "config", "fail", PURE_RPC_ERR_INVALID_PARAMS, 0, "local");
    }
}

                                                                       
                                                   
  
                                                           
                                                 
                                               
                                                    
                    
                                                                          
void handle_alert_config_reload(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    PcvAlertConfigSetResult apply_result =
        pcv_alert_engine_reload_daemon_config();

    if (apply_result != PCV_ALERT_CONFIG_SET_OK) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid alert daemon config");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(pcv_rpc_completion_actor(), "alert.config.reload", "config", "fail", PURE_RPC_ERR_INVALID_PARAMS, 0, "local");
        return;
    }

    JsonObject *result = pcv_alert_engine_get_config();
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    pcv_audit_log(pcv_rpc_completion_actor(), "alert.config.reload", "config", "ok", 0, 0, "local");
}

                                                                       
                                               
                                                         
                                      
                                                               
                                                                          
void handle_agent_config_get(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    extern JsonObject *pcv_agent_get_config(void);
    JsonObject *ag_cfg = pcv_agent_get_config();
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, ag_cfg);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                       
                                               
                                                      
                                                                                                    
                                        
                                                                          
void handle_agent_config_set(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    extern JsonObject *pcv_agent_get_config(void);
    extern gboolean pcv_agent_set_config(JsonObject *p);
    gboolean ok = pcv_agent_set_config(params);
    if (ok) {
        JsonObject *ag_cfg = pcv_agent_get_config();
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, ag_cfg);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid agent config");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }
}

                                                                       
                                          
                                                 
                                                           
                                                               
                                                         
                                                                          
void handle_agent_history(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    extern gchar *pcv_agent_get_last_comparison_json(void);
    gchar *json_str = pcv_agent_get_last_comparison_json();
    JsonParser *p2 = json_parser_new();
                                                     
    json_parser_load_from_data(p2, json_str, -1, NULL);
    JsonNode *node = json_node_copy(json_parser_get_root(p2));
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(json_str);
    g_object_unref(p2);
}

                                                                       
                                                      
  
                                                          
                                                   
  
                                                      
                                            
  
                 
                                                      
  
       
                                                             
                                               
  
       
                                                        
                            
                                                                          
void handle_agent_compare_manual(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *user_ctx = params && json_object_has_member(params, "context")
        ? json_object_get_string_member(params, "context") : "manual operator trigger";

    extern JsonObject *pcv_ebpf_telemetry_get_host(void);
    extern void pcv_agent_compare_async(const gchar *metrics_json, const gchar *anomaly_context);

    JsonObject *host = pcv_ebpf_telemetry_get_host();
    gchar *host_json = NULL;
    if (host) {
        JsonNode *n = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(n, host);
        host_json = json_to_string(n, FALSE);
        json_node_free(n);
        json_object_unref(host);
    }
    pcv_agent_compare_async(host_json ?: "{}", user_ctx);
    g_free(host_json);

    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "dispatched", TRUE);
    json_object_set_string_member(obj, "context", user_ctx);
    json_object_set_string_member(obj, "note",
        "AI Agent compare_async dispatched. Poll agent.history for results (5min rate limit applies).");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                       
                                                 
                                                    
  
                                                      
                                                         
  
            
                                              
                                           
                 
  
        
                                  
                              
                                                                          
void handle_anomaly_reset_baseline(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    extern void pcv_anomaly_reset_baseline(void);
    pcv_anomaly_reset_baseline();
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "reset", TRUE);
    json_object_set_string_member(obj, "message",
        "Anomaly baseline reset. Z-Score will warm up after 50s (10 samples).");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                       
                                                         
                                                           
                                                               
                                                   
  
                                                               
                                                                          
void handle_healing_pending(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    extern gchar *pcv_healing_get_pending_json(void);
    gchar *json_str = pcv_healing_get_pending_json();
    JsonParser *p2 = json_parser_new();
                                                     
    json_parser_load_from_data(p2, json_str, -1, NULL);
    JsonNode *node = json_node_copy(json_parser_get_root(p2));
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(json_str);
    g_object_unref(p2);
}

                                                                       
                                                                           
                                                  
  
                                                                       
                                                                                            
                                                         
                                                                                            
                                                                          
void handle_healing_status(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    extern gchar *pcv_healing_get_pending_json(void);
    extern gboolean pcv_healing_get_mode(void);
    gchar *json_str = pcv_healing_get_pending_json();
    JsonParser *parser = json_parser_new();
                                                     
    gboolean parsed = json_parser_load_from_data(parser, json_str, -1, NULL);
    JsonNode *root = parsed ? json_parser_get_root(parser) : NULL;
    gchar *resp;
    if (root && JSON_NODE_HOLDS_ARRAY(root)) {
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "mode", pcv_healing_get_mode() ? "dry_run" : "active");
        json_object_set_int_member(obj, "pending", (gint64)json_array_get_length(json_node_get_array(root)));
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        resp = pure_rpc_build_success_response(rpc_id, node);
    } else {
        resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR, "Failed to read pending healing actions");
    }
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(json_str);
    g_object_unref(parser);
}

                                                                       
                                                            
                                     
  
                                                             
                                                
                                               
                                                      
                                 
                                                                          
void handle_healing_set_mode(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    const gchar *mode = params ? json_object_get_string_member(params, "mode") : NULL;
    if (!mode || !*mode) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required param: mode (\"active\" | \"dry_run\")");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gboolean target_dry_run;
    if (g_ascii_strcasecmp(mode, "active") == 0) target_dry_run = FALSE;
    else if (g_ascii_strcasecmp(mode, "dry_run") == 0 ||
             g_ascii_strcasecmp(mode, "dryrun") == 0) target_dry_run = TRUE;
    else {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid mode (use \"active\" or \"dry_run\")");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    extern void pcv_healing_set_mode(gboolean dry_run);
    pcv_healing_set_mode(target_dry_run);

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "mode", target_dry_run ? "dry_run" : "active");
    json_object_set_boolean_member(obj, "dry_run", target_dry_run);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                       
                                 
  
                                                            
                                           
                                    
                                                          
                                                              
                                                                          
void handle_healing_history(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    extern gchar *pcv_healing_get_history_json(void);
    gchar *json_str = pcv_healing_get_history_json();
    JsonParser *p2 = json_parser_new();
                                                     
    json_parser_load_from_data(p2, json_str, -1, NULL);
    JsonNode *parsed = json_parser_get_root(p2);

    gint pg_off = 0, pg_lim = 0;
    pcv_disp_get_pagination_params(params, &pg_off, &pg_lim);

    if (pg_lim > 0 && parsed && JSON_NODE_HOLDS_ARRAY(parsed)) {
        JsonArray *full = json_array_ref(json_node_get_array(parsed));
        JsonNode *node = pcv_disp_paginate_array(full, pg_off, pg_lim);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        JsonNode *node = json_node_copy(parsed);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }
    g_free(json_str);
    g_object_unref(p2);
}

                              
                                                      
                                                     
void handle_alert_silence(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    const gchar *metric = params ? json_object_get_string_member_with_default(params, "metric", NULL) : NULL;
    gint duration_min = (params && json_object_has_member(params, "duration_min"))
        ? (gint)json_object_get_int_member(params, "duration_min") : 60;
    const gchar *reason = params ? json_object_get_string_member_with_default(params, "reason", "") : "";
    if (!metric) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "metric required");
        pure_uds_server_send_response(server, connection, r); g_free(r);
        pcv_audit_log(pcv_rpc_completion_actor(), "alert.silence", "", "fail", PURE_RPC_ERR_INVALID_PARAMS, 0, "local");
        return;
    }
                                  
    pcv_alert_add_silence(metric, duration_min, reason);
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "metric", metric);
    json_object_set_int_member(res, "duration_min", duration_min);
    json_object_set_string_member(res, "status", "silenced");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
    pcv_audit_log(pcv_rpc_completion_actor(), "alert.silence", metric, "ok", 0, 0, "local");
}

                                      
                                            
void handle_alert_silence_list(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = pcv_alert_get_silences();
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

                                           
                                                    
                                                                
                                                            
void handle_alert_dlq_list(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = pcv_alert_engine_dlq_list();
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr ? arr : json_array_new());
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

                                                              
                                                                    
                                                         
                                              

                                                                       
                                                           
  
                                                   
                                  
                                      
                                                     
                                               
                                            
                                                                   
                                                       
                                                                          
void handle_alert_ack(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
                                                      
                                                         
    JsonNode *id_node = params ? json_object_get_member(params, "alert_id") : NULL;
    if (!id_node
        || !JSON_NODE_HOLDS_VALUE(id_node)
        || json_node_get_value_type(id_node) != G_TYPE_INT64) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "alert_id must be an integer");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(pcv_rpc_completion_actor(), "alert.ack", "", "fail", PURE_RPC_ERR_INVALID_PARAMS, 0, "local");
        return;
    }

    gint64 alert_id_val = json_node_get_int(id_node);
    gboolean acked = pcv_alert_acknowledge(alert_id_val);
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, acked);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    gchar id_buf[32];
    g_snprintf(id_buf, sizeof(id_buf), "%" G_GINT64_FORMAT, alert_id_val);
    pcv_audit_log(pcv_rpc_completion_actor(), "alert.ack", id_buf, "ok", 0, 0, "local");
}

                                                                       
                                             
                                                           
                                                         
                                                                          
void handle_alert_action_list(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    JsonObject *cfg = pcv_alert_engine_get_config();
                                                                                      
    pcv_disp_webhook_member_for_caller(cfg, "webhook_url", params, connection);

                                                  
    JsonObject *result = json_object_new();
    JsonArray *actions = json_array_new();

    const gchar *webhook_url = json_object_get_string_member(cfg, "webhook_url");
    const gchar *webhook_fmt = json_object_get_string_member(cfg, "webhook_format");
    if (webhook_url && *webhook_url) {
        JsonObject *wh = json_object_new();
        json_object_set_string_member(wh, "type", "webhook");
        json_object_set_string_member(wh, "url", webhook_url);
        json_object_set_string_member(wh, "format", webhook_fmt ? webhook_fmt : "generic");
        json_object_set_boolean_member(wh, "enabled",
            json_object_get_boolean_member(cfg, "enabled"));
        json_array_add_object_element(actions, wh);
    }

    const gchar *tg_chat = json_object_get_string_member(cfg, "telegram_chat_id");
    if (tg_chat && *tg_chat) {
        JsonObject *tg = json_object_new();
        json_object_set_string_member(tg, "type", "telegram");
        json_object_set_string_member(tg, "chat_id", tg_chat);
        json_object_set_boolean_member(tg, "enabled",
            json_object_get_boolean_member(cfg, "enabled"));
        json_array_add_object_element(actions, tg);
    }

    json_object_set_array_member(result, "actions", actions);

                   
    json_object_set_int_member(result, "cpu_warn",
        json_object_get_int_member(cfg, "cpu_warn"));
    json_object_set_int_member(result, "cpu_crit",
        json_object_get_int_member(cfg, "cpu_crit"));
    json_object_set_int_member(result, "mem_warn",
        json_object_get_int_member(cfg, "mem_warn"));
    json_object_set_int_member(result, "mem_crit",
        json_object_get_int_member(cfg, "mem_crit"));
    json_object_set_int_member(result, "disk_warn",
        json_object_get_int_member(cfg, "disk_warn"));
    json_object_set_int_member(result, "disk_crit",
        json_object_get_int_member(cfg, "disk_crit"));
    json_object_set_int_member(result, "eval_period",
        json_object_get_int_member(cfg, "eval_period"));

                             
    if (json_object_has_member(cfg, "composite_rules")) {
        json_object_set_array_member(result, "composite_rules",
            json_array_ref(json_object_get_array_member(cfg, "composite_rules")));
    }

    json_object_unref(cfg);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                      
                                                                 
                                       
void handle_ai_healing_approve(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    if (!params || !json_object_has_member(params, "action_id")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: action_id");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }
    gint action_id = (gint)json_object_get_int_member(params, "action_id");
    extern void pcv_healing_approve(gint action_id);
    pcv_healing_approve(action_id);
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                  
                                                                
                                            
                                                       
                                                        
                                                     
void handle_ai_healing_reject(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    if (!params || !json_object_has_member(params, "action_id")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: action_id");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }
    gint action_id = (gint)json_object_get_int_member(params, "action_id");
    extern void pcv_healing_dismiss(gint action_id);
    pcv_healing_dismiss(action_id);
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}
