#ifndef PURECVISOR_DISPATCHER_HANDLER_VM_HOTPLUG_H
#define PURECVISOR_DISPATCHER_HANDLER_VM_HOTPLUG_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

void handle_vm_set_memory_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_vm_set_vcpu_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif /* PURECVISOR_DISPATCHER_HANDLER_VM_HOTPLUG_H */