/**
 * @file handler_vm_lifecycle.c
 * @brief VM 상태 조회, 종료, 삭제를 담당하는 비동기 디스패처 (Phase 6)
 */
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/core/vm_state.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"

// =================================================================
// 공통 컨텍스트 구조체
// =================================================================
typedef struct {
    gchar *vm_id;
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
} VmLifecycleCtx;

static void free_lifecycle_ctx(gpointer data) {
    if (!data) return;
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)data;
    g_free(ctx->vm_id);
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
static void vm_action_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;
    gboolean is_delete = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(task), "is_delete"));
    GError *error = NULL;

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

    if (is_delete) {
        if (virDomainIsActive(dom)) virDomainDestroy(dom); // 강제 종료 후
        if (virDomainUndefine(dom) < 0) {                  // 삭제
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to delete VM.");
            goto cleanup;
        }
    } else {
        if (virDomainDestroy(dom) < 0) {                   // 강제 종료 (ACPI Shutdown 대기 생략)
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to stop VM.");
            goto cleanup;
        }
    }

    g_task_return_boolean(task, TRUE);

cleanup:
    virDomainFree(dom);
    virConnectClose(conn);
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

    GTask *task = g_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE));
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}

// VM.DELETE 진입점
void handle_vm_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); return;
    }
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");

    gchar *err_msg = NULL;
    if (!lock_vm_operation(vm_id, 3, &err_msg)) { // 3 = OP_DELETING
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, err_msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(err_msg); return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id); ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server); ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(TRUE));
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}