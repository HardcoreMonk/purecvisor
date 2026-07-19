/**
 * @file handler_auth.c
 * @brief RBAC 인증/권한 관리 RPC 핸들러 — 사용자 CRUD + 역할 변경 (4개 메서드)
 *
 * ============================================================================
 * [아키텍처 위치]
 * ============================================================================
 *   클라이언트 (pcvctl / REST API / Web UI)
 *        |
 *        v
 *   UDS 서버 (JSON-RPC 2.0) 또는 REST 서버 (HTTP + JWT HS256, 포트 80/443)
 *        |
 *        v
 *   dispatcher.c — g_strcmp0() else-if 체인에서 "auth.user.*" / "auth.role.*" 라우팅
 *        |
 *        v
 *   handle_auth_*()  <-- 이 파일의 4개 핸들러
 *        |
 *        v
 *   pcv_rbac.c (src/modules/auth/) — SQLite 사용자 DB, SHA256 패스워드 해싱
 *
 * ============================================================================
 * [처리하는 RPC 메서드] (4개)
 * ============================================================================
 *   auth.user.create -> handle_auth_user_create : 새 사용자 생성
 *     - params: { "username", "password", "role": "viewer|operator|admin", "tenant"?: str }
 *     - 패스워드는 pcv_rbac 내부에서 SHA256 으로 해싱하여 저장 (평문 저장 안 함)
 *
 *   auth.user.list   -> handle_auth_user_list   : 전체 사용자 목록 (배열)
 *     - params: {} (없음)
 *     - 반환값에 패스워드 해시는 포함되지 않음 (보안)
 *
 *   auth.user.delete -> handle_auth_user_delete : 사용자 삭제
 *     - params: { "username" }
 *     - 자기 자신 삭제도 허용됨 (호출자 책임)
 *
 *   auth.role.set    -> handle_auth_role_set    : 사용자 역할 변경
 *     - params: { "username", "role": "viewer|operator|admin" }
 *     - 변경 즉시 DB 반영. REST 권한 검사는 매 요청마다 DB의 현재 role을 다시 읽음.
 *
 * ============================================================================
 * [fire-and-forget 패턴 미사용]
 * ============================================================================
 *   모든 핸들러가 동기 응답입니다.
 *   SQLite CRUD가 즉시 완료되므로 비동기 처리가 불필요합니다.
 *
 * ============================================================================
 * [동기 응답 처리 흐름] (4개 핸들러 공통 패턴)
 * ============================================================================
 *   1. params에서 필수 필드(username, password 등) 추출 및 검증
 *   2. pcv_rbac_* 함수 호출 (실제 SQLite DB 조작, SHA256 패스워드 해싱)
 *   3. pure_rpc_build_success/error_response로 JSON-RPC 2.0 응답 생성
 *   4. pure_uds_server_send_response로 UDS 소켓에 전송 (전송 즉시 소켓 닫힘)
 *
 * ============================================================================
 * [주의사항]
 * ============================================================================
 *   - 역할(role)은 3단계: VIEWER(읽기전용), OPERATOR(VM 조작), ADMIN(전체 권한)
 *   - REST API의 POST /auth/token (JWT 로그인)은 rest_server.c에서 직접 처리하며,
 *     이 파일에는 포함되지 않습니다.
 *   - tenant 필드는 멀티테넌트 격리용이며, 생략 시 기본 테넌트에 소속됩니다.
 *   - 이 핸들러들은 REST RBAC 체크(pcv_rbac_check_permission)와 무관합니다.
 *     RBAC 권한 검사는 rest_server.c에서 JWT 토큰 파싱 후 엔드포인트별로 수행됩니다.
 *
 * ============================================================================
 * [에러 코드] (JSON-RPC 2.0 표준)
 * ============================================================================
 *   -32602 (PURE_RPC_ERR_INVALID_PARAMS) :
 *       필수 파라미터 누락, 빈 문자열, 잘못된 role 값 등 입력 오류
 *   -32000 (PURE_RPC_ERR_INTERNAL_ERROR) :
 *       pcv_rbac 내부 실패 (SQLite DB 에러, 사용자 중복/미존재 등)
 *
 * ============================================================================
 * [메모리 소유권 규칙 요약] (이 파일 전체에 적용)
 * ============================================================================
 *   - json_object_get_string_member()가 반환하는 const gchar*는 JsonObject 소유
 *     → params 해제 전까지만 유효. 비동기 처리 시 g_strdup() 필요.
 *   - json_node_take_object(node, obj): obj 소유권이 node에 이전됨
 *     → 이후 obj를 별도로 json_object_unref() 하면 이중 해제(double-free)!
 *   - pure_rpc_build_success_response(id, node): node 소유권을 가져감
 *     → 호출 후 node를 별도 해제하면 안 됨
 *   - 반환된 gchar* resp만 호출자가 g_free()로 해제하면 됨
 */

/*
 * --- 헤더 인클루드 설명 ---
 *
 * handler_auth.h   : 이 파일의 공개 함수 선언 (handle_auth_user_create 등 4개)
 * rpc_utils.h      : JSON-RPC 응답 빌더 (pure_rpc_build_success/error_response),
 *                    에러코드 상수 (PURE_RPC_ERR_INVALID_PARAMS, PURE_RPC_ERR_INTERNAL_ERROR),
 *                    UDS 전송 함수 (pure_uds_server_send_response)
 * pcv_rbac.h       : RBAC 코어 모듈 — PcvRole 열거형, PcvUser 구조체,
 *                    pcv_rbac_user_create/list/delete/set_role 등 SQLite CRUD 함수
 * glib.h           : GLib 기본 타입 (gchar, gboolean, guint 등) + 유틸 함수
 * json-glib.h      : JSON 파싱/빌드 라이브러리 (JsonObject, JsonNode, JsonArray 등)
 * string.h         : C 표준 문자열 함수 (strlen 등 — 직접 사용하지 않으나 호환성 포함)
 */
#include "handler_auth.h"
#include "rpc_utils.h"
#include "../auth/pcv_rbac.h"
#include "../audit/pcv_audit.h"
#include "utils/pcv_validate.h"   /* Q-2: pcv_validate_password_complexity (생성 경로 전용) */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────── */
/* auth.user.create                                            */
/* ──────────────────────────────────────────────────────────── */

/**
 * handle_auth_user_create:
 * @params: JSON-RPC params — {"username", "password", "role", "tenant"?}
 * @rpc_id: 요청 ID (응답 JSON에 그대로 포함됨)
 * @server: UDS 서버 인스턴스 (응답 전송용)
 * @connection: 클라이언트 소켓 연결 (응답 전송 후 자동 닫힘)
 *
 * 필수 파라미터: username, password, role (모두 비어 있으면 안 됨)
 * 선택 파라미터: tenant (멀티테넌트 환경에서 사용자 소속 지정)
 *
 * 역할 검증: "admin" / "operator" / "viewer" 만 허용
 *   - 그 외 값 (예: "superadmin") → -32602 에러 반환
 *   - 대소문자 무시 비교 (g_ascii_strcasecmp)
 *
 * 중복 검사: pcv_rbac_user_create() 내부에서 처리 (이미 존재하면 GError 반환)
 */
void
handle_auth_user_create(JsonObject       *params,
                        const gchar      *rpc_id,
                        UdsServer        *server,
                        GSocketConnection *connection)
{
    /*
     * [1단계] 필수 파라미터 존재 여부 확인 — 하나라도 없으면 즉시 에러 반환.
     *
     * json_object_has_member()는 키 존재 여부만 확인하며, 값의 타입이나 유효성은
     * 검사하지 않습니다. 값이 null이거나 빈 문자열인 경우는 아래 2단계에서 별도 검증합니다.
     */
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

    /*
     * [2단계] 파라미터 값 추출.
     *
     * json_object_get_string_member()는 JsonObject가 소유하는 문자열의 const 포인터를 반환.
     * 이 포인터는 params(JsonObject)가 해제될 때까지만 유효합니다.
     * 동기 핸들러이므로 함수 종료 전까지 안전하게 사용 가능합니다.
     * (만약 비동기(GTask) 패턴이었다면 g_strdup()으로 복사해야 합니다.)
     */
    const gchar *username = json_object_get_string_member(params, "username");
    const gchar *password = json_object_get_string_member(params, "password");
    const gchar *role_str = json_object_get_string_member(params, "role");

    /*
     * tenant는 선택 파라미터 — 멀티테넌트 미사용 시 NULL.
     * json_object_get_string_member()에 존재하지 않는 키를 전달하면
     * GLib 경고 로그가 출력되므로, 반드시 has_member()로 확인 후 접근합니다.
     */
    const gchar *tenant   = json_object_has_member(params, "tenant")
                            ? json_object_get_string_member(params, "tenant")
                            : NULL;

    /*
     * [3단계] 빈 문자열 방어: JSON에 키가 있어도 값이 "" 이면 거부.
     *
     * json_object_get_string_member()가 NULL을 반환하는 경우:
     *   - 해당 멤버의 JSON 타입이 string이 아닌 경우 (예: 숫자, null)
     *   - 내부 에러 (극히 드뭄)
     * !*username: 포인터가 가리키는 첫 번째 문자가 '\0'인지 확인 (빈 문자열 검사)
     */
    if (!username || !*username || !password || !*password || !role_str || !*role_str) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "username, password, and role must be non-empty strings");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    /*
     * [3-b단계] 비밀번호 강도 정책 검증 (Q-2 / A07) — 생성 시에만 적용.
     *
     * 최소 길이 12 + 문자군(소문자/대문자/숫자/특수) 3종 이상을 요구합니다.
     * 정책 미달이면 계정을 만들지 않고 -32602(INVALID_PARAMS)로 거부합니다.
     * 로그인/기존 사용자/비밀번호 변경 경로는 이 검사를 거치지 않아 무영향입니다.
     */
    const gchar *pw_reason = NULL;
    if (!pcv_validate_password_complexity(password, &pw_reason)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            pw_reason ? pw_reason : "Password does not meet complexity policy");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    /*
     * [4단계] 역할 유효성 검사 — 허용된 3가지 역할만 통과, 나머지는 -32602 에러.
     *
     * g_ascii_strcasecmp: ASCII 대소문자 무시 비교 (로케일 독립).
     * 예: "Admin" == "admin" == "ADMIN" 모두 통과.
     *
     * 왜 화이트리스트 방식인가?
     *   pcv_rbac_str_to_role()에서 인식 못하는 문자열은 기본값(viewer)으로 매핑됨.
     *   즉 "superadmin"을 입력하면 에러 없이 viewer가 되어 의도치 않은 권한 할당이 발생.
     *   이를 방지하기 위해 핸들러 레벨에서 엄격하게 허용 목록을 검증합니다.
     */
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

    /*
     * [5단계] 문자열 → PcvRole 열거형 변환.
     * PcvRole 은 pcv_rbac.h에 정의된 enum:
     *   PCV_ROLE_ADMIN    = 2 — 전체 권한 (사용자 관리, 클러스터 설정 포함)
     *   PCV_ROLE_OPERATOR = 1 — VM/컨테이너 조작 가능 (start, stop, create 등)
     *   PCV_ROLE_VIEWER   = 0 — 읽기 전용 (vm.list, container.list 등 조회만 가능)
     */
    PcvRole role = pcv_rbac_str_to_role(role_str);

    /*
     * [6단계] SQLite DB에 사용자 레코드 삽입.
     *
     * pcv_rbac_user_create 내부 처리:
     *   1. password를 SHA256으로 해싱 (평문은 DB에 저장되지 않음)
     *   2. INSERT INTO users (username, password_hash, role, tenant) VALUES (...)
     *   3. 중복 username 시 UNIQUE 제약 위반 → GError 반환
     *
     * GError 패턴 (GLib 에러 처리 관례):
     *   - 성공 시: ok=TRUE, err는 건드리지 않음 (NULL 유지)
     *   - 실패 시: ok=FALSE, err에 새 GError 할당 → 사용 후 반드시 g_error_free()
     *   - &err에 NULL을 전달하면 에러 메시지를 무시할 수 있으나, 여기서는 메시지 필요
     */
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

    /*
     * [7단계] 성공 응답 빌드.
     *
     * 응답 JSON 구조:
     *   { "username": "dev", "role": "operator", "tenant": null, "status": "created" }
     *
     * JSON 메모리 소유권 흐름 (주니어 개발자 필독):
     *   1. json_object_new()            → result_obj 힙 할당 (refcount=1)
     *   2. json_node_take_object(n, o)  → obj 소유권이 node로 이전. obj를 별도 unref 금지!
     *   3. pure_rpc_build_success_response(id, node) → node 소유권을 가져감. 별도 해제 금지!
     *   4. 반환된 gchar* resp만 g_free()로 해제하면 됨
     *
     * pcv_rbac_role_to_str(role): 열거형 → 문자열 변환.
     *   PCV_ROLE_ADMIN → "admin", PCV_ROLE_OPERATOR → "operator", PCV_ROLE_VIEWER → "viewer"
     */
    /* 감사 로그: 사용자 생성 기록 */
    pcv_audit_log(NULL, "auth.user.create", username, "ok", 0, 0, "local");

    JsonObject *result_obj = json_object_new();
    json_object_set_string_member(result_obj, "username", username);
    json_object_set_string_member(result_obj, "role", pcv_rbac_role_to_str(role));
    if (tenant)
        json_object_set_string_member(result_obj, "tenant", tenant);
    else
        json_object_set_null_member(result_obj, "tenant");  /* JSON null 명시 */
    json_object_set_string_member(result_obj, "status", "created");

    JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(result_node, result_obj);  /* 소유권 이전: obj → node */

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ──────────────────────────────────────────────────────────── */
/* auth.user.list                                              */
/* ──────────────────────────────────────────────────────────── */

/**
 * handle_auth_user_list:
 * @params: 사용하지 않음 (빈 객체 허용)
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @connection: 클라이언트 소켓 연결
 *
 * 반환 형식: JSON 배열 [{"username", "role", "tenant"}, ...]
 * pcv_rbac_user_list()가 GPtrArray<PcvUser*> 반환 → JSON 배열로 변환.
 * 배열 사용 후 g_ptr_array_unref 로 해제 (PcvUser 내부 메모리도 함께 해제).
 */
void
handle_auth_user_list(JsonObject       *params,
                      const gchar      *rpc_id,
                      UdsServer        *server,
                      GSocketConnection *connection)
{
    (void)params;  /* 파라미터 불필요 — 컴파일러 unused 경고 억제 */

    /* 전체 사용자 목록 조회 (SQLite SELECT) */
    GPtrArray *users = pcv_rbac_user_list();

    /*
     * GPtrArray → JsonArray 변환:
     * 각 PcvUser 구조체를 JSON 객체로 직렬화합니다.
     *
     * json_array_add_object_element(arr, obj)는 obj의 소유권을 arr에 이전하므로,
     * 루프 내에서 생성한 obj를 별도로 json_object_unref()하면 이중 해제가 발생합니다.
     *
     * json_object_set_string_member()는 문자열을 내부적으로 g_strdup()으로 복사하므로,
     * users 해제 후에도 JSON 응답의 문자열은 안전합니다.
     */
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
        json_array_add_object_element(arr, obj);  /* obj 소유권 → arr 이전 */
    }
    g_ptr_array_unref(users);  /* PcvUser 구조체 메모리 일괄 해제 */

    JsonNode *result_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result_node, arr);  /* arr 소유권 → node 이전 */

    gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

/* ──────────────────────────────────────────────────────────── */
/* auth.user.delete                                            */
/* ──────────────────────────────────────────────────────────── */

/**
 * handle_auth_user_delete:
 * @params: JSON-RPC params — {"username"}
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @connection: 클라이언트 소켓 연결
 *
 * 필수 파라미터: username (비어 있으면 -32602 에러)
 * 존재하지 않는 사용자 삭제 시: pcv_rbac_user_delete 가 GError 반환 → -32000 에러
 * 성공 시 반환: {"username": "...", "status": "deleted"}
 */
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

    /*
     * pcv_rbac_user_delete: DB에서 사용자 레코드 삭제.
     *   - DELETE FROM users WHERE username = ?
     *   - 미존재 시 GError("User not found") 반환
     *   - 현재 로그인 중인 사용자를 삭제해도 이 핸들러는 토큰을 자동 폐기하지 않아
     *     기존 JWT 토큰은 만료 시까지 유효. 즉시 무효화하려면 auth.session.revoke
     *     (jti 블랙리스트, SEC-1) 또는 auth.user.sessions.revoke를 별도 호출.
     */
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

    /* 감사 로그: 사용자 삭제 기록 */
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

/* ──────────────────────────────────────────────────────────── */
/* auth.role.set                                               */
/* ──────────────────────────────────────────────────────────── */

/**
 * handle_auth_role_set:
 * @params: JSON-RPC params — {"username", "role"}
 * @rpc_id: 요청 ID
 * @server: UDS 서버 인스턴스
 * @connection: 클라이언트 소켓 연결
 *
 * [파라미터]
 *   필수: username — 대상 사용자 ID
 *         role     — 새 역할 문자열 ("admin" / "operator" / "viewer")
 *
 * [auth.user.create와의 차이점]
 *   auth.user.create는 role 화이트리스트 검증을 명시적으로 수행하지만,
 *   이 핸들러(auth.role.set)는 별도 화이트리스트 검증이 없습니다.
 *   → pcv_rbac_str_to_role()에서 알 수 없는 문자열을 기본값(viewer)으로 매핑하므로
 *     잘못된 role 입력 시 에러 없이 viewer로 설정됩니다. (주의 필요)
 *
 * [역할 변경 영향]
 *   변경은 즉시 DB에 반영됩니다. REST는 JWT에서 subject(사용자명)를 검증한 뒤
 *   권한 검사 시 pcv_rbac_get_role()로 현재 DB role을 다시 조회합니다.
 *   따라서 기존 refresh session은 revoke하지 않으며, 다음 API 요청부터 새 role이
 *   적용됩니다. 프론트엔드의 메뉴 가시성은 /auth/whoami를 다시 읽거나 새로고침해야
 *   최신 role과 맞춰집니다.
 *
 * 성공 시 반환: {"username": "...", "role": "...", "status": "updated"}
 */
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

    /* 역할 화이트리스트 검증 — auth.user.create와 동일 패턴.
     * pcv_rbac_str_to_role()은 미인식 문자열을 VIEWER로 매핑하므로,
     * 핸들러 레벨에서 허용 목록을 먼저 검증합니다. */
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

    /* B6-W4 (Phase 4): self-elevation 차단 — 자신의 역할을 변경 금지.
     * caller 정보가 UDS direct에서 "-"이면 이 검사 통과 (ADR-0019 Option C가
     * UDS = ADMIN 가정이므로 안전). REST 경로는 JWT의 sub와 target username을
     * 비교하여 동일하면 거부. connection metadata "pcv-caller-sub"로 전달. */
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

    /*
     * pcv_rbac_user_set_role: DB에서 해당 사용자의 역할을 업데이트.
     *   - UPDATE users SET role = ? WHERE username = ?
     *   - 미존재 사용자 시 GError("User not found") 반환
     */
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

    /* 감사 로그: 역할 변경 기록 */
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
