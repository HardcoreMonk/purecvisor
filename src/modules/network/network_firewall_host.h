   
                                
                                                          
  
                           
                                                   
                                                    
                                        
  
                                                   
                                                                 
                                                        
  
                                                                       
      
                                                            
                                                                  
                                                                    
                                                                
                                                                                    
                                                           
                                       
  
                 
                                                 
                                                              
                                            
                                   
  
                       
                                                        
                                                        
                                                                 
                                                                
  
               
                                                   
                                                       
                                                    
  
              
                                                                          
                                                                 
                                                                       
   
#ifndef PURECVISOR_NETWORK_FIREWALL_HOST_H
#define PURECVISOR_NETWORK_FIREWALL_HOST_H

#include <glib.h>

G_BEGIN_DECLS

   
                  
                           
  
                                                                
                                                             
                                                                     
                                                             
   
typedef enum {
    PCV_HOST_FW_OPEN = 0,
    PCV_HOST_FW_UFW,
    PCV_HOST_FW_IPTABLES_DROP,
    PCV_HOST_FW_FIREWALLD,
} PcvHostFwState;

   
                      
                                              
                                                 
  
         
                                                                        
                                                                           
                                                                
                          
  
                                                       
                                                     
   
PcvHostFwState pcv_host_fw_detect(void);

   
                    
                                                      
                                          
                                                      
                                                      
                                           
  
                                          
                                         
                                                   
                         
  
                                                      
                                                            
   
GPtrArray *pcv_host_fw_plan(PcvHostFwState st, const gchar *bridge, const gchar *mode, gboolean remove);

   
                        
                                                                              
                                                                                   
                                                                                
                                                                                           
                                                                                                      
                                                      
   
void pcv_host_fw_ufw_diff(const gchar *show_added, const gchar *bridge, const gchar *mode,
                          GPtrArray **to_delete, GPtrArray **to_add);

   
                              
                                                                                          
                                           
   
GPtrArray *pcv_host_fw_iptables_known(const gchar *bridge, const gchar *op);

   
                         
                                         
                         
                                                               
                                                        
  
                                                                             
                                                      
                                                    
                                                    
                                 
   
gboolean pcv_host_fw_integrate(const gchar *bridge, const gchar *mode, GError **error);

   
                      
                                           
                         
                                                        
  
                                                                                          
                                                                 
                                 
   
gboolean pcv_host_fw_remove(const gchar *bridge, GError **error);

G_END_DECLS

#endif                                         
