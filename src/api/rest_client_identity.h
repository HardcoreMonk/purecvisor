   
                               
                                                                  
  
                           
                                                   
                                                    
                                        
  
                                                        
                                                      
                                                         
                                                     
                         
  
                                                                      
                                                                              
   

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

   
                     
                                                                          
                                                                                            
                                                      
   
typedef struct PcvRestProxyTrust PcvRestProxyTrust;

   
                            
                                                                                         
                                                         
                                                                                        
                                                         
   
PcvRestProxyTrust *pcv_rest_proxy_trust_new(const gchar *trusted_proxies, const gchar *secret,
                                            GPtrArray **rejected_entries);

                                      
void pcv_rest_proxy_trust_free(PcvRestProxyTrust *trust);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PcvRestProxyTrust, pcv_rest_proxy_trust_free)

   
                                
                                                                             
                                                
                           
   
void pcv_rest_proxy_trust_install(PcvRestProxyTrust *trust);

   
                                    
                                                                                                     
                                                                                   
                                        
   
void pcv_rest_proxy_trust_load_config(void);

   
                          
                                                           
                                                      
                                                                                 
                      
   
gboolean pcv_rest_proxy_trusted(const PcvRestProxyTrust *trust, const gchar *peer_ip,
                                const gchar *proxy_auth_header);

   
                              
                                                             
                                                                                               
                                                  
   
gchar *pcv_rest_resolve_client_ip(gboolean trusted, const gchar *peer_ip,
                                  const gchar *x_real_ip, const gchar *x_forwarded_for);

   
                                   
                                                              
   
gboolean pcv_rest_resolve_external_https(gboolean trusted, const gchar *x_forwarded_proto);

   
                            
                                                                                          
                                                 
   
gboolean pcv_rest_is_direct_local(const gchar *peer_ip, gboolean has_forwarding_header);

   
                                
                                                           
  
                                                         
                                      
  
                                                  
   
const PcvRestClientIdentity *pcv_rest_client_identity_get(
    SoupServerMessage *request);

   
                                          
                      
  
                                                            
   
const gchar *pcv_rest_client_identity_get_client_ip(
    const PcvRestClientIdentity *identity);

   
                                              
                      
  
                                                                   
   
gboolean pcv_rest_client_identity_is_external_https(
    const PcvRestClientIdentity *identity);

   
                                            
                                                                                 
   
gboolean pcv_rest_client_identity_is_direct_local(const PcvRestClientIdentity *identity);

   
                                          
  
                                                  
                                                               
   
gboolean pcv_client_identity_admission_try(GHashTable *counts,
                                           const gchar *identity_key,
                                           gint limit);

G_END_DECLS

#endif                                 
