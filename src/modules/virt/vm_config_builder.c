
#include "vm_config_builder.h"
#include <libvirt-gobject/libvirt-gobject.h>
#include <glib.h>

struct _PureCVisorVmConfig {
    gchar *name;
    gint vcpu;
    gint memory_mb;
    gchar *disk_path;
    const gchar *iso_path;
    const gchar *network_bridge;
    gint          vlan_id;
    gint boot_mode;
    gboolean tpm;
    gint cpu_mode;
    gboolean hugepages;
};

PureCVisorVmConfig *purecvisor_vm_config_new(const gchar *name, gint vcpu, gint ram_mb) {
    PureCVisorVmConfig *config = g_new0(PureCVisorVmConfig, 1);
    config->name = g_strdup(name);
    config->vcpu = vcpu;
    config->memory_mb = ram_mb;
    config->network_bridge = NULL;
    config->vlan_id        = 0;
    return config;
}

void purecvisor_vm_config_free(PureCVisorVmConfig *config) {
    if (!config) return;
    g_free(config->name);
    g_free(config->disk_path);
    g_free((gpointer)config->iso_path);
    g_free((gpointer)config->network_bridge);
    g_free(config);
}

void purecvisor_vm_config_set_disk(PureCVisorVmConfig *config, const gchar *path) {
    if (config->disk_path) g_free(config->disk_path);
    config->disk_path = g_strdup(path);
}

void purecvisor_vm_config_set_iso(PureCVisorVmConfig *config, const gchar *path) {
    if (config->iso_path) g_free((gpointer)config->iso_path);
    config->iso_path = g_strdup(path);
}

void purecvisor_vm_config_set_network_bridge(PureCVisorVmConfig *config, const gchar *bridge_name) {
    if (config->network_bridge) g_free((gpointer)config->network_bridge);
    config->network_bridge = g_strdup(bridge_name);
}

void purecvisor_vm_config_set_vlan_id(PureCVisorVmConfig *config, gint vlan_id) {
    config->vlan_id = (vlan_id >= 1 && vlan_id <= 4094) ? vlan_id : 0;
}

void purecvisor_vm_config_set_boot_mode(PureCVisorVmConfig *config, gint mode) {
    config->boot_mode = mode;
}

void purecvisor_vm_config_set_tpm(PureCVisorVmConfig *config, gboolean enabled) {
    config->tpm = enabled;
}

void purecvisor_vm_config_set_cpu_mode(PureCVisorVmConfig *config, gint mode) {
    config->cpu_mode = mode;
}

void purecvisor_vm_config_set_hugepages(PureCVisorVmConfig *config, gboolean enabled) {
    config->hugepages = enabled;
}

GVirConfigDomain *purecvisor_vm_config_build(PureCVisorVmConfig *config) {
    if (!config) return NULL;

    GVirConfigDomain *domain = gvir_config_domain_new();

    gvir_config_domain_set_virt_type(domain, GVIR_CONFIG_DOMAIN_VIRT_KVM);

    gvir_config_domain_set_name(domain, config->name);
    gvir_config_domain_set_memory(domain, config->memory_mb * 1024);
    gvir_config_domain_set_vcpus(domain, config->vcpu);

    GVirConfigDomainOs *os = gvir_config_domain_os_new();
    gvir_config_domain_os_set_os_type(os, GVIR_CONFIG_DOMAIN_OS_TYPE_HVM);

    gvir_config_domain_os_set_machine(os, "q35");

    GList *boot_devs = NULL;
    boot_devs = g_list_append(boot_devs, GINT_TO_POINTER(GVIR_CONFIG_DOMAIN_OS_BOOT_DEVICE_CDROM));
    boot_devs = g_list_append(boot_devs, GINT_TO_POINTER(GVIR_CONFIG_DOMAIN_OS_BOOT_DEVICE_HD));

    gvir_config_domain_os_set_boot_devices(os, boot_devs);
    g_list_free(boot_devs);

    gvir_config_domain_set_os(domain, os);
    g_object_unref(os);

    {
        gchar *features[] = { (gchar *)"acpi", (gchar *)"apic", NULL };
        gvir_config_domain_set_features(domain, features);
    }

    {
        GVirConfigDomainCpu *cpu = gvir_config_domain_cpu_new();
        gint cpu_mode = config->cpu_mode == 2 ? 2 : 1;

        gvir_config_domain_cpu_set_mode(
            cpu,
            cpu_mode == 2
                ? GVIR_CONFIG_DOMAIN_CPU_MODE_HOST_MODEL
                : GVIR_CONFIG_DOMAIN_CPU_MODE_HOST_PASSTHROUGH);
        gvir_config_domain_set_cpu(domain, cpu);
        g_object_unref(cpu);
    }

    if (config->disk_path) {
        GVirConfigDomainDisk *disk = gvir_config_domain_disk_new();
        gboolean is_block = g_str_has_prefix(config->disk_path, "/dev/");

        if (is_block) {
            gvir_config_domain_disk_set_type(disk, GVIR_CONFIG_DOMAIN_DISK_BLOCK);
        } else {
            gvir_config_domain_disk_set_type(disk, GVIR_CONFIG_DOMAIN_DISK_FILE);
        }
        gvir_config_domain_disk_set_source(disk, config->disk_path);

        GVirConfigDomainDiskDriver *driver = gvir_config_domain_disk_driver_new();
        gvir_config_domain_disk_driver_set_name(driver, "qemu");

        GVirConfigDomainDiskFormat disk_fmt;
        if (is_block) {
            disk_fmt = GVIR_CONFIG_DOMAIN_DISK_FORMAT_RAW;
        } else if (g_str_has_suffix(config->disk_path, ".img")) {
            disk_fmt = GVIR_CONFIG_DOMAIN_DISK_FORMAT_RAW;
        } else {
            disk_fmt = GVIR_CONFIG_DOMAIN_DISK_FORMAT_QCOW2;
        }
        gvir_config_domain_disk_driver_set_format(driver, disk_fmt);
        gvir_config_domain_disk_set_driver(disk, driver);
        g_object_unref(driver);

        gvir_config_domain_disk_set_target_bus(disk, GVIR_CONFIG_DOMAIN_DISK_BUS_VIRTIO);
        gvir_config_domain_disk_set_target_dev(disk, "vda");

        gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(disk));
        g_object_unref(disk);
    }

    if (config->iso_path != NULL && strlen(config->iso_path) > 0) {
        GVirConfigDomainDisk *cdrom = gvir_config_domain_disk_new();

        gvir_config_domain_disk_set_type(cdrom, GVIR_CONFIG_DOMAIN_DISK_FILE);
        gvir_config_domain_disk_set_guest_device_type(cdrom, GVIR_CONFIG_DOMAIN_DISK_GUEST_DEVICE_CDROM);

        gvir_config_domain_disk_set_source(cdrom, config->iso_path);
        gvir_config_domain_disk_set_target_bus(cdrom, GVIR_CONFIG_DOMAIN_DISK_BUS_SATA);
        gvir_config_domain_disk_set_target_dev(cdrom, "sda");
        gvir_config_domain_disk_set_readonly(cdrom, TRUE);

        gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(cdrom));
        g_object_unref(cdrom);
    }

    GVirConfigDomainGraphicsVnc *vnc = gvir_config_domain_graphics_vnc_new();
    gvir_config_domain_graphics_vnc_set_autoport(vnc, TRUE);
    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(vnc));
    g_object_unref(vnc);

    GVirConfigDomainVideo *video = gvir_config_domain_video_new();
    gvir_config_domain_video_set_model(video, GVIR_CONFIG_DOMAIN_VIDEO_MODEL_VIRTIO);

    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(video));
    g_object_unref(video);

    GVirConfigDomainChannel *ga_channel = gvir_config_domain_channel_new();
    gvir_config_domain_channel_set_target_type(ga_channel,
        GVIR_CONFIG_DOMAIN_CHANNEL_TARGET_VIRTIO);
    gvir_config_domain_channel_set_target_name(ga_channel,
        "org.qemu.guest_agent.0");
    GVirConfigDomainChardevSourceUnix *ga_source =
        gvir_config_domain_chardev_source_unix_new();
    gvir_config_domain_chardev_set_source(GVIR_CONFIG_DOMAIN_CHARDEV(ga_channel),
        GVIR_CONFIG_DOMAIN_CHARDEV_SOURCE(ga_source));
    g_object_unref(ga_source);
    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(ga_channel));
    g_object_unref(ga_channel);

    return domain;
}
