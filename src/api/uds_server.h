/* src/api/uds_server.h */

#ifndef PURECVISOR_UDS_SERVER_H
#define PURECVISOR_UDS_SERVER_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

// 순환 참조 방지를 위한 전방 선언 (Forward Declaration)
typedef struct _PureCVisorDispatcher PureCVisorDispatcher;

#define PURECVISOR_TYPE_UDS_SERVER (uds_server_get_type())

G_DECLARE_FINAL_TYPE(UdsServer, uds_server, PURECVISOR, UDS_SERVER, GObject)

UdsServer *uds_server_new(const gchar *socket_path);

// [Fix] Dispatcher 설정 함수 추가
void uds_server_set_dispatcher(UdsServer *self, PureCVisorDispatcher *dispatcher);

// [Fix] 에러 리포팅을 위해 gboolean 반환 및 GError 인자 추가
gboolean uds_server_start(UdsServer *self, GError **error);
void uds_server_stop(UdsServer *self);

// Dispatcher가 응답을 보낼 때 사용하는 함수
void pure_uds_server_send_response(UdsServer *self, GSocketConnection *connection, const gchar *response);

G_END_DECLS

#endif /* PURECVISOR_UDS_SERVER_H */