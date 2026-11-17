                                                                             
                                       
                                                                                               
  
                           
                                                                               
                                                                                                                
                                                                                              
                                                                                                
                                                                                                 
                                                                                                   
                                                                           
                                                                                                
                                                                   
                                                                                       
                                                                                                
                                                                                  
                  
  
                       
                                                                                  
                                                                                      
                                                                                
#ifndef PURECVISOR_VM_READ_SCOPE_H
#define PURECVISOR_VM_READ_SCOPE_H

#include <glib.h>

#include "modules/resource/resource_gate.h"
#include "utils/pcv_job_queue.h"

G_BEGIN_DECLS

   
                                             
                                                        
                                          
                                                 
                                                                                  
                                                 
                                                                           
                                                                        
                                                  
   
gboolean pcv_vm_read_decide(gint role, const gchar *subject, const gchar *caller_tenant, const gchar *owner,
                            const gchar *owner_tenant);

                                             
typedef struct _PcvVmReadScope PcvVmReadScope;

   
                                                                                                      
                                                                               
                                                        
   
PcvVmReadScope *pcv_vm_read_scope_new(const PcvResCaller *caller);

   
                                                                                                
                                                                                              
                                                                                             
                                 
                                                                  
   
PcvVmReadScope *pcv_vm_read_scope_new_parts(gint role, const gchar *subject, gboolean tenant_known, const gchar *tenant);

                                                                                    
gboolean pcv_vm_read_scope_all(const PcvVmReadScope *s);

   
                                                                                
                                                                                          
                                                                                 
                  
                                                                 
   
gboolean pcv_vm_read_visible(PcvVmReadScope *s, const gchar *owner);

                                                    
void pcv_vm_read_scope_free(PcvVmReadScope *s);

  
                                                                                        
                                                                                                 
                                                                                                   
                                                            
   
typedef struct _PcvVmOwnerMap PcvVmOwnerMap;

   
                                                                                                           
                                                                                                           
                                                                          
   
PcvVmOwnerMap *pcv_vm_owner_map_current(gint64 now_mono_us);

   
                                                                                                       
                                                                                            
                                               
   
gboolean pcv_vm_read_visible_name(PcvVmReadScope *s, const PcvVmOwnerMap *m, const gchar *name);
                                                                                  
gboolean pcv_vm_read_visible_uuid(PcvVmReadScope *s, const PcvVmOwnerMap *m, const gchar *uuid);
                                                                                             
gboolean pcv_vm_read_visible_domain_id(PcvVmReadScope *s, const PcvVmOwnerMap *m, gint domain_id);
   
                                                                                                             
                                                                                                       
                                                
   
void pcv_vm_read_job_scope(const PcvVmReadScope *s, PcvJobScope *out);

                                                
void pcv_vm_owner_map_free(PcvVmOwnerMap *m);

G_END_DECLS

#endif                                 
