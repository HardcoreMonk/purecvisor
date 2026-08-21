                                        
  
                           
                                                   
                                                    
                                        
  
                                            
  
                                                 
                                                    
                                                 
                                  
  
                                                     
                                                 
  
       
                                                                 
                                                  
                                                 
                                                         
                                             
  
       
                                                         
                                                           
                                                         
                                                     
  
        
                                                                                  
                                                             
                                                            
                          
   
#ifndef PURECVISOR_SELF_HEALING_RESTART_H
#define PURECVISOR_SELF_HEALING_RESTART_H

#include <glib.h>

G_BEGIN_DECLS

                                                                          
                                      
   
                              
                                                             
                                                 
  
                                                            
                                                             
                                                           
                                                                 
                                                                  
                                                                    
  
                                                          
                                                 
                                                    
                                           
                            
   
const gchar *pcv_healing_restart_decide(int is_active,
                                        int (*create_fn)(gpointer dom), gpointer dom,
                                        gint *rb_feedback);

G_END_DECLS

#endif                                        
