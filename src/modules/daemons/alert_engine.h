   
                       
                                                
  
                           
                                                   
                                                    
                                        
  
                                                  
                                                  
                                               
                                                            
  
                                                                               
         
                                                                               
                                               
                                                   
                                   
                                                
                                           
  
                                                                               
           
                                                                               
                                                          
                                                       
  
                                             
                                                 
                                                  
                                                      
                 
                                                    
  
                                                                               
          
                                                                               
                                                      
                                                      
                                                               
                                                     
                                                                
  
                                                                               
           
                                                                               
                                        
                                                                               
                                                 
  
                                                                               
        
                                                                               
                                                                 
                                                    
                                                  
  
                                                                               
                               
                                                                               
                                                              
                                                            
                                                            
                                                            
                                                            
                                                            
                                                            
                                                              
                                                              
                                                          
                                                             
                                                                 
                                                           
                                                                  
                                                                    
                                                           
                                                                        
                                                            
  
                                                                               
                              
                                                                               
                        
                                             
                                             
                                                     
                    
  
                                     
                                                
                                 
                                                    
  
                                        
                                            
                                     
                                              
                           
  
                                                                               
                                   
                                                                               
  
                       
             
                  
                 
                 
                 
                 
                  
                  
                    
                                                              
                          
  
                 
                                                                            
                                               
  
                    
                                                     
                                                                     
                                                
   
#ifndef PURECVISOR_ALERT_ENGINE_H
#define PURECVISOR_ALERT_ENGINE_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "alert_silence.h"                                                   

G_BEGIN_DECLS

   
                         
  
                                                            
                                                      
   
typedef enum {
    PCV_ALERT_CONFIG_SET_OK = 0,
    PCV_ALERT_CONFIG_SET_INVALID,
    PCV_ALERT_CONFIG_SET_CONFLICT
} PcvAlertConfigSetResult;

                                    
typedef enum {
    PCV_ALERT_CONFIG_SOURCE_STARTUP = 0,
    PCV_ALERT_CONFIG_SOURCE_RELOAD
} PcvAlertConfigSourceMode;

   
                                    
  
                                                
  
                                                            
                                                                     
  
                                   
                                      
   
void        pcv_alert_engine_init(void);

   
                                     
  
                                     
  
                                                         
                                                      
  
                                          
                                                  
   
void        pcv_alert_engine_shutdown(void);

   
                                      
  
                                                 
  
                                           
                                                                       
  
                                                                         
  
                                        
                               
   
JsonArray  *pcv_alert_engine_get_history(void);

   
                                        
  
                                                 
  
                                                                    
                                                                  
  
                                                                           
  
                                        
                               
   
JsonObject *pcv_alert_engine_get_config(void);

   
                              
  
                                                     
                                            
  
                                                    
                                                           
                                                 
  
                                                                       
                                                                
                                                    
  
                                        
                                                                  
                              
   
PcvAlertConfigSetResult
            pcv_alert_engine_set_config(JsonObject *cfg,
                                        gint64 expected_revision);

   
                                           
  
                                                              
                                                        
                             
   
PcvAlertConfigSetResult
            pcv_alert_engine_apply_daemon_config(
                JsonObject *cfg,
                PcvAlertConfigSourceMode mode);

   
                                                                    
  
                                             
                                                          
                               
   
PcvAlertConfigSetResult
            pcv_alert_engine_load_daemon_config(
                PcvAlertConfigSourceMode mode);

   
                                                              
  
                                                           
                                                              
   
PcvAlertConfigSetResult
            pcv_alert_engine_reload_daemon_config(void);

   
                                                           
  
                                                      
                                                            
                                       
  
                                     
                         
                                                      
   
gboolean    pcv_alert_engine_validate_daemon_config_data(
                const gchar *data, gsize length);

   
                                                         
  
                                                             
                                                          
            
  
                                       
                                                  
   
void        pcv_alert_engine_restore_daemon_source_status(
                gboolean valid, const gchar *error);

   
                                
  
                                                             
  
                                                      
                                                   
   
void        pcv_alert_record_security_event(const gchar *event_id,
                                            const gchar *severity,
                                            const gchar *summary);

   
                                        
  
                                                  
                                                 
  
                                                             
                              
                                                         
                                          
                                                          
                                                         
  
                                               
  
                                                        
                                        
                                  
                                                        
  
                                                   
   
void        pcv_alert_fire_event(const gchar *source, gboolean is_crit,
                                 gdouble value, const gchar *message);

   
                                                  
  
                                                         
  
                                          
                                                      
  
                                     
   
JsonArray  *pcv_alert_engine_dlq_list(void);

   
                                
  
                                                
  
                      
  
                                                                   
   
JsonObject *pcv_alert_engine_dlq_retry(void);

                                                             

   
                         
                       
  
                                               
                                    
                                                        
   
gboolean    pcv_alert_acknowledge(gint64 alert_id);

                                                         
                                                                   
                                                    

                                                            
                                                                    
                                                
                                                         

   
                     
                      
  
                          
                                                          
  
                                                                        
                                                  
   
JsonObject *pcv_alert_get_sla(const gchar *vm_name);

                                                            

   
                            
                         
                                                 
  
                                
                                              
   
void pcv_alert_set_vm_webhook(const gchar *vm_name, const gchar *webhook_url);

                                                            

   
                     
                      
  
                          
                                                          
  
                                                                         
   
JsonObject *pcv_alert_get_sla(const gchar *vm_name);

                                                            

   
                            
                         
                                                 
  
                                
   
void pcv_alert_set_vm_webhook(const gchar *vm_name, const gchar *webhook_url);

G_END_DECLS

#endif                                
