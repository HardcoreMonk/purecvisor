   
                                 
                                                                
  
                           
                                                                       
                                                                    
  
                                                     
   
#ifndef PCV_HANDLER_SECURITY_GROUP_H
#define PCV_HANDLER_SECURITY_GROUP_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                                       
                           
void handle_security_group_create(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection);
void handle_security_group_list(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection);
void handle_security_group_delete(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection);
void handle_security_group_rule_add(JsonObject *params, const gchar *rpc_id,
                                             UdsServer *server, GSocketConnection *connection);
void handle_security_group_rule_remove(JsonObject *params, const gchar *rpc_id,
                                                UdsServer *server, GSocketConnection *connection);
void handle_security_group_detach(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection);
void handle_vm_security_group_set(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection);

#endif                                   
