   
                           
                                                              
  
                           
                                                            
                                                                   
  
                                                                                  
                                              
  
                                                    
                                                            
  
            
                                                                    
  
                                                     
                                                            
   
#ifndef PURECVISOR_DISPATCHER_HOTPLUG_DISK_XML_H
#define PURECVISOR_DISPATCHER_HOTPLUG_DISK_XML_H

#include <glib.h>

G_BEGIN_DECLS

   
                                                                      
                                                     
                                                                    
                                                                 
   
gchar *pcv_hotplug_select_disk_xml(const gchar *domain_xml,
                                   const gchar *target,
                                   GError **error);

   
                                                                      
                                                                        
                                                                          
                                                                                  
                                                                       
                                                              
                                                          
   
gboolean pcv_hotplug_disk_detach_plan(const gchar *live_xml,
                                      const gchar *config_xml,
                                      const gchar *target,
                                      gboolean is_active,
                                      gboolean *live_out,
                                      gboolean *config_out,
                                      gchar **device_xml_out,
                                      GError **error);

G_END_DECLS

#endif                                               
