   
                            
                                        
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                                            
   

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
#include "utils/pcv_ssrf.h"
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
#include "modules/dispatcher/dispatcher_shared.h"


                                                              
                                              
                                                        
                                                   
const gchar *
pcv_disp_json_string_member(JsonObject *params, const gchar *key)
{
    if (!params || !key || !json_object_has_member(params, key))
        return NULL;

    JsonNode *node = json_object_get_member(params, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return NULL;
    if (json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(node);
}

                                                                          
                                                    
                                                            
                                                                   
                                                    
                                              
gboolean
pcv_disp_valid_vm_storage_pool(const gchar *pool)
{
    if (!pool || !*pool || strlen(pool) > 255)                           
        return FALSE;
    if (pool[0] == '/' || pool[strlen(pool) - 1] == '/' ||
        strstr(pool, "..") || strstr(pool, "//") || strchr(pool, '@'))                
        return FALSE;

    gboolean prev_slash = FALSE;
    for (const gchar *p = pool; *p; p++) {                               
        if (*p == '/') {
            if (prev_slash)                                              
                return FALSE;
            prev_slash = TRUE;
            continue;
        }
        prev_slash = FALSE;
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-' && *p != '.')                   
            return FALSE;
    }
    return TRUE;
}

                                                                   
                                                       
                                              
                                                                 
gboolean
pcv_disp_json_int_member(JsonObject *params, const gchar *key, gint *out)
{
    if (!params || !key || !out || !json_object_has_member(params, key))
        return FALSE;

    JsonNode *node = json_object_get_member(params, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return FALSE;

    GType value_type = json_node_get_value_type(node);
    if (value_type != G_TYPE_INT64 && value_type != G_TYPE_INT &&
        value_type != G_TYPE_LONG && value_type != G_TYPE_UINT &&
        value_type != G_TYPE_UINT64)
        return FALSE;

    *out = (gint)json_node_get_int(node);
    return TRUE;
}

                                                                  
                                              
                                                  
                                                         
                                                           
const gchar *
pcv_disp_dispatcher_caller_subject(JsonObject *params, GSocketConnection *connection)
{
    if (connection) {                                      
        const gchar *sub = g_object_get_data(G_OBJECT(connection), "pcv-caller-sub");
        if (sub && *sub) return sub;
    }
    return pcv_disp_json_string_member(params, "_pcv_caller_sub");                     
}

                                                            
                                                   
                                    
                                                                        
                                                                                  
gint
pcv_disp_dispatcher_caller_role(JsonObject *params, GSocketConnection *connection)
{
    if (connection) {
        gpointer rdata = g_object_get_data(G_OBJECT(connection), "pcv-caller-role");
        if (rdata) return GPOINTER_TO_INT(rdata);
    }

    gint role = PCV_ROLE_ADMIN;                          
    if (pcv_disp_json_int_member(params, "_pcv_caller_role", &role)) {
        if (role < PCV_ROLE_VIEWER || role > PCV_ROLE_ADMIN)
            return PCV_ROLE_VIEWER;
        return role;
    }
    return role;
}

                                                                
                                                                                                 
                                                     
void
pcv_disp_webhook_member_for_caller(JsonObject *object, const gchar *member,
                                   JsonObject *params, GSocketConnection *connection)
{
    if (!object || pcv_disp_dispatcher_caller_role(params, connection) >= PCV_ROLE_ADMIN)
        return;
    const gchar *url = pcv_disp_json_string_member(object, member);
    if (!url)
        return;
    g_autofree gchar *origin = pcv_url_redact_origin(url);
    json_object_set_string_member(object, member, origin);
}

                                                                        
                                                   
                                                                                    


                                                            
                                                             
                                         
  
                     
                                                                                    
                      
                            
  
                                                    
                                                        
  
                                                   
                                      
                                             
                                                                      
                                                               
                                                                        
JsonNode *
pcv_disp_paginate_array(JsonArray *full_array, gint offset, gint limit)
{
    gint total = (gint)json_array_get_length(full_array);

    if (limit <= 0) {
                                                         
        JsonNode *node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(node, full_array);                    
        return node;
    }

                                        
    if (offset < 0) offset = 0;
    if (offset > total) offset = total;

    JsonArray *paged = json_array_new();
    for (gint i = offset; i < total && i < offset + limit; i++) {                                      
        JsonNode *elem = json_array_dup_element(full_array, (guint)i);
        json_array_add_element(paged, elem);
    }

    JsonObject *result = json_object_new();
    json_object_set_array_member(result, "items", paged);
    json_object_set_int_member(result, "total", total);
    json_object_set_int_member(result, "offset", offset);
    json_object_set_int_member(result, "limit", limit);
    json_object_set_boolean_member(result, "has_more", offset + limit < total);

    json_array_unref(full_array);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    return node;
}

   
                                  
                                             
                                         
                                                   
                                                 
   
void
pcv_disp_get_pagination_params(JsonObject *params, gint *out_offset, gint *out_limit)
{
    *out_offset = (params && json_object_has_member(params, "offset"))
        ? (gint)json_object_get_int_member(params, "offset") : 0;
    *out_limit = (params && json_object_has_member(params, "limit"))
        ? (gint)json_object_get_int_member(params, "limit") : 0;
}

   
                                                                      
  
                                                                     
                                              
                                                           
                                                
  
                                                  
                                               
                                       
  
                                              
                                              
                                                     
                                          
                                                         
                                              
                                 
                                                        
                                                     
                          
   
void
pcv_disp_wipe_json_string_member(JsonObject *object, const gchar *key)
{
    if (!object || !json_object_has_member(object, key))
        return;
    JsonNode *node = json_object_get_member(object, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node)
        || json_node_get_value_type(node) != G_TYPE_STRING) {
        return;
    }
    const gchar *value = json_node_get_string(node);
    if (value)
        pcv_secure_wipe((gpointer)value, strlen(value));
    json_object_set_string_member(object, key, "");
}
