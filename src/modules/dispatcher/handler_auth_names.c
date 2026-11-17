   
                             
                                                                 
  
                           
                                                                                           
                                                                                    
  
                                                                       
   
#include "handler_auth_names.h"

#include "modules/auth/pcv_principal_names.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/dispatcher/dispatcher_shared.h"
#include "modules/resource/resource_registry.h"                                    

                                                                        
PureRpcErrorCode
pcv_auth_principal_rpc_code(const GError *err, PureRpcErrorCode fallback)
{
    if (!err || err->domain != PCV_PRINCIPAL_ERROR)
        return fallback;
    switch ((PcvPrincipalError)err->code) {
    case PCV_PRINCIPAL_ERROR_RESERVED:
        return PURE_RPC_ERR_INVALID_PARAMS;
    case PCV_PRINCIPAL_ERROR_KEY_NAME:
    case PCV_PRINCIPAL_ERROR_USER_NAME:
    case PCV_PRINCIPAL_ERROR_DELETED:
    case PCV_PRINCIPAL_ERROR_EXISTING_USER:
        return PURE_RPC_ERR_CONFLICT;
    }
    return fallback;
}

                                                             
static void
_send_error(UdsServer *server, GSocketConnection *connection, const gchar *rpc_id, PureRpcErrorCode code, const gchar *msg)
{
    gchar *resp = pure_rpc_build_error_response(rpc_id, code, msg);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                                                                        
void
handle_auth_deleted_name_add(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    if (!params || !json_object_has_member(params, "username")) {
        _send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: username");
        return;
    }
    const gchar *username = pcv_disp_json_string_member(params, "username");
    if (!username || !*username) {
        _send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "username must be a non-empty string");
        return;
    }
    if (!pcv_res_name_is_valid(username)) {
        _send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                    "username must match [A-Za-z0-9][A-Za-z0-9._-]{0,62}");
        return;
    }
    gboolean added = FALSE;
    g_autoptr(GError) err = NULL;
    if (!pcv_rbac_deleted_name_add(username, &added, &err)) {
        _send_error(server, connection, rpc_id, pcv_auth_principal_rpc_code(err, PURE_RPC_ERR_INTERNAL_ERROR),
                    err ? err->message : "deleted name add failed");
        return;
    }
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "username", username);
    json_object_set_boolean_member(res, "added", added);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);                           
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}
