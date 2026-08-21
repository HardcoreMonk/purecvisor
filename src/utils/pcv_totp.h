   
                   
                                                         
  
                           
                                                   
                                                    
                                        
  
                                                
                                          
                                               
                         
  
        
                                                                 
                                                     
                                                               
                                        
  
                                                      
                                                       
   

#pragma once
#include <glib.h>

G_BEGIN_DECLS

   
                            
  
                                                          
          
  
                                                  
                                                      
  
                                                            
                                                              
   
gchar *pcv_totp_generate_secret(void);

   
                          
                                                
                                 
                             
  
                       
  
                                                          
                                                 
   
gboolean pcv_totp_base32_decode(const gchar *b32, guchar *out, gsize *out_len);

   
                    
                    
                    
                                              
                           
  
                                                                           
  
                                                                
                                                                    
                                                               
                                                                
                                                      
                                                  
   
guint pcv_totp_code_at(const guchar *key, gsize key_len, gint64 step, guint digits);

   
                  
                               
                             
                                  
                                                          
                                              
  
                                                                      
                                                         
  
                                                            
                                        
                                               
                                                   
   
gboolean pcv_totp_check(const gchar *secret_b32, const gchar *code,
                        gint64 now_unix, gint64 last_step, gint64 *out_step);

   
                      
                            
                                           
                                        
  
                                                                   
  
                                                       
                                                    
   
gchar *pcv_totp_build_uri(const gchar *username, const gchar *hostname, const gchar *secret_b32);

G_END_DECLS
