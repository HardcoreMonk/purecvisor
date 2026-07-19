
#ifndef HANDLER_STORAGE_H
#define HANDLER_STORAGE_H

#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "api/uds_server.h"

void handle_storage_pool_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_storage_zvol_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_storage_zvol_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_storage_zvol_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_storage_pool_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_storage_pool_destroy_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_storage_pool_scrub_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_iso_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

#endif
