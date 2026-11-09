   
                         
                                                                                                              
  
                           
                                                                       
                                                                    
  
                                                                  
   
#ifndef PCV_HANDLER_DAEMON_H
#define PCV_HANDLER_DAEMON_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                               
                           
void handle_config_reload(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection);
void handle_health_deep(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection);
void handle_db_migration_status(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection);
void handle_daemon_update_check(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection);
void handle_node_drain(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection);
void handle_node_resume(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection);
void handle_config_history(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
void handle_config_backup(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection);
void handle_daemon_config_get(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection);
void handle_daemon_config_set(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection);
void
handle_debug_trace_start(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection);
void
handle_debug_trace_stop(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection);
void
handle_debug_trace_status(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection);
void
handle_debug_trace_list(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection);
void
handle_debug_trace_report(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection);

#endif                           
