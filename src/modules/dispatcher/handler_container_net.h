   
                                
                                                                                                                     
  
                           
                                                                       
                                                                    
                                                                                
                                                 
  
                                                     
   
#ifndef PCV_HANDLER_CONTAINER_NET_H
#define PCV_HANDLER_CONTAINER_NET_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                                      
                           
void handle_container_clone(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection);
void handle_container_memory_stats(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection);
void handle_container_health_check(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection);
void handle_container_set_limits(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection);
void handle_container_nic_list(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection);
void handle_container_nic_attach(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection);
void handle_container_nic_detach(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection);
void handle_container_set_bandwidth(JsonObject *params, const gchar *rpc_id,
                                             UdsServer *server, GSocketConnection *connection);

#endif                                  
