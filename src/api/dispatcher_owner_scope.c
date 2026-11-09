   
                                 
                                                              
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                        
   

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
#include "modules/dispatcher/dispatcher_shared.h"
#include "api/dispatcher_owner_scope.h"

                                                           
                                                       
#define PCV_VM_METADATA_URI "urn:purecvisor:metadata"


                                                       
                                                            
                                           
                                                     
static const gchar *
_vm_name_from_params(JsonObject *params)
{
    const gchar *name = pcv_disp_json_string_member(params, "name");
    if (name && *name) return name;

    name = pcv_disp_json_string_member(params, "vm_id");
    if (name && *name) return name;

    name = pcv_disp_json_string_member(params, "vm_name");
    if (name && *name) return name;

    name = pcv_disp_json_string_member(params, "vm");
    if (name && *name) return name;

    return NULL;
}

                                                                        
                                                
                                                             
                                         
const gchar *
pcv_disp_vm_owner_scope_target_from_params(const gchar *method, JsonObject *params)
{
                                                                   
    if (g_strcmp0(method, "vm.inspect") == 0)
        return pcv_disp_json_string_member(params, "uuid");
                
                                               
                                                     
                                                          
    if (g_strcmp0(method, "vm.clone") == 0) {
        const gchar *source = pcv_disp_json_string_member(params, "source");
        if (source && *source)
            return source;
    }

    return _vm_name_from_params(params);
}

                                                                           
  
            
                                                  
                                               
                                                  
  
                  
                                                
                                                          
                                                   
                                
                                                          
   

                                                                         
                                                         
                                                  
                                                         
                                                          
static gchar *
_xml_find_owner_node(xmlNodePtr node)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {                 
        if (cur->type == XML_ELEMENT_NODE &&
            cur->name &&
            xmlStrcmp(cur->name, BAD_CAST "owner") == 0) {
            xmlChar *content = xmlNodeGetContent(cur);
            if (!content)
                return NULL;
            gchar *owner = g_strdup((const gchar *)content);
            xmlFree(content);
            return owner;
        }

        gchar *child_owner = _xml_find_owner_node(cur->children);
        if (child_owner)
            return child_owner;
    }
    return NULL;
}

                                                   
                                                 
                                                             
                                                        
static gchar *
_vm_owner_from_xml(const gchar *xml)
{
    if (!xml || !*xml)
        return NULL;

    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), "pcv-vm-metadata.xml",
                                  NULL, XML_PARSE_NONET | XML_PARSE_NOERROR |
                                                XML_PARSE_NOWARNING);
    if (!doc)
        return NULL;

    gchar *owner = _xml_find_owner_node(xmlDocGetRootElement(doc));
    xmlFreeDoc(doc);

    if (owner)
        g_strstrip(owner);
    if (owner && *owner)
        return owner;
    g_free(owner);
    return NULL;
}

                                                     
                                                         
                                                        
                                                                      
                                                                           
static gchar *
_lookup_vm_owner(const gchar *vm_name)
{
    if (!vm_name || !*vm_name)
        return NULL;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn)
        return NULL;

    gchar *owner = NULL;
    virDomainPtr dom = virDomainLookupByName(conn, vm_name);
    if (!dom) {
        virResetLastError();
        dom = virDomainLookupByUUIDString(conn, vm_name);
    }
    if (!dom) {
        virt_conn_pool_release(conn);
        return NULL;
    }

                                                         
                                                 
                                         
    char *metadata = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                          PCV_VM_METADATA_URI,
                                          VIR_DOMAIN_AFFECT_CONFIG);
    if (!metadata) {
        metadata = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                        PCV_VM_METADATA_URI, 0);
    }
    if (metadata) {
        owner = _vm_owner_from_xml(metadata);
        g_free(metadata);
    }

    if (!owner) {
        char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        if (!xml)
            xml = virDomainGetXMLDesc(dom, 0);
        if (xml) {
            owner = _vm_owner_from_xml(xml);
            g_free(xml);
        }
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
    return owner;
}

                                                                  
                                                      
                                     
                                                              
                                                 
static gboolean
_vm_owner_matches_caller(const gchar *vm_name,
                         const gchar *caller_sub,
                         gchar **deny_message)
{
    if (!vm_name || !*vm_name) {
        if (deny_message)
            *deny_message = g_strdup("Missing required parameter: name/vm_id");
        return FALSE;
    }

                
                                                        
                                              
                    
    gchar *owner = _lookup_vm_owner(vm_name);
    gboolean allowed = (owner && g_strcmp0(owner, caller_sub) == 0);
    g_free(owner);

    if (!allowed && deny_message) {
        *deny_message = g_strdup(
            "Permission denied: operators can access only VMs they created");
    }
    return allowed;
}

                                                                    
                                                 
                                                   
                                                                    
static gboolean
_vm_batch_owner_scoped_allowed(JsonObject *params,
                               const gchar *caller_sub,
                               gchar **deny_message)
{
    JsonArray *vms = (params && json_object_has_member(params, "vms"))
        ? json_object_get_array_member(params, "vms") : NULL;

    if (!vms) {
        if (deny_message)
            *deny_message = g_strdup("Missing required parameter: vms");
        return FALSE;
    }

                                        
                                               
                                                  
    guint len = json_array_get_length(vms);
    for (guint i = 0; i < len; i++) {
        JsonNode *node = json_array_get_element(vms, i);
        if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING) {
            if (deny_message)
                *deny_message = g_strdup("Invalid parameter: vms must contain VM names");
            return FALSE;
        }

        const gchar *vm_name = json_node_get_string(node);
        if (!_vm_owner_matches_caller(vm_name, caller_sub, deny_message))
            return FALSE;
    }

    return TRUE;
}

                                                                            
                                                        
                                                        
                                                            
                                                     
gboolean
pcv_disp_vm_owner_scoped_method_allowed(const gchar *method,
                                JsonObject *params,
                                GSocketConnection *connection,
                                gint caller_role,
                                gchar **deny_message)
{
                                             
                                                 
                                       
    if (caller_role >= PCV_ROLE_ADMIN)
        return TRUE;

    if (caller_role < PCV_ROLE_OPERATOR) {
        if (deny_message)
            *deny_message = g_strdup("Permission denied: insufficient role for this method");
        return FALSE;
    }

    const gchar *caller_sub = pcv_disp_dispatcher_caller_subject(params, connection);
    if (!caller_sub || !*caller_sub) {
        if (deny_message)
            *deny_message = g_strdup("Permission denied: missing authenticated subject");
        return FALSE;
    }

    if (g_strcmp0(method, "vm.batch") == 0)
        return _vm_batch_owner_scoped_allowed(params, caller_sub, deny_message);

    const gchar *vm_name = pcv_disp_vm_owner_scope_target_from_params(method, params);
    return _vm_owner_matches_caller(vm_name, caller_sub, deny_message);
}

                                                                         
  
            
                                                   
                                                          
                                 
  
            
                                                                 
                                                                    
                                                                             
                                                                       
                                                             
                                                                                         
                                                                    
                                                        
                                                                
   

                                                             
                                                                          
                                          
static const gchar *
_container_name_from_params(JsonObject *params)
{
    const gchar *name = pcv_disp_json_string_member(params, "name");
    if (name && *name) return name;

    name = pcv_disp_json_string_member(params, "container");
    if (name && *name) return name;

    name = pcv_disp_json_string_member(params, "container_id");
    if (name && *name) return name;

    return NULL;
}

                                                                           
                                                                    
                                           
const gchar *
pcv_disp_container_owner_scope_target_from_params(const gchar *method, JsonObject *params)
{
                                                              
                                                        
                                         
    if (g_strcmp0(method, "container.clone") == 0) {
        const gchar *source = pcv_disp_json_string_member(params, "source");
        if (source && *source)
            return source;
    }

    return _container_name_from_params(params);
}

                                                                                     
                                                   
                                                          
                                                              
const gchar *
pcv_disp_dispatcher_audit_target_from_params(const gchar *method, JsonObject *params)
{
    const gchar *target = NULL;
    if (g_str_has_prefix(method ? method : "", "container."))
        target = pcv_disp_container_owner_scope_target_from_params(method, params);
    else
        target = pcv_disp_vm_owner_scope_target_from_params(method, params);
    if (target && *target) return target;

    static const gchar *keys[] = {
        "username", "network", "bridge", "tenant", "event_id", "job_id",
        "target_node",                                                 
        NULL
    };
    for (guint i = 0; keys[i]; i++) {
        target = pcv_disp_json_string_member(params, keys[i]);
        if (target && *target) return target;
    }
    return NULL;
}

                                                                                
                                                                                                 
                                                        
                                                
gboolean
pcv_disp_container_method_requires_owner_scope(const gchar *method)
{
    if (!method)
        return FALSE;

                                                                    
                                                           
                                                                                            
                                                                                 
                                                                                               
                     
    return g_strcmp0(method, "container.start") == 0 ||
           g_strcmp0(method, "container.stop") == 0 ||
           g_strcmp0(method, "container.logs") == 0 ||
           g_strcmp0(method, "container.memory.stats") == 0 ||
           g_strcmp0(method, "container.health.check") == 0 ||
           g_strcmp0(method, "container.clone") == 0;
}

                                                       
                                                                 
                                                          
static gchar *
_lookup_container_owner(const gchar *name)
{
                                                            
    return pcv_lxc_read_owner(name);
}

                                                                      
                                                                 
                                                               
static gboolean
_container_owner_matches_caller(const gchar *name,
                                const gchar *caller_sub,
                                gchar **deny_message)
{
    if (!name || !*name) {
        if (deny_message)
            *deny_message = g_strdup("Missing required parameter: name");
        return FALSE;
    }

                                                          
                                              
    gchar *owner = _lookup_container_owner(name);
    gboolean allowed = (owner && g_strcmp0(owner, caller_sub) == 0);
    g_free(owner);

    if (!allowed && deny_message) {
        *deny_message = g_strdup(
            "Permission denied: operators can access only containers they created");
    }
    return allowed;
}

                                                                                     
                                                                                           
                                                               
gboolean
pcv_disp_container_owner_scoped_method_allowed(const gchar *method,
                                       JsonObject *params,
                                       GSocketConnection *connection,
                                       gint caller_role,
                                       gchar **deny_message)
{
                                             
                                                           
    if (caller_role >= PCV_ROLE_ADMIN)
        return TRUE;

    if (caller_role < PCV_ROLE_OPERATOR) {
        if (deny_message)
            *deny_message = g_strdup("Permission denied: insufficient role for this method");
        return FALSE;
    }

    const gchar *caller_sub = pcv_disp_dispatcher_caller_subject(params, connection);
    if (!caller_sub || !*caller_sub) {
        if (deny_message)
            *deny_message = g_strdup("Permission denied: missing authenticated subject");
        return FALSE;
    }

    const gchar *name = pcv_disp_container_owner_scope_target_from_params(method, params);
    return _container_owner_matches_caller(name, caller_sub, deny_message);
}
