   
                        
                                                          
  
                           
                                                   
                                                    
                                        
  
                                                
                                               
                                              
                                                
                                
  
                   
                                                   
                                                                             
                                               
  
                                                  
                                                         
                                                       
                                                                        
                                                                
  
                                                                              
                                                             
                                                        
                                                    
                                                                 
                                 
                                                               
                                          
  
                                                               
                                                
                                                             
                                                
                                                       
                                                               
                                                         
                                                    
  
                                              
                                        
                                                                
  
            
                                             
                                                      
                                               
                                              
  
                
                                                                      
                                                                 
                     
                                                                     
                                       
                                                                             
                                                              
                                               
                                                                          
                                                          
                                                              
             
                                                  
  
                              
                                                                        
                                                              
                                                                     
                                         
   
#ifndef PURECVISOR_ISCSI_MANAGER_H
#define PURECVISOR_ISCSI_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

                                                                    
  
                                                           
                                             
                                                
                                  
  
                                                     
                                                      
                                                       
                                                              
                                                
                                                      
                                         
  
      
                                                          
                                                                        
                                                   
                                                         
                                                             
                                                                 
                                                    
                                             
                                                         
                                      
                                                                          

               
                                                         
                                                             
                                                  
                                               
                                                      
void pcv_iscsi_init(void);
                                                           
                                                            
                                                    
                                                   
void pcv_iscsi_init_at(const gchar *root);
                                                           
                                                   
void pcv_iscsi_shutdown(void);

                                                  
                                                                
                                                  
                                                                  
                                                    
gboolean    pcv_iscsi_target_create(const gchar *vm_name, const gchar *zvol_path, GError **error);
                                                                    
                                                              
                                                
                                                   
                                                 
gboolean    pcv_iscsi_target_create_at(const gchar *root, const gchar *vm_name,
                                       const gchar *zvol_path, GError **error);
                                                                   
                                                          
                                                            
                                                  
                                 
gboolean    pcv_iscsi_target_delete(const gchar *vm_name, GError **error);
                                                                    
                                                  
                                                    
                                                                     
                                                   
gboolean    pcv_iscsi_target_delete_at(const gchar *root, const gchar *vm_name,
                                       GError **error);
                                                                 
                                                        
                                                             
                                    
JsonArray  *pcv_iscsi_target_list(void);

                                                              
                                                            
                                                 
                                                       
                                                                     
                                                         
  
        
                                                       
                                                                
                                                         
                                                                       
                                  
                                           
                                                        
  
                                                                    
                                                       
                                                  
                                                                          
                                                  
                                                        
                                                       
                                                             
  
                                                   
                                                          
                                               
                                                
                   
gboolean    pcv_iscsi_chap_validate(const gchar *chap_user, const gchar *chap_password,
                                    GError **error);

                                                       
                                                           
                                                                   
                                                                          
                                                      
                                                                
                                     
                                                 
                                                             
                 

                                                           
                                                              
                                                           
                                                                    
                                              
                                             
gboolean pcv_iscsi_initiator_connect(const gchar *target_ip, const gchar *vm_name,
                                      gchar **device_path, GError **error);
                                                                    
                                       
                                 
gboolean pcv_iscsi_initiator_disconnect(const gchar *target_ip, const gchar *vm_name,
                                         GError **error);

G_END_DECLS

#endif                                 
