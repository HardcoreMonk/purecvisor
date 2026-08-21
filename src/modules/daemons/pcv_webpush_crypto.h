  
                                                 
                           
                                                           
                                                  
                       
                                                   
                                              
   
#ifndef PURECVISOR_PCV_WEBPUSH_CRYPTO_H
#define PURECVISOR_PCV_WEBPUSH_CRYPTO_H
#include <glib.h>
G_BEGIN_DECLS

   
                                                          
  
                           
                                                   
                                                    
                                        
                                                           
  
                                               
                                                   
                                                  
                                                  
                                                     
                             
  
                                                        
                                                     
                                
                                               
                                                 
                                                    
                                
                                               
                                                                      
                                                             
                                        
                                            
                                               
                                          
                                                   
                                                     
                            
                                                   
                
                                                                         
                                                                       
   

   
                                 
  
                                                 
                                            
                            
  
                                                         
                                     
                                                                 
                                                          
                                     
                                                  
   
gboolean pcv_webpush_keypair_generate(gchar **priv_b64url,
                                      gchar **pub_b64url,
                                      GError **error);

   
                                      
  
                                                
                                               
                                                 
  
                                                                          
                                                               
                                                 
  
                                                            
                                                      
                                                              
                                               
                                                                                
                                                                  
                                            
                                          
                                               
   
gchar *pcv_webpush_vapid_jwt(const gchar *aud_origin,
                             const gchar *sub_contact,
                             const gchar *vapid_priv_b64url,
                             gint64 now_epoch,
                             GError **error);

   
                                     
  
                                                      
                                              
                                         
  
                                                  
                                                                                    
  
                                                 
                                                             
                                           
                                                             
                                                       
                                                                    
                                                     
                                                           
                                    
                                                
                                 
                                     
                                                       
   
gboolean pcv_webpush_encrypt(const guchar *pt, gsize pt_len,
                             const gchar *p256dh_b64url,
                             const gchar *auth_b64url,
                             const guchar *salt16,
                             const gchar *as_priv_b64url,
                             guchar **out_body, gsize *out_len,
                             GError **error);

                                              
void test_webpush_crypto_register(void);

G_END_DECLS
#endif
