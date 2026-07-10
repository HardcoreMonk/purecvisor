/**
 * @file handler_accel.h
 * @brief 네트워크 성능 가속 RPC 핸들러 공개 인터페이스 (OVS-DPDK + SR-IOV)
 *
 * ============================================================================
 * [아키텍처 내 위치]
 * ============================================================================
 *   dispatcher.c → 이 파일의 핸들러 → dpdk_manager.c / sriov_manager.c
 *
 *   두 가지 네트워크 가속 기술을 RPC로 제어합니다:
 *     1. OVS-DPDK: 커널 바이패스 기반 소프트웨어 가속.
 *        NIC을 DPDK 드라이버(vfio-pci)에 바인딩하여 userspace에서 직접 패킷 처리.
 *        hugepage 메모리 사용, PMD(Poll Mode Driver) 스레드로 인터럽트 없는 폴링.
 *     2. SR-IOV: 하드웨어 기반 NIC 가상화.
 *        물리 NIC(PF)에서 가상 NIC(VF)을 생성하여 VM에 PCI passthrough로 직접 할당.
 *        하이퍼바이저 경유 없이 NIC에 직접 접근하여 virtio보다 낮은 지연시간.
 *
 * ============================================================================
 * [전형적인 사용 흐름]
 * ============================================================================
 *   DPDK 흐름:
 *     dpdk.hugepage.info → dpdk.bind → dpdk.bridge.create → (VM 연결)
 *     → dpdk.bridge.delete → dpdk.unbind
 *
 *   SR-IOV 흐름:
 *     sriov.enable → sriov.set(MAC/VLAN) → sriov.attach(VM)
 *     → sriov.detach → sriov.disable
 *
 * ============================================================================
 * [RPC 메서드 매핑] (14개)
 *
 *   --- OVS-DPDK (7개) ---
 *   dpdk.status        -> handle_dpdk_status        : DPDK 초기화 상태/버전 조회
 *   dpdk.bind          -> handle_dpdk_bind          : NIC를 DPDK 드라이버(vfio-pci)에 바인딩
 *   dpdk.unbind        -> handle_dpdk_unbind        : NIC를 커널 드라이버로 복원
 *   dpdk.list          -> handle_dpdk_list          : DPDK 바인딩된 NIC 목록
 *   dpdk.bridge.create -> handle_dpdk_bridge_create : DPDK 가속 OVS 브릿지 생성
 *   dpdk.bridge.delete -> handle_dpdk_bridge_delete : DPDK OVS 브릿지 삭제
 *   dpdk.hugepage.info -> handle_dpdk_hugepage_info : HugePages 할당 현황 조회
 *
 *   --- SR-IOV (7개) ---
 *   sriov.status  -> handle_sriov_status  : SR-IOV 지원 NIC 및 VF 현황 조회
 *   sriov.enable  -> handle_sriov_enable  : 물리 NIC에 VF(Virtual Function) 활성화
 *   sriov.disable -> handle_sriov_disable : VF 비활성화
 *   sriov.list    -> handle_sriov_list    : 활성화된 VF 목록
 *   sriov.set     -> handle_sriov_set     : VF 속성 설정 (VLAN, MAC, spoof-check 등)
 *   sriov.attach  -> handle_sriov_attach  : VF를 VM에 PCI passthrough로 연결
 *   sriov.detach  -> handle_sriov_detach  : VF를 VM에서 분리
 *
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 * [모든 핸들러 동기 응답 — fire-and-forget 미사용]
 *
 * [CLI 명령어 매핑]
 *   pcvctl dpdk status / bind / unbind / list / bridge-create / bridge-delete / hugepage
 *   pcvctl sriov status / enable / disable / list / set / attach / detach
 * ──────────────────────────────────────────────────────────────
 */
#ifndef PURECVISOR_HANDLER_ACCEL_H
#define PURECVISOR_HANDLER_ACCEL_H

#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

/* ================================================================
 * OVS-DPDK RPC 핸들러 (7개)
 * - DPDK(Data Plane Development Kit)를 통한 커널 바이패스 네트워크 I/O
 * - HugePages 기반 제로카피 패킷 처리
 * - dpdk_manager.c에서 실제 명령어 실행
 * ================================================================ */

/** params: {} — DPDK 초기화 상태, 버전, 바인딩된 NIC 수 */
void handle_dpdk_status(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"pci_addr":"0000:03:00.0", "driver":"vfio-pci"} — NIC를 DPDK 드라이버에 바인딩 */
void handle_dpdk_bind(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"pci_addr":"0000:03:00.0"} — NIC를 원래 커널 드라이버로 복원 */
void handle_dpdk_unbind(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {} — DPDK에 바인딩된 모든 NIC 목록 */
void handle_dpdk_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"name":"br-dpdk", "dpdk_port":"dpdk0"} — DPDK 가속 OVS 브릿지 생성 */
void handle_dpdk_bridge_create(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"name":"br-dpdk"} — DPDK OVS 브릿지 삭제 */
void handle_dpdk_bridge_delete(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {} — HugePages 크기/할당/사용 현황 */
void handle_dpdk_hugepage_info(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

/* ================================================================
 * SR-IOV RPC 핸들러 (7개)
 * - SR-IOV(Single Root I/O Virtualization)를 통한 하드웨어 수준 NIC 가상화
 * - 물리 NIC(PF)에서 가상 NIC(VF)를 생성하여 VM에 직접 패스스루
 * - sriov_manager.c에서 실제 명령어 실행
 * ================================================================ */

/** params: {} — SR-IOV 지원 NIC 목록 및 VF 현황 */
void handle_sriov_status(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"pf":"enp3s0f0", "num_vfs":4} — 물리 NIC에 VF 활성화 (sysfs 경유) */
void handle_sriov_enable(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"pf":"enp3s0f0"} — VF 비활성화 (num_vfs=0) */
void handle_sriov_disable(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {} — 활성화된 VF 목록 (PCI 주소, MAC, VLAN 등) */
void handle_sriov_list(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"pf":"enp3s0f0", "vf_index":0, "mac":"...", "vlan":100} — VF 속성 설정 */
void handle_sriov_set(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"vm_name":"web-prod", "pci_addr":"0000:03:10.0"} — VF를 VM에 vfio-pci passthrough */
void handle_sriov_attach(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);
/** params: {"vm_name":"web-prod", "pci_addr":"0000:03:10.0"} — VF를 VM에서 분리 */
void handle_sriov_detach(JsonObject *p, const gchar *id, UdsServer *s, GSocketConnection *c);

G_END_DECLS

#endif /* PURECVISOR_HANDLER_ACCEL_H */
