#ifndef PURECVISOR_VM_DIAG_SAMPLER_H
#define PURECVISOR_VM_DIAG_SAMPLER_H

   
                          
                                                                                
  
                           
                                                          
                                                                               
                                                                         
                                 
                                                                                       
                                  
                                                                                  
                                                              
                                                                    
                                  
  
                       
                                                                   
                                                            
                                  
  
                                                                 
                                                      
                                                                
                                                      
   

#include <glib.h>

G_BEGIN_DECLS

   
                                              
                                                        
   
void pcv_vm_diag_sampler_collect(void);

   
                                                 
                                                          
   
void pcv_vm_diag_sampler_finish(void);

   
                                                               
                                                          
                                                        
                                   
                                    
                                   
                                    
   
void pcv_vm_diag_sampler_note_host_psi(const gchar *resource, gboolean have_some,
                                       gdouble some_avg10, gboolean have_full,
                                       gdouble full_avg10);

   
                                                   
                                 
   
void pcv_vm_diag_sampler_shutdown(void);

   
                                                                                      
                                                                    
   
void pcv_vm_diag_sampler_invalidate_owners(void);

G_END_DECLS

#endif                                   
