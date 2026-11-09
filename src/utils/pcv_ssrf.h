   
                   
                                                                   
  
                           
                                                   
                                                    
                                        
  
                                                   
                                                  
                                                              
  
                                                       
                                                          
  
                                                     
                                                    
  
                                             
                            
                                                                      
                                                               
                                                          
  
                                                                             
                                                                   
                                                                
                                                         
                                                               
  
                                                          
                                                                  
                                                     
  
               
                                         
                                                                         
   

#ifndef PURECVISOR_SSRF_H
#define PURECVISOR_SSRF_H

#include <glib.h>

   
                                                       
  
                                                     
                                                               
                                                                      
                                                                               
   
gboolean pcv_url_target_allowed(const gchar *url, GError **error);

   
                                                             
  
                                                                            
                                                
                                    
                                                                                                         
                                                                                                   
   
gchar *pcv_url_redact_origin(const gchar *url);

#endif                        
