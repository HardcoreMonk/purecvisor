
































































































#include "handler_auth.h"
#include "rpc_utils.h"
#include "../auth/pcv_rbac.h"
#include "../audit/pcv_audit.h"

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
