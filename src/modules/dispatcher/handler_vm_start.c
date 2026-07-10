/**
 * @file handler_vm_start.c
 * @brief VM 시작(vm.start) RPC 핸들러 — NUMA CPU 할당 + libvirt 도메인 기동
 *
 * [아키텍처 위치]
 *   클라이언트 -> UDS/REST -> dispatcher.c ("vm.start") -> handle_vm_start_request()
 *                                                            -> cpu_allocator (NUMA 코어 할당)
 *                                                            -> libvirt virDomainCreate
 *
 * [처리하는 RPC 메서드]
 *   vm.start  (params: { "vm_id": "<이름 또는 UUID>" })
 *     - VM을 시작하고, NUMA 인식 CPU 할당기(cpu_allocator.c)를 통해
 *       배타적 물리 코어를 할당한 뒤 virDomainPinVcpu로 바인딩합니다.
 *
 * [fire-and-forget 패턴 사용]
 *   1. 즉시 {"status":"accepted"} 응답을 UDS 소켓으로 전송 (소켓 닫힘)
 *   2. GTask 워커 스레드에서 libvirt 도메인 기동 + CPU 핀닝 수행
 *   3. 결과는 journalctl 로그에만 기록 (콜백에서 send_response 호출 금지)
 *
 * [주의사항]
 *   - pure_virt_get_domain()은 handler_vm_lifecycle.c에 구현된 extern 함수로,
 *     VM 이름 또는 UUID 모두로 도메인을 검색합니다.
 *   - VmStartContext 구조체는 GTask에 전달되며, free_vm_start_context()로 해제됩니다.
 *   - server/connection은 g_object_ref로 참조 카운트 증가 후 컨텍스트에 저장합니다.
 *   - vm_state.c의 오퍼레이션 잠금으로 동시 start/stop 경합을 방지합니다.
 *
 * [에러 코드]
 *   -32602 : 필수 파라미터(vm_id) 누락
 *   -32000 : libvirt 도메인 기동 실패 또는 CPU 할당 실패
 */
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/core/vm_state.h"
#include "modules/core/cpu_allocator.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/audit/pcv_audit.h"
#include "api/ws_server.h"
#include "../network/security_group.h"

/*
 * handler_vm_lifecycle.c에 정의된 다형성 검색 함수를 extern으로 연결합니다.
 * pure_virt_get_domain()은 UUID 또는 이름으로 VM을 검색하는 공용 함수입니다.
 * 링크 시 handler_vm_lifecycle.o에서 심볼이 해결됩니다.
 */
extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

/** virDomainPinVcpuFlags에 전달할 CPU 맵 최대 크기 (물리 CPU 256개까지 지원) */
#define MAX_PHYSICAL_CPUS 256

/**
 * VmStartContext:
 * GTask 비동기 작업에 전달되는 컨텍스트 구조체입니다.
 *
 * [fire-and-forget 패턴] 이 구조체는:
 *   1. handle_vm_start_request()에서 할당 및 초기화
 *   2. "accepted" 응답 전송 후 GTask에 전달
 *   3. vm_start_worker_thread()에서 사용 (워커 스레드)
 *   4. vm_start_callback()에서 결과 로깅 (메인 스레드)
 *   5. free_vm_start_context()로 자동 해제 (GDestroyNotify)
 *
 * [메모리 소유권] server/connection은 g_object_ref()로 참조 카운트 증가.
 * allocated_cpus는 cpu_allocator가 반환한 GArray (NULL일 수 있음 = 핀닝 없이 시작).
 */
typedef struct {
    gchar *vm_id;               /**< VM 이름 또는 UUID */
    gchar *bridge_name;         /**< 네트워크 브릿지 이름 (핫플러그용, 빈 문자열이면 미사용) */
    GArray *allocated_cpus;     /**< NUMA 인식 CPU 할당 결과 (NULL이면 핀닝 생략) */
    gint numa_node;             /**< 할당된 NUMA 노드 번호 (-1이면 NUMA 바인딩 생략) */
    gchar *rpc_id;              /**< JSON-RPC 요청 ID */
    UdsServer *server;          /**< UDS 서버 인스턴스 (ref 카운트 증가됨) */
    GSocketConnection *connection; /**< 클라이언트 소켓 연결 (ref 카운트 증가됨) */
    gint64 worker_start_us;     /**< ADR-0018: 워커 시작 시각 — audit duration 계산용 */
} VmStartContext;

/**
 * free_vm_start_context:
 * VmStartContext의 모든 필드를 안전하게 해제합니다.
 * GTask의 GDestroyNotify로 등록되어 태스크 완료 시 자동 호출됩니다.
 */
static void free_vm_start_context(gpointer data) {
    if (!data) return;
    VmStartContext *ctx = (VmStartContext *)data;
    g_free(ctx->vm_id);
    g_free(ctx->bridge_name);
    g_free(ctx->rpc_id);
    if (ctx->allocated_cpus) g_array_unref(ctx->allocated_cpus);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

/**
 * vm_start_worker_thread:
 * GTask 워커 스레드에서 실행되는 VM 시작 작업입니다.
 *
 * [실행 순서]
 *   1. libvirt 연결
 *   2. pure_virt_get_domain()으로 VM 검색 (UUID 또는 이름)
 *   3. virDomainCreate()로 VM 기동 (수십 초 블로킹 가능)
 *   4. NUMA CPU 핀닝 (allocated_cpus가 있는 경우)
 *   5. 네트워크 브릿지 핫플러그 (bridge_name이 있는 경우)
 *
 * [fire-and-forget 패턴]
 *   이 워커가 실행되기 전에 "accepted" 응답은 이미 전송되었습니다.
 *   따라서 여기서 에러가 발생해도 클라이언트에게 직접 알릴 수 없고,
 *   g_task_return_error()를 통해 콜백에서 로그만 남깁니다.
 *
 * [goto 패턴] cleanup_dom/cleanup_conn 레이블로 자원 해제를 일원화합니다.
 */
static void vm_start_worker_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    VmStartContext *ctx = (VmStartContext *)task_data;
    GError *error = NULL;

    /* 1단계: libvirt 하이퍼바이저에 연결 */
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt daemon.");
        g_task_return_error(task, error);
        return;
    }

    /* 2단계: 다형성 검색 — UUID 또는 이름으로 VM 조회 */
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Entity '%s' not found.", ctx->vm_id);
        goto cleanup_conn;
    }

    /* [W3 fix] 이미 실행 중이면 idempotent 성공 — virDomainCreate 재호출 방지
       (libvirt는 실행 중 도메인에 Create 호출 시 'Requested operation is not valid'
        에러를 반환하지만, 사용자 의도는 "원하는 상태로 만들어라"이므로 그대로 성공 처리) */
    {
        virDomainInfo info;
        if (virDomainGetInfo(dom, &info) == 0 &&
            (info.state == VIR_DOMAIN_RUNNING || info.state == VIR_DOMAIN_BLOCKED)) {
            g_message("[vm.start] VM '%s': already running (idempotent no-op)", ctx->vm_id);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            /* 데몬 재시작 후 이미 떠 있는 VM 의 SG 디스패치 재동기화 (워커 스레드). */
            pcv_security_group_sync_vm(ctx->vm_id);
            g_task_return_boolean(task, TRUE);
            return;
        }
    }

    /* 2.5단계: NUMA 메모리 바인딩 — <numatune> XML 패치
     *
     * CPU 할당기가 모든 코어를 같은 NUMA 노드에서 할당한 경우(numa_node >= 0),
     * 도메인 XML에 <numatune> 엘리먼트를 삽입하여 VM 메모리도 같은 NUMA 노드에
     * 바인딩합니다. 이렇게 하면 메모리 접근 지연을 30-60% 줄일 수 있습니다.
     *
     * 혼합 NUMA 할당(numa_node == -1)이거나 CPU 핀닝 자체를 건너뛴 경우에는
     * numatune을 추가하지 않습니다 (libvirt 기본 NUMA 정책 사용).
     *
     * XML 패치 후 virDomainDefineXML()로 재정의하면 기동 전에 적용됩니다.
     */
    if (ctx->numa_node >= 0) {
        char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        if (xml) {
            /* <numatune> 이미 존재하면 패치 생략 (사용자 수동 설정 존중) */
            if (!strstr(xml, "<numatune>")) {
                gchar *numatune_xml = g_strdup_printf(
                    "  <numatune>\n"
                    "    <memory mode='strict' nodeset='%d'/>\n"
                    "  </numatune>\n", ctx->numa_node);

                /* </domain> 직전에 <numatune> 삽입 */
                char *end = strstr(xml, "</domain>");
                if (end) {
                    gchar *patched = g_strdup_printf("%.*s%s%s",
                        (gint)(end - xml), xml, numatune_xml, end);

                    virDomainPtr new_dom = virDomainDefineXML(conn, patched);
                    if (new_dom) {
                        virDomainFree(dom);
                        dom = new_dom;
                        g_message("[vm.start] NUMA memory binding applied: node %d for VM '%s'",
                                  ctx->numa_node, ctx->vm_id);
                    } else {
                        g_warning("[vm.start] Failed to apply numatune for VM '%s', continuing without",
                                  ctx->vm_id);
                    }
                    g_free(patched);
                }
                g_free(numatune_xml);
            }
            free(xml);  /* virDomainGetXMLDesc returns malloc'd string */
        }
    }

    /* 3단계: VM 기동 — virDomainCreate()는 QEMU 프로세스를 fork하므로 블로킹될 수 있음 */
    if (virDomainCreate(dom) < 0) {
        virErrorPtr err = virGetLastError();
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to start VM: %s", err ? err->message : "Unknown error");
        goto cleanup_dom;
    }

    /*
     * 4단계: NUMA 인식 CPU 핀닝
     *
     * cpu_allocator가 할당한 물리 코어(pCPU)를 각 가상 코어(vCPU)에 1:1로 바인딩합니다.
     * - allocated_cpus가 NULL이면 핀닝을 생략합니다 (CPU 할당 실패 시 graceful fallback).
     * - 핀닝 실패는 경고만 출력하고 계속 진행합니다 (비필수 최적화).
     *
     * VIR_CPU_MAPLEN: 비트맵 크기 계산 매크로 (CPU 수 / 8 바이트)
     * VIR_USE_CPU: 비트맵에서 특정 CPU 비트를 1로 설정
     */
    int maplen = VIR_CPU_MAPLEN(MAX_PHYSICAL_CPUS);
    if (ctx->allocated_cpus) {
        for (guint i = 0; i < ctx->allocated_cpus->len; i++) {
            guint pcpu_id = g_array_index(ctx->allocated_cpus, guint, i);
            unsigned char *cpumap = g_malloc0(maplen);  /* 0으로 초기화된 비트맵 */
            VIR_USE_CPU(cpumap, pcpu_id);  /* pcpu_id 비트만 1로 설정 */

            /* vCPU i번을 pCPU pcpu_id에 배타적으로 바인딩 (LIVE = 실행 중 적용) */
            if (virDomainPinVcpuFlags(dom, i, cpumap, maplen, VIR_DOMAIN_AFFECT_LIVE) < 0) {
                g_warning("Failed to pin vCPU %u to pCPU %u. Continuing...", i, pcpu_id);
            }
            g_free(cpumap);
        }
    }

    /*
     * 5단계: 네트워크 브릿지 핫플러그 (선택적)
     *
     * bridge_name이 지정된 경우, VM에 virtio NIC를 동적으로 추가합니다.
     * vhost + 멀티큐 설정으로 네트워크 성능을 최적화합니다.
     * 큐 수는 할당된 vCPU 수와 동일하게 설정합니다.
     *
     * [핫플러그 실패 시] VM을 destroy하고 에러를 반환합니다.
     * 네트워크 없이 실행되는 것보다 실패하는 것이 안전하기 때문입니다.
     */
    if (ctx->bridge_name && strlen(ctx->bridge_name) > 0) {
        GString *net_xml = g_string_new("<interface type='bridge'>\n");
        g_string_append_printf(net_xml, "  <source bridge='%s'/>\n", ctx->bridge_name);
        g_string_append(net_xml, "  <model type='virtio'/>\n");
        g_string_append_printf(net_xml,
            "  <driver name='vhost' queues='%u' rx_queue_size='1024' tx_queue_size='1024'/>\n",
            ctx->allocated_cpus ? ctx->allocated_cpus->len : 1);
        g_string_append(net_xml, "</interface>");

        if (virDomainAttachDeviceFlags(dom, net_xml->str, VIR_DOMAIN_AFFECT_LIVE) < 0) {
            virErrorPtr err = virGetLastError();
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Network hotplug failed: %s", err ? err->message : "Unknown");
            virDomainDestroy(dom);  /* 네트워크 핫플러그 실패 시 VM 강제 종료 */
            g_string_free(net_xml, TRUE);
            goto cleanup_dom;
        }
        g_string_free(net_xml, TRUE);
    }

cleanup_dom:
    if (dom) virDomainFree(dom);
cleanup_conn:
    if (conn) virt_conn_pool_release(conn);

    if (error) {
        g_task_return_error(task, error);
    } else {
        /* SG 디스패치 재동기화 — vnet 이름은 시작 시마다 새로 배정된다 (spec §4-3 (2)).
         * 워커 스레드에서만 호출 (pcv_spawn_sync 블로킹 규약). vm_start_callback 은
         * 메인 루프라 여기서는 호출 금지. */
        pcv_security_group_sync_vm(ctx->vm_id);
        g_task_return_boolean(task, TRUE);
    }
}

/**
 * vm_start_callback:
 * GTask 워커 완료 후 메인 스레드에서 호출되는 콜백입니다.
 *
 * [fire-and-forget 콜백 패턴]
 *   "accepted" 응답은 이미 전송되었으므로 여기서 send_response()를 호출하면 안 됩니다!
 *   소켓은 이미 닫혀 있어 전송하면 크래시 또는 undefined behavior가 발생합니다.
 *
 * [콜백에서 하는 일]
 *   1. 오퍼레이션 잠금 해제 (unlock_vm_operation) — 필수!
 *   2. 실패 시 CPU 할당 롤백 (cpu_allocator_free_vm_cores)
 *   3. 결과 로깅 (journalctl에서 확인 가능)
 */
static void vm_start_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmStartContext *ctx = (VmStartContext *)user_data;
    GError *error = NULL;

    gboolean success = g_task_propagate_boolean(task, &error);
    unlock_vm_operation(ctx->vm_id);  /* 오퍼레이션 잠금 해제 (성공/실패 모두) */

    /*
     * [중요] 여기서 send_response 호출 금지!
     * "accepted" 응답 전송 후 소켓이 이미 닫혔습니다.
     * 에러/성공 모두 로그로만 기록합니다.
     */
    /* ADR-0018: 워커 콜백에서 직접 audit 기록.
     * dispatcher.c:rpc_done은 vm.start를 g_async_methods에 등록해 자동 기록을 건너뛴다.
     * 이렇게 해야 감사 DB가 진짜 결과(ok/fail + 워커 실제 소요시간)를 가진다. */
    gint64 worker_dur_ms = (g_get_monotonic_time() - ctx->worker_start_us) / 1000;
    pcv_audit_log(NULL, "vm.start", ctx->vm_id,
                  success ? "ok" : "fail",
                  success ? 0 : -32000, worker_dur_ms, "local");

    /* ADR-0012 + ADR-0018: WebSocket 푸시 — UI가 실패를 즉시 인지 가능 */
    {
        gchar *job_id = g_strdup_printf("vm.start:%s", ctx->vm_id);
        pcv_ws_broadcast_job_complete(job_id, "vm.start",
                                       success ? "ok" : "fail",
                                       (success || !error) ? NULL : error->message);
        g_free(job_id);
    }

    if (!success) {
        /* CPU 할당 롤백: VM 시작 실패 시 할당된 코어를 반환 */
        cpu_allocator_free_vm_cores(global_allocator, ctx->vm_id);
        g_warning("[vm.start] async worker failed for '%s': %s",
                  ctx->vm_id, error ? error->message : "unknown");
        if (error) g_error_free(error);
    } else {
        g_message("[vm.start] VM '%s' started successfully (async)", ctx->vm_id);
    }
}

/**
 * handle_vm_start_request:
 * vm.start RPC 진입점 — VM 시작 요청을 처리합니다.
 * dispatcher.c에서 "vm.start" 메서드로 라우팅되어 호출됩니다.
 *
 * [fire-and-forget 패턴 — 이 핸들러의 핵심 구조]
 *   1. 파라미터 검증 + 오퍼레이션 잠금 획득
 *   2. CPU 할당기에서 NUMA 코어 할당 시도 (실패 시 graceful fallback)
 *   3. 즉시 {"status":"accepted"} 응답 전송 (소켓 닫힘!)
 *   4. GTask 워커 스레드에서 virDomainCreate + CPU 핀닝 실행
 *   5. 콜백에서 로그만 기록 (send_response 호출 금지)
 *
 * 파라미터:
 *   vm_id*       : VM 이름 또는 UUID (필수)
 *   numa_node    : NUMA 노드 번호 (기본 0)
 *   vcpu_count   : 할당할 vCPU 수 (기본 1)
 *   bridge_name  : 네트워크 브릿지 (선택, 핫플러그용)
 */
void handle_vm_start_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    /* 파라미터 검증: vm_id는 필수 */
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    /* 선택 파라미터 추출 (없으면 기본값 사용) */
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    guint numa_node = json_object_has_member(params, "numa_node") ? json_object_get_int_member(params, "numa_node") : 0;
    guint vcpu_count = json_object_has_member(params, "vcpu_count") ? json_object_get_int_member(params, "vcpu_count") : 1;
    const gchar *bridge = json_object_has_member(params, "bridge_name") ? json_object_get_string_member(params, "bridge_name") : "";

    /*
     * 오퍼레이션 잠금 획득 (vm_state.c의 SQLite WAL 기반)
     * 동일 VM에 대한 동시 start/stop/delete를 방지합니다.
     * 잠금 실패 시 다른 작업이 진행 중이라는 에러를 즉시 반환합니다.
     */
    gchar *err_msg = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_STARTING, &err_msg)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, err_msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        g_free(err_msg);
        return;
    }

    /*
     * NUMA 인식 CPU 할당기에서 배타적 코어 할당 시도
     *
     * [CPU 할당기 graceful fallback]
     *   할당 실패(isolated 코어 부족)해도 VM 시작은 진행합니다.
     *   allocated_cpus = NULL이면 워커에서 CPU 핀닝을 건너뛰고,
     *   libvirt 기본 CFS 스케줄링으로 동작합니다.
     *
     *   예: 16코어 호스트에서 2개만 isolated인데 4개를 요청한 경우
     *       → 핀닝 없이 시작 (성능은 약간 떨어지지만 동작은 정상)
     */
    GArray *allocated_cpus = NULL;
    gint actual_numa_node = -1;
    if (!cpu_allocator_allocate_exclusive(global_allocator, vm_id, numa_node, vcpu_count, &allocated_cpus, &actual_numa_node)) {
        g_warning("[vm.start] No isolated cores for '%s' (need %u), starting without CPU pinning",
                  vm_id, vcpu_count);
        allocated_cpus = NULL; /* 핀닝 없이 진행 */
        actual_numa_node = -1; /* NUMA 바인딩도 생략 */
    }

    /* 비동기 작업 컨텍스트 구성 — 모든 필드를 복사/참조 카운트 증가 */
    VmStartContext *ctx = g_new0(VmStartContext, 1);
    ctx->vm_id = g_strdup(vm_id);
    ctx->bridge_name = g_strdup(bridge);
    ctx->allocated_cpus = allocated_cpus;  /* NULL일 수 있음 (fallback 시) */
    ctx->numa_node = actual_numa_node;     /* -1이면 NUMA 바인딩 생략 */
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);      /* ref 카운트 증가 — 워커 완료까지 유지 */
    ctx->connection = g_object_ref(connection);
    ctx->worker_start_us = g_get_monotonic_time();  /* ADR-0018: 워커 실제 소요시간 측정 */

    /* GTask 생성: 콜백(vm_start_callback) + 해제 함수(free_vm_start_context) 등록 */
    GTask *task = g_task_new(NULL, NULL, vm_start_callback, ctx);
    g_task_set_task_data(task, ctx, (GDestroyNotify)free_vm_start_context);

    /*
     * [fire-and-forget 핵심] 즉시 "accepted" 응답 전송
     *
     * virDomainCreate()는 QEMU 프로세스 생성 + 디스크 로딩으로 수십 초 블로킹될 수 있습니다.
     * UDS 소켓의 기본 타임아웃(5~30초) 전에 응답을 보내야 클라이언트가 끊기지 않습니다.
     *
     * 이 시점 이후 소켓은 닫힙니다 — 이후 send_response() 호출은 금지!
     */
    {
        JsonNode *acc_node = json_node_new(JSON_NODE_VALUE);
        json_node_set_string(acc_node, "accepted");
        gchar *acc_resp = pure_rpc_build_success_response(rpc_id, acc_node);
        pure_uds_server_send_response(server, connection, acc_resp);  /* 소켓 즉시 닫힘 */
        g_free(acc_resp);
    }

    /* GLib 스레드 풀에서 워커 스레드 실행 — 메인 루프는 차단되지 않음 */
    g_task_run_in_thread(task, vm_start_worker_thread);
    g_object_unref(task);  /* GTask ref 해제 — 실제 해제는 워커 완료 후 */
}