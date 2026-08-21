   
                               
                                                         
  
                           
                                                   
                                                    
                                        
  
                                             
                                           
  
                                                              
                                                       
                                                        
                                                   
        
   
#ifndef PURECVISOR_DAEMON_CONFIG_POLICY_H
#define PURECVISOR_DAEMON_CONFIG_POLICY_H

#include <glib.h>

#include "modules/daemons/alert_engine.h"

G_BEGIN_DECLS

   
                                                               
  
                                       
                                                           
                                               
   
gboolean pcv_daemon_config_set_alert_reload_is_fatal(
    const gchar *section, PcvAlertConfigSetResult reload_result);

   
                                                         
  
                                                     
                                                           
                                                                
                                  
  
                                   
                                        
                    
                          
                                                       
                                          
   
PcvAlertConfigSetResult pcv_daemon_config_set_value(
    const gchar *conf_path,
    const gchar *section,
    const gchar *key,
    const gchar *value,
    GError **error);

G_END_DECLS

#endif                                        
