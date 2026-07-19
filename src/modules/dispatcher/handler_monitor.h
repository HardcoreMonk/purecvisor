
#ifndef PURECVISOR_HANDLER_MONITOR_H
#define PURECVISOR_HANDLER_MONITOR_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

void handle_monitor_metrics(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection);

void handle_monitor_fleet(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif
