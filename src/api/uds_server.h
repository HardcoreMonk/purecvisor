#pragma once

#include <glib-object.h>
// [CRITICAL FIX] Dispatcher 헤더를 포함하거나, 순환 참조 방지를 위해 전방 선언 사용
// 여기서는 헤더를 포함합니다.
#include "dispatcher.h" 

G_BEGIN_DECLS

#define UDS_TYPE_SERVER (uds_server_get_type())
G_DECLARE_FINAL_TYPE(UdsServer, uds_server, UDS, SERVER, GObject)

UdsServer *uds_server_new(const gchar *socket_path);
gboolean uds_server_start(UdsServer *self, GError **error);
void uds_server_stop(UdsServer *self);

// [FIX] Type name matches dispatcher.h definition
void uds_server_set_dispatcher(UdsServer *self, PureCVisorDispatcher *dispatcher);

G_END_DECLS