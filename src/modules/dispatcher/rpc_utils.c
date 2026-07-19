
#include "rpc_utils.h"
#include <string.h>

gboolean pcv_rpc_json_depth_ok(const gchar *json, gint max_depth)
{
    if (!json) return TRUE;
    gint depth = 0;
    gboolean in_str = FALSE, esc = FALSE;
    for (const gchar *p = json; *p; p++) {
        gchar c = *p;
        if (in_str) {
            if (esc)            esc = FALSE;
            else if (c == '\\') esc = TRUE;
            else if (c == '"')  in_str = FALSE;
            continue;
        }
        if (c == '"')                   in_str = TRUE;
        else if (c == '[' || c == '{') { if (++depth > max_depth) return FALSE; }
        else if (c == ']' || c == '}')  { if (depth > 0) depth--; }
    }
    return TRUE;
}

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

gboolean pcv_rpc_parse_guarded(const gchar *data, gssize len,
                               JsonParser **parser, GError **err)
{
    if (parser) *parser = NULL;
    if (!data) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "null JSON input");
        return FALSE;
    }
    gsize n = (len < 0) ? strlen(data) : (gsize)len;
    if (n > PCV_RPC_JSON_MAX_BYTES) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "JSON payload too large");
        return FALSE;
    }

    gchar *buf = g_strndup(data, n);
    if (!pcv_rpc_json_depth_ok(buf, PCV_RPC_JSON_MAX_DEPTH)) {
        g_free(buf);
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "JSON nesting too deep");
        return FALSE;
    }
    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_data(p, buf, (gssize)n, err)) {
        g_object_unref(p);
        g_free(buf);
        return FALSE;
    }
    g_free(buf);
    if (parser) *parser = p;
    else        g_object_unref(p);
    return TRUE;
}
