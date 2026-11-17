                                                                             
                              
                                                                                                                         
  
                           
                                                                                                             
                                                                             
                                                                                      
                                        
           
                                                                                             
                                                                                          
                                          
                                                                        
                                                                                 
                                                                                     
                                                                                                      
                                                                                                  
                                                                        
                                                                                
                                    
  
                       
                                                                                
                                                                             
                                                                                
#ifndef PURECVISOR_VM_OWNER_H
#define PURECVISOR_VM_OWNER_H

#include <glib.h>
#include <libxml/tree.h>

G_BEGIN_DECLS

                                                                                
#define PCV_VM_OWNER_NS_URI "urn:purecvisor:metadata"

   
                                                                                              
                                                                                
                                                                        
                                                
   
gchar *pcv_vm_owner_from_domain_node(xmlNode *root);

   
                                                                                                        
                                                                                   
                                     
   
gchar *pcv_vm_owner_from_domain_xml(const gchar *xml);

G_END_DECLS

#endif                            
