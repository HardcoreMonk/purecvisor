   
                             
                                                                                          
  
                           
                                                                                                       
                                                                                                
                                                                                            
                                                     
  
                                                                               
                                
   
#ifndef PURECVISOR_HANDLER_AUTH_NAMES_H
#define PURECVISOR_HANDLER_AUTH_NAMES_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"

   
                                                                                                   
                                                                                                                            
                                                        
   
PureRpcErrorCode pcv_auth_principal_rpc_code(const GError *err, PureRpcErrorCode fallback);

   
                                                                                           
                                                                                               
                                                                                            
                                            
                                                     
   
void handle_auth_deleted_name_add(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

#endif                                      
