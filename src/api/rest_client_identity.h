   
                               
                                          
  
                           
                                                   
                                                    
                                        
  
                                                        
                                                      
                                                      
                                                     
                         
  
                                             
                         
   

#ifndef PCV_REST_CLIENT_IDENTITY_H
#define PCV_REST_CLIENT_IDENTITY_H

#include <libsoup/soup.h>

G_BEGIN_DECLS

#define PCV_TYPE_REST_CLIENT_IDENTITY (pcv_rest_client_identity_get_type())
G_DECLARE_FINAL_TYPE(PcvRestClientIdentity,
                     pcv_rest_client_identity,
                     PCV,
                     REST_CLIENT_IDENTITY,
                     GObject)

   
                              
                                           
                                         
                                                     
  
                                             
                                                 
                                                   
                                              
  
                                                                
   
gchar *pcv_rest_resolve_client_ip(const gchar *peer_ip,
                                  const gchar *x_real_ip,
                                  const gchar *x_forwarded_for);

   
                                   
                                           
                                                         
  
                                                     
                                                         
  
                                        
   
gboolean pcv_rest_resolve_external_https(const gchar *peer_ip,
                                         const gchar *x_forwarded_proto);

   
                                
                                                           
  
                                                         
                                      
  
                                                  
   
const PcvRestClientIdentity *pcv_rest_client_identity_get(
    SoupServerMessage *request);

   
                                          
                      
  
                                                            
   
const gchar *pcv_rest_client_identity_get_client_ip(
    const PcvRestClientIdentity *identity);

   
                                              
                      
  
                                                
   
gboolean pcv_rest_client_identity_is_external_https(
    const PcvRestClientIdentity *identity);

   
                                          
  
                                                  
                                                               
   
gboolean pcv_client_identity_admission_try(GHashTable *counts,
                                           const gchar *identity_key,
                                           gint limit);

G_END_DECLS

#endif                                 
