/**
 * @file handler_security.h
 * @brief security.* JSON-RPC 핸들러 인터페이스 — Security Guard 의 UDS 표면.
 *
 * HIDS(호스트 침입 탐지) 이벤트 조회와 HIPS(대응 액션) 승인/기각을 dispatcher 에
 * 노출하는 진입점 선언부다. 각 핸들러는 (params, rpc_id, server, connection) 서명을
 * 공유하며, dispatcher 가 인증·RBAC 를 통과시킨 뒤에만 호출한다. 구현·계약 상세와
 * ADR 참조는 handler_security.c 상단 헤더를 본다.
 *
 * Operator note:
 *   이 선언들이 가리키는 코드는 "탐지된 위협을 실제로 차단할지"가 결정되는 지점이다.
 *   조회 계열은 부작용이 없지만 approve 는 nftables DROP·API 키 폐기 같은 실제
 *   부작용으로 이어지므로, 승인은 사람이 명시적으로 통과시킨 뒤에만 실행된다.
 */
#ifndef PURECVISOR_HANDLER_SECURITY_H
#define PURECVISOR_HANDLER_SECURITY_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct _UdsServer UdsServer;

/* 조회 계열(read-only): 현재 로컬 이벤트/펜딩 상태를 그대로 반환한다. */
void handle_security_event_list(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection);
void handle_security_event_get(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection);
void handle_security_action_pending(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
/* approve: 부작용을 일으키는 유일한 경로 — accepted 를 먼저 보내고 워커에서
 * 실행(ADR-0018). dismiss 는 로컬 결정 상태만 바꾸는 동기 처리다. */
void handle_security_action_approve(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
void handle_security_action_dismiss(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
/* baseline: 파일 무결성 기준선의 신뢰 상태 조회/갱신. refresh 는 admin 이 명시한
 * 경로 집합만 신뢰로 승격한다(스캔 결과가 자동 승격하지 않는다). */
void handle_security_baseline_status(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection);
void handle_security_baseline_refresh(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_security_config_get(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection);
void handle_security_config_set(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif
