   
                    
                                                   
  
                           
                                                   
                                                    
                                        
  
                                                        
                                                     
                                                     
  
                                                                   
                                                      
                                                                
                                                      
  
                                                                  
                                             
  
                  
                                                     
                                                     
                                       
  
             
                                                                    
                                                      
                                                                       
                                                                                    
  
                                                               
                                                 
                                                                            
  
                          
                                                                 
                                                             
                                                           
  
           
                                                                              
                                      
                                                                
  
         
                                                              
                                               
                                                 
                                                   
   

#ifndef PURECVISOR_SPAWN_H
#define PURECVISOR_SPAWN_H

#include <gio/gio.h>
#include "pcv_privdrop.h"

G_BEGIN_DECLS

                                                                            
                                           
  
                                                        
                                                
                                
  
          
                                                                            
                                                
                                                
  
                                                          
                                                  
                                                                               

   
                           
                                               
                                                          
                               
                                  
                           
                                             
                                                                
   
void pcv_spawn_launcher_init(void);

   
                               
                                 
                                            
                                                
                               
   
void pcv_spawn_launcher_shutdown(void);

   
                                         
                                                                           
                                                         
                         
   
PcvChildCapabilityProfile
pcv_spawn_capability_profile_for_argv(const gchar * const *argv);

   
                  
                                                                    
                                                            
                                                 
   
GSubprocess *pcv_spawn_newv(const gchar * const *argv,
                            GSubprocessFlags flags,
                            GError **error);

   
                          
                                                        
                                                                
   
GSubprocess *pcv_spawn_newv_profile(const gchar * const *argv,
                                    GSubprocessFlags flags,
                                    PcvChildCapabilityProfile profile,
                                    GError **error);

   
                  
                                            
                                              
                                                                      
                                  
                                                                      
                                  
                                   
  
                                    
                                                   
                                                      
  
                                      
                                            
   
gboolean pcv_spawn_sync(const gchar * const *argv,
                        gchar              **stdout_out,
                        gchar              **stderr_out,
                        GError             **error);

   
                          
                                                    
                                                                 
   
gboolean pcv_spawn_sync_profile(const gchar * const *argv,
                                PcvChildCapabilityProfile profile,
                                gchar **stdout_out,
                                gchar **stderr_out,
                                GError **error);

   
                          
                                               
                                                          
                                                          
                                                                
                                                                 
  
                                                             
                                       
                                                             
                                 
   
gboolean pcv_spawn_sync_timeout(const gchar * const *argv,
                                gchar **stdout_out, gchar **stderr_out,
                                guint timeout_sec, GError **error);

   
                      
                                            
                                                         
                                                         
                                                       
                                                          
                                                          
                                   
  
                                                            
                                                        
                                                        
                                                              
                                                                   
                                                                  
                                                                           
                          
                                                               
  
                                 
   
gboolean pcv_spawn_sync_env(const gchar * const *argv,
                            const gchar * const *envp,
                            gchar              **stdout_out,
                            gchar              **stderr_out,
                            GError             **error);

   
                  
                            
                                                                
  
                                    
                                     
                              
                                                     
   
void pcv_spawn_fire(const gchar * const *argv);

   
                       
                                                                   
                                                                     
                                                                          
                                                                         
                              
  
                                                               
                                               
                                             
                   
                                                              
  
                                      
   
gboolean pcv_spawn_pipe_sync(const gchar * const *producer_argv,
                             const gchar * const *consumer_argv,
                             gchar              **consumer_stdout_out,
                             gchar              **combined_stderr_out,
                             GError             **error);

   
                        
                                            
                                                
                                                
                                                               
                                                               
                                   
  
                                                                     
                                                        
                            
                                                           
  
                                      
   
gboolean pcv_spawn_sync_stdin(const gchar * const *argv,
                              const gchar          *input,
                              gssize                input_len,
                              gchar               **stdout_out,
                              gchar               **stderr_out,
                              GError              **error);

G_END_DECLS

#endif                         
