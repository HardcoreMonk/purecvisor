


































#ifndef PURECVISOR_DISPATCHER_HANDLER_VM_LIFECYCLE_H
#define PURECVISOR_DISPATCHER_HANDLER_VM_LIFECYCLE_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS












void handle_vm_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_vm_stop_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_vm_pause_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_vm_resume_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);









void handle_vm_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);









void handle_vm_metrics_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);









void handle_vm_rename_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);














void handle_vm_guest_ping_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_vm_guest_agent_status_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_vm_guest_agent_ensure_channel_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_vm_guest_fsinfo_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_vm_guest_exec_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);





void handle_vm_guest_shutdown_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif
