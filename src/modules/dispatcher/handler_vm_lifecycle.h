   
                               
                                    
  
                           
                                                   
                                                    
                                        
  
                                                              
                                                                             
                                                                
                                                                  
  
                                                                 
              
                                                               
                                                           
                                                        
  
               
                                                                        
                                                                                         
                                                                                         
                                                                                        
                                                                                                   
                                                                           
  
                              
                                                          
                                                                   
  
             
                                                                  
                                                                
                                
                                                             
                                                       
  
          
                                                                     
                                                             
                                                             
                                                                        
                                                                 
   
#ifndef PURECVISOR_DISPATCHER_HANDLER_VM_LIFECYCLE_H
#define PURECVISOR_DISPATCHER_HANDLER_VM_LIFECYCLE_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"
#include "modules/virt/vm_manager.h"

G_BEGIN_DECLS

   
                                       
  
                                           
  
                                                                
                                                           
  
                                                               
                                                        
                                            
                                             
   
void handle_vm_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                                     
                                       
                                                        
   
void handle_vm_stop_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                                     
                                         
                                                           
   
void handle_vm_pause_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                                        
                                   
                                                        
   
void handle_vm_resume_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                                                            
  
                                                      
  
                                             
                                                  
  
                                                        
   
void handle_vm_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                                     
  
                                                   
  
                                                          
                        
  
                                                        
   
void handle_vm_metrics_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                               
  
                                                            
  
                                                           
                                         
  
                                                                               
   
void handle_vm_rename_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                                    
                         
                                                      
                                                        
  
                                         
                                                                 
  
                                                            
                                                                       

   
                                            
                                                        
   
void handle_vm_guest_ping_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                               
                                                        
   
void handle_vm_guest_agent_status_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                                 
                                                        
   
void handle_vm_guest_agent_ensure_channel_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                                                  
                                                        
   
void handle_vm_guest_fsinfo_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                          
                                               
                                                                                           
   
void handle_vm_guest_exec_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

   
                                                 
                                                     
                                                        
   
void handle_vm_guest_shutdown_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                                
                                                                                 
                                                       
                                             

                             
typedef void (*PcvVmStopDoneFn)(const gchar *vm_id, gboolean ok, const GError *error, gpointer user_data);

   
                                                                                 
  
                                                          
                                                
                                                   
  
                                                                                        
                                    
   
gboolean pcv_vm_graceful_stop_submit(PureCVisorVmManager *mgr, const gchar *vm_id, const gchar *origin,
                                     const gchar *actor, PcvVmStopDoneFn done, gpointer user_data, gchar **err_msg);

                                                                                            
void pcv_vm_stop_report_rejected(const gchar *vm_id, const gchar *origin, const gchar *reason, const gchar *actor);

G_END_DECLS

#endif                                                   
