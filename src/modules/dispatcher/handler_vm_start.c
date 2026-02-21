/**
 * @file handler_vm_start.c
 * @brief VM 구동 및 리소스 튜닝 디스패처 (Phase 6 규격 적용)
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

#define MAX_PHYSICAL_CPUS 256

typedef struct {
    gchar *vm_id;
    gchar *bridge_name;
    GArray *allocated_cpus;
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
} VmStartContext;

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

static void vm_start_worker_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    VmStartContext *ctx = (VmStartContext *)task_data;
    GError *error = NULL;
    
    virConnectPtr conn = virConnectOpen("qemu:///system");
    if (!conn) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt daemon.");
        g_task_return_error(task, error);
        return;
    }

    virDomainPtr dom = virDomainLookupByUUIDString(conn, ctx->vm_id);
    if (!dom) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM UUID %s not found.", ctx->vm_id);
        goto cleanup_conn;
    }

    if (virDomainCreate(dom) < 0) {
        virErrorPtr err = virGetLastError();
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to start VM: %s", err ? err->message : "Unknown error");
        goto cleanup_dom;
    }

    int maplen = VIR_CPU_MAPLEN(MAX_PHYSICAL_CPUS);
    if (ctx->allocated_cpus) {
        for (guint i = 0; i < ctx->allocated_cpus->len; i++) {
            guint pcpu_id = g_array_index(ctx->allocated_cpus, guint, i);
            unsigned char *cpumap = g_malloc0(maplen);
            VIR_USE_CPU(cpumap, pcpu_id); 
            
            if (virDomainPinVcpuFlags(dom, i, cpumap, maplen, VIR_DOMAIN_AFFECT_LIVE) < 0) {
                g_warning("Failed to pin vCPU %u to pCPU %u. Continuing...", i, pcpu_id);
            }
            g_free(cpumap);
        }
    }

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
            virDomainDestroy(dom);
            g_string_free(net_xml, TRUE);
            goto cleanup_dom;
        }
        g_string_free(net_xml, TRUE);
    }

cleanup_dom:
    if (dom) virDomainFree(dom);
cleanup_conn:
    if (conn) virConnectClose(conn);

    if (error) g_task_return_error(task, error);
    else g_task_return_boolean(task, TRUE);
}

static void vm_start_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmStartContext *ctx = (VmStartContext *)user_data;
    GError *error = NULL;

    gboolean success = g_task_propagate_boolean(task, &error);
    unlock_vm_operation(ctx->vm_id);

    if (!success) {
        cpu_allocator_free_vm_cores(global_allocator, ctx->vm_id);
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

// 🚀 라우터에서 호출될 최종 진입점
void handle_vm_start_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    guint numa_node = json_object_has_member(params, "numa_node") ? json_object_get_int_member(params, "numa_node") : 0;
    guint vcpu_count = json_object_has_member(params, "vcpu_count") ? json_object_get_int_member(params, "vcpu_count") : 1;
    const gchar *bridge = json_object_has_member(params, "bridge_name") ? json_object_get_string_member(params, "bridge_name") : "";

    gchar *err_msg = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_STARTING, &err_msg)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, err_msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        g_free(err_msg);
        return;
    }

    GArray *allocated_cpus = NULL;
    if (!cpu_allocator_allocate_exclusive(global_allocator, vm_id, numa_node, vcpu_count, &allocated_cpus)) {
        unlock_vm_operation(vm_id);
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32000, "Not enough isolated CPU cores available.");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    VmStartContext *ctx = g_new0(VmStartContext, 1);
    ctx->vm_id = g_strdup(vm_id);
    ctx->bridge_name = g_strdup(bridge);
    ctx->allocated_cpus = allocated_cpus;
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, vm_start_callback, ctx);
    g_task_set_task_data(task, ctx, (GDestroyNotify)free_vm_start_context);
    g_task_run_in_thread(task, vm_start_worker_thread);
    g_object_unref(task);
}