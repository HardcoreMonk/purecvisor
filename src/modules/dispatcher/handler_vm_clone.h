   
                           
                                                
  
                           
                                                                       
                                                                    
  
                                                                 
   
#ifndef PCV_HANDLER_VM_CLONE_H
#define PCV_HANDLER_VM_CLONE_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                                 
                           
void handle_vm_clone(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection);

#endif                             
