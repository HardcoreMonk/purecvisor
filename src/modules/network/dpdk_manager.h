
#ifndef PURECVISOR_DPDK_MANAGER_H
#define PURECVISOR_DPDK_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

void     pcv_dpdk_init(void);
void     pcv_dpdk_shutdown(void);
gboolean pcv_dpdk_is_available(void);

JsonObject *pcv_dpdk_status(void);
JsonObject *pcv_dpdk_hugepage_info(void);

gboolean    pcv_dpdk_bind(const gchar *pci_addr, const gchar *driver, GError **error);
gboolean    pcv_dpdk_unbind(const gchar *pci_addr, GError **error);
JsonArray  *pcv_dpdk_list(void);

gboolean pcv_dpdk_nic_is_protected(const gchar *pci_addr, gchar **reason);

gboolean pcv_dpdk_route_is_default_dev(const gchar *netdev, const gchar *proc_base);

gboolean pcv_dpdk_bridge_create(const gchar *name, const gchar *dpdk_port, GError **error);
gboolean pcv_dpdk_bridge_delete(const gchar *name, GError **error);

gchar *pcv_dpdk_vhost_socket_path(const gchar *vm_name);

G_END_DECLS

#endif
