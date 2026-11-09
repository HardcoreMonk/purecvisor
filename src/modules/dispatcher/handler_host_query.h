   
                             
                                                                           
  
                           
                                                                       
                                                                    
  
                                                          
   
#ifndef PCV_HANDLER_HOST_QUERY_H
#define PCV_HANDLER_HOST_QUERY_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                                   
                           
void handle_pool_conninfo(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection);
void handle_capacity_forecast(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_quota_get(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection);
void handle_audit_search(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection);

#endif                               
