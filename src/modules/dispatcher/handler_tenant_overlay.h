#ifndef PURECVISOR_HANDLER_TENANT_OVERLAY_H
#define PURECVISOR_HANDLER_TENANT_OVERLAY_H

   
                                 
                                                                        
  
                           
                                                   
                                                    
                                        
  
                                                     
                                                       
                                                       
                                              
  
            
                                                           
                                                                   
                                                                                                
  
                                  
                                                         
                                                 
                                                 
                                              
                                                                  
                                                      
  
            
                                                       
                                                        
                                                                    
                                                 
                                              
   

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct _UdsServer UdsServer;

   
                                   
                                                         
                                                      
                                                                
                                                             
                                                                   
                                                                       
                                                             
  
                                               
                                               
                                                              
                                              
                                       
  
                                   
                                                   
   
gboolean pcv_tenant_overlay_rpc_validate(JsonObject *params,
                                         gboolean require_vm,
                                         const gchar **tenant_out,
                                         const gchar **vm_out,
                                         GError **error);

                                                          
                                                                  
                                                         
                                                    
void handle_tenant_overlay_create(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection);
void handle_tenant_overlay_delete(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection);
void handle_tenant_overlay_list(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection);
void handle_tenant_overlay_get(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection);
void handle_tenant_overlay_attach_vm(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection);
void handle_tenant_overlay_detach_vm(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif                                          
