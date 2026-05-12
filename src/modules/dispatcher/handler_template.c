
















































#include "handler_template.h"
#include "rpc_utils.h"
#include "../../modules/template/vm_template.h"
#include "../../api/uds_server.h"
#include "../../utils/pcv_validate.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>





















static JsonObject *
_tmpl_to_json_obj(const PcvVmTemplate *t)
{
    JsonObject *obj = json_object_new();

    json_object_set_string_member(obj, "name", t->name ? t->name : "");
    json_object_set_int_member(obj, "vcpu", t->vcpu);
    json_object_set_int_member(obj, "memory_mb", t->memory_mb);
    json_object_set_int_member(obj, "disk_gb", t->disk_gb);
    json_object_set_string_member(obj, "os_variant",
                                  t->os_variant ? t->os_variant : "");

    if (t->iso_path)
        json_object_set_string_member(obj, "iso_path", t->iso_path);
    if (t->network_bridge)
        json_object_set_string_member(obj, "network_bridge", t->network_bridge);
    if (t->cloud_init_user_data)
        json_object_set_string_member(obj, "cloud_init_user_data",
                                      t->cloud_init_user_data);
    if (t->description)
        json_object_set_string_member(obj, "description", t->description);

    return obj;
}




















void
handle_template_list(JsonObject *params, const gchar *rpc_id,
                     UdsServer *server, GSocketConnection *conn)
{
    (void)params;

    GPtrArray *list = pcv_vm_template_list();

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < list->len; i++) {
        PcvVmTemplate *t = g_ptr_array_index(list, i);

        json_array_add_object_element(arr, _tmpl_to_json_obj(t));
    }

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);

    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);

    g_ptr_array_unref(list);
}


















void
handle_template_get(JsonObject *params, const gchar *rpc_id,
                    UdsServer *server, GSocketConnection *conn)
{
    if (!json_object_has_member(params, "name")) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }

    const gchar *name = json_object_get_string_member(params, "name");












    if (!name || name[0] == '\0' || !pcv_validate_vm_name(name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Parameter 'name' must be a valid identifier (alphanumeric, -, _)");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }







    PcvVmTemplate *t = pcv_vm_template_get(name);
    if (!t) {
        gchar *msg = g_strdup_printf("Template not found: %s", name);
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, msg);
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        g_free(msg);
        return;
    }

    JsonObject *obj = _tmpl_to_json_obj(t);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);

    pcv_vm_template_free(t);
}

























void
handle_template_create(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{









    const gchar *required[] = {"name", "vcpu", "memory_mb", "disk_gb", "os_variant"};
    for (gsize i = 0; i < G_N_ELEMENTS(required); i++) {
        if (!json_object_has_member(params, required[i])) {
            gchar *msg = g_strdup_printf("Missing required parameter: %s",
                                         required[i]);
            gchar *resp = pure_rpc_build_error_response(
                rpc_id, PURE_RPC_ERR_INVALID_PARAMS, msg);
            pure_uds_server_send_response(server, conn, resp);
            g_free(resp);
            g_free(msg);
            return;
        }
    }

    const gchar *name = json_object_get_string_member(params, "name");
    if (!name || name[0] == '\0' || !pcv_validate_vm_name(name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Parameter 'name' must be a valid identifier (alphanumeric, -, _)");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }












    PcvVmTemplate tmpl = {
        .name       = (gchar *)name,
        .vcpu       = (gint)json_object_get_int_member(params, "vcpu"),
        .memory_mb  = (gint)json_object_get_int_member(params, "memory_mb"),
        .disk_gb    = (gint)json_object_get_int_member(params, "disk_gb"),
        .os_variant = (gchar *)json_object_get_string_member(params, "os_variant"),
        .iso_path           = NULL,
        .network_bridge     = NULL,
        .cloud_init_user_data = NULL,
        .description        = NULL,
    };







    if (json_object_has_member(params, "iso_path"))
        tmpl.iso_path = (gchar *)json_object_get_string_member(params, "iso_path");
    if (json_object_has_member(params, "network_bridge"))
        tmpl.network_bridge = (gchar *)json_object_get_string_member(params, "network_bridge");
    if (json_object_has_member(params, "cloud_init_user_data"))
        tmpl.cloud_init_user_data = (gchar *)json_object_get_string_member(params, "cloud_init_user_data");
    if (json_object_has_member(params, "description"))
        tmpl.description = (gchar *)json_object_get_string_member(params, "description");














    if (tmpl.vcpu < 1 || tmpl.vcpu > 128) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "vcpu must be between 1 and 128");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }
    if (tmpl.memory_mb < 128 || tmpl.memory_mb > 1048576) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "memory_mb must be between 128 and 1048576");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }
    if (tmpl.disk_gb < 1 || tmpl.disk_gb > 65536) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "disk_gb must be between 1 and 65536");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    if (!pcv_vm_template_create(&tmpl, &err)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Template creation failed");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }


    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "created");
    json_object_set_string_member(res, "name", name);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);

    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}
















void
handle_template_delete(JsonObject *params, const gchar *rpc_id,
                       UdsServer *server, GSocketConnection *conn)
{
    if (!json_object_has_member(params, "name")) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }

    const gchar *name = json_object_get_string_member(params, "name");
    if (!name || name[0] == '\0' || !pcv_validate_vm_name(name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Parameter 'name' must be a valid identifier (alphanumeric, -, _)");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    if (!pcv_vm_template_delete(name, &err)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Template deletion failed");
        pure_uds_server_send_response(server, conn, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "deleted");
    json_object_set_string_member(res, "name", name);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);

    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, conn, resp);
    g_free(resp);
}
