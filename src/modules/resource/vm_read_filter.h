                                                                             
                                        
                                                                                                 
  
                           
                                                                                                            
                                                                                                
                                                                         
                                                                                              
                                                                   
                                                                                
  
                       
                                                                                   
                  
                                                                                
#ifndef PURECVISOR_VM_READ_FILTER_H
#define PURECVISOR_VM_READ_FILTER_H

#include <gio/gio.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include "modules/auth/pcv_rbac.h"
#include "modules/resource/vm_read_scope.h"

G_BEGIN_DECLS

   
                                                                                 
                                                                                          
                                                                                                       
                               
                                                                           
   
gboolean pcv_vm_read_xml_visible(PcvVmReadScope *s, const gchar *domain_xml, gchar **owner_out);

   
                                                                                              
                                                                                 
                                                   
   
typedef struct {
    PcvVmReadScope *scope;                    
    PcvVmOwnerMap  *map;                                            
} PcvVmReadView;

   
                                                                                                                     
                                                                                               
                                                                           
                                             
   
void pcv_vm_read_view_init(PcvVmReadView *v, JsonObject *params, GSocketConnection *connection);
                                                                 
void pcv_vm_read_view_clear(PcvVmReadView *v);

   
                                                                                                              
                                                                                                        
                                                                        
                                                         
   
void pcv_vm_read_filter_processes(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *rows, gint top_n);

   
                                                                                                         
                                                                                                 
                                                    
   
void pcv_vm_read_mask_numa(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonObject *numa);

   
                                                                                                 
                                                                                                       
                                                           
   
JsonArray *pcv_vm_read_filter_by_member(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *take, const gchar *member,
                                        gboolean keep_star);

   
                                                                                                   
                                          
                                                             
   
void pcv_vm_read_filter_sg_vms(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *groups);

                                                                                              
#define PCV_VM_READ_BATCH      200
#define PCV_VM_READ_SOURCE_CAP 5000

   
                                                                                                            
                                                                         
                                                                                              
                                           
   
JsonArray *pcv_vm_read_filter_healing_history(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *take);

   
                                                                                                          
                                                           
                                                               
   
gboolean pcv_vm_read_security_row_visible(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonObject *row);

                                                                                                                   
JsonArray *pcv_vm_read_filter_security_rows(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *take);

                                                                                        
typedef JsonArray *(*PcvVmReadRowSource)(gint offset, gint limit, gpointer ud);

   
                                                                                                               
                                                                                                            
                                                                                         
                                                                  
   
JsonArray *pcv_vm_read_collect_security_rows(PcvVmReadScope *s, const PcvVmOwnerMap *m, PcvVmReadRowSource src, gpointer ud,
                                             gint offset, gint limit);

                                                                                         
typedef gboolean (*PcvVmReadEventTargetFn)(const gchar *event_id, gchar **kind_out, gchar **target_out, gpointer ud);

   
                                                                                                              
                                                                                                    
                                                                                                 
                                                                     
                                                           
   
void pcv_vm_read_mask_security_alerts(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonArray *alerts, PcvVmReadEventTargetFn lookup,
                                      gpointer ud);

   
                                                                                                                   
                                                                                     
                                                            
   
JsonNode *pcv_vm_read_filter_agent_comparison(PcvVmReadScope *s, const PcvVmOwnerMap *m, JsonNode *take);

                                                              
typedef enum {
    PCV_WS_SCOPE_ALL,                                
    PCV_WS_SCOPE_ADMIN,                                 
    PCV_WS_SCOPE_FILTER,                                                             
    PCV_WS_SCOPE_JOB,                                     
} PcvWsScopeKind;

   
                                                                               
                                                                                   
   
PcvWsScopeKind pcv_ws_scope_kind(const gchar *type);

   
                                                                                                
                                                                                         
                                                        
   
gboolean pcv_ws_scope_should_send(PcvWsScopeKind kind, gint conn_role, const gchar *conn_subject, const gchar *conn_tenant,
                                  const gchar *actor, const gchar *actor_tenant);

                                                                                         
typedef struct _PcvWsSubjectCache PcvWsSubjectCache;
                                                                     
PcvWsSubjectCache *pcv_ws_subject_cache_new(void);
   
                                                                                                           
                                                                                                         
   
gboolean pcv_ws_subject_cache_lookup(PcvWsSubjectCache *c, const gchar *subject, gint *role_out, const gchar **tenant_out);
                                              
void pcv_ws_subject_cache_free(PcvWsSubjectCache *c);

   
                                                                                                           
                                                                                           
                                                                        
   
static inline gboolean
pcv_vm_read_vnc_should_close(gint role, gboolean same_vm)
{
    return same_vm && role < PCV_ROLE_ADMIN;
}

G_END_DECLS

#endif                                  
