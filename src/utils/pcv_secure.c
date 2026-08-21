                            
   
                                           
  
                           
                                                   
                                                    
                                        
  
                                               
                          
  
                                                                           
                                                               
                                                     
                                                       
                                                                             
                                                      
                                   
                                                             
                                                         
                                    
   
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                                                            
#endif
#include "utils/pcv_secure.h"
#include <string.h>
                                          
void pcv_secure_wipe(void *p, size_t n) {
    if (!p || n == 0) return;                                  
    explicit_bzero(p, n);                           
}
                                                          
void pcv_secure_free_str(gchar **p) {
    if (!p || !*p) return;                                       
    pcv_secure_wipe(*p, strlen(*p));                                   
    g_free(*p);                                   
    *p = NULL;                                                        
}
