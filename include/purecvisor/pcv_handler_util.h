/**
 * @file pcv_handler_util.h
 * @brief RPC 핸들러 입력 검증 매크로 — 일관된 파라미터 검증 + 에러 응답
 *
 * [목적]
 *   핸들러마다 개별적으로 구현하던 파라미터 검증을 표준화하여:
 *   1. NULL 역참조 방지 (json_object_get_string_member 반환값 검증)
 *   2. 무응답 리턴 방지 (항상 -32602 에러 응답 전송)
 *   3. libvirt 커넥션 풀 고갈 시 적절한 에러 응답
 *
 * [사용법]
 *   void handle_xxx(JsonObject *params, const gchar *rpc_id,
 *                   UdsServer *server, GSocketConnection *connection) {
 *       const gchar *vm_id;
 *       PCV_REQUIRE_PARAM(params, "vm_id", vm_id, rpc_id, server, connection);
 *       // vm_id는 여기서 non-NULL 보장
 *
 *       virConnectPtr conn;
 *       PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
 *       // conn은 여기서 non-NULL 보장 (사용 후 virt_conn_pool_release 필수)
 *   }
 */
#ifndef PCV_HANDLER_UTIL_H
#define PCV_HANDLER_UTIL_H

#include <json-glib/json-glib.h>
#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/virt/virt_conn_pool.h"

/**
 * PCV_REQUIRE_PARAM:
 * JSON 파라미터 존재 + non-NULL 검증. 실패 시 -32602 에러 응답 후 return.
 *
 * @param params_obj  JsonObject* 파라미터 객체
 * @param key         검사할 키 문자열 (예: "vm_id")
 * @param out_var     결과를 저장할 const gchar* 변수 (미리 선언 필요)
 * @param rpc_id      JSON-RPC 요청 ID
 * @param srv         UdsServer*
 * @param conn        GSocketConnection*
 */
#define PCV_REQUIRE_PARAM(params_obj, key, out_var, rpc_id, srv, conn)       \
    do {                                                                      \
        if (!(params_obj) || !json_object_has_member((params_obj), (key))) {  \
            gchar *_e = pure_rpc_build_error_response(                        \
                (rpc_id), -32602, "Missing required parameter: " key);        \
            pure_uds_server_send_response((srv), (conn), _e);                \
            g_free(_e);                                                       \
            return;                                                           \
        }                                                                     \
        (out_var) = json_object_get_string_member((params_obj), (key));       \
        if (!(out_var) || (out_var)[0] == '\0') {                             \
            gchar *_e = pure_rpc_build_error_response(                        \
                (rpc_id), -32602, "Empty or invalid parameter: " key);        \
            pure_uds_server_send_response((srv), (conn), _e);                \
            g_free(_e);                                                       \
            return;                                                           \
        }                                                                     \
    } while (0)

/**
 * PCV_REQUIRE_PARAM_OR:
 * 2개 키 중 하나라도 있으면 통과 (REST "name" / UDS "vm_id" 호환용).
 */
#define PCV_REQUIRE_PARAM_OR(params_obj, key1, key2, out_var, rpc_id, srv, conn) \
    do {                                                                          \
        (out_var) = NULL;                                                         \
        if ((params_obj) && json_object_has_member((params_obj), (key1)))         \
            (out_var) = json_object_get_string_member((params_obj), (key1));      \
        if ((!(out_var) || (out_var)[0] == '\0') &&                               \
            (params_obj) && json_object_has_member((params_obj), (key2)))         \
            (out_var) = json_object_get_string_member((params_obj), (key2));      \
        if (!(out_var) || (out_var)[0] == '\0') {                                 \
            gchar *_e = pure_rpc_build_error_response(                            \
                (rpc_id), -32602,                                                 \
                "Missing required parameter: " key1 " or " key2);                \
            pure_uds_server_send_response((srv), (conn), _e);                    \
            g_free(_e);                                                           \
            return;                                                               \
        }                                                                         \
    } while (0)

/**
 * PCV_REQUIRE_VIRT_CONN:
 * libvirt 커넥션 풀에서 연결 획득. 실패 시 -32000 에러 응답 후 return.
 *
 * @param out_conn  결과를 저장할 virConnectPtr 변수 (미리 선언 필요)
 * @param rpc_id    JSON-RPC 요청 ID
 * @param srv       UdsServer*
 * @param conn      GSocketConnection*
 *
 * [주의] 성공 시 반드시 virt_conn_pool_release(out_conn) 호출 필요.
 */
#define PCV_REQUIRE_VIRT_CONN(out_conn, rpc_id, srv, conn)                   \
    do {                                                                      \
        (out_conn) = virt_conn_pool_acquire();                               \
        if (!(out_conn)) {                                                    \
            gchar *_e = pure_rpc_build_error_response(                        \
                (rpc_id), -32000,                                             \
                "Hypervisor connection pool exhausted");                      \
            pure_uds_server_send_response((srv), (conn), _e);               \
            g_free(_e);                                                       \
            return;                                                           \
        }                                                                     \
    } while (0)

#endif /* PCV_HANDLER_UTIL_H */
