                                                                             
                                                    
                                                                                                         
  
                           
                                                                                      
                                                                               
                                                                         
                                                                                    
                                                                                     
                                                                                          
                                                                                    
                                                                                                   
                                                                                                           
                                                                                 
                                             
  
                                                                       
                                                      
                                                                                
#ifndef PURECVISOR_HANDLER_CONTAINER_HEALTH_H
#define PURECVISOR_HANDLER_CONTAINER_HEALTH_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "utils/pcv_validate.h"

G_BEGIN_DECLS

typedef struct _UdsServer UdsServer;

                                                                    
#define PCV_HEALTH_MAX_INFLIGHT 4

   
                      
                                                                           
                                                                     
   
typedef struct {
    gchar   name[PCV_MAX_VM_NAME + 1];                               
    gchar   type[8];                                      
    gchar   target[256];                               
    gint    timeout_sec;                       
    guint64 gen;                                            
} PcvHealthProbeSpec;

   
                
                                                                                 
                                                                    
   
typedef struct {
    gboolean (*probe)(const PcvHealthProbeSpec *spec, GError **error);                  
    gpointer (*lock)(const gchar *name);                                                   
    void     (*unlock)(gpointer lock);
    gboolean (*restart_allowed)(const gchar *name);
    gint     (*stop)(const gchar *name);                                                                      
    gboolean (*after_stop)(const gchar *name, gint stop_rc);
    gboolean (*start)(const gchar *name);                                                                                                      
    gboolean (*renew)(gpointer lock);                                               
    gboolean (*is_terminating)(void);                                                                        
    gboolean (*is_draining)(void);                                                                               
} PcvHealthOps;

                                                               
                                                                                                   
                                              
void     handle_container_health_set(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *conn);
                                                                 
                             
void     handle_container_health_get(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *conn);
                                                          
                                        
void     handle_container_health_delete(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *conn);

   
                                      
                                                      
                                            
   
gint     pcv_container_health_clamp_timeout(gint requested);

                                                                   
                                                         
                                      
void     pcv_container_health_set_ops_for_test(const PcvHealthOps *ops);
                                                                                     
                                                                                                           
                                     
guint    pcv_container_health_tick_for_test(gint64 now_us, gboolean run_inline, gboolean *source_continue);
                                                               
                                          
void     pcv_container_health_complete_for_test(const gchar *name, guint64 gen, gboolean healthy, gboolean cancelled);
                                               
gboolean pcv_container_health_timer_active_for_test(void);
                                                                            
guint64  pcv_container_health_gen_for_test(const gchar *name);
                                                                
void     pcv_container_health_reset_for_test(void);

G_END_DECLS

#endif                                            
