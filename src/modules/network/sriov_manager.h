/**
 * @file sriov_manager.h
 * @brief SR-IOV 매니저 공개 인터페이스 -- VF 관리 + PCI Passthrough
 *
 * ====================================================================
 * [역할]
 *   handler_accel.c 에서 sriov.* RPC 7개 처리 시 호출.
 *   VM에 물리 NIC의 VF를 직접 할당하여 near-native 네트워크 성능 제공.
 *
 * [SR-IOV 기본 개념 (주니어 참고)]
 *   SR-IOV(Single Root I/O Virtualization)는 PCIe 디바이스를
 *   하드웨어 레벨에서 여러 가상 디바이스로 분리하는 기술이다.
 *
 *   [PF (Physical Function)]
 *     물리 NIC의 실제 PCIe 기능. 호스트 OS가 직접 사용.
 *     예: eno1, enp3s0f0
 *
 *   [VF (Virtual Function)]
 *     PF에서 파생된 경량 가상 PCIe 기능.
 *     각 VF는 자체 MAC/VLAN/큐를 가지며 VM에 직접 할당 가능.
 *     VF 수는 NIC 펌웨어에 의해 결정 (sriov_totalvfs).
 *     일반적으로 PF당 최대 64~128개 VF 지원.
 *
 *   [PCI Passthrough]
 *     VF를 VM에 직접 할당하여 호스트 커널을 경유하지 않고
 *     VM이 NIC에 직접 접근하게 하는 기술.
 *     - vfio-pci 드라이버: VM에 안전한 PCI passthrough 제공 (IOMMU 필수)
 *     - virsh attach-device: 실행 중 VM에 PCI 디바이스 핫플러그
 *
 *   [IOMMU]
 *     I/O Memory Management Unit. DMA 접근을 격리하여
 *     VM이 다른 VM이나 호스트의 메모리에 접근하지 못하게 한다.
 *     BIOS에서 VT-d(Intel) 또는 AMD-Vi(AMD) 활성화 필수.
 *     커널 파라미터: intel_iommu=on 또는 amd_iommu=on
 *
 *   [성능 비교]
 *     virtio-net:   ~10Gbps, 호스트 CPU 소비 높음
 *     SR-IOV VF:    line-rate(10/25/40/100Gbps), 호스트 CPU 소비 최소
 *     DPDK vhost:   ~10Gbps+, CPU 코어 전용 할당 필요
 *
 * [호출 흐름]
 *   handler_accel.c (sriov.* RPC) --> pcv_sriov_*() --> sysfs + vfio-pci
 *   VF 생성(echo N > sriov_numvfs) --> MAC/VLAN 설정 --> vfio-pci 바인딩
 *   --> virsh attach-device 로 VM에 PCI passthrough.
 *
 * [함수 분류]
 *   Lifecycle    : init, shutdown
 *   Status       : status (지원 NIC 스캔, 활성 VF 수, IOMMU 확인)
 *   VF 관리      : enable(VF 생성), disable(전부 제거), list(VF 목록)
 *   VF 설정      : set (MAC/VLAN/spoofchk 변경, ip link set 사용)
 *   VM 연결      : attach_vm (vfio-pci + virsh), detach_vm (virsh)
 *   헬퍼         : vf_pci_addr (VF 인덱스 --> PCI 주소 문자열)
 *
 * [파라미터 용어]
 *   pf       : Physical Function 인터페이스명 (예: "eno1", "enp3s0f0")
 *   vf_index : VF 번호 (0-based, 최대 sriov_totalvfs-1)
 *   pci_addr : PCI 주소 문자열 "DDDD:BB:SS.F" (예: "0000:03:10.0")
 *              Domain(16bit):Bus(8bit):Slot(5bit).Function(3bit)
 *
 * [sysfs 인터페이스]
 *   /sys/class/net/<pf>/device/sriov_totalvfs  -- 최대 VF 수 (읽기전용, 하드웨어 결정)
 *   /sys/class/net/<pf>/device/sriov_numvfs    -- 현재 VF 수 (읽기/쓰기)
 *   /sys/class/net/<pf>/device/virtfnN         -- VF N의 PCI 디바이스 심볼릭 링크
 *
 * [주의]
 *   IOMMU 비활성 환경에서는 attach_vm 이 실패한다.
 *   enable 호출 시 기존 VF가 있으면 먼저 0으로 리셋 후 재생성.
 *   VF 수는 SRIOV_MAX_VFS(64)로 내부 클램프된다.
 * ====================================================================
 */
#ifndef PURECVISOR_SRIOV_MANAGER_H
#define PURECVISOR_SRIOV_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/*
 * SR-IOV Manager -- VF(Virtual Function) 생성/관리 + PCI Passthrough
 *
 * Phase 4: SR-IOV VF 생성, 속성 설정, VM PCI passthrough
 *
 * sysfs (/sys/class/net/<pf>/device/sriov_numvfs) 직접 접근 +
 * ip link set 명령으로 VF 속성 관리.
 * Graceful degradation: SR-IOV 미지원 NIC 시 available=false
 */

/* ---- Lifecycle ---- */
void     pcv_sriov_init(void);      /* 뮤텍스 초기화. 데몬 시작 시 호출. */
void     pcv_sriov_shutdown(void);  /* 뮤텍스 해제. 데몬 종료 시 호출. */

/* ---- Status ---- */
/**
 * pcv_sriov_status -- SR-IOV 전체 상태 조회
 * @return {available, physical_functions: [{name, pci_addr, max_vfs, current_vfs, driver, iommu_enabled}]}
 */
JsonObject *pcv_sriov_status(void);

/* ---- VF management ---- */
gboolean pcv_sriov_enable(const gchar *pf, gint num_vfs, GError **error);  /* VF 생성 */
gboolean pcv_sriov_disable(const gchar *pf, GError **error);               /* VF 전부 제거 (멱등) */
JsonArray *pcv_sriov_list(const gchar *pf);                                /* VF 목록 조회 */

/* ---- VF configuration ---- */
/**
 * pcv_sriov_set -- VF 속성 설정
 * @pf: 물리 NIC 이름
 * @vf_index: VF 인덱스 (0-based)
 * @mac: (nullable) MAC 주소 설정 (NULL이면 건너뜀)
 * @vlan: VLAN ID 설정 (-1이면 건너뜀, 0이면 VLAN 해제)
 * @spoofchk: 스푸핑 체크 (0=off, 1=on, -1이면 건너뜀)
 */
gboolean pcv_sriov_set(const gchar *pf, gint vf_index,
                        const gchar *mac, gint vlan,
                        gint spoofchk, GError **error);

/* ---- VM attachment -- libvirt PCI hostdev XML 생성 ---- */
/**
 * pcv_sriov_attach_vm -- VM에 VF PCI passthrough 연결
 * 실행 중 VM에 핫플러그 (virsh attach-device --live).
 * IOMMU 활성화가 전제 조건.
 */
gboolean pcv_sriov_attach_vm(const gchar *vm_name, const gchar *pf,
                              gint vf_index, GError **error);

/**
 * pcv_sriov_detach_vm -- VM에서 VF PCI 디바이스 분리 (멱등)
 * virsh detach-device --live 사용.
 */
gboolean pcv_sriov_detach_vm(const gchar *vm_name, const gchar *pci_addr, GError **error);

/* ---- PCI address helpers ---- */
/**
 * pcv_sriov_vf_pci_addr -- VF 인덱스에서 PCI 주소 조회
 * @pf: 물리 NIC 이름
 * @vf_index: VF 인덱스
 * @return (transfer full): "0000:03:10.0" 형태 PCI 주소 (호출자 g_free), 실패 시 NULL
 */
gchar *pcv_sriov_vf_pci_addr(const gchar *pf, gint vf_index);

G_END_DECLS

#endif /* PURECVISOR_SRIOV_MANAGER_H */
