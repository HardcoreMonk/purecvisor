   
                       
                                                                                   
  
                           
                                                                       
                                                                    
  
                                                         
   
#ifndef PCV_HANDLER_JOBS_H
#define PCV_HANDLER_JOBS_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                             
                           
void handle_jobs_list(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection);
void handle_jobs_get(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection);
void handle_jobs_cancel(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection);
void handle_prometheus_sd(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection);
void handle_webhook_dlq_list(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_webhook_dlq_retry(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection);

#endif                         
