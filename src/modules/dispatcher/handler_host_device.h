   
                              
                                                                                                     
  
                           
                                                                                            
                                                                                                     
                                                                                                               
                                                                                                        
                                                                                                 
                                                                          
                                                                                                            
                                                                                                   
                                                                      
  
                                                                                
                               
   
#ifndef HANDLER_HOST_DEVICE_H
#define HANDLER_HOST_DEVICE_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "api/uds_server.h"

   
                                                                                                            
                                                                                                       
                                                                                                           
                             
   
void handle_host_device_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

#endif                            
