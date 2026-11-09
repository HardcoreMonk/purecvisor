                                                                             
                                       
                                                                                      
  
                           
                                                                                     
                                                                                        
                                                                                    
                                                                                        
                                                                                     
                                                                               
                                                                                                 
  
                                                                                 
                                                     
                                                                                
#ifndef PURECVISOR_RPC_OFFLOAD_H
#define PURECVISOR_RPC_OFFLOAD_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "api/uds_server.h"
#include "modules/dispatcher/rpc_completion.h"

G_BEGIN_DECLS

                                                                                                                           
typedef enum {
    PCV_OFFLOAD_LANE_LIBVIRT_READ,
    PCV_OFFLOAD_LANE_HOST_READ,
    PCV_OFFLOAD_LANE_NET_MUTATE,
    PCV_OFFLOAD_LANE_STORAGE_MUTATE,
    PCV_OFFLOAD_LANE_CTR_MUTATE,
    PCV_OFFLOAD_LANE_COUNT
} PcvOffloadLane;

                                                                          
#define PCV_OFFLOAD_MUTATE_MAX_WAIT_SEC 8

                                                  
typedef void (*PcvRpcOffloadHandler)(JsonObject *params, const gchar *rpc_id, UdsServer *server,
                                     GSocketConnection *connection);
                                                                         
typedef gboolean (*PcvRpcOffloadOwnerCheck)(const gchar *method, JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection, const gchar *actor);
                                                                           
typedef void (*PcvRpcOffloadFinish)(const gchar *method, const gchar *rpc_id, const gchar *actor, const gchar *target,
                                    gint64 start_us, PcvRpcCompletionResult completion);

                                                                           
typedef struct {
    const gchar             *method;
    const gchar             *rpc_id;                     
    const gchar             *req_id;                               
    const gchar             *actor;
    const gchar             *target;
    gint64                   start_us;
    PcvRpcOffloadHandler     handler;
    PcvRpcOffloadOwnerCheck  owner_check;                     
    PcvRpcOffloadFinish      finish;
    JsonParser              *parser;
    JsonObject              *params;                     
    UdsServer               *server;
    GSocketConnection       *connection;
} PcvRpcOffloadRequest;

                                                                   
gint pcv_rpc_offload_lane_of(const gchar *method);

   
                                                              
                                                                           
                                       
   
gboolean pcv_rpc_offload_capture(const gchar *response);

   
                                                                                  
                                                                                  
                                           
   
gboolean pcv_rpc_offload_submit(const PcvRpcOffloadRequest *req);

                                                                                                 
void pcv_rpc_offload_shutdown(void);

                                                               
void pcv_rpc_offload_add_for_test(const gchar *method, gint lane);
                                                           
void pcv_rpc_offload_set_now_fn_for_test(gint64 (*fn)(void));
                                                          
void pcv_rpc_offload_set_after_job_fn_for_test(void (*fn)(void));
                                                       
void pcv_rpc_offload_set_terminating_fn_for_test(gboolean (*fn)(void));

G_END_DECLS

#endif                               
