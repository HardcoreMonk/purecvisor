#include "vm_config_builder.h"
#include <libvirt-gconfig/libvirt-gconfig.h>
#include <gio/gio.h>

/* ... _build_os_section, _build_cpu_mem 함수는 기존과 동일 ... */
static void _build_os_section(GVirConfigDomain *domain, PureCVisorVmConfig *config) {
    GVirConfigDomainOs *os = gvir_config_domain_os_new();
    gvir_config_domain_os_set_os_type(os, GVIR_CONFIG_DOMAIN_OS_TYPE_HVM);
    gvir_config_domain_os_set_arch(os, "x86_64");
    
    GList *boot_devs = NULL;
    boot_devs = g_list_append(boot_devs, (gpointer)GVIR_CONFIG_DOMAIN_OS_BOOT_DEVICE_CDROM);
    boot_devs = g_list_append(boot_devs, (gpointer)GVIR_CONFIG_DOMAIN_OS_BOOT_DEVICE_HD);
    gvir_config_domain_os_set_boot_devices(os, boot_devs);
    g_list_free(boot_devs);

    gvir_config_domain_set_os(domain, os);
    g_object_unref(os);
}

static void _build_cpu_mem(GVirConfigDomain *domain, PureCVisorVmConfig *config) {
    gvir_config_domain_set_memory(domain, config->memory_mb * 1024);
    gvir_config_domain_set_vcpus(domain, config->vcpu);
    
    GVirConfigDomainCpu *cpu = gvir_config_domain_cpu_new();
    gvir_config_domain_cpu_set_mode(cpu, GVIR_CONFIG_DOMAIN_CPU_MODE_HOST_MODEL);
    gvir_config_domain_set_cpu(domain, cpu);
    g_object_unref(cpu);
}

/* [Helper] Disk Driver 설정 */
static void _set_disk_driver(GVirConfigDomainDisk *disk, const gchar *name, GVirConfigDomainDiskFormat format) {
    GVirConfigDomainDiskDriver *driver = gvir_config_domain_disk_driver_new();
    gvir_config_domain_disk_driver_set_name(driver, name);
    gvir_config_domain_disk_driver_set_format(driver, format);
    gvir_config_domain_disk_set_driver(disk, driver);
    g_object_unref(driver);
}

static void _build_disk_device(GVirConfigDomain *domain, PureCVisorVmConfig *config) {
    /* 1. Main Disk (ZFS Volume) -> BLOCK DEVICE */
    gchar *zvol_path = g_strdup_printf("/dev/zvol/tank/%s", config->name);
    
    GVirConfigDomainDisk *disk = gvir_config_domain_disk_new();
    
    // [CRITICAL FIX] ZVol은 블록 장치이므로 GVIR_CONFIG_DOMAIN_DISK_BLOCK 사용
    gvir_config_domain_disk_set_type(disk, GVIR_CONFIG_DOMAIN_DISK_BLOCK);
    
    gvir_config_domain_disk_set_guest_device_type(disk, GVIR_CONFIG_DOMAIN_DISK_GUEST_DEVICE_DISK);
    
    // Driver: qemu, Format: raw
    _set_disk_driver(disk, "qemu", GVIR_CONFIG_DOMAIN_DISK_FORMAT_RAW);
    
    gvir_config_domain_disk_set_source(disk, zvol_path);
    gvir_config_domain_disk_set_target_bus(disk, GVIR_CONFIG_DOMAIN_DISK_BUS_VIRTIO);
    gvir_config_domain_disk_set_target_dev(disk, "vda");
    
    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(disk));
    g_free(zvol_path);
    g_object_unref(disk);

    /* 2. CD-ROM (ISO) -> FILE DEVICE */
    if (config->iso_path && g_strcmp0(config->iso_path, "") != 0) {
        GVirConfigDomainDisk *cdrom = gvir_config_domain_disk_new();
        
        // [NOTE] ISO 파일은 일반 파일이므로 GVIR_CONFIG_DOMAIN_DISK_FILE 유지
        gvir_config_domain_disk_set_type(cdrom, GVIR_CONFIG_DOMAIN_DISK_FILE);
        
        gvir_config_domain_disk_set_guest_device_type(cdrom, GVIR_CONFIG_DOMAIN_DISK_GUEST_DEVICE_CDROM);
        
        _set_disk_driver(cdrom, "qemu", GVIR_CONFIG_DOMAIN_DISK_FORMAT_RAW);
        
        gvir_config_domain_disk_set_source(cdrom, config->iso_path);
        gvir_config_domain_disk_set_target_bus(cdrom, GVIR_CONFIG_DOMAIN_DISK_BUS_SATA);
        gvir_config_domain_disk_set_target_dev(cdrom, "hda");
        gvir_config_domain_disk_set_readonly(cdrom, TRUE);

        gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(cdrom));
        g_object_unref(cdrom);
    }
}

static void _build_net_interface(GVirConfigDomain *domain, PureCVisorVmConfig *config) {
    GVirConfigDomainInterfaceNetwork *net = gvir_config_domain_interface_network_new();
    gvir_config_domain_interface_network_set_source(net, "default");
    gvir_config_domain_interface_set_model(GVIR_CONFIG_DOMAIN_INTERFACE(net), "virtio");
    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(net));
    g_object_unref(net);
}

static void _build_graphics(GVirConfigDomain *domain, PureCVisorVmConfig *config) {
    GVirConfigDomainGraphicsVnc *vnc = gvir_config_domain_graphics_vnc_new();
    gvir_config_domain_graphics_vnc_set_autoport(vnc, TRUE);
    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(vnc));
    g_object_unref(vnc);
    
    GVirConfigDomainVideo *video = gvir_config_domain_video_new();
    gvir_config_domain_video_set_model(video, GVIR_CONFIG_DOMAIN_VIDEO_MODEL_QXL);
    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(video));
    g_object_unref(video);
}

GVirConfigDomain *purecvisor_vm_config_builder_create_config(PureCVisorVmConfig *config, GError **error) {
    if (!config || !config->name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid VM Configuration");
        return NULL;
    }

    GVirConfigDomain *domain = gvir_config_domain_new();
    gvir_config_domain_set_name(domain, config->name);
    gvir_config_domain_set_virt_type(domain, GVIR_CONFIG_DOMAIN_VIRT_KVM);
    
    _build_os_section(domain, config);
    _build_cpu_mem(domain, config);
    _build_disk_device(domain, config);
    _build_net_interface(domain, config);
    _build_graphics(domain, config);

    return domain;
}