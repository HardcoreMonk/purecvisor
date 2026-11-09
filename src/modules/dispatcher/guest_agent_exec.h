                                                                             
                                            
                                                                             
                                                                                
  
                           
                                                                           
                                                                             
                                                                            
                                                                       
                                                                    
                                                                  
                                                                     
                                                              
                                                
                                                                                   
                                                                            
                       
                                                                                                    
                                                                                                                                                                           
                                                                 
                                                                          
                                                                  
                                                                    
                                         
  
                                                                
                                                                     
                                                                                
#ifndef PURECVISOR_GUEST_AGENT_EXEC_H
#define PURECVISOR_GUEST_AGENT_EXEC_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "api/uds_server.h"

G_BEGIN_DECLS

                                                                                
                                                      
                                                             
typedef struct {
    gboolean running;                                          
    gboolean channel_configured;                                         
    gboolean channel_live;                                        
    gboolean agent_ping;                                     
    gchar   *agent_error;                          
} PcvGuestAgentStatus;

                                                           
                                                                         
                                        
                                                                               
                                                                             
typedef struct {
    gchar *(*agent_command)(const gchar *uuid, const gchar *name, const gchar *cmd,
                            gint timeout_s, GError **error);
    gint64 (*now_us)(void);
    gboolean (*status_probe)(const gchar *name, PcvGuestAgentStatus *out, GError **error);
} PcvGuestAgentOps;

                                                        
void pcv_guest_agent_set_ops_for_test(const PcvGuestAgentOps *ops);

                                                                                  
                                       
GPtrArray *pcv_guest_agent_cache_lookup(const gchar *uuid);

                                                                   
                                                                      
void pcv_guest_agent_probe_request(const gchar *uuid, const gchar *name);

                                                               
void pcv_guest_agent_invalidate(const gchar *uuid);

                                                                               
                                                                  
                                                                                 
                                                                      
void pcv_guest_agent_status_submit(const gchar *name, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection);

                                                                                              
gboolean pcv_guest_agent_xml_has_channel(const gchar *xml);

                                                                                               
void pcv_guest_agent_add_install_commands(JsonObject *obj);

                                                                       
                                                                                
void pcv_guest_agent_flush_for_test(void);

G_END_DECLS

#endif                                    
