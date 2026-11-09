   
                           
                                                                  
  
                           
                                                                    
                                                                       
                                                         
                                                                            
  
                                                              
   
#ifndef PURECVISOR_VM_DELETE_STATUS_H
#define PURECVISOR_VM_DELETE_STATUS_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

   
                                                                  
                                                                                         
                                             
                                       
                                                    
                                   
                               
   
gchar *pcv_vm_delete_job_result(const gchar *name, gboolean already_absent,
                                const GPtrArray *warnings, const gchar *error);

   
                                                            
                                                                
                                                                                               
                                                 
                                                      
                                         
                                                    
                                         
   
JsonObject *pcv_vm_delete_status_build(const gchar *name, JsonObject *job);

G_END_DECLS

#endif                                    
