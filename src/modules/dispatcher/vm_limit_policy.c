                                                                             
                                           
                                                                          
  
                           
                                                   
                                                    
                                        
                                                                                
  
                                                       
                                                        
                                                                                

#include "vm_limit_policy.h"

                                                                            
gboolean
pcv_vm_limit_cpu_quota_us(gint cpu, gint64 *quota_us)
{
    if (cpu == -1) {
        *quota_us = -1;
        return TRUE;
    }
    if (cpu > 0) {
        *quota_us = (gint64)cpu * 1000;
        return TRUE;
    }
    return FALSE;
}
