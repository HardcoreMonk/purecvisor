/**
 * @file handler_vm_hotplug.h
 * @brief VM 핫플러그 RPC 핸들러 공개 인터페이스
 *
 * ──────────────────────────────────────────────────────────────
 * [아키텍처 내 위치]
 *   dispatcher.c → 이 파일의 핸들러 → libvirt API (virDomainAttachDevice 등)
 *   실행 중인 VM에 대해 재부팅 없이 하드웨어를 동적으로 추가/제거한다.
 *
 * [RPC 메서드 매핑] (12개)
 *   vm.set_vcpu            -> handle_vm_set_vcpu_request        (콜백 기반 비동기 — vCPU 수 변경)
 *   vm.set_memory          -> handle_vm_set_memory_request      (콜백 기반 비동기 — 메모리 크기 변경)
 *   vm.memory.stats        -> handle_vm_memory_stats_request    (동기 — 벌룬 메모리 통계)
 *   vm.cpu.stats           -> handle_vm_cpu_stats_request       (동기 — per-vCPU 통계)
 *   vm.disk.live_resize    -> handle_vm_disk_live_resize_request(fire-and-forget — 라이브 디스크 리사이즈)
 *   vm.eject               -> handle_vm_eject_iso               (동기 — CDROM 미디어 제거)
 *   vm.mount_iso           -> handle_vm_mount_iso               (동기 — ISO 파일 마운트)
 *   device.disk.attach     -> handle_device_disk_attach         (동기 — 블록 디바이스 연결)
 *   device.disk.detach     -> handle_device_disk_detach         (동기 — 블록 디바이스 분리)
 *   device.nic.list        -> handle_device_nic_list            (동기 — VM NIC 목록 조회)
 *   device.nic.attach      -> handle_device_nic_attach          (동기 — 가상 NIC 추가)
 *   device.nic.detach      -> handle_device_nic_detach          (동기 — 가상 NIC 제거)
 *
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 * [핫플러그 원리]
 *   libvirt의 virDomainAttachDeviceFlags()/virDomainDetachDeviceFlags()를 사용.
 *   VIR_DOMAIN_AFFECT_LIVE 플래그로 실행 중인 VM에 즉시 적용된다.
 *   QEMU의 QMP(QEMU Machine Protocol)를 통해 게스트 OS에 디바이스가 추가/제거.
 *
 * [REST API 매핑]
 *   PUT  /api/v1/vms/{name}/vcpu       -> vm.set_vcpu
 *   PUT  /api/v1/vms/{name}/memory     -> vm.set_memory
 *   POST /api/v1/vms/{name}/iso        -> vm.mount_iso
 *   DELETE /api/v1/vms/{name}/iso      -> vm.eject
 *   GET  /api/v1/vms/{name}/nics       -> device.nic.list
 *   POST /api/v1/vms/{name}/nics       -> device.nic.attach
 *   DELETE /api/v1/vms/{name}/nics/{mac} -> device.nic.detach
 * ──────────────────────────────────────────────────────────────
 */
#ifndef PURECVISOR_DISPATCHER_HANDLER_VM_HOTPLUG_H
#define PURECVISOR_DISPATCHER_HANDLER_VM_HOTPLUG_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

/**
 * @brief VM 메모리 크기를 동적으로 변경한다 (콜백 기반 비동기).
 * @param params  {"name":"<vm>", "memory_mb":<int>} — VM 이름, 새 메모리(MB)
 */
void handle_vm_set_memory_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM vCPU 수를 동적으로 변경한다 (콜백 기반 비동기).
 * @param params  {"name":"<vm>", "vcpu":<int>} — VM 이름, 새 vCPU 수
 */
void handle_vm_set_vcpu_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief 블록 디바이스(디스크)를 VM에 핫 연결한다 (동기).
 * @param params  {"name":"<vm>", "source":"<zvol_path>", "target":"vdb"} — 디스크 경로, 타겟 디바이스명
 */
void handle_device_disk_attach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief 블록 디바이스(디스크)를 VM에서 핫 분리한다 (동기).
 * @param params  {"name":"<vm>", "target":"vdb"} — 분리할 타겟 디바이스명
 */
void handle_device_disk_detach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM에 연결된 NIC 목록을 조회한다 (동기).
 * @param params  {"name":"<vm>"} — VM 이름
 * 응답: JSON 배열 [{mac, model, bridge, ...}, ...]
 */
void handle_device_nic_list  (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief 가상 NIC를 VM에 핫 추가한다 (동기).
 * @param params  {"name":"<vm>", "bridge":"<br>", "model":"virtio"} — 브릿지, NIC 모델(기본 virtio)
 *                "mac" 필드 생략 시 랜덤 MAC 자동 생성
 */
void handle_device_nic_attach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief 가상 NIC를 VM에서 핫 제거한다 (동기).
 * @param params  {"name":"<vm>", "mac":"52:54:00:xx:xx:xx"} — 제거할 NIC의 MAC 주소
 */
void handle_device_nic_detach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief ISO 파일을 VM CDROM에 마운트한다 (동기).
 * @param params  {"name":"<vm>", "iso_path":"/pcvpool/iso/ubuntu.iso"} — ISO 파일 경로
 */
void handle_vm_mount_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM CDROM에서 미디어를 제거한다 (동기).
 * @param params  {"name":"<vm>"} — VM 이름
 */
void handle_vm_eject_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM의 특정 vCPU를 물리 CPU 코어에 피닝한다 (동기).
 * @param params  {"name":"<vm>", "vcpu":<int>, "cpuset":"4-7"} — vCPU 번호, CPU 범위
 *
 * cpuset 형식: "0-3" (범위), "4,5,6" (개별), "0-3,8-11" (혼합)
 * virDomainPinVcpu API를 사용하여 실행 중인 VM에 즉시 적용.
 */
void handle_vm_pin_vcpu(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM 네트워크 인터페이스의 대역폭을 제한한다 (동기, QoS).
 * @param params  {"name":"<vm>", "inbound_kbps":<int>, "outbound_kbps":<int>}
 *                inbound/outbound 중 최소 하나는 > 0이어야 함
 * libvirt virDomainSetInterfaceParameters() 사용.
 */
void handle_vm_set_bandwidth(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM 메모리 벌룬 통계를 조회한다 (동기).
 * @param params  {"name":"<vm>"} — VM 이름
 * 응답: { "actual_balloon_kb", "rss_kb", "unused_kb", "available_kb",
 *         "usable_kb", "swap_in", "swap_out" }
 *
 * virDomainMemoryStats()를 사용하여 balloon 드라이버 통계를 수집합니다.
 * 조회 전 virDomainSetMemoryStatsPeriod(5초)으로 수집 주기를 활성화합니다.
 */
void handle_vm_memory_stats_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM vCPU별 상세 통계를 조회한다 (동기).
 * @param params  {"name":"<vm>"} — VM 이름
 * 응답: { "vcpu_count", "max_vcpu", "cpu_time_ns",
 *         "vcpus": [{"number", "state", "cpu_time", "cpu_affinity"}, ...] }
 *
 * virDomainGetInfo()+virDomainGetVcpus()를 사용하여 per-vCPU 통계를 수집합니다.
 */
void handle_vm_cpu_stats_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief 실행 중 VM의 블록 디바이스를 라이브 리사이즈한다 (fire-and-forget).
 * @param params  {"name":"<vm>", "target":"vda", "new_size_gb":<int>}
 *
 * ZFS zvol 리사이즈 + virDomainBlockResize()로 게스트에 새 크기 알림.
 * 응답은 즉시 "accepted"를 전송하고 실제 작업은 GTask 비동기 실행.
 */
void handle_vm_disk_live_resize_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM 블록 디바이스의 I/O 대역폭/IOPS를 제한한다 (동기).
 * @param params  {"name":"<vm>", "device":"vda",
 *                 "read_bytes_sec":104857600, "write_bytes_sec":52428800,
 *                 "read_iops_sec":1000, "write_iops_sec":500}
 *                최소 하나의 제한값이 > 0이어야 함
 * virDomainSetBlockIoTune() 사용.
 */
void handle_vm_blkio_set(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM 블록 디바이스의 현재 I/O 제한값을 조회한다 (동기).
 * @param params  {"name":"<vm>", "device":"vda"}
 * 응답: { "device", "read_bytes_sec", "write_bytes_sec",
 *         "read_iops_sec", "write_iops_sec", "total_bytes_sec", "total_iops_sec" }
 * virDomainGetBlockIoTune() 사용.
 */
void handle_vm_blkio_get(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection);

/* ── USB Device Passthrough ─────────────────────────────────────── *
 * vm.usb.attach   → handle_vm_usb_attach  : USB 호스트 디바이스 VM에 패스스루 연결
 * vm.usb.detach   → handle_vm_usb_detach  : USB 호스트 디바이스 VM에서 분리
 * vm.usb.list     → handle_vm_usb_list    : VM에 연결된 USB 디바이스 목록 조회
 * ──────────────────────────────────────────────────────────────── */

/**
 * @brief USB 호스트 디바이스를 실행 중인 VM에 패스스루 연결한다 (동기).
 * @param params  {"vm_id":"<vm>", "vendor_id":"0x1234", "product_id":"0x5678"}
 */
void handle_vm_usb_attach(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection);

/**
 * @brief USB 호스트 디바이스를 실행 중인 VM에서 분리한다 (동기).
 * @param params  {"vm_id":"<vm>", "vendor_id":"0x1234", "product_id":"0x5678"}
 */
void handle_vm_usb_detach(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM에 연결된 USB 호스트 디바이스 목록을 조회한다 (동기).
 * @param params  {"vm_id":"<vm>"}
 * 응답: JSON 배열 [{vendor_id, product_id}, ...]
 */
void handle_vm_usb_list(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif /* PURECVISOR_DISPATCHER_HANDLER_VM_HOTPLUG_H */
