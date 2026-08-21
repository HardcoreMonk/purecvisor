   
                          
                                                       
  
                           
                                                            
                                                                    
  
                                                    
                                             
  
            
                                                                    
  
                                                     
                                                            
   
#ifndef PURECVISOR_DISPATCHER_HOTPLUG_NIC_XML_H
#define PURECVISOR_DISPATCHER_HOTPLUG_NIC_XML_H

#include <glib.h>

G_BEGIN_DECLS

gchar *pcv_hotplug_select_nic_xml(const gchar *domain_xml,
                                  const gchar *mac,
                                  GError **error);

G_END_DECLS

#endif                                              
