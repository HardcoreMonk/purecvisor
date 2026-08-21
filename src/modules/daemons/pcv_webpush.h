  
                                          
                           
                                                         
                                                    
                       
                                                 
                                           
   
#ifndef PURECVISOR_PCV_WEBPUSH_H
#define PURECVISOR_PCV_WEBPUSH_H
#include <glib.h>
#include <json-glib/json-glib.h>
G_BEGIN_DECLS

   
                                                                  
  
                           
                                                   
                                                    
                                        
  
                                               
                                                  
                                                          
                                                      
                                                      
               
  
                                                               
                                                                     
                                                
                                                    
       
                                                                       
                                                                    
                                                  
                                                                 
                                            
                                                         
                                                    
                                                   
                                                             
                                          
                                                       
                                                            
                                                                                             
                                                                  
   

   
                                                   
  
                                                  
  
                                                              
                                                   
                                                               
                                                       
   
enum {
    WP_ERR_ARG      = 1,                    
    WP_ERR_NOT_INIT = 2,                       
    WP_ERR_DB       = 3,                  
    WP_ERR_BLOCKED  = 4,                   
    WP_ERR_NOT_FOUND= 5,                           
    WP_ERR_KEY      = 6,                            
    WP_ERR_LIMIT    = 7,                               
};

   
                          
  
                                                  
                           
  
                                                                 
                                             
                                                 
  
                                   
   
GQuark pcv_webpush_error_quark(void);

   
                                       
  
                                             
                                             
                   
  
                                                                             
                                                           
                                                      
                                        
                                                 
   
gboolean pcv_webpush_init(const gchar *db_path, const gchar *vapid_pem_path,
                          GError **error);

   
                                         
  
                                                 
                                                 
  
                                                    
                                                        
                                   
                                                         
   
void pcv_webpush_shutdown(void);

   
                                                
  
                                             
                                               
                               
  
                                                    
                                        
  
                                                             
                                    
   
gboolean pcv_webpush_wait_idle(void);

   
                                      
  
                                                 
  
                                                           
                                                      
                                                    
                                                   
                                                    
                                                    
   
#define PCV_WEBPUSH_MAX_SUBS_PER_USER 10u

   
                                             
  
                                              
                                         
  
                                                         
                                                         
                                            
                                           
                                                 
                                                 
                                                               
                                                      
                   
   
gboolean pcv_webpush_subscribe(const gchar *username, const gchar *endpoint,
                               const gchar *p256dh, const gchar *auth,
                               GError **error);

   
            
  
                                                
                                          
  
                                
                                                          
                                
                                  
                                
   
gboolean pcv_webpush_unsubscribe(const gchar *endpoint, const gchar *username,
                                 GError **error);

   
                      
  
                                               
                       
  
                                                                  
                                                  
  
                                                                 
                                      
   
JsonArray *pcv_webpush_list(void);

   
                                                        
  
                                                     
                                                        
                                              
  
                                                                  
                                                                     
                                                     
  
                                                          
                                                     
                                                
  
                                                         
                                                                  
   
JsonArray *pcv_webpush_list_mine(const gchar *username);

   
                                                          
  
                                              
                                          
  
                                                          
   
gchar *pcv_webpush_vapid_public(void);

   
                                 
  
                                             
                                              
  
                               
                                     
   
guint pcv_webpush_vapid_rotate(GError **error);

   
                                       
  
                                               
  
                          
                                 
   
guint pcv_webpush_remove_user(const gchar *username);

   
                                  
  
                                                     
                     
  
                                                       
                                                      
                     
  
                                                                      
                                                   
                                                                    
                                                    
                           
   
void pcv_webpush_set_policy(gboolean enabled, gboolean crit_only,
                            const gchar *contact);

   
                                                 
  
                                               
                                               
  
                                                 
                                                                             
                                        
   
void pcv_webpush_notify(const gchar *source, gboolean is_crit,
                        const gchar *message);

   
                                                   
  
                                               
                                              
                            
  
                                                  
                                                             
                                                                           
                                              
  
                                                    
                                                                   
                                         
                                                             
                     
  
                           
                                  
                     
   
gboolean pcv_webpush_endpoint_allowed(const gchar *endpoint, GError **error);

   
                                    
  
                                                
                                                        
  
                                                 
                                                   
                                                 
   
gboolean pcv_webpush_send_test(const gchar *username, GError **error);

   
                                           
  
                                 
                                     
                           
                                                                 
                                                    
                              
                                     
   
typedef guint (*PcvWebpushPostFn)(const gchar *endpoint, const guchar *body,
                                  gsize len, const gchar *vapid_auth,
                                  const gchar *urgency, guint ttl);

                                                        
                                                              
                                           
   
                         
                                        
   
void pcv_webpush_set_post_hook(PcvWebpushPostFn fn);

                                              
void test_webpush_register(void);

G_END_DECLS
#endif
