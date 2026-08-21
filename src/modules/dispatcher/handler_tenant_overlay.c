   
                                                                         
  
                           
                                                   
                                                    
                                        
  
                                                   
                                                   
                                             
                                                         
  
                                                                    
                                                        
                                                        
                                                       
                                                           
                            
                                                                              
                                          
                                                        
                                                                  
                                                              
                                                               
   
#include "handler_tenant_overlay.h"
#include "rpc_utils.h"

#include "../network/tenant_overlay.h"
#include "../../api/uds_server.h"
#include "../../utils/pcv_validate.h"

  
                                                                            
                                                                
                                                                 
                                                                
   

   
                                                                           
                                                          
                                                                            
                                                           
   
static const gchar *
json_string_default(JsonObject *params, const gchar *key, const gchar *def)
{
                                                   
    if (!params || !key || !json_object_has_member(params, key)) {
        return def;
    }
    JsonNode *node = json_object_get_member(params, key);
                                                   
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING) {
        return def;
    }
    const gchar *value = json_node_get_string(node);
    return value ? value : def;                                    
}

   
                                                                   
                                                              
                            
                                        
                                                                        
                                                                          
                                                                         
                                                    
   
gboolean
pcv_tenant_overlay_rpc_validate(JsonObject *params,
                                gboolean require_vm,
                                const gchar **tenant_out,
                                const gchar **vm_out,
                                GError **error)
{
                                                               
    if (tenant_out) *tenant_out = NULL;
    if (vm_out) *vm_out = NULL;

    const gchar *tenant = json_string_default(params, "tenant", NULL);
    if (!tenant || !*tenant) {                       
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "tenant is required");
        return FALSE;
    }
                                                                   
    if (!pcv_validate_vm_name(tenant)) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "tenant must match [a-zA-Z0-9_-]{1,%d}", PCV_MAX_VM_NAME);
        return FALSE;
    }
    if (tenant_out) *tenant_out = tenant;                          

                                                                    
    if (!require_vm) {
        return TRUE;
    }

    const gchar *vm = json_string_default(params, "vm", NULL);
    if (!vm || !*vm) {                                          
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "vm is required");
        return FALSE;
    }
    if (!pcv_validate_vm_name(vm)) {                              
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "vm must match [a-zA-Z0-9_-]{1,%d}", PCV_MAX_VM_NAME);
        return FALSE;
    }
    if (vm_out) *vm_out = vm;

    return TRUE;
}

  
                                                                 
                                                             
                                                               
                                            
   

                                                             
static void
send_error(UdsServer *server,
           GSocketConnection *connection,
           const gchar *rpc_id,
           PureRpcErrorCode code,
           const gchar *message)
{
    gchar *resp = pure_rpc_build_error_response(rpc_id, code, message);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                  
static void
send_success_object(UdsServer *server,
                    GSocketConnection *connection,
                    const gchar *rpc_id,
                    JsonObject *obj)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);                                         
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                        
static void
send_success_array(UdsServer *server,
                   GSocketConnection *connection,
                   const gchar *rpc_id,
                   JsonArray *arr)
{
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);                                         
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                           
static void
send_success_boolean(UdsServer *server,
                     GSocketConnection *connection,
                     const gchar *rpc_id,
                     gboolean value)
{
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, value);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

   
                                                                             
                                                              
                                                                      
                                                                 
                                                                       
   
void
handle_tenant_overlay_create(JsonObject *params,
                             const gchar *rpc_id,
                             UdsServer *server,
                             GSocketConnection *connection)
{
    const gchar *tenant = NULL;
    GError *verr = NULL;
                                                                       
    if (!pcv_tenant_overlay_rpc_validate(params, FALSE, &tenant, NULL, &verr)) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                  verr ? verr->message : "invalid params");
        g_clear_error(&verr);
        return;
    }

    GError *error = NULL;
                                                  
    if (!pcv_tenant_overlay_create(tenant, &error)) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_CONFLICT,
                  error ? error->message : "tenant create failed");
        g_clear_error(&error);
        return;
    }

                                                                          
    gchar *subnet = NULL;
    pcv_tenant_overlay_get_subnet(tenant, &subnet);
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "tenant", tenant);
    json_object_set_string_member(obj, "subnet", subnet ? subnet : "");                      
    g_free(subnet);                                                  
    send_success_object(server, connection, rpc_id, obj);                            
}

   
                                                                            
                                                               
                                                                              
                       
   
void
handle_tenant_overlay_delete(JsonObject *params,
                             const gchar *rpc_id,
                             UdsServer *server,
                             GSocketConnection *connection)
{
    const gchar *tenant = NULL;
    GError *verr = NULL;
    if (!pcv_tenant_overlay_rpc_validate(params, FALSE, &tenant, NULL, &verr)) {                   
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                  verr ? verr->message : "invalid params");
        g_clear_error(&verr);
        return;
    }

    GError *error = NULL;
                                                         
    if (!pcv_tenant_overlay_delete(tenant, &error)) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_CONFLICT,
                  error ? error->message : "tenant delete failed");
        g_clear_error(&error);
        return;
    }

    send_success_boolean(server, connection, rpc_id, TRUE);                              
}

   
                                                                           
                                                      
                                                                              
   
void
handle_tenant_overlay_list(JsonObject *params __attribute__((unused)),
                           const gchar *rpc_id,
                           UdsServer *server,
                           GSocketConnection *connection)
{
    GPtrArray *names = pcv_tenant_overlay_list();                                 
    JsonArray *arr = json_array_new();
                                                      
    for (guint i = 0; i < names->len; i++) {
        const gchar *name = g_ptr_array_index(names, i);
        gchar *subnet = NULL;
        pcv_tenant_overlay_get_subnet(name, &subnet);                                       

        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "tenant", name);
        json_object_set_string_member(obj, "subnet", subnet ? subnet : "");
        g_free(subnet);                               

        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);                              
        json_array_add_element(arr, node);                             
    }
    g_ptr_array_unref(names);                                    

    send_success_array(server, connection, rpc_id, arr);
}

   
                                                                         
                                                   
                                                                                
                                 
   
void
handle_tenant_overlay_get(JsonObject *params,
                          const gchar *rpc_id,
                          UdsServer *server,
                          GSocketConnection *connection)
{
    const gchar *tenant = NULL;
    GError *verr = NULL;
    if (!pcv_tenant_overlay_rpc_validate(params, FALSE, &tenant, NULL, &verr)) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                  verr ? verr->message : "invalid params");
        g_clear_error(&verr);
        return;
    }

                                                                          
    gchar *subnet = NULL;
    if (!pcv_tenant_overlay_get_subnet(tenant, &subnet)) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
                  "tenant not found");
        return;
    }

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "tenant", tenant);
    json_object_set_string_member(obj, "subnet", subnet);                           
    g_free(subnet);
    send_success_object(server, connection, rpc_id, obj);
}

   
                                                                                      
                                                            
                                                                        
                                                                             
   
void
handle_tenant_overlay_attach_vm(JsonObject *params,
                                const gchar *rpc_id,
                                UdsServer *server,
                                GSocketConnection *connection)
{
    const gchar *tenant = NULL, *vm = NULL;
    GError *verr = NULL;
                                                                     
    if (!pcv_tenant_overlay_rpc_validate(params, TRUE, &tenant, &vm, &verr)) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                  verr ? verr->message : "invalid params");
        g_clear_error(&verr);
        return;
    }

    GError *error = NULL;
                                                                      
    gchar *overlay_ip = pcv_tenant_overlay_attach_vm(tenant, vm, &error);
    if (!overlay_ip) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                  error ? error->message : "attach_vm failed");
        g_clear_error(&error);
        return;
    }

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "tenant", tenant);
    json_object_set_string_member(obj, "vm", vm);
    json_object_set_string_member(obj, "overlay_ip", overlay_ip);
    g_free(overlay_ip);                                     
    send_success_object(server, connection, rpc_id, obj);
}

   
                                                                                   
                                               
                                                                        
                                                          
   
void
handle_tenant_overlay_detach_vm(JsonObject *params,
                                const gchar *rpc_id,
                                UdsServer *server,
                                GSocketConnection *connection)
{
    const gchar *tenant = NULL, *vm = NULL;
    GError *verr = NULL;
    if (!pcv_tenant_overlay_rpc_validate(params, TRUE, &tenant, &vm, &verr)) {                     
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                  verr ? verr->message : "invalid params");
        g_clear_error(&verr);
        return;
    }

    GError *error = NULL;
                                                        
    if (!pcv_tenant_overlay_detach_vm(tenant, vm, &error)) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                  error ? error->message : "detach_vm failed");
        g_clear_error(&error);
        return;
    }

    send_success_boolean(server, connection, rpc_id, TRUE);
}
