                                                                                             
                                                                                                 
                                                               
                                                                            
                        
   
                         
                                   
  
                                                                               
                 
                                                                               
                                                                    
                            
  
                                      
                                                                                         
                                                                                          
  
             
                                                              
                            
                                                       
                                                
                          
  
             
                                           
                               
                             
  
                                                          
                                                      
                                                                               
   
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "../src/modules/dispatcher/rpc_utils.h"

                                                       

static JsonObject *parse_resp(const gchar *json) {
    JsonParser *p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, json, -1, NULL));
    JsonNode *root = json_parser_get_root(p);
    JsonObject *obj = json_object_ref(json_node_get_object(root));
    g_object_unref(p);
    return obj;
}

static void test_error_response_basic(void) {
    gchar *r = pure_rpc_build_error_response("rpc-1", PURE_RPC_ERR_INVALID_PARAMS, "vm_id required");
    g_assert_nonnull(r);
    JsonObject *o = parse_resp(r);
    g_assert_cmpstr(json_object_get_string_member(o, "jsonrpc"), ==, "2.0");
    g_assert_cmpstr(json_object_get_string_member(o, "id"), ==, "rpc-1");
    JsonObject *err = json_object_get_object_member(o, "error");
    g_assert_nonnull(err);
    g_assert_cmpint((gint)json_object_get_int_member(err, "code"), ==, PURE_RPC_ERR_INVALID_PARAMS);
    g_assert_cmpstr(json_object_get_string_member(err, "message"), ==, "vm_id required");
    json_object_unref(o);
    g_free(r);
}

static void test_error_response_null_id(void) {
    gchar *r = pure_rpc_build_error_response(NULL, PURE_RPC_ERR_PARSE_ERROR, "bad json");
    g_assert_nonnull(r);
                          
    g_assert_nonnull(g_strstr_len(r, -1, "\"id\""));
    g_free(r);
}

static void test_error_response_all_codes(void) {
    PureRpcErrorCode codes[] = {
        PURE_RPC_ERR_PARSE_ERROR,
        PURE_RPC_ERR_INVALID_REQUEST,
        PURE_RPC_ERR_METHOD_NOT_FOUND,
        PURE_RPC_ERR_INVALID_PARAMS,
        PURE_RPC_ERR_INTERNAL_ERROR,
        PURE_RPC_ERR_ZFS_OPERATION,
        PURE_RPC_ERR_VM_NOT_FOUND,
        PURE_RPC_ERR_CONFLICT,
        PURE_RPC_ERR_TIMEOUT,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(codes); i++) {
        gchar *r = pure_rpc_build_error_response("test", codes[i], "msg");
        g_assert_nonnull(r);
        JsonObject *o = parse_resp(r);
        JsonObject *err = json_object_get_object_member(o, "error");
        g_assert_cmpint((gint)json_object_get_int_member(err, "code"), ==, codes[i]);
        json_object_unref(o);
        g_free(r);
    }
}

static void test_error_response_special_chars_message(void) {
                        
    gchar *r = pure_rpc_build_error_response("id", PURE_RPC_ERR_INTERNAL_ERROR,
        "error with \"quotes\" and \\ backslash");
    g_assert_nonnull(r);
    JsonObject *o = parse_resp(r);
    JsonObject *err = json_object_get_object_member(o, "error");
                               
    g_assert_cmpstr(json_object_get_string_member(err, "message"), ==,
                    "error with \"quotes\" and \\ backslash");
    json_object_unref(o);
    g_free(r);
}

static void test_error_response_null_message(void) {
    gchar *r = pure_rpc_build_error_response("id", PURE_RPC_ERR_INTERNAL_ERROR, NULL);
                                       
    g_assert_nonnull(r);
    g_free(r);
}

                                                       

static void test_success_response_object_result(void) {
    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "status", "ok");
    json_object_set_int_member(result, "count", 42);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);

    gchar *r = pure_rpc_build_success_response("req-1", node);
    g_assert_nonnull(r);
    JsonObject *o = parse_resp(r);
    g_assert_cmpstr(json_object_get_string_member(o, "jsonrpc"), ==, "2.0");
    g_assert_cmpstr(json_object_get_string_member(o, "id"), ==, "req-1");
    JsonObject *res = json_object_get_object_member(o, "result");
    g_assert_nonnull(res);
    g_assert_cmpstr(json_object_get_string_member(res, "status"), ==, "ok");
    g_assert_cmpint((gint)json_object_get_int_member(res, "count"), ==, 42);
    json_object_unref(o);
    g_free(r);
                                                  
}

static void test_success_response_array_result(void) {
    JsonArray *arr = json_array_new();
    json_array_add_string_element(arr, "vm-a");
    json_array_add_string_element(arr, "vm-b");
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);

    gchar *r = pure_rpc_build_success_response("req-2", node);
    g_assert_nonnull(r);
    JsonObject *o = parse_resp(r);
    JsonArray *res = json_object_get_array_member(o, "result");
    g_assert_cmpuint(json_array_get_length(res), ==, 2);
    json_object_unref(o);
    g_free(r);
}

static void test_success_response_null_id(void) {
    JsonObject *result = json_object_new();
    json_object_set_boolean_member(result, "ok", TRUE);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);

    gchar *r = pure_rpc_build_success_response(NULL, node);
    g_assert_nonnull(r);
    g_assert_nonnull(g_strstr_len(r, -1, "\"id\""));
    g_free(r);
}

static void test_success_response_int_id(void) {
    JsonObject *result = json_object_new();
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);

                         
    gchar *r = pure_rpc_build_success_response("12345", node);
    JsonObject *o = parse_resp(r);
    g_assert_cmpstr(json_object_get_string_member(o, "id"), ==, "12345");
    json_object_unref(o);
    g_free(r);
}

static void test_params_get_int_alias_primary_key(void) {
    JsonObject *params = json_object_new();
    json_object_set_int_member(params, "vcpu_count", 6);
    json_object_set_int_member(params, "vcpu", 4);

    gint value = 0;
    g_assert_true(pcv_rpc_params_get_int_alias(params, "vcpu_count", "vcpu", &value));
    g_assert_cmpint(value, ==, 6);

    json_object_unref(params);
}

static void test_params_get_int_alias_alias_key(void) {
    JsonObject *params = json_object_new();
    json_object_set_int_member(params, "vcpu", 4);

    gint value = 0;
    g_assert_true(pcv_rpc_params_get_int_alias(params, "vcpu_count", "vcpu", &value));
    g_assert_cmpint(value, ==, 4);

    json_object_unref(params);
}

static void test_params_get_int_alias_missing_key(void) {
    JsonObject *params = json_object_new();
    json_object_set_int_member(params, "memory_mb", 4096);

    gint value = 123;
    g_assert_false(pcv_rpc_params_get_int_alias(params, "vcpu_count", "vcpu", &value));
    g_assert_cmpint(value, ==, 123);

    json_object_unref(params);
}




static void test_params_get_string_type_safe(void) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser,
        "{\"s\":\"/mnt\",\"e\":\"\",\"n\":null,\"i\":7,\"o\":{\"x\":1},\"a\":[\"/mnt\"]}",
        -1, NULL));
    JsonObject *params = json_node_get_object(json_parser_get_root(parser));


    GLogLevelFlags saved = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

    g_assert_cmpstr(pcv_rpc_params_get_string(params, "s"), ==, "/mnt");
    g_assert_cmpstr(pcv_rpc_params_get_string(params, "e"), ==, "");
    g_assert_null(pcv_rpc_params_get_string(params, "n"));
    g_assert_null(pcv_rpc_params_get_string(params, "i"));
    g_assert_null(pcv_rpc_params_get_string(params, "o"));
    g_assert_null(pcv_rpc_params_get_string(params, "a"));
    g_assert_null(pcv_rpc_params_get_string(params, "missing"));
    g_assert_null(pcv_rpc_params_get_string(params, NULL));
    g_assert_null(pcv_rpc_params_get_string(NULL, "s"));
    g_log_set_always_fatal(saved);
}


static void test_params_get_uint_in_range(void) {
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser,
        "{\"ok\":57,\"lo\":1,\"hi\":86400,\"zero\":0,\"neg\":-5,\"big\":86401,"
        "\"s\":\"57\",\"f\":57.5,\"b\":true,\"n\":null,\"o\":{\"x\":1}}",
        -1, NULL));
    JsonObject *params = json_node_get_object(json_parser_get_root(parser));
    GLogLevelFlags saved = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    guint out = 0;

    g_assert_true(pcv_rpc_params_get_uint_in_range(params, "ok", 1, 86400, 30, &out));
    g_assert_cmpuint(out, ==, 57);
    g_assert_true(pcv_rpc_params_get_uint_in_range(params, "lo", 1, 86400, 30, &out));
    g_assert_cmpuint(out, ==, 1);
    g_assert_true(pcv_rpc_params_get_uint_in_range(params, "hi", 1, 86400, 30, &out));
    g_assert_cmpuint(out, ==, 86400);
    g_assert_true(pcv_rpc_params_get_uint_in_range(params, "missing", 1, 86400, 30, &out));
    g_assert_cmpuint(out, ==, 30);
    out = 99;
    g_assert_true(pcv_rpc_params_get_uint_in_range(NULL, "ok", 1, 86400, 30, &out));
    g_assert_cmpuint(out, ==, 30);

    const gchar *invalid[] = { "zero", "neg", "big", "s", "f", "b", "n", "o" };
    for (gsize i = 0; i < G_N_ELEMENTS(invalid); i++) {
        out = 4242;
        g_assert_false(pcv_rpc_params_get_uint_in_range(params, invalid[i], 1, 86400, 30, &out));
        g_assert_cmpuint(out, ==, 4242);
    }
    g_assert_false(pcv_rpc_params_get_uint_in_range(params, NULL, 1, 86400, 30, &out));
    g_assert_false(pcv_rpc_params_get_uint_in_range(params, "ok", 1, 86400, 30, NULL));
    g_log_set_always_fatal(saved);
}

static void test_params_get_bool_type_safe(void) {
    JsonParser *pr = json_parser_new();
    g_assert_true(json_parser_load_from_data(pr,
        "{\"t\":true,\"f\":false,\"s\":\"true\",\"n\":1,\"z\":null,\"o\":{}}", -1, NULL));
    JsonObject *p = json_node_get_object(json_parser_get_root(pr));
    gboolean v = FALSE;
    g_assert_true(pcv_rpc_params_get_bool(p, "t", FALSE, &v));       g_assert_true(v);
    g_assert_true(pcv_rpc_params_get_bool(p, "f", TRUE, &v));        g_assert_false(v);
    v = FALSE;
    g_assert_true(pcv_rpc_params_get_bool(p, "missing", TRUE, &v));  g_assert_true(v);
    v = FALSE;
    g_assert_true(pcv_rpc_params_get_bool(NULL, "t", TRUE, &v));     g_assert_true(v);
    for (const gchar *const *k = (const gchar *const[]){ "s", "n", "z", "o", NULL }; *k; k++) {
        v = TRUE;
        g_assert_false(pcv_rpc_params_get_bool(p, *k, FALSE, &v));
        g_assert_true(v);
    }
    g_assert_false(pcv_rpc_params_get_bool(p, "t", TRUE, NULL));
    g_object_unref(pr);
}

static void test_params_is_object_array(void) {
    JsonParser *pr = json_parser_new();
    g_assert_true(json_parser_load_from_data(pr,
        "{\"ok\":[{\"name\":\"Claude\"},{}],\"empty\":[],\"o\":{\"Claude\":{}},\"s\":\"x\",\"n\":1,\"z\":null,"
        "\"strs\":[\"x\"],\"mixed\":[{},1],\"nulls\":[null]}", -1, NULL));
    JsonObject *p = json_node_get_object(json_parser_get_root(pr));
    g_assert_true(pcv_rpc_params_is_object_array(p, "ok"));
    g_assert_true(pcv_rpc_params_is_object_array(p, "empty"));
    for (const gchar *const *k = (const gchar *const[]){ "o", "s", "n", "z", "strs", "mixed", "nulls", "missing", NULL }; *k; k++)
        g_assert_false(pcv_rpc_params_is_object_array(p, *k));
    g_assert_false(pcv_rpc_params_is_object_array(NULL, "ok"));
    g_assert_false(pcv_rpc_params_is_object_array(p, NULL));
    g_object_unref(pr);
}

static gboolean source_cb_remove(gpointer data) {
    (*(gint *)data)++;
    return G_SOURCE_REMOVE;
}

static void test_source_remove_fired_not_removed_again(void) {
    gint count = 0;
    guint id = g_timeout_add(0, source_cb_remove, &count);
    g_assert_cmpuint(id, >, 0);
    for (gint i = 0; i < 50 && count == 0; i++)
        g_main_context_iteration(NULL, TRUE);
    g_assert_cmpint(count, ==, 1);
    GLogLevelFlags saved = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    gboolean removed = pcv_source_remove_if_attached(id);
    g_log_set_always_fatal(saved);
    g_assert_false(removed);
    g_assert_cmpint(count, ==, 1);
}

static void test_source_remove_pending_removed(void) {
    gint count = 0;
    guint id = g_timeout_add_seconds(30, source_cb_remove, &count);
    g_assert_cmpuint(id, >, 0);
    g_assert_true(pcv_source_remove_if_attached(id));
    for (gint i = 0; i < 5; i++)
        g_main_context_iteration(NULL, FALSE);
    g_assert_cmpint(count, ==, 0);
    g_assert_false(pcv_source_remove_if_attached(id));
}

static void test_source_remove_zero_id(void) {
    g_assert_false(pcv_source_remove_if_attached(0));
}

void test_rpc_utils_register(void) {
    g_test_add_func("/rpc_utils/source_remove/fired_source_not_removed_again", test_source_remove_fired_not_removed_again);
    g_test_add_func("/rpc_utils/source_remove/pending_source_removed", test_source_remove_pending_removed);
    g_test_add_func("/rpc_utils/source_remove/zero_id", test_source_remove_zero_id);
    g_test_add_func("/rpc_utils/params_get_uint_in_range", test_params_get_uint_in_range);
    g_test_add_func("/rpc_utils/params_get_bool_type_safe", test_params_get_bool_type_safe);
    g_test_add_func("/rpc_utils/params_get_string_type_safe", test_params_get_string_type_safe);
    g_test_add_func("/rpc_utils/error_response_basic", test_error_response_basic);
    g_test_add_func("/rpc_utils/error_response_null_id", test_error_response_null_id);
    g_test_add_func("/rpc_utils/error_response_all_codes", test_error_response_all_codes);
    g_test_add_func("/rpc_utils/error_response_special_chars_message", test_error_response_special_chars_message);
    g_test_add_func("/rpc_utils/error_response_null_message", test_error_response_null_message);
    g_test_add_func("/rpc_utils/success_response_object_result", test_success_response_object_result);
    g_test_add_func("/rpc_utils/success_response_array_result", test_success_response_array_result);
    g_test_add_func("/rpc_utils/success_response_null_id", test_success_response_null_id);
    g_test_add_func("/rpc_utils/success_response_int_id", test_success_response_int_id);
    g_test_add_func("/rpc_utils/params_get_int_alias_primary_key", test_params_get_int_alias_primary_key);
    g_test_add_func("/rpc_utils/params_get_int_alias_alias_key", test_params_get_int_alias_alias_key);
    g_test_add_func("/rpc_utils/params_get_int_alias_missing_key", test_params_get_int_alias_missing_key);
    g_test_add_func("/rpc_utils/params_is_object_array", test_params_is_object_array);
}
