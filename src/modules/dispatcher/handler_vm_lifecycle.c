
































#include <unistd.h>
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <libvirt/libvirt-qemu.h>
#include <libvirt/virterror.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "../audit/pcv_audit.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "api/uds_server.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/core/vm_state.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"
#include "modules/virt/cancellable_map.h"
#include "utils/pcv_config.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_validate.h"
#include "utils/pcv_log.h"
#include "modules/virt/virt_conn_pool.h"
#include "purecvisor/pcv_handler_util.h"












typedef struct {
    gchar *vm_id;
    gchar *action;
    gint cpu_quota;
    gint mem_quota_mb;
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;

    gint out_cpu_pct;
    gint out_mem_pct;
    gint out_vcpu;
    gint64 out_memory_mb;
    gint64 out_disk_rd;
    gint64 out_disk_wr;
    gint64 out_net_rx;
    gint64 out_net_tx;
    gint64 out_disk_rd_req;
    gint64 out_disk_wr_req;
    gint64 out_net_rx_pkts;
    gint64 out_net_tx_pkts;

    gint page_offset;
    gint page_limit;
} VmLifecycleCtx;








static void free_lifecycle_ctx(gpointer data) {
    if (!data) return;
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)data;
    g_free(ctx->vm_id);
    g_free(ctx->action);
    g_free(ctx->rpc_id);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

static gchar *
_extract_domain_disk_source_attr(const gchar *xml, const gchar *attr)
{
    if (!xml || !attr || !*attr)
        return NULL;

    const gchar *p = xml;
    while ((p = strstr(p, "<disk")) != NULL) {
        const gchar *tag_end = strchr(p, '>');
        const gchar *disk_end = strstr(p, "</disk>");
        if (!tag_end || !disk_end || disk_end <= tag_end)
            return NULL;

        gboolean is_primary_disk =
            g_strstr_len(p, tag_end - p, "device='disk'") ||
            g_strstr_len(p, tag_end - p, "device=\"disk\"");
        if (!is_primary_disk) {
            p = disk_end + strlen("</disk>");
            continue;
        }

        gchar *needle = g_strdup_printf("<source %s='", attr);
        const gchar *start = g_strstr_len(tag_end, disk_end - tag_end, needle);
        gchar quote = '\'';
        if (!start) {
            g_free(needle);
            needle = g_strdup_printf("<source %s=\"", attr);
            start = g_strstr_len(tag_end, disk_end - tag_end, needle);
            quote = '"';
        }

        if (start) {
            start += strlen(needle);
            const gchar *end = strchr(start, quote);
            g_free(needle);
            if (end && end <= disk_end)
                return g_strndup(start, end - start);
            return NULL;
        }

        g_free(needle);
        p = disk_end + strlen("</disk>");
    }

    return NULL;
}

typedef enum {
    PCV_VM_RENAME_DISK_NONE = 0,
    PCV_VM_RENAME_DISK_ZVOL,
    PCV_VM_RENAME_DISK_FILE,
} PcvVmRenameDiskKind;

typedef struct {
    PcvVmRenameDiskKind disk_kind;
    gchar *old_disk_path;
    gchar *new_disk_path;
    gchar *old_dataset;
    gchar *new_dataset;
    gchar *old_nvram_path;
    gchar *new_nvram_path;
} PcvVmRenamePlan;

static const gchar *_guest_get_vm_name(JsonObject *params);
virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);

static void
_vm_rename_plan_clear(PcvVmRenamePlan *plan)
{
    if (!plan) return;
    g_free(plan->old_disk_path);
    g_free(plan->new_disk_path);
    g_free(plan->old_dataset);
    g_free(plan->new_dataset);
    g_free(plan->old_nvram_path);
    g_free(plan->new_nvram_path);
    memset(plan, 0, sizeof(*plan));
}

static void
_send_rpc_error(UdsServer *server, GSocketConnection *connection,
                const gchar *rpc_id, gint code, const gchar *message)
{
    gchar *err = pure_rpc_build_error_response(rpc_id, code,
                    message ? message : "Unknown error");
    pure_uds_server_send_response(server, connection, err);
    g_free(err);
}

static const gchar *
_vm_rename_get_new_name(JsonObject *params)
{
    if (!params) return NULL;
    if (json_object_has_member(params, "new_name"))
        return json_object_get_string_member(params, "new_name");
    if (json_object_has_member(params, "target_name"))
        return json_object_get_string_member(params, "target_name");
    if (json_object_has_member(params, "target"))
        return json_object_get_string_member(params, "target");
    return NULL;
}

static xmlNodePtr
_xml_direct_child(xmlNodePtr parent, const gchar *name)
{
    if (!parent || !name) return NULL;
    for (xmlNodePtr cur = parent->children; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE &&
            xmlStrcmp(cur->name, BAD_CAST name) == 0)
            return cur;
    }
    return NULL;
}

static xmlNodePtr
_xml_find_primary_disk_source(xmlNodePtr node)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE &&
            xmlStrcmp(cur->name, BAD_CAST "disk") == 0) {
            xmlChar *device = xmlGetProp(cur, BAD_CAST "device");
            gboolean is_disk = device && xmlStrcmp(device, BAD_CAST "disk") == 0;
            if (device) xmlFree(device);
            if (is_disk) {
                xmlNodePtr source = _xml_direct_child(cur, "source");
                if (source)
                    return source;
            }
        }

        xmlNodePtr child = _xml_find_primary_disk_source(cur->children);
        if (child)
            return child;
    }
    return NULL;
}

static gboolean
_vm_rename_build_zvol_target(const gchar *old_name, const gchar *new_name,
                             const gchar *old_path, PcvVmRenamePlan *plan,
                             gchar **error_msg)
{
    if (!g_str_has_prefix(old_path, "/dev/zvol/")) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename supports only ZFS zvol or standard qcow2/raw primary disks");
        return FALSE;
    }

    gchar *dataset = g_strdup(old_path + strlen("/dev/zvol/"));
    gchar *slash = strrchr(dataset, '/');
    if (!slash || slash == dataset || *(slash + 1) == '\0') {
        if (error_msg)
            *error_msg = g_strdup("vm.rename could not parse source zvol dataset path");
        g_free(dataset);
        return FALSE;
    }

    const gchar *leaf = slash + 1;
    if (g_strcmp0(leaf, old_name) != 0) {
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.rename requires the primary zvol leaf '%s' to match VM name '%s'",
                leaf, old_name);
        }
        g_free(dataset);
        return FALSE;
    }

    gchar *parent = g_strndup(dataset, (gsize)(slash - dataset));
    gchar *target_dataset = g_strdup_printf("%s/%s", parent, new_name);
    gchar *target_path = g_strdup_printf("/dev/zvol/%s", target_dataset);

    plan->disk_kind = PCV_VM_RENAME_DISK_ZVOL;
    plan->old_disk_path = g_strdup(old_path);
    plan->new_disk_path = target_path;
    plan->old_dataset = dataset;
    plan->new_dataset = target_dataset;

    g_free(parent);
    return TRUE;
}

static gboolean
_vm_rename_file_ext_allowed(const gchar *base, const gchar *old_name,
                            const gchar **ext_out)
{
    static const gchar *exts[] = { ".qcow2", ".raw", ".img", NULL };
    for (gint i = 0; exts[i]; i++) {
        gchar *expected = g_strdup_printf("%s%s", old_name, exts[i]);
        gboolean match = g_strcmp0(base, expected) == 0;
        g_free(expected);
        if (match) {
            if (ext_out) *ext_out = exts[i];
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
_vm_rename_build_file_target(const gchar *old_name, const gchar *new_name,
                             const gchar *old_path, PcvVmRenamePlan *plan,
                             gchar **error_msg)
{
    gchar *base = g_path_get_basename(old_path);
    gchar *dir = g_path_get_dirname(old_path);
    const gchar *ext = NULL;

    if (!_vm_rename_file_ext_allowed(base, old_name, &ext)) {
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.rename requires primary file disk basename to be %s.qcow2, %s.raw, or %s.img",
                old_name, old_name, old_name);
        }
        g_free(base);
        g_free(dir);
        return FALSE;
    }

    gchar *new_base = g_strdup_printf("%s%s", new_name, ext);
    gchar *target_path = g_build_filename(dir, new_base, NULL);

    plan->disk_kind = PCV_VM_RENAME_DISK_FILE;
    plan->old_disk_path = g_strdup(old_path);
    plan->new_disk_path = target_path;

    g_free(new_base);
    g_free(base);
    g_free(dir);
    return TRUE;
}

static gboolean
_vm_rename_prepare_nvram(xmlNodePtr root, const gchar *old_name,
                         const gchar *new_name, PcvVmRenamePlan *plan,
                         gchar **error_msg)
{
    xmlNodePtr os = _xml_direct_child(root, "os");
    xmlNodePtr nvram = os ? _xml_direct_child(os, "nvram") : NULL;
    if (!nvram)
        return TRUE;

    xmlChar *content = xmlNodeGetContent(nvram);
    if (!content || !*content) {
        if (content) xmlFree(content);
        return TRUE;
    }

    gchar *old_path = g_strdup((const gchar *)content);
    xmlFree(content);
    g_strstrip(old_path);
    if (!*old_path) {
        g_free(old_path);
        return TRUE;
    }

    gchar *base = g_path_get_basename(old_path);
    gchar *expected = g_strdup_printf("%s_VARS.fd", old_name);
    if (g_strcmp0(base, expected) != 0) {
        if (error_msg) {
            *error_msg = g_strdup_printf(
                "vm.rename found non-standard NVRAM path '%s'; expected basename '%s'",
                old_path, expected);
        }
        g_free(base);
        g_free(expected);
        g_free(old_path);
        return FALSE;
    }

    gchar *dir = g_path_get_dirname(old_path);
    gchar *new_base = g_strdup_printf("%s_VARS.fd", new_name);
    gchar *new_path = g_build_filename(dir, new_base, NULL);
    xmlNodeSetContent(nvram, BAD_CAST new_path);

    plan->old_nvram_path = old_path;
    plan->new_nvram_path = new_path;

    g_free(new_base);
    g_free(dir);
    g_free(base);
    g_free(expected);
    return TRUE;
}

static gchar *
_vm_rename_build_patched_xml(const gchar *xml,
                             const gchar *old_name,
                             const gchar *new_name,
                             PcvVmRenamePlan *plan,
                             gchar **error_msg)
{
    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), "pcv-vm-rename.xml",
                                  NULL, XML_PARSE_NONET | XML_PARSE_NOERROR |
                                                XML_PARSE_NOWARNING);
    if (!doc) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename failed to parse domain XML");
        return NULL;
    }

    gchar *patched = NULL;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr name_node = root ? _xml_direct_child(root, "name") : NULL;
    if (!name_node) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename could not find domain <name> node");
        goto cleanup;
    }
    xmlNodeSetContent(name_node, BAD_CAST new_name);

    xmlNodePtr source = root ? _xml_find_primary_disk_source(root) : NULL;
    if (!source) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename requires a primary disk source");
        goto cleanup;
    }

    xmlChar *dev = xmlGetProp(source, BAD_CAST "dev");
    xmlChar *file = dev ? NULL : xmlGetProp(source, BAD_CAST "file");
    if (dev) {
        if (!_vm_rename_build_zvol_target(old_name, new_name, (const gchar *)dev,
                                          plan, error_msg)) {
            xmlFree(dev);
            goto cleanup;
        }
        xmlSetProp(source, BAD_CAST "dev", BAD_CAST plan->new_disk_path);
        xmlFree(dev);
    } else if (file) {
        if (!_vm_rename_build_file_target(old_name, new_name, (const gchar *)file,
                                          plan, error_msg)) {
            xmlFree(file);
            goto cleanup;
        }
        xmlSetProp(source, BAD_CAST "file", BAD_CAST plan->new_disk_path);
        xmlFree(file);
    } else {
        if (error_msg)
            *error_msg = g_strdup("vm.rename could not find primary disk dev/file source");
        goto cleanup;
    }

    if (!_vm_rename_prepare_nvram(root, old_name, new_name, plan, error_msg))
        goto cleanup;

    xmlChar *out = NULL;
    int out_len = 0;
    xmlDocDumpMemory(doc, &out, &out_len);
    if (!out || out_len <= 0) {
        if (error_msg)
            *error_msg = g_strdup("vm.rename failed to serialize patched XML");
        if (out) xmlFree(out);
        goto cleanup;
    }
    patched = g_strdup((const gchar *)out);
    xmlFree(out);

cleanup:
    xmlFreeDoc(doc);
    if (!patched)
        _vm_rename_plan_clear(plan);
    return patched;
}

static gboolean
_vm_rename_zfs_exists(const gchar *dataset)
{
    const gchar *argv[] = {"zfs", "list", "-H", "-o", "name", dataset, NULL};
    return pcv_spawn_sync(argv, NULL, NULL, NULL);
}

static gboolean
_vm_rename_zfs_rename(const gchar *from, const gchar *to, gchar **error_msg)
{
    gchar *stderr_s = NULL;
    const gchar *argv[] = {"zfs", "rename", from, to, NULL};
    gboolean ok = pcv_spawn_sync(argv, NULL, &stderr_s, NULL);
    if (!ok && error_msg) {
        *error_msg = g_strdup_printf("zfs rename %s -> %s failed: %s",
                                     from, to, stderr_s ? stderr_s : "unknown error");
    }
    g_free(stderr_s);
    return ok;
}

static gboolean
_vm_rename_storage_apply(PcvVmRenamePlan *plan, gchar **error_msg)
{
    if (plan->disk_kind == PCV_VM_RENAME_DISK_ZVOL) {
        if (!_vm_rename_zfs_exists(plan->old_dataset)) {
            if (error_msg)
                *error_msg = g_strdup_printf("source zvol dataset not found: %s",
                                             plan->old_dataset);
            return FALSE;
        }
        if (_vm_rename_zfs_exists(plan->new_dataset)) {
            if (error_msg)
                *error_msg = g_strdup_printf("target zvol dataset already exists: %s",
                                             plan->new_dataset);
            return FALSE;
        }
        return _vm_rename_zfs_rename(plan->old_dataset, plan->new_dataset, error_msg);
    }

    if (plan->disk_kind == PCV_VM_RENAME_DISK_FILE) {
        if (!g_file_test(plan->old_disk_path, G_FILE_TEST_EXISTS)) {
            if (error_msg)
                *error_msg = g_strdup_printf("source disk file not found: %s",
                                             plan->old_disk_path);
            return FALSE;
        }
        if (g_file_test(plan->new_disk_path, G_FILE_TEST_EXISTS)) {
            if (error_msg)
                *error_msg = g_strdup_printf("target disk file already exists: %s",
                                             plan->new_disk_path);
            return FALSE;
        }
        if (g_rename(plan->old_disk_path, plan->new_disk_path) != 0) {
            if (error_msg)
                *error_msg = g_strdup_printf("disk file rename failed: %s",
                                             g_strerror(errno));
            return FALSE;
        }
        return TRUE;
    }

    if (error_msg)
        *error_msg = g_strdup("vm.rename has no storage plan");
    return FALSE;
}

static void
_vm_rename_storage_rollback(PcvVmRenamePlan *plan)
{
    if (!plan) return;
    if (plan->disk_kind == PCV_VM_RENAME_DISK_ZVOL) {
        if (plan->new_dataset && plan->old_dataset &&
            _vm_rename_zfs_exists(plan->new_dataset)) {
            gchar *ignored = NULL;
            if (!_vm_rename_zfs_rename(plan->new_dataset, plan->old_dataset, &ignored)) {
                PCV_LOG_ERROR("vm_rename", "storage rollback failed: %s",
                              ignored ? ignored : "unknown error");
            }
            g_free(ignored);
        }
    } else if (plan->disk_kind == PCV_VM_RENAME_DISK_FILE) {
        if (plan->new_disk_path && plan->old_disk_path &&
            g_file_test(plan->new_disk_path, G_FILE_TEST_EXISTS)) {
            if (g_rename(plan->new_disk_path, plan->old_disk_path) != 0) {
                PCV_LOG_ERROR("vm_rename", "disk rollback failed: %s",
                              g_strerror(errno));
            }
        }
    }
}

static gboolean
_vm_rename_nvram_apply(PcvVmRenamePlan *plan, gchar **error_msg)
{
    if (!plan->old_nvram_path || !plan->new_nvram_path)
        return TRUE;

    if (g_file_test(plan->new_nvram_path, G_FILE_TEST_EXISTS)) {
        if (error_msg)
            *error_msg = g_strdup_printf("target NVRAM file already exists: %s",
                                         plan->new_nvram_path);
        return FALSE;
    }

    if (!g_file_test(plan->old_nvram_path, G_FILE_TEST_EXISTS))
        return TRUE;

    if (g_rename(plan->old_nvram_path, plan->new_nvram_path) != 0) {
        if (error_msg)
            *error_msg = g_strdup_printf("NVRAM file rename failed: %s",
                                         g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static void
_vm_rename_nvram_rollback(PcvVmRenamePlan *plan)
{
    if (!plan || !plan->old_nvram_path || !plan->new_nvram_path)
        return;
    if (g_file_test(plan->new_nvram_path, G_FILE_TEST_EXISTS)) {
        if (g_rename(plan->new_nvram_path, plan->old_nvram_path) != 0) {
            PCV_LOG_ERROR("vm_rename", "NVRAM rollback failed: %s",
                          g_strerror(errno));
        }
    }
}





void
handle_vm_rename_request(JsonObject *params, const gchar *rpc_id,
                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *old_name = _guest_get_vm_name(params);
    const gchar *new_name = _vm_rename_get_new_name(params);

    if (!old_name || !new_name) {
        _send_rpc_error(server, connection, rpc_id, -32602,
                        "Invalid params: 'name' and 'new_name' are required");
        return;
    }
    if (!pcv_validate_vm_name(old_name) || !pcv_validate_vm_name(new_name)) {
        _send_rpc_error(server, connection, rpc_id, -32602,
                        "Invalid VM name: only [A-Za-z0-9_-], max 64 chars");
        return;
    }
    if (g_strcmp0(old_name, new_name) == 0) {
        _send_rpc_error(server, connection, rpc_id, -32602,
                        "new_name must be different from current name");
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        _send_rpc_error(server, connection, rpc_id, -32000,
                        "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, old_name);
    if (!dom) {
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32001, "VM not found.");
        return;
    }

    virDomainPtr existing = virDomainLookupByName(conn, new_name);
    if (existing) {
        virDomainFree(existing);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32602,
                        "Target VM name already exists.");
        return;
    }
    virResetLastError();

    int active = virDomainIsActive(dom);
    if (active < 0) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32000,
                        "Could not verify VM power state.");
        return;
    }
    if (active == 1) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32000,
                        "vm.rename requires the VM to be shut off.");
        return;
    }

    int snap_count = virDomainSnapshotNum(dom, 0);
    if (snap_count > 0) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32000,
                        "vm.rename is blocked while libvirt snapshot metadata exists.");
        return;
    }
    if (snap_count < 0)
        virResetLastError();

    char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    if (!xml)
        xml = virDomainGetXMLDesc(dom, 0);
    if (!xml) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32000,
                        "Failed to read domain XML.");
        return;
    }

    PcvVmRenamePlan plan = {0};
    gchar *err_msg = NULL;
    gchar *new_xml = _vm_rename_build_patched_xml(xml, old_name, new_name,
                                                  &plan, &err_msg);
    if (!new_xml) {
        free(xml);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _send_rpc_error(server, connection, rpc_id, -32000, err_msg);
        g_free(err_msg);
        return;
    }

    gboolean storage_moved = FALSE;
    gboolean nvram_moved = FALSE;
    gboolean old_undefined = FALSE;

    if (!_vm_rename_storage_apply(&plan, &err_msg))
        goto fail;
    storage_moved = TRUE;

    if (!_vm_rename_nvram_apply(&plan, &err_msg))
        goto fail;
    nvram_moved = plan.old_nvram_path && plan.new_nvram_path &&
                  g_file_test(plan.new_nvram_path, G_FILE_TEST_EXISTS);

    int undef_rc = virDomainUndefineFlags(dom, VIR_DOMAIN_UNDEFINE_KEEP_NVRAM);
    if (undef_rc < 0) {
        virResetLastError();
        undef_rc = virDomainUndefine(dom);
    }
    if (undef_rc < 0) {
        const gchar *vir_err = virGetLastErrorMessage();
        err_msg = g_strdup_printf("Failed to undefine old VM: %s",
                                  vir_err ? vir_err : "unknown error");
        virResetLastError();
        goto fail;
    }
    old_undefined = TRUE;
    virDomainFree(dom);
    dom = NULL;

    virDomainPtr new_dom = virDomainDefineXML(conn, new_xml);
    if (!new_dom) {
        const gchar *vir_err = virGetLastErrorMessage();
        err_msg = g_strdup_printf("Failed to define renamed VM: %s",
                                  vir_err ? vir_err : "unknown error");
        virResetLastError();
        goto fail;
    }
    virDomainFree(new_dom);

#if PCV_CLUSTER_ENABLED
    pcv_cluster_remove_vm_xml(old_name);
    pcv_cluster_sync_vm_xml(new_name);
#endif

    pcv_audit_log(NULL, "vm.rename", new_name, "ok", 0, 0, "local");

    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "status", "renamed");
    json_object_set_string_member(res_obj, "old_name", old_name);
    json_object_set_string_member(res_obj, "new_name", new_name);
    json_object_set_string_member(res_obj, "storage_type",
        plan.disk_kind == PCV_VM_RENAME_DISK_ZVOL ? "zvol" : "file");
    json_object_set_string_member(res_obj, "old_disk", plan.old_disk_path);
    json_object_set_string_member(res_obj, "new_disk", plan.new_disk_path);
    json_object_set_boolean_member(res_obj, "disk_renamed", storage_moved);
    json_object_set_boolean_member(res_obj, "nvram_renamed", nvram_moved);

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    _vm_rename_plan_clear(&plan);
    g_free(new_xml);
    free(xml);
    virt_conn_pool_release(conn);
    return;

fail:
    pcv_audit_log(NULL, "vm.rename", old_name, "fail", -32000, 0, "local");
    if (nvram_moved)
        _vm_rename_nvram_rollback(&plan);
    if (storage_moved)
        _vm_rename_storage_rollback(&plan);
    if (old_undefined) {
        virDomainPtr rollback_dom = virDomainDefineXML(conn, xml);
        if (rollback_dom)
            virDomainFree(rollback_dom);
        else
            PCV_LOG_ERROR("vm_rename", "failed to restore original XML for '%s'",
                          old_name);
    }

    if (dom)
        virDomainFree(dom);
    _send_rpc_error(server, connection, rpc_id, -32000,
                    err_msg ? err_msg : "vm.rename failed");
    g_free(err_msg);
    _vm_rename_plan_clear(&plan);
    g_free(new_xml);
    free(xml);
    virt_conn_pool_release(conn);
}




















static void vm_list_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    (void)source_obj; (void)task_data;

    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                "vm.list timed out (30s)");
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }


    virDomainPtr *domains;
    int ret = virConnectListAllDomains(conn, &domains, 0);
    if (ret < 0) {
        virt_conn_pool_release(conn);
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
        if (virDomainGetInfo(domains[i], &info) < 0) {

            virDomainFree(domains[i]);
            json_object_unref(vm_obj);
            continue;
        }
        const char *state_str = (info.state == VIR_DOMAIN_RUNNING)    ? "running" :
                                (info.state == VIR_DOMAIN_SHUTOFF)    ? "shutoff" :
                                (info.state == VIR_DOMAIN_PAUSED)     ? "paused" :
                                (info.state == VIR_DOMAIN_SHUTDOWN)   ? "shutdown" :
                                (info.state == VIR_DOMAIN_CRASHED)    ? "crashed" :
                                (info.state == VIR_DOMAIN_PMSUSPENDED)? "pmsuspended" : "unknown";
        json_object_set_string_member(vm_obj, "state", state_str);
        json_object_set_int_member(vm_obj, "vcpu", (gint64)info.nrVirtCpu);
        json_object_set_int_member(vm_obj, "memory_mb", (gint64)(info.maxMem / 1024));


        char *xml = virDomainGetXMLDesc(domains[i], VIR_DOMAIN_XML_INACTIVE);
        if (xml) {

            const char *storage_type = "unknown";
            if (strstr(xml, "<disk type='block'"))
                storage_type = "zvol";
            else if (strstr(xml, "<disk type='file'"))
                storage_type = "qcow2";
            json_object_set_string_member(vm_obj, "storage_type", storage_type);


            const char *boot_mode = "bios";
            const char *loader_pos = strstr(xml, "<loader");
            if (loader_pos) {
                boot_mode = strstr(loader_pos, "secure='yes'") ? "uefi-secureboot" : "uefi";
            }
            json_object_set_string_member(vm_obj, "boot_mode", boot_mode);


            const char *disk_format = "unknown";
            const char *drv = strstr(xml, "<driver");
            if (drv) {
                const char *drv_type = strstr(drv, "type='qcow2'");
                if (drv_type)
                    disk_format = "qcow2";
                else if (strstr(drv, "type='raw'"))
                    disk_format = "raw";
            }
            json_object_set_string_member(vm_obj, "disk_format", disk_format);


            const char *disk_path = NULL;
            const char *src_dev = strstr(xml, "<source dev='");
            const char *src_file = strstr(xml, "<source file='");
            if (src_dev) {
                src_dev += 13;
                const char *end = strchr(src_dev, '\'');
                if (end) {
                    gchar *path = g_strndup(src_dev, (gsize)(end - src_dev));
                    json_object_set_string_member(vm_obj, "disk_path", path);
                    g_free(path);
                    disk_path = "set";
                }
            } else if (src_file) {
                src_file += 14;
                const char *end = strchr(src_file, '\'');
                if (end) {
                    gchar *path = g_strndup(src_file, (gsize)(end - src_file));
                    json_object_set_string_member(vm_obj, "disk_path", path);
                    g_free(path);
                    disk_path = "set";
                }
            }
            if (!disk_path)
                json_object_set_string_member(vm_obj, "disk_path", "");


            int net_count = 0;
            const char *p = xml;
            while ((p = strstr(p, "<interface type=")) != NULL) {
                net_count++;
                p++;
            }
            json_object_set_int_member(vm_obj, "network_count", (gint64)net_count);

            free(xml);
        }


        int autostart_val = 0;
        virDomainGetAutostart(domains[i], &autostart_val);
        json_object_set_boolean_member(vm_obj, "auto_start", autostart_val ? TRUE : FALSE);


        int snap_count = virDomainSnapshotNum(domains[i], 0);
        json_object_set_int_member(vm_obj, "snapshot_count", (gint64)(snap_count >= 0 ? snap_count : 0));

        json_array_add_object_element(array, vm_obj);
        virDomainFree(domains[i]);
    }
    free(domains);


    virt_conn_pool_release(conn);


    JsonNode *root_node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(root_node, array);
    g_task_return_pointer(task, root_node, (GDestroyNotify)json_node_free);
}










static void vm_list_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;


    guint tid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(task), "timeout_id"));
    if (tid > 0) g_source_remove(tid);


    JsonNode *result_node = g_task_propagate_pointer(task, &error);

    pcv_audit_log(NULL, "vm.list", "", error ? "fail" : "ok",
                  error ? PURE_RPC_ERR_ZFS_OPERATION : 0, 0, "local");
    if (error) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else if (ctx->page_limit > 0 && JSON_NODE_HOLDS_ARRAY(result_node)) {

        JsonArray *full = json_node_get_array(result_node);
        gint total = (gint)json_array_get_length(full);
        gint off = ctx->page_offset < 0 ? 0 : ctx->page_offset;
        if (off > total) off = total;

        JsonArray *paged = json_array_new();
        for (gint i = off; i < total && i < off + ctx->page_limit; i++)
            json_array_add_element(paged, json_array_dup_element(full, (guint)i));

        JsonObject *pg = json_object_new();
        json_object_set_array_member(pg, "items", paged);
        json_object_set_int_member(pg, "total", total);
        json_object_set_int_member(pg, "offset", off);
        json_object_set_int_member(pg, "limit", ctx->page_limit);
        json_object_set_boolean_member(pg, "has_more", off + ctx->page_limit < total);

        JsonNode *pg_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(pg_node, pg);
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, pg_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);
        json_node_free(result_node);
    } else {
        gchar *succ_resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, succ_resp);
        g_free(succ_resp);

    }
}








void handle_vm_list_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);


    ctx->page_offset = (params && json_object_has_member(params, "offset"))
        ? (gint)json_object_get_int_member(params, "offset") : 0;
    ctx->page_limit = (params && json_object_has_member(params, "limit"))
        ? (gint)json_object_get_int_member(params, "limit") : 0;


    if (ctx->page_offset < 0 || ctx->page_offset > 100000 ||
        (ctx->page_limit != 0 && (ctx->page_limit < 0 || ctx->page_limit > 10000))) {
        gchar *err = pure_rpc_build_error_response(rpc_id,
            PURE_RPC_ERR_INVALID_PARAMS,
            "Pagination out of range: offset 0-100000, limit 1-10000");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(ctx->rpc_id); g_object_unref(ctx->server);
        g_object_unref(ctx->connection); g_free(ctx);
        return;
    }









    GCancellable *cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, cancel, vm_list_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);

    guint timeout_id = g_timeout_add_seconds(30, (GSourceFunc)(void(*)(void))g_cancellable_cancel, cancel);
    g_object_set_data(G_OBJECT(task), "timeout_id", GUINT_TO_POINTER(timeout_id));
    g_task_run_in_thread(task, vm_list_worker);
    g_object_unref(task);
    g_object_unref(cancel);
}


























virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier) {

    virDomainPtr dom = virDomainLookupByUUIDString(conn, identifier);
    if (!dom) {

        virResetLastError();

        dom = virDomainLookupByName(conn, identifier);
    }
    return dom;
}

















static void vm_action_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    (void)source_obj;
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;
    GError *error = NULL;


    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                "vm.%s timed out (30s)", ctx->action ? ctx->action : "unknown");
        return;
    }


    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to connect to Libvirt.");
        return;
    }






    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);





    if (!dom) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "VM not found: %s", ctx->vm_id);
        virt_conn_pool_release(conn);
        g_task_return_error(task, error);
        return;
    }








    if (g_strcmp0(ctx->action, "start") == 0) {
        if (virDomainIsActive(dom)) {
            g_print("VM '%s' is already running. Skipping start sequence.\n", ctx->vm_id);
        } else if (virDomainCreate(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to start VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }

    else if (g_strcmp0(ctx->action, "stop") == 0) {
        if (!virDomainIsActive(dom)) {
            g_print("VM '%s' is already shut off. Skipping stop sequence.\n", ctx->vm_id);
        } else if (virDomainDestroy(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to stop VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }

    else if (g_strcmp0(ctx->action, "reset") == 0) {
        if (virDomainIsActive(dom)) {
            if (virDomainDestroy(dom) < 0) {
                g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to destroy VM before reset: %s", ctx->vm_id);
                virDomainFree(dom); virt_conn_pool_release(conn);
                g_task_return_error(task, error); return;
            }
        }
        if (virDomainCreate(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to reset VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }


    else if (g_strcmp0(ctx->action, "pause") == 0) {
        if (!virDomainIsActive(dom)) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "VM '%s' is not running, cannot pause.", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
        if (virDomainSuspend(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to pause VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }




    else if (g_strcmp0(ctx->action, "resume") == 0) {
        virDomainInfo info;
        if (virDomainGetInfo(dom, &info) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to query VM state: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
        if (info.state != VIR_DOMAIN_PAUSED) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "VM '%s' is not paused.", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
        if (virDomainResume(dom) < 0) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to resume VM: %s", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }
    }





    else if (g_strcmp0(ctx->action, "limit") == 0) {
        if (!virDomainIsActive(dom)) {
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Entity '%s' is not active. Cannot apply live limits.", ctx->vm_id);
            virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
        }







        if (ctx->cpu_quota > 0) {
            virTypedParameter params[1];

            strncpy(params[0].field, VIR_DOMAIN_SCHEDULER_VCPU_QUOTA, VIR_TYPED_PARAM_FIELD_LENGTH);
            params[0].type = VIR_TYPED_PARAM_LLONG;


            params[0].value.l = (long long)ctx->cpu_quota * 1000;


            if (ctx->cpu_quota == -1) {
                params[0].value.l = -1;
            } else {
                params[0].value.l = (long long)ctx->cpu_quota * 1000;
            }

            if (virDomainSetSchedulerParametersFlags(dom, params, 1, VIR_DOMAIN_AFFECT_LIVE) < 0) {
                g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to inject cgroup limits to kernel.");
                virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
            }
        }







        if (ctx->mem_quota_mb > 0) {
            virTypedParameter mem_params[1];

            strncpy(mem_params[0].field, VIR_DOMAIN_MEMORY_HARD_LIMIT, VIR_TYPED_PARAM_FIELD_LENGTH);
            mem_params[0].type = VIR_TYPED_PARAM_ULLONG;
            mem_params[0].value.ul = (unsigned long long)ctx->mem_quota_mb * 1024;


            if (ctx->mem_quota_mb == -1) {
                mem_params[0].value.ul = VIR_DOMAIN_MEMORY_PARAM_UNLIMITED;
            } else {
                mem_params[0].value.ul = (unsigned long long)ctx->mem_quota_mb * 1024;
            }

            if (virDomainSetMemoryParameters(dom, mem_params, 1, VIR_DOMAIN_AFFECT_LIVE) < 0) {
                g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to inject memory limits to kernel.");
                virDomainFree(dom); virt_conn_pool_release(conn); g_task_return_error(task, error); return;
            }
        }
    }



    virDomainFree(dom);
    virt_conn_pool_release(conn);

    g_task_return_boolean(task, TRUE);

}











static void vm_action_callback(GObject *source_obj, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;


    guint action_tid = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(task), "timeout_id"));
    if (action_tid > 0) g_source_remove(action_tid);

    gboolean success = g_task_propagate_boolean(task, &error);
    unlock_vm_operation(ctx->vm_id);




    {
        gchar *audit_method = g_strdup_printf("vm.%s", ctx->action ? ctx->action : "unknown");
        pcv_audit_log(NULL, audit_method, ctx->vm_id,
                      success ? "ok" : "fail",
                      success ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        g_free(audit_method);
    }

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















void handle_vm_stop_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");







    gchar *err_msg = NULL;
    if (!lock_vm_operation(vm_id, 2, &err_msg)) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err_msg);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp); g_free(err_msg); return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id); ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server); ctx->connection = g_object_ref(connection);

    ctx->action = g_strdup("stop");

    GCancellable *cancel = g_cancellable_new();
    GTask *task = g_task_new(NULL, cancel, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE));
    guint tid = g_timeout_add_seconds(30, (GSourceFunc)(void(*)(void))g_cancellable_cancel, cancel);
    g_object_set_data(G_OBJECT(task), "timeout_id", GUINT_TO_POINTER(tid));
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
    g_object_unref(cancel);
}










void handle_vm_pause_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp); g_free(err_resp); return;
    }
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id); ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server); ctx->connection = g_object_ref(connection);
    ctx->action = g_strdup("pause");
    GTask *task = g_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}






void handle_vm_resume_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp); g_free(err_resp); return;
    }
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id); ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server); ctx->connection = g_object_ref(connection);
    ctx->action = g_strdup("resume");
    GTask *task = g_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}













void handle_vm_limit_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {

    if (!params || !json_object_has_member(params, "vm_id")) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' missing");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }


    if (json_object_has_member(params, "cpu")) {
        gint64 cpu_val = json_object_get_int_member(params, "cpu");
        if (cpu_val != -1 && (cpu_val <= 0 || cpu_val >= 10000000)) {
            gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602,
                "Invalid params: 'cpu' must be -1 (unlimited) or 1..9999999 (microseconds)");
            pure_uds_server_send_response(server, connection, err_resp);
            g_free(err_resp);
            return;
        }
    }
    if (json_object_has_member(params, "mem")) {
        gint64 mem_val = json_object_get_int_member(params, "mem");
        if (mem_val != -1 && (mem_val <= 0 || mem_val > 1048576)) {
            gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602,
                "Invalid params: 'mem' must be -1 (unlimited) or 1..1048576 MB");
            pure_uds_server_send_response(server, connection, err_resp);
            g_free(err_resp);
            return;
        }
    }

    const gchar *vm_id_str = json_object_get_string_member_with_default(params, "vm_id", NULL);
    if (!vm_id_str || !*vm_id_str) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, -32602, "Invalid params: 'vm_id' must be non-empty string");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        return;
    }
    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id_str);
    ctx->action = g_strdup("limit");

    if (json_object_has_member(params, "cpu")) {
        ctx->cpu_quota = json_object_get_int_member(params, "cpu");
    }


    if (json_object_has_member(params, "mem")) {
        ctx->mem_quota_mb = json_object_get_int_member(params, "mem");
    }
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, vm_action_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);


    g_object_set_data(G_OBJECT(task), "is_delete", GINT_TO_POINTER(FALSE));

    g_task_run_in_thread(task, vm_action_worker);
    g_object_unref(task);
}







static void vm_metrics_callback(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    gboolean _success = g_task_propagate_boolean(task, &error);

    pcv_audit_log(NULL, "vm.metrics", ctx->vm_id ?: "",
                  _success ? "ok" : "fail",
                  _success ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (_success) {

        JsonObject *result_obj = json_object_new();
        json_object_set_int_member(result_obj, "cpu", ctx->out_cpu_pct);
        json_object_set_int_member(result_obj, "mem", ctx->out_mem_pct);
        json_object_set_int_member(result_obj, "vcpu", ctx->out_vcpu);
        json_object_set_int_member(result_obj, "memory_mb", ctx->out_memory_mb);
        json_object_set_int_member(result_obj, "disk_rd", ctx->out_disk_rd);
        json_object_set_int_member(result_obj, "disk_wr", ctx->out_disk_wr);
        json_object_set_int_member(result_obj, "net_rx", ctx->out_net_rx);
        json_object_set_int_member(result_obj, "net_tx", ctx->out_net_tx);
        json_object_set_int_member(result_obj, "disk_rd_req", ctx->out_disk_rd_req);
        json_object_set_int_member(result_obj, "disk_wr_req", ctx->out_disk_wr_req);
        json_object_set_int_member(result_obj, "net_rx_pkts", ctx->out_net_rx_pkts);
        json_object_set_int_member(result_obj, "net_tx_pkts", ctx->out_net_tx_pkts);

        JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(result_node, result_obj);

        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    } else {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    }
}










static void vm_metrics_worker(GTask *task, gpointer source_obj, gpointer task_data, GCancellable *cancellable) {
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {

        g_warning("vm_metrics_worker: connection pool exhausted for VM '%s'",
                  ctx->vm_id ? ctx->vm_id : "(null)");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Hypervisor connection pool exhausted");
        return;
    }
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);


    if (!dom || !virDomainIsActive(dom)) {
        ctx->out_cpu_pct = 0;
        ctx->out_mem_pct = 0;
    } else {
        virDomainInfo info1, info2;












        if (virDomainGetInfo(dom, &info1) < 0) {
            ctx->out_cpu_pct = 0; ctx->out_mem_pct = 0;
            goto metrics_cleanup;
        }
        g_usleep(100000);
        if (virDomainGetInfo(dom, &info2) < 0) {
            ctx->out_cpu_pct = 0; ctx->out_mem_pct = 0;
            goto metrics_cleanup;
        }

        unsigned long long time_diff = info2.cpuTime - info1.cpuTime;
        unsigned long long wall_diff = 100000000ULL * info1.nrVirtCpu;
        ctx->out_cpu_pct = (wall_diff > 0) ? (int)((time_diff * 100) / wall_diff) : 0;
        if (ctx->out_cpu_pct > 100) ctx->out_cpu_pct = 100;







        virDomainMemoryStatStruct mem_stats[VIR_DOMAIN_MEMORY_STAT_NR];
        int nr_stats = virDomainMemoryStats(dom, mem_stats, VIR_DOMAIN_MEMORY_STAT_NR, 0);
        unsigned long long mem_actual = info2.memory;
        unsigned long long mem_usable = 0;
        unsigned long long mem_unused = 0;
        unsigned long long mem_rss = 0;
        for (int i = 0; i < nr_stats; i++) {
            switch (mem_stats[i].tag) {
            case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON:
                mem_actual = mem_stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_USABLE:
                mem_usable = mem_stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_UNUSED:
                mem_unused = mem_stats[i].val;
                break;
            case VIR_DOMAIN_MEMORY_STAT_RSS:
                mem_rss = mem_stats[i].val;
                break;
            default:
                break;
            }
        }

        unsigned long long mem_used = info2.memory / 5;
        if (mem_actual > 0 && mem_usable > 0 && mem_actual > mem_usable)
            mem_used = mem_actual - mem_usable;
        else if (mem_actual > 0 && mem_unused > 0 && mem_actual > mem_unused)
            mem_used = mem_actual - mem_unused;
        else if (mem_actual > 0 && mem_rss > 0)
            mem_used = mem_rss < mem_actual ? mem_rss : mem_actual;

        ctx->out_mem_pct = (mem_actual > 0) ? (int)((mem_used * 100) / mem_actual) : 0;
        if (ctx->out_mem_pct > 100) ctx->out_mem_pct = 100;


        ctx->out_vcpu = (gint)info2.nrVirtCpu;
        ctx->out_memory_mb = (gint64)(info2.maxMem / 1024);


        virDomainBlockStatsStruct blk_stats;
        if (virDomainBlockStats(dom, "vda", &blk_stats, sizeof(blk_stats)) == 0) {
            ctx->out_disk_rd = blk_stats.rd_bytes;
            ctx->out_disk_wr = blk_stats.wr_bytes;
            ctx->out_disk_rd_req = blk_stats.rd_req;
            ctx->out_disk_wr_req = blk_stats.wr_req;
        }


        gchar *xml = virDomainGetXMLDesc(dom, 0);
        if (xml) {

            gchar *tgt = strstr(xml, "<target dev='");
            if (tgt) {
                tgt += 13;
                gchar *end = strchr(tgt, '\'');
                if (end) {
                    gchar *iface = g_strndup(tgt, end - tgt);
                    virDomainInterfaceStatsStruct if_stats;
                    if (virDomainInterfaceStats(dom, iface, &if_stats, sizeof(if_stats)) == 0) {
                        ctx->out_net_rx = if_stats.rx_bytes;
                        ctx->out_net_tx = if_stats.tx_bytes;
                        ctx->out_net_rx_pkts = if_stats.rx_packets;
                        ctx->out_net_tx_pkts = if_stats.tx_packets;
                    }
                    g_free(iface);
                }
            }
            free(xml);
        }
    }

metrics_cleanup:
    if (dom) virDomainFree(dom);
    if (conn) virt_conn_pool_release(conn);

    g_task_return_boolean(task, TRUE);
}










void handle_vm_metrics_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id;
    PCV_REQUIRE_PARAM(params, "vm_id", vm_id, rpc_id, server, connection);

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_id);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, vm_metrics_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, vm_metrics_worker);
    g_object_unref(task);
}












void handle_vm_vnc_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection) {
    const gchar *vm_id;
    PCV_REQUIRE_PARAM(params, "vm_id", vm_id, rpc_id, server, connection);

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Hypervisor connection failed");
        pure_uds_server_send_response(server, connection, err); g_free(err); return;
    }
    virDomainPtr dom = pure_virt_get_domain(conn, vm_id);
    if (!dom) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Entity not found");
        pure_uds_server_send_response(server, connection, err); g_free(err); virt_conn_pool_release(conn); return;
    }
    virDomainInfo info;
    virDomainGetInfo(dom, &info);
    if (info.state != VIR_DOMAIN_RUNNING) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM is not running. No VNC port active.");
        pure_uds_server_send_response(server, connection, err); g_free(err); virDomainFree(dom); virt_conn_pool_release(conn); return;
    }
    gchar *xml = virDomainGetXMLDesc(dom, 0);









    gchar *port_start = strstr(xml, "graphics type='vnc' port='");
    if (port_start) {
        port_start += 26;
        gchar *port_end = strchr(port_start, '\'');
        if (port_end) {
            gchar *port_str = g_strndup(port_start, port_end - port_start);
            JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
            JsonObject *res_obj = json_object_new();
            json_object_set_string_member(res_obj, "vnc_port", port_str);
            json_node_take_object(res_node, res_obj);
            gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp); g_free(port_str);
        }
    } else {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VNC Graphics adapter not found in XML");
        pure_uds_server_send_response(server, connection, err); g_free(err);
    }
    g_free(xml); virDomainFree(dom); virt_conn_pool_release(conn);
}


















typedef struct {
    gchar        *vm_id;
    gchar        *rpc_id;
    UdsServer    *server;
    GSocketConnection *connection;
} VmDeleteCtx;

static void
_vm_delete_ctx_free(VmDeleteCtx *ctx)
{
    g_free(ctx->vm_id);
    g_free(ctx->rpc_id);
    g_object_unref(ctx->server);
    g_object_unref(ctx->connection);
    g_free(ctx);
}























static void
_vm_delete_worker(GTask *task, gpointer src __attribute__((unused)),
                  gpointer task_data, GCancellable *cancel)
{
    VmDeleteCtx *ctx = task_data;
    const gchar  *vm_id = ctx->vm_id;


    if (cancel && g_cancellable_is_cancelled(cancel)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "vm.delete cancelled before start");
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    virDomainPtr  dom  = conn ? pure_virt_get_domain(conn, vm_id) : NULL;

    gchar *zvol_path  = g_strdup_printf("/dev/zvol/%s/%s", pcv_config_get_zvol_pool(), vm_id);
    gchar *zfs_dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), vm_id);
    gboolean zfs_exists = FALSE;


    gchar *file_disk_path = NULL;
    gchar *saved_xml      = NULL;
    if (dom) {
        char *xml = virDomainGetXMLDesc(dom, 0);
        if (xml) {
            saved_xml = g_strdup(xml);



            file_disk_path = _extract_domain_disk_source_attr(xml, "file");



            gchar *xml_zvol_path = _extract_domain_disk_source_attr(xml, "dev");
            if (xml_zvol_path) {
                if (g_str_has_prefix(xml_zvol_path, "/dev/zvol/")) {
                    g_free(zvol_path);
                    g_free(zfs_dataset);
                    zvol_path = xml_zvol_path;
                    zfs_dataset = g_strdup(xml_zvol_path + strlen("/dev/zvol/"));
                } else {
                    g_free(xml_zvol_path);
                }
            }
            free(xml);
        }
    }
    zfs_exists = zvol_path && access(zvol_path, F_OK) == 0;



    gboolean file_exists = file_disk_path && access(file_disk_path, F_OK) == 0;
    if (!dom && !zfs_exists && !file_exists) {
        g_free(zvol_path); g_free(zfs_dataset); g_free(file_disk_path); g_free(saved_xml);
        if (conn) virt_conn_pool_release(conn);
        PCV_LOG_INFO("vm_delete", "VM '%s': already absent (idempotent success)", vm_id);
        g_task_return_boolean(task, TRUE);
        return;
    }








    if (dom) {
        virDomainInfo info;
        virDomainGetInfo(dom, &info);
        if (info.state == VIR_DOMAIN_RUNNING || info.state == VIR_DOMAIN_PAUSED)
            virDomainDestroy(dom);
        int undef_rc = virDomainUndefineFlags(dom,
                VIR_DOMAIN_UNDEFINE_SNAPSHOTS_METADATA |
                VIR_DOMAIN_UNDEFINE_MANAGED_SAVE);
        if (undef_rc < 0) {

            if (virDomainUndefine(dom) < 0) {
                virErrorPtr e = virGetLastError();
                PCV_LOG_WARN("vm_delete", "VM '%s': undefine failed: %s",
                             vm_id, e ? e->message : "unknown");
            }
        }
        virDomainFree(dom);
    }
    if (conn) virt_conn_pool_release(conn);























    gboolean  zfs_success = TRUE;
    gchar    *zfs_err_msg = g_strdup("Success");
    gboolean  exorcism_partial = FALSE;

    if (zfs_exists) {


        GError *spawn_e = NULL;

        const gchar *fuser_argv[] = {"fuser", "-k", "-9", zvol_path, NULL};
        if (!pcv_spawn_sync(fuser_argv, NULL, NULL, &spawn_e)) {
            PCV_LOG_WARN("vm_delete", "VM '%s': fuser partial: %s",
                         vm_id, spawn_e ? spawn_e->message : "nonzero exit (non-fatal)");
            exorcism_partial = TRUE;
        }
        if (spawn_e) { g_error_free(spawn_e); spawn_e = NULL; }

        const gchar *wipefs_argv[] = {"wipefs", "-a", zvol_path, NULL};
        if (!pcv_spawn_sync(wipefs_argv, NULL, NULL, &spawn_e)) {
            PCV_LOG_WARN("vm_delete", "VM '%s': wipefs partial: %s",
                         vm_id, spawn_e ? spawn_e->message : "nonzero exit");
            exorcism_partial = TRUE;
        }
        if (spawn_e) { g_error_free(spawn_e); spawn_e = NULL; }

        gchar *dd_of = g_strdup_printf("of=%s", zvol_path);
        const gchar *dd_argv[] = {"dd", "if=/dev/zero", dd_of,
                                   "bs=1M", "count=10", "status=none", NULL};
        if (!pcv_spawn_sync(dd_argv, NULL, NULL, &spawn_e)) {
            PCV_LOG_WARN("vm_delete", "VM '%s': dd zero partial: %s",
                         vm_id, spawn_e ? spawn_e->message : "nonzero exit");
            exorcism_partial = TRUE;
        }
        g_free(dd_of);
        if (spawn_e) { g_error_free(spawn_e); spawn_e = NULL; }

        const gchar *partx_argv[] = {"partx", "-d", zvol_path, NULL};
        (void)pcv_spawn_sync(partx_argv, NULL, NULL, NULL);

        const gchar *kpartx_argv[] = {"kpartx", "-d", zvol_path, NULL};
        (void)pcv_spawn_sync(kpartx_argv, NULL, NULL, NULL);

        const gchar *partprobe_argv[] = {"partprobe", NULL};
        (void)pcv_spawn_sync(partprobe_argv, NULL, NULL, NULL);

        const gchar *udevadm_argv[] = {"udevadm", "settle", "--timeout=5", NULL};
        (void)pcv_spawn_sync(udevadm_argv, NULL, NULL, NULL);

        g_usleep(1 * G_USEC_PER_SEC);


        gchar *zfs_stderr = NULL;
        const gchar *zfs_argv[] = {"zfs", "destroy", "-R", zfs_dataset, NULL};
        if (!pcv_spawn_sync(zfs_argv, NULL, &zfs_stderr, NULL)) {
            zfs_success = FALSE;
            g_free(zfs_err_msg);
            zfs_err_msg = zfs_stderr ? g_strdup(zfs_stderr) : g_strdup("zfs destroy failed");
        }
        g_free(zfs_stderr);
    }

    g_free(zvol_path);
    g_free(zfs_dataset);

    if (!zfs_success) {



        gboolean redefined = FALSE;
        if (saved_xml) {
            virConnectPtr rc = virt_conn_pool_acquire();
            if (rc) {
                virDomainPtr rdom = virDomainDefineXML(rc, saved_xml);
                if (rdom) {
                    virDomainFree(rdom);
                    redefined = TRUE;
                    PCV_LOG_WARN("vm_delete",
                        "VM '%s': ZFS destroy failed — definition restored from saved XML. "
                        "Delete can be retried.", vm_id);
                } else {
                    virErrorPtr e = virGetLastError();
                    PCV_LOG_ERROR("vm_delete",
                        "VM '%s': ZFS destroy failed AND redefine failed: %s. "
                        "Manual recovery required: restore XML and investigate zvol state.",
                        vm_id, e ? e->message : "unknown");
                }
                virt_conn_pool_release(rc);
            }
        }

        gchar *reason = g_strdup_printf(
            "ZFS destroy failed%s: %s",
            redefined ? " (VM definition rolled back)" : " (VM XML gone — manual recovery)",
            zfs_err_msg);
        g_free(zfs_err_msg);
        g_free(file_disk_path);
        g_free(saved_xml);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", reason);
        g_free(reason);
        return;
    }
    g_free(zfs_err_msg);
    g_free(saved_xml);










    if (file_disk_path && !g_str_has_prefix(file_disk_path, "/dev/")) {
        if (access(file_disk_path, F_OK) == 0) {
            if (unlink(file_disk_path) == 0) {
                PCV_LOG_INFO("vm_delete", "VM '%s': disk file deleted: %s",
                             vm_id, file_disk_path);
            } else {
                int err = errno;
                PCV_LOG_ERROR("vm_delete", "VM '%s': failed to delete disk file '%s': %s",
                              vm_id, file_disk_path, g_strerror(err));
                gchar *reason = g_strdup_printf(
                    "VM definition removed, but disk file cleanup failed: %s (%s). "
                    "Manual cleanup required.",
                    file_disk_path, g_strerror(err));
                g_free(file_disk_path);
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", reason);
                g_free(reason);
                return;
            }
        }
    }
    g_free(file_disk_path);

    if (exorcism_partial) {
        PCV_LOG_WARN("vm_delete", "VM '%s': delete succeeded with partial exorcism (check above warnings)", vm_id);
    }
    g_task_return_boolean(task, TRUE);
}













static void
_vm_delete_callback(GObject *src __attribute__((unused)), GAsyncResult *res,
                    gpointer user_data)
{
    VmDeleteCtx *ctx = user_data;
    GError      *err = NULL;

    gboolean ok = g_task_propagate_boolean(G_TASK(res), &err);

    pcv_audit_log(NULL, "vm.delete", ctx->vm_id,
                  ok ? "ok" : "fail",
                  ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION,
                  0, "local");

    cmap_remove(ctx->vm_id);
    if (!ok) {
        g_warning("[vm.delete] background worker failed for '%s': %s",
                  ctx->vm_id, err ? err->message : "unknown");
        if (err) g_error_free(err);
    } else {
        g_message("[vm.delete] ZFS destroy complete for '%s'", ctx->vm_id);
    }
    _vm_delete_ctx_free(ctx);
}

















void handle_vm_delete_request(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_id = json_object_get_string_member(params, "vm_id");
    if (!vm_id || !pcv_validate_vm_name(vm_id)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
            "Missing or invalid param: vm_id (alphanumeric, -, _ only)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }











    JsonNode *acc_node = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(acc_node, "accepted");
    gchar *acc_resp = pure_rpc_build_success_response(rpc_id, acc_node);
    pure_uds_server_send_response(server, connection, acc_resp);
    g_free(acc_resp);

    VmDeleteCtx *ctx = g_new0(VmDeleteCtx, 1);
    ctx->vm_id      = g_strdup(vm_id);
    ctx->rpc_id     = g_strdup(rpc_id);
    ctx->server     = g_object_ref(server);
    ctx->connection = g_object_ref(connection);




    GCancellable *cancel = g_cancellable_new();
    cmap_register(vm_id, cancel);






    GTask *task = g_task_new(NULL, cancel, _vm_delete_callback, ctx);
    g_task_set_task_data(task, ctx, NULL);
    g_task_run_in_thread(task, _vm_delete_worker);
    g_object_unref(task);
    g_object_unref(cancel);
}




























gchar *handle_vm_create(JsonObject *params, GError **error) {


    const gchar *vm_name = NULL;
    if (json_object_has_member(params, "name"))
        vm_name = json_object_get_string_member(params, "name");

    if (!vm_name || strlen(vm_name) == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "VM name is required.");
        return NULL;
    }


    gint vcpu = 2;
    if (json_object_has_member(params, "vcpu"))
        vcpu = (gint)json_object_get_int_member(params, "vcpu");
    if (vcpu < 1) vcpu = 1;


    gint64 memory_mb = 2048;
    if (json_object_has_member(params, "memory_mb"))
        memory_mb = json_object_get_int_member(params, "memory_mb");
    if (memory_mb < 512) memory_mb = 512;
    gint64 memory_kib = memory_mb * 1024;


    gint disk_size_gb = 0;
    if (json_object_has_member(params, "disk_size_gb"))
        disk_size_gb = (gint)json_object_get_int_member(params, "disk_size_gb");


    const gchar *iso_path = NULL;
    if (json_object_has_member(params, "iso_path"))
        iso_path = json_object_get_string_member(params, "iso_path");


    const gchar *net_bridge = NULL;
    if (json_object_has_member(params, "network_bridge"))
        net_bridge = json_object_get_string_member(params, "network_bridge");




    const gchar *ovn_switch = NULL;
    if (json_object_has_member(params, "ovn_switch")) {
        ovn_switch = json_object_get_string_member(params, "ovn_switch");
        if (ovn_switch && *ovn_switch) {
            net_bridge = "br-int";
        }
    }


    GError *validate_err = NULL;
    if (!pcv_validate_vm_create_params(vm_name, vcpu, (gint)memory_mb,
                                       disk_size_gb, iso_path, net_bridge,
                                       &validate_err)) {
        g_propagate_error(error, validate_err);
        return NULL;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Libvirt connection failed.");
        return NULL;
    }


    const gchar *storage_type = "zvol";
    if (json_object_has_member(params, "storage_type"))
        storage_type = json_object_get_string_member(params, "storage_type");


    gchar *disk_xml = g_strdup("");
    if (disk_size_gb > 0) {
        if (g_strcmp0(storage_type, "qcow2") == 0) {

            const gchar *img_dir = pcv_config_get_image_dir();
            gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2", img_dir, vm_name);

            gchar *size_str = g_strdup_printf("%dG", disk_size_gb);
            const gchar *qimg_argv[] = {
                "qemu-img", "create", "-f", "qcow2", qcow2_path, size_str, NULL
            };
            GError *qerr = NULL;
            pcv_spawn_sync(qimg_argv, NULL, NULL, &qerr);
            if (qerr) {
                g_warning("qemu-img create failed: %s", qerr->message);
                g_error_free(qerr);
            }
            g_free(size_str);
            g_free(disk_xml);
            disk_xml = g_strdup_printf(
                "    <disk type='file' device='disk'>"
                  "<driver name='qemu' type='qcow2'/>"
                  "<source file='%s'/>"
                  "<target dev='vda' bus='virtio'/>"
                "</disk>", qcow2_path);
            g_free(qcow2_path);
        } else if (g_strcmp0(storage_type, "raw") == 0) {

            const gchar *img_dir = pcv_config_get_image_dir();
            gchar *raw_path = g_strdup_printf("%s/%s.raw", img_dir, vm_name);
            gchar *size_str = g_strdup_printf("%dG", disk_size_gb);
            const gchar *qimg_argv[] = {
                "qemu-img", "create", "-f", "raw", raw_path, size_str, NULL
            };
            GError *qerr = NULL;
            pcv_spawn_sync(qimg_argv, NULL, NULL, &qerr);
            if (qerr) {
                g_warning("qemu-img create (raw) failed: %s", qerr->message);
                g_error_free(qerr);
            }
            g_free(size_str);
            g_free(disk_xml);
            disk_xml = g_strdup_printf(
                "    <disk type='file' device='disk'>"
                  "<driver name='qemu' type='raw' cache='none' io='native'/>"
                  "<source file='%s'/>"
                  "<target dev='vda' bus='virtio'/>"
                "</disk>", raw_path);
            g_free(raw_path);
        } else {

            gchar *zvol_dev = g_strdup_printf("/dev/zvol/%s/%s", pcv_config_get_zvol_pool(), vm_name);
            g_free(disk_xml);
            disk_xml = g_strdup_printf(
                "    <disk type='block' device='disk'>"
                  "<driver name='qemu' type='raw'/>"
                  "<source dev='%s'/>"
                  "<target dev='vda' bus='virtio'/>"
                "</disk>", zvol_dev);
            g_free(zvol_dev);
        }
    }


    gchar *cdrom_xml = g_strdup("");
    if (iso_path && strlen(iso_path) > 0) {
        g_free(cdrom_xml);
        cdrom_xml = g_strdup_printf(
            "    <disk type='file' device='cdrom'>"
              "<driver name='qemu' type='raw'/>"
              "<source file='%s'/>"
              "<target dev='sda' bus='sata'/>"
              "<readonly/>"
            "</disk>", iso_path);
    }


    const gchar *boot_xml = (iso_path && strlen(iso_path) > 0)
        ? "<boot dev='cdrom'/><boot dev='hd'/>"
        : "<boot dev='hd'/>";


    gchar *net_xml = NULL;
    if (net_bridge && strlen(net_bridge) > 0) {

        const gchar *ovs_av[] = {"ovs-vsctl", "br-exists", net_bridge, NULL};
        gboolean is_ovs = pcv_spawn_sync(ovs_av, NULL, NULL, NULL);

        net_xml = is_ovs
            ? g_strdup_printf(
                "    <interface type='bridge'>"
                  "<source bridge='%s'/>"
                  "<virtualport type='openvswitch'/>"
                  "<model type='virtio'/>"
                "</interface>", net_bridge)
            : g_strdup_printf(
                "    <interface type='bridge'>"
                  "<source bridge='%s'/>"
                  "<model type='virtio'/>"
                "</interface>", net_bridge);
    } else {
        net_xml = g_strdup(
            "    <interface type='network'>"
              "<source network='default'/>"
              "<model type='virtio'/>"
            "</interface>");
    }














    gchar *xml_str = g_strdup_printf(
        "<domain type='kvm'>"
          "<name>%s</name>"
          "<memory unit='KiB'>%lld</memory>"
          "<currentMemory unit='KiB'>%lld</currentMemory>"
          "<vcpu placement='static'>%d</vcpu>"
          "<os>"
            "<type arch='x86_64' machine='pc-q35-7.2'>hvm</type>"
            "%s"
          "</os>"
          "<features><acpi/><apic/></features>"
          "<cpu mode='host-passthrough' check='none'/>"
          "<devices>"
            "<emulator>/usr/bin/qemu-system-x86_64</emulator>"
            "%s"
            "%s"
            "%s"
            "<graphics type='vnc' port='-1' autoport='yes' listen='0.0.0.0'/>"
            "<video><model type='virtio'/></video>"
            "<channel type='unix'>"
              "<target type='virtio' name='org.qemu.guest_agent.0'/>"
            "</channel>"
          "</devices>"
        "</domain>",
        vm_name,
        (long long)memory_kib, (long long)memory_kib,
        vcpu,
        boot_xml,
        disk_xml, cdrom_xml, net_xml);

    g_free(disk_xml);
    g_free(cdrom_xml);
    g_free(net_xml);










    virDomainPtr dom = virDomainDefineXML(conn, xml_str);
    g_free(xml_str);
    if (!dom) {
        virErrorPtr libvirt_err = virGetLastError();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create VM: %s", libvirt_err ? libvirt_err->message : "Unknown Libvirt Error");
        virt_conn_pool_release(conn);
        return NULL;
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);














    if (ovn_switch && *ovn_switch) {
        gchar *port_name = g_strdup_printf("%s-port", vm_name);

        const gchar *add_argv[] = {"ovn-nbctl", "lsp-add", ovn_switch, port_name, NULL};
        pcv_spawn_sync(add_argv, NULL, NULL, NULL);

        const gchar *addr_argv[] = {"ovn-nbctl", "lsp-set-addresses", port_name, "dynamic", NULL};
        pcv_spawn_sync(addr_argv, NULL, NULL, NULL);
        PCV_LOG_INFO("vm_manager", "OVN port '%s' created on switch '%s' for VM '%s'",
                     port_name, ovn_switch, vm_name);
        g_free(port_name);
    }

    JsonObject *rpc_resp = json_object_new();
    json_object_set_string_member(rpc_resp, "jsonrpc", "2.0");
    json_object_set_string_member(rpc_resp, "id", "create-req");
    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "status", "success");
    json_object_set_string_member(res_obj, "message", "VM Created Successfully.");
    json_object_set_object_member(rpc_resp, "result", res_obj);

    JsonNode *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, rpc_resp);
    gchar *response_str = json_to_string(root_node, FALSE);
    json_node_free(root_node);

    return response_str;
}
















static const gchar *
_guest_get_vm_name(JsonObject *params)
{
    if (!params) return NULL;
    if (json_object_has_member(params, "name"))
        return json_object_get_string_member(params, "name");
    if (json_object_has_member(params, "vm_id"))
        return json_object_get_string_member(params, "vm_id");
    return NULL;
}

static gboolean
_guest_agent_xml_has_channel(const gchar *xml)
{
    return xml && strstr(xml, "org.qemu.guest_agent.0") != NULL;
}

static void
_guest_agent_add_install_commands(JsonObject *obj)
{
    JsonObject *cmds = json_object_new();
    json_object_set_string_member(cmds, "debian_ubuntu",
        "sudo apt update && sudo apt install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent");
    json_object_set_string_member(cmds, "rhel_rocky_fedora",
        "sudo dnf install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent");
    json_object_set_string_member(cmds, "suse",
        "sudo zypper install -y qemu-guest-agent && sudo systemctl enable --now qemu-guest-agent");
    json_object_set_object_member(obj, "install_commands", cmds);
}





void
handle_vm_guest_agent_status_request(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
                         "Failed to connect to Libvirt.");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, vm_name);
    if (!dom) {
        virt_conn_pool_release(conn);
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001,
                         "VM not found.");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gboolean running = virDomainIsActive(dom) == 1;
    char *config_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    char *live_xml = virDomainGetXMLDesc(dom, 0);
    gboolean channel_configured = _guest_agent_xml_has_channel(config_xml);
    gboolean channel_live = _guest_agent_xml_has_channel(live_xml);
    gboolean agent_ping = FALSE;
    gchar *agent_error = NULL;

    if (running && channel_live) {
        char *result = virDomainQemuAgentCommand(dom, "{\"execute\":\"guest-ping\"}", 3, 0);
        if (result) {
            agent_ping = TRUE;
            free(result);
        } else {
            const char *vir_err = virGetLastErrorMessage();
            agent_error = g_strdup(vir_err ? vir_err : "unknown error");
            virResetLastError();
        }
    }

    const gchar *status = "channel_missing";
    const gchar *message = "Guest agent channel is not configured.";
    if (agent_ping) {
        status = "ok";
        message = "Guest agent is responding.";
    } else if (!running && (channel_configured || channel_live)) {
        status = "vm_stopped";
        message = "Guest agent channel is configured; start the VM to verify the agent.";
    } else if (running && channel_configured && !channel_live) {
        status = "reboot_required";
        message = "Guest agent channel is configured for the next boot; restart the VM or attach it live.";
    } else if (running && channel_live) {
        status = "agent_unavailable";
        message = "Guest agent channel exists, but qemu-guest-agent is not responding in the guest.";
    }

    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "name", vm_name);
    json_object_set_string_member(res_obj, "status", status);
    json_object_set_string_member(res_obj, "message", message);
    json_object_set_boolean_member(res_obj, "running", running);
    json_object_set_boolean_member(res_obj, "channel_present", channel_configured || channel_live);
    json_object_set_boolean_member(res_obj, "channel_configured", channel_configured);
    json_object_set_boolean_member(res_obj, "channel_live", channel_live);
    json_object_set_boolean_member(res_obj, "agent_ping", agent_ping);
    json_object_set_boolean_member(res_obj, "reboot_required", running && channel_configured && !channel_live);
    json_object_set_boolean_member(res_obj, "package_required", running && channel_live && !agent_ping);
    json_object_set_boolean_member(res_obj, "can_ensure_channel", TRUE);
    if (agent_error)
        json_object_set_string_member(res_obj, "agent_error", agent_error);
    _guest_agent_add_install_commands(res_obj);

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    g_free(agent_error);
    if (config_xml) free(config_xml);
    if (live_xml) free(live_xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}





void
handle_vm_guest_agent_ensure_channel_request(JsonObject *params, const gchar *rpc_id,
                                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
                         "Failed to connect to Libvirt.");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, vm_name);
    if (!dom) {
        virt_conn_pool_release(conn);
        gchar *err = pure_rpc_build_error_response(rpc_id, -32001,
                         "VM not found.");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gboolean running = virDomainIsActive(dom) == 1;
    gboolean persistent = virDomainIsPersistent(dom) == 1;
    char *config_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    char *live_xml = virDomainGetXMLDesc(dom, 0);
    gboolean channel_configured = _guest_agent_xml_has_channel(config_xml);
    gboolean channel_live = _guest_agent_xml_has_channel(live_xml);
    gboolean config_changed = FALSE;
    gboolean live_changed = FALSE;
    gchar *config_error = NULL;
    gchar *live_error = NULL;

    const gchar *channel_xml =
        "<channel type='unix'>"
          "<target type='virtio' name='org.qemu.guest_agent.0'/>"
        "</channel>";

    if (!channel_configured && persistent) {
        if (virDomainAttachDeviceFlags(dom, channel_xml, VIR_DOMAIN_AFFECT_CONFIG) == 0) {
            channel_configured = TRUE;
            config_changed = TRUE;
        } else {
            const char *vir_err = virGetLastErrorMessage();
            config_error = g_strdup(vir_err ? vir_err : "unknown error");
            virResetLastError();
        }
    }

    if (running && !channel_live) {
        if (virDomainAttachDeviceFlags(dom, channel_xml, VIR_DOMAIN_AFFECT_LIVE) == 0) {
            channel_live = TRUE;
            live_changed = TRUE;
        } else {
            const char *vir_err = virGetLastErrorMessage();
            live_error = g_strdup(vir_err ? vir_err : "unknown error");
            virResetLastError();
        }
    }

    if (!channel_configured && !channel_live) {
        gchar *msg = g_strdup_printf(
            "Failed to add guest agent channel.%s%s%s%s",
            config_error ? " config: " : "", config_error ? config_error : "",
            live_error ? " live: " : "", live_error ? live_error : "");
        gchar *err = pure_rpc_build_error_response(rpc_id, -32000, msg);
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        g_free(msg);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "name", vm_name);
        json_object_set_string_member(res_obj, "status",
            (config_changed || live_changed) ? "updated" : "already_configured");
        json_object_set_boolean_member(res_obj, "running", running);
        json_object_set_boolean_member(res_obj, "persistent", persistent);
        json_object_set_boolean_member(res_obj, "changed", config_changed || live_changed);
        json_object_set_boolean_member(res_obj, "channel_configured", channel_configured);
        json_object_set_boolean_member(res_obj, "channel_live", channel_live);
        json_object_set_boolean_member(res_obj, "reboot_required", channel_configured && running && !channel_live);
        json_object_set_boolean_member(res_obj, "install_required", TRUE);
        if (config_error)
            json_object_set_string_member(res_obj, "config_warning", config_error);
        if (live_error)
            json_object_set_string_member(res_obj, "live_warning", live_error);
        _guest_agent_add_install_commands(res_obj);

        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, res_node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }

    g_free(config_error);
    g_free(live_error);
    if (config_xml) free(config_xml);
    if (live_xml) free(live_xml);
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}





static gboolean
_guest_fsinfo_get_int64(JsonObject *obj, const gchar *member, gint64 *out)
{
    if (!obj || !member || !json_object_has_member(obj, member))
        return FALSE;
    *out = json_object_get_int_member(obj, member);
    return TRUE;
}

static gboolean
_guest_fsinfo_should_count(const gchar *type)
{
    static const gchar *skip[] = {
        "tmpfs", "devtmpfs", "proc", "sysfs", "devpts",
        "cgroup", "cgroup2", "squashfs", "overlay", NULL
    };

    if (!type || !*type)
        return TRUE;
    for (guint i = 0; skip[i]; i++) {
        if (g_strcmp0(type, skip[i]) == 0)
            return FALSE;
    }
    return TRUE;
}

static void
_guest_fsinfo_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                     gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }

    char *agent_result = virDomainQemuAgentCommand(
        dom, "{\"execute\":\"guest-get-fsinfo\"}", 10, 0);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    if (!agent_result) {
        const char *vir_err = virGetLastErrorMessage();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "guest-get-fsinfo failed on '%s': %s",
                                ctx->vm_id, vir_err ? vir_err : "unknown error");
        return;
    }

    GError *parse_error = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, agent_result, -1, &parse_error)) {
        free(agent_result);
        g_object_unref(parser);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to parse guest-get-fsinfo response: %s",
                                parse_error ? parse_error->message : "invalid JSON");
        g_clear_error(&parse_error);
        return;
    }
    free(agent_result);

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *root_obj = root && JSON_NODE_HOLDS_OBJECT(root)
        ? json_node_get_object(root) : NULL;
    if (!root_obj || !json_object_has_member(root_obj, "return")) {
        g_object_unref(parser);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "guest-get-fsinfo response missing return array.");
        return;
    }

    JsonArray *raw_filesystems = json_object_get_array_member(root_obj, "return");
    JsonArray *filesystems = json_array_new();
    gint64 total_bytes = 0;
    gint64 used_bytes = 0;

    for (guint i = 0; raw_filesystems && i < json_array_get_length(raw_filesystems); i++) {
        JsonObject *raw = json_array_get_object_element(raw_filesystems, i);
        if (!raw)
            continue;

        const gchar *name = json_object_has_member(raw, "name")
            ? json_object_get_string_member(raw, "name") : "";
        const gchar *mountpoint = json_object_has_member(raw, "mountpoint")
            ? json_object_get_string_member(raw, "mountpoint") : "";
        const gchar *type = json_object_has_member(raw, "type")
            ? json_object_get_string_member(raw, "type") : "";

        gint64 fs_total = 0;
        gint64 fs_used = 0;
        gboolean has_total = _guest_fsinfo_get_int64(raw, "total-bytes", &fs_total);
        gboolean has_used = _guest_fsinfo_get_int64(raw, "used-bytes", &fs_used);

        JsonObject *fs_obj = json_object_new();
        json_object_set_string_member(fs_obj, "name", name);
        json_object_set_string_member(fs_obj, "mountpoint", mountpoint);
        json_object_set_string_member(fs_obj, "type", type);
        if (has_total)
            json_object_set_int_member(fs_obj, "total_bytes", fs_total);
        if (has_used)
            json_object_set_int_member(fs_obj, "used_bytes", fs_used);
        if (has_total && has_used && fs_total >= fs_used) {
            json_object_set_int_member(fs_obj, "available_bytes", fs_total - fs_used);
            if (fs_total > 0) {
                double pct = ((double)fs_used * 100.0) / (double)fs_total;
                json_object_set_double_member(fs_obj, "usage_percent", pct);
            }
        }

        if (json_object_has_member(raw, "disk")) {
            JsonArray *disks = json_object_get_array_member(raw, "disk");
            if (disks && json_array_get_length(disks) > 0) {
                JsonObject *disk = json_array_get_object_element(disks, 0);
                if (disk && json_object_has_member(disk, "dev"))
                    json_object_set_string_member(fs_obj, "device",
                                                  json_object_get_string_member(disk, "dev"));
            }
        }

        if (has_total && has_used && fs_total > 0 && _guest_fsinfo_should_count(type)) {
            total_bytes += fs_total;
            used_bytes += fs_used;
        }

        json_array_add_object_element(filesystems, fs_obj);
    }

    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "name", ctx->vm_id);
    json_object_set_string_member(res_obj, "status", "ok");
    json_object_set_int_member(res_obj, "total_bytes", total_bytes);
    json_object_set_int_member(res_obj, "used_bytes", used_bytes);
    if (total_bytes > 0)
        json_object_set_double_member(res_obj, "usage_percent",
                                      ((double)used_bytes * 100.0) / (double)total_bytes);
    json_object_set_array_member(res_obj, "filesystems", filesystems);

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    g_object_unref(parser);
    g_task_return_pointer(task, res_node, (GDestroyNotify)json_node_free);
}

static void
_guest_fsinfo_callback(GObject *source_obj __attribute__((unused)),
                       GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    JsonNode *result_node = g_task_propagate_pointer(task, &error);

    pcv_audit_log(NULL, "vm.guest.fsinfo", ctx->vm_id ?: "",
                  error ? "fail" : "ok",
                  error ? PURE_RPC_ERR_ZFS_OPERATION : 0, 0, "local");

    if (error) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id,
            PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

void
handle_vm_guest_fsinfo_request(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _guest_fsinfo_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, _guest_fsinfo_worker);
    g_object_unref(task);
}











static void
_guest_ping_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                   gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }


    char *result = virDomainQemuAgentCommand(dom, "{\"execute\":\"guest-ping\"}",
                                              5, 0);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    if (!result) {
        const char *vir_err = virGetLastErrorMessage();
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Guest agent not available on '%s': %s",
                                ctx->vm_id, vir_err ? vir_err : "unknown error");
        return;
    }

    free(result);
    g_task_return_boolean(task, TRUE);
}

static void
_guest_ping_callback(GObject *source_obj __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    gboolean _ok = g_task_propagate_boolean(task, &error);

    pcv_audit_log(NULL, "vm.guest.ping", ctx->vm_id ?: "",
                  _ok ? "ok" : "fail",
                  _ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (!_ok) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "agent", "connected");
        json_object_set_string_member(res_obj, "name", ctx->vm_id);
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, res_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

void
handle_vm_guest_ping_request(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _guest_ping_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, _guest_ping_worker);
    g_object_unref(task);
}

















typedef struct {
    gchar *vm_id;
    gchar *command;
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
} GuestExecCtx;

static void
_guest_exec_ctx_free(gpointer data)
{
    if (!data) return;
    GuestExecCtx *ctx = (GuestExecCtx *)data;
    g_free(ctx->vm_id);
    g_free(ctx->command);
    g_free(ctx->rpc_id);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

static void
_guest_exec_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                   gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    GuestExecCtx *ctx = (GuestExecCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }







    GString *safe_cmd = g_string_new(NULL);
    for (const gchar *p = ctx->command; *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_c(safe_cmd, '\\');
        g_string_append_c(safe_cmd, *p);
    }

    gchar *exec_json = g_strdup_printf(
        "{\"execute\":\"guest-exec\",\"arguments\":"
        "{\"path\":\"/bin/sh\",\"arg\":[\"-c\",\"%s\"],\"capture-output\":true}}",
        safe_cmd->str);
    g_string_free(safe_cmd, TRUE);

    char *exec_result = virDomainQemuAgentCommand(dom, exec_json,
                                                   30, 0);
    g_free(exec_json);

    if (!exec_result) {
        const char *vir_err = virGetLastErrorMessage();
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "guest-exec failed on '%s': %s",
                                ctx->vm_id, vir_err ? vir_err : "unknown error");
        return;
    }


    JsonParser *parser = json_parser_new();
    gint64 pid = -1;
    if (json_parser_load_from_data(parser, exec_result, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "return")) {
                JsonObject *ret_obj = json_object_get_object_member(obj, "return");
                if (ret_obj && json_object_has_member(ret_obj, "pid"))
                    pid = json_object_get_int_member(ret_obj, "pid");
            }
        }
    }
    g_object_unref(parser);
    free(exec_result);

    if (pid < 0) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to parse guest-exec PID from response.");
        return;
    }


    g_usleep(500000);

    gchar *status_json = g_strdup_printf(
        "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":%" G_GINT64_FORMAT "}}",
        pid);

    char *status_result = virDomainQemuAgentCommand(dom, status_json,
                                                     30, 0);
    g_free(status_json);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    if (!status_result) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "guest-exec-status failed for PID %" G_GINT64_FORMAT, pid);
        return;
    }


    JsonParser *sp = json_parser_new();
    gint64 exitcode = -1;
    gchar *stdout_decoded = NULL;
    gchar *stderr_decoded = NULL;

    if (json_parser_load_from_data(sp, status_result, -1, NULL)) {
        JsonNode *sroot = json_parser_get_root(sp);
        if (sroot && JSON_NODE_HOLDS_OBJECT(sroot)) {
            JsonObject *sobj = json_node_get_object(sroot);
            if (json_object_has_member(sobj, "return")) {
                JsonObject *sret = json_object_get_object_member(sobj, "return");
                if (sret) {
                    if (json_object_has_member(sret, "exitcode"))
                        exitcode = json_object_get_int_member(sret, "exitcode");

                    if (json_object_has_member(sret, "out-data")) {
                        const gchar *b64 = json_object_get_string_member(sret, "out-data");
                        if (b64) {
                            gsize out_len = 0;
                            guchar *decoded = g_base64_decode(b64, &out_len);
                            stdout_decoded = g_strndup((const gchar *)decoded, out_len);
                            g_free(decoded);
                        }
                    }

                    if (json_object_has_member(sret, "err-data")) {
                        const gchar *b64 = json_object_get_string_member(sret, "err-data");
                        if (b64) {
                            gsize out_len = 0;
                            guchar *decoded = g_base64_decode(b64, &out_len);
                            stderr_decoded = g_strndup((const gchar *)decoded, out_len);
                            g_free(decoded);
                        }
                    }
                }
            }
        }
    }
    g_object_unref(sp);
    free(status_result);


    JsonObject *res_obj = json_object_new();
    json_object_set_string_member(res_obj, "name", ctx->vm_id);
    json_object_set_int_member(res_obj, "exitcode", exitcode);
    json_object_set_string_member(res_obj, "stdout", stdout_decoded ? stdout_decoded : "");
    json_object_set_string_member(res_obj, "stderr", stderr_decoded ? stderr_decoded : "");

    g_free(stdout_decoded);
    g_free(stderr_decoded);

    JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(res_node, res_obj);
    g_task_return_pointer(task, res_node, (GDestroyNotify)json_node_free);
}

static void
_guest_exec_callback(GObject *source_obj __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    GuestExecCtx *ctx = (GuestExecCtx *)user_data;
    GError *error = NULL;

    JsonNode *result_node = g_task_propagate_pointer(task, &error);

    pcv_audit_log(NULL, "vm.guest.exec", ctx->vm_id ?: "",
                  error ? "fail" : "ok",
                  error ? PURE_RPC_ERR_ZFS_OPERATION : 0, 0, "local");
    if (error) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, result_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

void
handle_vm_guest_exec_request(JsonObject *params, const gchar *rpc_id,
                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    const gchar *command = NULL;
    if (params && json_object_has_member(params, "command"))
        command = json_object_get_string_member(params, "command");

    if (!command || strlen(command) == 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'command' must be non-empty");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    if (strlen(command) > 1024) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'command' exceeds 1024 characters");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    GuestExecCtx *ctx = g_new0(GuestExecCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->command = g_strdup(command);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _guest_exec_callback, ctx);
    g_task_set_task_data(task, ctx, _guest_exec_ctx_free);
    g_task_run_in_thread(task, _guest_exec_worker);
    g_object_unref(task);
}











static void
_guest_shutdown_worker(GTask *task, gpointer source_obj __attribute__((unused)),
                       gpointer task_data, GCancellable *cancellable __attribute__((unused)))
{
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)task_data;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to connect to Libvirt.");
        return;
    }

    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_id);
    if (!dom) {
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "VM not found: %s", ctx->vm_id);
        return;
    }

    if (!virDomainIsActive(dom)) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "VM '%s' is not running.", ctx->vm_id);
        return;
    }


    const gchar *method_used = "guest-agent";
    int rc = virDomainShutdownFlags(dom, VIR_DOMAIN_SHUTDOWN_GUEST_AGENT);
    if (rc < 0) {

        virResetLastError();
        method_used = "acpi";
        rc = virDomainShutdown(dom);
        if (rc < 0) {
            const char *vir_err = virGetLastErrorMessage();
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Failed to shutdown VM '%s': %s",
                                    ctx->vm_id, vir_err ? vir_err : "unknown error");
            return;
        }
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);


    g_free(ctx->action);
    ctx->action = g_strdup(method_used);
    g_task_return_boolean(task, TRUE);
}

static void
_guest_shutdown_callback(GObject *source_obj __attribute__((unused)), GAsyncResult *res, gpointer user_data)
{
    GTask *task = G_TASK(res);
    VmLifecycleCtx *ctx = (VmLifecycleCtx *)user_data;
    GError *error = NULL;
    gboolean _ok = g_task_propagate_boolean(task, &error);

    pcv_audit_log(NULL, "vm.guest.shutdown", ctx->vm_id ?: "",
                  _ok ? "ok" : "fail",
                  _ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (!_ok) {
        gchar *err_resp = pure_rpc_build_error_response(ctx->rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(ctx->server, ctx->connection, err_resp);
        g_free(err_resp);
        g_error_free(error);
    } else {
        JsonObject *res_obj = json_object_new();
        json_object_set_string_member(res_obj, "status", "shutdown_initiated");
        json_object_set_string_member(res_obj, "name", ctx->vm_id);
        json_object_set_string_member(res_obj, "method", ctx->action ? ctx->action : "unknown");
        JsonNode *res_node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(res_node, res_obj);
        gchar *resp = pure_rpc_build_success_response(ctx->rpc_id, res_node);
        pure_uds_server_send_response(ctx->server, ctx->connection, resp);
        g_free(resp);
    }
}

void
handle_vm_guest_shutdown_request(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = _guest_get_vm_name(params);
    if (!vm_name) {
        gchar *err = pure_rpc_build_error_response(rpc_id, -32602,
                         "Invalid params: 'name' missing");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    VmLifecycleCtx *ctx = g_new0(VmLifecycleCtx, 1);
    ctx->vm_id = g_strdup(vm_name);
    ctx->rpc_id = g_strdup(rpc_id);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _guest_shutdown_callback, ctx);
    g_task_set_task_data(task, ctx, free_lifecycle_ctx);
    g_task_run_in_thread(task, _guest_shutdown_worker);
    g_object_unref(task);
}
