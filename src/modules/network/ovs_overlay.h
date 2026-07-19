
#ifndef PURECVISOR_OVS_OVERLAY_H
#define PURECVISOR_OVS_OVERLAY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

void pcv_overlay_init(const gchar *local_tunnel_ip);

void pcv_overlay_shutdown(void);

void pcv_overlay_restore(void);

void pcv_overlay_reconcile(void);
void pcv_overlay_reconcile_timer_init(void);
void pcv_overlay_reconcile_timer_shutdown(void);

gboolean    pcv_overlay_create(const gchar *name, gint vni, const gchar *cidr, GError **error);

gboolean    pcv_overlay_delete(const gchar *name, GError **error);

JsonArray  *pcv_overlay_list(void);

JsonObject *pcv_overlay_info(const gchar *name);

gboolean pcv_overlay_add_peer(const gchar *name, const gchar *peer_tunnel_ip, GError **error);

gboolean pcv_overlay_remove_peer(const gchar *name, const gchar *peer_tunnel_ip, GError **error);

gboolean pcv_overlay_auto_mesh(const gchar *name, const gchar *peers_csv, GError **error);

G_END_DECLS

#endif
