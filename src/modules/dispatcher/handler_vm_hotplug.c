/**
 * @file handler_vm_hotplug.c
 * @brief VM 핫플러그 RPC 핸들러 — vCPU/메모리 조정, 디스크/NIC 장치 부착/탈착, ISO 마운트
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c -> handle_vm_set_vcpu/memory/...()
 *                                              -> libvirt virDomainSetVcpus / virDomainSetMemory
 *                                              -> libvirt virDomainAttachDevice / virDomainDetachDevice
 *
 * [처리하는 RPC 메서드] (12개)
 *   vm.set_vcpu            -> handle_vm_set_vcpu_request        : 실행 중 VM의 vCPU 수 변경 (핫플러그)
 *   vm.set_memory          -> handle_vm_set_memory_request      : 실행 중 VM의 메모리 크기 변경
 *   vm.memory.stats        -> handle_vm_memory_stats_request    : 벌룬 메모리 통계 조회
 *   vm.cpu.stats           -> handle_vm_cpu_stats_request       : per-vCPU 통계 조회
 *   vm.disk.live_resize    -> handle_vm_disk_live_resize_request: 라이브 디스크 리사이즈
 *   vm.eject               -> handle_vm_eject_iso               : CD-ROM 디스크 꺼내기
 *   vm.mount_iso           -> handle_vm_mount_iso               : ISO 파일을 CD-ROM에 삽입
 *   device.disk.attach     -> handle_device_disk_attach         : 블록 디바이스(zvol) 핫 추가
 *   device.disk.detach     -> handle_device_disk_detach         : 블록 디바이스 핫 제거
 *   device.nic.list        -> handle_device_nic_list            : VM에 연결된 NIC 목록 조회
 *   device.nic.attach      -> handle_device_nic_attach          : 가상 NIC 핫 추가
 *   device.nic.detach      -> handle_device_nic_detach          : 가상 NIC 핫 제거
 *
 * [fire-and-forget 패턴 사용 여부]
 *   - vm.set_vcpu, vm.set_memory: callback 응답 기반 비동기
 *   - vm.disk.live_resize: fire-and-forget (응답 먼저 전송 -> GTask 비동기)
 *     실제 결과는 ADR-0018에 따라 worker-result audit과 WS 완료 이벤트로 기록
 *   - device.*, vm.eject, vm.mount_iso: 동기 응답 (libvirt API가 즉시 완료)
 *
 * [주의사항]
 *   - pure_virt_get_domain()은 handler_vm_lifecycle.c에 정의된 extern 함수입니다.
 *   - NIC 핫플러그 시 OVS 브릿지이면 <virtualport type='openvswitch'/> 자동 삽입됩니다.
 *   - VmHotplugCtx 구조체의 target_value는 memory_mb 또는 vcpu_count를 겸용합니다.
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터 누락 (vm_id, vcpu_count, memory_mb 등)
 *   -32001 : 지정한 VM이 존재하지 않음
 *   -32000 : libvirt 핫플러그 작업 실패
 */
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/handler_vm_hotplug.h"
#include "modules/audit/pcv_audit.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/core/vm_state.h"   /* AF-P1: lock_vm_operation / VM_OP_TUNING */
#include "purecvisor/pcv_handler_util.h"
#include "api/ws_server.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_validate.h"

/*
 * handler_vm_lifecycle.c에 정의된 다형성 검색 함수를 재사용합니다.
 * VM 이름 또는 UUID 어느 쪽으로든 도메인을 검색할 수 있습니다.
 * 링크 시 handler_vm_lifecycle.o에서 심볼이 해결됩니다.
 */
extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);


/* =================================================================
 * 공통 컨텍스트 구조체
 *
 * vCPU/메모리 핫플러그 비동기 워커에서 사용하는 공유 컨텍스트입니다.
 * target_value는 핸들러에 따라 memory_mb 또는 vcpu_count를 저장하는
 * 범용 정수 필드입니다 (타입 안전 주의).
 *
 * [메모리 소유권]
 *   - vm_id, rpc_id: g_strdup()으로 복사 → free_hotplug_ctx()에서 g_free()
 *   - server, connection: g_object_ref() → g_object_unref()
 *   - GTask의 GDestroyNotify로 등록되어 태스크 완료 시 자동 해제
 * ================================================================= */
typedef struct {
    gchar *vm_id;           /**< VM 이름 또는 UUID 문자열 (pure_virt_get_domain에 전달) */
    gint target_value;      /**< memory_mb 또는 vcpu_count 겸용 필드 */
    gchar *rpc_id;          /**< JSON-RPC 요청 ID (응답 매칭용) */
    UdsServer *server;      /**< UDS 서버 인스턴스 (ref 카운트 증가됨) */
    GSocketConnection *connection; /**< 클라이언트 소켓 연결 (ref 카운트 증가됨) */
} VmHotplugCtx;

/**
 * free_hotplug_ctx:
 * VmHotplugCtx의 모든 필드를 안전하게 해제합니다.
 * GTask의 GDestroyNotify 콜백으로 등록되어 태스크 완료 시 자동 호출됩니다.
 */
static void free_hotplug_ctx(gpointer data) {
    if (!data) return;
    VmHotplugCtx *ctx = (VmHotplugCtx *)data;
    g_free(ctx->vm_id);
    g_free(ctx->rpc_id);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

static gboolean hotplug_get_affect_flags(virDomainPtr dom, unsigned int *flags, GError **error) {
    int active = virDomainIsActive(dom);
    if (active < 0) {
        virErrorPtr err = virGetLastError();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to read VM active state: %s",
                    err ? err->message : "Unknown");
        return FALSE;
    }

    *flags = VIR_DOMAIN_AFFECT_CONFIG;
    if (active == 1) {
        *flags |= VIR_DOMAIN_AFFECT_LIVE;
    }

    return TRUE;
}

/* =================================================================
 * 1. 메모리 핫플러그 비동기 워커
 *
 * GTask 워커 스레드에서 실행됩니다.
 * virDomainSetMemoryFlags()로 실행 중 VM의 메모리 크기를 변경합니다.
 *
 * [처리 흐름]
 *   1. libvirt 연결
 *   2. VM 이름 또는 UUID로 도메인 조회
 *   3. MB→KB 변환 후 virDomainSetMemoryFlags() 호출
 *   4. 실행 중이면 LIVE+CONFIG, 꺼져 있으면 CONFIG만 반영
 *
 * [주의] 메모리 증가는 VM 게스트에 balloon 드라이버가 설치되어 있어야 합니다.
 *        balloon 미설치 시 libvirt 에러가 발생합니다.
 * ================================================================= */
static void vm_set_memory_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmHotplugCtx *ctx = (VmHotplugCtx *)task_data;
    virConnectPtr conn = virt_conn_pool_acquire();
    
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM %s not found.", ctx->vm_id);
        return;
    }

    /* MB 단위를 KB 단위로 변환 (libvirt 내부 단위가 KB) */
    unsigned long memory_kb = (unsigned long)ctx->target_value * 1024;

    unsigned int flags = 0;
    GError *state_error = NULL;
    if (!hotplug_get_affect_flags(dom, &flags, &state_error)) {
        g_task_return_error(task, state_error);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    if (virDomainSetMemoryFlags(dom, memory_kb, flags) < 0) {
        virErrorPtr err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Memory hotplug failed: %s", err ? err->message : "Unknown");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * 2. vCPU 핫플러그 비동기 워커
 *
 * GTask 워커 스레드에서 실행됩니다.
 * virDomainSetVcpusFlags()로 실행 중 VM의 vCPU 수를 변경합니다.
 *
 * [처리 흐름]
 *   1. libvirt 연결
 *   2. VM 이름 또는 UUID로 도메인 조회
 *   3. 실행 중이면 LIVE+CONFIG, 꺼져 있으면 CONFIG만 반영
 *
 * [주의] vCPU 증가는 VM의 maxVcpus 설정 범위 내에서만 가능합니다.
 *        maxVcpus 초과 시 libvirt 에러가 발생합니다.
 * ================================================================= */
static void vm_set_vcpu_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmHotplugCtx *ctx = (VmHotplugCtx *)task_data;
    virConnectPtr conn = virt_conn_pool_acquire();
    
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM %s not found.", ctx->vm_id);
        return;
    }

    unsigned int flags = 0;
    GError *state_error = NULL;
    if (!hotplug_get_affect_flags(dom, &flags, &state_error)) {
        g_task_return_error(task, state_error);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    if (virDomainSetVcpusFlags(dom, ctx->target_value, flags) < 0) {
        virErrorPtr err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "vCPU hotplug failed: %s", err ? err->message : "Unknown");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * 3. 공통 콜백 함수
 *
 * vCPU/메모리 핫플러그 워커 완료 후 메인 스레드에서 호출됩니다.
 * 워커 결과에 따라 성공/에러 응답을 클라이언트 소켓으로 전송합니다.
 *
 * [콜백 기반 응답 패턴] (fire-and-forget이 아님)
 *   소켓은 이 콜백 시점까지 유지되어 있으므로 여기서 응답을 전송합니다.
 *   ctx 메모리는 GTask의 GDestroyNotify(free_hotplug_ctx)가 자동 해제합니다.
 * ================================================================= */
static void hotplug_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmHotplugCtx *ctx = (VmHotplugCtx *)user_data;
    GError *error = NULL;

    /* AF-P1: tuning 오퍼레이션 락 해제 — 콜백 최우선(성공/실패 무관 항상 실행).
     * set_memory/set_vcpu 진입부에서 획득한 VM_OP_TUNING 을 여기서 반드시 푼다. */
    unlock_vm_operation(ctx->vm_id);

    gboolean success = g_task_propagate_boolean(task, &error);

    if (!success) {
        /* 핫플러그 실패 — 에러 메시지를 클라이언트에 전달 */
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, -32000, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        /* 핫플러그 성공 — NULL 결과로 성공 응답 (데이터 없음) */
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, json_node_new(JSON_NODE_NULL));
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
    }
}

/* =================================================================
 * 4. 진입점 (Dispatchers)
 * ================================================================= */

/**
 * handle_vm_set_memory_request:
 * vm.set_memory RPC 진입점 — 실행 중 VM의 메모리 크기를 변경합니다.
 *
 * @param params: { "vm_id": "<UUID>", "memory_mb": <새 메모리 MB> }
 * @param rpc_id: JSON-RPC 요청 ID
 * @param server: UDS 서버 인스턴스
 * @param connection: 클라이언트 소켓 연결
 *
 * [비동기 패턴] GTask 워커에서 libvirt API 호출 → 콜백에서 응답 전송
 * [에러] vm_id 또는 memory_mb 누락 시 -32602 즉시 반환
 */
void handle_vm_set_memory_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    /* [주니어 참고] 파라미터 검증 패턴 — 모든 RPC 핸들러의 첫 번째 단계
     * 필수 파라미터가 누락되면 -32602 (Invalid params) 에러를 즉시 반환합니다.
     * 이 패턴은 JSON-RPC 2.0 표준 에러 코드를 따릅니다.
     * 비동기 워커로 진입하기 전에 반드시 검증을 완료해야 합니다. */
    if (!params || !json_object_has_member(params, "vm_id") || !json_object_has_member(params, "memory_mb")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' or 'memory_mb' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    /* [주니어 참고] 컨텍스트 초기화 순서:
     *   1. g_new0: 0으로 초기화된 힙 메모리 할당
     *   2. g_strdup: 문자열 복사 (원본 JSON 파라미터 수명과 분리)
     *   3. g_object_ref: GObject 참조 카운트 증가 (소켓이 도중에 해제되는 것 방지) */
    /* AF-P1: tuning op-lock 획득 — create/delete/start/stop 과 동일하게 라이브
     * 리소스 변경(vCPU/Memory 핫플러그)을 직렬화한다(동시 tuning, 또는 tuning 중
     * stop/delete 인터리브 차단). 해제는 완료 콜백 hotplug_callback 이 최우선으로
     * 수행(항상 실행). 락 획득 이후~task 디스패치 사이에 조기 return 경로가 없어
     * 락 누수는 발생하지 않는다. 실패 시 -32004(conflict/busy). */
    {
        const gchar *_lock_vm = json_object_get_string_member(params, "vm_id");
        gchar *lock_err = NULL;
        if (!lock_vm_operation(_lock_vm, VM_OP_TUNING, &lock_err)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, -32004,
                           lock_err ? lock_err : "VM busy (operation in progress)");
            pure_uds_server_send_response(server, connection, e);
            g_free(e); g_free(lock_err);
            return;
        }
    }
    VmHotplugCtx *ctx = g_new0(VmHotplugCtx, 1);
    ctx->vm_id = g_strdup(json_object_get_string_member(params, "vm_id"));
    ctx->target_value = json_object_get_int_member(params, "memory_mb");
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, hotplug_callback, ctx);
    g_task_set_task_data(task, ctx, free_hotplug_ctx);
    g_task_run_in_thread(task, vm_set_memory_worker);
    g_object_unref(task);
}

/**
 * handle_vm_set_vcpu_request:
 * vm.set_vcpu RPC 진입점 — 실행 중 VM의 vCPU 수를 변경합니다.
 *
 * @param params: { "vm_id": "<이름/UUID>", "vcpu_count" 또는 "vcpu" 또는 "count": <새 vCPU 수> }
 * @param rpc_id: JSON-RPC 요청 ID
 * @param server: UDS 서버 인스턴스
 * @param connection: 클라이언트 소켓 연결
 *
 * [비동기 패턴] GTask 워커에서 libvirt API 호출 → 콜백에서 응답 전송
 * [에러] vm_id 또는 vcpu_count 누락 시 -32602 즉시 반환
 */
void handle_vm_set_vcpu_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    gint vcpu_count = 0;
    gboolean has_vcpu_count = pcv_rpc_params_get_int_alias(params, "vcpu_count", "vcpu", &vcpu_count);
    if (!has_vcpu_count) {
        has_vcpu_count = pcv_rpc_params_get_int_alias(params, "count", NULL, &vcpu_count);
    }

    if (!params || !json_object_has_member(params, "vm_id") || !has_vcpu_count) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid params: 'vm_id' and one of 'vcpu_count', 'vcpu', or 'count' required");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    /* AF-P1: tuning op-lock 획득 — create/delete/start/stop 과 동일하게 라이브
     * 리소스 변경(vCPU/Memory 핫플러그)을 직렬화한다(동시 tuning, 또는 tuning 중
     * stop/delete 인터리브 차단). 해제는 완료 콜백 hotplug_callback 이 최우선으로
     * 수행(항상 실행). 락 획득 이후~task 디스패치 사이에 조기 return 경로가 없어
     * 락 누수는 발생하지 않는다. 실패 시 -32004(conflict/busy). */
    {
        const gchar *_lock_vm = json_object_get_string_member(params, "vm_id");
        gchar *lock_err = NULL;
        if (!lock_vm_operation(_lock_vm, VM_OP_TUNING, &lock_err)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, -32004,
                           lock_err ? lock_err : "VM busy (operation in progress)");
            pure_uds_server_send_response(server, connection, e);
            g_free(e); g_free(lock_err);
            return;
        }
    }
    VmHotplugCtx *ctx = g_new0(VmHotplugCtx, 1);
    ctx->vm_id = g_strdup(json_object_get_string_member(params, "vm_id"));
    ctx->target_value = vcpu_count;
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, hotplug_callback, ctx);
    g_task_set_task_data(task, ctx, free_hotplug_ctx);
    g_task_run_in_thread(task, vm_set_vcpu_worker);
    g_object_unref(task);
}

/* =================================================================
 * [API 진입점] 라이브 디스크 장착 (Attach) — device.disk.attach RPC
 *
 * 실행 중인 VM에 블록 디바이스(ZFS zvol 등)를 핫 추가합니다.
 *
 * @param params: { "vm_id": "<이름/UUID>", "source": "/dev/zvol/...", "target": "vdb" }
 * @param rpc_id: JSON-RPC 요청 ID
 *
 * [동기 응답] libvirt virDomainAttachDeviceFlags()가 즉시 완료되므로
 *             fire-and-forget 없이 동기적으로 응답합니다.
 *
 * [디스크 XML] virtio 버스, cache=none(직접 I/O), io=native(AIO 사용)
 *             → ZFS zvol에 최적화된 설정입니다.
 * ================================================================= */
void handle_device_disk_attach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    const gchar *source_dev = json_object_get_string_member(params, "source");
    const gchar *target_dev = json_object_get_string_member(params, "target");

    if (!vm_id || !source_dev || !target_dev) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602, "Missing vm_id, source, or target");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    /* [주니어 참고] PCV_REQUIRE_VIRT_CONN 매크로 패턴:
     * 이 매크로는 virt_conn_pool_acquire()로 libvirt 연결을 획득하고,
     * 실패 시 자동으로 에러 응답을 전송하고 return합니다.
     * 매크로 내부에서 conn 변수에 값을 대입하므로, 이 줄 이후에는
     * conn이 유효한 libvirt 연결임이 보장됩니다.
     * 비슷한 매크로로 PCV_REQUIRE_PARAM이 있으며, 필수 파라미터 추출에 사용됩니다. */
    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }

    /* ZVOL을 위한 블록 디바이스 XML 조립 (virtio 버스, 직접 I/O)
     * [주니어 참고] libvirt 디바이스 핫플러그 XML 옵션:
     *   cache='none'  : 호스트 OS 페이지 캐시를 우회하여 직접 I/O (ZFS ARC와 중복 캐싱 방지)
     *   io='native'   : 리눅스 native AIO(비동기 I/O) 사용 (높은 동시성 처리)
     *   bus='virtio'  : 준가상화 디스크 드라이버 (SCSI/IDE 대비 최고 성능) */
    gchar *xml_payload = g_strdup_printf(
        "<disk type='block' device='disk'>\n"
        "  <driver name='qemu' type='raw' cache='none' io='native'/>\n"
        "  <source dev='%s'/>\n"
        "  <target dev='%s' bus='virtio'/>\n"
        "</disk>", source_dev, target_dev);

    // VIR_DOMAIN_AFFECT_LIVE: 켜져 있는 상태에 즉시 반영
    // VIR_DOMAIN_AFFECT_CONFIG: 재부팅 후에도 유지되도록 설정 파일에 저장
    unsigned int flags = VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG;

    if (virDomainAttachDeviceFlags(dom, xml_payload, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, libvirt_err ? libvirt_err->message : "Attach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, json_object_new());
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }

    g_free(xml_payload);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * [API 진입점] 블록 디바이스 탈착 (Detach) — device.disk.detach RPC
 *
 * 실행 중인 VM에서 지정된 블록 디바이스를 핫 제거합니다.
 *
 * @param params: { "vm_id": "<이름/UUID>", "target": "vdb" }
 * @param rpc_id: JSON-RPC 요청 ID
 *
 * [Live XML 파싱 기반 적출 엔진]
 *   단순히 target만으로 detach하면 libvirt가 정확한 디스크를 찾지 못할 수 있습니다.
 *   따라서 VM의 Live XML에서 해당 target을 포함하는 <disk>...</disk> 블록 전체를
 *   역추적하여 추출(파싱)한 뒤, 완벽한 XML로 virDomainDetachDeviceFlags()를 호출합니다.
 *
 * [동기 응답] libvirt API가 즉시 완료되므로 fire-and-forget 미사용.
 * ================================================================= */
void handle_device_disk_detach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    const gchar *target_dev = json_object_get_string_member(params, "target");

    if (!vm_id || !target_dev) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602, "Missing vm_id or target");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }

    /* 1단계: 가동 중인 VM의 실시간(Live) XML을 가져옵니다 (플래그 0 = 메모리 상태)
     *
     * [주니어 참고] virDomainGetXMLDesc(dom, 0) 플래그 값의 의미:
     *   0                        : 현재 메모리 상태(Live) 기준 XML
     *   VIR_DOMAIN_XML_INACTIVE  : 영구 설정(Config) 기준 XML
     *   VIR_DOMAIN_XML_SECURE    : 비밀정보(비밀번호 등) 포함
     * 핫플러그 탈착에는 Live XML을 사용해야 합니다 (설정 XML과 다를 수 있음). */
    gchar *live_xml = virDomainGetXMLDesc(dom, 0);
    gchar *target_tag = g_strdup_printf("<target dev='%s'", target_dev);

    /* 2단계: XML 내부에서 타겟 디바이스(예: vdb)의 위치를 찾습니다 */
    gchar *target_pos = strstr(live_xml, target_tag);
    
    if (!target_pos) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "Device not found in live XML");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        g_free(live_xml); g_free(target_tag); virDomainFree(dom); virt_conn_pool_release(conn); return;
    }

    /* 3단계: target 위치에서 역추적하여 <disk> 시작점과 </disk> 끝점을 찾습니다
     *
     * [주니어 참고] 역추적(Backward scan) 알고리즘:
     *   target_pos(예: <target dev='vdb'>) 위치에서 한 글자씩 뒤로 이동하면서
     *   <disk 또는 <disk> 태그를 찾습니다. 이렇게 하면 해당 target을 포함하는
     *   정확한 <disk>...</disk> 블록 전체를 추출할 수 있습니다.
     *
     *   XML 파서를 쓰지 않는 이유: libvirt XML 구조가 고정적이고,
     *   하나의 디스크 블록만 추출하면 되므로 이 방식이 더 경량입니다.
     *   단, 이 기법은 XML 형식이 바뀌면 깨질 수 있어 주의가 필요합니다. */
    gchar *disk_start = target_pos;
    while (disk_start >= live_xml && strncmp(disk_start, "<disk ", 6) != 0 && strncmp(disk_start, "<disk>", 6) != 0) {
        disk_start--;
    }

    gchar *disk_end = strstr(target_pos, "</disk>");
    if (disk_end) disk_end += 7; // "</disk>" 문자열 길이 포함

    /* 추출된 완전한 <disk>...</disk> XML 블록 */
    gchar *exact_xml = g_strndup(disk_start, disk_end - disk_start);

    /*
     * 4단계: 완전한 XML로 디스크 탈착 실행
     *
     * AFFECT_LIVE만 적용 — CONFIG를 함께 적용하면 영구 설정까지 변경되어
     * 재부팅 후에도 디스크가 사라지므로, 임시 탈착 시에는 LIVE만 사용합니다.
     */
    unsigned int flags = VIR_DOMAIN_AFFECT_LIVE;
    if (virDomainDetachDeviceFlags(dom, exact_xml, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, libvirt_err ? libvirt_err->message : "Detach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, json_object_new());
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }

    /* 메모리 해제: XML 문자열, 태그, libvirt 핸들 */
    g_free(exact_xml);
    g_free(target_tag);
    g_free(live_xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * [API 진입점] ISO 마운트 — vm.mount_iso RPC
 *
 * 실행 중인 VM의 가상 CD-ROM 드라이브에 ISO 파일을 삽입합니다.
 *
 * @param params: { "vm_id": "<이름/UUID>", "iso_path": "/pcvpool/iso/ubuntu.iso" }
 * @param rpc_id: JSON-RPC 요청 ID
 *
 * [virDomainUpdateDeviceFlags 사용 이유]
 *   AttachDevice가 아닌 UpdateDevice를 사용합니다. CD-ROM 디바이스는 이미 VM XML에
 *   정의되어 있고, 미디어(source)만 변경하는 것이므로 Update가 적합합니다.
 *
 * [동기 응답] 동기적으로 처리 — ISO 마운트는 즉시 완료됩니다.
 * ================================================================= */
void handle_vm_mount_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    if ((!vm_id || *vm_id == '\0') && json_object_has_member(params, "name"))
        vm_id = json_object_get_string_member(params, "name");
    const gchar *iso_path = json_object_has_member(params, "iso_path")
        ? json_object_get_string_member(params, "iso_path") : NULL;

    if (!vm_id || *vm_id == '\0' || !iso_path || *iso_path == '\0') {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602, "Missing: vm_id or iso_path");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    if (!pcv_validate_iso_path(iso_path)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid iso_path: must be absolute, non-empty, and must not contain '..'");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    if (!g_file_test(iso_path, G_FILE_TEST_IS_REGULAR)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid iso_path: file does not exist or is not a regular file");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "VM not found");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    gchar *iso_escaped = g_markup_escape_text(iso_path, -1);
    gchar *mount_xml = g_strdup_printf(
        "<disk type='file' device='cdrom'>\n"
        "  <driver name='qemu' type='raw'/>\n"
        "  <source file='%s'/>\n"
        "  <target dev='sda' bus='sata'/>\n"
        "  <readonly/>\n"
        "</disk>", iso_escaped);

    int flags = VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG;

    if (virDomainUpdateDeviceFlags(dom, mount_xml, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            libvirt_err ? libvirt_err->message : "Failed to mount ISO");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_boolean_member(res_obj, "mounted", TRUE);
        json_object_set_string_member(res_obj, "iso_path", iso_path);
        json_node_take_object(res_node, res_obj);

        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(mount_xml);
    g_free(iso_escaped);
    virDomainFree(dom); virt_conn_pool_release(conn);
}

/* =================================================================
 * [API 진입점] ISO 사출 — vm.eject RPC
 *
 * 실행 중인 VM의 가상 CD-ROM에서 ISO 미디어를 꺼냅니다.
 *
 * @param params: { "vm_id": "<이름/UUID>" }
 * @param rpc_id: JSON-RPC 요청 ID
 *
 * [사출 원리]
 *   <source> 태그가 없는 빈 CD-ROM XML을 UpdateDevice로 전달하면
 *   libvirt가 기존 미디어를 제거하고 빈 드라이브 상태로 변경합니다.
 *   물리 CD-ROM의 eject 버튼을 누르는 것과 동일한 효과입니다.
 *
 * [동기 응답] 동기적으로 처리 — ISO 사출은 즉시 완료됩니다.
 * ================================================================= */
void handle_vm_eject_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id;
    PCV_REQUIRE_PARAM(params, "vm_id", vm_id, rpc_id, server, connection);

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }

    /*
     * <source> 태그가 없는 빈 CD-ROM XML을 정의합니다.
     * target dev='sda' bus='sata': vm.create/vm.mount_iso에서 사용하는 동일한 타겟 경로
     * <source> 태그 부재 → libvirt가 미디어 제거로 해석
     *
     * [주니어 참고] mount_iso와 eject의 차이:
     *   mount_iso: <source file='xxx.iso'/> 태그가 있는 XML → 미디어 삽입
     *   eject    : <source> 태그가 없는 XML → 미디어 제거 (빈 드라이브)
     *   둘 다 virDomainUpdateDeviceFlags()를 사용합니다.
     *   AttachDevice가 아닌 UpdateDevice인 이유: CD-ROM 장치 자체는 이미
     *   VM에 존재하고, 미디어(source)만 변경하는 것이기 때문입니다. */
    const gchar *eject_xml =
        "<disk type='file' device='cdrom'>\n"
        "  <target dev='sda' bus='sata'/>\n"
        "</disk>";

    /* AFFECT_LIVE + AFFECT_CONFIG: 즉시 사출 + 다음 부팅에도 빈 상태 유지 */
    int flags = VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG;
    
    if (virDomainUpdateDeviceFlags(dom, eject_xml, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, libvirt_err ? libvirt_err->message : "Failed to eject ISO");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        /* 성공 응답: {"ejected": true} */
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_boolean_member(res_obj, "ejected", TRUE);
        json_node_take_object(res_node, res_obj);

        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    virDomainFree(dom); virt_conn_pool_release(conn);
}
/* =================================================================
 * [NIC 핫플러그] device.nic.list / device.nic.attach / device.nic.detach
 * Sprint F — REST /vms/{n}/nics 엔드포인트 지원
 * ================================================================= */

static gchar *_pcv_xml_attr_dup(const gchar *start, const gchar *limit, const gchar *attr)
{
    if (!start || !attr) return NULL;

    gssize len = (limit && limit > start) ? (gssize)(limit - start) : -1;
    gchar *needle = g_strdup_printf("%s='", attr);
    const gchar *hit = g_strstr_len(start, len, needle);
    gchar quote = '\'';

    if (!hit) {
        g_free(needle);
        needle = g_strdup_printf("%s=\"", attr);
        hit = g_strstr_len(start, len, needle);
        quote = '"';
    }

    if (!hit) {
        g_free(needle);
        return NULL;
    }

    hit += strlen(needle);
    const gchar *end = strchr(hit, quote);
    if (!end || (limit && end > limit)) {
        g_free(needle);
        return NULL;
    }

    gchar *value = g_strndup(hit, (gsize)(end - hit));
    g_free(needle);
    return value;
}

static gboolean _pcv_mac_equal(const gchar *a, const gchar *b)
{
    return a && b && g_ascii_strcasecmp(a, b) == 0;
}

static gchar *_pcv_first_ip_from_ifaces(virDomainPtr dom, const gchar *mac,
                                        unsigned int source)
{
    virDomainInterfacePtr *ifaces = NULL;
    int count = virDomainInterfaceAddresses(dom, &ifaces, source, 0);
    gchar *fallback = NULL;
    gchar *result = NULL;

    if (count <= 0 || !ifaces)
        return NULL;

    for (int i = 0; i < count && !result; i++) {
        virDomainInterfacePtr iface = ifaces[i];
        if (!iface) continue;
        if (iface->name && g_strcmp0(iface->name, "lo") == 0) continue;
        if (mac && *mac) {
            if (!iface->hwaddr || !_pcv_mac_equal(iface->hwaddr, mac))
                continue;
        }

        for (unsigned int a = 0; a < iface->naddrs; a++) {
            virDomainIPAddressPtr addr = &iface->addrs[a];
            if (!addr->addr || !*addr->addr) continue;
            if (addr->type == VIR_IP_ADDR_TYPE_IPV4) {
                result = g_strdup(addr->addr);
                break;
            }
            if (!fallback)
                fallback = g_strdup(addr->addr);
        }
    }

    for (int i = 0; i < count; i++)
        if (ifaces[i]) virDomainInterfaceFree(ifaces[i]);
    free(ifaces);

    if (result) {
        g_free(fallback);
        return result;
    }
    return fallback;
}

static gchar *_pcv_lease_ip_for_mac(const gchar *bridge, const gchar *mac)
{
    if (!bridge || !*bridge || !mac || !*mac)
        return NULL;

    gchar *lease_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.leases", bridge);
    gchar *content = NULL;
    gchar *result = NULL;

    if (g_file_get_contents(lease_path, &content, NULL, NULL) && content) {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (guint i = 0; lines[i] && !result; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (!*line) continue;

            gchar expiry[32] = {0};
            gchar lease_mac[32] = {0};
            gchar ip[64] = {0};
            if (sscanf(line, "%31s %31s %63s", expiry, lease_mac, ip) == 3 &&
                _pcv_mac_equal(lease_mac, mac))
                result = g_strdup(ip);
        }
        g_strfreev(lines);
    }

    g_free(content);
    g_free(lease_path);
    return result;
}

static gchar *_pcv_arp_ip_for_mac(const gchar *mac)
{
    if (!mac || !*mac)
        return NULL;

    gchar *content = NULL;
    gchar *result = NULL;
    if (!g_file_get_contents("/proc/net/arp", &content, NULL, NULL) || !content)
        return NULL;

    gchar **lines = g_strsplit(content, "\n", -1);
    for (guint i = 1; lines[i] && !result; i++) {
        gchar ip[64] = {0};
        gchar hw_type[16] = {0};
        gchar flags[16] = {0};
        gchar hw_addr[32] = {0};
        gchar mask[16] = {0};
        gchar device[64] = {0};
        if (sscanf(lines[i], "%63s %15s %15s %31s %15s %63s",
                   ip, hw_type, flags, hw_addr, mask, device) == 6 &&
            _pcv_mac_equal(hw_addr, mac)) {
            result = g_strdup(ip);
        }
    }

    g_strfreev(lines);
    g_free(content);
    return result;
}

static gchar *_pcv_dns_for_bridge(const gchar *bridge)
{
    if (!bridge || !*bridge)
        return g_strdup("");

    gchar *conf_path = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf", bridge);
    gchar *content = NULL;
    gchar *result = NULL;

    if (g_file_get_contents(conf_path, &content, NULL, NULL) && content) {
        if (g_strstr_len(content, -1, "\nport=0") ||
            g_str_has_prefix(content, "port=0")) {
            result = g_strdup("off");
        } else {
            gchar **lines = g_strsplit(content, "\n", -1);
            for (guint i = 0; lines[i] && !result; i++) {
                gchar *line = g_strstrip(lines[i]);
                if (g_str_has_prefix(line, "server="))
                    result = g_strdup(line + 7);
            }
            g_strfreev(lines);
        }
    }

    g_free(content);
    g_free(conf_path);
    return result ? result : g_strdup("");
}

static gchar *_pcv_nic_ip_for_mac(virDomainPtr dom, const gchar *mac,
                                  const gchar *bridge, const gchar **source_out)
{
    int state = VIR_DOMAIN_NOSTATE;
    int reason = 0;
    gboolean running = virDomainGetState(dom, &state, &reason, 0) == 0 &&
                       state == VIR_DOMAIN_RUNNING;

    if (running) {
        gchar *ip = _pcv_first_ip_from_ifaces(dom, mac,
            VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_LEASE);
        if (ip) {
            if (source_out) *source_out = "lease";
            return ip;
        }

        ip = _pcv_first_ip_from_ifaces(dom, mac,
            VIR_DOMAIN_INTERFACE_ADDRESSES_SRC_AGENT);
        if (ip) {
            if (source_out) *source_out = "guest-agent";
            return ip;
        }
    }

    gchar *ip = _pcv_lease_ip_for_mac(bridge, mac);
    if (ip) {
        if (source_out) *source_out = "lease-file";
        return ip;
    }

    if (running) {
        ip = _pcv_arp_ip_for_mac(mac);
        if (ip) {
            if (source_out) *source_out = "arp";
            return ip;
        }
    }

    if (source_out) *source_out = "";
    return NULL;
}

/**
 * handle_device_nic_list:
 * device.nic.list RPC 진입점 — VM에 연결된 NIC 목록을 조회합니다.
 *
 * @param params: { "vm_id": "<이름/UUID>" }
 * @return: [{"mac":"52:54:...","type":"bridge","source":"pcvbr0",
 *            "bridge":"pcvbr0","model":"virtio","ip":"10.0.0.12",
 *            "ip_source":"lease","dns":"off"}, ...]
 *
 * [XML 간이 파싱]
 *   libvirt XML에서 <interface> 블록 단위로 MAC, source, model, target을
 *   추출합니다. IP는 libvirt lease, guest-agent, dnsmasq lease file, ARP
 *   순서로 보강합니다.
 *
 * [동기 응답] XML 조회가 즉시 완료되므로 fire-and-forget 미사용.
 */
void handle_device_nic_list(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602, "Missing: vm_id");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

    /* VM의 실시간 XML에서 <interface> 블록을 파싱하여 NIC 목록 추출 */
    char *xml = virDomainGetXMLDesc(dom, 0);
    JsonArray *arr = json_array_new();

    if (xml) {
        const gchar *iface = xml;
        while ((iface = strstr(iface, "<interface ")) != NULL) {
            const gchar *tag_end = strchr(iface, '>');
            const gchar *block_end = strstr(iface, "</interface>");
            if (!tag_end || !block_end)
                break;

            const gchar *mac_tag = g_strstr_len(iface, block_end - iface, "<mac ");
            const gchar *source_tag = g_strstr_len(iface, block_end - iface, "<source ");
            const gchar *model_tag = g_strstr_len(iface, block_end - iface, "<model ");
            const gchar *target_tag = g_strstr_len(iface, block_end - iface, "<target ");

            gchar *type = _pcv_xml_attr_dup(iface, tag_end, "type");
            gchar *mac = mac_tag ? _pcv_xml_attr_dup(mac_tag, block_end, "address") : NULL;
            gchar *source = NULL;
            const gchar *source_key = "";
            if (source_tag) {
                const gchar *source_end = strchr(source_tag, '>');
                if (!source_end || source_end > block_end)
                    source_end = block_end;
                const gchar *keys[] = {"bridge", "network", "dev", "port", "path", NULL};
                for (guint k = 0; keys[k] && !source; k++) {
                    source = _pcv_xml_attr_dup(source_tag, source_end, keys[k]);
                    if (source)
                        source_key = keys[k];
                }
            }
            gchar *model = model_tag ? _pcv_xml_attr_dup(model_tag, block_end, "type") : NULL;
            gchar *target = target_tag ? _pcv_xml_attr_dup(target_tag, block_end, "dev") : NULL;
            const gchar *ip_source = "";
            gchar *ip = (mac && *mac) ? _pcv_nic_ip_for_mac(dom, mac, source, &ip_source) : NULL;
            gchar *dns = (source && g_strcmp0(source_key, "bridge") == 0)
                ? _pcv_dns_for_bridge(source)
                : g_strdup("");

            JsonObject *nic = json_object_new();
            json_object_set_string_member(nic, "mac", mac ? mac : "");
            json_object_set_string_member(nic, "type", type ? type : "unknown");
            json_object_set_string_member(nic, "source", source ? source : "");
            json_object_set_string_member(nic, "source_type", source_key);
            if (g_strcmp0(source_key, "bridge") == 0)
                json_object_set_string_member(nic, "bridge", source ? source : "");
            json_object_set_string_member(nic, "model", model ? model : "virtio");
            json_object_set_string_member(nic, "target", target ? target : "");
            json_object_set_string_member(nic, "ip", ip ? ip : "");
            json_object_set_string_member(nic, "ip_source", ip_source ? ip_source : "");
            json_object_set_string_member(nic, "dns", dns ? dns : "");
            json_array_add_object_element(arr, nic);

            g_free(type);
            g_free(mac);
            g_free(source);
            g_free(model);
            g_free(target);
            g_free(ip);
            g_free(dns);

            iface = block_end + strlen("</interface>");
        }
        free(xml);  /* libvirt API 반환값은 libc free()로 해제 (g_free 아님) */
    }

    JsonNode *res = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(res, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, res);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    virDomainFree(dom); virt_conn_pool_release(conn);
}

/**
 * handle_device_nic_attach:
 * device.nic.attach RPC 진입점 — 실행 중인 VM에 가상 NIC를 핫 추가합니다.
 *
 * @param params: { "vm_id": "<이름/UUID>", "bridge": "pcvbr0" (기본 virbr0), "model": "virtio" }
 * @return: true (성공)
 *
 * [OVS 브릿지 자동 감지]
 *   OVS 브릿지인 경우 <virtualport type='openvswitch'/> 가 필요하지만,
 *   이 함수에서는 자동 삽입하지 않습니다 (vm.create에서만 OVS 감지).
 *   NIC 핫플러그 시 OVS 브릿지를 사용하려면 별도 처리가 필요합니다.
 *
 * [동기 응답] libvirt API가 즉시 완료되므로 fire-and-forget 미사용.
 */
void handle_device_nic_attach(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    const gchar *bridge = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : "virbr0";
    const gchar *model  = json_object_has_member(params, "model")
        ? json_object_get_string_member(params, "model")  : "virtio";

    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602, "Missing: vm_id");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

    gchar *xml = g_strdup_printf(
        "<interface type='bridge'>\n"
        "  <source bridge='%s'/>\n"
        "  <model type='%s'/>\n"
        "</interface>", bridge, model);

    int rc = virDomainAttachDeviceFlags(dom, xml,
                 VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    g_free(xml);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            e ? e->message : "NIC attach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(res, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
    virDomainFree(dom); virt_conn_pool_release(conn);
}

/**
 * handle_device_nic_detach:
 * device.nic.detach RPC 진입점 — MAC 주소로 VM에서 NIC를 핫 제거합니다.
 *
 * @param params: { "vm_id": "<이름/UUID>", "mac": "52:54:00:ab:cd:ef" }
 * @return: true (성공)
 *
 * [MAC 기반 탈착]
 *   libvirt는 MAC 주소만으로 NIC를 식별하여 제거할 수 있습니다.
 *   최소한의 XML (<interface type='bridge'><mac address='...'/></interface>)만
 *   전달하면 libvirt가 해당 NIC를 찾아 제거합니다.
 *
 * [동기 응답] libvirt API가 즉시 완료되므로 fire-and-forget 미사용.
 */
void handle_device_nic_detach(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    const gchar *mac   = json_object_has_member(params, "mac")
        ? json_object_get_string_member(params, "mac")   : NULL;

    if (!vm_id || !mac) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602, "Missing: vm_id or mac");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

    gchar *xml = g_strdup_printf(
        "<interface type='bridge'>\n"
        "  <mac address='%s'/>\n"
        "</interface>", mac);

    int rc = virDomainDetachDeviceFlags(dom, xml,
                 VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    g_free(xml);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            e ? e->message : "NIC detach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(res, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
    virDomainFree(dom); virt_conn_pool_release(conn);
}

/* =================================================================
 * [API 진입점] vCPU 피닝 — vm.pin_vcpu RPC
 *
 * 실행 중인 VM의 특정 vCPU를 물리 CPU 코어에 고정(pin)합니다.
 *
 * @param params: { "name": "<vm>", "vcpu": <int>, "cpuset": "4-7" }
 *   - name: VM 이름 또는 UUID (필수)
 *   - vcpu: 피닝할 vCPU 번호 (0부터 시작, 필수)
 *   - cpuset: 물리 CPU 범위 문자열 (예: "0-3", "4,5,6", "0-1,4-7", 필수)
 *
 * [cpuset 파싱 규칙]
 *   - 쉼표로 구분된 항목들로 분리
 *   - 각 항목은 단일 CPU 번호("4") 또는 범위("4-7")
 *   - 최대 CPU 수: virNodeGetInfo로 조회한 호스트 CPU 수
 *
 * [동기 응답] virDomainPinVcpu는 즉시 완료되므로 fire-and-forget 미사용.
 * ================================================================= */
void handle_vm_pin_vcpu(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!vm_id) vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;

    const gchar *cpuset = json_object_has_member(params, "cpuset")
        ? json_object_get_string_member(params, "cpuset") : NULL;

    if (!vm_id || !cpuset || !json_object_has_member(params, "vcpu")) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing: name/vm_id, vcpu, or cpuset");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gint vcpu_id = (gint)json_object_get_int_member(params, "vcpu");

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "libvirt connection unavailable");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    /* 호스트 CPU 수 조회 — cpumap 크기 결정에 필요 */
    virNodeInfo node_info;
    if (virNodeGetInfo(conn, &node_info) < 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "Failed to get host CPU info");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    /* [주니어 참고] CPU 어피니티 비트맵 구조:
     * VIR_NODEINFO_MAXCPUS: 호스트의 최대 CPU 수 (코어 x 소켓 x 스레드)
     * VIR_CPU_MAPLEN: CPU 수를 바이트 수로 변환 (8 CPU = 1바이트)
     * cpumap: 비트맵 배열 — 각 비트가 하나의 물리 CPU를 나타냅니다.
     *   예: CPU 0,1,4를 선택하면 비트맵은 00010011(2) = 0x13
     * VIR_USE_CPU(cpumap, n): n번 CPU에 해당하는 비트를 1로 설정하는 매크로 */
    int max_cpus = VIR_NODEINFO_MAXCPUS(node_info);
    int maplen = VIR_CPU_MAPLEN(max_cpus);
    unsigned char *cpumap = g_malloc0((gsize)maplen);

    /*
     * cpuset 문자열 파싱: "0-3,6,8-11" → 비트맵 변환
     * 쉼표로 항목 분리 → 각 항목에서 '-'로 범위 파싱
     */
    gboolean parse_ok = TRUE;
    gchar **parts = g_strsplit(cpuset, ",", -1);
    for (int i = 0; parts[i] && parse_ok; i++) {
        gchar *part = g_strstrip(parts[i]);
        if (strlen(part) == 0) continue;

        gchar *dash = strchr(part, '-');
        if (dash) {
            /* 범위: "4-7" */
            *dash = '\0';
            gint start = atoi(part);
            gint end   = atoi(dash + 1);
            if (start < 0 || end < start || end >= max_cpus) {
                parse_ok = FALSE;
                break;
            }
            for (gint c = start; c <= end; c++) {
                VIR_USE_CPU(cpumap, c);
            }
        } else {
            /* 단일 CPU: "4" */
            gint cpu = atoi(part);
            if (cpu < 0 || cpu >= max_cpus) {
                parse_ok = FALSE;
                break;
            }
            VIR_USE_CPU(cpumap, cpu);
        }
    }
    g_strfreev(parts);

    if (!parse_ok) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Invalid cpuset range (exceeds host CPU count or malformed)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(cpumap);
        virt_conn_pool_release(conn);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(cpumap);
        virt_conn_pool_release(conn);
        return;
    }

    /* virDomainPinVcpu: 지정 vCPU를 cpumap 비트맵에 해당하는 물리 CPU에 고정
     *
     * [주니어 참고] CPU 피닝이 필요한 이유:
     *   기본적으로 KVM은 vCPU를 임의의 물리 CPU에 스케줄링합니다.
     *   이러면 CPU 캐시(L1/L2)가 자주 무효화되어 성능이 저하됩니다.
     *   피닝으로 vCPU를 특정 물리 코어에 고정하면 캐시 히트율이 크게 향상됩니다.
     *   특히 NUMA 환경에서는 메모리 로컬리티까지 최적화할 수 있습니다. */
    int rc = virDomainPinVcpu(dom, (unsigned int)vcpu_id, cpumap, maplen);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            e ? e->message : "vCPU pinning failed");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
    } else {
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_boolean_member(res_obj, "pinned", TRUE);
        json_object_set_int_member(res_obj, "vcpu", vcpu_id);
        json_object_set_string_member(res_obj, "cpuset", cpuset);
        json_node_take_object(res_node, res_obj);

        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(cpumap);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * QoS / Bandwidth Limiting — vm.set_bandwidth
 *
 * virDomainSetInterfaceParameters()로 VM NIC의 송수신 대역폭을 제한합니다.
 * 첫 번째 네트워크 인터페이스에 적용합니다.
 *
 * [파라미터]
 *   name          : VM 이름 (필수)
 *   inbound_kbps  : 수신 대역폭 제한 KB/s (0 = 변경 안 함)
 *   outbound_kbps : 송신 대역폭 제한 KB/s (0 = 변경 안 함)
 *
 * [구현]
 *   libvirt virDomainSetInterfaceParameters()를 사용합니다.
 *   virTypedParams로 inbound.average / outbound.average를 설정합니다.
 *   AFFECT_LIVE 플래그로 실행 중 VM에 즉시 적용됩니다.
 *
 * [동기 응답] libvirt API가 즉시 완료되므로 fire-and-forget 미사용.
 * ================================================================= */
void handle_vm_set_bandwidth(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gint inbound_kbps = json_object_has_member(params, "inbound_kbps")
        ? (gint)json_object_get_int_member(params, "inbound_kbps") : 0;
    gint outbound_kbps = json_object_has_member(params, "outbound_kbps")
        ? (gint)json_object_get_int_member(params, "outbound_kbps") : 0;

    if (inbound_kbps <= 0 && outbound_kbps <= 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "At least one of inbound_kbps or outbound_kbps must be > 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    /* virsh domiflist로 첫 번째 인터페이스 이름 추출
     *
     * [주니어 참고] virsh domiflist 출력 형식:
     *   Interface   Type       Source     Model     MAC
     *   -----------------------------------------------
     *   vnet0       bridge     pcvbr0     virtio    52:54:00:...
     *
     * 첫 2줄(헤더+구분선)을 건너뛰고 3번째 줄의 첫 번째 컬럼(vnet0)을 추출합니다.
     * virDomainSetInterfaceParameters()는 이 인터페이스 이름이 필요합니다. */
    gchar *iface = NULL;
    {
        const gchar *argv2[] = {"virsh", "domiflist", name, NULL};
        gchar *stdout_buf = NULL;
        GError *spawn_err = NULL;
        if (pcv_spawn_sync(argv2, &stdout_buf, NULL, &spawn_err) && stdout_buf) {
            gchar **lines = g_strsplit(stdout_buf, "\n", -1);
            for (int i = 0; lines[i]; i++) {
                gchar *line = g_strstrip(lines[i]);
                if (i >= 2 && line[0] && line[0] != '-') {
                    gchar **cols = g_strsplit_set(line, " \t", -1);
                    if (cols[0] && cols[0][0]) {
                        iface = g_strdup(cols[0]);
                    }
                    g_strfreev(cols);
                    break;
                }
            }
            g_strfreev(lines);
        }
        g_free(stdout_buf);
        if (spawn_err) g_error_free(spawn_err);
    }

    if (!iface) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            "No network interface found on VM");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    /* [주니어 참고] virTypedParameter — libvirt의 범용 파라미터 구조체
     *
     * libvirt는 다양한 설정을 하나의 API로 전달하기 위해 "typed parameter" 패턴을 사용합니다.
     * 각 파라미터는 { field(이름 문자열), type(타입 enum), value(유니온) } 3요소로 구성됩니다.
     *
     * 주요 타입:
     *   VIR_TYPED_PARAM_UINT   : unsigned int (대역폭 KB/s 등)
     *   VIR_TYPED_PARAM_LLONG  : long long (CPU quota us 등)
     *   VIR_TYPED_PARAM_ULLONG : unsigned long long (메모리 KB 등)
     *
     * nparams는 실제로 설정된 파라미터 수를 추적합니다 (0/1/2). */
    virTypedParameter typed_params[2];
    int nparams = 0;

    if (inbound_kbps > 0) {
        strncpy(typed_params[nparams].field, "inbound.average",
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_UINT;
        typed_params[nparams].value.ui = (unsigned int)inbound_kbps;
        nparams++;
    }
    if (outbound_kbps > 0) {
        strncpy(typed_params[nparams].field, "outbound.average",
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_UINT;
        typed_params[nparams].value.ui = (unsigned int)outbound_kbps;
        nparams++;
    }

    int rc = virDomainSetInterfaceParameters(dom, iface, typed_params, nparams,
                 VIR_DOMAIN_AFFECT_LIVE);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            e ? e->message : "Failed to set bandwidth limits");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
    } else {
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "vm", name);
        json_object_set_string_member(obj, "interface", iface);
        json_object_set_int_member(obj, "inbound_kbps", inbound_kbps);
        json_object_set_int_member(obj, "outbound_kbps", outbound_kbps);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(iface);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * vm.memory.stats — Balloon Memory Statistics (동기)
 *
 * virDomainMemoryStats()로 balloon 드라이버 통계를 조회합니다.
 * 조회 전 virDomainSetMemoryStatsPeriod(5)으로 수집 주기를 활성화합니다.
 *
 * [파라미터]
 *   name : VM 이름/UUID (필수)
 *
 * [응답 필드]
 *   actual_balloon_kb, rss_kb, unused_kb, available_kb,
 *   usable_kb, swap_in, swap_out
 *
 * [동기 응답] virDomainMemoryStats()가 즉시 완료되므로 fire-and-forget 미사용.
 * ================================================================= */
void handle_vm_memory_stats_request(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    /* balloon stats 수집 주기 활성화 (5초) — 이미 활성화된 경우 무해
     *
     * [주니어 참고] virtio-balloon 드라이버란?
     *   게스트 OS 내부에서 실행되는 가상 디바이스 드라이버입니다.
     *   호스트가 게스트의 실제 메모리 사용량을 알기 위해 게스트에게 주기적으로
     *   통계를 보고하도록 요청합니다. 이 주기를 SetMemoryStatsPeriod()로 설정합니다.
     *   이 호출이 없으면 virDomainMemoryStats()가 빈 결과를 반환할 수 있습니다. */
    virDomainSetMemoryStatsPeriod(dom, 5, VIR_DOMAIN_AFFECT_LIVE);

    /* balloon 메모리 통계 조회 */
    virDomainMemoryStatStruct stats[VIR_DOMAIN_MEMORY_STAT_NR];
    int nr_stats = virDomainMemoryStats(dom, stats, VIR_DOMAIN_MEMORY_STAT_NR, 0);

    if (nr_stats < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            e ? e->message : "Failed to get memory stats");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    /* 통계를 JSON 오브젝트로 변환 (태그별 매핑) */
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm", name);

    for (int i = 0; i < nr_stats; i++) {
        switch (stats[i].tag) {
        case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON:
            json_object_set_int_member(obj, "actual_balloon_kb", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_RSS:
            json_object_set_int_member(obj, "rss_kb", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_UNUSED:
            json_object_set_int_member(obj, "unused_kb", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_AVAILABLE:
            json_object_set_int_member(obj, "available_kb", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_USABLE:
            json_object_set_int_member(obj, "usable_kb", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_SWAP_IN:
            json_object_set_int_member(obj, "swap_in", (gint64)stats[i].val);
            break;
        case VIR_DOMAIN_MEMORY_STAT_SWAP_OUT:
            json_object_set_int_member(obj, "swap_out", (gint64)stats[i].val);
            break;
        default:
            break;
        }
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * vm.cpu.stats — Per-vCPU Statistics (동기)
 *
 * virDomainGetInfo()+virDomainGetVcpus()로 vCPU별 상세 통계를 조회합니다.
 *
 * [파라미터]
 *   name : VM 이름/UUID (필수)
 *
 * [응답]
 *   vcpu_count  : 현재 vCPU 수
 *   max_vcpu    : 최대 허용 vCPU 수
 *   cpu_time_ns : 전체 CPU 사용 시간 (ns)
 *   vcpus       : [{ number, state, cpu_time, cpu_affinity }, ...]
 *
 * [동기 응답] virDomainGetVcpus()가 즉시 완료되므로 fire-and-forget 미사용.
 * ================================================================= */
void handle_vm_cpu_stats_request(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    /* 도메인 기본 정보 조회 */
    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            e ? e->message : "Failed to get domain info");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    /* 최대 vCPU 수 조회 (설정 기준) */
    int max_vcpu = virDomainGetVcpusFlags(dom,
        VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_VCPU_MAXIMUM);
    if (max_vcpu < 0) max_vcpu = info.nrVirtCpu;

    /* per-vCPU 상세 정보 조회 */
    int nr_vcpus = info.nrVirtCpu;
    virVcpuInfoPtr vcpuinfo = g_new0(virVcpuInfo, nr_vcpus);

    /* CPU affinity 맵 (호스트 CPU 수 기반)
     *
     * [주니어 참고] cpumaps는 2차원 비트맵 배열입니다:
     *   - 행(row) = 각 vCPU (0 ~ nr_vcpus-1)
     *   - 열(col) = 각 물리 CPU (0 ~ host_cpus-1)의 비트
     *   - 전체 크기 = nr_vcpus * maplen (바이트)
     *   - VIR_CPU_USABLE(cpumaps, maplen, vcpu_idx, cpu_idx) 매크로로
     *     특정 vCPU가 특정 물리 CPU에서 실행 가능한지 확인합니다. */
    int host_cpus = virNodeGetCPUMap(conn, NULL, NULL, 0);
    if (host_cpus <= 0) host_cpus = 64; /* 폴백: CPU 수 조회 실패 시 안전한 기본값 */
    int maplen = VIR_CPU_MAPLEN(host_cpus);
    unsigned char *cpumaps = g_new0(unsigned char, nr_vcpus * maplen);

    int got = virDomainGetVcpus(dom, vcpuinfo, nr_vcpus, cpumaps, maplen);

    /* JSON 응답 구성 */
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm", name);
    json_object_set_int_member(obj, "vcpu_count", nr_vcpus);
    json_object_set_int_member(obj, "max_vcpu", max_vcpu);
    json_object_set_int_member(obj, "cpu_time_ns", (gint64)info.cpuTime);

    JsonArray *arr = json_array_new();

    if (got > 0) {
        for (int i = 0; i < got; i++) {
            JsonObject *vcpu_obj = json_object_new();
            json_object_set_int_member(vcpu_obj, "number", vcpuinfo[i].number);
            json_object_set_int_member(vcpu_obj, "state", vcpuinfo[i].state);
            json_object_set_int_member(vcpu_obj, "cpu_time", (gint64)vcpuinfo[i].cpuTime);

            /* CPU affinity를 "0,1,4,5" 형식의 문자열로 변환 */
            GString *aff_str = g_string_new(NULL);
            for (int c = 0; c < host_cpus; c++) {
                if (VIR_CPU_USABLE(cpumaps, maplen, i, c)) {
                    if (aff_str->len > 0) g_string_append_c(aff_str, ',');
                    g_string_append_printf(aff_str, "%d", c);
                }
            }
            json_object_set_string_member(vcpu_obj, "cpu_affinity", aff_str->str);
            g_string_free(aff_str, TRUE);

            json_array_add_object_element(arr, vcpu_obj);
        }
    }

    json_object_set_array_member(obj, "vcpus", arr);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    g_free(vcpuinfo);
    g_free(cpumaps);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * vm.disk.live_resize — Live Block Device Resize (fire-and-forget)
 *
 * ZFS zvol 크기 변경 + virDomainBlockResize()로 게스트에 새 크기 알림.
 * 응답은 즉시 "accepted"를 전송하고, 실제 작업은 GTask 비동기 실행.
 *
 * [파라미터]
 *   name       : VM 이름 (필수)
 *   target     : 블록 디바이스 타겟 (예: "vda", 필수)
 *   new_size_gb: 새 크기 GB (필수, > 0)
 *
 * [처리 흐름]
 *   1. 파라미터 검증 → 즉시 "accepted" 응답
 *   2. GTask 워커:
 *      a. ZFS zvol 리사이즈 (zfs set volsize=NGB pcvpool/vms/<name>)
 *      b. virDomainBlockResize()로 QEMU에 블록 크기 변경 알림
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터 누락
 *   -32001 : VM 없음 (워커 내부)
 *   -32000 : 리사이즈 실패 (워커 내부)
 * ================================================================= */

/** fire-and-forget용 컨텍스트 구조체 */
typedef struct {
    gchar *vm_name;     /**< VM 이름 */
    gchar *target;      /**< 블록 디바이스 타겟 (예: "vda") */
    gint   new_size_gb; /**< 새 크기 (GB) */
} DiskLiveResizeCtx;

static void free_disk_live_resize_ctx(gpointer data) {
    if (!data) return;
    DiskLiveResizeCtx *ctx = (DiskLiveResizeCtx *)data;
    g_free(ctx->vm_name);
    g_free(ctx->target);
    g_free(ctx);
}

static void
audit_disk_live_resize_success(DiskLiveResizeCtx *ctx)
{
    gchar *target = g_strdup_printf("%s:%s", ctx->vm_name, ctx->target);
    gchar *job_id = g_strdup_printf("vm.disk.live_resize:%s", target);
    pcv_audit_log(NULL, "vm.disk.live_resize", target, "ok", 0, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.disk.live_resize",
                                     "completed", NULL);
    g_free(job_id);
    g_free(target);
}

static void
audit_disk_live_resize_failure(DiskLiveResizeCtx *ctx, const gchar *error_msg)
{
    gchar *target = g_strdup_printf("%s:%s", ctx->vm_name, ctx->target);
    gchar *job_id = g_strdup_printf("vm.disk.live_resize:%s", target);
    pcv_audit_log(NULL, "vm.disk.live_resize", target, "fail", -32000, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.disk.live_resize",
                                     "failed", error_msg ? error_msg : "unknown");
    g_free(job_id);
    g_free(target);
}

/**
 * GTask 워커: ZFS zvol 리사이즈 + virDomainBlockResize
 */
static void vm_disk_live_resize_worker(GTask *task, gpointer source_obj,
                                        gpointer task_data, GCancellable *cancellable)
{
    (void)source_obj;
    (void)cancellable;

    DiskLiveResizeCtx *ctx = (DiskLiveResizeCtx *)task_data;

    /* 1단계: ZFS zvol 리사이즈 (실패해도 계속 — qcow2 환경일 수 있음) */
    gchar *volsize_arg = g_strdup_printf("volsize=%dG", ctx->new_size_gb);
    gchar *zvol_path = g_strdup_printf("pcvpool/vms/%s", ctx->vm_name);
    const gchar *zfs_argv[] = {"zfs", "set", volsize_arg, zvol_path, NULL};
    GError *zfs_err = NULL;
    pcv_spawn_sync(zfs_argv, NULL, NULL, &zfs_err);
    if (zfs_err) g_error_free(zfs_err);
    g_free(volsize_arg);
    g_free(zvol_path);

    /* 2단계: libvirt에 블록 디바이스 크기 변경 알림 */
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        audit_disk_live_resize_failure(ctx, "Failed to connect to libvirt");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Failed to connect to libvirt");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_name);
    if (!dom) {
        gchar *msg = g_strdup_printf("VM '%s' not found", ctx->vm_name);
        audit_disk_live_resize_failure(ctx, msg);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "VM '%s' not found", ctx->vm_name);
        g_free(msg);
        return;
    }

    unsigned long long new_size_bytes =
        (unsigned long long)ctx->new_size_gb * 1024ULL * 1024ULL * 1024ULL;

    if (virDomainBlockResize(dom, ctx->target, new_size_bytes, 0) < 0) {
        virErrorPtr e = virGetLastError();
        const gchar *err_msg = e ? e->message : "Unknown";
        audit_disk_live_resize_failure(ctx, err_msg);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Block resize failed: %s", err_msg);
    } else {
        audit_disk_live_resize_success(ctx);
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

void handle_vm_disk_live_resize_request(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *target = json_object_has_member(params, "target")
        ? json_object_get_string_member(params, "target") : NULL;
    gint new_size_gb = json_object_has_member(params, "new_size_gb")
        ? (gint)json_object_get_int_member(params, "new_size_gb") : 0;

    if (!name || !target || new_size_gb <= 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing or invalid params: name, target required, new_size_gb must be > 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    /* fire-and-forget: 즉시 "accepted" 응답 전송
     *
     * [주니어 참고] fire-and-forget과 콜백 기반 응답의 차이:
     *   1. 콜백 기반: 워커 완료 후 콜백에서 결과를 소켓으로 전송 (클라이언트가 대기)
     *   2. fire-and-forget: 여기서 즉시 응답 전송 후, 워커는 백그라운드 실행
     *      - GTask 생성 시 콜백을 NULL로 전달하면 결과가 버려짐
     *      - 시간이 오래 걸리는 작업(ZFS 리사이즈 등)에 적합 */
    JsonNode *accepted = json_node_new(JSON_NODE_OBJECT);
    JsonObject *accepted_obj = json_object_new();
    json_object_set_string_member(accepted_obj, "status", "accepted");
    json_object_set_string_member(accepted_obj, "vm", name);
    json_object_set_string_member(accepted_obj, "target", target);
    json_object_set_int_member(accepted_obj, "new_size_gb", new_size_gb);
    json_node_take_object(accepted, accepted_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, accepted);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    /* GTask 비동기 실행 (콜백 없음 — fire-and-forget) */
    DiskLiveResizeCtx *ctx = g_new0(DiskLiveResizeCtx, 1);
    ctx->vm_name = g_strdup(name);
    ctx->target = g_strdup(target);
    ctx->new_size_gb = new_size_gb;

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, free_disk_live_resize_ctx);
    g_task_run_in_thread(task, vm_disk_live_resize_worker);
    g_object_unref(task);
}

/* =================================================================
 * vm.blkio.set — 블록 디바이스 I/O 스로틀링 설정 (동기)
 *
 * virDomainSetBlockIoTune()으로 per-VM 디스크 대역폭/IOPS를 제한합니다.
 *
 * [파라미터]
 *   name            : VM 이름/UUID (필수)
 *   device          : 블록 디바이스 타겟 (필수, 예: "vda")
 *   read_bytes_sec  : 읽기 대역폭 제한 (bytes/sec, 선택, 0=무제한)
 *   write_bytes_sec : 쓰기 대역폭 제한 (bytes/sec, 선택, 0=무제한)
 *   read_iops_sec   : 읽기 IOPS 제한 (선택, 0=무제한)
 *   write_iops_sec  : 쓰기 IOPS 제한 (선택, 0=무제한)
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터 누락 또는 제한값 없음
 *   -32001 : VM 미존재
 *   -32000 : libvirt 작업 실패
 * ================================================================= */
void handle_vm_blkio_set(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *device = json_object_has_member(params, "device")
        ? json_object_get_string_member(params, "device") : NULL;

    if (!name || !device) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing required parameters: name, device");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gint64 read_bytes_sec = json_object_has_member(params, "read_bytes_sec")
        ? json_object_get_int_member(params, "read_bytes_sec") : 0;
    gint64 write_bytes_sec = json_object_has_member(params, "write_bytes_sec")
        ? json_object_get_int_member(params, "write_bytes_sec") : 0;
    gint64 read_iops_sec = json_object_has_member(params, "read_iops_sec")
        ? json_object_get_int_member(params, "read_iops_sec") : 0;
    gint64 write_iops_sec = json_object_has_member(params, "write_iops_sec")
        ? json_object_get_int_member(params, "write_iops_sec") : 0;

    if (read_bytes_sec <= 0 && write_bytes_sec <= 0 &&
        read_iops_sec <= 0 && write_iops_sec <= 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "At least one I/O limit (read/write bytes_sec or iops_sec) must be > 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    /* Build virTypedParameter array for block I/O tune */
    virTypedParameter typed_params[4];
    int nparams = 0;

    if (read_bytes_sec > 0) {
        strncpy(typed_params[nparams].field, VIR_DOMAIN_BLOCK_IOTUNE_READ_BYTES_SEC,
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_ULLONG;
        typed_params[nparams].value.ul = (unsigned long long)read_bytes_sec;
        nparams++;
    }
    if (write_bytes_sec > 0) {
        strncpy(typed_params[nparams].field, VIR_DOMAIN_BLOCK_IOTUNE_WRITE_BYTES_SEC,
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_ULLONG;
        typed_params[nparams].value.ul = (unsigned long long)write_bytes_sec;
        nparams++;
    }
    if (read_iops_sec > 0) {
        strncpy(typed_params[nparams].field, VIR_DOMAIN_BLOCK_IOTUNE_READ_IOPS_SEC,
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_ULLONG;
        typed_params[nparams].value.ul = (unsigned long long)read_iops_sec;
        nparams++;
    }
    if (write_iops_sec > 0) {
        strncpy(typed_params[nparams].field, VIR_DOMAIN_BLOCK_IOTUNE_WRITE_IOPS_SEC,
                VIR_TYPED_PARAM_FIELD_LENGTH);
        typed_params[nparams].type = VIR_TYPED_PARAM_ULLONG;
        typed_params[nparams].value.ul = (unsigned long long)write_iops_sec;
        nparams++;
    }

    int rc = virDomainSetBlockIoTune(dom, device, typed_params, nparams,
                 VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            e ? e->message : "Failed to set block I/O tune");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
    } else {
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "vm", name);
        json_object_set_string_member(obj, "device", device);
        json_object_set_int_member(obj, "read_bytes_sec", read_bytes_sec);
        json_object_set_int_member(obj, "write_bytes_sec", write_bytes_sec);
        json_object_set_int_member(obj, "read_iops_sec", read_iops_sec);
        json_object_set_int_member(obj, "write_iops_sec", write_iops_sec);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * vm.blkio.get — 블록 디바이스 I/O 스로틀링 조회 (동기)
 *
 * virDomainGetBlockIoTune()으로 현재 설정된 I/O 제한값을 반환합니다.
 *
 * [파라미터]
 *   name   : VM 이름/UUID (필수)
 *   device : 블록 디바이스 타겟 (필수, 예: "vda")
 *
 * [응답 필드]
 *   device, total_bytes_sec, read_bytes_sec, write_bytes_sec,
 *   total_iops_sec, read_iops_sec, write_iops_sec
 * ================================================================= */
void handle_vm_blkio_get(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *device = json_object_has_member(params, "device")
        ? json_object_get_string_member(params, "device") : NULL;

    if (!name || !device) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing required parameters: name, device");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    /* First call: get the number of parameters */
    int nparams = 0;
    if (virDomainGetBlockIoTune(dom, device, NULL, &nparams, 0) < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            e ? e->message : "Failed to query block I/O tune params count");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    if (nparams <= 0) {
        /* No params available — return empty result */
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "device", device);
        json_object_set_string_member(obj, "status", "no_iotune_params");
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    virTypedParameterPtr blk_params = g_new0(virTypedParameter, nparams);

    int rc = virDomainGetBlockIoTune(dom, device, blk_params, &nparams, 0);
    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
            e ? e->message : "Failed to get block I/O tune");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(blk_params);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    /* Parse typed params into JSON */
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "vm", name);
    json_object_set_string_member(obj, "device", device);

    for (int i = 0; i < nparams; i++) {
        if (blk_params[i].type == VIR_TYPED_PARAM_ULLONG) {
            json_object_set_int_member(obj, blk_params[i].field,
                                        (gint64)blk_params[i].value.ul);
        } else if (blk_params[i].type == VIR_TYPED_PARAM_UINT) {
            json_object_set_int_member(obj, blk_params[i].field,
                                        (gint64)blk_params[i].value.ui);
        } else if (blk_params[i].type == VIR_TYPED_PARAM_LLONG) {
            json_object_set_int_member(obj, blk_params[i].field,
                                        blk_params[i].value.l);
        }
    }

    g_free(blk_params);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/* =================================================================
 * [USB Passthrough] vm.usb.attach / vm.usb.detach / vm.usb.list
 *
 * USB 호스트 디바이스를 실행 중인 VM에 패스스루로 연결/분리/조회합니다.
 * vendor_id, product_id로 디바이스를 식별합니다 (예: "0x1234", "0x5678").
 *
 * [동기 응답] libvirt API가 즉시 완료되므로 fire-and-forget 미사용.
 * ================================================================= */

/**
 * pcv_validate_usb_id:
 * USB vendor/product ID 형식 검증 (0x0000~0xFFFF 16진수).
 * "0x" 프리픽스 필수, 4자리 16진수.
 */
static gboolean pcv_validate_usb_id(const gchar *id) {
    if (!id || strlen(id) != 6) return FALSE;
    if (id[0] != '0' || (id[1] != 'x' && id[1] != 'X')) return FALSE;
    for (int i = 2; i < 6; i++) {
        if (!g_ascii_isxdigit(id[i])) return FALSE;
    }
    return TRUE;
}

/**
 * handle_vm_usb_attach:
 * vm.usb.attach RPC 진입점 — USB 호스트 디바이스를 VM에 패스스루 연결.
 *
 * @param params: { "vm_id": "<이름/UUID>", "vendor_id": "0x1234", "product_id": "0x5678" }
 *
 * [hostdev XML]
 *   <hostdev mode='subsystem' type='usb'>
 *     <source>
 *       <vendor id='0x1234'/>
 *       <product id='0x5678'/>
 *     </source>
 *   </hostdev>
 */
void handle_vm_usb_attach(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    const gchar *vendor_id = json_object_has_member(params, "vendor_id")
        ? json_object_get_string_member(params, "vendor_id") : NULL;
    const gchar *product_id = json_object_has_member(params, "product_id")
        ? json_object_get_string_member(params, "product_id") : NULL;

    if (!vm_id || !vendor_id || !product_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing required parameters: vm_id, vendor_id, product_id");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    if (!pcv_validate_usb_id(vendor_id) || !pcv_validate_usb_id(product_id)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "vendor_id and product_id must be hex format: 0xNNNN");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

    gchar *xml = g_strdup_printf(
        "<hostdev mode='subsystem' type='usb'>\n"
        "  <source>\n"
        "    <vendor id='%s'/>\n"
        "    <product id='%s'/>\n"
        "  </source>\n"
        "</hostdev>", vendor_id, product_id);

    unsigned int flags = VIR_DOMAIN_AFFECT_LIVE;
    if (virDomainAttachDeviceFlags(dom, xml, flags) < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000,
            e ? e->message : "USB attach failed");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_boolean_member(res_obj, "attached", TRUE);
        json_object_set_string_member(res_obj, "vendor_id", vendor_id);
        json_object_set_string_member(res_obj, "product_id", product_id);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *usb_resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, usb_resp);
        g_free(usb_resp);
    }

    g_free(xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/**
 * handle_vm_usb_detach:
 * vm.usb.detach RPC 진입점 — USB 호스트 디바이스를 VM에서 분리.
 *
 * @param params: { "vm_id": "<이름/UUID>", "vendor_id": "0x1234", "product_id": "0x5678" }
 */
void handle_vm_usb_detach(JsonObject *params, const gchar *rpc_id,
                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    const gchar *vendor_id = json_object_has_member(params, "vendor_id")
        ? json_object_get_string_member(params, "vendor_id") : NULL;
    const gchar *product_id = json_object_has_member(params, "product_id")
        ? json_object_get_string_member(params, "product_id") : NULL;

    if (!vm_id || !vendor_id || !product_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing required parameters: vm_id, vendor_id, product_id");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    if (!pcv_validate_usb_id(vendor_id) || !pcv_validate_usb_id(product_id)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "vendor_id and product_id must be hex format: 0xNNNN");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

    gchar *xml = g_strdup_printf(
        "<hostdev mode='subsystem' type='usb'>\n"
        "  <source>\n"
        "    <vendor id='%s'/>\n"
        "    <product id='%s'/>\n"
        "  </source>\n"
        "</hostdev>", vendor_id, product_id);

    unsigned int flags = VIR_DOMAIN_AFFECT_LIVE;
    if (virDomainDetachDeviceFlags(dom, xml, flags) < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000,
            e ? e->message : "USB detach failed");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_boolean_member(res_obj, "detached", TRUE);
        json_object_set_string_member(res_obj, "vendor_id", vendor_id);
        json_object_set_string_member(res_obj, "product_id", product_id);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *usb_resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, usb_resp);
        g_free(usb_resp);
    }

    g_free(xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

/**
 * handle_vm_usb_list:
 * vm.usb.list RPC 진입점 — VM에 연결된 USB 호스트 디바이스 목록 조회.
 *
 * @param params: { "vm_id": "<이름/UUID>" }
 * @return: [{"vendor_id": "0x1234", "product_id": "0x5678"}, ...]
 *
 * [XML 간이 파싱]
 *   libvirt XML에서 <hostdev ... type='usb'> 블록 내의
 *   <vendor id='...'/> 와 <product id='...'/> 태그를 추출합니다.
 */
void handle_vm_usb_list(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;

    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602, "Missing: vm_id");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

    char *dom_xml = virDomainGetXMLDesc(dom, 0);
    JsonArray *arr = json_array_new();

    if (dom_xml) {
        /* 간이 XML 파싱: <hostdev ... type='usb'> 블록 내
         * vendor/product id 추출.
         * 상태 머신: in_usb_hostdev 플래그로 USB hostdev 블록 추적 */
        gchar **lines = g_strsplit(dom_xml, "\n", -1);
        gboolean in_usb_hostdev = FALSE;
        gchar *cur_vendor = NULL;
        gchar *cur_product = NULL;

        for (int i = 0; lines[i]; i++) {
            gchar *l = g_strstrip(lines[i]);

            if (strstr(l, "<hostdev") && strstr(l, "type='usb'")) {
                in_usb_hostdev = TRUE;
                g_free(cur_vendor);  cur_vendor = NULL;
                g_free(cur_product); cur_product = NULL;
            } else if (in_usb_hostdev && strstr(l, "<vendor id=")) {
                gchar *v = strstr(l, "id='");
                if (v) cur_vendor = g_strndup(v + 4, strcspn(v + 4, "'"));
            } else if (in_usb_hostdev && strstr(l, "<product id=")) {
                gchar *p = strstr(l, "id='");
                if (p) cur_product = g_strndup(p + 4, strcspn(p + 4, "'"));
            } else if (in_usb_hostdev && strstr(l, "</hostdev>")) {
                if (cur_vendor && cur_product) {
                    JsonObject *usb = json_object_new();
                    json_object_set_string_member(usb, "vendor_id", cur_vendor);
                    json_object_set_string_member(usb, "product_id", cur_product);
                    json_array_add_object_element(arr, usb);
                }
                g_free(cur_vendor);  cur_vendor = NULL;
                g_free(cur_product); cur_product = NULL;
                in_usb_hostdev = FALSE;
            }
        }
        g_free(cur_vendor);
        g_free(cur_product);
        g_strfreev(lines);
        free(dom_xml);  /* libvirt 반환값은 libc free() */
    }

    JsonNode *res = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(res, arr);
    gchar *usb_list_resp = pure_rpc_build_success_response(rpc_id, res);
    pure_uds_server_send_response(server, connection, usb_list_resp);
    g_free(usb_list_resp);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}
