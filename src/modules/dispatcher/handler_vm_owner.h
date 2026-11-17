                                                                             
                                            
                                                                                                              
  
                           
                                                                                                         
                                                                         
                           
  
                       
                                                                    
                                                                                
#ifndef PURECVISOR_HANDLER_VM_OWNER_H
#define PURECVISOR_HANDLER_VM_OWNER_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "api/uds_server.h"

   
                                                                                                                 
                                                                                                           
                                                           
   
void handle_vm_owner_set(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

#endif                                    
