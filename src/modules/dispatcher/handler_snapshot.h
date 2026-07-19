
#ifndef PURECVISOR_HANDLER_SNAPSHOT_H
#define PURECVISOR_HANDLER_SNAPSHOT_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct _UdsServer UdsServer;

void handle_vm_snapshot_create(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_vm_snapshot_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_vm_snapshot_rollback(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_vm_snapshot_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_vm_snapshot_delete_all(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif
