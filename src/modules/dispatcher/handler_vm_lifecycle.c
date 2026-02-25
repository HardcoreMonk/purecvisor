/**
 * @file handler_vm_lifecycle.c
 * @brief VM 상태 조회, 종료, 삭제를 담당하는 비동기 디스패처 (Phase 6)
 */
#include <unistd.h>
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/core/vm_state.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"

// =================================================================
// 공통 컨텍스트 구조체
// =================================================================
typedef struct {
    gchar *vm_id;
    gchar *action; // 어떤 동작(start/stop/reset)을 할지 구분하는 변수
    gint cpu_quota; // CPU 제한 퍼센티지 (예 : 50 = 50%)
    gint mem_quota_mb; // 메모리 제한(MB 단위)
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
    // 🚀 모니터링 결과 저장용 변수 추가
    gint out_cpu_pct;
    gint out_mem_pct;
} VmLifecycleCtx;

static void free_lifecycle_ctx(gpointer data) {
    if (!data) return;
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)data;
    g_free(ctx->vm_id);
    g_free(ctx->action); // 메모리 해제
    g_free(ctx->rpc_id);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

// =================================================================
// 1. VM.LIST (상태 조회) 비동기 워커 및 콜백
// =================================================================
static void vm_list_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    virConnectPtr conn = virConnectOpen("qemu:///system");
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr *domains;
    int ret = virConnectListAllDomains(conn, &domains, 0);
    if (ret < 0) {
        virConnectClose(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to list domains.");
        return;
    }

    JsonArray *array = json_array_new();
    for (int i = 0; i < ret; i++) {
        JsonObject *vm_obj = json_object_new();
        char uuid[VIR_UUID_STRING_BUFLEN];
        virDomainGetUUIDString(domains[i], uuid);
        
        json_object_set_string_member(vm_obj, "uuid", uuid);
        json_object_set_string_member(vm_obj, "name", virDomainGetName(domains[i]));
        
        virDomainInfo info;
        virDomainGetInfo(domains[i], &info);
        const char *state_str = (info.state == VIR_DOMAIN_RUNNING) ? "running" : 
                                (info.state == VIR_DOMAIN_SHUTOFF) ? "shutoff" : "unknown";
        json_object_set_string_member(vm_obj, "state", state_str);
        
        json_array_add_object_element(array, vm_obj);
        virDomainFree(domains[i]);
    }
    free(domains);
    virConnectClose(conn);

    JsonNode *root_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(root_node, array);
    g_task_return_pointer(task, root_node, (GDestroyNotify)json_node_free);
}

static void vm_list_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;

    JsonNode *result_node = g_task_propagate_pointer(task, &error);
    if (error) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, -32000, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
        json_node_free(result_node);
    }
}

void handle_vm_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, vm_list_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, vm_list_worker);
    g_object_unref(task);
}

// =================================================================
// 2. VM.STOP & VM.DELETE 공용 워커 및 콜백 (Lock-Free 방어 적용)
// =================================================================

// 🚀 [이동됨] 워커 함수가 이 함수를 부르기 전에 미리 정의되어 있어야 합니다.
virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier) {
    virDomainPtr dom = virDomainLookupByUUIDString(conn, identifier);
    if (!dom) {
        virResetLastError();
        dom = virDomainLookupByName(conn, identifier);
    }
    return dom;
}
static void vm_action_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;
    // 비동기 워커 미사용 gboolean is_delete = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(task), "is_delete"));
    GError *error = NULL;
    // 1. 하이퍼바이저 연결
    virConnectPtr conn = virConnectOpen("qemu:///system");
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }
    
    // 🚀 [핵심 수정 포인트] 통합 검색 함수를 호출하여 dom 변수를 선언하고 초기화합니다!
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    
    // VM을 찾지 못한 경우의 에러 처리
    if (!dom) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM not found: %s", ctx->vm_id);
        virConnectClose(conn);
        g_task_return_error(task, error);
        return;
    }

    // 2. 액션 분기 처리 (안전한 상태 검사 추가)
    if (g_strcmp0(ctx->action, "start") == 0) {
        if (virDomainIsActive(dom)) {
            g_print("VM '%s' is already running. Skipping start sequence.\n", ctx->vm_id);
        } else if (virDomainCreate(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to start VM: %s", ctx->vm_id);
            virDomainFree(dom); virConnectClose(conn); g_task_return_error(task, error); return;
        }
    } 
    else if (g_strcmp0(ctx->action, "stop") == 0) {
        if (!virDomainIsActive(dom)) {
            g_print("VM '%s' is already shut off. Skipping stop sequence.\n", ctx->vm_id);
        } else if (virDomainDestroy(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to stop VM: %s", ctx->vm_id);
            virDomainFree(dom); virConnectClose(conn); g_task_return_error(task, error); return;
        }
    } 
    else if (g_strcmp0(ctx->action, "reset") == 0) {
        if (virDomainIsActive(dom)) {
            virDomainDestroy(dom); // 강제 종료
        }
        if (virDomainCreate(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to reset VM: %s", ctx->vm_id);
            virDomainFree(dom); virConnectClose(conn); g_task_return_error(task, error); return;
        }
    
    }
    else if (g_strcmp0(ctx->action, "limit") == 0) {
        if (!virDomainIsActive(dom)) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Entity '%s' is not active. Cannot apply live limits.", ctx->vm_id);
            virDomainFree(dom); virConnectClose(conn); g_task_return_error(task, error); return;
        }

        // 🚀 CPU Cgroup v2 (cpu.max) 실시간 제어
        if (ctx->cpu_quota > 0) {
            virTypedParameter params[1];
            // VIR_DOMAIN_SCHEDULER_VCPU_QUOTA가 cgroup의 cpu.max quota 값으로 맵핑됩니다.
            strncpy(params[0].field, VIR_DOMAIN_SCHEDULER_VCPU_QUOTA, VIR_TYPED_PARAM_FIELD_LENGTH);
            params[0].type = VIR_TYPED_PARAM_LLONG;
            
            // 기본 period가 100,000us(100ms)이므로, 1%는 1,000us에 해당합니다.
            params[0].value.l = (long long)ctx->cpu_quota * 1000;

            // -1 이면 제한 해제(Unlimited), 그 외에는 지정된 퍼센티지 적용
            if (ctx->cpu_quota == -1) {
                params[0].value.l = -1; // 커널 CFS Quota 무제한
            } else {
                params[0].value.l = (long long)ctx->cpu_quota * 1000;
            }

            if (virDomainSetSchedulerParametersFlags(dom, params, 1, VIR_DOMAIN_AFFECT_LIVE) < 0) {
                g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to inject cgroup limits to kernel.");
                virDomainFree(dom); virConnectClose(conn); g_task_return_error(task, error); return;
            }
        }

        // 🚀 신규 추가: Memory Cgroup v2 (memory.max) 실시간 제어
        if (ctx->mem_quota_mb > 0) {
            virTypedParameter mem_params[1];
            // Libvirt의 HARD_LIMIT이 cgroup의 memory.max와 직접 매핑됩니다.
            strncpy(mem_params[0].field, VIR_DOMAIN_MEMORY_HARD_LIMIT, VIR_TYPED_PARAM_FIELD_LENGTH);
            mem_params[0].type = VIR_TYPED_PARAM_ULLONG;
            mem_params[0].value.ul = (unsigned long long)ctx->mem_quota_mb * 1024; // MB -> KiB 변환

            // -1 이면 제한 해제(Unlimited), 그 외에는 MB를 KiB로 변환하여 적용
            if (ctx->mem_quota_mb == -1) {
                mem_params[0].value.ul = VIR_DOMAIN_MEMORY_PARAM_UNLIMITED; // Libvirt 무제한 상수
            } else {
                mem_params[0].value.ul = (unsigned long long)ctx->mem_quota_mb * 1024;
            }

            if (virDomainSetMemoryParameters(dom, mem_params, 1, VIR_DOMAIN_AFFECT_LIVE) < 0) {
                g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to inject memory limits to kernel.");
                virDomainFree(dom); virConnectClose(conn); g_task_return_error(task, error); return;
            }
        }
    }


    // 3. 자원 해제 및 성공 리턴
    virDomainFree(dom);
    virConnectClose(conn);
    
    g_task_return_boolean(task, TRUE);
    
}
static void vm_action_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;

    gboolean success = g_task_propagate_boolean(task, &error);
    unlock_vm_operation(ctx->vm_id); // 🚀 락 해제

    if (!success) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, -32000, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, json_node_new(JSON_NODE_NULL));
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
    }
}

// VM.STOP 진입점
void handle_vm_stop_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");

    gchar *err_msg = NULL;
    if (!lock_vm_operation(vm_id, 2, &err_msg)) { // 2 = OP_STOPPING
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, err_msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(err_msg); return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id); ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server); ctx->connection = g_object_ref(connection);
    // 🚀 추가: 워커 스레드에게 "이것은 stop 명령이야"라고 알려줍니다.
    ctx->action = g_strdup("stop"); 
    ctx->rpc_id = g_strdup(rpc_id);

    GTask *task = g_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE));
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}


// 🚀 Limit 전용 요청 핸들러
void handle_vm_limit_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(json_object_get_string_member(params, "vm_id"));
    ctx->action = g_strdup("limit");
    
    if (json_object_has_member(params, "cpu")) {
        ctx->cpu_quota = json_object_get_int_member(params, "cpu");
    }

    // 🚀 신규 추가: JSON에서 mem 값 추출
    if (json_object_has_member(params, "mem")) {
        ctx->mem_quota_mb = json_object_get_int_member(params, "mem");
    }
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    
    // 상태값(deleted/created) 대신 일반 성공 메시지를 띄우기 위해 태그 생략 또는 FALSE 세팅
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE)); 
    
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}

// =================================================================
// [비동기 콜백] JSON 응답 조립 (Metrics)
// =================================================================
static void vm_metrics_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;

    if (g_task_propagate_boolean(task, &error)) {
        // 성공 시 JSON Object 조립 {"cpu": 45, "mem": 82}
        JsonObject *result_obj = json_object_new();
        json_object_set_int_member(result_obj, "cpu", ctx->out_cpu_pct);
        json_object_set_int_member(result_obj, "mem", ctx->out_mem_pct);

        JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(result_node, result_obj);

        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp); // json_node는 헬퍼 내부 로직에 따라 해제 유무 확인
    } else {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, -32000, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    }
}

// =================================================================
// [워커 스레드] Libvirt CPU & Memory 샘플링
// =================================================================
static void vm_metrics_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;
    
    virConnectPtr conn = virConnectOpen("qemu:///system");
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);

    // VM이 꺼져있으면 점유율 0%로 반환하여 UI 붕괴 방지
    if (!dom || !virDomainIsActive(dom)) {
        ctx->out_cpu_pct = 0;
        ctx->out_mem_pct = 0;
    } else {
        virDomainInfo info1, info2;
        
        // 1. CPU Delta 계산 (100ms 대기)
        virDomainGetInfo(dom, &info1);
        g_usleep(100000); 
        virDomainGetInfo(dom, &info2);

        unsigned long long time_diff = info2.cpuTime - info1.cpuTime;
        unsigned long long wall_diff = 100000000ULL * info1.nrVirtCpu; // 100ms in 나노초
        ctx->out_cpu_pct = (wall_diff > 0) ? (int)((time_diff * 100) / wall_diff) : 0;
        if (ctx->out_cpu_pct > 100) ctx->out_cpu_pct = 100;

        // 2. Memory RSS 계산
        virDomainMemoryStatStruct mem_stats[VIR_DOMAIN_MEMORY_STAT_NR];
        int nr_stats = virDomainMemoryStats(dom, mem_stats, VIR_DOMAIN_MEMORY_STAT_NR, 0);
        unsigned long long mem_rss = info2.memory / 5; // 텔레메트리 실패 시 더미 기본값
        for (int i = 0; i < nr_stats; i++) {
            if (mem_stats[i].tag == VIR_DOMAIN_MEMORY_STAT_RSS) mem_rss = mem_stats[i].val;
        }
        ctx->out_mem_pct = (info2.memory > 0) ? (int)((mem_rss * 100) / info2.memory) : 0;
        if (ctx->out_mem_pct > 100) ctx->out_mem_pct = 100;
    }

    if (dom) virDomainFree(dom);
    if (conn) virConnectClose(conn);
    
    g_task_return_boolean(task, TRUE);
}

// =================================================================
// [API 진입점]
// =================================================================
void handle_vm_metrics_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(json_object_get_string_member(params, "vm_id"));
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, vm_metrics_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx); // 해제 함수 이름 주의!
    g_task_run_in_thread(task, vm_metrics_worker);
    g_object_unref(task);
}

// =================================================================
// [가상 머신 시각 피질] 실시간 VNC 포트 추출기
// =================================================================
void handle_vm_vnc_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    if (!vm_id) return;

    virConnectPtr conn = virConnectOpen("qemu:///system");
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virConnectClose(conn); return;
    }

    // 1. 살아있는(RUNNING) 상태인지 확인 (꺼져있으면 포트가 없음)
    virDomainInfo info;
    virDomainGetInfo(dom, &info);
    if (info.state != VIR_DOMAIN_RUNNING) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "VM is not running. No VNC port active.");
        pure_uds_server_send_response(server, connection, err); g_free(err); virDomainFree(dom); virConnectClose(conn); return;
    }

    // 2. 실시간 메모리 XML을 스캔하여 VNC 포트 번호 획득
    gchar *xml = virDomainGetXMLDesc(dom, 0);
    gchar *port_start = strstr(xml, "graphics type='vnc' port='");
    
    if (port_start) {
        port_start += 26; // 문자열 길이만큼 이동
        gchar *port_end = strchr(port_start, '\'');
        if (port_end) {
            gchar *port_str = g_strndup(port_start, port_end - port_start);
            
            JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
            JsonObject *res_obj = json_object_new();
            json_object_set_string_member(res_obj, "vnc_port", port_str);
            json_node_take_object(res_node, res_obj);

            gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp);
            g_free(port_str);
        }
    } else {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "VNC Graphics adapter not found in XML");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    }

    g_free(xml); virDomainFree(dom); virConnectClose(conn);
}

// ===================================================================================================
// [VM Lifecycle] 궁극의 파괴 엔진 (XML + ZVOL + Partition Exorcism + Validation & Error Reporting 탑재)
// ===================================================================================================

void handle_vm_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    if (!vm_id) return;

    virConnectPtr conn = virConnectOpen("qemu:///system");
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    gchar *zvol_path = g_strdup_printf("/dev/zvol/rpool/vms/%s", vm_id);
    gchar *zfs_dataset = g_strdup_printf("rpool/vms/%s", vm_id);

    // ---------------------------------------------------------
    // 🛡️ 1단계: 존재 유무 절대 검증 (Physical & Logical)
    // ---------------------------------------------------------
    // 쉘(Shell) 상태에 의존하던 불확실한 방식을 버리고, 
    // OS 레벨의 물리적 파일/심볼릭링크 존재 여부(access)로 확실하게 팩트 체크합니다!
    gboolean zfs_exists = (access(zvol_path, F_OK) == 0);

    // 뼈대(XML)도 없고 디스크(ZFS)도 아예 없다면 완벽한 유령이므로 즉시 에러 튕겨내기!
    if (!dom && !zfs_exists) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "Entity not found: The specified VM does not exist.");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_free(zvol_path); g_free(zfs_dataset); 
        if (conn) virConnectClose(conn); 
        return;
    }

    // ---------------------------------------------------------
    // 💀 2단계: 가상 머신 숨통 끊기 및 뼈대 완벽 소각 (Zombie 방지)
    // ---------------------------------------------------------
    if (dom) {
        virDomainInfo info;
        virDomainGetInfo(dom, &info);
        
        if (info.state == VIR_DOMAIN_RUNNING || info.state == VIR_DOMAIN_PAUSED) {
            virDomainDestroy(dom); 
        }
        
        // 🚀 완벽한 뼈대 소각을 위한 2단 Fallback 체인!
        // 플래그 삭제가 실패할 경우, 무식하고 확실한 기본 삭제 명령으로 2차 타격을 가합니다.
        if (virDomainUndefineFlags(dom, VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA | VIR_DOMAIN_UNDEFINE_MANAGED_SAVE) < 0) {
            virDomainUndefine(dom); 
        }
        virDomainFree(dom);
    }
    if (conn) virConnectClose(conn);

    // ---------------------------------------------------------
    // 💣 3단계: 호스트 멱살 강제 해제 및 ZFS 연쇄 파괴
    // ---------------------------------------------------------
    gboolean zfs_success = TRUE;
    gchar *zfs_err_msg = g_strdup("Success");

    if (zfs_exists) {
        gchar *cmd_exorcism = g_strdup_printf(
            "fuser -k -9 %s >/dev/null 2>&1; " 
            "VG_NAME=$(pvs --noheadings -o vg_name $(ls %s-part* 2>/dev/null) 2>/dev/null | awk '{print $1}' | sort -u); "
            "for vg in $VG_NAME; do vgchange -a n \"$vg\" >/dev/null 2>&1; done; "
            "wipefs -a %s >/dev/null 2>&1; "   
            "dd if=/dev/zero of=%s bs=1M count=10 status=none; "
            "partx -d %s >/dev/null 2>&1; "    
            "kpartx -d %s >/dev/null 2>&1; "   
            "partprobe >/dev/null 2>&1; "
            "udevadm settle; "
            "sleep 2", 
            zvol_path, zvol_path, zvol_path, zvol_path, zvol_path, zvol_path); // %s 6개 유지
        
        system(cmd_exorcism);
        g_free(cmd_exorcism);

        gchar *cmd_zfs = g_strdup_printf("zfs destroy -R %s 2>&1", zfs_dataset);
        FILE *fp = popen(cmd_zfs, "r");
        if (fp) {
            char output[512] = {0};
            if (fgets(output, sizeof(output)-1, fp) != NULL) {
                output[strcspn(output, "\n")] = 0; 
                g_free(zfs_err_msg);
                zfs_err_msg = g_strdup(output);
            }
            int ret = pclose(fp);
            if (ret != 0) zfs_success = FALSE; 
        }
        g_free(cmd_zfs);
    }

    g_free(zvol_path);
    g_free(zfs_dataset);

    // ---------------------------------------------------------
    // 📡 4단계: 결과 전송 
    // ---------------------------------------------------------
    if (!zfs_success) {
        gchar *fail_reason = g_strdup_printf("VM XML deleted, but ZFS destroy failed: %s", zfs_err_msg);
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, fail_reason);
        pure_uds_server_send_response(server, connection, err);
        
        g_free(err); g_free(fail_reason); g_free(zfs_err_msg); return;
    }

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    JsonObject *res_obj = json_object_new();
    json_object_set_boolean_member(res_obj, "deleted", TRUE);
    json_node_take_object(res_node, res_obj);

    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    
    g_free(resp);
    g_free(zfs_err_msg);
}

