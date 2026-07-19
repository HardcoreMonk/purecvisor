
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
#include "modules/dispatcher/hotplug_affect_policy.h"
#include "modules/audit/pcv_audit.h"
#include "modules/virt/virt_conn_pool.h"
#include "modules/core/vm_state.h"
#include "purecvisor/pcv_handler_util.h"
#include "api/ws_server.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_validate.h"

extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

typedef struct {
    gchar *vm_id;
    gint target_value;
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
    gboolean config_only;
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

static gboolean hotplug_get_affect_flags(virDomainPtr dom, unsigned int *flags, gboolean config_only, GError **error) {
    int active = virDomainIsActive(dom);
    if (active < 0) {
        virErrorPtr err = virGetLastError();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to read VM active state: %s",
                    err ? err->message : "Unknown");
        return FALSE;
    }

    *flags = pcv_hotplug_compute_affect_flags(active == 1, config_only);

    return TRUE;
}

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

    unsigned long memory_kb = (unsigned long)ctx->target_value * 1024;

    unsigned int flags = 0;
    GError *state_error = NULL;
    if (!hotplug_get_affect_flags(dom, &flags, ctx->config_only, &state_error)) {
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
    if (!hotplug_get_affect_flags(dom, &flags, ctx->config_only, &state_error)) {
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

static void hotplug_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmHotplugCtx *ctx = (VmHotplugCtx *)user_data;
    GError *error = NULL;

    unlock_vm_operation(ctx->vm_id);

    gboolean success = g_task_propagate_boolean(task, &error);

    if (!success) {

        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {

        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, json_node_new(JSON_NODE_NULL));
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
    }
}

void handle_vm_set_memory_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {

    if (!params || !json_object_has_member(params, "vm_id") || !json_object_has_member(params, "memory_mb")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid params: 'vm_id' or 'memory_mb' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    {
        const gchar *_lock_vm = json_object_get_string_member(params, "vm_id");
        gchar *lock_err = NULL;
        if (!lock_vm_operation(_lock_vm, VM_OP_TUNING, &lock_err)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
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

    const gchar *_apply = json_object_has_member(params, "apply")
        ? json_object_get_string_member(params, "apply") : NULL;
    ctx->config_only = (_apply && g_strcmp0(_apply, "config") == 0);

    GTask *task = g_task_new(NULL, NULL, hotplug_callback, ctx);
    g_task_set_task_data(task, ctx, free_hotplug_ctx);
    g_task_run_in_thread(task, vm_set_memory_worker);
    g_object_unref(task);
}

void handle_vm_set_vcpu_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    gint vcpu_count = 0;
    gboolean has_vcpu_count = pcv_rpc_params_get_int_alias(params, "vcpu_count", "vcpu", &vcpu_count);
    if (!has_vcpu_count) {
        has_vcpu_count = pcv_rpc_params_get_int_alias(params, "count", NULL, &vcpu_count);
    }

    if (!params || !json_object_has_member(params, "vm_id") || !has_vcpu_count) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid params: 'vm_id' and one of 'vcpu_count', 'vcpu', or 'count' required");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }

    {
        const gchar *_lock_vm = json_object_get_string_member(params, "vm_id");
        gchar *lock_err = NULL;
        if (!lock_vm_operation(_lock_vm, VM_OP_TUNING, &lock_err)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
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

    const gchar *_apply = json_object_has_member(params, "apply")
        ? json_object_get_string_member(params, "apply") : NULL;
    ctx->config_only = (_apply && g_strcmp0(_apply, "config") == 0);

    GTask *task = g_task_new(NULL, NULL, hotplug_callback, ctx);
    g_task_set_task_data(task, ctx, free_hotplug_ctx);
    g_task_run_in_thread(task, vm_set_vcpu_worker);
    g_object_unref(task);
}

void handle_device_disk_attach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    const gchar *source_dev = json_object_get_string_member(params, "source");
    const gchar *target_dev = json_object_get_string_member(params, "target");

    if (!vm_id || !source_dev || !target_dev) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing vm_id, source, or target");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    const gchar *bus = json_object_has_member(params, "bus")
        ? json_object_get_string_member(params, "bus") : "virtio";
    if (g_strcmp0(bus, "virtio") != 0 && g_strcmp0(bus, "scsi") != 0 &&
        g_strcmp0(bus, "sata")   != 0 && g_strcmp0(bus, "ide")  != 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid bus: must be virtio, scsi, sata, or ide");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }

    gchar *xml_payload = g_strdup_printf(
        "<disk type='block' device='disk'>\n"
        "  <driver name='qemu' type='raw' cache='none' io='native'/>\n"
        "  <source dev='%s'/>\n"
        "  <target dev='%s' bus='%s'/>\n"
        "</disk>", source_dev, target_dev, bus);

    unsigned int flags = VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG;

    gchar *lock_err = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(xml_payload); virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    if (virDomainAttachDeviceFlags(dom, xml_payload, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, libvirt_err ? libvirt_err->message : "Attach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, json_object_new());
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }

    unlock_vm_operation(vm_id);
    g_free(xml_payload);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

void handle_device_disk_detach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    const gchar *target_dev = json_object_get_string_member(params, "target");

    if (!vm_id || !target_dev) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing vm_id or target");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }

    gchar *live_xml = virDomainGetXMLDesc(dom, 0);
    gchar *target_tag = g_strdup_printf("<target dev='%s'", target_dev);

    gchar *target_pos = strstr(live_xml, target_tag);

    if (!target_pos) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Device not found in live XML");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        g_free(live_xml); g_free(target_tag); virDomainFree(dom); virt_conn_pool_release(conn); return;
    }

    gchar *disk_start = target_pos;
    while (disk_start >= live_xml && strncmp(disk_start, "<disk ", 6) != 0 && strncmp(disk_start, "<disk>", 6) != 0) {
        disk_start--;
    }

    gchar *disk_end = strstr(target_pos, "</disk>");
    if (disk_end) disk_end += 7;

    gchar *exact_xml = g_strndup(disk_start, disk_end - disk_start);

    unsigned int flags = VIR_DOMAIN_AFFECT_LIVE;

    gchar *lock_err = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(exact_xml); g_free(target_tag); g_free(live_xml);
        virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }
    if (virDomainDetachDeviceFlags(dom, exact_xml, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, libvirt_err ? libvirt_err->message : "Detach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, json_object_new());
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }

    unlock_vm_operation(vm_id);

    g_free(exact_xml);
    g_free(target_tag);
    g_free(live_xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

void handle_vm_mount_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    if ((!vm_id || *vm_id == '\0') && json_object_has_member(params, "name"))
        vm_id = json_object_get_string_member(params, "name");
    const gchar *iso_path = json_object_has_member(params, "iso_path")
        ? json_object_get_string_member(params, "iso_path") : NULL;

    if (!vm_id || *vm_id == '\0' || !iso_path || *iso_path == '\0') {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_id or iso_path");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    if (!pcv_validate_iso_path(iso_path)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid iso_path: must be absolute, non-empty, and must not contain '..'");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    if (!g_file_test(iso_path, G_FILE_TEST_IS_REGULAR)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid iso_path: file does not exist or is not a regular file");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
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

    gchar *lock_err = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(mount_xml); g_free(iso_escaped);
        virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    if (virDomainUpdateDeviceFlags(dom, mount_xml, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
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

    unlock_vm_operation(vm_id);
    g_free(mount_xml);
    g_free(iso_escaped);
    virDomainFree(dom); virt_conn_pool_release(conn);
}

void handle_vm_eject_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id;
    PCV_REQUIRE_PARAM(params, "vm_id", vm_id, rpc_id, server, connection);

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }

    const gchar *eject_xml =
        "<disk type='file' device='cdrom'>\n"
        "  <target dev='sda' bus='sata'/>\n"
        "</disk>";

    int flags = VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG;

    gchar *lock_err = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    if (virDomainUpdateDeviceFlags(dom, eject_xml, flags) < 0) {
        virErrorPtr libvirt_err = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, libvirt_err ? libvirt_err->message : "Failed to eject ISO");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {

        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        JsonObject *res_obj = json_object_new();
        json_object_set_boolean_member(res_obj, "ejected", TRUE);
        json_node_take_object(res_node, res_obj);

        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    unlock_vm_operation(vm_id);
    virDomainFree(dom); virt_conn_pool_release(conn);
}

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

void handle_device_nic_list(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_id");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

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
        free(xml);
    }

    JsonNode *res = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(res, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, res);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    virDomainFree(dom); virt_conn_pool_release(conn);
}

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
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_id");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

    gchar *xml = g_strdup_printf(
        "<interface type='bridge'>\n"
        "  <source bridge='%s'/>\n"
        "  <model type='%s'/>\n"
        "</interface>", bridge, model);

    gchar *lock_err = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(xml); virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainAttachDeviceFlags(dom, xml,
                 VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    g_free(xml);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "NIC attach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(res, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
    unlock_vm_operation(vm_id);
    virDomainFree(dom); virt_conn_pool_release(conn);
}

void handle_device_nic_detach(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;
    const gchar *mac   = json_object_has_member(params, "mac")
        ? json_object_get_string_member(params, "mac")   : NULL;

    if (!vm_id || !mac) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_id or mac");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

    gchar *xml = g_strdup_printf(
        "<interface type='bridge'>\n"
        "  <mac address='%s'/>\n"
        "</interface>", mac);

    gchar *lock_err = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(xml); virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainDetachDeviceFlags(dom, xml,
                 VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);
    g_free(xml);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "NIC detach failed");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    } else {
        JsonNode *res = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(res, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
    unlock_vm_operation(vm_id);
    virDomainFree(dom); virt_conn_pool_release(conn);
}

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
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing: name/vm_id, vcpu, or cpuset");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gint vcpu_id = (gint)json_object_get_int_member(params, "vcpu");

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "libvirt connection unavailable");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virNodeInfo node_info;
    if (virNodeGetInfo(conn, &node_info) < 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Failed to get host CPU info");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    int max_cpus = VIR_NODEINFO_MAXCPUS(node_info);
    int maplen = VIR_CPU_MAPLEN(max_cpus);
    unsigned char *cpumap = g_malloc0((gsize)maplen);

    gboolean parse_ok = TRUE;
    gchar **parts = g_strsplit(cpuset, ",", -1);
    for (int i = 0; parts[i] && parse_ok; i++) {
        gchar *part = g_strstrip(parts[i]);
        if (strlen(part) == 0) continue;

        gchar *dash = strchr(part, '-');
        if (dash) {

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
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid cpuset range (exceeds host CPU count or malformed)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(cpumap);
        virt_conn_pool_release(conn);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(cpumap);
        virt_conn_pool_release(conn);
        return;
    }

    gchar *lock_err = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(cpumap); virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainPinVcpu(dom, (unsigned int)vcpu_id, cpumap, maplen);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
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

    unlock_vm_operation(vm_id);
    g_free(cpumap);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

void handle_vm_set_bandwidth(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
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
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "At least one of inbound_kbps or outbound_kbps must be > 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

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
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "No network interface found on VM");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

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

    gchar *lock_err = NULL;
    if (!lock_vm_operation(name, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(iface); virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainSetInterfaceParameters(dom, iface, typed_params, nparams,
                 VIR_DOMAIN_AFFECT_LIVE);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
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

    unlock_vm_operation(name);
    g_free(iface);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

void handle_vm_memory_stats_request(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    virDomainSetMemoryStatsPeriod(dom, 5, VIR_DOMAIN_AFFECT_LIVE);

    virDomainMemoryStatStruct stats[VIR_DOMAIN_MEMORY_STAT_NR];
    int nr_stats = virDomainMemoryStats(dom, stats, VIR_DOMAIN_MEMORY_STAT_NR, 0);

    if (nr_stats < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "Failed to get memory stats");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

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

void handle_vm_cpu_stats_request(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    virDomainInfo info;
    if (virDomainGetInfo(dom, &info) < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "Failed to get domain info");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    int max_vcpu = virDomainGetVcpusFlags(dom,
        VIR_DOMAIN_AFFECT_CONFIG | VIR_DOMAIN_VCPU_MAXIMUM);
    if (max_vcpu < 0) max_vcpu = info.nrVirtCpu;

    int nr_vcpus = info.nrVirtCpu;
    virVcpuInfoPtr vcpuinfo = g_new0(virVcpuInfo, nr_vcpus);

    int host_cpus = virNodeGetCPUMap(conn, NULL, NULL, 0);
    if (host_cpus <= 0) host_cpus = 64;
    int maplen = VIR_CPU_MAPLEN(host_cpus);
    unsigned char *cpumaps = g_new0(unsigned char, nr_vcpus * maplen);

    int got = virDomainGetVcpus(dom, vcpuinfo, nr_vcpus, cpumaps, maplen);

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

typedef struct {
    gchar *vm_name;
    gchar *target;
    gint   new_size_gb;
} DiskLiveResizeCtx;

static void free_disk_live_resize_ctx(gpointer data) {
    if (!data) return;
    DiskLiveResizeCtx *ctx = (DiskLiveResizeCtx *)data;

    unlock_vm_operation(ctx->vm_name);
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
    pcv_audit_log(NULL, "vm.disk.live_resize", target, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.disk.live_resize",
                                     "failed", error_msg ? error_msg : "unknown");
    g_free(job_id);
    g_free(target);
}

static void vm_disk_live_resize_worker(GTask *task, gpointer source_obj,
                                        gpointer task_data, GCancellable *cancellable)
{
    (void)source_obj;
    (void)cancellable;

    DiskLiveResizeCtx *ctx = (DiskLiveResizeCtx *)task_data;

    gchar *volsize_arg = g_strdup_printf("volsize=%dG", ctx->new_size_gb);
    gchar *zvol_path = g_strdup_printf("pcvpool/vms/%s", ctx->vm_name);
    const gchar *zfs_argv[] = {"zfs", "set", volsize_arg, zvol_path, NULL};
    GError *zfs_err = NULL;
    pcv_spawn_sync(zfs_argv, NULL, NULL, &zfs_err);
    if (zfs_err) g_error_free(zfs_err);
    g_free(volsize_arg);
    g_free(zvol_path);

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
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid params: name, target required, new_size_gb must be > 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gchar *lock_err = NULL;
    if (!lock_vm_operation(name, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        return;
    }

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

    DiskLiveResizeCtx *ctx = g_new0(DiskLiveResizeCtx, 1);
    ctx->vm_name = g_strdup(name);
    ctx->target = g_strdup(target);
    ctx->new_size_gb = new_size_gb;

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, free_disk_live_resize_ctx);
    g_task_run_in_thread(task, vm_disk_live_resize_worker);
    g_object_unref(task);
}

void handle_vm_blkio_set(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *device = json_object_has_member(params, "device")
        ? json_object_get_string_member(params, "device") : NULL;

    if (!name || !device) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
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
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "At least one I/O limit (read/write bytes_sec or iops_sec) must be > 0");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

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

    gchar *lock_err = NULL;
    if (!lock_vm_operation(name, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    int rc = virDomainSetBlockIoTune(dom, device, typed_params, nparams,
                 VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG);

    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
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

    unlock_vm_operation(name);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

void handle_vm_blkio_get(JsonObject *params, const gchar *rpc_id,
                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *device = json_object_has_member(params, "device")
        ? json_object_get_string_member(params, "device") : NULL;

    if (!name || !device) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameters: name, device");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Failed to connect to libvirt");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, name);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
            "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virt_conn_pool_release(conn);
        return;
    }

    int nparams = 0;
    if (virDomainGetBlockIoTune(dom, device, NULL, &nparams, 0) < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "Failed to query block I/O tune params count");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

    if (nparams <= 0) {

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
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            e ? e->message : "Failed to get block I/O tune");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(blk_params);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        return;
    }

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

static gboolean pcv_validate_usb_id(const gchar *id) {
    if (!id || strlen(id) != 6) return FALSE;
    if (id[0] != '0' || (id[1] != 'x' && id[1] != 'X')) return FALSE;
    for (int i = 2; i < 6; i++) {
        if (!g_ascii_isxdigit(id[i])) return FALSE;
    }
    return TRUE;
}

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
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameters: vm_id, vendor_id, product_id");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    if (!pcv_validate_usb_id(vendor_id) || !pcv_validate_usb_id(product_id)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "vendor_id and product_id must be hex format: 0xNNNN");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "VM not found");
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

    gchar *lock_err = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(xml); virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    if (virDomainAttachDeviceFlags(dom, xml, flags) < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
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

    unlock_vm_operation(vm_id);
    g_free(xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

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
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameters: vm_id, vendor_id, product_id");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    if (!pcv_validate_usb_id(vendor_id) || !pcv_validate_usb_id(product_id)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "vendor_id and product_id must be hex format: 0xNNNN");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "VM not found");
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

    gchar *lock_err = NULL;
    if (!lock_vm_operation(vm_id, VM_OP_TUNING, &lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       lock_err ? lock_err : "VM busy (operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(lock_err);
        g_free(xml); virDomainFree(dom); virt_conn_pool_release(conn);
        return;
    }

    if (virDomainDetachDeviceFlags(dom, xml, flags) < 0) {
        virErrorPtr e = virGetLastError();
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
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

    unlock_vm_operation(vm_id);
    g_free(xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}

void handle_vm_usb_list(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_has_member(params, "vm_id")
        ? json_object_get_string_member(params, "vm_id") : NULL;

    if (!vm_id) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing: vm_id");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); return;
    }

    virConnectPtr conn;
    PCV_REQUIRE_VIRT_CONN(conn, rpc_id, server, connection);
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);

    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "VM not found");
        pure_uds_server_send_response(server, connection, err);
        g_free(err); virt_conn_pool_release(conn); return;
    }

    char *dom_xml = virDomainGetXMLDesc(dom, 0);
    JsonArray *arr = json_array_new();

    if (dom_xml) {

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
        free(dom_xml);
    }

    JsonNode *res = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(res, arr);
    gchar *usb_list_resp = pure_rpc_build_success_response(rpc_id, res);
    pure_uds_server_send_response(server, connection, usb_list_resp);
    g_free(usb_list_resp);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}
