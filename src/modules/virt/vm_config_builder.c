/*
 * src/modules/virt/vm_config_builder.c
 *
 * Description:
 * Implementation of the VM Configuration Builder using libvirt-gconfig.
 * Adjusted for libvirt-gconfig API (GStrv features compatibility).
 */

#include "vm_config_builder.h"
#include <libvirt-gconfig/libvirt-gconfig.h>
#include <string.h>

/* Private Structure */
struct _PureCVisorVmConfigBuilder {
    GObject parent_instance;
    PureCVisorVmConfig *config;
};

G_DEFINE_TYPE(PureCVisorVmConfigBuilder, purecvisor_vm_config_builder, G_TYPE_OBJECT)

static void
purecvisor_vm_config_builder_dispose(GObject *object)
{
    PureCVisorVmConfigBuilder *self = PURECVISOR_VM_CONFIG_BUILDER(object);

    if (self->config) {
        purecvisor_vm_config_free(self->config);
        self->config = NULL;
    }

    G_OBJECT_CLASS(purecvisor_vm_config_builder_parent_class)->dispose(object);
}

static void
purecvisor_vm_config_builder_class_init(PureCVisorVmConfigBuilderClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = purecvisor_vm_config_builder_dispose;
}

static void
purecvisor_vm_config_builder_init(PureCVisorVmConfigBuilder *self)
{
    self->config = NULL;
}

PureCVisorVmConfigBuilder *
purecvisor_vm_config_builder_new(void)
{
    return g_object_new(PURECVISOR_TYPE_VM_CONFIG_BUILDER, NULL);
}

void
purecvisor_vm_config_free(PureCVisorVmConfig *config)
{
    if (!config) return;
    g_free(config->name);
    g_free(config->arch);
    g_free(config->os_type);
    g_free(config->disk_path);
    g_free(config->iso_path);
    g_free(config->bridge_iface);
    g_free(config);
}

gboolean
purecvisor_vm_config_builder_set_config(PureCVisorVmConfigBuilder *self,
                                        PureCVisorVmConfig *config,
                                        GError **error)
{
    g_return_val_if_fail(PURECVISOR_IS_VM_CONFIG_BUILDER(self), FALSE);
    g_return_val_if_fail(config != NULL, FALSE);

    if (!config->name || strlen(config->name) == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "VM name is required");
        return FALSE;
    }

    if (self->config) {
        purecvisor_vm_config_free(self->config);
    }

    self->config = g_new0(PureCVisorVmConfig, 1);
    self->config->name = g_strdup(config->name);
    self->config->memory_kb = config->memory_kb;
    self->config->vcpus = config->vcpus;
    self->config->arch = config->arch ? g_strdup(config->arch) : g_strdup("x86_64");
    self->config->os_type = config->os_type ? g_strdup(config->os_type) : g_strdup("hvm");
    self->config->disk_path = g_strdup(config->disk_path);
    self->config->iso_path = g_strdup(config->iso_path);
    self->config->bridge_iface = config->bridge_iface ? g_strdup(config->bridge_iface) : g_strdup("virbr0");

    return TRUE;
}

static void
_build_os_section(GVirConfigDomain *domain, PureCVisorVmConfig *config)
{
    GVirConfigDomainOs *os = gvir_config_domain_os_new();
    
    gvir_config_domain_os_set_os_type(os, GVIR_CONFIG_DOMAIN_OS_TYPE_HVM);
    gvir_config_domain_os_set_arch(os, config->arch);
    
    gvir_config_domain_set_os(domain, os);
    g_object_unref(os);
}

/* * Helper: Build Features Section (ACPI, APIC)
 * FIX: Uses GStrv (string array) instead of Object, compatible with older APIs.
 */
static void
_set_features_list(GVirConfigDomain *domain)
{
    /* "acpi", "apic" and NULL terminator */
    const gchar *features[] = { "acpi", "apic", NULL };
    
    /* Casting to GStrv (char ** const) */
    gvir_config_domain_set_features(domain, (GStrv)features);
}

static void
_build_disk_device(GVirConfigDomain *domain, PureCVisorVmConfig *config)
{
    if (!config->disk_path) return;

    GVirConfigDomainDisk *disk = gvir_config_domain_disk_new();
    
    /* Use correct ENUMs available in 1.0 */
    gvir_config_domain_disk_set_type(disk, GVIR_CONFIG_DOMAIN_DISK_BLOCK);
    gvir_config_domain_disk_set_guest_device_type(disk, GVIR_CONFIG_DOMAIN_DISK_GUEST_DEVICE_DISK);
    
    gvir_config_domain_disk_set_source(disk, config->disk_path);
    
    GVirConfigDomainDiskDriver *driver = gvir_config_domain_disk_driver_new();
    gvir_config_domain_disk_driver_set_name(driver, "qemu");
    gvir_config_domain_disk_driver_set_format(driver, GVIR_CONFIG_DOMAIN_DISK_FORMAT_RAW);
    gvir_config_domain_disk_driver_set_cache(driver, GVIR_CONFIG_DOMAIN_DISK_CACHE_NONE);
    
    /* FIX: 'io' policy removed due to missing enum in installed lib. 
     * Defaults to 'threads' which is safe. 'native' can be added later if needed. */
    
    gvir_config_domain_disk_set_driver(disk, driver);
    
    gvir_config_domain_disk_set_target_bus(disk, GVIR_CONFIG_DOMAIN_DISK_BUS_VIRTIO);
    gvir_config_domain_disk_set_target_dev(disk, "vda");

    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(disk));
    
    g_object_unref(disk);
    g_object_unref(driver);
}

static void
_build_net_interface(GVirConfigDomain *domain, PureCVisorVmConfig *config)
{
    /* Instantiate concrete class: GVirConfigDomainInterfaceBridge */
    GVirConfigDomainInterfaceBridge *iface = gvir_config_domain_interface_bridge_new();
    
    /* Setup Bridge Source */
    GVirConfigDomainInterfaceBridge *bridge_source = GVIR_CONFIG_DOMAIN_INTERFACE_BRIDGE(iface);
    gvir_config_domain_interface_bridge_set_source(bridge_source, config->bridge_iface);
    
    /* Set Model to VirtIO */
    gvir_config_domain_interface_set_model(GVIR_CONFIG_DOMAIN_INTERFACE(iface), "virtio");

    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(iface));
    
    g_object_unref(iface);
}

gchar *
purecvisor_vm_config_builder_generate_xml(PureCVisorVmConfigBuilder *self,
                                          GError **error)
{
    g_return_val_if_fail(PURECVISOR_IS_VM_CONFIG_BUILDER(self), NULL);

    if (!self->config) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Configuration not set");
        return NULL;
    }

    GVirConfigDomain *domain = gvir_config_domain_new();
    
    gvir_config_domain_set_name(domain, self->config->name);
    gvir_config_domain_set_memory(domain, self->config->memory_kb);
    gvir_config_domain_set_vcpus(domain, self->config->vcpus);
    
    /* Lifecycle */
    gvir_config_domain_set_lifecycle(domain, 
        GVIR_CONFIG_DOMAIN_LIFECYCLE_ON_POWEROFF, GVIR_CONFIG_DOMAIN_LIFECYCLE_DESTROY);
    gvir_config_domain_set_lifecycle(domain, 
        GVIR_CONFIG_DOMAIN_LIFECYCLE_ON_REBOOT, GVIR_CONFIG_DOMAIN_LIFECYCLE_RESTART);
    gvir_config_domain_set_lifecycle(domain, 
        GVIR_CONFIG_DOMAIN_LIFECYCLE_ON_CRASH, GVIR_CONFIG_DOMAIN_LIFECYCLE_RESTART);

    _build_os_section(domain, self->config);
    _set_features_list(domain); /* Revised ACPI/APIC setup */

    _build_disk_device(domain, self->config);
    _build_net_interface(domain, self->config);
    
    /* VNC Graphics - Simplified */
    GVirConfigDomainGraphicsVnc *vnc = gvir_config_domain_graphics_vnc_new();
    gvir_config_domain_graphics_vnc_set_autoport(vnc, TRUE);
    gvir_config_domain_graphics_vnc_set_port(vnc, -1);
    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(vnc));
    g_object_unref(vnc);

    gchar *xml_str = gvir_config_object_to_xml(GVIR_CONFIG_OBJECT(domain));
    
    g_object_unref(domain);

    if (!xml_str) {
         g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to serialize XML");
         return NULL;
    }

    return xml_str;
}