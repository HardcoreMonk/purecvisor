                                                         
                           
                                                              
                                                           
                                                 
                                               
   
#ifndef PCV_HANDLER_VM_INSPECT_H
#define PCV_HANDLER_VM_INSPECT_H

#include "../../api/uds_server.h"
#include <json-glib/json-glib.h>

                                                            
                                                     
void handle_vm_inspect_request(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection);

#endif
