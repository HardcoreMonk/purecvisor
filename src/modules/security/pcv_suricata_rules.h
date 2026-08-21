   
                             
                                                    
  
                           
                                                   
                                                    
                                        
  
                                                             
                                                          
                                                       
                                                         
                                                           
  
                                                    
                                                                          
                                                             
                                                      
                                                              
                                                        
                                         
  
            
                                                                            
                                                                             
  
                                                     
                                                
                                                                  
                                                              
                                                             
                              
                                                         
                                                  
                                                                         
                                            
                                                                
                                       
                                                                     
                                                   
                                                  
                                                  
                                                  
                                  
                             
  
                                                                      
                                                                  
                                                                 
                                               
                                                       
                                                          
                              
  
                                                                      
                                                     
                                                           
                                             
                                                              
                                                 
                                              
  
                                                        
                                                        
                                                   
                                              
                       
  
                                                     
                                                            
                                                            
                                              
                                                                 
                                                
                    
  
                                                          
                                                                 
                                                                    
                                                                 
                                                             
                                                                     
                                                                      
         
                                                                
                                                       
          
                                                                     
                                                        
                                                             
                                              
  
                                                             
                                                      
                                                        
                                                       
                                      
   
#ifndef PURECVISOR_PCV_SURICATA_RULES_H
#define PURECVISOR_PCV_SURICATA_RULES_H

#include <glib.h>

G_BEGIN_DECLS

                                                                   
                                                   
#define PCV_SURICATA_RULES_DIR  "/var/lib/suricata/rules"
#define PCV_SURICATA_RULES_FILE "suricata.rules"
#define PCV_SURICATA_RULES_PATH PCV_SURICATA_RULES_DIR "/" PCV_SURICATA_RULES_FILE

                                                               
#define PCV_SURICATA_CONFIG_PATH "/etc/suricata/suricata.yaml"

   
                              
                                                          
                               
                                                   
                            
  
                                                            
                                                    
               
  
                         
   
typedef gboolean (*PcvSuricataRulesDownloadFn)(const gchar *url, const gchar *tmp_path,
                                                GError **error);

   
                              
                                                         
                            
  
                                                            
                                           
  
                       
   
typedef gboolean (*PcvSuricataRulesValidateFn)(const gchar *rules_path, GError **error);

   
                            
                            
  
                                                         
                                                
                    
  
                           
   
typedef gboolean (*PcvSuricataRulesReloadFn)(GError **error);

   
                         
                                                                  
                                                     
                                                     
   
typedef struct {
    PcvSuricataRulesDownloadFn download;
    PcvSuricataRulesValidateFn validate;
    PcvSuricataRulesReloadFn   reload;
} PcvSuricataRulesHooks;

   
                               
                            
                            
  
                                                            
  
                                                                  
                                                    
                                                           
                                                              
                        
  
                                                  
  
                                                           
   
gboolean pcv_suricata_rules_validate(const gchar *rules_path, GError **error);

   
                                           
                                                       
                                                           
                                                          
                                                        
                                                         
                                                        
                    
                                      
                                                            
                                                                       
                                        
                            
  
                                                    
                                                               
                                                           
                                         
                                                                              
                                                              
                                                                 
                            
  
                                                       
                                                 
                                      
                                         
  
                                                    
                                    
  
                                                         
   
gboolean pcv_suricata_rules_update_at(const gchar *rules_dir, const gchar *url,
                                      const gchar *admin, PcvSuricataRulesHooks *hooks,
                                      GError **error);

   
                             
                                                 
                                      
                            
  
                                                               
  
                                                      
                                                        
                                                           
                                                       
                                                           
       
  
                                                         
                                
  
                                               
   
gboolean pcv_suricata_rules_update(const gchar *source_url, const gchar *admin,
                                   GError **error);

G_END_DECLS

#endif                                      
