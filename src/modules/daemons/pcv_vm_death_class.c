   
                                                               
  
                           
                                                   
                                                    
                                        
  
                                                  
                                               
  
                                                     
                                                                
                                                               
                                                                
                                                                    
                                              
                                               
                                                     
                                                     
                                                           
   
#include "pcv_vm_death_class.h"
#include <libvirt/libvirt.h>                                            

                                                               
                                         
gboolean pcv_vm_death_is_anomaly(int event, int detail) {
                                                                   
                                         
    if (event == VIR_DOMAIN_EVENT_CRASHED)
        return TRUE;                                                                    
                                                                       
                                                            
    if (event == VIR_DOMAIN_EVENT_STOPPED)
        return detail == VIR_DOMAIN_EVENT_STOPPED_CRASHED                    
            || detail == VIR_DOMAIN_EVENT_STOPPED_FAILED;                                 
                                                                   
    return FALSE;                                                                
}
