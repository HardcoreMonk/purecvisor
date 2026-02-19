// src/modules/dispatcher/rpc_utils.c

#include "rpc_utils.h"

gchar* pure_rpc_build_error_response(const gchar *rpc_id, 
                                     PureRpcErrorCode code, 
                                     const gchar *message)
{
    JsonBuilder *builder = json_builder_new();
    
    json_builder_begin_object(builder);
    
    // 1. JSON-RPC Version
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");
    
    // 2. Error Object
    json_builder_set_member_name(builder, "error");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "code");
    json_builder_add_int_value(builder, (gint)code);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, message ? message : "Unknown error");
    json_builder_end_object(builder); 
    
    // 3. ID 
    json_builder_set_member_name(builder, "id");
    if (rpc_id != NULL) {
        json_builder_add_string_value(builder, rpc_id);
    } else {
        json_builder_add_null_value(builder);
    }
    
    json_builder_end_object(builder); 
    
    // 4. Serialize to string
    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    
    gsize length;
    gchar *raw_str = json_generator_to_data(gen, &length);
    
    // 5. 개행 문자(\n) 덧붙이기 (UX 및 파싱 안정성 확보)
    gchar *response_str = g_strdup_printf("%s\n", raw_str);
    
    // 6. Cleanup
    g_free(raw_str); // 원본 JSON 문자열 메모리 해제
    json_node_free(root);
    g_object_unref(gen);
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
    
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root_node);
    
    gsize length;
    gchar *raw_str = json_generator_to_data(gen, &length);
    
    // 개행 문자(\n) 덧붙이기
    gchar *response_str = g_strdup_printf("%s\n", raw_str);
    
    // 메모리 해제
    g_free(raw_str);
    json_node_free(root_node); 
    g_object_unref(gen);
    
    return response_str; 
}