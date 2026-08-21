   
                        
                                                                                      
  
                           
                                                   
                                                    
                                        
  
                                                      
                                            
                                                     
                                             
                                                         
                      
                                                           
                                                     
  
                              
                                                                    
                                                                  
                                                             
                                                            
                                                               
                                             
  
                                                                   
                                                                      
                                                         
                                                        
                                        
  
                
                                                   
                                                                  
                        
                    
                                
  
         
                                                                 
                                                    
                                                                  
                             
  
            
                                                                    
                                                      
                                            
                                                              
                                                              
                              
  
               
                                                                        
                                                                  
                                                             
                                                                  
                                                                 
                                               
                                                      
                                                   
                                                        
                                                    
                                               
                                    
  
                             
                                                                      
                                                            
                                                    
                                            
                                                         
                                                    
   
#ifndef PURECVISOR_PCV_QOS_CHAOS_H
#define PURECVISOR_PCV_QOS_CHAOS_H
#include <glib.h>
#include <json-glib/json-glib.h>
G_BEGIN_DECLS

                                                
#define PCV_QOS_CHAOS_MAX_SEC 3600u

                                                                             
                                              
                                                                                

   
                               
                             
  
                                                              
   
gboolean pcv_qos_chaos_timebox_valid(guint timebox_sec);

   
                                  
                                                         
                                                          
                            
  
                                                         
                                                      
                                                 
                                                          
                                               
                                                
                          
  
                                                                    
                                                                    
                                              
   
gchar **pcv_qos_chaos_profile_validate(const gchar *profile, GError **error);

   
                                 
                                                      
  
                                                       
                                                      
                                                           
                                         
  
                                    
   
gboolean pcv_qos_chaos_resolve_dry_run(JsonObject *params);

   
                                     
                                                                       
                    
  
                                                
                                                                    
                                                                        
                                                 
  
                                                                       
                                                    
   
GPtrArray *pcv_qos_chaos_parse_netem_parents(const gchar *tc_qdisc_show_output);

                                                                             
                                                         
                                                                                

   
                       
                                                       
                                                           
                                      
                                                           
                                                              
                                                              
                                                                
                                                  
                                                          
                                                 
                     
  
                                                                     
                                                       
                                                                     
                                                   
                                                           
  
                                                    
                                                 
                                          
                                      
  
                                                               
   
gboolean pcv_qos_chaos_start(const char *vm, const char *profile, guint timebox_sec,
                             const char *admin, gboolean dry_run, GError **error);

   
                      
                                              
                                        
                                          
                     
  
                                                        
                                                      
                                                    
                                                  
                                      
  
                                          
   
gboolean pcv_qos_chaos_stop(const char *vm, const char *admin, GError **error);

   
                           
                                                       
  
                                                             
                                                   
                                                        
                                                          
                                                      
                                                   
            
   
void pcv_qos_chaos_purge_all(void);

   
                                                     
                         
                                             
                                              
                                          
                                                       
   
typedef struct {
    gchar  *vm;
    gchar  *profile;
    gchar  *admin;
    guint   timebox_sec;
    gint64  expires_at;
} PcvQosChaosStatusEntry;

   
                        
                                               
  
                                          
  
                                                                  
                                                            
                      
   
GPtrArray *pcv_qos_chaos_status(void);

   
                       
  
                                                    
                                               
                                                           
                                                      
                                                      
                                                                     
                                                     
                         
   
void pcv_qos_chaos_clear(void);

G_END_DECLS
#endif                                 
