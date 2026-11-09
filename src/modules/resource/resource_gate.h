                                                                             
                                       
                                                                                 
  
                           
                                                                                
                                                                                                     
                                                                                 
                                                                                                  
                                                                                    
                                                                                                    
                                                                                          
                                                                                                     
                            
                                                                              
                          
  
                                                                                 
                                                                                    
                                                                                
#ifndef PURECVISOR_RESOURCE_GATE_H
#define PURECVISOR_RESOURCE_GATE_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "api/uds_server.h"
#include "modules/resource/resource_registry.h"

G_BEGIN_DECLS

                                                                                                    
typedef struct {
    gint   role;
    gchar *subject;
    gchar *tenant;
} PcvResCaller;

   
                                                                
                                                                                               
                                                                 
                                                         
   
void pcv_res_caller_init(PcvResCaller *c, JsonObject *params, GSocketConnection *connection);
void pcv_res_caller_clear(PcvResCaller *c);

   
                                                                                                      
                                                                                             
                                                                                     
                                                        
                                                                                      
                                                                                                          
              
                                                                            
   
const gchar *pcv_res_owner_tenant(const gchar *owner);

                                                                                              
typedef enum { PCV_RES_ALLOW, PCV_RES_DENY_NO_TENANT, PCV_RES_DENY_NOT_ALLOWED } PcvResDecision;

   
                                                                                    
                                                                  
                                                          
   
PcvResDecision pcv_res_decide(gint role, gboolean has_tenant, gboolean normalized, gboolean allowed);

   
                                                                                                   
                                                                                      
                                                                  
   
PcvResDecision pcv_res_check(const PcvResCaller *c, PcvResKind kind, const gchar *value, gchar **effective_out,
                             PcvResPin *pin_out);

                                                                                                               
PcvResDecision pcv_res_check_tenant_name(const PcvResCaller *c, const gchar *requested);

                                                                                              
gchar *pcv_res_decision_message(PcvResDecision d, const gchar *param_name);

   
                                                                                               
                                                             
                                                                                              
                                                                                                            
                                                                                       
                                                                                           
                                                                     
                                                                            
                                                     
   
void pcv_res_audit_denied(const gchar *method, const gchar *param_name, const gchar *value);

   
                                                                                               
                                                                                       
                                                               
   
gboolean pcv_res_gate(JsonObject *params, GSocketConnection *connection, const gchar *method, const gchar *param_name,
                      PcvResKind kind, const gchar *value, gchar **effective_out, PcvResPin *pin_out,
                      const gchar *rpc_id, UdsServer *server);

                                                                                                
gboolean pcv_res_visible(const PcvResCaller *c, PcvResKind kind, const gchar *value);

   
                                                                                           
                                     
   
JsonArray *pcv_res_filter_array(const PcvResCaller *c, PcvResKind kind, JsonArray *array, const gchar *member);

   
                                                                                               
                                                                                                    
                                                                          
                               
                                                                               
   
JsonObject *pcv_res_template_view(const PcvResCaller *c, JsonObject *template_obj);

   
                                                                                                   
                                                                        
   
gboolean pcv_res_request_can_see_usb(JsonObject *params, GSocketConnection *connection, const gchar *vendor,
                                     const gchar *product);

G_END_DECLS

#endif                                 
