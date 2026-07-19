
#ifndef PURECVISOR_NETWORK_FIREWALL_H
#define PURECVISOR_NETWORK_FIREWALL_H

#include <glib.h>

G_BEGIN_DECLS

gboolean network_firewall_setup_nat     (const gchar *bridge_name, const gchar *cidr, GError **error);

gboolean network_firewall_setup_isolated(const gchar *bridge_name, const gchar *cidr, GError **error);

gboolean network_firewall_setup_routed  (const gchar *bridge_name, const gchar *cidr, GError **error);

gboolean network_firewall_teardown      (const gchar *bridge_name, GError **error);

G_END_DECLS

#endif
