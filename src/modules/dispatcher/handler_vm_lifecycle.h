#ifndef PURECVISOR_DISPATCHER_HANDLER_VM_LIFECYCLE_H
#define PURECVISOR_DISPATCHER_HANDLER_VM_LIFECYCLE_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

void handle_vm_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_vm_stop_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_vm_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
// 기존 선언들 아래에 추가
void handle_vm_metrics_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
G_END_DECLS

#endif /* PURECVISOR_DISPATCHER_HANDLER_VM_LIFECYCLE_H */