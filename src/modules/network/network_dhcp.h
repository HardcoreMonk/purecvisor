   
                       
                                  
  
                           
                                                   
                                                    
                                        
  
                                                         
                                                       
                                                             
  
                                                                       
       
                                                    
                                                      
  
                        
                                                             
                                                   
                                          
                                                 
  
              
                                                        
                                                    
                                        
  
              
                                                            
                                                        
  
            
                                                          
                                                                
                                             
                                                       
  
                       
                                   
                                                            
                                        
  
                                                               
                                                 
                                                        
                                                            
                                                         
                                                                       
   
#ifndef PURECVISOR_NETWORK_DHCP_H
#define PURECVISOR_NETWORK_DHCP_H

#include <glib.h>

G_BEGIN_DECLS

   
                                                         
                                                           
                                        
                                            
                         
  
                                                                          
                              
  
                    
   
gboolean network_dhcp_start(const gchar *bridge_name,
                             const gchar *cidr,
                             GError     **error);

   
                                                       
                                                  
                          
                       
                                                       
                                                                        
                                                                     
                         
  
                    
                                          
                                              
  
                    
                                         
                                                    
                                      
  
                    
   
gboolean network_dhcp_start_ex(const gchar *bridge_name,
                                const gchar *cidr,
                                gboolean     dns_enabled,
                                const gchar *upstream_dns,
                                GError     **error);

   
                                                             
                                                     
                           
                                            
  
                                                  
                                                        
                                                          
                                       
  
                                                       
   
gboolean network_dhcp_stop(const gchar *bridge_name,
                           GError     **error);

   
                                                                      
                                         
                          
                                                                   
                         
  
                                                  
                                             
  
                  
                                             
                                             
                                       
  
                    
   
gboolean network_dhcp_start_v6(const gchar *bridge_name,
                                const gchar *ipv6_prefix,
                                GError     **error);

   
                                                                    
                                                                  
              
                                                   
                                                         
                                                                          
                                                                      
                                      
                                                                        
                                                  
                                                 
                                             
                               
  
                                                                           
                                                                   
                                                                         
                                                                 
                                                            
                                                            
                                                             
  
                     
   
gboolean network_dhcp_reserve_overlay_ip(const gchar *ep_name,
                                          const gchar *tap_iface,
                                          const gchar *gw_cidr,
                                          const gchar *guest_mac,
                                          const gchar *overlay_ip,
                                          GError     **error);

   
                                                                                 
                                           
                                                    
                                            
                            
  
                                                                      
                                                               
      
  
                     
   
gboolean network_dhcp_release_overlay_ip(const gchar *ep_name,
                                          GError     **error);

G_END_DECLS

#endif                                
