/**
 * @file vm_config_builder.c
 * @brief VM XML 설정 빌더 — libvirt-gobject API로 도메인 XML 생성
 *
 * == 아키텍처에서의 위치 ==
 *   vm_manager.c → create_vm_thread() → vm_config_build() → GVirConfigDomain
 *                                          ↓
 *                                     gvir_config_object_to_xml() → XML 문자열
 *                                          ↓
 *                                     vm_manager.c에서 NIC XML 패치
 *                                          ↓
 *                                     virDomainDefineXML() → libvirt 등록
 *
 * == 이 파일이 생성하는 XML 구조 ==
 *   <domain type='kvm'>
 *     <name>vm-name</name>
 *     <memory unit='KiB'>2097152</memory>    ← memory_mb × 1024
 *     <vcpu>4</vcpu>
 *     <os>
 *       <type machine='q35'>hvm</type>       ← q35 마더보드 (핫플러그 지원)
 *       <boot dev='cdrom'/>                  ← 1순위: ISO
 *       <boot dev='hd'/>                     ← 2순위: 디스크
 *     </os>
 *     <features><acpi/><apic/></features>    ← 최신 Linux 설치기 필수
 *     <cpu mode='host-passthrough'/>          ← Single Edge 기본 CPU
 *     <devices>
 *       <disk type='block'>                  ← ZFS zvol 블록 디바이스 (zvol 모드)
 *         <driver name='qemu' type='raw'/>
 *         <source dev='/dev/zvol/pcvpool/vms/vm-name'/>
 *         <target bus='virtio' dev='vda'/>
 *       </disk>
 *       -- 또는 (qcow2 파일 모드) --
 *       <disk type='file'>                   ← qcow2 파일 디스크 (폴백 모드)
 *         <driver name='qemu' type='qcow2'/>
 *         <source file='/var/lib/libvirt/images/vm-name.qcow2'/>
 *         <target bus='virtio' dev='vda'/>
 *       </disk>
 *       <disk type='file' device='cdrom'>    ← ISO (있을 때만)
 *         <source file='/path/to/iso'/>
 *         <target bus='sata' dev='sda'/>
 *         <readonly/>
 *       </disk>
 *       <graphics type='vnc' autoport='yes'/> ← VNC (5900부터 자동 할당)
 *       <video><model type='virtio'/></video>
 *       <channel type='unix'>                 ← 게스트 에이전트 (VP-2)
 *         <target type='virtio' name='org.qemu.guest_agent.0'/>
 *       </channel>
 *     </devices>
 *   </domain>
 *
 * == NIC(<interface>)가 빠진 이유 ==
 *   libvirt-gobject에 NIC model 설정 API가 부족하므로
 *   vm_manager.c에서 XML 문자열의 </devices> 직전에 직접 삽입합니다.
 */
/* src/modules/virt/vm_config_builder.c */

#include "vm_config_builder.h"
#include <libvirt-gobject/libvirt-gobject.h>
#include <glib.h>

/**
 * PureCVisorVmConfig 내부 구조체 정의
 * 헤더에서는 typedef만 선언 (불투명 타입)하고 여기서 실제 필드를 정의합니다.
 * 외부에서는 이 필드에 직접 접근할 수 없고, setter 함수만 사용해야 합니다.
 */
struct _PureCVisorVmConfig {
    gchar *name;              /* VM 이름 — ZFS zvol 이름과 동일 */
    gint vcpu;                /* 가상 CPU 개수 */
    gint memory_mb;           /* 메모리 크기 (MB) — build 시 ×1024로 KB 변환 */
    gchar *disk_path;         /* ZFS zvol 블록 디바이스 경로 (예: /dev/zvol/pcvpool/vms/xxx) */
    const gchar *iso_path;    /* ISO 파일 경로 (NULL이면 CD-ROM 미생성) */
    const gchar *network_bridge; /* [Phase 5-2] 브릿지 이름 (예: "br0", "virbr0") */
    gint          vlan_id;       /* [Sprint G] 0 = 태깅 없음, 1~4094 = dot1q VLAN ID */
    gint boot_mode;              /* 0=BIOS(기본), 1=UEFI, 2=UEFI+SecureBoot */
    gboolean tpm;                /* TRUE=TPM 2.0 에뮬레이터 추가 */
    gint cpu_mode;               /* 0=Single Edge 기본(host-passthrough), 1=host-passthrough, 2=host-model */
    gboolean hugepages;          /* TRUE=2MB huge pages 사용 */
};

/**
 * purecvisor_vm_config_new:
 * 기본 VM 설정 객체를 힙에 할당하고 초기화합니다.
 * g_new0로 모든 필드를 0/NULL로 초기화 후 필수 값만 설정합니다.
 */
PureCVisorVmConfig *purecvisor_vm_config_new(const gchar *name, gint vcpu, gint ram_mb) {
    PureCVisorVmConfig *config = g_new0(PureCVisorVmConfig, 1);
    config->name = g_strdup(name);
    config->vcpu = vcpu;
    config->memory_mb = ram_mb;
    config->network_bridge = NULL; // Default to NAT
    config->vlan_id        = 0;    // No VLAN tagging
    return config;
}

/**
 * purecvisor_vm_config_free:
 * 설정 객체와 내부 문자열을 모두 해제합니다.
 * NULL 안전 — NULL이 들어오면 아무것도 하지 않습니다.
 *
 * 주의: iso_path와 network_bridge는 const gchar*로 선언되어 있지만
 * g_strdup()으로 복사한 값이므로 g_free()로 해제해야 합니다.
 * (gpointer) 캐스팅으로 const 제거 후 해제합니다.
 */
void purecvisor_vm_config_free(PureCVisorVmConfig *config) {
    if (!config) return;
    g_free(config->name);
    g_free(config->disk_path);
    g_free((gpointer)config->iso_path);
    g_free((gpointer)config->network_bridge);
    g_free(config);
}

/** 디스크 경로 설정 — 기존 값이 있으면 먼저 해제 */
void purecvisor_vm_config_set_disk(PureCVisorVmConfig *config, const gchar *path) {
    if (config->disk_path) g_free(config->disk_path);
    config->disk_path = g_strdup(path);
}

/** ISO 경로 설정 — 기존 값이 있으면 먼저 해제 */
void purecvisor_vm_config_set_iso(PureCVisorVmConfig *config, const gchar *path) {
    if (config->iso_path) g_free((gpointer)config->iso_path);
    config->iso_path = g_strdup(path);
}

/** [Phase 5-2] 브릿지 이름 설정 — 기존 값이 있으면 먼저 해제 */
// [Phase 5-2] Bridge 설정 Setter
void purecvisor_vm_config_set_network_bridge(PureCVisorVmConfig *config, const gchar *bridge_name) {
    if (config->network_bridge) g_free((gpointer)config->network_bridge);
    config->network_bridge = g_strdup(bridge_name);
}

/**
 * [Sprint G] VLAN ID 설정
 * 유효 범위(1~4094) 밖의 값은 자동으로 0(비활성)으로 클램핑됩니다.
 * IEEE 802.1Q 표준에 따라 VLAN ID 0은 태그 없음, 4095는 예약 값입니다.
 */
// [Sprint G] VLAN ID Setter (0 = 비활성화, 1~4094 = dot1q tag)
void purecvisor_vm_config_set_vlan_id(PureCVisorVmConfig *config, gint vlan_id) {
    config->vlan_id = (vlan_id >= 1 && vlan_id <= 4094) ? vlan_id : 0;
}

/**
 * 부팅 모드 설정
 * 0=BIOS(기본), 1=UEFI, 2=UEFI+SecureBoot
 * UEFI 사용 시 vm_manager.c에서 XML 패치로 OVMF 로더가 삽입됩니다.
 */
void purecvisor_vm_config_set_boot_mode(PureCVisorVmConfig *config, gint mode) {
    config->boot_mode = mode;
}

/**
 * TPM 2.0 에뮬레이터 활성화 설정
 * TRUE이면 vm_manager.c에서 XML 패치로 <tpm> 디바이스가 삽입됩니다.
 * swtpm 백엔드를 사용하며 UEFI SecureBoot에 필요합니다.
 */
void purecvisor_vm_config_set_tpm(PureCVisorVmConfig *config, gboolean enabled) {
    config->tpm = enabled;
}

/**
 * CPU 모드 설정
 * 0=Single Edge 기본(host-passthrough), 1=host-passthrough, 2=host-model
 * host-passthrough: 호스트 CPU 기능을 그대로 게스트에 노출 (최고 성능)
 * host-model: 호스트와 유사한 CPU 모델 사용 (라이브 마이그레이션 호환)
 */
void purecvisor_vm_config_set_cpu_mode(PureCVisorVmConfig *config, gint mode) {
    config->cpu_mode = mode;
}

/**
 * 2MB Huge Pages 사용 설정
 * TRUE이면 vm_manager.c에서 XML 패치로 <memoryBacking> 블록이 삽입됩니다.
 * 호스트에 충분한 hugepage가 할당되어 있어야 합니다 (sysctl vm.nr_hugepages).
 */
void purecvisor_vm_config_set_hugepages(PureCVisorVmConfig *config, gboolean enabled) {
    config->hugepages = enabled;
}

/**
 * purecvisor_vm_config_build:
 * 설정 값을 기반으로 libvirt-gobject GVirConfigDomain 객체를 조립합니다.
 *
 * 조립 순서:
 *   1. 기본 정보: 이름, 메모리(KB), vCPU, 가상화 타입(KVM)
 *   2. OS 설정: HVM 타입, q35 머신, 부팅 순서(CD-ROM → HDD)
 *   3. 기능/CPU: ACPI+APIC, 기본 host-passthrough CPU
 *   4. 메인 디스크: ZFS zvol 블록 디바이스 (virtio/raw)
 *   5. CD-ROM: ISO 파일 (있을 때만, SATA/readonly)
 *   6. 그래픽: VNC (autoport=yes, 5900번부터 자동 할당)
 *   7. 비디오: virtio 모델
 *
 * @return GVirConfigDomain* — 호출자가 g_object_unref()로 해제
 *
 * 주의: 반환된 객체를 gvir_config_object_to_xml()로 XML 문자열화한 후
 * vm_manager.c에서 NIC XML을 </devices> 앞에 직접 패치합니다.
 */
GVirConfigDomain *purecvisor_vm_config_build(PureCVisorVmConfig *config) {
    if (!config) return NULL;

    GVirConfigDomain *domain = gvir_config_domain_new();

    /* 가상화 타입을 KVM으로 명시 설정
     * 이 설정이 누락되면 libvirt가 도메인 정의를 거부합니다.
     * 개발 환경(중첩 가상화 미지원)에서는 GVIR_CONFIG_DOMAIN_VIRT_QEMU 사용 가능 */
    // 하드웨어 가속(KVM)을 지원하지 않는 환경(예: 단순 가상머신 위에서 개발 중)이라면 GVIR_CONFIG_DOMAIN_VIRT_KVM 대신 GVIR_CONFIG_DOMAIN_VIRT_QEMU를 사용
    // [Fix] 가상화 타입(KVM) 명시 (이 부분이 누락되어 Libvirt가 거부함)
    gvir_config_domain_set_virt_type(domain, GVIR_CONFIG_DOMAIN_VIRT_KVM);

    /* 1. 기본 정보 설정 */
    // 1. Basic Info
    gvir_config_domain_set_name(domain, config->name);
    gvir_config_domain_set_memory(domain, config->memory_mb * 1024);
    gvir_config_domain_set_vcpus(domain, config->vcpu);

    /* 2. OS 및 부팅 설정
     * - HVM: 하드웨어 가상 머신 (전가상화)
     * - q35: 최신 마더보드 칩셋 (PCIe 지원, 핫플러그 네이티브 지원)
     *        구형 i440fx 대비 NVMe, USB3, PCIe passthrough 등 지원
     * - 부팅 순서: CD-ROM(ISO 설치용) → HDD(정상 부팅) */
    // 2. OS & Features
    GVirConfigDomainOs *os = gvir_config_domain_os_new();
    gvir_config_domain_os_set_os_type(os, GVIR_CONFIG_DOMAIN_OS_TYPE_HVM);

    // 🚀 [추가] 1996년산 i440fx 대신, 핫플러그를 네이티브 지원하는 최신 q35 마더보드로 강제 업그레이드!
    gvir_config_domain_os_set_machine(os, "q35");

    /* 부팅 디바이스 목록 생성
     * GList에 enum 값을 GINT_TO_POINTER로 변환하여 추가합니다.
     * set_boot_devices()가 리스트를 복사하므로 호출 후 즉시 g_list_free 가능 */
    // 🚀 링커 에러를 피하기 위해 GList로 부팅 순서를 묶어서 한방에 전달합니다!
    GList *boot_devs = NULL;
    boot_devs = g_list_append(boot_devs, GINT_TO_POINTER(GVIR_CONFIG_DOMAIN_OS_BOOT_DEVICE_CDROM)); // 1순위: CD-ROM
    boot_devs = g_list_append(boot_devs, GINT_TO_POINTER(GVIR_CONFIG_DOMAIN_OS_BOOT_DEVICE_HD)); // 2순위: 하드디스크

    gvir_config_domain_os_set_boot_devices(os, boot_devs);
    g_list_free(boot_devs); // 메모리 누수 방지 청소!

    gvir_config_domain_set_os(domain, os);
    g_object_unref(os);  /* [주니어 참고] GObject 참조 카운트 패턴:
                          * set_os()가 os를 ref(+1) → 여기서 unref(-1) → 카운트=1
                          * domain이 해제될 때 os도 자동 unref(-1) → 카운트=0 → 메모리 해제
                          * 이 패턴을 따르지 않으면 메모리 누수가 발생합니다. */

    /* 3. 게스트 기본 기능 및 CPU 모드
     * - ACPI/APIC: 최신 Linux 설치기와 정상 종료 경로에 필요합니다.
     * - host-passthrough: Single Edge는 라이브 마이그레이션보다 단일 노드
     *   호환성과 성능이 우선입니다. Rocky/RHEL 10 계열은 x86-64-v3 CPU
     *   기능이 필요하므로 qemu64 기본값을 사용하지 않습니다.
     *
     * [비전공자 설명]
     * 게스트 OS가 "이 컴퓨터는 어떤 CPU인가"를 물어볼 때, qemu64 같은
     * 오래된 가짜 CPU 대신 실제 서버 CPU 기능을 그대로 보여 줍니다.
     * 최신 리눅스 설치기가 필요한 명령어 세트를 찾지 못해 커널 패닉으로
     * 멈추는 일을 줄이기 위한 기본값입니다. */
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

    /* 4. 메인 디스크 — zvol 블록 디바이스 또는 qcow2 파일 자동 감지
     *
     * disk_path가 "/dev/" 로 시작하면 zvol 블록 디바이스:
     *   - type=block, format=raw, source dev=...
     * 그 외에는 qcow2 파일 디스크:
     *   - type=file, format=qcow2, source file=...
     *
     * 두 경우 모두 bus=virtio, target=vda */
    /* [주니어 참고] zvol vs qcow2 자동 감지 원리:
     * "/dev/" 로 시작하면 ZFS zvol 블록 디바이스 (예: /dev/zvol/pcvpool/vms/web-prod)
     *   → type=block, format=raw: 블록 디바이스를 raw로 직접 접근 (최고 성능)
     * 그 외 경로면 qcow2 이미지 파일 (예: /var/lib/libvirt/images/web-prod.qcow2)
     *   → type=file, format=qcow2: Copy-on-Write 파일 디스크 (스냅샷 지원)
     *
     * ZFS가 설치되지 않은 환경에서는 qcow2 폴백이 자동으로 동작합니다. */
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
        /* 포맷 자동 감지: /dev/ → raw(zvol), .img → raw(파일), 그 외 → qcow2 */
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

        /* [주니어 참고] 디스크 버스 타입별 차이:
         *   virtio : 준가상화 (vd*) — 최고 성능, 모든 최신 OS 지원
         *   scsi   : SCSI 에뮬레이션 (sd*) — 범용, 구형 OS 호환
         *   ide    : IDE 에뮬레이션 (hd*) — 레거시, 매우 느림
         *   sata   : SATA 에뮬레이션 (sd*) — CD-ROM에 주로 사용 */
        gvir_config_domain_disk_set_target_bus(disk, GVIR_CONFIG_DOMAIN_DISK_BUS_VIRTIO);
        gvir_config_domain_disk_set_target_dev(disk, "vda");  /* vda = virtio 디스크 첫 번째 디바이스 */

        /* domain에 디스크 디바이스 추가 후 로컬 참조 해제
         * [주니어 참고] add_device()가 내부적으로 disk를 ref하므로
         * 여기서 unref해도 domain이 해제될 때까지 disk는 살아 있습니다. */
        gvir_config_domain_add_device(domain, GVIR_CONFIG_DOMAIN_DEVICE(disk));
        g_object_unref(disk);
    }

    /* 4. 가상 CD-ROM 드라이브 (ISO 마운트)
     * - type=file: ISO 파일 경로 사용
     * - guest_device=cdrom: 게스트에 CD-ROM으로 인식
     * - bus=sata: 광학 드라이브 표준 버스 (virtio는 CD-ROM 미지원)
     * - target=sda: SATA 첫 번째 디바이스
     * - readonly: ISO는 읽기 전용 */
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


    /* 5. VNC 그래픽 + 비디오 디바이스
     * - VNC autoport=yes: libvirt가 5900번부터 사용 가능한 포트를 자동 할당
     *   (VM별 고유 포트, virsh vncdisplay 또는 XML에서 확인 가능)
     * - Video model=virtio: 준가상화 비디오 — 최고 성능, 해상도 자동 조절
     *   (대안: QXL=SPICE용, VGA=레거시, Cirrus=구형 호환) */
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
    // 6. 게스트 에이전트 채널 (org.qemu.guest_agent.0)
    // ==========================================
    /* VP-2: guest-ping/guest-exec/guest-shutdown 은 이 virtio-serial 채널이
     * 있어야만 동작한다. 소켓 경로는 libvirt 가 자동 관리(mode=bind)하고
     * 게스트에 에이전트가 없어도 무해하므로 무조건 포함한다.
     * (target 설정 후 source 설정 — libvirt-gconfig 채널 조립 관례 순서) */
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

    /* NIC(<interface>) 및 VLAN 태깅은 vm_config_builder 수준에서
     * libvirt-gobject API 제약(모델 enum 부재, xmlNode 미공개)으로 인해
     * vm_manager.c의 create_vm_thread에서 XML 문자열 직접 패치로 처리합니다.
     *
     * 패치 대상: build() 반환값을 gvir_config_object_to_xml()한 문자열에서
     * "</devices>" 직전에 <interface> 블록을 삽입합니다. */
    // NOTE: NIC(<interface>) 및 VLAN 태깅은 vm_config_builder 수준에서
    // libvirt-gobject API 제약(모델 enum, xmlNode 미공개)으로 인해
    // vm_manager.c의 create_vm_thread에서 XML 문자열 직접 패치로 처리합니다.

    return domain;
}
