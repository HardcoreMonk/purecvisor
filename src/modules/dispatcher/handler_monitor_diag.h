   
                               
                                                                                                     
  
                           
                                                                                                     
                                                                                       
                                                                                             
                                                              
                                                                                          
                                                                                                            
                            
                                                                        
  
                                                                        
                                                                
   
#ifndef HANDLER_MONITOR_DIAG_H
#define HANDLER_MONITOR_DIAG_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "api/uds_server.h"

   
                                                                                    
                                                                      
                                     
   
void handle_monitor_vm_diag(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                            GSocketConnection *connection);

#endif                             
