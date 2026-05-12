



























































#ifndef PURECVISOR_HANDLER_ACCEL_H
#define PURECVISOR_HANDLER_ACCEL_H

#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "api/uds_server.h"

G_BEGIN_DECLS









void handle_dpdk_status(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_dpdk_bind(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_dpdk_unbind(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_dpdk_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_dpdk_bridge_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_dpdk_bridge_delete(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_dpdk_hugepage_info(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);









void handle_sriov_status(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_sriov_enable(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_sriov_disable(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_sriov_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_sriov_set(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_sriov_attach(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

void handle_sriov_detach(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

G_END_DECLS

#endif
