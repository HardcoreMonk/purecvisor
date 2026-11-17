                                                                             
                                   
                                                                                                           
  
                           
                                                                                                                        
                                                                                                    
                                                                                         
                                                                                  
                                   
  
                       
                                                                               
                  
                                                                                
#ifndef PURECVISOR_VM_OWNER_META_H
#define PURECVISOR_VM_OWNER_META_H

#include <glib.h>
#include <libvirt/libvirt.h>

G_BEGIN_DECLS

   
                                                                                                                       
                                                   
   
gchar *pcv_vm_owner_metadata_fragment(const gchar *owner);

   
                                                                                                    
                                                                                                                                 
   
gchar *pcv_vm_owner_metadata_read(virDomainPtr dom, gboolean live);

   
                                                                                                           
                                                                                   
                                                                                                           
                                                                                       
   
gboolean pcv_vm_owner_metadata_set(virDomainPtr dom, const gchar *owner, const gchar *previous, gboolean *live_updated,
                                   GError **error);

G_END_DECLS

#endif                                 
