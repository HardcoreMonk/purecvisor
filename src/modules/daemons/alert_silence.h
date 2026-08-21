                                      
  
                                                      
  
                           
                                                   
                                                    
                                        
  
                                                   
                                                          
                                                     
  
                                                    
                                                                       
                                                   
                                                                           
                                                             
                                                 
                                                      
                                                    
                                                     
                                                     
  
                                                    
  
                  
                                                                
                            
                                                             
                                              
                                                                       
                                                      
                         
                                                                    
                                  
  
                     
                                                             
                                                        
                                                    
                                                               
   
#ifndef PURECVISOR_ALERT_SILENCE_H
#define PURECVISOR_ALERT_SILENCE_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

                                                         

   
                         
                                                           
                              
                                        
  
                                       
                                            
   
void       pcv_alert_add_silence(const gchar *metric, gint duration_min, const gchar *reason);

   
                         
                      
  
                                            
                                                
                                                     
   
gboolean   pcv_alert_is_silenced(const gchar *metric);

   
                          
  
                                                    
                                   
                                                  
   
JsonArray *pcv_alert_get_silences(void);

   
                           
  
                              
                                                 
   
void       pcv_alert_silence_reset(void);

G_END_DECLS

#endif                                 
