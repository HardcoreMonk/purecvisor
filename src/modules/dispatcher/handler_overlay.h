






















































#ifndef PURECVISOR_HANDLER_OVERLAY_H
#define PURECVISOR_HANDLER_OVERLAY_H

#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "api/uds_server.h"

G_BEGIN_DECLS








void handle_overlay_create(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);


void handle_overlay_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);


void handle_overlay_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);


void handle_overlay_info(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);


void handle_overlay_add_peer(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);


void handle_overlay_remove_peer(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);








void handle_iscsi_target_create(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);


void handle_iscsi_target_delete(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);


void handle_iscsi_target_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);


void handle_iscsi_connect(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);


void handle_iscsi_disconnect(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);









void handle_ovn_switch_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_switch_delete(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_switch_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_switch_detail(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_port_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_port_remove(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_acl_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_acl_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_router_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_router_delete(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_router_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_router_detail(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_router_add_port(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_dhcp_enable(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_nat_add(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_nat_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_tenant_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_ovn_status(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

G_END_DECLS

#endif
