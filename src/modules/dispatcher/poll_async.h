                                                                             
                                      
                                                                         
                                                                   
  
                           
                                                                                 
                                                                             
                                                                             
                                                                               
                                                                         
                                 
                                                                                   
                                                                 
                                                                             
                                                                                 
                                                
                                                                                        
                                           
                                                                         
                                                                                                   
                                                                                
                                                                   
                                                                            
                                                                                                
                                                                   
                                                        
  
                                                                 
                                                             
                                                      
                                                                                
#ifndef PURECVISOR_POLL_ASYNC_H
#define PURECVISOR_POLL_ASYNC_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "api/uds_server.h"
#include "utils/pcv_spawn.h"

G_BEGIN_DECLS

                                                                         
#define PCV_POLL_MAX_WAITERS 16

                                                                        
typedef struct PcvPollGuard PcvPollGuard;

                                                       
                                                                       
typedef JsonNode *(*PcvPollComputeFn)(JsonObject *params, PcvPollGuard *guard, GError **error);

   
                                                     
                                                                                
                                                                                  
                                                      
   
void pcv_poll_async_submit(const gchar *method, JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection, PcvPollComputeFn compute);

   
                                                             
                                                                                       
                                                                       
                                                                                         
                                                                   
   
gboolean pcv_poll_guard_spawn(PcvPollGuard *g, const gchar * const *argv, gchar **out, gchar **err,
                              GError **error);

   
                                                                               
                                                     
   
PcvPollGuard *pcv_poll_guard_new(const gchar *method);
                        
void pcv_poll_guard_free(PcvPollGuard *g);
   
                                                                                                       
                                                                                                     
                                                                      
                                                              
                                      
   
gboolean pcv_poll_guard_spawn_override(gpointer data, const gchar * const *argv, gchar **out, gchar **err, GError **error);
   
                                                                                                 
                                                                                            
                                                       
   
gboolean pcv_poll_guard_host_stalled(const PcvPollGuard *g);
   
                                                                                                   
                                                                 
                                                       
   
gboolean pcv_poll_guard_host_aborted(const PcvPollGuard *g);

                                                                                
void pcv_poll_async_set_spawn_fn_for_test(gboolean (*fn)(const gchar * const *argv, const PcvSpawnBound *b,
                                                         gchar **out, gchar **err, gint *status,
                                                         GError **error));
                                                                               
void pcv_poll_async_set_now_fn_for_test(gint64 (*fn)(void));

G_END_DECLS

#endif                              
