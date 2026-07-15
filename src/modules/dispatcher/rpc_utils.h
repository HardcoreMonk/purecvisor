/**
 * @file rpc_utils.h
 * @brief JSON-RPC 2.0 응답 빌더 유틸리티 공개 인터페이스
 *
 * ──────────────────────────────────────────────────────────────
 * [아키텍처 내 위치]
 *   모든 디스패처 핸들러(handler_*.c)가 클라이언트에 JSON-RPC 2.0 규격의
 *   성공/에러 응답을 반환할 때 사용하는 공통 빌더 함수를 선언합니다.
 *   dispatcher.c와 모든 handler_*.c 파일에서 include합니다.
 *
 * [제공 함수]
 *   pure_rpc_build_error_response   : 에러 응답 JSON 문자열 생성
 *   pure_rpc_build_success_response : 성공 응답 JSON 문자열 생성
 *   pcv_rpc_params_get_int_alias    : JSON-RPC 정수 파라미터 호환 키 조회
 *
 * [에러 코드 enum: PureRpcErrorCode]
 *   JSON-RPC 2.0 표준 코드(-32700~-32603)와
 *   애플리케이션 전용 코드(-32000~-32001)를 정의합니다.
 *
 * [JSON-RPC 2.0 응답 형식]
 *   성공 응답:
 *     {"jsonrpc":"2.0", "id":"<rpc_id>", "result": <JsonNode>}
 *   에러 응답:
 *     {"jsonrpc":"2.0", "id":"<rpc_id>", "error": {"code":<int>, "message":"<string>"}}
 *
 * [사용 패턴 — 에러 응답]
 *   gchar *resp = pure_rpc_build_error_response(rpc_id,
 *                     PURE_RPC_ERR_INVALID_PARAMS, "vm_id required");
 *   pure_uds_server_send_response(server, connection, resp);
 *   g_free(resp);  // 반환된 문자열은 반드시 해제
 *
 * [사용 패턴 — 성공 응답]
 *   JsonNode *node = json_node_new(JSON_NODE_OBJECT);
 *   json_node_take_object(node, result_obj);  // obj 소유권 이전
 *   gchar *resp = pure_rpc_build_success_response(rpc_id, node);
 *   pure_uds_server_send_response(server, connection, resp);
 *   g_free(resp);  // node는 내부에서 해제됨 — 별도 free 금지
 *
 * [주의사항]
 *   - success_response에 전달하는 JsonNode*는 소유권이 이전(take)되므로
 *     호출 후 별도로 json_node_free()하면 이중 해제(double-free) 크래시 발생.
 *   - 반환된 gchar*는 호출자가 g_free()로 해제해야 한다.
 *   - rpc_id가 NULL이면 JSON-RPC 2.0 규격에 따라 "id":null 로 직렬화된다.
 * ──────────────────────────────────────────────────────────────
 */
#ifndef PURECVISOR_RPC_UTILS_H
#define PURECVISOR_RPC_UTILS_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * @brief JSON-RPC 2.0 표준 + 애플리케이션 전용 에러 코드
 *
 * JSON-RPC 2.0 스펙(https://www.jsonrpc.org/specification)에 정의된
 * 표준 에러 코드와, PureCVisor 전용 확장 코드를 정의한다.
 *
 * [에러 코드 범위 규약]
 *   -32700 ~ -32600 : JSON-RPC 표준 예약 (프로토콜 레벨 에러)
 *   -32099 ~ -32000 : JSON-RPC 서버 에러 예약 (애플리케이션 확장 가능)
 *   -32000 이상      : PureCVisor 자체 에러 코드
 */
typedef enum {
    /** 수신된 JSON이 유효하지 않음 (파싱 실패) */
    PURE_RPC_ERR_PARSE_ERROR     = -32700,
    /** JSON은 유효하지만 JSON-RPC 2.0 요청 객체 형식이 아님 (id/method 누락 등) */
    PURE_RPC_ERR_INVALID_REQUEST = -32600,
    /** 요청된 메서드가 존재하지 않거나 등록되지 않음 */
    PURE_RPC_ERR_METHOD_NOT_FOUND= -32601,
    /** 메서드 파라미터가 유효하지 않음 (필수 필드 누락, 타입 불일치 등) */
    PURE_RPC_ERR_INVALID_PARAMS  = -32602,
    /** 내부 JSON-RPC 처리 오류 (예상치 못한 서버 에러) */
    PURE_RPC_ERR_INTERNAL_ERROR  = -32603,

    /* === 애플리케이션 전용 에러 (-32000 ~ -32099) === */

    /** ZFS 명령 실행 실패 (zfs create/destroy/list 등의 외부 프로세스 에러)
     *  (오버로드: 일부 사이트는 각각 일반 서버에러/미구현/서비스불가 — 구 PCV_ERR_SERVER, 동일 wire값) */
    PURE_RPC_ERR_ZFS_OPERATION   = -32000,
    /** 지정된 VM이 libvirt에 존재하지 않음 (virDomainLookupByName 실패)
     *  (오버로드: 일부 사이트는 각각 일반 서버에러/미구현/서비스불가 — 구 PCV_ERR_NOT_IMPL, 동일 wire값) */
    PURE_RPC_ERR_VM_NOT_FOUND    = -32001,
    /** 리소스 상태 충돌 (동시 작업 시도, 오퍼레이션 잠금 보유 등)
     *  (오버로드: 일부 사이트는 각각 일반 서버에러/미구현/서비스불가 — 구 PCV_ERR_UNAVAILABLE, 동일 wire값) */
    PURE_RPC_ERR_CONFLICT        = -32002,
    /** 오퍼레이션 타임아웃 (libvirt/ZFS/etcd 응답 지연) */
    PURE_RPC_ERR_TIMEOUT         = -32003,
    /** 오퍼레이션 진행중 — 대상이 다른 작업으로 잠김(VM busy). (구 PCV_ERR_CONFLICT) */
    PURE_RPC_ERR_BUSY            = -32004,
    /** 일반 리소스 미존재 (VM 외 리소스). (구 PCV_ERR_NOT_FOUND) */
    PURE_RPC_ERR_NOT_FOUND       = -32005,
    /** 권한 거부 (RBAC). (구 PCV_ERR_FORBIDDEN) */
    PURE_RPC_ERR_FORBIDDEN       = -32006
} PureRpcErrorCode;

/**
 * @brief JSON-RPC 2.0 에러 응답 문자열을 생성한다.
 *
 * 생성되는 JSON 형식:
 *   {"jsonrpc":"2.0", "id":"<rpc_id>", "error":{"code":<code>, "message":"<message>"}}
 *
 * @param rpc_id   클라이언트가 보낸 요청 ID 문자열. NULL 허용 (→ "id":null).
 * @param code     PureRpcErrorCode 열거형 값 또는 임의의 정수 에러 코드.
 * @param message  사람이 읽을 수 있는 에러 메시지 문자열. NULL이면 빈 문자열.
 * @return 새로 할당된 JSON 문자열 (gchar*). 호출자가 g_free()로 해제해야 한다.
 */
gchar* pure_rpc_build_error_response(const gchar *rpc_id,
                                     PureRpcErrorCode code,
                                     const gchar *message);

/**
 * @brief JSON-RPC 2.0 성공 응답 문자열을 생성한다.
 *
 * 생성되는 JSON 형식:
 *   {"jsonrpc":"2.0", "id":"<rpc_id>", "result": <result_node의 JSON 표현>}
 *
 * @param rpc_id       클라이언트가 보낸 요청 ID 문자열. NULL 허용.
 * @param result_node  결과 데이터를 담은 JsonNode 포인터.
 *                     **소유권이 이 함수로 이전(take)된다** — 호출 후
 *                     json_node_free()를 별도로 호출하면 안 된다.
 *                     NULL이면 "result":null 로 직렬화된다.
 * @return 새로 할당된 JSON 문자열 (gchar*). 호출자가 g_free()로 해제해야 한다.
 */
gchar* pure_rpc_build_success_response(const gchar *rpc_id,
                                       JsonNode *result_node);

/**
 * @brief 정수 파라미터를 기본 키 또는 호환 별칭 키에서 읽는다.
 *
 * REST/UI/UDS 경로가 같은 의미의 값을 다른 필드명으로 보내는 전환 구간에서
 * 핸들러가 한 곳에서 호환성을 유지하도록 돕는다. 기본 키가 있으면 별칭보다
 * 우선한다.
 *
 * @param params       JSON-RPC params 객체. NULL이면 FALSE.
 * @param primary_key  우선 조회할 표준 키 이름. NULL 허용.
 * @param alias_key    표준 키가 없을 때 조회할 호환 키 이름. NULL 허용.
 * @param out_value    읽은 정수 값을 저장할 포인터. NULL이면 FALSE.
 * @return 값을 읽었으면 TRUE, 어떤 키도 없거나 인자가 유효하지 않으면 FALSE.
 */
gboolean pcv_rpc_params_get_int_alias(JsonObject *params,
                                      const gchar *primary_key,
                                      const gchar *alias_key,
                                      gint *out_value);

/* JSON-RPC 요청의 최대 허용 중첩 깊이 (감사 SEC-F2).
 * 정상 요청은 수 단계에 그친다. json-glib의 재귀 하강 파서는 깊은 중첩
 * (`[[[…]]]`)에서 스택오버플로우로 데몬을 크래시시키므로, 파싱 전에 이 상한으로
 * 사전 거부한다. 크래시 임계(스택 크기에 따라 수천~수만 depth)보다 훨씬 낮고
 * 실사용 페이로드보다 훨씬 높은 값. */
#define PCV_RPC_JSON_MAX_DEPTH 128

/**
 * pcv_rpc_json_depth_ok — JSON 문자열의 괄호 중첩 깊이가 max_depth 이하인지
 * 파싱 없이 스캔한다. 문자열 리터럴 내부의 괄호는 세지 않는다(이스케이프 처리).
 * @return 깊이가 상한 이하이거나 json이 NULL이면 TRUE, 초과하면 FALSE.
 */
gboolean pcv_rpc_json_depth_ok(const gchar *json, gint max_depth);

/* JSON 페이로드 바이트 상한 (외부 입력 DoS 방어) */
#define PCV_RPC_JSON_MAX_BYTES (1u * 1024u * 1024u)

/**
 * pcv_rpc_parse_guarded — 외부 입력 JSON 파싱의 유일 sanctioned 경로.
 * 깊이(≤PCV_RPC_JSON_MAX_DEPTH) + 크기(≤PCV_RPC_JSON_MAX_BYTES) 선검사 후 파싱.
 * @return TRUE=성공(*parser 소유권 이전), FALSE=거부/실패(*parser=NULL, *err 설정).
 */
gboolean pcv_rpc_parse_guarded(const gchar *data, gssize len,
                               JsonParser **parser, GError **err);

G_END_DECLS

#endif /* PURECVISOR_RPC_UTILS_H */
