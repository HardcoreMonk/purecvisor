   
                           
                                                                                                                 
  
                           
                                                                       
                                                                    
  
                                                                
   
#ifndef PCV_HANDLER_VM_ADMIN_H
#define PCV_HANDLER_VM_ADMIN_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

                                                                                 
                           
void handle_vm_delete_status(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_vm_resize_disk(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
void handle_vm_batch(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection);
void handle_vm_event_webhook_list(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection);
void handle_vm_autostart(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection);
void handle_vm_sla_report(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection);
void handle_vm_schedule_list(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection);
void handle_vm_schedule_set(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);
void handle_vm_billing_report(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection);
void handle_vm_numa_info(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection);

#endif                             
