   
                             
                                                       
  
                           
                                                                                                       
                                                                                                         
                                                                                         
  
                                              
   
#ifndef PCV_REST_PUBLIC_RENDER_H
#define PCV_REST_PUBLIC_RENDER_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include "api/rest_server.h"

                                                                                               
                                            
typedef struct {
    guint        status;
    const gchar *content_type;
    gchar       *body;
} PcvRestRendered;

                     
void pcv_rest_rendered_clear(PcvRestRendered *r);

                                                                                        
void pcv_rest_render_metrics(const gchar *fleet_resp, PcvRestRendered *out);
                                                                                                    
void pcv_rest_render_rpc_result(const gchar *resp, const gchar *fail_detail, PcvRestRendered *out);
                                                                                               
void pcv_rest_render_internal_vms(PcvRestRendered *out);
void pcv_rest_render_internal_telemetry(PcvRestRendered *out);

                                                                            
typedef struct {
    PcvRestTlsMode mode;
    gboolean       tls_degraded;
    const gchar   *tls_degraded_reason;
} PcvRestHealthInput;
                                                                                   
void pcv_rest_render_health(const PcvRestHealthInput *in, PcvRestRendered *out);

typedef enum {
    PCV_REST_HEALTH_CACHED,                                                           
    PCV_REST_HEALTH_PROBE,                                              
    PCV_REST_HEALTH_WAIT,                                                  
} PcvRestHealthEntry;
                                                                                               
PcvRestHealthEntry pcv_rest_health_enter(gint64 now_us, gpointer waiter, gchar **body, guint *code);
                                                                               
GPtrArray *pcv_rest_health_complete(const gchar *body, guint code, gint64 now_us);

typedef virConnectPtr (*PcvRestHealthOpenFn)(const gchar *uri, gboolean read_only, guint timeout_sec, GError **error);
                                                              
void pcv_rest_render_set_open_fn_for_test(PcvRestHealthOpenFn fn);
                                                                                                                 
void pcv_rest_render_set_internal_fns_for_test(JsonArray *(*vms_fn)(void), gint (*vm_count_fn)(void), guint deadline_ms);
                               
void pcv_rest_render_metrics_reset_for_test(void);
                                    
void pcv_rest_health_reset_for_test(void);

#endif                               
