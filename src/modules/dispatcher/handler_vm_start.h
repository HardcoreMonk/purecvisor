#ifndef PURECVISOR_DISPATCHER_HANDLER_VM_START_H
#define PURECVISOR_DISPATCHER_HANDLER_VM_START_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

void handle_vm_start_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif