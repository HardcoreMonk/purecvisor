   
                      
                                                                     
  
                           
                                                           
                                                                                     
                                                                         
                                                         
  
                                                          
                                                             
                                                       
   
#pragma once
#include <glib.h>

   
                                                                     
                                                        
                       
                                       
                                  
                        
  
                                                  
  
                                                                                    
  
                                                                    
   
gboolean pcv_iface_guard_route_has_default(const gchar *path, const gchar *iface,
                                           gboolean ipv6, gboolean *has_default,
                                           GError **error);

   
                                                                       
                                            
                    
                                                                    
                          
  
                                                   
  
                              
   
gboolean pcv_iface_guard_master_read(const gchar *sysfs_net_root, const gchar *iface,
                                     gchar **master_out, GError **error);

   
                                                                   
                          
                                                                      
                                                      
                                        
                                            
                                    
  
                                                                   
                                                                                 
                                            
  
                                                        
  
                                              
   
gboolean pcv_iface_guard_check_dedicated(const gchar *physical_if,
                                         const gchar *expected_master,
                                         const gchar *sysfs_net_root,
                                         const gchar *proc_root,
                                         gboolean *was_up,
                                         GError **error);
