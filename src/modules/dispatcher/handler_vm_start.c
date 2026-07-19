
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

extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

#define MAX_PHYSICAL_CPUS 256

typedef struct {
    gchar *vm_id;
    gchar *bridge_name;
    GArray *allocated_cpus;
    gint numa_node;
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
    gint64 worker_start_us;
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

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt daemon.");
        g_task_return_error(task, error);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Entity '%s' not found.", ctx->vm_id);
        goto cleanup_conn;
    }

    {
        virDomainInfo info;
        if (virDomainGetInfo(dom, &info) == 0 &&
            (info.state == VIR_DOMAIN_RUNNING || info.state == VIR_DOMAIN_BLOCKED)) {
            g_message("[vm.start] VM '%s': already running (idempotent no-op)", ctx->vm_id);
            virDomainFree(dom);
            virt_conn_pool_release(conn);

            pcv_security_group_sync_vm(ctx->vm_id);
            g_task_return_boolean(task, TRUE);
            return;
        }
    }

    if (ctx->numa_node >= 0) {
        char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        if (xml) {

            if (!strstr(xml, "<numatune>")) {
                gchar *numatune_xml = g_strdup_printf(
                    "  <numatune>\n"
                    "    <memory mode='strict' nodeset='%d'/>\n"
                    "  </numatune>\n", ctx->numa_node);

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
            free(xml);
        }
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
    if (conn) virt_conn_pool_release(conn);

    if (error) {
        g_task_return_error(task, error);
    } else {

        pcv_security_group_sync_vm(ctx->vm_id);
        g_task_return_boolean(task, TRUE);
    }
}

static void vm_start_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmStartContext *ctx = (VmStartContext *)user_data;
    GError *error = NULL;

    gboolean success = g_task_propagate_boolean(task, &error);
    unlock_vm_operation(ctx->vm_id);

    gint64 worker_dur_ms = (g_get_monotonic_time() - ctx->worker_start_us) / 1000;
    pcv_audit_log(NULL, "vm.start", ctx->vm_id,
                  success ? "ok" : "fail",
                  success ? 0 : PURE_RPC_ERR_ZFS_OPERATION, worker_dur_ms, "local");

    {
        gchar *job_id = g_strdup_printf("vm.start:%s", ctx->vm_id);
        pcv_ws_broadcast_job_complete(job_id, "vm.start",
                                       success ? "ok" : "fail",
                                       (success || !error) ? NULL : error->message);
        g_free(job_id);
    }

    if (!success) {

        cpu_allocator_free_vm_cores(global_allocator, ctx->vm_id);
        g_warning("[vm.start] async worker failed for '%s': %s",
                  ctx->vm_id, error ? error->message : "unknown");
        if (error) g_error_free(error);
    } else {
        g_message("[vm.start] VM '%s' started successfully (async)", ctx->vm_id);
    }
}

void handle_vm_start_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {

    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid params");
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
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err_msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        g_free(err_msg);
        return;
    }

    GArray *allocated_cpus = NULL;
    gint actual_numa_node = -1;
    if (!cpu_allocator_allocate_exclusive(global_allocator, vm_id, numa_node, vcpu_count, &allocated_cpus, &actual_numa_node)) {
        g_warning("[vm.start] No isolated cores for '%s' (need %u), starting without CPU pinning",
                  vm_id, vcpu_count);
        allocated_cpus = NULL;
        actual_numa_node = -1;
    }

    VmStartContext *ctx = g_new0(VmStartContext, 1);
    ctx->vm_id = g_strdup(vm_id);
    ctx->bridge_name = g_strdup(bridge);
    ctx->allocated_cpus = allocated_cpus;
    ctx->numa_node = actual_numa_node;
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
    ctx->worker_start_us = g_get_monotonic_time();

    GTask *task = g_task_new(NULL, NULL, vm_start_callback, ctx);
    g_task_set_task_data(task, ctx, (GDestroyNotify)free_vm_start_context);

    {
        JsonNode *acc_node = json_node_new(JSON_NODE_VALUE);
        json_node_set_string(acc_node, "accepted");
        gchar *acc_resp = pure_rpc_build_success_response(rpc_id, acc_node);
        pure_uds_server_send_response(server, connection, acc_resp);
        g_free(acc_resp);
    }

    g_task_run_in_thread(task, vm_start_worker_thread);
    g_object_unref(task);
}
