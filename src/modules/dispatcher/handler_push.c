   
                       
                                   
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                              
   

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
#include "api/dispatcher_internal.h"


                                                                  
                                                         
                                           
  
                                                       
                                                                         
                 
  
         
                                                        
                                                   
                                                                              
                                                            
                                                   
                                                                        
                              
                                                                   
                                                      
                                                                
                                                                          

                                                          
                                                         
                                                  
static void
_push_ep_summary(gchar *buf, gsize buf_len, const gchar *endpoint)
{
    g_snprintf(buf, buf_len, "%.80s", endpoint ? endpoint : "");
}

                                                                  
static const gchar *
_push_caller_username(JsonObject *params, GSocketConnection *connection)
{
    const gchar *sub = pcv_disp_dispatcher_caller_subject(params, connection);
    return (sub && *sub) ? sub : "admin";
}

                                                    
                                              
                                               
                                  
void handle_push_vapid_get(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    gchar *key = pcv_webpush_vapid_public();
    if (!key) {
                                                         
                                               
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            "VAPID 공개키를 사용할 수 없습니다 (webpush 미초기화 또는 키 생성 실패)");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "key", key);
    g_free(key);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                             
                                                
                                    
                                                            
                                 
                                                 
                                                                 
                                                              
void handle_push_subscribe(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    const gchar *endpoint = pcv_disp_json_string_member(params, "endpoint");
    const gchar *p256dh   = NULL;
    const gchar *auth     = NULL;

                                                                            
                                                
                  
    JsonNode *keys_node = params ? json_object_get_member(params, "keys") : NULL;
    if (keys_node && JSON_NODE_HOLDS_OBJECT(keys_node)) {
        JsonObject *keys = json_node_get_object(keys_node);
        p256dh = pcv_disp_json_string_member(keys, "p256dh");
        auth   = pcv_disp_json_string_member(keys, "auth");
    }

    if (!endpoint || !*endpoint || !p256dh || !*p256dh || !auth || !*auth) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "endpoint, keys.p256dh, keys.auth 는 모두 비어 있지 않은 문자열이어야 합니다");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(pcv_rpc_completion_actor(), "push.subscribe", "", "fail",
                      PURE_RPC_ERR_INVALID_PARAMS, 0, "local");
        return;
    }

    gchar ep_buf[96];
    _push_ep_summary(ep_buf, sizeof ep_buf, endpoint);
    const gchar *username = _push_caller_username(params, connection);

    GError *err = NULL;
    if (!pcv_webpush_subscribe(username, endpoint, p256dh, auth, &err)) {
                                                               
                                                                     
                                                           
                                                                   
                                                           
        gboolean client_error =
            g_error_matches(err, pcv_webpush_error_quark(), WP_ERR_ARG)
            || g_error_matches(err, pcv_webpush_error_quark(), WP_ERR_BLOCKED)
            || g_error_matches(err, pcv_webpush_error_quark(), WP_ERR_LIMIT);
        gint rpc_err = client_error ? PURE_RPC_ERR_INVALID_PARAMS : PURE_RPC_ERR_INTERNAL_ERROR;
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, rpc_err,
            err ? err->message : "구독 등록에 실패했습니다");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(username, "push.subscribe", ep_buf, "fail",
                      rpc_err, 0, "local");
        g_clear_error(&err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "subscribed");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    pcv_audit_log(username, "push.subscribe", ep_buf, "ok", 0, 0, "local");
}

                            
                                               
                         
                         
                                   
                                                                     
                                                     
void handle_push_unsubscribe(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    const gchar *endpoint = pcv_disp_json_string_member(params, "endpoint");
    if (!endpoint || !*endpoint) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "endpoint 는 비어 있지 않은 문자열이어야 합니다");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(pcv_rpc_completion_actor(), "push.unsubscribe", "", "fail",
                      PURE_RPC_ERR_INVALID_PARAMS, 0, "local");
        return;
    }

    gchar ep_buf[96];
    _push_ep_summary(ep_buf, sizeof ep_buf, endpoint);
    const gchar *username = _push_caller_username(params, connection);
    gint         role     = pcv_disp_dispatcher_caller_role(params, connection);
                                                          
    const gchar *scope    = (role >= PCV_ROLE_ADMIN) ? NULL : username;

    GError *err = NULL;
    if (!pcv_webpush_unsubscribe(endpoint, scope, &err)) {
                                                        
                                                      
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_NOT_FOUND,
            err ? err->message : "해당 구독을 찾을 수 없습니다");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(username, "push.unsubscribe", ep_buf, "fail",
                      PURE_RPC_ERR_NOT_FOUND, 0, "local");
        g_clear_error(&err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "unsubscribed");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    pcv_audit_log(username, "push.unsubscribe", ep_buf, "ok", 0, 0, "local");
}

                                              
                                                   
                                                        
                                                  
                                      
                                                   
                                                 
                                                                         
                                
                                                                  
                                                
                                                   
                                                  
void handle_push_mine(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    const gchar *username = _push_caller_username(params, connection);

                                                   
    JsonArray *arr = pcv_webpush_list_mine(username);
    guint      n   = json_array_get_length(arr);

    JsonObject *res = json_object_new();
    json_object_set_array_member(res, "subscriptions", arr);               
    json_object_set_int_member(res, "count", n);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                               
                                                     
                            
                                                                                                  
void handle_push_list(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    (void)params;
                                                   
    JsonArray *arr = pcv_webpush_list();
    JsonObject *res = json_object_new();
    json_object_set_array_member(res, "subscriptions", arr);               
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                    
                                               
                                             
                                                 
                                                               
void handle_push_vapid_rotate(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
                                                                
    const gchar *username = _push_caller_username(params, connection);

    GError *err     = NULL;
    guint   revoked = pcv_webpush_vapid_rotate(&err);
    if (err) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR, err->message);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(username, "push.vapid.rotate", "", "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        g_clear_error(&err);
        return;
    }

    gchar *key = pcv_webpush_vapid_public();
    if (!key) {
                                                    
                                                   
                                 
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            "VAPID 키 회전 후 새 공개키를 읽지 못했습니다");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(username, "push.vapid.rotate", "", "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_int_member(res, "revoked", (gint64)revoked);
    json_object_set_string_member(res, "key", key);
    g_free(key);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    gchar cnt_buf[32];
    g_snprintf(cnt_buf, sizeof cnt_buf, "revoked=%u", revoked);
    pcv_audit_log(username, "push.vapid.rotate", cnt_buf, "ok", 0, 0, "local");
}

                                              
                                                              
                             
                                      
                                                                  
void handle_push_test(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
                                                       
    const gchar *target = pcv_disp_json_string_member(params, "username");
    if (target && !*target) target = NULL;

    const gchar *username = _push_caller_username(params, connection);

    GError *err = NULL;
    if (!pcv_webpush_send_test(target, &err)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "테스트 알림 큐잉에 실패했습니다");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(username, "push.test", target ? target : "*", "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        g_clear_error(&err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "queued");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    pcv_audit_log(username, "push.test", target ? target : "*", "ok", 0, 0, "local");
}
