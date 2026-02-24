/* src/modules/virt/vm_config_builder.c */

#include "vm_config_builder.h"
#include <libvirt-gobject/libvirt-gobject.h>
#include <glib.h>

struct _PureCVisorVmConfig {
    gchar *name;
    gint vcpu;
    gint memory_mb;
    gchar *disk_path;
    gchar *iso_path;
    gchar *network_bridge; // [Phase 5-2] Bridge Name (e.g., "br0", "virbr0")
};

PureCVisorVmConfig *purecvisor_vm_config_new(const gchar *name, gint vcpu, gint ram_mb) {
    PureCVisorVmConfig *config = g_new0(PureCVisorVmConfig, 1);
    config->name = g_strdup(name);
    config->vcpu = vcpu;
    config->memory_mb = ram_mb;
    config->network_bridge = NULL; // Default to NAT
    return config;
}

void purecvisor_vm_config_free(PureCVisorVmConfig *config) {
    if (!config) return;
    g_free(config->name);
    g_free(config->disk_path);
    g_free(config->iso_path);
    g_free(config->network_bridge);
    g_free(config);
}

void purecvisor_vm_config_set_disk(PureCVisorVmConfig *config, const gchar *path) {
    if (config->disk_path) g_free(config->disk_path);
    config->disk_path = g_strdup(path);
}

void purecvisor_vm_config_set_iso(PureCVisorVmConfig *config, const gchar *path) {
    if (config->iso_path) g_free(config->iso_path);
    config->iso_path = g_strdup(path);
}

// [Phase 5-2] Bridge 설정 Setter
void purecvisor_vm_config_set_network_bridge(PureCVisorVmConfig *config, const gchar *bridge_name) {
    if (config->network_bridge) g_free(config->network_bridge);
    config->network_bridge = g_strdup(bridge_name);
}

GVirConfigDomain *purecvisor_vm_config_build(PureCVisorVmConfig *config) {
    GVirConfigDomain *domain = gvir_config_domain_new();
    
    // 하드웨어 가속(KVM)을 지원하지 않는 환경(예: 단순 가상머신 위에서 개발 중)이라면 GVIR_CONFIG_DOMAIN_VIRT_KVM 대신 GVIR_CONFIG_DOMAIN_VIRT_QEMU를 사용
    // [Fix] 가상화 타입(KVM) 명시 (이 부분이 누락되어 Libvirt가 거부함)
    gvir_config_domain_set_virt_type(domain, GVIR_CONFIG_DOMAIN_VIRT_KVM);

    // 1. Basic Info
    gvir_config_domain_set_name(domain, config->name);
    gvir_config_domain_set_memory(domain, config->memory_mb * 1024);
    gvir_config_domain_set_vcpus(domain, config->vcpu);
    
    // 2. OS & Features
    GVirConfigDomainOs *os = gvir_config_domain_os_new();
    gvir_config_domain_os_set_os_type(os, GVIR_CONFIG_DOMAIN_OS_TYPE_HVM);

    // 🚀 [추가] 1996년산 i440fx 대신, 핫플러그를 네이티브 지원하는 최신 q35 마더보드로 강제 업그레이드!
    gvir_config_domain_os_set_machine(os, "q35");

    gvir_config_domain_set_os(domain, os);
    g_object_unref(os);

    // 3. Disk (ZVol)
    if (config->disk_path) {
        GVirConfigDomainDisk *disk = gvir_config_domain_disk_new();
        gvir_config_domain_disk_set_type(disk, GVIR_CONFIG_DOMAIN_DISK_BLOCK);
        gvir_config_domain_disk_set_source(disk, config->disk_path);
        
        GVirConfigDomainDiskDriver *driver = gvir_config_domain_disk_driver_new();
        gvir_config_domain_disk_driver_set_name(driver, "qemu");
        gvir_config_domain_disk_driver_set_format(driver, GVIR_CONFIG_DOMAIN_DISK_FORMAT_RAW);
        gvir_config_domain_disk_set_driver(disk, driver);
        g_object_unref(driver);
        
        gvir_config_domain_disk_set_target_bus(disk, GVIR_CONFIG_DOMAIN_DISK_BUS_VIRTIO);
        gvir_config_domain_disk_set_target_dev(disk, "vda");
        
        gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(disk));
        g_object_unref(disk);
    }

    // 4. ISO (CDROM)
    if (config->iso_path) {
        GVirConfigDomainDisk *cdrom = gvir_config_domain_disk_new();
        gvir_config_domain_disk_set_type(cdrom, GVIR_CONFIG_DOMAIN_DISK_FILE);
        gvir_config_domain_disk_set_guest_device_type(cdrom, GVIR_CONFIG_DOMAIN_DISK_GUEST_DEVICE_CDROM);
        gvir_config_domain_disk_set_source(cdrom, config->iso_path);

        // 🚀 [수정됨] q35 보드는 IDE(hda)를 버렸으므로, 네이티브 SATA(sda) 버스로 장착!
        gvir_config_domain_disk_set_target_bus(cdrom, GVIR_CONFIG_DOMAIN_DISK_BUS_SATA);
        gvir_config_domain_disk_set_target_dev(cdrom, "sda");

        gvir_config_domain_disk_set_readonly(cdrom, TRUE);
        
        gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(cdrom));
        g_object_unref(cdrom);
    }

    // 5. VNC Graphics
    GVirConfigDomainGraphicsVnc *graphics = gvir_config_domain_graphics_vnc_new();
    gvir_config_domain_graphics_vnc_set_autoport(graphics, TRUE);
    // 외부 접속을 위해 0.0.0.0 바인딩 (보안상 주의 필요하나 개발용으로 허용)
    // gvir_config_domain_graphics_set_listen(GVIR_CONFIG_DOMAIN_GRAPHICS(graphics), "0.0.0.0");
    
    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(graphics));
    g_object_unref(graphics);

    // 6. Network (Bridge Support)
    if (config->network_bridge) {
        // Bridge Mode (e.g., br0)
        GVirConfigDomainInterfaceBridge *iface = gvir_config_domain_interface_bridge_new();
        gvir_config_domain_interface_bridge_set_source(iface, config->network_bridge);
        gvir_config_domain_interface_set_model(GVIR_CONFIG_DOMAIN_INTERFACE(iface), "virtio");
        gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(iface));
        g_object_unref(iface);
    } else {
        
        // 🚀 High-Performance NAT (Libvirt 'default' network)
        GVirConfigDomainInterfaceNetwork *iface = gvir_config_domain_interface_network_new();
        gvir_config_domain_interface_network_set_source(iface, "default"); // virbr0와 연결되는 기본 NAT 망
        gvir_config_domain_interface_set_model(GVIR_CONFIG_DOMAIN_INTERFACE(iface), "virtio");
        gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(iface));
        g_object_unref(iface);
    }

    return domain;
}