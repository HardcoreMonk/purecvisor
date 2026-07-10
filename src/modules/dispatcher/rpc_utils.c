/**
 * @file rpc_utils.c
 * @brief JSON-RPC 2.0 응답 빌더 유틸리티 — 모든 디스패처 핸들러의 공통 응답 생성기
 *
 * [아키텍처 위치]
 *   모든 handler_*.c 파일에서 #include "rpc_utils.h"로 참조합니다.
 *   디스패처 핸들러가 클라이언트에 JSON-RPC 2.0 규격의 응답을 반환할 때
 *   반드시 이 파일의 빌더 함수를 사용해야 합니다.
 *
 * [제공 함수]
 *   pure_rpc_build_error_response(rpc_id, code, message)
 *     - JSON-RPC 2.0 에러 응답 문자열을 생성합니다.
 *     - 반환값: g_free()로 해제해야 하는 gchar* (JSON 문자열)
 *     - 예시: {"jsonrpc":"2.0","error":{"code":-32602,"message":"..."},"id":"1"}
 *
 *   pure_rpc_build_success_response(rpc_id, result_node)
 *     - JSON-RPC 2.0 성공 응답 문자열을 생성합니다.
 *     - result_node: JsonNode* (소유권 이전 — 함수 내부에서 해제됨)
 *     - 반환값: g_free()로 해제해야 하는 gchar* (JSON 문자열)
 *     - 예시: {"jsonrpc":"2.0","result":{...},"id":"1"}
 *
 * [사용 패턴 — 핸들러에서의 전형적인 호출]
 *   성공 시:
 *     JsonNode *node = json_node_new(JSON_NODE_OBJECT);
 *     json_node_take_object(node, obj);  // obj 소유권 이전
 *     gchar *resp = pure_rpc_build_success_response(rpc_id, node);
 *     pure_uds_server_send_response(server, connection, resp);
 *     g_free(resp);
 *
 *   실패 시:
 *     gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "vm_id required");
 *     pure_uds_server_send_response(server, connection, resp);
 *     g_free(resp);
 *
 * [표준 에러 코드] (rpc_utils.h에 enum 정의)
 *   -32700 : Parse Error (잘못된 JSON)
 *   -32600 : Invalid Request (유효하지 않은 요청 객체)
 *   -32601 : Method Not Found (존재하지 않는 메서드)
 *   -32602 : Invalid Params (파라미터 누락/잘못됨)
 *   -32603 : Internal Error (내부 JSON-RPC 오류)
 *   -32000 : ZFS Operation Failed (애플리케이션 에러)
 *   -32001 : VM Not Found (지정한 VM 없음)
 *
 * [주의사항]
 *   - 반환된 문자열은 호출자가 g_free()로 반드시 해제해야 합니다.
 *   - success_response의 result_node는 함수 내부에서 소유권을 가져가므로
 *     호출 후 별도로 해제하면 안 됩니다 (double-free 위험).
 */

#include "rpc_utils.h"

/* 감사 SEC-F2 — 파싱 전 JSON 중첩 깊이 사전 스캔(스택오버플로우 크래시 차단). */
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

/**
 * pure_rpc_build_error_response:
 * JSON-RPC 2.0 에러 응답 문자열을 생성합니다.
 *
 * @param rpc_id: 요청의 JSON-RPC ID (NULL이면 "id":null로 설정)
 * @param code: PureRpcErrorCode 에러 코드 (-32700~-32000 범위)
 * @param message: 사람이 읽을 수 있는 에러 메시지 (NULL이면 "Unknown error")
 * @return: g_free()로 해제해야 하는 JSON 문자열 (끝에 개행 문자 포함)
 *
 * [생성되는 JSON 형식]
 *   {"jsonrpc":"2.0","error":{"code":-32602,"message":"vm_id required"},"id":"1"}\n
 *
 * [JsonBuilder 사용 이유]
 *   에러 응답은 중첩 객체(error.code, error.message)를 포함하므로
 *   JsonBuilder의 begin_object/end_object 패턴이 직관적입니다.
 *   성공 응답은 JsonObject 직접 조립을 사용합니다 (아래 함수 참조).
 */
gchar* pure_rpc_build_error_response(const gchar *rpc_id,
                                     PureRpcErrorCode code,
                                     const gchar *message)
{
    JsonBuilder *builder = json_builder_new();

    json_builder_begin_object(builder);

    /* 1. JSON-RPC 프로토콜 버전 (항상 "2.0") */
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");

    /* 2. Error 객체: { "code": <정수>, "message": "<문자열>" } */
    json_builder_set_member_name(builder, "error");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "code");
    json_builder_add_int_value(builder, (gint)code);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, message ? message : "Unknown error");
    json_builder_end_object(builder);  /* error 객체 닫기 */

    /* 3. ID: 요청과 응답을 매칭하는 식별자 (NULL이면 null) */
    json_builder_set_member_name(builder, "id");
    if (rpc_id != NULL) {
        json_builder_add_string_value(builder, rpc_id);
    } else {
        json_builder_add_null_value(builder);  /* 알림(notification)이나 파싱 에러 시 */
    }

    json_builder_end_object(builder);  /* 루트 객체 닫기 */

    /* 4. JsonBuilder 트리 → JSON 문자열 직렬화 */
    JsonNode *root = json_builder_get_root(builder);
    gchar *raw_str = json_to_string(root, FALSE);

    /*
     * 5. 개행 문자(\n) 추가
     *
     * UDS 소켓 통신에서 메시지 경계를 구분하기 위해 끝에 \n을 붙입니다.
     * io_uring 기반 UDS 서버(uds_server.c)는 \n을 메시지 구분자로 사용합니다.
     * 또한 nc(netcat)로 수동 테스트할 때 출력이 깔끔하게 보입니다.
     */
    gchar *response_str = g_strdup_printf("%s\n", raw_str);

    /* 6. 중간 객체 메모리 해제 (반환 문자열만 남김) */
    g_free(raw_str);
    json_node_free(root);
    g_object_unref(builder);

    return response_str;  /* 호출자가 g_free()로 해제 */
}

/**
 * pure_rpc_build_success_response:
 * JSON-RPC 2.0 성공 응답 문자열을 생성합니다.
 *
 * @param rpc_id: 요청의 JSON-RPC ID (NULL이면 "id":null로 설정)
 * @param result_node: 결과 데이터가 담긴 JsonNode (소유권 이전 — 함수 내부에서 관리)
 *                     NULL이면 "result":null로 설정됩니다.
 * @return: g_free()로 해제해야 하는 JSON 문자열 (끝에 개행 문자 포함)
 *
 * [생성되는 JSON 형식]
 *   {"jsonrpc":"2.0","result":{...},"id":"1"}\n
 *
 * [소유권 이전 규칙 — 반드시 숙지]
 *   result_node의 소유권은 이 함수로 이전됩니다.
 *   json_object_set_member()가 result_node를 루트 객체에 편입시키므로,
 *   호출 후 result_node를 별도로 json_node_free()하면 double-free가 발생합니다.
 *   json_node_free(root_node)가 result_node까지 재귀적으로 해제합니다.
 *
 * [JsonObject 직접 조립 방식 사용 이유]
 *   성공 응답은 에러 응답과 달리 중첩 객체가 없으므로(result가 그대로 편입),
 *   JsonBuilder 대신 JsonObject에 직접 멤버를 추가하는 방식이 더 간결합니다.
 */
gchar* pure_rpc_build_success_response(const gchar *rpc_id, JsonNode *result_node)
{
    /* 1. 루트 JSON 객체 생성 */
    JsonObject *root_obj = json_object_new();

    /* 2. JSON-RPC 프로토콜 버전 (항상 "2.0") */
    json_object_set_string_member(root_obj, "jsonrpc", "2.0");

    /* 3. ID: 요청과 응답을 매칭하는 식별자 (NULL이면 null) */
    if (rpc_id != NULL) {
        json_object_set_string_member(root_obj, "id", rpc_id);
    } else {
        json_object_set_null_member(root_obj, "id");
    }

    /*
     * 4. Result: 핸들러가 조립한 결과 데이터
     *    json_object_set_member()는 result_node의 소유권을 가져갑니다.
     *    이후 result_node를 별도로 free하면 안 됩니다 (double-free 위험).
     */
    if (result_node != NULL) {
        json_object_set_member(root_obj, "result", result_node);
    } else {
        json_object_set_null_member(root_obj, "result");
    }

    /* 5. JsonObject → JsonNode 트리 구성 (take = 소유권 이전) */
    JsonNode *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, root_obj);

    /* 6. JSON 문자열 직렬화 */
    gchar *raw_str = json_to_string(root_node, FALSE);

    /*
     * 7. 개행 문자(\n) 추가
     *    UDS 소켓 메시지 구분자로 사용됩니다 (error_response와 동일한 규칙).
     */
    gchar *response_str = g_strdup_printf("%s\n", raw_str);

    /* 8. 중간 객체 메모리 해제 (반환 문자열만 남김) */
    g_free(raw_str);
    json_node_free(root_node);  /* root_obj + result_node도 재귀적으로 해제됨 */

    return response_str;  /* 호출자가 g_free()로 해제 */
}
