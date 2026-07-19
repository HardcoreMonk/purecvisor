
#ifndef PURECVISOR_SRIOV_MANAGER_H
#define PURECVISOR_SRIOV_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

void     pcv_sriov_init(void);
void     pcv_sriov_shutdown(void);

JsonObject *pcv_sriov_status(void);

gboolean pcv_sriov_enable(const gchar *pf, gint num_vfs, GError **error);
gboolean pcv_sriov_disable(const gchar *pf, GError **error);
JsonArray *pcv_sriov_list(const gchar *pf);

gboolean pcv_sriov_set(const gchar *pf, gint vf_index,
                        const gchar *mac, gint vlan,
                        gint spoofchk, GError **error);

gboolean pcv_sriov_attach_vm(const gchar *vm_name, const gchar *pf,
                              gint vf_index, GError **error);

gboolean pcv_sriov_detach_vm(const gchar *vm_name, const gchar *pci_addr, GError **error);

gchar *pcv_sriov_vf_pci_addr(const gchar *pf, gint vf_index);

G_END_DECLS

#endif
