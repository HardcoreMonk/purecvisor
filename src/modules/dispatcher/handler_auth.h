




















































#ifndef HANDLER_AUTH_H
#define HANDLER_AUTH_H






#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "api/uds_server.h"













void handle_auth_user_create(JsonObject       *params,
                             const gchar      *rpc_id,
                             UdsServer        *server,
                             GSocketConnection *connection);










void handle_auth_user_list(JsonObject       *params,
                           const gchar      *rpc_id,
                           UdsServer        *server,
                           GSocketConnection *connection);










void handle_auth_user_delete(JsonObject       *params,
                             const gchar      *rpc_id,
                             UdsServer        *server,
                             GSocketConnection *connection);














void handle_auth_role_set(JsonObject       *params,
                          const gchar      *rpc_id,
                          UdsServer        *server,
                          GSocketConnection *connection);

#endif
