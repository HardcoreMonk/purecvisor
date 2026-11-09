   
                                      
                                                                                   
  
                                                                                                   
                                                                               
                                                  
   
#ifndef PURECVISOR_HANDLER_REPLICATION_HOSTKEY_H
#define PURECVISOR_HANDLER_REPLICATION_HOSTKEY_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

                                                              
typedef struct _UdsServer UdsServer;

                                                                                                                              
                                                                                         
void handle_backup_replication_hostkey_scan(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection);

                                                                                      
                                                                                                                
void handle_backup_replication_hostkey_trust(JsonObject *params, const gchar *rpc_id,
                                             UdsServer *server, GSocketConnection *connection);

                                                                                 
void handle_backup_replication_hostkey_list(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection);

                                                                                                        
void handle_backup_replication_hostkey_remove(JsonObject *params, const gchar *rpc_id,
                                              UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif                                               
