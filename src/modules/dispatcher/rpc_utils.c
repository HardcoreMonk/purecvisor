
















































#include "rpc_utils.h"

gboolean pcv_rpc_params_get_int_alias(JsonObject *params,
                                      const gchar *primary_key,
                                      const gchar *alias_key,
                                      gint *out_value)
{
    if (!params || !out_value) {
        return FALSE;
    }

    if (primary_key && json_object_has_member(params, primary_key)) {
        *out_value = json_object_get_int_member(params, primary_key);
        return TRUE;
    }

    if (alias_key && json_object_has_member(params, alias_key)) {
        *out_value = json_object_get_int_member(params, alias_key);
        return TRUE;
    }

    return FALSE;
}


















gchar* pure_rpc_build_error_response(const gchar *rpc_id,
                                     PureRpcErrorCode code,
                                     const gchar *message)
{
    JsonBuilder *builder = json_builder_new();

    json_builder_begin_object(builder);


    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");


    json_builder_set_member_name(builder, "error");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "code");
    json_builder_add_int_value(builder, (gint)code);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, message ? message : "Unknown error");
    json_builder_end_object(builder);


    json_builder_set_member_name(builder, "id");
    if (rpc_id != NULL) {
        json_builder_add_string_value(builder, rpc_id);
    } else {
        json_builder_add_null_value(builder);
    }

    json_builder_end_object(builder);


    JsonNode *root = json_builder_get_root(builder);
    gchar *raw_str = json_to_string(root, FALSE);








    gchar *response_str = g_strdup_printf("%s\n", raw_str);


    g_free(raw_str);
    json_node_free(root);
    g_object_unref(builder);

    return response_str;
}























gchar* pure_rpc_build_success_response(const gchar *rpc_id, JsonNode *result_node)
{

    JsonObject *root_obj = json_object_new();


    json_object_set_string_member(root_obj, "jsonrpc", "2.0");


    if (rpc_id != NULL) {
        json_object_set_string_member(root_obj, "id", rpc_id);
    } else {
        json_object_set_null_member(root_obj, "id");
    }






    if (result_node != NULL) {
        json_object_set_member(root_obj, "result", result_node);
    } else {
        json_object_set_null_member(root_obj, "result");
    }


    JsonNode *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, root_obj);


    gchar *raw_str = json_to_string(root_node, FALSE);





    gchar *response_str = g_strdup_printf("%s\n", raw_str);


    g_free(raw_str);
    json_node_free(root_node);

    return response_str;
}
