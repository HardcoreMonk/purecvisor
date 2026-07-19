
#ifndef PURECVISOR_DISPATCHER_H
#define PURECVISOR_DISPATCHER_H

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include "uds_server.h"
#include "../modules/virt/vm_manager.h"

G_BEGIN_DECLS

#define PURECVISOR_TYPE_DISPATCHER (purecvisor_dispatcher_get_type())

G_DECLARE_FINAL_TYPE(PureCVisorDispatcher, purecvisor_dispatcher, PURECVISOR, DISPATCHER, GObject)

PureCVisorDispatcher *purecvisor_dispatcher_new(void);

void purecvisor_dispatcher_set_connection(PureCVisorDispatcher *self, GVirConnection *conn);

PureCVisorVmManager *purecvisor_dispatcher_get_vm_manager(PureCVisorDispatcher *self);

void purecvisor_dispatcher_dispatch(PureCVisorDispatcher *self,
                                   UdsServer *server,
                                   GSocketConnection *connection,
                                   const gchar *request_json);

void dispatcher_shutdown_routes(void);

typedef gboolean (*PcvDispatchHook)(const gchar *method, JsonObject *params,
                                     const gchar *rpc_id, gpointer user_data);

void pcv_dispatcher_register_pre_hook(PcvDispatchHook hook, gpointer user_data);

void pcv_dispatcher_handle_cluster_maintenance_enter(JsonObject *params, const gchar *rpc_id,
                                                     UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_maintenance_exit(JsonObject *params, const gchar *rpc_id,
                                                    UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_affinity_set(JsonObject *params, const gchar *rpc_id,
                                                UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_affinity_list(JsonObject *params, const gchar *rpc_id,
                                                 UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_affinity_delete(JsonObject *params, const gchar *rpc_id,
                                                   UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_node_label_set(JsonObject *params, const gchar *rpc_id,
                                                  UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_node_label_get(JsonObject *params, const gchar *rpc_id,
                                                  UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_node_label_delete(JsonObject *params, const gchar *rpc_id,
                                                     UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_node_drain(JsonObject *params, const gchar *rpc_id,
                                              UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_node_resume(JsonObject *params, const gchar *rpc_id,
                                               UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_upgrade_status(JsonObject *params, const gchar *rpc_id,
                                                  UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_config_push(JsonObject *params, const gchar *rpc_id,
                                               UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_cluster_config_get(JsonObject *params, const gchar *rpc_id,
                                              UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_federation_site_join(JsonObject *params, const gchar *rpc_id,
                                                UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_federation_site_list(JsonObject *params, const gchar *rpc_id,
                                                UdsServer *server, GSocketConnection *connection);
void pcv_dispatcher_handle_federation_site_remove(JsonObject *params, const gchar *rpc_id,
                                                  UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif
