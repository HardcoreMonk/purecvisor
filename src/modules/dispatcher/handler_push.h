   
                       
                                        
  
                           
                                                                       
                                                                    
  
                                              
   
#ifndef PCV_HANDLER_PUSH_H
#define PCV_HANDLER_PUSH_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                             
                           
void handle_push_vapid_get(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
void handle_push_subscribe(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
void handle_push_unsubscribe(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_push_mine(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection);
void handle_push_list(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection);
void handle_push_vapid_rotate(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection);
void handle_push_test(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection);

#endif                         
