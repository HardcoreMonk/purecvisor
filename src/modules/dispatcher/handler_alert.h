   
                        
                                                                                   
  
                           
                                                                       
                                                                    
  
                                                 
   
#ifndef PCV_HANDLER_ALERT_H
#define PCV_HANDLER_ALERT_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                              
                           
void handle_alert_history(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection);
void handle_alert_config_get(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_alert_config_set(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_alert_config_reload(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection);
void handle_agent_config_get(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_agent_config_set(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_agent_history(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection);
void handle_agent_compare_manual(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection);
void handle_anomaly_reset_baseline(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection);
void handle_healing_pending(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection);
void handle_healing_status(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
void handle_healing_set_mode(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_healing_history(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection);
void handle_alert_silence(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection);
void handle_alert_silence_list(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection);
void handle_alert_dlq_list(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
void handle_alert_ack(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection);
void handle_alert_action_list(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection);
void handle_ai_healing_approve(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection);
void handle_ai_healing_reject(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection);

#endif                          
