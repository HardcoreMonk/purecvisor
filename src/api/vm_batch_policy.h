   
                          
                                                        
  
                           
                                                   
                                                    
                                        
  
                                                
                                                        
                                                
  
           
                                                                  
                                                                 
                                                          
                                            
  
         
                                                      
                                                                              
                                                                      
                           
  
                       
   
#ifndef PURECVISOR_VM_BATCH_POLICY_H
#define PURECVISOR_VM_BATCH_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

   
                                      
                                                                 
  
                                                   
  
                                                                 
  
                                         
   
gboolean pcv_vm_batch_action_is_whitelisted(const gchar *action);

   
                                                                      
                                                                      
                                          
                                                                                           
                            
   
const gchar *pcv_vm_batch_name_reject_reason(const gchar *vm, GHashTable *seen);

   
                                                                       
                                                                 
                                   
                                                      
                                                                     
   
guint pcv_vm_batch_stop_concurrency(guint pool_max_conn);

G_END_DECLS

#endif                                   
