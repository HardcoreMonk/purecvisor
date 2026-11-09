   
                       
                                                                  
                                                                            
  
                           
                                                   
                                                    
                                        
  
                                                     
                                                  
                                                                        
                                                        
  
                                                       
                                                                 
                                                        
                                                             
  
                                                                               
            
                                                                               
                                       
           
           
                                                                   
           
           
                                                                                   
           
           
                                            
           
           
                                                                               
  
                                                                               
                                                                                  
                                                                               
                                                           
                                                                                            
                                                                      
  
                                                                 
                        
                                   
  
                                                         
                               
                               
  
                                                            
                                                                
                                                            
  
                                                                                           
                                                                                 
  
                                                                                 
                                                              
                                                            
                                                                     
                                                              
                                                             
                                             
  
                                                                               
                           
                                                                               
                      
                                          
  
                                                                               
                            
                                                                               
                                                    
                                                                      
                                                                 
                                                               
  
                                                                               
         
                                                                               
                                                                 
                                                                     
                        
                                                  
                                                              
                                                             
  
                                                                               
                            
                                                                               
                                           
                                              
                                           
                                                    
                                      
                                                  
                                                                                  
                                                                        
  
                                                                               
                                
                                                                               
                                                                        
                                                   
                                                           
                                                               
                                                               
                               
                                             
   

  
                     
  
                                                                
                                                                             
                                                                                         
                                                               
                                                            
                                                                                
                                                                   
                                                                          
                                                                 
   
#include "handler_auth.h"
#include "rpc_utils.h"
#include "../auth/pcv_rbac.h"
#include "../audit/pcv_audit.h"
#include "rpc_completion.h"                                  
#include "../daemons/pcv_webpush.h"                                              
#include "api/ws_server.h"                                                                 
#include "utils/pcv_validate.h"                                                         
#include "modules/resource/resource_registry.h"                                                            

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "modules/dispatcher/dispatcher_shared.h"
#include "utils/pcv_secure.h"
#include "utils/pcv_jwt.h"

                                                                  
                                                                 
                                                                  

   
                           
                                                                         
                                    
                                
                                           
  
                                                       
                                                    
                                         
  
                                              
                                             
                                      
  
                                                            
   
void
handle_auth_user_create(JsonObject       *params,
                        const gchar      *rpc_id,
                        UdsServer        *server,
                        GSocketConnection *connection)
{
      
                                                  
      
                                                            
                                                          
       
    if (!json_object_has_member(params, "username") ||
        !json_object_has_member(params, "password") ||
        !json_object_has_member(params, "role"))
    {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required params: username, password, role");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

      
                       
      
                                                                            
                                                 
                                         
                                                    
       
    const gchar *username = json_object_get_string_member(params, "username");
    const gchar *password = json_object_get_string_member(params, "password");
    const gchar *role_str = json_object_get_string_member(params, "role");

      
                                          
                                                       
                                                       
       
    const gchar *tenant   = json_object_has_member(params, "tenant")
                            ? json_object_get_string_member(params, "tenant")
                            : NULL;

      
                                                
      
                                                      
                                                      
                        
                                                          
       
    if (!username || !*username || !password || !*password || !role_str || !*role_str) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "username, password, and role must be non-empty strings");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

      
                                                                    
      
                                                                       
                                                           
                                                                         
                                                       
                                                                         
       
    if (!pcv_res_name_is_valid(username)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "username must match [A-Za-z0-9][A-Za-z0-9._-]{0,62}");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

      
                                                     
      
                                                                  
                                                  
                                                        
                                                  
       
    const gchar *pw_reason = NULL;
    if (!pcv_validate_password_complexity(password, &pw_reason)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            pw_reason ? pw_reason : "Password does not meet complexity policy");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

      
                                                        
      
                                                            
                                                     
                                              
      
                     
                                                                
                                                                
                                               
       
    if (g_ascii_strcasecmp(role_str, "admin") != 0 &&
        g_ascii_strcasecmp(role_str, "operator") != 0 &&
        g_ascii_strcasecmp(role_str, "viewer") != 0) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid role (must be: admin, operator, viewer)");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

      
                                  
                                      
                                                           
                                                                      
                                                                         
       
    PcvRole role = pcv_rbac_str_to_role(role_str);

      
                                   
      
                                  
                                                               
                                                                                  
                                                  
      
                                 
                                                
                                                                     
                                                          
       
    GError *err = NULL;
    gboolean ok = pcv_rbac_user_create(username, password, role, tenant, &err);

    if (!ok) {
        pcv_audit_log(pcv_rpc_completion_actor(), "auth.user.create", username, "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "User creation failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

      
                      
      
                  
                                                                                       
      
                                    
                                                                       
                                                                               
                                                                                
                                             
      
                                                
                                                                                             
       
                          
    pcv_audit_log(pcv_rpc_completion_actor(), "auth.user.create", username, "ok", 0, 0, "local");

    JsonObject *result_obj = json_object_new();
    json_object_set_string_member(result_obj, "username", username);
    json_object_set_string_member(result_obj, "role", pcv_rbac_role_to_str(role));
    if (tenant)
        json_object_set_string_member(result_obj, "tenant", tenant);
    else
        json_object_set_null_member(result_obj, "tenant");                    
    json_object_set_string_member(result_obj, "status", "created");

    JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result_node, result_obj);                          

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                  
                                                                 
                                                                  

   
                         
                             
                 
                       
                           
  
                                                              
                                                                                        
                                                                    
                                              
                                                          
   
void
handle_auth_user_list(JsonObject       *params,
                      const gchar      *rpc_id,
                      UdsServer        *server,
                      GSocketConnection *connection)
{
    (void)params;                                    

                                      
    GPtrArray *users = pcv_rbac_user_list();

      
                                
                                      
      
                                                                     
                                                              
      
                                                                      
                                        
       
    JsonArray *arr = json_array_new();
    for (guint i = 0; i < users->len; i++) {
        PcvUser *u = g_ptr_array_index(users, i);
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "username", u->username);
        json_object_set_string_member(obj, "role", pcv_rbac_role_to_str(u->role));
        if (u->tenant)
            json_object_set_string_member(obj, "tenant", u->tenant);
        else
            json_object_set_null_member(obj, "tenant");

          
                                                            
                                                
                                                                 
                                          
          
                                                             
                                                    
          
                                                                          
                                                           
                                                                     
          
                                                                                 
                                                          
           
        JsonObject *totp_obj = json_object_new();
        json_object_set_boolean_member(totp_obj, "enrolled", u->totp_enrolled);
        json_object_set_boolean_member(totp_obj, "confirmed", u->totp_confirmed);
        json_object_set_object_member(obj, "totp", totp_obj);                          

        json_array_add_object_element(arr, obj);                        
    }
    g_ptr_array_unref(users);                             

    JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result_node, arr);                         

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                  
                                                                 
                                                                  

   
                           
                                          
                 
                       
                           
  
                                                               
                                                  
                                       
                                                                 
                                                    
   
void
handle_auth_user_delete(JsonObject       *params,
                        const gchar      *rpc_id,
                        UdsServer        *server,
                        GSocketConnection *connection)
{
    if (!json_object_has_member(params, "username")) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required param: username");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    const gchar *username = json_object_get_string_member(params, "username");
    if (!username || !*username) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "username must be a non-empty string");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

      
                                                                             
                                            
                                                                         
                                                                          
                                                                         
       
    GError *err = NULL;
    gboolean ok = pcv_rbac_user_delete(username, &err);

    if (!ok) {
        pcv_audit_log(pcv_rpc_completion_actor(), "auth.user.delete", username, "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "User deletion failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

                          
    pcv_audit_log(pcv_rpc_completion_actor(), "auth.user.delete", username, "ok", 0, 0, "local");

                                                               
                                                         
                                            
                                                      
                                             
    guint push_revoked = pcv_webpush_remove_user(username);
    if (push_revoked > 0)
        g_message("[auth] 사용자 '%s' 삭제 — Web Push 구독 %u건 연쇄 삭제",
                  username, push_revoked);

                                                                                
                                                                    
    pcv_ws_close_subject(username);

    JsonObject *result_obj = json_object_new();
    json_object_set_string_member(result_obj, "username", username);
    json_object_set_string_member(result_obj, "status", "deleted");

    JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result_node, result_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                  
                                                                 
                                                                  

   
                        
                                                  
                 
                       
                           
  
                                                              
         
                             
                                                                
  
                           
                                                   
                                              
                                                               
                                                 
  
             
                                                       
                                                                       
                                                                              
                                                              
                                                      
                    
  
                                                                   
   
void
handle_auth_role_set(JsonObject       *params,
                     const gchar      *rpc_id,
                     UdsServer        *server,
                     GSocketConnection *connection)
{
    if (!json_object_has_member(params, "username") ||
        !json_object_has_member(params, "role"))
    {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required params: username, role");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    const gchar *username = json_object_get_string_member(params, "username");
    const gchar *role_str = json_object_get_string_member(params, "role");

    if (!username || !*username || !role_str || !*role_str) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "username and role must be non-empty strings");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

                                              
                                                      
                                   
    if (g_ascii_strcasecmp(role_str, "admin") != 0 &&
        g_ascii_strcasecmp(role_str, "operator") != 0 &&
        g_ascii_strcasecmp(role_str, "viewer") != 0) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid role (must be: admin, operator, viewer)");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    PcvRole role = pcv_rbac_str_to_role(role_str);

                                                         
                                                          
                                                                        
                                                                                    
                                                    
    const gchar *caller_sub = pcv_disp_dispatcher_caller_subject(params, connection);
    if (caller_sub && g_strcmp0(caller_sub, username) == 0) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Self role change is not permitted (B6-W4 self-elevation protection)");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(pcv_rpc_completion_actor(), "auth.role.set", username, "denied",
                      PURE_RPC_ERR_INVALID_PARAMS, 0, "self-elevation");
        return;
    }

      
                                                     
                                                       
                                                
       
    GError *err = NULL;
    PcvRole previous = PCV_ROLE_VIEWER;
    gboolean ok = pcv_rbac_user_set_role(username, role, &previous, &err);

    if (!ok) {
        pcv_audit_log(pcv_rpc_completion_actor(), "auth.role.set", username, "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Role update failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

                                                                                      
                                                
                                                                   
    g_autofree gchar *transition = g_strdup_printf("%s->%s", pcv_rbac_role_to_str(previous),
                                                   pcv_rbac_role_to_str(role));
    pcv_audit_log(pcv_rpc_completion_actor(), "auth.role.set", username, "ok", 0, 0, transition);

    JsonObject *result_obj = json_object_new();
    json_object_set_string_member(result_obj, "username", username);
    json_object_set_string_member(result_obj, "role", pcv_rbac_role_to_str(role));
    json_object_set_string_member(result_obj, "status", "updated");

    JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result_node, result_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                  
                                                                 
                                                                  

                                                       
static void
_tenant_set_error(const gchar *rpc_id, UdsServer *server, GSocketConnection *connection,
                  PureRpcErrorCode code, const gchar *message)
{
    gchar *resp = pure_rpc_build_error_response(rpc_id, code, message);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

   
                                                    
  
                                                                           
                                                                                      
                                                                                                              
                                                                                 
   
void
handle_auth_user_tenant_set(JsonObject       *params,
                            const gchar      *rpc_id,
                            UdsServer        *server,
                            GSocketConnection *connection)
{
    if (!json_object_has_member(params, "username") ||
        !json_object_has_member(params, "tenant")) {
        _tenant_set_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS,
                          "Missing required params: username, tenant");
        return;
    }
    const gchar *username = pcv_disp_json_string_member(params, "username");
    const gchar *tenant = pcv_disp_json_string_member(params, "tenant");
    if (!username || !*username || !tenant || !*tenant) {
        _tenant_set_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS,
                          "username and tenant must be non-empty strings");
        return;
    }
    if (!pcv_res_name_is_valid(tenant)) {
        _tenant_set_error(rpc_id, server, connection, PURE_RPC_ERR_INVALID_PARAMS,
                          "tenant must match [A-Za-z0-9][A-Za-z0-9._-]{0,62}");
        return;
    }

    switch (pcv_rbac_user_set_tenant_if_empty(username, tenant)) {
    case PCV_RBAC_TENANT_SET:
        break;
    case PCV_RBAC_TENANT_ALREADY:
        _tenant_set_error(rpc_id, server, connection, PURE_RPC_ERR_CONFLICT,
                          "tenant already set — moving a user between tenants is not supported");
        return;
    case PCV_RBAC_TENANT_NO_USER: {
        g_autofree gchar *msg = g_strdup_printf("User '%s' not found", username);
        _tenant_set_error(rpc_id, server, connection, PURE_RPC_ERR_NOT_FOUND, msg);
        return;
    }
    case PCV_RBAC_TENANT_ERROR:
    default:
        _tenant_set_error(rpc_id, server, connection, PURE_RPC_ERR_INTERNAL_ERROR, "Tenant update failed");
        return;
    }

    JsonObject *result_obj = json_object_new();
    json_object_set_string_member(result_obj, "username", username);
    json_object_set_string_member(result_obj, "tenant", tenant);
    JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result_node, result_obj);                           
    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                  
                                                               
                                                                  

   
                                                                                  
                                                   
                                                           
                                                                  
                                                     
                                                      
                                                                      
  
                                   
                                                                  
                                         
                                                                        
                                              
                                                               
                               
  
                                                         
   
typedef enum {
    PCV_TOTP_SCOPE_OK = 0,
    PCV_TOTP_SCOPE_MISSING_USERNAME,                                    
    PCV_TOTP_SCOPE_FORBIDDEN,                                         
} PcvTotpScopeResult;

static PcvTotpScopeResult
_totp_self_scope_resolve(JsonObject *params, const gchar **out_target,
                         const gchar **out_caller_sub, const gchar **out_denied_req)
{
                                                            
                                                            
                                                      
    const gchar *caller_sub = json_object_get_string_member_with_default(
        params, "_pcv_caller_sub", NULL);
    const gchar *req_username = json_object_get_string_member_with_default(
        params, "username", "");

    if (out_caller_sub) *out_caller_sub = caller_sub;
    if (out_denied_req) *out_denied_req = req_username;

    if (caller_sub && *caller_sub) {
        if (*req_username && g_strcmp0(req_username, caller_sub) != 0) {
            return PCV_TOTP_SCOPE_FORBIDDEN;
        }
        *out_target = caller_sub;
        return PCV_TOTP_SCOPE_OK;
    }

                                                          
                                                           
    if (!*req_username) {
        return PCV_TOTP_SCOPE_MISSING_USERNAME;
    }
    *out_target = req_username;
    return PCV_TOTP_SCOPE_OK;
}

   
                           
                                           
                 
                       
                           
  
                                                 
                                            
   
void
handle_auth_totp_status(JsonObject       *params,
                        const gchar      *rpc_id,
                        UdsServer        *server,
                        GSocketConnection *connection)
{
    const gchar *target = NULL, *caller_sub = NULL, *denied_req = NULL;
    PcvTotpScopeResult scope = _totp_self_scope_resolve(params, &target, &caller_sub, &denied_req);

    if (scope == PCV_TOTP_SCOPE_MISSING_USERNAME) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "username is required (no verified caller identity on this channel)");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (scope == PCV_TOTP_SCOPE_FORBIDDEN) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_FORBIDDEN,
            "본인 계정의 TOTP 상태만 조회할 수 있습니다");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(caller_sub, "auth.totp.status", denied_req, "denied",
                      PURE_RPC_ERR_FORBIDDEN, 0, "local");
        return;
    }

                                                               
    PcvTotpStatus st = { 0 };
    pcv_rbac_totp_status(target, &st);

    JsonObject *result_obj = json_object_new();
    json_object_set_string_member(result_obj, "username", target);
    json_object_set_boolean_member(result_obj, "enrolled", st.enrolled);
    json_object_set_boolean_member(result_obj, "confirmed", st.confirmed);
    json_object_set_int_member(result_obj, "recovery_remaining", st.recovery_remaining);

    JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result_node, result_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    pcv_audit_log(caller_sub, "auth.totp.status", target, "ok", 0, 0, "local");
}

   
                            
                                                   
                 
                       
                           
  
                                                  
                                                           
                                             
   
void
handle_auth_totp_disable(JsonObject       *params,
                         const gchar      *rpc_id,
                         UdsServer        *server,
                         GSocketConnection *connection)
{
    if (!json_object_has_member(params, "code")) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: code");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    const gchar *code = json_object_get_string_member(params, "code");
    if (!code || !*code) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "code must be a non-empty string");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    const gchar *target = NULL, *caller_sub = NULL, *denied_req = NULL;
    PcvTotpScopeResult scope = _totp_self_scope_resolve(params, &target, &caller_sub, &denied_req);

    if (scope == PCV_TOTP_SCOPE_MISSING_USERNAME) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "username is required (no verified caller identity on this channel)");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (scope == PCV_TOTP_SCOPE_FORBIDDEN) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_FORBIDDEN,
            "본인 계정의 TOTP만 비활성화할 수 있습니다");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(caller_sub, "auth.totp.disable", denied_req, "denied",
                      PURE_RPC_ERR_FORBIDDEN, 0, "local");
        return;
    }

                                                                  
                                                
                                                                       
    GError *verr = NULL;
    if (!pcv_rbac_totp_verify_code(target, code, FALSE, NULL, &verr)) {
        gint lock_sec = pcv_rbac_totp_get_remaining_lockout(target);
        gint err_code = (lock_sec > 0) ? PURE_RPC_ERR_TOTP_LOCKED : PURE_RPC_ERR_TOTP_INVALID_CODE;
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, err_code, verr ? verr->message : "Invalid TOTP code");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(caller_sub, "auth.totp.disable", target, "fail", err_code, 0, "local");
        g_clear_error(&verr);
        return;
    }

    GError *err = NULL;
    if (!pcv_rbac_totp_disable(target, &err)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "TOTP disable failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(caller_sub, "auth.totp.disable", target, "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        if (err) g_error_free(err);
        return;
    }

    JsonObject *result_obj = json_object_new();
    json_object_set_string_member(result_obj, "username", target);
    json_object_set_string_member(result_obj, "status", "disabled");

    JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result_node, result_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    pcv_audit_log(caller_sub, "auth.totp.disable", target, "ok", 0, 0, "local");
}

   
                          
                                          
                 
                       
                           
  
                                                
                                                      
                                                          
                                                               
                                      
   
void
handle_auth_totp_reset(JsonObject       *params,
                       const gchar      *rpc_id,
                       UdsServer        *server,
                       GSocketConnection *connection)
{
    if (!json_object_has_member(params, "username")) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: username");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    const gchar *username = json_object_get_string_member(params, "username");
    if (!username || !*username) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "username must be a non-empty string");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

                                                   
    const gchar *caller_sub = json_object_get_string_member_with_default(
        params, "_pcv_caller_sub", NULL);

    GError *err = NULL;
    if (!pcv_rbac_totp_disable(username, &err)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "TOTP reset failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(caller_sub, "auth.totp.reset", username, "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        if (err) g_error_free(err);
        return;
    }

    JsonObject *result_obj = json_object_new();
    json_object_set_string_member(result_obj, "username", username);
    json_object_set_string_member(result_obj, "status", "reset");

    JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result_node, result_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    pcv_audit_log(caller_sub, "auth.totp.reset", username, "ok", 0, 0, "local");
}

   
                                        
                                                   
                 
                       
                           
  
                                              
                                                       
                                                   
                                                 
   
void
handle_auth_totp_recovery_regenerate(JsonObject       *params,
                                     const gchar      *rpc_id,
                                     UdsServer        *server,
                                     GSocketConnection *connection)
{
    if (!json_object_has_member(params, "code")) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: code");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    const gchar *code = json_object_get_string_member(params, "code");
    if (!code || !*code) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "code must be a non-empty string");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    const gchar *target = NULL, *caller_sub = NULL, *denied_req = NULL;
    PcvTotpScopeResult scope = _totp_self_scope_resolve(params, &target, &caller_sub, &denied_req);

    if (scope == PCV_TOTP_SCOPE_MISSING_USERNAME) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "username is required (no verified caller identity on this channel)");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (scope == PCV_TOTP_SCOPE_FORBIDDEN) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_FORBIDDEN,
            "본인 계정의 비상코드만 재발급할 수 있습니다");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(caller_sub, "auth.totp.recovery.regenerate", denied_req, "denied",
                      PURE_RPC_ERR_FORBIDDEN, 0, "local");
        return;
    }

                                                                       
    GError *verr = NULL;
    if (!pcv_rbac_totp_verify_code(target, code, FALSE, NULL, &verr)) {
        gint lock_sec = pcv_rbac_totp_get_remaining_lockout(target);
        gint err_code = (lock_sec > 0) ? PURE_RPC_ERR_TOTP_LOCKED : PURE_RPC_ERR_TOTP_INVALID_CODE;
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, err_code, verr ? verr->message : "Invalid TOTP code");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(caller_sub, "auth.totp.recovery.regenerate", target, "fail",
                      err_code, 0, "local");
        g_clear_error(&verr);
        return;
    }

    GError    *err = NULL;
    GPtrArray *recovery = pcv_rbac_totp_generate_recovery(target, &err);
    if (!recovery) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Recovery code regeneration failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        pcv_audit_log(caller_sub, "auth.totp.recovery.regenerate", target, "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        if (err) g_error_free(err);
        return;
    }

                                                           
    JsonArray *arr = json_array_new();
    for (guint i = 0; i < recovery->len; i++) {
        json_array_add_string_element(arr, (const gchar *)g_ptr_array_index(recovery, i));
    }
    g_ptr_array_unref(recovery);

    JsonObject *result_obj = json_object_new();
    json_object_set_string_member(result_obj, "username", target);
    json_object_set_array_member(result_obj, "recovery_codes", arr);                                 

    JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result_node, result_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    pcv_audit_log(caller_sub, "auth.totp.recovery.regenerate", target, "ok", 0, 0, "local");
}


                                                                        
                                                                  
                                             
                                              
                                                                   
                                
                            
                                                           
                               
static gint64 _apikey_parse_expires_at(JsonObject *params)
{
    if (!params || !json_object_has_member(params, "expires_at")) return 0;
    JsonNode *node = json_object_get_member(params, "expires_at");
    if (!node || JSON_NODE_HOLDS_NULL(node) || !JSON_NODE_HOLDS_VALUE(node)) return 0;

    GType vt = json_node_get_value_type(node);
    if (vt == G_TYPE_INT64)  return json_node_get_int(node);
    if (vt == G_TYPE_DOUBLE) return (gint64)json_node_get_double(node);
    if (vt == G_TYPE_STRING) {
        const gchar *s = json_node_get_string(node);
        if (!s || !*s) return 0;
        gchar *end = NULL;
        gint64 v = g_ascii_strtoll(s, &end, 10);
        if (end && *end == '\0') return v;                    
        GDateTime *dt = g_date_time_new_from_iso8601(s, NULL);
        if (dt) { gint64 u = g_date_time_to_unix(dt); g_date_time_unref(dt); return u; }
    }
    return 0;
}

                                       
                                                          
                                                        
                                
                                                                                      
                                                              
                                                         
                                                                     
void handle_apikey_create(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
                                                   
    const gchar *client_name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!client_name || !*client_name)
        client_name = params ? json_object_get_string_member_with_default(params, "client_name", NULL) : NULL;
    gint role = (params && json_object_has_member(params, "role"))
        ? (gint)json_object_get_int_member(params, "role") : 1;
    const gchar *description = params
        ? json_object_get_string_member_with_default(params, "description", NULL) : NULL;
    gint64 expires_at = _apikey_parse_expires_at(params);
    if (!client_name || !*client_name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
                                                                          
                                                          
                                                               
                                                                    
                                                             
    if (role < PCV_ROLE_VIEWER || role > PCV_ROLE_ADMIN) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "role out of range (0=viewer, 1=operator, 2=admin)");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    {
        gint caller_role = pcv_disp_dispatcher_caller_role(params, connection);
        if (role > caller_role) {
            gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "requested key role exceeds caller role");
            pure_uds_server_send_response(server, connection, r); g_free(r); return;
        }
    }
    gchar *key_out = nullptr;
    GError *err = nullptr;
    if (!pcv_rbac_apikey_create(client_name, (PcvRole)role, description, expires_at, &key_out, &err)) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err ? err->message : "Create failed");
        pure_uds_server_send_response(server, connection, r); g_free(r);
        if (err) g_error_free(err);
        return;
    }
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "api_key", key_out);
    json_object_set_string_member(res, "client_name", client_name);
    if (description) json_object_set_string_member(res, "description", description);
    if (expires_at > 0) json_object_set_int_member(res, "expires_at", expires_at);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r);
                                                                        
    g_free(r); pcv_secure_free_str(&key_out);
}

                                                          
                                                                  
void handle_apikey_list(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = pcv_rbac_apikey_list();
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

                                              
                                                                    
void handle_apikey_revoke(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    const gchar *cn = params ? json_object_get_string_member_with_default(params, "client_name", NULL) : NULL;
    if (!cn || !*cn) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "client_name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    GError *err = nullptr;
    if (!pcv_rbac_apikey_revoke(cn, &err)) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err ? err->message : "Revoke failed");
        pure_uds_server_send_response(server, connection, r); g_free(r);
        if (err) g_error_free(err);
        return;
    }
    JsonNode *n = json_node_new(JSON_NODE_NULL);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

                                        
                                                        
                                                                
                                                                                           
void handle_session_revoke(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    const gchar *jti = params ? json_object_get_string_member_with_default(params, "jti", NULL) : NULL;
    if (!jti) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "jti required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
                                                                      
                                                            
                                                            
    gint64 exp = g_get_real_time() / G_USEC_PER_SEC + 900;
    pcv_jwt_blacklist_add(jti, exp);
    gchar jti_masked[9];
    g_snprintf(jti_masked, sizeof(jti_masked), "%.8s", jti);
    const gchar *caller_sub = pcv_disp_dispatcher_caller_subject(params, connection);
    pcv_audit_log(caller_sub && *caller_sub ? caller_sub : "-",
                  "auth.session.revoke", jti_masked, "ok", 0, 0, "local");
    JsonNode *n = json_node_new(JSON_NODE_NULL);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

                                                                                      
                                                            
                                                                   
                                                                                
                                                                    
                                                                 
                                                                   
void handle_user_sessions_revoke(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *username = params
        ? json_object_get_string_member_with_default(params, "username", NULL) : NULL;
    if (!username || !*username) {
        gchar *r = pure_rpc_build_error_response(rpc_id,
            PURE_RPC_ERR_INVALID_PARAMS, "username required");
        pure_uds_server_send_response(server, connection, r); g_free(r);
        return;
    }
    const gchar *caller = pcv_disp_dispatcher_caller_subject(params, connection);
    GError *err = nullptr;
    gboolean ok = pcv_rbac_revoke_session(username, &err);
    if (!ok) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "Failed to revoke sessions");
        pure_uds_server_send_response(server, connection, r); g_free(r);
        pcv_audit_log((caller && *caller) ? caller : "-",
                      "auth.user.sessions.revoke", username, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        if (err) g_error_free(err);
        return;
    }
    pcv_audit_log((caller && *caller) ? caller : "-",
                  "auth.user.sessions.revoke", username, "ok", 0, 0, "local");
    JsonNode *n = json_node_new(JSON_NODE_NULL);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}
