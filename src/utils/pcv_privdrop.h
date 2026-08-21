   
                       
                                          
  
                           
                                                   
                                                    
                                        
  
                                                    
                                                   
                                            
  
                                                      
                                                     
                                                    
                                                              
                        
  
                                              
                                                
  
                                            
                                                  
                                                                                          
                                      
                                                
  
                        
                  
                                                   
                                                             
                                                              
  
                      
                                                                
  
             
                                   
                                               
                                                         
  
           
                                       
                                             
                                     
  
            
                                                       
                                                           
  
         
                                      
                                                                 
                                                               
                                                           
                               
   

#ifndef PURECVISOR_PRIVDROP_H
#define PURECVISOR_PRIVDROP_H

#include <glib.h>

G_BEGIN_DECLS

   
                             
                                                           
                                                  
                                                   
   
typedef enum {
    PCV_CHILD_CAP_BASE = 0,                             
    PCV_CHILD_CAP_STORAGE,                                     
    PCV_CHILD_CAP_SIGNAL,                             
    PCV_CHILD_CAP_DHCP,                                           
    PCV_CHILD_CAP_RUNTIME,                                        
    PCV_CHILD_CAP_N_PROFILES
} PcvChildCapabilityProfile;

   
                             
                                                                           
                          
                                                     
                          
  
                    
                                                     
                                                     
                                                                        
                                         
                                                   
  
                      
                                                                       
                                       
                                        
                                                    
  
                                            
   
gboolean pcv_privdrop_capabilities(void);

                                                                   
gboolean pcv_privdrop_child_profiles_enabled(void);

                                                                           
guint64 pcv_privdrop_child_profile_mask(PcvChildCapabilityProfile profile);

   
                            
                                                              
                                                                     
                                            
  
                                                              
                          
   
void pcv_privdrop_child_setup(gpointer user_data);

   
                             
                                                                     
                                                           
  
       
                                         
                                          
                                        
  
            
                                           
                                         
  
                  
   
gboolean pcv_privdrop_no_new_privs(void);

   
                        
                                                                       
                                   
                                                                
  
            
                                                   
                                                     
                                   
  
                                                       
                                                                
                                                                 
                                                              
                                                        
                                                 
                                                           
                                                              
                                                            
  
                                                
   
gboolean pcv_privdrop_seccomp(void);

   
                          
                                       
                                                       
  
          
                                                      
                                                   
                                                          
  
                                  
                                 
   
void pcv_privdrop_apply_all(void);

   
                                  
                                                                   
                                                 
                                                 
   
void pcv_privdrop_disable_coredumps(void);

  
                                 
                                                                                 
                                                                   
                                        
   
void pcv_privdrop_enable_coredumps(void);

G_END_DECLS

#endif                            
