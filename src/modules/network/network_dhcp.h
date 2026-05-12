









































#ifndef PURECVISOR_NETWORK_DHCP_H
#define PURECVISOR_NETWORK_DHCP_H

#include <glib.h>

G_BEGIN_DECLS












gboolean network_dhcp_start(const gchar *bridge_name,
                             const gchar *cidr,
                             GError     **error);





















gboolean network_dhcp_start_ex(const gchar *bridge_name,
                                const gchar *cidr,
                                gboolean     dns_enabled,
                                const gchar *upstream_dns,
                                GError     **error);

















gboolean network_dhcp_start_v6(const gchar *bridge_name,
                                const gchar *ipv6_prefix,
                                GError     **error);

G_END_DECLS

#endif
