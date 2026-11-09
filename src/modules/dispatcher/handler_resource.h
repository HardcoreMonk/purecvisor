   
                           
                                                                                                     
  
                           
                                                                                           
                                                                                                                    
                                                                                          
                                                                                   
                                                                                               
                                                                   
                                                                                                           
                                                                                                     
                                                                                                                                 
                                                                                                   
                                                                                                                           
                                                                                                       
                                                                          
                                                                                                      
                                                                                          
                                                                                                   
                                                                                       
                                                
                                                                                                         
                                                                                          
                                                      
                                                                                    
                                                                        
  
                                                                                 
                                                                                
   
#ifndef HANDLER_RESOURCE_H
#define HANDLER_RESOURCE_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "api/uds_server.h"

   
                                                                                         
                                                                      
                                           
   
void handle_resource_register(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_resource_unregister(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_resource_grant(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_resource_revoke(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                                                                        
                                                                                             
                                                                    
   
void handle_resource_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_resource_unregistered_list(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                                       GSocketConnection *connection);
void handle_resource_import(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

#endif                         
