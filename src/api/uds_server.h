/**
 * @file uds_server.h
 * @brief Unix Domain Socket JSON-RPC 2.0 서버 공개 인터페이스
 *
 * 아키텍처 위치:
 *   main.c, dispatcher.c, dispatcher 핸들러들(handler_*.c), rest_server.c에서 include합니다.
 *   main.c: uds_server_new() → uds_server_set_dispatcher() → uds_server_start() → uds_server_stop()
 *   핸들러: pure_uds_server_send_response()로 응답 전송
 *
 * 사용 패턴 (main.c에서):
 *   UdsServer *uds = uds_server_new("/var/run/purecvisor/daemon.sock");
 *   uds_server_set_dispatcher(uds, dispatcher);
 *   GError *err = NULL;
 *   if (!uds_server_start(uds, &err)) { ... 에러 처리 ... }
 *   // ... GMainLoop 실행 ...
 *   uds_server_stop(uds);
 *   g_object_unref(uds);
 *
 * 사용 패턴 (핸들러에서):
 *   void handle_xxx_request(JsonObject *params, const gchar *rpc_id,
 *                           UdsServer *server, GSocketConnection *connection) {
 *       // ... 처리 로직 ...
 *       gchar *resp = pure_rpc_build_success_response(rpc_id, result_node);
 *       pure_uds_server_send_response(server, connection, resp);
 *       g_free(resp);
 *       // 이후 connection은 닫힘 — 더 이상 사용 금지
 *   }
 *
 * 순환 참조 방지:
 *   PureCVisorDispatcher는 전방 선언(forward declaration)으로 참조합니다.
 *   dispatcher.h를 include하면 uds_server.h ↔ dispatcher.h 순환이 발생하므로,
 *   typedef struct _PureCVisorDispatcher PureCVisorDispatcher; 로 해결합니다.
 *
 * GObject 타입: PURECVISOR_TYPE_UDS_SERVER
 */

#ifndef PURECVISOR_UDS_SERVER_H
#define PURECVISOR_UDS_SERVER_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* 순환 참조 방지를 위한 전방 선언 (Forward Declaration)
 * dispatcher.h를 직접 include하면 uds_server.h ↔ dispatcher.h 순환 발생 */
typedef struct _PureCVisorDispatcher PureCVisorDispatcher;

#define PURECVISOR_TYPE_UDS_SERVER (uds_server_get_type())

G_DECLARE_FINAL_TYPE(UdsServer, uds_server, PURECVISOR, UDS_SERVER, GObject)

/**
 * uds_server_new:
 * @socket_path: UDS 소켓 파일 경로 (예: "/var/run/purecvisor/daemon.sock")
 *
 * UdsServer 인스턴스를 생성합니다. 아직 리스닝하지 않습니다.
 * uds_server_start() 호출 전에 uds_server_set_dispatcher()로 디스패처를 설정해야 합니다.
 *
 * Returns: (transfer full) 새 UdsServer 인스턴스
 */
UdsServer *uds_server_new(const gchar *socket_path);

/**
 * uds_server_set_dispatcher:
 * @self: UdsServer 인스턴스
 * @dispatcher: RPC 디스패처 (transfer none — 소유권 이전 없음)
 *
 * 수신된 JSON-RPC 요청을 전달할 디스패처를 설정합니다.
 * start() 호출 전에 반드시 설정해야 합니다.
 */
void uds_server_set_dispatcher(UdsServer *self, PureCVisorDispatcher *dispatcher);

/**
 * uds_server_start:
 * @self: UdsServer 인스턴스
 * @error: (nullable) 실패 시 GError가 설정됨
 *
 * 소켓 파일을 생성하고 리스닝을 시작합니다.
 * GMainLoop의 "incoming" 시그널로 연결을 수락합니다.
 *
 * Returns: 성공 시 TRUE, 실패 시 FALSE (error에 상세 정보)
 */
gboolean uds_server_start(UdsServer *self, GError **error);

/**
 * uds_server_stop:
 * @self: UdsServer 인스턴스
 *
 * 리스닝을 중지하고 소켓 파일을 삭제합니다.
 * main.c cleanup 블록에서 호출합니다.
 */
void uds_server_stop(UdsServer *self);

/**
 * pure_uds_server_send_response:
 * @self: UdsServer 인스턴스
 * @connection: 응답을 보낼 클라이언트 연결 (GSocketConnection)
 * @response: JSON-RPC 응답 문자열 (개행 포함)
 *
 * 핸들러에서 호출하여 클라이언트에 응답을 전송합니다.
 * 전송 후 연결을 즉시 닫으므로, 이 함수를 같은 connection에 2번 호출하면 안 됩니다.
 * fire-and-forget 패턴에서는 이 함수 호출 후 GTask로 백그라운드 작업을 시작합니다.
 */
void pure_uds_server_send_response(UdsServer *self, GSocketConnection *connection, const gchar *response);

G_END_DECLS

#endif /* PURECVISOR_UDS_SERVER_H */