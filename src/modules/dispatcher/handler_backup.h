



















































#ifndef PURECVISOR_HANDLER_BACKUP_H
#define PURECVISOR_HANDLER_BACKUP_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS


typedef struct _UdsServer UdsServer;








void handle_backup_policy_set(JsonObject       *params,
                               const gchar      *rpc_id,
                               UdsServer        *server,
                               GSocketConnection *connection);






void handle_backup_policy_list(JsonObject       *params,
                                const gchar      *rpc_id,
                                UdsServer        *server,
                                GSocketConnection *connection);






void handle_backup_policy_delete(JsonObject       *params,
                                  const gchar      *rpc_id,
                                  UdsServer        *server,
                                  GSocketConnection *connection);






void handle_backup_history(JsonObject       *params,
                            const gchar      *rpc_id,
                            UdsServer        *server,
                            GSocketConnection *connection);









void handle_backup_restore(JsonObject       *params,
                            const gchar      *rpc_id,
                            UdsServer        *server,
                            GSocketConnection *connection);








void handle_backup_incremental(JsonObject       *params,
                                const gchar      *rpc_id,
                                UdsServer        *server,
                                GSocketConnection *connection);








void handle_backup_verify(JsonObject       *params,
                           const gchar      *rpc_id,
                           UdsServer        *server,
                           GSocketConnection *connection);








void handle_backup_replicate(JsonObject       *params,
                              const gchar      *rpc_id,
                              UdsServer        *server,
                              GSocketConnection *connection);






void handle_snapshot_schedule_set(JsonObject       *params,
                                  const gchar      *rpc_id,
                                  UdsServer        *server,
                                  GSocketConnection *connection);






void handle_snapshot_schedule_list(JsonObject       *params,
                                   const gchar      *rpc_id,
                                   UdsServer        *server,
                                   GSocketConnection *connection);






void handle_snapshot_schedule_delete(JsonObject       *params,
                                     const gchar      *rpc_id,
                                     UdsServer        *server,
                                     GSocketConnection *connection);

G_END_DECLS

#endif
