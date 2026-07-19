
#ifndef PURECVISOR_NETWORK_MANAGER_H
#define PURECVISOR_NETWORK_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

void handle_network_create_request  (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_delete_request  (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_list_request    (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_info_request    (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_mode_set_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

gboolean network_bridge_create(const gchar *bridge_name, const gchar *cidr, gint mtu, GError **error);

void pcv_network_meta_save(const gchar *bridge_name, const gchar *mode, const gchar *cidr);

gboolean network_bridge_delete(const gchar *bridge_name, GError **error);

void handle_network_bind_phys_request  (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_dhcp_toggle_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_ovs_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_ovs_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_ovs_vxlan_add_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_ovs_vxlan_del_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_qos_set(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_qos_get(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_qos_remove(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void pcv_qos_restore(void);

void pcv_qos_reconcile(void);
void pcv_qos_reconcile_timer_init(void);
void pcv_qos_reconcile_timer_shutdown(void);

gboolean pcv_bridge_vlan_add(const gchar *bridge, const gchar *iface, gint vlan_id);
gboolean pcv_bridge_vlan_remove(const gchar *bridge, const gchar *iface, gint vlan_id);

G_END_DECLS

#endif
