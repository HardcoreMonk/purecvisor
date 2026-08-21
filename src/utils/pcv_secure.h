                            
   
                                                      
  
                           
                                                   
                                                    
                                        
  
                                                 
                                                  
                                                   
                            
  
                                                       
                                                                
                                                 
                                                            
                                             
                                                                     
   
#ifndef PURECVISOR_SECURE_H
#define PURECVISOR_SECURE_H
#include <glib.h>
#include <string.h>
G_BEGIN_DECLS
                                                                      
                                                       
                                              
                                                                    
void pcv_secure_wipe(void *p, size_t n);
                                                                            
                                                       
                                                                    
void pcv_secure_free_str(gchar **p);
                                                            
void test_secure_register(void);
G_END_DECLS
#endif
