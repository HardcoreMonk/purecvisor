   
                      
                                   
  
                           
                                                   
                                                    
                                        
  
                                                         
                                                          
                                                     
  
                                                              
                                                
                                                                                           
                                                  
                                                                    
  
           
                         
                                                                   
                                                               
  
                    
                                                                         
                        
                                                              
                            
                                                     
                          
  
                                                       
                                                         
  
                          
                                                       
                                                                
                                                       
                                                     
                                                   
  
                          
                                                     
                                                   
                                                    
                                                        
                                                  
                                                      
                                                         
                                                     
                                                     
                                                     
                                                  
  
                                   
   

#ifndef PCV_REST_SERVER_H
#define PCV_REST_SERVER_H

#include <glib-object.h>
#include <libsoup/soup.h>
#include "dispatcher.h"

G_BEGIN_DECLS

#define PCV_TYPE_REST_SERVER (pcv_rest_server_get_type())

G_DECLARE_FINAL_TYPE(PcvRestServer, pcv_rest_server,
                     PCV, REST_SERVER, GObject)

typedef enum {
    PCV_REST_TLS_INTERNAL,
    PCV_REST_TLS_EXTERNAL_TERMINATION,
} PcvRestTlsMode;

typedef struct {
    const gchar *mode;
    gboolean     enabled;
    gboolean     degraded;
    const gchar *status;
    const gchar *reason;
} PcvRestTlsHealth;

typedef struct {
    PcvRestTlsMode mode;
    gboolean       initialize_tls;
    gboolean       load_certificate;
    gboolean       create_tls_context;
    gboolean       listen_https;
    const gchar   *plaintext_bind_mode;
    const gchar   *plaintext_host;
} PcvRestTransportPlan;

PcvRestTransportPlan pcv_rest_transport_plan(
    PcvRestTlsMode mode,
    const gchar   *configured_plaintext_bind);

   
                                  
  
                                                            
   
const gchar *pcv_rest_auth_audit_address(SoupServerMessage *message);

   
                                         
  
                                                
   
gchar *pcv_rest_rate_limit_key_for_message(SoupServerMessage *message,
                                           const gchar *path,
                                           const gchar *http_method);

typedef gboolean (*PcvRestTransportStep)(gpointer     context,
                                         const gchar *endpoint,
                                         GError     **error);

typedef struct {
    PcvRestTransportStep load_certificate;
    PcvRestTransportStep create_tls_context;
    PcvRestTransportStep listen_plaintext;
    PcvRestTransportStep listen_https;
} PcvRestTransportOps;

typedef struct {
    gboolean certificate_loaded;
    gboolean tls_context_created;
    gboolean plaintext_listening;
    gboolean https_listening;
} PcvRestTransportOutcome;

gboolean pcv_rest_transport_start(const PcvRestTransportPlan *transport,
                                  const PcvRestTransportOps  *ops,
                                  gpointer                    context,
                                  PcvRestTransportOutcome    *outcome,
                                  GError                     **error);

                                                                  
                                                             
                                                       
                                                                 
                                                               
                                 
PcvRestTlsMode pcv_rest_tls_mode_from_config(gboolean https_enabled);

                                                                  
                                                              
                                                        
                                    
typedef void (*PcvRestTlsInitFunc)(void);

void pcv_rest_transport_initialize(const PcvRestTransportPlan *transport,
                                   PcvRestTlsInitFunc           init_func);

PcvRestTlsHealth pcv_rest_tls_health(PcvRestTlsMode mode,
                                     gboolean tls_enabled,
                                     gboolean tls_degraded,
                                     const gchar *degraded_reason);

                                                        
                                                    
guint pcv_rest_http_status_for_rpc_error(gint64 rpc_error_code);

   
                       
                                                            
                                             
                                       
  
                                                
  
                                                
   
PcvRestServer *pcv_rest_server_new(PureCVisorDispatcher *dispatcher,
                                   guint16               port,
                                   PcvRestTransportPlan  transport);

   
                         
                                      
                                      
                                                       
   
gboolean pcv_rest_server_start(PcvRestServer *self, GError **error);

   
                        
                                        
                                     
   
void pcv_rest_server_stop(PcvRestServer *self);

G_END_DECLS

#endif                        
