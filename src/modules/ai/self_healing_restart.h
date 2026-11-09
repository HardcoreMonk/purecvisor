                                        
  
                           
                                                   
                                                    
                                        
  
                                            
  
                                                    
                                                     
                                                             
                                                                    
  
                                                   
                                                      
              
  
       
                                                            
                                                                 
                                                         
                                                       
                                                                  
  
       
                                                              
                                                                         
                               
  
        
                                                                                
                                                                     
                                                            
                 
   
#ifndef PURECVISOR_SELF_HEALING_RESTART_H
#define PURECVISOR_SELF_HEALING_RESTART_H

#include <glib.h>
#include "../dispatcher/vm_start_result.h"

G_BEGIN_DECLS

                                                                                 
                                                                     
                                   
   
                               
                                                                        
                                         
                                                     
                                                 
                                                                                  
  
                                                                               
                                          
                              
                                                             
   
const gchar *pcv_healing_restart_outcome(PcvVmStartSubmit submit, gboolean done_ok,
                                         const GError *done_error, gint *rb_feedback);

G_END_DECLS

#endif                                        
