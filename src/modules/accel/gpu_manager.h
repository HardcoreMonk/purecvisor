/* ==========================================================================
 * src/modules/accel/gpu_manager.h
 * PureCVisor — GPU Passthrough (vGPU) 매니저 공개 API
 *
 * [파일 역할]
 *   호스트 GPU 장치 열거, vGPU(Mediated Device) 생성/삭제, PCI Passthrough를
 *   통한 VM 직접 연결을 관리하는 모듈의 공개 인터페이스.
 *   lspci, mdevctl, virsh 외부 명령을 래핑하여 RPC/REST API로 노출한다.
 *
 * [아키텍처 위치]
 *   handler_accel.c (gpu.* RPC 핸들러)
 *     -> pcv_gpu_list/info/vgpu_create/attach/detach() [이 모듈]
 *       -> pcv_spawn_sync() 경유 외부 명령 실행
 *
 * [GPU 관리 3가지 모드]
 *   1. 장치 열거: lspci + sysfs로 VGA/3D GPU 목록 조회
 *   2. vGPU (mdev): mdevctl start/stop/list로 가상 GPU 인스턴스 관리
 *   3. PCI Passthrough: virsh attach/detach-device로 VM에 GPU 직접 할당
 *
 * [PCI Passthrough 원리]
 *   GPU를 vfio-pci 드라이버에 바인딩 → libvirt <hostdev> XML 생성
 *   → virsh attach-device로 VM에 핫플러그
 *   PCI 주소(domain:bus:slot.function)를 XML에 16진수 인코딩
 *
 * [메모리 관리]
 *   - JsonArray 반환: 호출자 json_array_unref()
 *   - JsonObject 반환: 호출자 json_object_unref()
 *   - uuid_out: 호출자 g_free()
 *
 * [외부 의존성]
 *   lspci (pciutils), mdevctl, virsh (libvirt-clients)
 * ========================================================================== */

#ifndef PURECVISOR_GPU_MANAGER_H
#define PURECVISOR_GPU_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/** GPU 매니저 초기화 (현재 로그만 출력, 상태 없음) */
void        pcv_gpu_init(void);
/** GPU 매니저 종료 (현재 no-op) */
void        pcv_gpu_shutdown(void);

/** 호스트 전체 GPU 열거 — JsonArray [{pci_addr, description, sriov_vfs, mdev_supported}] */
JsonArray  *pcv_gpu_list(void);

/** GPU 상세 정보 조회 (lspci -v) — JsonObject {pci_addr, detail} */
JsonObject *pcv_gpu_info(const gchar *pci_addr);

/** 지원하는 vGPU(mdev) 타입 목록 — JsonArray [타입명 문자열] */
JsonArray  *pcv_gpu_vgpu_types(const gchar *pci_addr);

/** vGPU 인스턴스 생성 (mdevctl start) — @uuid_out에 생성된 UUID 반환 */
gboolean    pcv_gpu_vgpu_create(const gchar *pci_addr, const gchar *type,
                                 gchar **uuid_out, GError **error);

/** vGPU 인스턴스 삭제 (mdevctl stop) — 멱등성 (미존재 UUID 무시) */
gboolean    pcv_gpu_vgpu_delete(const gchar *uuid, GError **error);

/** 현재 생성된 모든 vGPU 인스턴스 목록 (mdevctl list) */
JsonArray  *pcv_gpu_vgpu_list(void);

/** VM에 GPU PCI Passthrough 연결 (virsh attach-device --live) */
gboolean    pcv_gpu_attach(const gchar *vm_name, const gchar *pci_addr, GError **error);

/** VM에서 GPU PCI Passthrough 분리 (virsh detach-device --live, 멱등성) */
gboolean    pcv_gpu_detach(const gchar *vm_name, const gchar *pci_addr, GError **error);

G_END_DECLS

#endif /* PURECVISOR_GPU_MANAGER_H */
