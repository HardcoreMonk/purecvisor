   
                       
                                                           
  
                           
                                                   
                                                    
                                        
  
                                                     
                                                  
                                                            
                                                        
  
                                                       
                                                                 
                                                        
                                                             
  
                                                                               
            
                                                                               
                                       
           
           
                                                                   
           
           
                                                                              
           
           
                                      
           
           
                                                                   
  
                                                                               
                                                   
                                                                               
                                                           
                                                                                            
                                                          
  
                                                                 
                        
                                   
  
                                                         
                               
                               
  
                                                            
                                                                
                                                            
  
                                                                                 
                                                              
                                                            
                                                                     
                                                              
                                                             
                                             
  
                                                                               
                           
                                                                               
                      
                                          
  
                                                                               
                               
                                                                               
                                                    
                                                          
                                                                 
                                                               
  
                                                                               
         
                                                                               
                                                                 
                                                                     
                        
                                                  
                                                              
                                                             
  
                                                                               
                            
                                                                               
                                           
                                              
                                           
                                                    
                                      
                                                  
                                                                                  
                                                                        
  
                                                                               
                                
                                                                               
                                                                        
                                                   
                                                           
                                                               
                                                               
                               
                                             
   

  
                     
  
                                                                   
                                                                             
                                                                                         
                                                               
                                                            
                                                                                
                                                                   
                                                                          
                                                                 
   
#include "handler_auth.h"
#include "rpc_utils.h"
#include "../auth/pcv_rbac.h"
#include "../audit/pcv_audit.h"
#include "../daemons/pcv_webpush.h"                                              
#include "utils/pcv_validate.h"                                                         

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

                                                                  
                                                                 
                                                                  

   
                           
                                                                         
                                    
                                
                                           
  
                                                       
                                                    
                                         
  
                                              
                                             
                                      
  
                                                            
   
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
        pcv_audit_log(NULL, "auth.user.create", username, "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "User creation failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

      
                      
      
                  
                                                                                       
      
                                    
                                                                       
                                                                               
                                                                                
                                             
      
                                                
                                                                                             
       
                          
    pcv_audit_log(NULL, "auth.user.create", username, "ok", 0, 0, "local");

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
        pcv_audit_log(NULL, "auth.user.delete", username, "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "User deletion failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

                          
    pcv_audit_log(NULL, "auth.user.delete", username, "ok", 0, 0, "local");

                                                               
                                                         
                                            
                                                      
                                             
    guint push_revoked = pcv_webpush_remove_user(username);
    if (push_revoked > 0)
        g_message("[auth] 사용자 '%s' 삭제 — Web Push 구독 %u건 연쇄 삭제",
                  username, push_revoked);

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

                                                         
                                                          
                                                                
                                                                 
                                                                 
    if (connection) {
        const gchar *caller_sub = g_object_get_data(G_OBJECT(connection), "pcv-caller-sub");
        if (caller_sub && g_strcmp0(caller_sub, username) == 0) {
            gchar *resp = pure_rpc_build_error_response(
                rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "Self role change is not permitted (B6-W4 self-elevation protection)");
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp);
            pcv_audit_log(NULL, "auth.role.set", username, "denied",
                          PURE_RPC_ERR_INVALID_PARAMS, 0, "self-elevation");
            return;
        }
    }

      
                                                     
                                                       
                                                
       
    GError *err = NULL;
    gboolean ok = pcv_rbac_user_set_role(username, role, &err);

    if (!ok) {
        pcv_audit_log(NULL, "auth.role.set", username, "fail",
                      PURE_RPC_ERR_INTERNAL_ERROR, 0, "local");
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Role update failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

                         
    pcv_audit_log(NULL, "auth.role.set", username, "ok", 0, 0, "local");

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
