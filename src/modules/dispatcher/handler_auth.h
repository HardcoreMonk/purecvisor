/**
 * @file handler_auth.h
 * @brief RBAC 인증/권한 관리 RPC 핸들러 공개 인터페이스
 *
 * ============================================================================
 * [RPC 메서드 매핑] (4개)
 * ============================================================================
 *   auth.user.create -> handle_auth_user_create (동기 — 사용자 생성, SHA256 해싱)
 *   auth.user.list   -> handle_auth_user_list   (동기 — 전체 사용자 목록)
 *   auth.user.delete -> handle_auth_user_delete (동기 — 사용자 삭제)
 *   auth.role.set    -> handle_auth_role_set    (동기 — 역할 변경)
 *
 * ============================================================================
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 * ============================================================================
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 *   - params:     JSON-RPC 요청의 "params" 객체 (호출자 소유, 핸들러가 해제하면 안 됨)
 *   - rpc_id:     요청 ID 문자열 (응답에 그대로 포함됨)
 *   - server:     UDS 서버 인스턴스 (pure_uds_server_send_response에 필요)
 *   - connection: 클라이언트 GSocketConnection (응답 전송 후 자동 닫힘)
 *
 * ============================================================================
 * [모든 핸들러 동기 응답 — fire-and-forget 미사용]
 * ============================================================================
 *   SQLite CRUD는 밀리초 내에 완료되므로 비동기 GTask가 불필요합니다.
 *
 * ============================================================================
 * [호출 경로]
 * ============================================================================
 *   dispatcher.c -> handle_auth_*() -> pcv_rbac.c (SQLite 사용자 DB)
 *
 * ============================================================================
 * [역할 종류 (PcvRole 열거형)]
 * ============================================================================
 *   "viewer"   (PCV_ROLE_VIEWER)   : 읽기 전용 (vm.list, container.list 등 조회만)
 *   "operator" (PCV_ROLE_OPERATOR) : VM/컨테이너 조작 가능 (start, stop, create 등)
 *   "admin"    (PCV_ROLE_ADMIN)    : 전체 권한 (사용자 관리, 클러스터 설정 포함)
 *
 * ============================================================================
 * [CLI / REST 사용 예시]
 * ============================================================================
 *   CLI:  pcvctl auth create dev pass123 operator
 *         pcvctl auth list
 *         pcvctl auth role dev admin
 *         pcvctl auth delete dev
 *   REST: POST /api/v1/auth/users  (auth.user.create)
 *         GET  /api/v1/auth/users  (auth.user.list)
 *         DELETE /api/v1/auth/users/{name}  (auth.user.delete)
 *         PUT  /api/v1/auth/role   (auth.role.set)
 */

#ifndef HANDLER_AUTH_H
#define HANDLER_AUTH_H

/*
 * json-glib.h : JsonObject 타입 (핸들러 파라미터로 사용)
 * gio.h       : GSocketConnection 타입 (UDS 소켓 연결)
 * uds_server.h: UdsServer 타입 정의 + pure_uds_server_send_response() 함수 선언
 */
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "api/uds_server.h"

/**
 * handle_auth_user_create:
 * RPC 메서드: auth.user.create
 * 동기 응답 — 새 사용자를 RBAC DB(SQLite)에 생성합니다.
 *
 * @param params: { "username": str, "password": str, "role": str, "tenant"?: str }
 *   role 허용값: "viewer" | "operator" | "admin" (대소문자 무관)
 *   tenant: 멀티테넌트 격리용, 생략 시 기본 테넌트
 *
 * 에러: -32602 (파라미터 누락/무효), -32000 (DB 중복/내부 에러)
 * 성공: {"username":"...", "role":"...", "tenant":"...", "status":"created"}
 */
void handle_auth_user_create(JsonObject       *params,
                             const gchar      *rpc_id,
                             UdsServer        *server,
                             GSocketConnection *connection);

/**
 * handle_auth_user_list:
 * RPC 메서드: auth.user.list
 * 동기 응답 — 전체 사용자 목록을 JSON 배열로 반환합니다.
 *
 * @param params: {} (파라미터 불필요)
 * 반환: [{"username":"admin", "role":"admin", "tenant":null}, ...]
 * 패스워드 해시는 보안상 응답에 포함되지 않습니다.
 */
void handle_auth_user_list(JsonObject       *params,
                           const gchar      *rpc_id,
                           UdsServer        *server,
                           GSocketConnection *connection);

/**
 * handle_auth_user_delete:
 * RPC 메서드: auth.user.delete
 * 동기 응답 — 지정된 사용자를 RBAC DB에서 삭제합니다.
 *
 * @param params: { "username": str }
 * 에러: -32602 (누락), -32000 (미존재 사용자)
 * 성공: {"username":"...", "status":"deleted"}
 */
void handle_auth_user_delete(JsonObject       *params,
                             const gchar      *rpc_id,
                             UdsServer        *server,
                             GSocketConnection *connection);

/**
 * handle_auth_role_set:
 * RPC 메서드: auth.role.set
 * 동기 응답 — 기존 사용자의 역할(role)을 변경합니다.
 *
 * @param params: { "username": str, "role": str }
 *   role 허용값: "viewer" | "operator" | "admin"
 *   주의: 이 핸들러는 role 화이트리스트 검증 없이 pcv_rbac_str_to_role()에 위임.
 *         인식 못하는 문자열은 viewer로 매핑됩니다.
 *
 * 에러: -32602 (누락), -32000 (미존재 사용자)
 * 성공: {"username":"...", "role":"...", "status":"updated"}
 */
void handle_auth_role_set(JsonObject       *params,
                          const gchar      *rpc_id,
                          UdsServer        *server,
                          GSocketConnection *connection);

#endif /* HANDLER_AUTH_H */
