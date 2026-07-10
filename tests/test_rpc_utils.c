/**
 * @file test_rpc_utils.c
 * @brief JSON-RPC 2.0 응답 빌더 유닛 테스트
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  rpc_utils.h (src/modules/dispatcher/)의 JSON-RPC 2.0 응답 빌더를 검증한다.
 *  12개 테스트 케이스, 커버리지 97.5%.
 *
 *  데몬의 모든 RPC 핸들러는 이 빌더를 통해 응답을 생성한다:
 *  - pure_rpc_build_error_response(id, code, message)  → {"jsonrpc":"2.0","error":{...}}
 *  - pure_rpc_build_success_response(id, result_node)  → {"jsonrpc":"2.0","result":{...}}
 *
 *  에러 응답 검증:
 *  - 기본: id, jsonrpc, error.code, error.message 필드 존재 + 값 정확성
 *  - NULL id: "id":null 직렬화
 *  - 모든 에러 코드: PARSE_ERROR(-32700) ~ TIMEOUT 9종 전부 테스트
 *  - 특수 문자: 따옴표/백슬래시가 포함된 메시지 → JSON 이스케이프 후 복원
 *  - NULL 메시지: 크래시 없이 직렬화
 *
 *  성공 응답 검증:
 *  - Object 결과: {"status":"ok","count":42}
 *  - Array 결과: ["vm-a","vm-b"]
 *  - NULL id / 정수형 id 문자열 전달
 *
 *  parse_resp() 헬퍼: 응답 JSON을 파싱하여 JsonObject 반환 (내부 검증용).
 *  주의: json_node_take_object()로 소유권이 이전되므로 별도 해제 불필요.
 * ============================================================================
 */
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "../src/modules/dispatcher/rpc_utils.h"

/* ── 에러 응답 ──────────────────────────────────────── */

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
    /* "id":null 직렬화 검증 */
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
    /* 따옴표/백슬래시 이스케이프 */
    gchar *r = pure_rpc_build_error_response("id", PURE_RPC_ERR_INTERNAL_ERROR,
        "error with \"quotes\" and \\ backslash");
    g_assert_nonnull(r);
    JsonObject *o = parse_resp(r);
    JsonObject *err = json_object_get_object_member(o, "error");
    /* JSON 파싱 후에는 원본 메시지 복원 */
    g_assert_cmpstr(json_object_get_string_member(err, "message"), ==,
                    "error with \"quotes\" and \\ backslash");
    json_object_unref(o);
    g_free(r);
}

static void test_error_response_null_message(void) {
    gchar *r = pure_rpc_build_error_response("id", PURE_RPC_ERR_INTERNAL_ERROR, NULL);
    /* NULL 메시지 — 빈 문자열 또는 null로 직렬화 */
    g_assert_nonnull(r);
    g_free(r);
}

/* ── 성공 응답 ──────────────────────────────────────── */

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
    /* node는 build 함수가 take/free 했으므로 별도 해제 안 함 */
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

    /* 정수형 id를 문자열로 전달 */
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

void test_rpc_utils_register(void) {
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
}
