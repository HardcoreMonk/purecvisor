   
                           
                                                                      
  
                           
                                                                       
                                                                    
  
                                                                                   
   
#ifndef PCV_HANDLER_SURICATA_H
#define PCV_HANDLER_SURICATA_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                                 
                           
void
handle_suricata_policy_set(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection);
void
handle_suricata_policy_get(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection);
void
handle_suricata_rules_update(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection);
void
handle_suricata_status(JsonObject *params, const gchar *rpc_id,
                        UdsServer *server, GSocketConnection *connection);
void
handle_suricata_ips_enable(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection);
void
handle_suricata_ips_disable(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection);
void
handle_suricata_ips_status(JsonObject *params, const gchar *rpc_id,
                            UdsServer *server, GSocketConnection *connection);
void
handle_suricata_ips_drop_list(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection);
void
handle_suricata_ips_drop_add(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection);
void
handle_suricata_ips_drop_remove(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection);

#endif                             
