   
                        
                                                                           
  
                           
                                                                          
                                                                            
                                                                            
                                                                            
  
                                                             
   
#ifndef PCV_REST_INTERNAL_H
#define PCV_REST_INTERNAL_H

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>                                                                
#include "api/rest_server.h"
#include "utils/pcv_log.h"                                                        

#define REST_LOG_DOM   "rest_server"

#define REST_API_PREFIX "/api/v1"
constexpr int REST_MAX_BODY = 1 * 1024 * 1024;             
constexpr int REST_RPC_TIMEOUT_SEC = 8;                                        
                                                                                         

                                                                    
struct _PcvRestServer {
    GObject      parent_instance;
    SoupServer  *soup;
    guint16      port;
    guint16      https_port;                                                              
    gboolean     tls_active;                         
    PcvRestTransportPlan transport;                             
                                                
                                                                   
                                                   
                                                   
    GThread     *thread;
    GMainLoop   *rest_loop;
    GMainContext *rest_ctx;
};

                                                                           
                                                                     
                                                              
                                                         
                                           
#define PCV_CSP_POLICY \
    "default-src 'self'; script-src 'self' 'unsafe-inline'; " \
    "style-src 'self' 'unsafe-inline'; img-src 'self' data:; " \
    "font-src 'self'; " \
    "connect-src 'self' ws: wss:"

  
                          
  
                   
                                             
                                        
                                         
  
                       
                                                          
                                            
   
typedef struct {
    SoupServerMessage *msg;                                                    
    gchar             *rpc;                                         
    gchar             *rpc_method;                                           
    GMainContext      *rest_ctx;                                                       
} _RestAsyncCtx;

  
                                                             
  
                                    
                                                                 
                                           
                                
                                                                    
   
typedef struct {
    SoupServerMessage *msg;                                      
    gchar *resp;                                             
} _RestUnpauseData;

typedef struct {
    PcvRestServer   *server;
    GTlsCertificate *certificate;
} RestTransportContext;

extern gboolean pcv_rest_hsts_enabled;
extern gchar *pcv_rest_tls_degraded_reason;
extern gboolean pcv_rest_tls_degraded;

                                                                                                 
                                                                                    
                                                     
typedef enum {
    PCV_REST_STEP_NEXT,
    PCV_REST_STEP_CLEANUP,
    PCV_REST_STEP_RETURN,
    PCV_REST_STEP_UNMATCHED,
} PcvRestStep;

                                                                                      
                                                                                             
                                                                  
typedef struct {
    SoupServerMessage *msg;
    const gchar       *path;
    GHashTable        *query;
    PcvRestServer     *self;
    const gchar       *method;
    gchar             *req_id;
    PcvTraceContext   *trace_ctx;
    JsonObject        *body;
    gchar            **segs;
    gchar             *subject;
    gchar             *rpc;
    const gchar       *request_client_ip;
    gint               key_role;
    gint               subject_role;                                                          
    const gchar       *resource;
    const gchar       *name;
    const gchar       *action;
    const gchar       *sub;
} PcvRestRequest;

                                                                               
#define PCV_REST_RUN(call) \
    do { PcvRestStep pcv_rest_st_ = (call); \
         if (pcv_rest_st_ == PCV_REST_STEP_CLEANUP) goto cleanup; \
         if (pcv_rest_st_ == PCV_REST_STEP_RETURN) return; } while (0)

                                    
PcvRestStep
pcv_rest_prelude(PcvRestRequest *rq);
PcvRestStep
pcv_rest_ui_static(PcvRestRequest *rq);
PcvRestStep
pcv_rest_health(PcvRestRequest *rq);
PcvRestStep
pcv_rest_version(PcvRestRequest *rq);
PcvRestStep
pcv_rest_internal(PcvRestRequest *rq);
PcvRestStep
pcv_rest_metrics(PcvRestRequest *rq);
PcvRestStep
pcv_rest_auth_token(PcvRestRequest *rq);
PcvRestStep
pcv_rest_auth_refresh(PcvRestRequest *rq);
PcvRestStep
pcv_rest_auth_logout(PcvRestRequest *rq);
PcvRestStep
pcv_rest_auth_password(PcvRestRequest *rq);
PcvRestStep
pcv_rest_auth_register(PcvRestRequest *rq);
PcvRestStep
pcv_rest_auth_totp_verify(PcvRestRequest *rq);
PcvRestStep
pcv_rest_auth_totp_enroll(PcvRestRequest *rq);
PcvRestStep
pcv_rest_map_compute(PcvRestRequest *rq);
PcvRestStep
pcv_rest_map_observe(PcvRestRequest *rq);
PcvRestStep
pcv_rest_map_platform(PcvRestRequest *rq);

                                          
                                                                                     
                                                                      
gboolean
pcv_rest_collector_gate(SoupServerMessage *msg);
                                                                           
void
pcv_rest_collector_auth_log_config(void);

#endif                          
