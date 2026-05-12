




































#ifndef PURECVISOR_VM_CONFIG_BUILDER_H
#define PURECVISOR_VM_CONFIG_BUILDER_H

#include <glib.h>
#include <libvirt-gobject/libvirt-gobject.h>






typedef struct _PureCVisorVmConfig PureCVisorVmConfig;












PureCVisorVmConfig *purecvisor_vm_config_new(const gchar *name, gint vcpu, gint ram_mb);







void purecvisor_vm_config_free(PureCVisorVmConfig *config);











void purecvisor_vm_config_set_disk(PureCVisorVmConfig *config, const gchar *path);









void purecvisor_vm_config_set_iso(PureCVisorVmConfig *config, const gchar *path);










void purecvisor_vm_config_set_network_bridge(PureCVisorVmConfig *config, const gchar *bridge_name);









void purecvisor_vm_config_set_vlan_id(PureCVisorVmConfig *config, gint vlan_id);













GVirConfigDomain *purecvisor_vm_config_build(PureCVisorVmConfig *config);








void purecvisor_vm_config_set_boot_mode(PureCVisorVmConfig *config, gint mode);






void purecvisor_vm_config_set_tpm(PureCVisorVmConfig *config, gboolean enabled);






void purecvisor_vm_config_set_cpu_mode(PureCVisorVmConfig *config, gint mode);






void purecvisor_vm_config_set_hugepages(PureCVisorVmConfig *config, gboolean enabled);

#endif
