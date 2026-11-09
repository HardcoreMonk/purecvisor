   
                      
                                      
  
                           
                                                                       
                                                                    
  
                                                   
   
#ifndef PCV_HANDLER_QOS_H
#define PCV_HANDLER_QOS_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                            
                           
void
handle_qos_vm_set(JsonObject *params, const gchar *rpc_id,
                    UdsServer *server, GSocketConnection *connection);
void
handle_qos_vm_get(JsonObject *params, const gchar *rpc_id,
                    UdsServer *server, GSocketConnection *connection);
void
handle_qos_tenant_set(JsonObject *params, const gchar *rpc_id,
                        UdsServer *server, GSocketConnection *connection);
void
handle_qos_tenant_get(JsonObject *params, const gchar *rpc_id,
                        UdsServer *server, GSocketConnection *connection);
void
handle_qos_stats(JsonObject *params, const gchar *rpc_id,
                   UdsServer *server, GSocketConnection *connection);
void
handle_qos_chaos_start(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection);
void
handle_qos_chaos_stop(JsonObject *params, const gchar *rpc_id,
                        UdsServer *server, GSocketConnection *connection);
void
handle_qos_chaos_status(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection);

#endif                        
