/**
 * @file handler_vm_hotplug.c
 * @brief VM 리소스(메모리, vCPU) 동적 할당을 담당하는 비동기 디스패처 (Phase 6)
 */
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/handler_vm_hotplug.h"

// 라이프사이클 모듈에 있는 다형성 검색 함수를 재사용합니다.
extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);


// =================================================================
// 공통 컨텍스트 구조체
// =================================================================
typedef struct {
    gchar *vm_id;
    gint target_value; // memory_mb 또는 vcpu_count 저장
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
} VmHotplugCtx;

static void free_hotplug_ctx(gpointer data) {
    if (!data) return;
    VmHotplugCtx *ctx = (VmHotplugCtx *)data;
    g_free(ctx->vm_id);
    g_free(ctx->rpc_id);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

// =================================================================
// 1. 메모리 핫플러그 비동기 워커
// =================================================================
static void vm_set_memory_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmHotplugCtx *ctx = (VmHotplugCtx *)task_data;
    virConnectPtr conn = virConnectOpen("qemu:///system");
    
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = virDomainLookupByUUIDString(conn, ctx->vm_id);
    if (!dom) {
        virConnectClose(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM UUID %s not found.", ctx->vm_id);
        return;
    }

    // MB 단위를 KB 단위로 변환
    unsigned long memory_kb = (unsigned long)ctx->target_value * 1024;

    // AFFECT_LIVE(현재 실행중인 VM)와 AFFECT_CONFIG(다음 부팅시 적용)를 동시 적용
    if (virDomainSetMemoryFlags(dom, memory_kb, VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG) < 0) {
        virErrorPtr err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Memory hotplug failed: %s", err ? err->message : "Unknown");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(dom);
    virConnectClose(conn);
}

// =================================================================
// 2. vCPU 핫플러그 비동기 워커
// =================================================================
static void vm_set_vcpu_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmHotplugCtx *ctx = (VmHotplugCtx *)task_data;
    virConnectPtr conn = virConnectOpen("qemu:///system");
    
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = virDomainLookupByUUIDString(conn, ctx->vm_id);
    if (!dom) {
        virConnectClose(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM UUID %s not found.", ctx->vm_id);
        return;
    }

    if (virDomainSetVcpusFlags(dom, ctx->target_value, VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG) < 0) {
        virErrorPtr err = virGetLastError();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "vCPU hotplug failed: %s", err ? err->message : "Unknown");
    } else {
        g_task_return_boolean(task, TRUE);
    }

    virDomainFree(dom);
    virConnectClose(conn);
}

// =================================================================
// 3. 공통 콜백 함수
// =================================================================
static void hotplug_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmHotplugCtx *ctx = (VmHotplugCtx *)user_data;
    GError *error = NULL;

    gboolean success = g_task_propagate_boolean(task, &error);

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

// =================================================================
// 4. 진입점 (Dispatchers)
// =================================================================
void handle_vm_set_memory_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id") || !json_object_has_member(params, "memory_mb")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' or 'memory_mb' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
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

void handle_vm_set_vcpu_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id") || !json_object_has_member(params, "vcpu_count")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' or 'vcpu_count' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    VmHotplugCtx *ctx = g_new0(VmHotplugCtx, 1);
    ctx->vm_id = g_strdup(json_object_get_string_member(params, "vm_id"));
    ctx->target_value = json_object_get_int_member(params, "vcpu_count");
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, hotplug_callback, ctx);
    g_task_set_task_data(task, ctx, free_hotplug_ctx);
    g_task_run_in_thread(task, vm_set_vcpu_worker);
    g_object_unref(task);
}

// =================================================================
// [API 진입점] 라이브 디스크 장착 (Attach)
// =================================================================
void handle_device_disk_attach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    const gchar *source_dev = json_object_get_string_member(params, "source");
    const gchar *target_dev = json_object_get_string_member(params, "target");

    if (!vm_id || !source_dev || !target_dev) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602, "Missing vm_id, source, or target");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn = virConnectOpen("qemu:///system");
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virConnectClose(conn); return;
    }

    // 🚀 [핵심] ZVOL을 위한 블록 디바이스 XML 조립 (virtio 버스 사용)
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
    virConnectClose(conn);
}

// =================================================================
// [블록 디바이스 적출] Live XML 파싱 기반 완벽 적출 엔진
// =================================================================
void handle_device_disk_detach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    const gchar *target_dev = json_object_get_string_member(params, "target");

    if (!vm_id || !target_dev) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602, "Missing vm_id or target");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn = virConnectOpen("qemu:///system");
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virConnectClose(conn); return;
    }

    // 1. 가동 중인 가상 머신의 실시간(Live) XML을 가져옵니다.
    gchar *live_xml = virDomainGetXMLDesc(dom, 0);
    gchar *target_tag = g_strdup_printf("<target dev='%s'", target_dev);
    
    // 2. XML 내부에서 타겟 디바이스(예: vdb)의 위치를 찾습니다.
    gchar *target_pos = strstr(live_xml, target_tag);
    
    if (!target_pos) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, "Device not found in live XML");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        g_free(live_xml); g_free(target_tag); virDomainFree(dom); virConnectClose(conn); return;
    }

    // 3. 해당 타겟을 감싸고 있는 <disk> 태그의 시작과 끝을 역추적하여 완벽하게 발라냅니다.
    gchar *disk_start = target_pos;
    while (disk_start >= live_xml && strncmp(disk_start, "<disk ", 6) != 0 && strncmp(disk_start, "<disk>", 6) != 0) {
        disk_start--;
    }
    
    gchar *disk_end = strstr(target_pos, "</disk>");
    if (disk_end) disk_end += 7; // "</disk>" 문자열 길이 포함

    // 발라낸 100% 순정 디스크 XML
    gchar *exact_xml = g_strndup(disk_start, disk_end - disk_start);

    // 4. 완벽한 XML로 적출(Detach) 타격!
    // unsigned int flags = VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG;
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

    // 메모리 대청소
    g_free(exact_xml);
    g_free(target_tag);
    g_free(live_xml);
    virDomainFree(dom);
    virConnectClose(conn);
}