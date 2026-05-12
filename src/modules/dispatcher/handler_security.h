#ifndef PURECVISOR_HANDLER_SECURITY_H
#define PURECVISOR_HANDLER_SECURITY_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct _UdsServer UdsServer;

void handle_security_event_list(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection);
void handle_security_event_get(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection);
void handle_security_action_pending(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
void handle_security_action_approve(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
void handle_security_action_dismiss(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
void handle_security_baseline_status(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection);
void handle_security_baseline_refresh(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_security_config_get(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection);
void handle_security_config_set(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif
