
#ifndef PURECVISOR_HANDLER_CONTAINER_H
#define PURECVISOR_HANDLER_CONTAINER_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct _UdsServer UdsServer;

void handle_container_create  (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_destroy (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_start   (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_stop    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_list    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_metrics (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_exec    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_snapshot_create   (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

void handle_container_snapshot_rollback (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

void handle_container_snapshot_delete   (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

void handle_container_snapshot_list     (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

void handle_container_logs    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_volume_attach(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

void handle_container_volume_detach(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

void handle_container_volume_list  (JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

void handle_container_env_set    (JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *conn);

void handle_container_env_list   (JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *conn);

void handle_container_env_delete (JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *conn);

void handle_container_health_set   (JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

void handle_container_health_get   (JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

void handle_container_health_delete(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

G_END_DECLS

#endif
