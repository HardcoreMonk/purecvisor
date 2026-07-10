/**
 * @file vm_config_builder.h
 * @brief VM XML 설정 빌더 — libvirt-gobject 기반 도메인 정의 생성
 *
 * == 아키텍처에서의 위치 ==
 *   vm_manager.c의 create_vm_thread() → vm_config_builder → GVirConfigDomain
 *
 * == 빌더 패턴 ==
 *   1. purecvisor_vm_config_new()로 기본 설정(이름, vCPU, RAM) 생성
 *   2. set_disk/set_iso/set_network_bridge/set_vlan_id로 추가 설정
 *   3. purecvisor_vm_config_build()로 GVirConfigDomain XML 객체 생성
 *   4. 사용 후 purecvisor_vm_config_free()로 설정 해제
 *
 * == 생성되는 XML 구성 ==
 *   - 가상화: KVM (하드웨어 가속)
 *   - 마더보드: q35 (핫플러그 지원)
 *   - 부팅 순서: CD-ROM → 하드디스크
 *   - 디스크: ZFS zvol (virtio 버스, raw 포맷)
 *   - CD-ROM: ISO 파일 (SATA 버스, 읽기 전용)
 *   - 그래픽: VNC (자동 포트 할당, 5900~)
 *   - 비디오: virtio
 *
 * == NIC가 여기서 생성되지 않는 이유 ==
 *   libvirt-gobject에 GVIR_CONFIG_DOMAIN_INTERFACE_MODEL_VIRTIO 상수가 없고
 *   gvir_config_object_get_xml_node()도 공개 API가 아니므로,
 *   NIC(interface) 및 VLAN 태깅은 vm_manager.c에서 XML 문자열을 직접 패치합니다.
 *
 * == 불투명(opaque) 구조체 ==
 *   PureCVisorVmConfig의 실제 필드는 .c 파일에만 정의되어 있습니다.
 *   외부에서는 포인터로만 접근하며, setter 함수를 통해 필드를 설정합니다.
 *
 * 주의: 이 헤더의 PureCVisorVmConfig와 vm_types.h의 동명 구조체는 별개입니다.
 *       vm_types.h는 디스패처 핸들러용 간단한 DTO이고,
 *       이 헤더의 것은 XML 빌드를 위한 불투명 빌더 객체입니다.
 */
/* src/modules/virt/vm_config_builder.h */

#ifndef PURECVISOR_VM_CONFIG_BUILDER_H
#define PURECVISOR_VM_CONFIG_BUILDER_H

#include <glib.h>
#include <libvirt-gobject/libvirt-gobject.h>

/* 불투명 구조체 (Opaque struct) — 실제 필드는 vm_config_builder.c에 정의됨
 * 외부에서는 포인터로만 사용하며, new/free/set/build 함수로만 접근합니다. */
// 불투명 구조체 (Opaque struct)로 사용하거나 정의 필요.
// 여기서는 헤더에 구조체 정의를 포함하거나 typedef만 하고 .c에서 정의.
// 컴파일을 위해 typedef만 선언
typedef struct _PureCVisorVmConfig PureCVisorVmConfig;

/**
 * purecvisor_vm_config_new:
 * @name:   VM 이름 (ZFS zvol 이름으로도 사용됨)
 * @vcpu:   가상 CPU 개수
 * @ram_mb: 메모리 크기 (MB, 내부에서 ×1024로 KB 변환)
 *
 * 기본 VM 설정 객체를 생성합니다.
 * network_bridge는 NULL(NIC 미추가), vlan_id는 0(태깅 없음)으로 초기화됩니다.
 *
 * @return 새 설정 객체 (호출자가 purecvisor_vm_config_free로 해제)
 */
PureCVisorVmConfig *purecvisor_vm_config_new(const gchar *name, gint vcpu, gint ram_mb);

/**
 * purecvisor_vm_config_free:
 * @config: 해제할 설정 객체 (NULL 안전)
 *
 * 내부의 name, disk_path, iso_path, network_bridge 문자열을 모두 g_free합니다.
 */
void purecvisor_vm_config_free(PureCVisorVmConfig *config);

/**
 * purecvisor_vm_config_set_disk:
 * @config: 설정 객체
 * @path:   디스크 경로 — 자동 감지:
 *          - "/dev/zvol/..." → ZFS zvol 블록 디바이스 (type=block, format=raw)
 *          - 그 외 경로       → qcow2 파일 디스크 (type=file, format=qcow2)
 *
 * VM의 메인 디스크 경로를 설정합니다.
 * 기존 값이 있으면 g_free 후 교체합니다.
 */
void purecvisor_vm_config_set_disk(PureCVisorVmConfig *config, const gchar *path);

/**
 * purecvisor_vm_config_set_iso:
 * @config: 설정 객체
 * @path:   ISO 파일 경로 (예: "/pcvpool/iso/ubuntu-24.04-live.iso")
 *
 * VM의 CD-ROM에 마운트할 ISO 경로를 설정합니다.
 * NULL이거나 빈 문자열이면 CD-ROM 장치가 XML에 포함되지 않습니다.
 */
void purecvisor_vm_config_set_iso(PureCVisorVmConfig *config, const gchar *path);

/**
 * purecvisor_vm_config_set_network_bridge:
 * @config:      설정 객체
 * @bridge_name: 브릿지 이름 (예: "pcvbr0", "pcvoverlay0")
 *
 * 주의: 이 값은 build() 단계에서 직접 XML에 삽입되지 않습니다.
 * vm_manager.c에서 이 값을 읽어 XML 문자열을 패치합니다.
 */
// [Added] Bridge 설정 함수
void purecvisor_vm_config_set_network_bridge(PureCVisorVmConfig *config, const gchar *bridge_name);

/**
 * purecvisor_vm_config_set_vlan_id:
 * @config:  설정 객체
 * @vlan_id: VLAN ID (1~4094 범위, 0이면 태깅 비활성화)
 *
 * 범위 밖의 값은 자동으로 0(비활성)으로 클램핑됩니다.
 */
// [Sprint G] VLAN ID 설정 (0 = 비활성, 1~4094 = dot1q)
void purecvisor_vm_config_set_vlan_id(PureCVisorVmConfig *config, gint vlan_id);

/**
 * purecvisor_vm_config_build:
 * @config: 설정 객체
 *
 * 설정된 값을 기반으로 GVirConfigDomain(libvirt XML) 객체를 생성합니다.
 * 생성되는 XML 항목: OS(KVM/q35), 디스크(virtio), CD-ROM(SATA), VNC, Video
 *
 * NIC(interface) XML은 여기서 생성되지 않습니다.
 * vm_manager.c의 create_vm_thread에서 XML 문자열 패치로 추가됩니다.
 *
 * @return GVirConfigDomain* (호출자가 g_object_unref로 해제)
 */
GVirConfigDomain *purecvisor_vm_config_build(PureCVisorVmConfig *config);

/**
 * purecvisor_vm_config_set_boot_mode:
 * @config: 설정 객체
 * @mode:   0=BIOS(기본), 1=UEFI, 2=UEFI+SecureBoot
 *
 * UEFI 사용 시 OVMF 펌웨어 로더가 XML에 패치됩니다.
 */
void purecvisor_vm_config_set_boot_mode(PureCVisorVmConfig *config, gint mode);

/**
 * purecvisor_vm_config_set_tpm:
 * @config:  설정 객체
 * @enabled: TRUE이면 TPM 2.0 에뮬레이터(swtpm) 추가
 */
void purecvisor_vm_config_set_tpm(PureCVisorVmConfig *config, gboolean enabled);

/**
 * purecvisor_vm_config_set_cpu_mode:
 * @config: 설정 객체
 * @mode:   0=Single Edge 기본(host-passthrough), 1=host-passthrough, 2=host-model
 */
void purecvisor_vm_config_set_cpu_mode(PureCVisorVmConfig *config, gint mode);

/**
 * purecvisor_vm_config_set_hugepages:
 * @config:  설정 객체
 * @enabled: TRUE이면 2MB huge pages 사용
 */
void purecvisor_vm_config_set_hugepages(PureCVisorVmConfig *config, gboolean enabled);

#endif
