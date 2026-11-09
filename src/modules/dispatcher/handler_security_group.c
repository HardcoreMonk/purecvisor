   
                                 
                                                           
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                     
   

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
#include "api/dispatcher_internal.h"


                                                                         
                            
  
                                                          
                                                       
                               
  
             
                                                      
                                             
  
           
                                              
                                       
                                    
                                                     
                                        
  
                 
                                                      
                                             
                                                                            
                                                                      
                                                      
void handle_security_group_create(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *sg_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!sg_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        extern gboolean pcv_security_group_create(const gchar *name, const gchar *description);
        const gchar *desc = json_object_has_member(params, "description")
            ? json_object_get_string_member(params, "description") : "";
        gboolean ok = pcv_security_group_create(sg_name, desc);
        if (ok) {
            JsonNode *node = json_node_new(JSON_NODE_VALUE);
            json_node_set_boolean(node, TRUE);
            gchar *resp = pure_rpc_build_success_response(rpc_id, node);
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
        } else {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Security group creation failed (already exists?)");
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
        }
    }
}

                                                                      
                                                 
void handle_security_group_list(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    extern JsonArray *pcv_security_group_list(void);
    JsonArray *arr = pcv_security_group_list();
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

                                                                      
                                      
void handle_security_group_delete(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *sg_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!sg_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        extern gboolean pcv_security_group_delete(const gchar *name);
        gboolean ok = pcv_security_group_delete(sg_name);
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, ok);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
}

                                                                      
                                                       
void handle_security_group_rule_add(JsonObject *params, const gchar *rpc_id,
                                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *sg_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!sg_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        extern gboolean pcv_security_group_rule_add(const gchar *name, JsonObject *rule);
        gboolean ok = pcv_security_group_rule_add(sg_name, params);
        if (ok) {
            JsonNode *node = json_node_new(JSON_NODE_VALUE);
            json_node_set_boolean(node, TRUE);
            gchar *resp = pure_rpc_build_success_response(rpc_id, node);
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
        } else {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Rule add failed (group not found?)");
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
        }
    }
}

                                                                      
                                         
void handle_security_group_rule_remove(JsonObject *params, const gchar *rpc_id,
                                                UdsServer *server, GSocketConnection *connection)
{
    const gchar *sg_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!sg_name || !json_object_has_member(params, "rule_id")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required params: name, rule_id");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }
    gint64 rule_id = json_object_get_int_member(params, "rule_id");
    extern gboolean pcv_security_group_rule_remove(const gchar *name, gint64 rule_id);
    gboolean ok = pcv_security_group_rule_remove(sg_name, rule_id);
    if (ok) {
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Rule remove failed (group/rule not found)");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
}

                                                                    
                                                            
                                                                       
                                                                     
                                                                        
void handle_security_group_detach(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm = json_object_has_member(params, "vm")
        ? json_object_get_string_member(params, "vm") : NULL;
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!vm || !name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required params: vm, name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    extern gboolean pcv_security_group_detach_vm(const gchar *vm, const gchar *sg_name);
    gboolean ok = pcv_security_group_detach_vm(vm, name);
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, ok);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                      
                                                                
                                                       
void handle_vm_security_group_set(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = json_object_has_member(params, "vm")
        ? json_object_get_string_member(params, "vm") : NULL;
    const gchar *sg_name = json_object_has_member(params, "security_group")
        ? json_object_get_string_member(params, "security_group") : NULL;
    if (!vm_name || !sg_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required params: vm, security_group");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        extern gboolean pcv_tenant_overlay_vm_in_any_tenant(const gchar *vm);
        if (pcv_tenant_overlay_vm_in_any_tenant(vm_name)) {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                "SG와 tenant-overlay는 상호 배타입니다: 암호 격리=tenant-overlay, "
                "per-VM L3/L4 정책=SG(bridge 모드) — overlay 멤버 VM에는 SG를 바인딩할 수 없습니다");
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp);
            return;
        }
        extern gboolean pcv_security_group_apply_to_vm(const gchar *vm, const gchar *sg);
        gboolean ok = pcv_security_group_apply_to_vm(vm_name, sg_name);
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, ok);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
}
