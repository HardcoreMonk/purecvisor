   
                           
                                          
  
                           
                                                   
                                                    
                                        
  
                                               
                                              
                                              
  
               
                                          
  
                              
                                                          
                                                                   
  
                       
                          
                                              
                                                
  
          
                                                                           
   
#ifndef PURECVISOR_DISPATCHER_HANDLER_VM_START_H
#define PURECVISOR_DISPATCHER_HANDLER_VM_START_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"
#include "modules/dispatcher/vm_start_result.h"

G_BEGIN_DECLS

                                                                           
typedef void (*PcvVmStartDoneFn)(const gchar *vm_id, gboolean ok, const GError *error, gpointer user_data);

                                                                                              
typedef struct {
    const gchar *vm_id;                                 
    guint        numa_node;                          
    guint        vcpu_count;                                                                                       
    const gchar *bridge_name;                           
    const gchar *origin;                                                                                                                  
    gboolean     skip_if_active;                                                                             
    const gchar *actor;                                                                                          
} PcvVmStartRequest;

   
                                                          
  
                                                               
                                            
  
                                                                       
                                    
                                                                                
                                                                                         
                                                     
   
PcvVmStartSubmit pcv_vm_start_submit(const PcvVmStartRequest *req, PcvVmStartDoneFn done,
                                     gpointer user_data, gchar **err_msg);

   
                                            
  
                                                      
                                                                                                                         
   
void pcv_vm_start_report_rejected(const gchar *vm_id, const gchar *origin, const gchar *reason, const gchar *actor);

   
                                                          
  
                                               
  
                                              
                                                  
                                                                      
   
void handle_vm_start_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif
