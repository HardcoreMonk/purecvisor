                                                                             
                                                  
                                                                                                
                                                                                               
  
                           
                                                                                   
                                                                   
                                                                                            
                                                                                                              
                                                                                                  
                                                                                                
                                                        
                                                                                                           
                                                                                                                 
                                                                                  
                                                                                      
                                       
  
                                                                  
                                                                      
                                 
                                                                                
#ifndef PURECVISOR_HANDLER_CONTAINER_MODE_H
#define PURECVISOR_HANDLER_CONTAINER_MODE_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "modules/dispatcher/rpc_utils.h"
#include "modules/lxc/lxc_driver.h"

G_BEGIN_DECLS

   
                          
                                                                                                  
                                             
   
typedef struct {
    gint             rootless;
    PureRpcErrorCode code;
    gchar           *message;
} PcvContainerCreateMode;

   
                                     
                                                                                                             
                                                                                   
                                                                                                                 
                                                                                           
                                                                                      
                                                                                                
                                                         
                                         
                                                                                     
                             
   
[[nodiscard]] gboolean pcv_container_create_bridge_alias(JsonObject *params, gchar **message);

   
                                   
                                                                                  
                                                                                                    
                                                                                 
                                                                                  
                                                                                                                 
                                                       
                                                         
   
[[nodiscard]] gboolean pcv_container_create_mode_check(JsonObject *params, GSocketConnection *conn,
                                                       const PcvLxcUsernsSettings *settings,
                                                       const gchar *subuid_path, const gchar *subgid_path,
                                                       PcvContainerCreateMode *out);

   
                           
                                                                                           
                                
                                                     
   
[[nodiscard]] JsonObject *pcv_container_info_json(const PcvLxcInfo *info);

typedef struct _UdsServer UdsServer;

   
                                    
                                                                                                             
                                                                                                                       
                                                                                                  
                                                    
                                                                 
                   
                                                                
   
gboolean pcv_container_volume_start_check(const gchar *name, gchar **msg_out);

   
                                        
                                                                                                    
                                                                            
                                                                              
                                                            
                            
                                                            
   
gboolean pcv_container_volume_restart_allowed(const gchar *name);

   
                                           
                                                                                                   
                                                                                                
                                                                         
                                                                                                                             
                                                                                
                                                                                            
                                                       
                         
                                                          
   
gboolean pcv_container_volume_restart_after_stop(const gchar *name, gint stop_rc);

   
                                   
                                                                                                     
                                                                                                 
                     
                     
                                                 
   
gboolean pcv_container_config_token_safe(const gchar *value, gboolean allow_space);

   
                                   
                                                                              
                                                                                                    
                                                                                          
                                      
                                                         
   
gboolean pcv_container_volume_hook_ready(const gchar *rpc_id, UdsServer *server, GSocketConnection *conn);

   
                                   
                                                                                                                    
                                                                                                
                                                                                  
                                                                                                              
                                                                                                      
                                                                                                      
                                                                                  
                                                       
                                                                                 
                                                                                        
                                                                                                                      
                             
                                                                             
                                                                     
   
[[nodiscard]] gboolean pcv_container_clone_volume_gate(gint role, const gchar *config_text, gint *volume_entries_out,
                                                       gchar **msg_out);

                                                                                                    
                                                                                                  
typedef gboolean (*PcvContainerCloneBridgeCheck)(const gchar *bridge, gpointer data);

   
                                   
                                                                                                                     
                                                                                      
                                                                                                         
                                                                                                        
                                                                                                              
                                                                               
                                                                                      
                                                                    
                                                                                                                                 
                                                                                                                        
                                                                
                             
                                                                                
                                 
   
[[nodiscard]] gboolean pcv_container_clone_bridge_gate(gint role, const gchar *config_text, PcvContainerCloneBridgeCheck check,
                                                       gpointer data, gchar **bridge_out, gchar **msg_out);

G_END_DECLS

#endif                                          
