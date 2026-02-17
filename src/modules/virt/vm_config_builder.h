/*
 * src/modules/virt/vm_config_builder.h
 *
 * Description:
 * Converts generic VM configuration parameters into Libvirt-compatible XML
 * using libvirt-gconfig. Strictly synchronous and purely functional.
 *
 * Author: PureCVisor Architect
 */

#ifndef PURECVISOR_VM_CONFIG_BUILDER_H
#define PURECVISOR_VM_CONFIG_BUILDER_H

#include <glib-object.h>
#include <libvirt-gconfig/libvirt-gconfig.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Type Declaration */
#define PURECVISOR_TYPE_VM_CONFIG_BUILDER (purecvisor_vm_config_builder_get_type())

G_DECLARE_FINAL_TYPE(PureCVisorVmConfigBuilder, purecvisor_vm_config_builder, PURECVISOR, VM_CONFIG_BUILDER, GObject)

/**
 * PureCVisorVmConfig:
 * A plain struct to hold parsed parameters before building XML.
 * This separates JSON parsing logic from XML building logic.
 */
typedef struct {
    gchar *name;
    guint64 memory_kb;
    guint vcpus;
    gchar *arch;        // e.g., "x86_64"
    gchar *os_type;     // e.g., "hvm"
    
    /* Storage Configuration */
    gchar *disk_path;   // Path to the ZVol (e.g., /dev/zvol/tank/vm-100)
    gchar *iso_path;    // Optional CD-ROM ISO path
    
    /* Network Configuration */
    gchar *bridge_iface; // e.g., "br0"
} PureCVisorVmConfig;

/* Constructor */
PureCVisorVmConfigBuilder *purecvisor_vm_config_builder_new(void);

/*
 * Method: purecvisor_vm_config_builder_set_config
 * Description: Validates and sets the configuration data.
 */
gboolean purecvisor_vm_config_builder_set_config(PureCVisorVmConfigBuilder *self,
                                                 PureCVisorVmConfig *config,
                                                 GError **error);

/*
 * Method: purecvisor_vm_config_builder_generate_xml
 * Description:
 * Constructs the GVirConfigDomain hierarchy and returns the XML string.
 * The caller owns the returned string.
 */
gchar *purecvisor_vm_config_builder_generate_xml(PureCVisorVmConfigBuilder *self,
                                                 GError **error);

/* Helper to free the plain config struct */
void purecvisor_vm_config_free(PureCVisorVmConfig *config);

G_END_DECLS

#endif /* PURECVISOR_VM_CONFIG_BUILDER_H */