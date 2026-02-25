/* src/modules/virt/vm_config_builder.c */

#include "vm_config_builder.h"
#include <libvirt-gobject/libvirt-gobject.h>
#include <glib.h>

struct _PureCVisorVmConfig {
    gchar *name;
    gint vcpu;
    gint memory_mb;
    gchar *disk_path;
    const gchar *iso_path;
    const gchar *network_bridge; // [Phase 5-2] Bridge Name (e.g., "br0", "virbr0")
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

// [Phase 5-2] Bridge 설정 Setter
void purecvisor_vm_config_set_network_bridge(PureCVisorVmConfig *config, const gchar *bridge_name) {
    if (config->network_bridge) g_free((gpointer)config->network_bridge);
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

    // 🚀 [여기 추가!] SeaBIOS에게 부팅 순서를 확실하게 주입합니다.
    // 🚀 링커 에러를 피하기 위해 GList로 부팅 순서를 묶어서 한방에 전달합니다!
    GList *boot_devs = NULL;
    boot_devs = g_list_append(boot_devs, GINT_TO_POINTER(GVIR_CONFIG_DOMAIN_OS_BOOT_DEVICE_CDROM)); // 1순위: CD-ROM
    boot_devs = g_list_append(boot_devs, GINT_TO_POINTER(GVIR_CONFIG_DOMAIN_OS_BOOT_DEVICE_HD)); // 2순위: 하드디스크
    
    gvir_config_domain_os_set_boot_devices(os, boot_devs);
    g_list_free(boot_devs); // 메모리 누수 방지 청소!

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

    // ==========================================
    // 4. 💿 가상 CD-ROM 드라이브 및 ISO 마운트
    // ==========================================
    
    if (config->iso_path != NULL && strlen(config->iso_path) > 0) {
        GVirConfigDomainDisk *cdrom = gvir_config_domain_disk_new();
        
        // 🚀 컴파일러가 원하던 정확한 상수로 주입!
        gvir_config_domain_disk_set_type(cdrom, GVIR_CONFIG_DOMAIN_DISK_FILE);
        gvir_config_domain_disk_set_guest_device_type(cdrom, GVIR_CONFIG_DOMAIN_DISK_GUEST_DEVICE_CDROM);
        
        // 🚀 그냥 iso_path가 아니라 config->iso_path 로 접근!
        gvir_config_domain_disk_set_source(cdrom, config->iso_path);
        gvir_config_domain_disk_set_target_bus(cdrom, GVIR_CONFIG_DOMAIN_DISK_BUS_SATA);
        gvir_config_domain_disk_set_target_dev(cdrom, "sda");
        gvir_config_domain_disk_set_readonly(cdrom, TRUE);
        
        gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(cdrom));
        g_object_unref(cdrom);
    }
    
    
    // ==========================================
    // 5. 시각 피질 (VNC Graphics & Virtio Video)
    // ==========================================
    GVirConfigDomainGraphicsVnc *vnc = gvir_config_domain_graphics_vnc_new();
    gvir_config_domain_graphics_vnc_set_autoport(vnc, TRUE); // 5900번부터 자동 할당
    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(vnc));
    g_object_unref(vnc);

    // 🚀 수정됨: VideoModel은 객체가 아니라 enum 이므로 직접 세팅합니다!
    GVirConfigDomainVideo *video = gvir_config_domain_video_new();
    gvir_config_domain_video_set_model(video, GVIR_CONFIG_DOMAIN_VIDEO_MODEL_VIRTIO);
    
    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(video));
    g_object_unref(video);

    // ==========================================
    // 🖥️ Graphics (VNC Socket Binding)
    // ==========================================

    GVirConfigDomainGraphicsVnc *graphics = gvir_config_domain_graphics_vnc_new();

    // 🚀 수정 완료: 컴파일러의 조언대로 VNC 전용 함수를 사용하고, 불필요한 캐스팅을 벗겨냅니다.
    gvir_config_domain_graphics_vnc_set_autoport(graphics, TRUE);

    gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(graphics));
    g_object_unref(graphics);
        
    return domain;
}