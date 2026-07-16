/**
 * @file handler_vm_lifecycle.h
 * @brief VM 라이프사이클 RPC 핸들러 공개 인터페이스
 *
 * ──────────────────────────────────────────────────────────────
 * [아키텍처 내 위치]
 *   dispatcher.c의 else-if 라우팅 체인에서 vm.list, vm.stop, vm.pause,
 *   vm.resume, vm.delete, vm.metrics 메서드를 이 파일의 핸들러로 전달한다.
 *   각 핸들러는 libvirt API(vm_manager.c)를 호출하여 VM 상태를 제어한다.
 *
 * [RPC 메서드 매핑]
 *   vm.list    -> handle_vm_list_request     (동기 — 전체 VM 목록 JSON 배열 반환)
 *   vm.stop    -> handle_vm_stop_request     (콜백 기반 비동기 — virDomainDestroy 후 콜백에서 결과 응답)
 *   vm.pause   -> handle_vm_pause_request    (콜백 기반 비동기 — virDomainSuspend 후 콜백에서 결과 응답)
 *   vm.resume  -> handle_vm_resume_request   (콜백 기반 비동기 — virDomainResume 후 콜백에서 결과 응답)
 *   vm.delete  -> handle_vm_delete_request   (fire-and-forget — VM 삭제 + zvol 정리)
 *   vm.metrics -> handle_vm_metrics_request  (동기 — 단일 VM CPU/MEM/DISK 메트릭)
 *
 * [핸들러 시그니처] (모든 디스패처 핸들러 공통)
 *   void handler(JsonObject *params, const gchar *rpc_id,
 *                UdsServer *server, GSocketConnection *connection)
 *
 * [응답 패턴 주의]
 *   vm.delete만 fire-and-forget: 즉시 "accepted" 응답 후 소켓을 닫고 GTask에서
 *   실제 삭제 수행 → 완료 콜백(_vm_delete_callback)에서 send_response 호출 금지
 *   (소켓이 이미 닫혀 있어 크래시(UB) 발생!).
 *   vm.stop/pause/resume은 콜백 기반 비동기: 응답을 미리 보내지 않고 GTask 실행,
 *   완료 콜백(vm_action_callback)에서 실제 결과 응답을 전송한다(소켓 유지).
 *
 * [추가 참고]
 *   - 이 파일의 구현체(handler_vm_lifecycle.c)에는 pure_virt_get_domain() 함수가
 *     정의되어 있어, 다른 핸들러 모듈(handler_snapshot.c 등)에서 extern으로 참조
 *   - vm.limit, vm.vnc는 dispatcher.c 인라인 핸들러로 처리되며 이 헤더에 미포함
 *   - vm.create, vm.start는 각각 dispatcher.c 인라인, handler_vm_start.h에서 처리
 * ──────────────────────────────────────────────────────────────
 */
#ifndef PURECVISOR_DISPATCHER_HANDLER_VM_LIFECYCLE_H
#define PURECVISOR_DISPATCHER_HANDLER_VM_LIFECYCLE_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

/**
 * @brief 전체 VM 목록을 JSON 배열로 반환한다 (동기).
 *
 * libvirt의 virConnectListAllDomains()를 호출하여 모든 정의된/실행 중인 VM 정보를
 * 수집한다. 각 VM의 이름, 상태, UUID, vCPU, 메모리 등을 포함하는 JSON 배열을 응답.
 *
 * @param params     JSON-RPC params 객체 (이 핸들러에서는 무시 — 파라미터 없음)
 * @param rpc_id     JSON-RPC 요청 ID (응답에 동일 ID를 포함하여 반환)
 * @param server     UDS 서버 인스턴스 (응답 전송에 사용)
 * @param connection 클라이언트 소켓 연결 (응답 전송 후 닫힘)
 */
void handle_vm_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM을 강제 종료한다 — 콜백 기반 비동기 (virDomainDestroy).
 * @param params  {"name": "<vm_name>"} — 종료할 VM 이름 (필수)
 */
void handle_vm_stop_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM을 일시 정지한다 — 콜백 기반 비동기 (virDomainSuspend).
 * @param params  {"name": "<vm_name>"} — 일시 정지할 VM 이름 (필수)
 */
void handle_vm_pause_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief 일시 정지된 VM을 재개한다 — 콜백 기반 비동기 (virDomainResume).
 * @param params  {"name": "<vm_name>"} — 재개할 VM 이름 (필수)
 */
void handle_vm_resume_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM을 삭제한다 — fire-and-forget 비동기 (virDomainUndefine + zvol destroy).
 *
 * VM이 실행 중이면 먼저 강제 종료(destroy) 후 undefine한다.
 * ZFS zvol이 있으면 함께 삭제한다. etcd에서 VM XML 메타데이터도 제거.
 *
 * @param params  {"name": "<vm_name>"} — 삭제할 VM 이름 (필수)
 */
void handle_vm_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief 단일 VM의 메트릭(CPU%, MEM, DISK I/O)을 반환한다 (동기).
 *
 * virDomainGetInfo()와 virConnectGetAllDomainStats()를 사용하여
 * 실시간 VM 리소스 사용량을 조회한다.
 *
 * @param params  {"name": "<vm_name>"} — 조회할 VM 이름 (필수)
 */
void handle_vm_metrics_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief 정지된 VM의 이름과 표준 스토리지 경로를 함께 변경한다 (동기).
 *
 * 실행 중 VM은 거부한다. 표준 ZFS zvol(`/dev/zvol/<parent>/<vm>`) 또는
 * 표준 파일 디스크(`<vm>.qcow2|raw|img`)만 지원한다.
 *
 * @param params  {"name": "<old_name>", "new_name": "<new_name>"} — 기존/새 VM 이름
 */
void handle_vm_rename_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/* =================================================================
 * Guest Agent 연동 RPC 핸들러
 *
 * qemu-guest-agent를 통해 게스트 OS와 직접 통신합니다.
 * virDomainQemuAgentCommand() API를 사용하며, libvirt-qemu 링크가 필요합니다.
 *
 * [전제조건] VM이 running 상태이고 qemu-guest-agent가 설치/실행 중이어야 합니다.
 * ================================================================= */

/**
 * @brief qemu-guest-agent 응답 여부를 확인한다 (동기).
 * @param params  {"name": "<vm_name>"} — 확인할 VM 이름 (필수)
 */
void handle_vm_guest_ping_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief qemu-guest-agent channel/응답 상태를 진단한다.
 * @param params  {"name": "<vm_name>"} — 확인할 VM 이름 (필수)
 */
void handle_vm_guest_agent_status_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief VM XML에 qemu-guest-agent channel을 추가한다.
 * @param params  {"name": "<vm_name>"} — 보정할 VM 이름 (필수)
 */
void handle_vm_guest_agent_ensure_channel_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief qemu-guest-agent의 guest-get-fsinfo로 게스트 파일시스템 사용량을 조회한다.
 * @param params  {"name": "<vm_name>"} — 조회할 VM 이름 (필수)
 */
void handle_vm_guest_fsinfo_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief 게스트 OS에서 명령을 실행하고 결과를 반환한다 (동기).
 * @param params  {"name": "<vm_name>", "command": "<shell_command>"} — VM 이름 + 실행할 명령 (필수)
 */
void handle_vm_guest_exec_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

/**
 * @brief 게스트 에이전트를 통해 VM을 정상 종료한다 (동기, ACPI 폴백).
 * @param params  {"name": "<vm_name>"} — 종료할 VM 이름 (필수)
 */
void handle_vm_guest_shutdown_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

G_END_DECLS

#endif /* PURECVISOR_DISPATCHER_HANDLER_VM_LIFECYCLE_H */
