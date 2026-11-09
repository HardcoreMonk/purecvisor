                                        
  
                           
                                                   
                                                    
                                        
  
                                                          
  
                                                     
                                               
                                                               
                                            
  
                                                             
                                                  
                                                             
                                                    
                                                          
                                                                 
                                                         
                                        
                                                                          
                                                       
                                                           
                                                 
  
                                                         
                                                          
                                                                
                                                         
                                                               
                                                     
                                                        
   
#include <gio/gio.h>
#include "self_healing_restart.h"

                                                                                 
                                                    
                                                                  
                                        
                                                                 
const gchar *
pcv_healing_restart_outcome(PcvVmStartSubmit submit, gboolean done_ok,
                            const GError *done_error, gint *rb_feedback)
{
    switch (submit) {
    case PCV_VM_START_ALREADY_ACTIVE:
        *rb_feedback = 1;                                                
        return "skipped";
    case PCV_VM_START_REJECTED:
        *rb_feedback = 0;                                      
        return "skipped";
    case PCV_VM_START_SUBMITTED:
    default:
        break;
    }
                                                  
    if (done_ok) {
        *rb_feedback = 1;
        return "success";
    }
    if (done_error && done_error->domain == G_IO_ERROR &&
        (done_error->code == G_IO_ERROR_BUSY || done_error->code == G_IO_ERROR_NOT_FOUND ||
         done_error->code == G_IO_ERROR_NOT_CONNECTED)) {
        *rb_feedback = 0;                                                          
        return "failed";
    }
    *rb_feedback = -1;                                                  
    return "failed";
}
