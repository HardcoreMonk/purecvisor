   
                            
                                                                  
  
                           
                                                            
                                                               
                                                                                     
                  
  
                                                          
                                                         
                                                               
             
   
#pragma once
#include <glib.h>

   
                                                   
                                             
                                     
                                            
  
                                                       
  
                                                                          
   
gboolean pcv_network_qos_rules_save(const gchar *iface, const gchar *direction,
                                    gint rate_mbps, gint burst_kb);

   
                                                      
                    
                                           
  
                                           
   
void pcv_network_qos_rules_remove(const gchar *iface, const gchar *direction);
