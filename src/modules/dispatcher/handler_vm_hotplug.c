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