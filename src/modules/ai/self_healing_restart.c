                                        
  
                           
                                                   
                                                    
                                        
  
                                                          
  
                                                  
                                                       
                                                
                                          
  
                                                                          
                                                                       
                                                              
                                                           
                                                               
                               
                                                          
                                                                 
                                                          
                                        
                                                                   
                                                              
                                                
                                                        
  
                                                             
                                                         
                                                    
                                                                  
                                                           
                                                               
                                                     
                                                        
                                                                
   
#include "self_healing_restart.h"

                                                                          
                                      
                                                               
                                                     
                                                                 
const gchar *
pcv_healing_restart_decide(int is_active,
                           int (*create_fn)(gpointer dom), gpointer dom,
                           gint *rb_feedback)
{
                                                                
                                                         
    if (is_active > 0) {
                                                                
                                        
        *rb_feedback = +1;
        return "skipped";
    }
                                                           
                                                                    
    if (create_fn(dom) == 0) {
        *rb_feedback = +1;                                      
        return "success";
    }
    *rb_feedback = -1;                                        
    return "failed";
}
