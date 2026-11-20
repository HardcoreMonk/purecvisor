#ifndef PURECVISOR_HANDLER_VM_CONFIG_HISTORY_H
#define PURECVISOR_HANDLER_VM_CONFIG_HISTORY_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "api/uds_server.h"
#include "modules/resource/resource_gate.h"

G_BEGIN_DECLS

#define PCV_VMCH_LIST_DEFAULT_LIMIT 50
#define PCV_VMCH_LIST_MAX_LIMIT     200
#define PCV_VMCH_LIST_DEFAULT_SPAN_MS (7LL * 86400000LL)

#define PCV_VMCH_GET_DENY "Permission denied: configuration change not found or not visible"

JsonObject *pcv_vm_cfg_history_list_build(JsonObject *params, const PcvResCaller *caller, gint *err_code, gchar **err_msg);

JsonObject *pcv_vm_cfg_history_get_build(JsonObject *params, const PcvResCaller *caller, gint *err_code, gchar **err_msg);

void handle_vm_config_history_list(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                                   GSocketConnection *connection);

void handle_vm_config_history_get(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                                  GSocketConnection *connection);

G_END_DECLS

#endif
