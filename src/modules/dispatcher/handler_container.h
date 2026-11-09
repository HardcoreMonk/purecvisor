   
                            
                                   
  
                           
                                                   
                                                    
                                        
  
                                                          
                                                        
                                                        
                                               
  
                     
                                                                               
                                                                               
                                                                               
                                                                               
                                                                                                       
                                                                               
                                                                               
                                                                               
                                                                               
                                                                               
                                                                               
                                                                           
                                                                           
                                                                           
                                                                           
                                                                           
                                                                           
                                                                           
  
                              
                                                          
                                                                   
  
          
                                                                           
  
         
                                                             
                                                      
   

#ifndef PURECVISOR_HANDLER_CONTAINER_H
#define PURECVISOR_HANDLER_CONTAINER_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef struct _UdsServer UdsServer;

                                                                          
                                                                       
void handle_container_create  (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_destroy (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_start   (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_stop    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

                                                                               
typedef enum {
    PCV_CTR_START_SUBMITTED,                                                                                  
    PCV_CTR_START_DENIED,                                                              
    PCV_CTR_START_BUSY,                                                                  
} PcvCtrStartSubmit;

   
                              
                                                                                             
                                                                                                      
                                                  
                                                        
                                                                                      
                                                                        
                                                                    
                                                                        
   
PcvCtrStartSubmit pcv_container_start_submit(const gchar *name, const gchar *actor, gchar **msg_out);

   
                                       
                                                                                                          
                                                                                                                        
                                                                                                                                     
                                                                                          
                                                                                                         
                                                                    
   
void pcv_container_start_report_rejected(const gchar *name, PcvCtrStartSubmit why, const gchar *msg, const gchar *actor);

                                                                            
                                                   
void handle_container_list    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

void handle_container_metrics (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

                                                                              
                                       
void handle_container_exec    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

                                                                           
                                                                         
void handle_container_snapshot_create   (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

void handle_container_snapshot_rollback (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

void handle_container_snapshot_delete   (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

void handle_container_snapshot_list     (JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *conn);

                                                                            
                                                                                   
                                                                                     
void handle_container_logs    (JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *conn);

                                                                                                             
                                                                                               
gboolean pcv_container_config_ready(const gchar *name, gchar **out_config_path, GError **error);

                                                                        
                                                                                                             
                                                               
void handle_container_volume_attach(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

void handle_container_volume_detach(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

void handle_container_volume_list  (JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *conn);

                                                                          
                                                                    
void handle_container_env_set    (JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *conn);

void handle_container_env_list   (JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *conn);

void handle_container_env_delete (JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *conn);

                                                                                               

G_END_DECLS

#endif                                     
