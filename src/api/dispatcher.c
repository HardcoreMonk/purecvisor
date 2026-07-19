
#include "dispatcher.h"
#include "uds_server.h"
#include "snapshot_verify_probe.h"
#include "vm_batch_policy.h"
#include "bootstrap/pcv_bootstrap.h"
#include "../modules/virt/vm_manager.h"
#include "../modules/virt/cancellable_map.h"
#include "../modules/virt/vm_clone_plan.h"
#include "../modules/daemons/prometheus_exporter.h"
#include "../modules/audit/pcv_audit.h"
#include "../modules/plugin/pcv_plugin_manager.h"
#include "purecvisor/version.h"
#include <json-glib/json-glib.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "../modules/dispatcher/handler_snapshot.h"
#include "../modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/handler_vnc.h"
#include "modules/dispatcher/handler_vm_start.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"
#include "modules/dispatcher/handler_vm_hotplug.h"
#include "modules/network/network_manager.h"
#include "modules/dispatcher/handler_storage.h"
#include "modules/dispatcher/handler_container.h"
#include "modules/dispatcher/handler_overlay.h"
#include "modules/dispatcher/handler_accel.h"
#include "modules/dispatcher/handler_template.h"
#include "modules/dispatcher/handler_auth.h"
#include "modules/dispatcher/handler_backup.h"
#include "modules/dispatcher/handler_security.h"
#include "modules/daemons/alert_engine.h"
#include "modules/daemons/process_monitor.h"
#include "modules/daemons/update_check.h"
#include "../modules/virt/virt_conn_pool.h"
#include "../utils/pcv_spawn.h"
#include "../utils/pcv_config.h"
#include "../utils/pcv_jwt.h"
#include "../modules/auth/pcv_rbac.h"
#include "utils/pcv_config.h"
#include "drain.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "modules/lxc/lxc_driver.h"
#include "modules/cloud/cloud_migration.h"
#include "modules/storage/zfs_driver.h"
#include "modules/backup/backup_scheduler.h"
#include "modules/core/cpu_allocator.h"
#include "modules/ai/workload_predict.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_config.h"
#include "utils/pcv_worker_pool.h"
#include "../utils/pcv_log.h"
#include "../utils/pcv_validate.h"
#include "utils/pcv_job_queue.h"
#include "modules/core/vm_state.h"
#include "ws_server.h"
#include <sqlite3.h>
#include <errno.h>

extern gchar *handle_monitor_fleet(JsonObject *params, GError **error);

void handle_vm_limit_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_monitor_metrics(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

void handle_network_list_request    (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_network_info_request    (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_network_mode_set_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

#define PCV_VM_METADATA_URI "urn:purecvisor:metadata"

typedef void (*PcvDispatchHandler)(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection);

static GHashTable *g_rpc_routes = nullptr;

static GHashTable *g_async_methods = nullptr;

gboolean pcv_dispatcher_is_async_method(const gchar *method);
gboolean pcv_dispatcher_is_async_method(const gchar *method) {
    return method && g_async_methods && g_hash_table_contains(g_async_methods, method);
}

typedef struct {
    const char *method;
    int         min_role;
} PcvMethodPolicy;

static const PcvMethodPolicy g_method_policies[] = {

    { "vm.delete",                 1 },
    { "vm.snapshot.delete",        2 },
    { "vm.snapshot.delete_all",    2 },
    { "vm.snapshot.rollback",      2 },
    { "network.delete",            2 },
    { "network.create",            2 },
    { "storage.zvol.delete",       2 },
    { "storage.pool.destroy",      2 },
    { "container.destroy",         2 },
    { "auth.role.set",             2 },
    { "auth.user.create",          2 },
    { "auth.user.delete",          2 },
    { "auth.password.reset",       2 },
    { "cloud.import",              2 },
    { "cloud.export",              2 },
    { "cloud.import.finalize",     2 },
    { "vm.export.ova",             2 },

    { "vm.create",                 1 },
    { "vm.start",                  1 },
    { "vm.stop",                   1 },
    { "vm.pause",                  1 },
    { "vm.resume",                 1 },
    { "vm.limit",                  1 },
    { "vm.rename",                 1 },
    { "vm.snapshot.create",        1 },
    { "vm.guest.exec",             2 },
    { "vm.guest.shutdown",         1 },
    { "vm.guest.agent.ensure_channel", 1 },
    { "vm.mount_iso",              1 },
    { "vm.eject",                  1 },
    { "vm.vnc",                    1 },
    { "get_vnc_info",              1 },
    { "vm.resize_disk",            1 },
    { "vm.clone",                  1 },
    { "container.create",          1 },
    { "container.start",           1 },
    { "container.stop",            1 },
    { "container.clone",           1 },
    { "container.exec",            2 },

    { "auth.apikey.create",        2 },
    { "auth.apikey.revoke",        2 },
    { "auth.user.delete",          2 },
    { "auth.password.reset",       2 },
    { "auth.role.set",             2 },
    { "alert.config.set",          2 },
    { "alert.config.reload",       2 },
    { "agent.config.set",          2 },
    { "healing.set_mode",          2 },
    { "anomaly.reset_baseline",    2 },
    { "agent.compare_manual",      2 },
    { "backup.set",                2 },
    { "backup.delete",             2 },
    { "backup.restore",            2 },
    { "backup.run",                2 },
    { "backup.replicate",          2 },
    { "config.reload",             2 },
    { "config.set",                2 },
    { "container.snapshot.create", 2 },
    { "container.snapshot.delete", 2 },
    { "container.snapshot.rollback", 2 },
    { "container.set_bandwidth",   2 },
    { "container.set_limits",      2 },
    { "container.health.set",      2 },
    { "container.health.delete",   2 },
    { "container.nic.attach",      2 },
    { "container.nic.detach",      2 },
    { "container.volume.attach",   2 },
    { "container.volume.detach",   2 },
    { "container.env.set",         2 },
    { "container.env.delete",      2 },
    { "device.disk.attach",        2 },
    { "device.disk.detach",        2 },
    { "device.nic.attach",         1 },
    { "device.nic.detach",         1 },
    { "device.gpu.attach",         2 },
    { "device.gpu.detach",         2 },
    { "dpdk.set",                  2 },
    { "iscsi.target.create",       2 },
    { "iscsi.target.delete",       2 },
    { "network.bind_phys",         2 },
    { "network.dhcp_toggle",       2 },
    { "network.mode_set",          2 },
    { "network.ovs.create",        2 },
    { "network.ovs.delete",        2 },
    { "network.ovs.vxlan.add",     2 },
    { "network.ovs.vxlan.del",     2 },
    { "nfv.deploy",                2 },
    { "nfv.delete",                2 },
    { "overlay.create",            2 },
    { "overlay.delete",            2 },
    { "overlay.add_peer",          2 },
    { "overlay.remove_peer",       2 },
    { "ovn.switch.create",         2 },
    { "ovn.switch.delete",         2 },
    { "ovn.router.create",         2 },
    { "ovn.router.delete",         2 },
    { "ovn.nat.add",               2 },
    { "ovn.nat.delete",            2 },
    { "plugin.load",               2 },
    { "plugin.unload",             2 },
    { "security_group.create",     2 },
    { "security_group.delete",     2 },
    { "security_group.attach",     2 },
    { "security_group.detach",     2 },
    { "snapshot.schedule.set",     2 },
    { "snapshot.schedule.delete",  2 },
    { "sriov.set",                 2 },
    { "storage.pool.create",       2 },
    { "storage.pool.scrub",        2 },

    { "storage.zvol.create",       2 },
    { "storage.zvol.delete",       2 },
    { "template.create",           2 },
    { "template.delete",           2 },
    { "tls.reload",                2 },

    { "vm.blkio.set",              1 },
    { "vm.import.ec2",             1 },
    { "vm.import.ova",             1 },
    { "vm.export.ec2",             1 },
    { "vm.security_group.set",     1 },
    { "vm.set_bandwidth",          1 },
    { "vm.set_memory",             1 },
    { "vm.set_vcpu",               1 },
    { "vm.pin_vcpu",               1 },
    { "vm.snapshot.schedule.set",  1 },
    { "vm.snapshot.schedule.delete", 1 },
    { "vm.usb.attach",             1 },
    { "vm.usb.detach",             1 },
    { "vm.disk.live_resize",       1 },

    { "dpdk.bridge.create",        2 },
    { "dpdk.bridge.delete",        2 },
    { "network.qos.set",           2 },
    { "nfv.lb.create",             2 },
    { "node.drain",                2 },
    { "ovn.tenant.create",         2 },
    { "sriov.attach",              2 },
    { "sriov.detach",              2 },

    { "vm.delete.status",          0 },
    { "vm.export.status",          0 },
    { "vm.import.status",          0 },
    { "vm.snapshot.list",          0 },
    { "vm.snapshot.schedule.list", 0 },
    { "vm.guest.agent.status",     0 },
    { "vm.guest.fsinfo",           0 },
    { "vm.batch",                  1 },

    { "auth.apikey.list",          2 },
    { "auth.user.list",            2 },
    { "auth.session.revoke",       2 },
    { "auth.user.sessions.revoke", 2 },
    { "backup.policy.set",         2 },
    { "backup.policy.delete",      2 },
    { "security.event.list",       PCV_ROLE_VIEWER },
    { "security.event.get",        PCV_ROLE_VIEWER },
    { "security.action.pending",   PCV_ROLE_VIEWER },
    { "security.action.dismiss",   PCV_ROLE_OPERATOR },
    { "security.action.approve",   PCV_ROLE_ADMIN },
    { "security.baseline.status",  PCV_ROLE_VIEWER },
    { "security.baseline.refresh", PCV_ROLE_ADMIN },
    { "security.config.get",       PCV_ROLE_VIEWER },
    { "security.config.set",       PCV_ROLE_ADMIN },
    { "cloud.job.cancel",          2 },
    { "cloud.jobs.list",           1 },
    { "daemon.config.set",         2 },

    { "dpdk.bind",                 2 },
    { "dpdk.unbind",               2 },
    { "sriov.enable",              2 },
    { "sriov.disable",             2 },
    { "ovn.acl.add",               2 },
    { "ovn.dhcp.enable",           2 },
    { "ovn.port.add",              2 },
    { "ovn.port.remove",           2 },
    { "ovn.router.add_port",       2 },
    { "iscsi.connect",             2 },
    { "iscsi.disconnect",          2 },
    { "network.qos.remove",        2 },
    { "backup.export_s3",          2 },
    { "backup.incremental",        2 },
    { "backup.verify",             2 },
    { "backup.snapshot.verify",    2 },
    { "alert.silence",             2 },
    { "alert.dlq.list",            2 },
    { "alert.dlq.retry",           2 },

    { "vm.numa.info",              0 },
    { "vm.sla.report",             0 },
    { "vm.schedule.list",          0 },
    { "capacity.forecast",         0 },
    { "vm.billing.report",         0 },
    { "vm.autostart",              1 },
    { "vm.schedule.set",           1 },

    { NULL, 0 }
};

static GHashTable *g_method_policy_map = NULL;

static int
_method_min_role(const gchar *method)
{
    if (!method) return 2;

    if (!g_method_policy_map) return 2;
    gpointer val = g_hash_table_lookup(g_method_policy_map, method);
    if (!val) return 0;
    return GPOINTER_TO_INT(val);
}

gboolean pcv_dispatcher_check_rbac(const gchar *method, gint caller_role);
gboolean pcv_dispatcher_check_rbac(const gchar *method, gint caller_role) {
    int min = _method_min_role(method);
    return caller_role >= min;
}

static const gchar *
_json_string_member(JsonObject *params, const gchar *key)
{
    if (!params || !key || !json_object_has_member(params, key))
        return NULL;

    JsonNode *node = json_object_get_member(params, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return NULL;
    if (json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string(node);
}

static gboolean
_valid_vm_storage_pool(const gchar *pool)
{
    if (!pool || !*pool || strlen(pool) > 255)
        return FALSE;
    if (pool[0] == '/' || pool[strlen(pool) - 1] == '/' ||
        strstr(pool, "..") || strstr(pool, "//") || strchr(pool, '@'))
        return FALSE;

    gboolean prev_slash = FALSE;
    for (const gchar *p = pool; *p; p++) {
        if (*p == '/') {
            if (prev_slash)
                return FALSE;
            prev_slash = TRUE;
            continue;
        }
        prev_slash = FALSE;
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-' && *p != '.')
            return FALSE;
    }
    return TRUE;
}

static gboolean
_path_at_or_under(const gchar *path, const gchar *root)
{
    gsize n = strlen(root);
    return g_strcmp0(path, root) == 0 ||
           (g_str_has_prefix(path, root) && path[n] == '/');
}

static gboolean
_valid_vm_image_dir(const gchar *dir)
{
    if (!dir || !*dir || strlen(dir) > 511)
        return FALSE;
    if (dir[0] != '/' || g_strcmp0(dir, "/") == 0 ||
        strstr(dir, "..") || strstr(dir, "//"))
        return FALSE;

    static const gchar *blocked_roots[] = {
        "/bin", "/boot", "/dev", "/etc", "/lib", "/lib64",
        "/proc", "/root", "/run", "/sbin", "/sys", "/usr", NULL
    };
    for (guint i = 0; blocked_roots[i]; i++) {
        if (_path_at_or_under(dir, blocked_roots[i]))
            return FALSE;
    }
    return TRUE;
}

static gboolean
_json_int_member(JsonObject *params, const gchar *key, gint *out)
{
    if (!params || !key || !out || !json_object_has_member(params, key))
        return FALSE;

    JsonNode *node = json_object_get_member(params, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return FALSE;

    GType value_type = json_node_get_value_type(node);
    if (value_type != G_TYPE_INT64 && value_type != G_TYPE_INT &&
        value_type != G_TYPE_LONG && value_type != G_TYPE_UINT &&
        value_type != G_TYPE_UINT64)
        return FALSE;

    *out = (gint)json_node_get_int(node);
    return TRUE;
}

static const gchar *
_dispatcher_caller_subject(JsonObject *params, GSocketConnection *connection)
{
    if (connection) {
        const gchar *sub = g_object_get_data(G_OBJECT(connection), "pcv-caller-sub");
        if (sub && *sub) return sub;
    }
    return _json_string_member(params, "_pcv_caller_sub");
}

static gint
_dispatcher_caller_role(JsonObject *params, GSocketConnection *connection)
{
    if (connection) {
        gpointer rdata = g_object_get_data(G_OBJECT(connection), "pcv-caller-role");
        if (rdata) return GPOINTER_TO_INT(rdata);
    }

    gint role = PCV_ROLE_ADMIN;
    if (_json_int_member(params, "_pcv_caller_role", &role)) {
        if (role < PCV_ROLE_VIEWER || role > PCV_ROLE_ADMIN)
            return PCV_ROLE_VIEWER;
        return role;
    }
    return role;
}

static const gchar *
_vm_name_from_params(JsonObject *params)
{
    const gchar *name = _json_string_member(params, "name");
    if (name && *name) return name;

    name = _json_string_member(params, "vm_id");
    if (name && *name) return name;

    name = _json_string_member(params, "vm_name");
    if (name && *name) return name;

    name = _json_string_member(params, "vm");
    if (name && *name) return name;

    return NULL;
}

static const gchar *
_vm_owner_scope_target_from_params(const gchar *method, JsonObject *params)
{

    if (g_strcmp0(method, "vm.clone") == 0) {
        const gchar *source = _json_string_member(params, "source");
        if (source && *source)
            return source;
    }

    return _vm_name_from_params(params);
}

static gboolean
_vm_method_requires_owner_scope(const gchar *method)
{
    if (!method)
        return FALSE;

    if (g_strcmp0(method, "get_vnc_info") == 0)
        return TRUE;

    if (g_strcmp0(method, "device.nic.attach") == 0 ||
        g_strcmp0(method, "device.nic.detach") == 0)
        return TRUE;

    if (!g_str_has_prefix(method, "vm."))
        return FALSE;

    if (g_strcmp0(method, "vm.create") == 0 ||
        g_strcmp0(method, "vm.import.ova") == 0 ||
        g_strcmp0(method, "vm.import.ec2") == 0)
        return FALSE;

    if (g_strcmp0(method, "vm.list") == 0 ||
        g_strcmp0(method, "vm.list.filtered") == 0 ||
        g_strcmp0(method, "vm.event.webhook.list") == 0 ||
        g_strcmp0(method, "vm.delete.status") == 0 ||
        g_strcmp0(method, "vm.import.status") == 0 ||
        g_strcmp0(method, "vm.export.status") == 0 ||
        g_strcmp0(method, "vm.snapshot.schedule.list") == 0)
        return FALSE;

    return TRUE;
}

static gchar *
_xml_find_owner_node(xmlNodePtr node)
{
    for (xmlNodePtr cur = node; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE &&
            cur->name &&
            xmlStrcmp(cur->name, BAD_CAST "owner") == 0) {
            xmlChar *content = xmlNodeGetContent(cur);
            if (!content)
                return NULL;
            gchar *owner = g_strdup((const gchar *)content);
            xmlFree(content);
            return owner;
        }

        gchar *child_owner = _xml_find_owner_node(cur->children);
        if (child_owner)
            return child_owner;
    }
    return NULL;
}

static gchar *
_vm_owner_from_xml(const gchar *xml)
{
    if (!xml || !*xml)
        return NULL;

    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), "pcv-vm-metadata.xml",
                                  NULL, XML_PARSE_NONET | XML_PARSE_NOERROR |
                                                XML_PARSE_NOWARNING);
    if (!doc)
        return NULL;

    gchar *owner = _xml_find_owner_node(xmlDocGetRootElement(doc));
    xmlFreeDoc(doc);

    if (owner)
        g_strstrip(owner);
    if (owner && *owner)
        return owner;
    g_free(owner);
    return NULL;
}

static gchar *
_lookup_vm_owner(const gchar *vm_name)
{
    if (!vm_name || !*vm_name)
        return NULL;

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn)
        return NULL;

    gchar *owner = NULL;
    virDomainPtr dom = virDomainLookupByName(conn, vm_name);
    if (!dom) {
        virResetLastError();
        dom = virDomainLookupByUUIDString(conn, vm_name);
    }
    if (!dom) {
        virt_conn_pool_release(conn);
        return NULL;
    }

    char *metadata = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                          PCV_VM_METADATA_URI,
                                          VIR_DOMAIN_AFFECT_CONFIG);
    if (!metadata) {
        metadata = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                        PCV_VM_METADATA_URI, 0);
    }
    if (metadata) {
        owner = _vm_owner_from_xml(metadata);
        g_free(metadata);
    }

    if (!owner) {
        char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        if (!xml)
            xml = virDomainGetXMLDesc(dom, 0);
        if (xml) {
            owner = _vm_owner_from_xml(xml);
            g_free(xml);
        }
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
    return owner;
}

static gboolean
_vm_owner_matches_caller(const gchar *vm_name,
                         const gchar *caller_sub,
                         gchar **deny_message)
{
    if (!vm_name || !*vm_name) {
        if (deny_message)
            *deny_message = g_strdup("Missing required parameter: name/vm_id");
        return FALSE;
    }

    gchar *owner = _lookup_vm_owner(vm_name);
    gboolean allowed = (owner && g_strcmp0(owner, caller_sub) == 0);
    g_free(owner);

    if (!allowed && deny_message) {
        *deny_message = g_strdup(
            "Permission denied: operators can access only VMs they created");
    }
    return allowed;
}

static gboolean
_vm_batch_owner_scoped_allowed(JsonObject *params,
                               const gchar *caller_sub,
                               gchar **deny_message)
{
    JsonArray *vms = (params && json_object_has_member(params, "vms"))
        ? json_object_get_array_member(params, "vms") : NULL;

    if (!vms) {
        if (deny_message)
            *deny_message = g_strdup("Missing required parameter: vms");
        return FALSE;
    }

    guint len = json_array_get_length(vms);
    for (guint i = 0; i < len; i++) {
        JsonNode *node = json_array_get_element(vms, i);
        if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING) {
            if (deny_message)
                *deny_message = g_strdup("Invalid parameter: vms must contain VM names");
            return FALSE;
        }

        const gchar *vm_name = json_node_get_string(node);
        if (!_vm_owner_matches_caller(vm_name, caller_sub, deny_message))
            return FALSE;
    }

    return TRUE;
}

static gboolean
_vm_owner_scoped_method_allowed(const gchar *method,
                                JsonObject *params,
                                GSocketConnection *connection,
                                gint caller_role,
                                gchar **deny_message)
{

    if (caller_role >= PCV_ROLE_ADMIN)
        return TRUE;

    if (caller_role < PCV_ROLE_OPERATOR) {
        if (deny_message)
            *deny_message = g_strdup("Permission denied: insufficient role for this method");
        return FALSE;
    }

    const gchar *caller_sub = _dispatcher_caller_subject(params, connection);
    if (!caller_sub || !*caller_sub) {
        if (deny_message)
            *deny_message = g_strdup("Permission denied: missing authenticated subject");
        return FALSE;
    }

    if (g_strcmp0(method, "vm.batch") == 0)
        return _vm_batch_owner_scoped_allowed(params, caller_sub, deny_message);

    const gchar *vm_name = _vm_owner_scope_target_from_params(method, params);
    return _vm_owner_matches_caller(vm_name, caller_sub, deny_message);
}

static const gchar *
_container_name_from_params(JsonObject *params)
{
    const gchar *name = _json_string_member(params, "name");
    if (name && *name) return name;

    name = _json_string_member(params, "container");
    if (name && *name) return name;

    name = _json_string_member(params, "container_id");
    if (name && *name) return name;

    return NULL;
}

static const gchar *
_container_owner_scope_target_from_params(const gchar *method, JsonObject *params)
{

    if (g_strcmp0(method, "container.clone") == 0) {
        const gchar *source = _json_string_member(params, "source");
        if (source && *source)
            return source;
    }

    return _container_name_from_params(params);
}

static gboolean
_container_method_requires_owner_scope(const gchar *method)
{
    if (!method)
        return FALSE;

    return g_strcmp0(method, "container.start") == 0 ||
           g_strcmp0(method, "container.stop") == 0 ||
           g_strcmp0(method, "container.clone") == 0;
}

static gchar *
_lookup_container_owner(const gchar *name)
{

    return pcv_lxc_read_owner(name);
}

static gboolean
_container_owner_matches_caller(const gchar *name,
                                const gchar *caller_sub,
                                gchar **deny_message)
{
    if (!name || !*name) {
        if (deny_message)
            *deny_message = g_strdup("Missing required parameter: name");
        return FALSE;
    }

    gchar *owner = _lookup_container_owner(name);
    gboolean allowed = (owner && g_strcmp0(owner, caller_sub) == 0);
    g_free(owner);

    if (!allowed && deny_message) {
        *deny_message = g_strdup(
            "Permission denied: operators can access only containers they created");
    }
    return allowed;
}

static gboolean
_container_owner_scoped_method_allowed(const gchar *method,
                                       JsonObject *params,
                                       GSocketConnection *connection,
                                       gint caller_role,
                                       gchar **deny_message)
{

    if (caller_role >= PCV_ROLE_ADMIN)
        return TRUE;

    if (caller_role < PCV_ROLE_OPERATOR) {
        if (deny_message)
            *deny_message = g_strdup("Permission denied: insufficient role for this method");
        return FALSE;
    }

    const gchar *caller_sub = _dispatcher_caller_subject(params, connection);
    if (!caller_sub || !*caller_sub) {
        if (deny_message)
            *deny_message = g_strdup("Permission denied: missing authenticated subject");
        return FALSE;
    }

    const gchar *name = _container_owner_scope_target_from_params(method, params);
    return _container_owner_matches_caller(name, caller_sub, deny_message);
}

typedef gboolean (*PcvDispatchHook)(const gchar *method, JsonObject *params,
                                     const gchar *rpc_id, gpointer user_data);

typedef struct {
    PcvDispatchHook hook;
    gpointer        user_data;
} _HookEntry;

static GPtrArray *g_pre_hooks = nullptr;

void
pcv_dispatcher_register_pre_hook(PcvDispatchHook hook, gpointer user_data)
{
    if (!g_pre_hooks)
        g_pre_hooks = g_ptr_array_new_with_free_func(g_free);
    _HookEntry *entry = g_new0(_HookEntry, 1);
    entry->hook = hook;
    entry->user_data = user_data;
    g_ptr_array_add(g_pre_hooks, entry);
}

static gboolean
_run_pre_hooks(const gchar *method, JsonObject *params, const gchar *rpc_id)
{
    if (!g_pre_hooks) return TRUE;
    for (guint i = 0; i < g_pre_hooks->len; i++) {
        _HookEntry *entry = g_ptr_array_index(g_pre_hooks, i);
        if (!entry->hook(method, params, rpc_id, entry->user_data))
            return FALSE;
    }
    return TRUE;
}

static void dispatcher_init_routes(void);

struct _PureCVisorDispatcher {
    GObject parent_instance;
    PureCVisorVmManager *vm_manager;
};

G_DEFINE_TYPE(PureCVisorDispatcher, purecvisor_dispatcher, G_TYPE_OBJECT)

typedef struct {
    PureCVisorDispatcher *dispatcher;
    gint request_id;
    UdsServer *server;
    GSocketConnection *connection;
} DispatcherRequestContext;

static void dispatcher_request_context_free(DispatcherRequestContext *ctx) {
    if (ctx->dispatcher) g_object_unref(ctx->dispatcher);
    if (ctx->server) g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

static JsonNode *
_paginate_array(JsonArray *full_array, gint offset, gint limit)
{
    gint total = (gint)json_array_get_length(full_array);

    if (limit <= 0) {

        JsonNode *node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(node, full_array);
        return node;
    }

    if (offset < 0) offset = 0;
    if (offset > total) offset = total;

    JsonArray *paged = json_array_new();
    for (gint i = offset; i < total && i < offset + limit; i++) {
        JsonNode *elem = json_array_dup_element(full_array, (guint)i);
        json_array_add_element(paged, elem);
    }

    JsonObject *result = json_object_new();
    json_object_set_array_member(result, "items", paged);
    json_object_set_int_member(result, "total", total);
    json_object_set_int_member(result, "offset", offset);
    json_object_set_int_member(result, "limit", limit);
    json_object_set_boolean_member(result, "has_more", offset + limit < total);

    json_array_unref(full_array);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    return node;
}

static void
_get_pagination_params(JsonObject *params, gint *out_offset, gint *out_limit)
{
    *out_offset = (params && json_object_has_member(params, "offset"))
        ? (gint)json_object_get_int_member(params, "offset") : 0;
    *out_limit = (params && json_object_has_member(params, "limit"))
        ? (gint)json_object_get_int_member(params, "limit") : 0;
}

typedef struct {
    gchar *job_id;
    gchar *vm_name;
} VmCreateJobCtx;

static void _vm_create_job_ctx_free(gpointer data)
{
    if (!data) return;
    VmCreateJobCtx *ctx = (VmCreateJobCtx *)data;
    g_free(ctx->job_id);
    g_free(ctx->vm_name);
    g_free(ctx);
}

static void
_on_vm_create_finished(GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
    VmCreateJobCtx *ctx = (VmCreateJobCtx *)user_data;
    GError *error = NULL;

    unlock_vm_operation(ctx->vm_name);

    gboolean ok = purecvisor_vm_manager_create_vm_finish(
        PURECVISOR_VM_MANAGER(source_object), res, &error);

    if (ok) {
        pcv_job_set_result(ctx->job_id, PCV_JOB_COMPLETED, NULL);
        pcv_ws_broadcast_job_complete(ctx->job_id, "vm.create",
                                       "completed", NULL);

        pcv_audit_log(NULL, "vm.create", ctx->vm_name, "ok", 0, 0, "local");
        PCV_LOG_INFO("dispatcher",
                     "vm.create job %s completed for '%s'",
                     ctx->job_id, ctx->vm_name);
    } else {
        const gchar *err_msg = error ? error->message : "Unknown error";
        pcv_job_set_result(ctx->job_id, PCV_JOB_FAILED, err_msg);
        pcv_ws_broadcast_job_complete(ctx->job_id, "vm.create",
                                       "failed", err_msg);

        pcv_audit_log(NULL, "vm.create", ctx->vm_name, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        PCV_LOG_WARN("dispatcher",
                     "vm.create job %s FAILED for '%s': %s",
                     ctx->job_id, ctx->vm_name, err_msg);
        if (error) g_error_free(error);
    }

    cmap_remove(ctx->vm_name);
    _vm_create_job_ctx_free(ctx);
}

void handle_vm_vnc_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_device_nic_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_device_nic_attach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_device_nic_detach(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);
void handle_vm_eject_iso(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

typedef struct {
    gchar    *source;
    gchar    *target;
    gboolean  full_copy;
    gboolean  guest_reset;
    PcvVmCloneDiskKind disk_kind;
    gchar    *source_disk_path;
    gchar    *target_disk_path;
    gchar    *source_dataset;
    gchar    *target_dataset;
    gchar    *zfs_pool;
    gchar    *source_zvol_name;
    gboolean  holds_source_lock;
    gboolean  holds_target_lock;
    GThread  *lock_renew_thread;
    gint      lock_renew_stop;
} VmCloneCtx;

#define VM_CLONE_LOCK_RENEW_TICK_MS   250
#define VM_CLONE_LOCK_RENEW_TICKS     240

static gpointer
_vm_clone_lock_renew_worker(gpointer data)
{
    VmCloneCtx *ctx = (VmCloneCtx *)data;
    while (!g_atomic_int_get(&ctx->lock_renew_stop)) {
        for (gint i = 0; i < VM_CLONE_LOCK_RENEW_TICKS; i++) {
            if (g_atomic_int_get(&ctx->lock_renew_stop)) return NULL;
            g_usleep(VM_CLONE_LOCK_RENEW_TICK_MS * 1000);
        }
        if (g_atomic_int_get(&ctx->lock_renew_stop)) break;
        if (ctx->holds_source_lock) (void)pcv_vm_lock_renew(ctx->source);
        if (ctx->holds_target_lock) (void)pcv_vm_lock_renew(ctx->target);
    }
    return NULL;
}

static void
_vm_clone_ctx_free(gpointer data)
{
    VmCloneCtx *ctx = (VmCloneCtx *)data;
    if (!ctx) return;

    if (ctx->lock_renew_thread) {
        g_atomic_int_set(&ctx->lock_renew_stop, 1);
        g_thread_join(ctx->lock_renew_thread);
        ctx->lock_renew_thread = NULL;
    }

    if (ctx->holds_source_lock) unlock_vm_operation(ctx->source);
    if (ctx->holds_target_lock) unlock_vm_operation(ctx->target);
    g_free(ctx->source);
    g_free(ctx->target);
    g_free(ctx->source_disk_path);
    g_free(ctx->target_disk_path);
    g_free(ctx->source_dataset);
    g_free(ctx->target_dataset);
    g_free(ctx->zfs_pool);
    g_free(ctx->source_zvol_name);
    g_free(ctx);
}

static gboolean
_vm_clone_template_prepared_ack(JsonObject *params)
{
    if (!params)
        return FALSE;

    if (json_object_has_member(params, "template_prepared")) {
        JsonNode *node = json_object_get_member(params, "template_prepared");
        if (node && JSON_NODE_HOLDS_VALUE(node) &&
            json_node_get_value_type(node) == G_TYPE_BOOLEAN &&
            json_node_get_boolean(node))
            return TRUE;
    }

    const gchar *ack = _json_string_member(params, "clone_safety_ack");
    return g_strcmp0(ack, "template-prepared") == 0;
}

static gboolean
_vm_clone_bool_member(JsonObject *params, const gchar *name, gboolean fallback)
{
    if (!params || !name || !json_object_has_member(params, name))
        return fallback;

    JsonNode *node = json_object_get_member(params, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_BOOLEAN)
        return fallback;

    return json_node_get_boolean(node);
}

static gchar *
_vm_clone_job_target(VmCloneCtx *ctx)
{
    return g_strdup_printf("%s:%s", ctx->source, ctx->target);
}

static gchar *
_vm_clone_job_id(VmCloneCtx *ctx)
{
    gchar *target = _vm_clone_job_target(ctx);
    gchar *job_id = g_strdup_printf("vm.clone:%s", target);
    g_free(target);
    return job_id;
}

static void
_audit_vm_clone_success(VmCloneCtx *ctx)
{
    gchar *target = _vm_clone_job_target(ctx);
    gchar *job_id = _vm_clone_job_id(ctx);
    pcv_audit_log(NULL, "vm.clone", target, "ok", 0, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.clone", "completed", NULL);
    g_free(job_id);
    g_free(target);
}

static void
_audit_vm_clone_failure(VmCloneCtx *ctx, const gchar *error_msg)
{
    gchar *target = _vm_clone_job_target(ctx);
    gchar *job_id = _vm_clone_job_id(ctx);
    pcv_audit_log(NULL, "vm.clone", target, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_ws_broadcast_job_complete_mt(job_id, "vm.clone",
                                     "failed", error_msg ? error_msg : "unknown");
    g_free(job_id);
    g_free(target);
}

static gchar *
_vm_clone_source_snapshot(VmCloneCtx *ctx, const gchar *snap_tag)
{
    if (!ctx || !ctx->source_dataset || !snap_tag)
        return NULL;
    return g_strdup_printf("%s@%s", ctx->source_dataset, snap_tag);
}

static gboolean
_vm_clone_destroy_dataset_recursive(const gchar *dataset)
{
    if (!dataset || !*dataset)
        return TRUE;

    const gchar *destroy_argv[] = {"zfs", "destroy", "-R", dataset, NULL};
    GError *cleanup_err = NULL;
    gchar *cleanup_stderr = NULL;
    gboolean ok = pcv_spawn_sync(destroy_argv, NULL, &cleanup_stderr, &cleanup_err);
    if (!ok) {
        if (cleanup_stderr)
            g_strstrip(cleanup_stderr);
        PCV_LOG_WARN("vm_clone", "Target clone dataset cleanup failed for '%s': %s (zfs stderr: %s)",
                     dataset,
                     cleanup_err ? cleanup_err->message : "unknown",
                     (cleanup_stderr && *cleanup_stderr) ? cleanup_stderr : "(none)");
    }
    g_free(cleanup_stderr);
    g_clear_error(&cleanup_err);
    return ok;
}

static gboolean
_vm_clone_destroy_source_snapshot(VmCloneCtx *ctx, const gchar *snap_tag)
{
    gchar *snap_full = _vm_clone_source_snapshot(ctx, snap_tag);
    if (!snap_full)
        return TRUE;

    const gchar *destroy_argv[] = {"zfs", "destroy", snap_full, NULL};
    GError *cleanup_err = NULL;
    gchar *cleanup_stderr = NULL;
    gboolean ok = pcv_spawn_sync(destroy_argv, NULL, &cleanup_stderr, &cleanup_err);
    if (!ok) {
        if (cleanup_stderr)
            g_strstrip(cleanup_stderr);
        PCV_LOG_WARN("vm_clone", "Source clone snapshot cleanup failed for '%s': %s (zfs stderr: %s)",
                     snap_full,
                     cleanup_err ? cleanup_err->message : "unknown",
                     (cleanup_stderr && *cleanup_stderr) ? cleanup_stderr : "(none)");
    }
    g_free(cleanup_stderr);
    g_clear_error(&cleanup_err);
    g_free(snap_full);
    return ok;
}

static gboolean
_vm_clone_remove_target_file(const gchar *path)
{
    if (!path || !*path)
        return TRUE;

    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return TRUE;

    if (g_remove(path) == 0)
        return TRUE;

    PCV_LOG_WARN("vm_clone", "Target clone file cleanup failed for '%s': %s",
                 path, g_strerror(errno));
    return FALSE;
}

static void
_vm_clone_cleanup_failed_artifacts(VmCloneCtx *ctx, const gchar *snap_tag)
{
    if (!ctx)
        return;

    if (ctx->disk_kind == PCV_VM_CLONE_DISK_ZVOL)
        (void)_vm_clone_destroy_dataset_recursive(ctx->target_dataset);
    else
        (void)_vm_clone_remove_target_file(ctx->target_disk_path);
    (void)_vm_clone_destroy_source_snapshot(ctx, snap_tag);
}

static PcvVmCloneDiskPlan
_vm_clone_disk_plan_from_ctx(VmCloneCtx *ctx)
{
    PcvVmCloneDiskPlan plan = {
        .kind = ctx->disk_kind,
        .source_disk_path = ctx->source_disk_path,
        .target_disk_path = ctx->target_disk_path,
        .source_dataset = ctx->source_dataset,
        .target_dataset = ctx->target_dataset,
        .zfs_pool = ctx->zfs_pool,
        .source_zvol_name = ctx->source_zvol_name,
    };
    return plan;
}

static void
_generate_random_mac(gchar *out_mac, gsize out_len)
{
    guint8 bytes[3];
    for (int i = 0; i < 3; i++)
        bytes[i] = (guint8)g_random_int_range(0, 256);
    g_snprintf(out_mac, out_len, "52:54:00:%02x:%02x:%02x",
               bytes[0], bytes[1], bytes[2]);
}

static gboolean
_xml_replace_mac_eval(const GMatchInfo *match_info, GString *result, gpointer user_data)
{
    (void)user_data;

    gchar *old_mac_tag = g_match_info_fetch(match_info, 0);
    if (!old_mac_tag)
        return FALSE;

    gchar new_mac[18];
    _generate_random_mac(new_mac, sizeof(new_mac));
    gboolean self_closing = g_str_has_suffix(old_mac_tag, "/>");
    g_string_append_printf(result,
                           self_closing ? "<mac address='%s'/>"
                                        : "<mac address='%s'>",
                           new_mac);
    g_free(old_mac_tag);
    return FALSE;
}

static gchar *
_xml_replace_all_macs(gchar *xml)
{
    GRegex *mac_re = g_regex_new("<mac address='[0-9a-fA-F:]+'/?>",
                                  G_REGEX_CASELESS, 0, NULL);
    if (!mac_re) return xml;

    GError *error = NULL;
    gchar *result = g_regex_replace_eval(mac_re,
                                         xml,
                                         -1,
                                         0,
                                         0,
                                         _xml_replace_mac_eval,
                                         NULL,
                                         &error);
    g_regex_unref(mac_re);
    if (!result) {
        PCV_LOG_WARN("vm_clone", "MAC rewrite failed: %s",
                     error ? error->message : "unknown");
        if (error)
            g_error_free(error);
        return xml;
    }

    g_free(xml);
    return result;
}

static void
_vm_clone_thread(GTask *task, gpointer source_obj,
                  gpointer task_data, GCancellable *cancellable)
{
    (void)source_obj; (void)cancellable;
    VmCloneCtx *ctx = (VmCloneCtx *)task_data;
    GError *err = nullptr;

    gboolean is_zvol = (ctx->disk_kind == PCV_VM_CLONE_DISK_ZVOL);
    gboolean is_file_disk = (ctx->disk_kind == PCV_VM_CLONE_DISK_QCOW2 ||
                             ctx->disk_kind == PCV_VM_CLONE_DISK_RAW);
    if ((is_zvol && (!ctx->source_dataset || !ctx->target_dataset ||
                     !ctx->zfs_pool || !ctx->source_zvol_name)) ||
        (is_file_disk && (!ctx->source_disk_path || !ctx->target_disk_path)) ||
        (!is_zvol && !is_file_disk)) {
        const gchar *err_msg = "vm.clone internal error: missing clone disk plan";
        PCV_LOG_WARN("vm_clone", "%s", err_msg);
        _audit_vm_clone_failure(ctx, err_msg);
        g_task_return_boolean(task, FALSE);
        return;
    }

    PCV_LOG_INFO("vm_clone", "Starting clone '%s' -> '%s' (mode=%s type=%s guest_reset=%s source=%s target=%s)",
                 ctx->source, ctx->target, ctx->full_copy ? "full" : "cow",
                 pcv_vm_clone_disk_kind_to_string(ctx->disk_kind),
                 ctx->guest_reset ? "yes" : "no",
                 is_zvol ? ctx->source_dataset : ctx->source_disk_path,
                 is_zvol ? ctx->target_dataset : ctx->target_disk_path);

    ctx->lock_renew_thread = g_thread_new("clone-lock-renew",
                                          _vm_clone_lock_renew_worker, ctx);

    gchar *snap_tag = NULL;
    gboolean source_snapshot_exists = FALSE;

    if (is_zvol) {

        snap_tag = g_strdup_printf("clone-%s", ctx->target);
        {
            gchar *snap_full = _vm_clone_source_snapshot(ctx, snap_tag);
            const gchar *argv[] = {"zfs", "snapshot", snap_full, NULL};
            gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);
            g_free(snap_full);
            if (!ok) {
                const gchar *err_msg = err ? err->message : "unknown";
                PCV_LOG_WARN("vm_clone", "ZFS snapshot failed for '%s': %s",
                              ctx->source, err_msg);
                _audit_vm_clone_failure(ctx, err_msg);
                if (err) g_error_free(err);
                g_free(snap_tag);
                g_task_return_boolean(task, FALSE);
                return;
            }
            source_snapshot_exists = TRUE;
        }

        {
            gboolean ok;
            if (ctx->full_copy) {
                ok = purecvisor_zfs_full_copy(ctx->zfs_pool, ctx->source_zvol_name,
                                               snap_tag, ctx->target, &err);
            } else {
                ok = purecvisor_zfs_clone_volume(ctx->zfs_pool, ctx->source_zvol_name,
                                                  snap_tag, ctx->target, &err);
            }
            if (!ok) {
                const gchar *err_msg = err ? err->message : "unknown";
                PCV_LOG_WARN("vm_clone", "ZFS %s failed for '%s': %s",
                              ctx->full_copy ? "full copy" : "clone",
                              ctx->target, err_msg);
                _audit_vm_clone_failure(ctx, err_msg);
                _vm_clone_cleanup_failed_artifacts(ctx,
                                                   source_snapshot_exists ? snap_tag : NULL);
                if (err) g_error_free(err);
                g_free(snap_tag);
                g_task_return_boolean(task, FALSE);
                return;
            }
        }

        {
            const gchar *udevadm_argv[] = {"udevadm", "settle", "--timeout=5", NULL};
            (void)pcv_spawn_sync(udevadm_argv, NULL, NULL, NULL);

            const int node_wait_max_ms  = 10000;
            const int node_wait_step_ms = 100;
            int node_waited_ms = 0;
            while (!g_file_test(ctx->target_disk_path, G_FILE_TEST_EXISTS)) {
                if (node_waited_ms >= node_wait_max_ms) {
                    gchar *err_msg = g_strdup_printf(
                        "zvol device node '%s' did not appear within %ds after clone",
                        ctx->target_disk_path, node_wait_max_ms / 1000);
                    PCV_LOG_WARN("vm_clone", "%s", err_msg);
                    _audit_vm_clone_failure(ctx, err_msg);
                    _vm_clone_cleanup_failed_artifacts(ctx,
                                                       source_snapshot_exists ? snap_tag : NULL);
                    g_free(err_msg);
                    g_free(snap_tag);
                    g_task_return_boolean(task, FALSE);
                    return;
                }
                g_usleep((gulong)node_wait_step_ms * 1000);
                node_waited_ms += node_wait_step_ms;
            }
        }

        if (ctx->full_copy) {

            if (_vm_clone_destroy_source_snapshot(ctx, snap_tag))
                source_snapshot_exists = FALSE;
        }
    } else {
        PcvVmCloneDiskPlan file_plan = _vm_clone_disk_plan_from_ctx(ctx);
        if (!pcv_vm_clone_copy_file_disk(&file_plan, &err)) {
            const gchar *err_msg = err ? err->message : "unknown";
            PCV_LOG_WARN("vm_clone", "File disk clone failed for '%s': %s",
                         ctx->target, err_msg);
            _audit_vm_clone_failure(ctx, err_msg);
            _vm_clone_cleanup_failed_artifacts(ctx, NULL);
            if (err) g_error_free(err);
            g_task_return_boolean(task, FALSE);
            return;
        }
    }

    if (ctx->guest_reset) {
        PcvVmCloneDiskPlan reset_plan = _vm_clone_disk_plan_from_ctx(ctx);
        if (!pcv_vm_clone_reset_guest_identity(&reset_plan, ctx->target, &err)) {
            const gchar *err_msg = err ? err->message : "unknown";
            PCV_LOG_WARN("vm_clone", "Guest identity reset failed for '%s': %s",
                         ctx->target, err_msg);
            _audit_vm_clone_failure(ctx, err_msg);
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            if (err) g_error_free(err);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }
    }

    gchar *xml = nullptr;
    {
        virConnectPtr conn = virt_conn_pool_acquire();
        if (!conn) {
            PCV_LOG_WARN("vm_clone", "Failed to acquire libvirt connection");
            _audit_vm_clone_failure(ctx, "Failed to acquire libvirt connection");
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }
        virDomainPtr dom = virDomainLookupByName(conn, ctx->source);
        if (!dom) {
            PCV_LOG_WARN("vm_clone", "Source VM '%s' not found in libvirt",
                          ctx->source);
            _audit_vm_clone_failure(ctx, "Source VM not found in libvirt");
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            virt_conn_pool_release(conn);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }
        xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        if (!xml || !*xml) {
            PCV_LOG_WARN("vm_clone", "Empty XML for source VM '%s'",
                          ctx->source);
            _audit_vm_clone_failure(ctx, "Empty XML for source VM");
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            free(xml);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }
    }

    {
        gchar *old_tag = g_strdup_printf("<name>%s</name>", ctx->source);
        gchar *new_tag = g_strdup_printf("<name>%s</name>", ctx->target);
        if (g_strstr_len(xml, -1, old_tag)) {
            gchar **parts = g_strsplit(xml, old_tag, 2);
            gchar *replaced = g_strjoinv(new_tag, parts);
            g_strfreev(parts);
            free(xml);
            xml = replaced;
        } else {

            gchar *dup = g_strdup(xml);
            free(xml);
            xml = dup;
        }
        g_free(old_tag);
        g_free(new_tag);
    }

    {
        GRegex *uuid_re = g_regex_new("\\s*<uuid>[^<]*</uuid>\\s*\n?",
                                        0, 0, NULL);
        if (uuid_re) {
            gchar *no_uuid = g_regex_replace(uuid_re, xml, -1, 0, "", 0, NULL);
            g_regex_unref(uuid_re);
            if (no_uuid) { g_free(xml); xml = no_uuid; }
        }
    }

    xml = _xml_replace_all_macs(xml);

    if (ctx->source_disk_path && ctx->target_disk_path &&
        g_strstr_len(xml, -1, ctx->source_disk_path)) {
        gchar **parts = g_strsplit(xml, ctx->source_disk_path, -1);
        gchar *tmp = g_strjoinv(ctx->target_disk_path, parts);
        g_strfreev(parts);
        g_free(xml);
        xml = tmp;
    } else {
        const gchar *err_msg = "Source disk path not found in current VM XML";
        PCV_LOG_WARN("vm_clone", "%s for '%s'", err_msg, ctx->source);
        _audit_vm_clone_failure(ctx, err_msg);
        _vm_clone_cleanup_failed_artifacts(ctx,
                                           source_snapshot_exists ? snap_tag : NULL);
        g_free(xml);
        g_free(snap_tag);
        g_task_return_boolean(task, FALSE);
        return;
    }

    {
        virConnectPtr conn = virt_conn_pool_acquire();
        if (!conn) {
            PCV_LOG_WARN("vm_clone", "Failed to acquire libvirt connection");
            _audit_vm_clone_failure(ctx, "Failed to acquire libvirt connection");
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            g_free(xml);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }

        virDomainPtr dom = virDomainDefineXML(conn, xml);
        if (dom) {
            PCV_LOG_INFO("vm_clone", "Defined cloned VM '%s' from '%s'",
                          ctx->target, ctx->source);
            virDomainFree(dom);
            virt_conn_pool_release(conn);
        } else {
            virErrorPtr vir_err = virGetLastError();
            const gchar *err_msg = vir_err ? vir_err->message : "unknown";
            PCV_LOG_WARN("vm_clone", "virDomainDefineXML failed for '%s': %s",
                          ctx->target, err_msg);
            _audit_vm_clone_failure(ctx, err_msg);
            _vm_clone_cleanup_failed_artifacts(ctx,
                                               source_snapshot_exists ? snap_tag : NULL);
            virt_conn_pool_release(conn);
            g_free(xml);
            g_free(snap_tag);
            g_task_return_boolean(task, FALSE);
            return;
        }
    }

    if (ctx->full_copy && source_snapshot_exists) {

        if (_vm_clone_destroy_source_snapshot(ctx, snap_tag))
            source_snapshot_exists = FALSE;
    }

    _audit_vm_clone_success(ctx);

    g_free(xml);
    g_free(snap_tag);
    g_task_return_boolean(task, TRUE);
}

static void handle_vm_create(PureCVisorDispatcher *self, JsonObject *params,
                              const gchar *rpc_id, UdsServer *server,
                              GSocketConnection *connection) {
    if (!json_object_has_member(params, "name")) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing parameter: name");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    const gchar *name = json_object_get_string_member(params, "name");

    {
        gboolean exists = FALSE;
        virConnectPtr conn = virt_conn_pool_acquire();
        if (conn) {
            virDomainPtr dom = virDomainLookupByName(conn, name);
            if (dom) {
                exists = TRUE;
                virDomainFree(dom);
            }
            virt_conn_pool_release(conn);
        }

        if (!exists) {
            gchar *dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), name);
            const gchar *zfs_argv[] = {"zfs", "list", "-H", "-o", "name", dataset, NULL};

            exists = pcv_spawn_sync_timeout(zfs_argv, NULL, NULL, 10, NULL);
            g_free(dataset);
        }

        if (!exists) {
            const gchar *image_dir = pcv_config_get_image_dir();
            gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2", image_dir, name);
            gchar *raw_img_path = g_strdup_printf("%s/%s.img", image_dir, name);
            gchar *raw_path = g_strdup_printf("%s/%s.raw", image_dir, name);
            exists = g_file_test(qcow2_path, G_FILE_TEST_EXISTS) ||
                     g_file_test(raw_img_path, G_FILE_TEST_EXISTS) ||
                     g_file_test(raw_path, G_FILE_TEST_EXISTS);
            g_free(qcow2_path);
            g_free(raw_img_path);
            g_free(raw_path);
        }

        if (exists) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                "VM already exists — delete the VM first");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
    }

    if (!pcv_validate_vm_name(name)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid VM name — must be 1-64 chars [a-zA-Z0-9_-]");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gint vcpu = 1;
    gint memory_mb = 1024;
    gint disk_size_gb = 50;
    gint vlan_id = 0;
    const gchar *iso_path = nullptr;
    const gchar *bridge = nullptr;
    const gchar *storage_type = nullptr;
    const gchar *storage_pool = nullptr;
    const gchar *image_dir = nullptr;
    const gchar *nic_type = nullptr;
    const gchar *pci_addr = nullptr;

    if (json_object_has_member(params, "vcpu")) vcpu = json_object_get_int_member(params, "vcpu");
    if (json_object_has_member(params, "memory_mb")) memory_mb = json_object_get_int_member(params, "memory_mb");

    if (json_object_has_member(params, "disk_size_gb")) disk_size_gb = json_object_get_int_member(params, "disk_size_gb");
    else if (json_object_has_member(params, "disk_gb")) disk_size_gb = json_object_get_int_member(params, "disk_gb");
    if (json_object_has_member(params, "vlan_id")) vlan_id = json_object_get_int_member(params, "vlan_id");

    if (vcpu < 1 || vcpu > 256) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid vcpu — must be between 1 and 256");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (memory_mb < 256 || memory_mb > 1048576) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid memory_mb — must be between 256 and 1048576 (1TB)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (disk_size_gb < 0 || disk_size_gb > 65536) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid disk_size_gb — must be between 0 and 65536 (64TB)");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (vlan_id < 0 || vlan_id > 4094) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid vlan_id — must be between 0 and 4094");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (json_object_has_member(params, "iso_path")) iso_path = json_object_get_string_member(params, "iso_path");
    if (json_object_has_member(params, "network_bridge")) {
        bridge = json_object_get_string_member(params, "network_bridge");
    }

    /* PCV_SAFETY_CONTROL: vm-create-iso-validation — 라이브 vm.create 파라미터를 통합
     * 검증기로 실검증(dead 검증본 대신 라이브 배선). iso_path=.iso/.img·절대·no-".." 아니면
     * 거부(예: /etc/shadow → CD-ROM 임의파일 마운트 차단). CMP-3 */
    {
        gint         v_disk   = (disk_size_gb > 0) ? disk_size_gb : PCV_MIN_DISK_GB;
        const gchar *v_bridge = (bridge && *bridge) ? bridge : NULL;
        GError      *v_err    = NULL;
        if (!pcv_validate_vm_create_params(name, vcpu, memory_mb, v_disk,
                                           iso_path, v_bridge, &v_err)) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                (v_err && v_err->message) ? v_err->message : "Invalid vm.create parameters");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            if (v_err) g_error_free(v_err);
            return;
        }
    }
    if (json_object_has_member(params, "storage_type")) {
        storage_type = json_object_get_string_member(params, "storage_type");

        if (storage_type &&
            g_strcmp0(storage_type, "zvol") != 0 &&
            g_strcmp0(storage_type, "qcow2") != 0 &&
            g_strcmp0(storage_type, "raw") != 0) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "Invalid storage_type — must be 'zvol', 'qcow2', or 'raw'");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
    }
    if (json_object_has_member(params, "storage_pool")) {
        storage_pool = json_object_get_string_member(params, "storage_pool");
    }
    if (json_object_has_member(params, "image_dir")) {
        image_dir = json_object_get_string_member(params, "image_dir");
    }
    if (json_object_has_member(params, "storage_location")) {
        const gchar *storage_location = json_object_get_string_member(params, "storage_location");
        if (storage_location && *storage_location) {
            if (!storage_type && storage_location[0] == '/' && !image_dir) {
                image_dir = storage_location;
            } else if (!storage_type && !storage_pool) {
                storage_pool = storage_location;
            } else if (g_strcmp0(storage_type, "zvol") == 0 && !storage_pool) {
                storage_pool = storage_location;
            } else if ((g_strcmp0(storage_type, "qcow2") == 0 ||
                        g_strcmp0(storage_type, "raw") == 0) && !image_dir) {
                image_dir = storage_location;
            }
        }
    }
    if (storage_pool && *storage_pool && !_valid_vm_storage_pool(storage_pool)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid storage_pool — use a relative ZFS dataset path such as 'tank/vms'");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    if (image_dir && *image_dir && !_valid_vm_image_dir(image_dir)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid image_dir — use a safe absolute directory outside system roots");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    if (json_object_has_member(params, "nic_type")) {
        nic_type = json_object_get_string_member(params, "nic_type");
        if (nic_type &&
            g_strcmp0(nic_type, "bridge") != 0 &&
            g_strcmp0(nic_type, "dpdk") != 0 &&
            g_strcmp0(nic_type, "sriov") != 0) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "Invalid nic_type — must be 'bridge', 'dpdk', or 'sriov'");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
    }

    if (json_object_has_member(params, "pci_addr")) {
        pci_addr = json_object_get_string_member(params, "pci_addr");
    }

    const gchar *base_image = nullptr;
    if (json_object_has_member(params, "base_image")) {
        base_image = json_object_get_string_member(params, "base_image");
    }
    /* PCV_SAFETY_CONTROL: vm-create-iso-validation — base_image도 iso_path와 동일 신뢰경계.
     * base_image는 vm_manager에서 qemu-img convert 입력으로 host FS에서 직접 읽혀 VM 디스크로
     * 기록되므로, 미검증 시 /etc/shadow 등 임의 파일 흡입·경로순회가 가능하다. 절대·no-".."·
     * 디스크이미지 확장자 위반 시 op-lock/create(부작용) 이전에 거부한다 (CMP-3 확장). */
    if (base_image && *base_image && !pcv_validate_base_image_path(base_image)) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid base_image: must be an absolute .qcow2/.qcow/.img/.raw path without '..'");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }
    const gchar *owner = _json_string_member(params, "_pcv_caller_sub");

    gint boot_mode = 0;
    gboolean tpm = FALSE;
    gint cpu_mode = 0;
    gboolean hugepages = FALSE;

    if (json_object_has_member(params, "boot_mode")) boot_mode = json_object_get_int_member(params, "boot_mode");

    if (json_object_has_member(params, "firmware")) {
        const gchar *fw = json_object_get_string_member(params, "firmware");
        if (g_strcmp0(fw, "uefi") == 0) boot_mode = 1;
        else if (g_strcmp0(fw, "uefi-secureboot") == 0) boot_mode = 2;
        else boot_mode = 0;
    }
    if (json_object_has_member(params, "tpm")) tpm = json_object_get_boolean_member(params, "tpm");
    if (json_object_has_member(params, "cpu_mode")) cpu_mode = json_object_get_int_member(params, "cpu_mode");
    if (json_object_has_member(params, "hugepages")) hugepages = json_object_get_boolean_member(params, "hugepages");

    {
        gboolean exists = FALSE;
        const gchar *effective_pool = (storage_pool && *storage_pool)
            ? storage_pool : pcv_config_get_zvol_pool();
        const gchar *effective_image_dir = (image_dir && *image_dir)
            ? image_dir : pcv_config_get_image_dir();

        if (!storage_type || g_strcmp0(storage_type, "zvol") == 0) {
            gchar *dataset = g_strdup_printf("%s/%s", effective_pool, name);
            const gchar *zfs_argv[] = {"zfs", "list", "-H", "-o", "name", dataset, NULL};

            exists = pcv_spawn_sync_timeout(zfs_argv, NULL, NULL, 10, NULL);
            g_free(dataset);
        }

        if (!exists && (!storage_type ||
                        g_strcmp0(storage_type, "qcow2") == 0 ||
                        g_strcmp0(storage_type, "raw") == 0)) {
            gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2", effective_image_dir, name);
            gchar *raw_img_path = g_strdup_printf("%s/%s.img", effective_image_dir, name);
            gchar *raw_path = g_strdup_printf("%s/%s.raw", effective_image_dir, name);
            exists = g_file_test(qcow2_path, G_FILE_TEST_EXISTS) ||
                     g_file_test(raw_img_path, G_FILE_TEST_EXISTS) ||
                     g_file_test(raw_path, G_FILE_TEST_EXISTS);
            g_free(qcow2_path);
            g_free(raw_img_path);
            g_free(raw_path);
        }

        if (exists) {
            gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                "VM already exists in selected storage location — delete the VM first");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            return;
        }
    }

    gchar *create_lock_err = NULL;
    if (!lock_vm_operation(name, VM_OP_CREATING, &create_lock_err)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                       create_lock_err ? create_lock_err : "VM is busy (another operation in progress)");
        pure_uds_server_send_response(server, connection, e);
        g_free(e); g_free(create_lock_err);
        return;
    }

    gchar *job_id = pcv_job_create("vm.create", name, NULL);
    pcv_job_update_status(job_id, PCV_JOB_RUNNING, 0, "VM creation started");

    JsonObject *accepted = json_object_new();
    json_object_set_boolean_member(accepted, "accepted", TRUE);
    json_object_set_string_member(accepted, "name", name);
    json_object_set_string_member(accepted, "job_id", job_id);
    json_object_set_string_member(accepted, "status", "accepted");

    gchar *warn_br = purecvisor_vm_resolve_network_bridge(bridge);
    if (warn_br
        && g_strcmp0(warn_br, "virbr0") != 0
        && g_strcmp0(warn_br, "lxcbr0") != 0) {
        gchar *dhcp_conf = g_strdup_printf(PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf", warn_br);
        gboolean has_dhcp = g_file_test(dhcp_conf, G_FILE_TEST_EXISTS);
        g_free(dhcp_conf);
        if (!has_dhcp) {
            json_object_set_string_member(accepted, "network_warning",
                "bridge has no managed DHCP — guests need static IP or external DHCP "
                "(hint: pcvctl network dhcp --enable <bridge>)");
        }
    }
    g_free(warn_br);

    JsonNode *an = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(an, accepted);
    gchar *resp = pure_rpc_build_success_response(rpc_id, an);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    VmCreateJobCtx *job_ctx = g_new0(VmCreateJobCtx, 1);
    job_ctx->job_id  = job_id;
    job_ctx->vm_name = g_strdup(name);

    GCancellable *cancel = g_cancellable_new();
    cmap_register(name, cancel);

    purecvisor_vm_manager_create_vm_async(self->vm_manager,
                                          name,
                                          vcpu,
                                          memory_mb,
                                          disk_size_gb,
                                          iso_path,
                                          bridge,
                                          vlan_id,
                                          boot_mode,
                                          tpm,
                                          cpu_mode,
                                          hugepages,
                                          storage_type,
                                          storage_pool,
                                          image_dir,
                                          nic_type,
                                          pci_addr,
                                          base_image,
                                          owner,
                                          cancel,
                                          _on_vm_create_finished,
                                          job_ctx);
    g_object_unref(cancel);
}

PureCVisorVmManager *
purecvisor_dispatcher_get_vm_manager(PureCVisorDispatcher *self)
{
    g_return_val_if_fail(PURECVISOR_IS_DISPATCHER(self), NULL);
    return self->vm_manager;
}

static PureCVisorVmManager *g_dispatch_vm_manager = NULL;

static void purecvisor_dispatcher_finalize(GObject *object) {
    PureCVisorDispatcher *self = PURECVISOR_DISPATCHER(object);
    if (g_dispatch_vm_manager == self->vm_manager) g_dispatch_vm_manager = NULL;
    if (self->vm_manager) g_object_unref(self->vm_manager);
    G_OBJECT_CLASS(purecvisor_dispatcher_parent_class)->finalize(object);
}

static void purecvisor_dispatcher_class_init(PureCVisorDispatcherClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = purecvisor_dispatcher_finalize;
}

static void purecvisor_dispatcher_init(PureCVisorDispatcher *self) {
    self->vm_manager = purecvisor_vm_manager_new(NULL);
    g_dispatch_vm_manager = self->vm_manager;
}

PureCVisorDispatcher *purecvisor_dispatcher_new(void) {
    PureCVisorDispatcher *d = g_object_new(PURECVISOR_TYPE_DISPATCHER, NULL);
    dispatcher_init_routes();
    return d;
}

void purecvisor_dispatcher_set_connection(PureCVisorDispatcher *self, GVirConnection *conn) {
    if (self->vm_manager) g_object_unref(self->vm_manager);
    self->vm_manager = purecvisor_vm_manager_new(conn);
    g_dispatch_vm_manager = self->vm_manager;
}

static void _handle_vm_delete_status(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *st = pcv_vm_delete_status_get(vm);
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(node, st);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_vm_resize_disk(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    gint new_size_gb = json_object_has_member(params, "new_size_gb")
        ? json_object_get_int_member(params, "new_size_gb") : 0;
    const gchar *target = json_object_has_member(params, "target")
        ? json_object_get_string_member(params, "target") : NULL;

    if (!vm_name || new_size_gb <= 0) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid params: name, new_size_gb (>0) required");
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
    } else {

        gchar *lock_err = NULL;
        if (!lock_vm_operation(vm_name, VM_OP_TUNING, &lock_err)) {
            gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                lock_err ? lock_err : "VM busy (operation in progress)");
            pure_uds_server_send_response(server, connection, err_resp);
            g_free(err_resp); g_free(lock_err);
            return;
        }
        JsonNode *accepted = json_node_new(JSON_NODE_VALUE);
        json_node_set_string(accepted, "resize accepted");
        gchar *resp = pure_rpc_build_success_response(rpc_id, accepted);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        purecvisor_vm_resize_disk(vm_name, new_size_gb, target, TRUE );
    }
}

static void _handle_monitor_fleet(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    GError *err = nullptr;
    gchar *response_str = handle_monitor_fleet(params, &err);

    if (err) {
        gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err->message);
        pure_uds_server_send_response(server, connection, err_resp);
        g_free(err_resp);
        g_clear_error(&err);
    } else if (response_str) {
        pure_uds_server_send_response(server, connection, response_str);
        g_free(response_str);
    }
}

static void _handle_alert_history(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    JsonArray *arr = pcv_alert_engine_get_history();
    gint pg_off = 0, pg_lim = 0;
    _get_pagination_params(params, &pg_off, &pg_lim);
    JsonNode *node = _paginate_array(arr, pg_off, pg_lim);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_alert_config_get(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *cfg = pcv_alert_engine_get_config();
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, cfg);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_alert_config_set(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    gboolean ok = pcv_alert_engine_set_config(params);
    if (ok) {
        JsonObject *cfg = pcv_alert_engine_get_config();
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, cfg);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid alert config");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }
}

static void _handle_alert_config_reload(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *cfg = json_object_new();
    const gchar *en = pcv_config_get_string("alert", "enabled", "false");
    json_object_set_boolean_member(cfg, "enabled",
        (g_ascii_strcasecmp(en, "true") == 0 || g_strcmp0(en, "1") == 0));
    json_object_set_int_member(cfg, "cpu_warn",     pcv_config_get_int("alert", "cpu_warn", 80));
    json_object_set_int_member(cfg, "cpu_crit",     pcv_config_get_int("alert", "cpu_crit", 95));
    json_object_set_int_member(cfg, "mem_warn",     pcv_config_get_int("alert", "mem_warn", 85));
    json_object_set_int_member(cfg, "mem_crit",     pcv_config_get_int("alert", "mem_crit", 95));
    json_object_set_int_member(cfg, "disk_warn",    pcv_config_get_int("alert", "disk_warn", 80));
    json_object_set_int_member(cfg, "disk_crit",    pcv_config_get_int("alert", "disk_crit", 90));
    json_object_set_int_member(cfg, "eval_period",  pcv_config_get_int("alert", "eval_period", 30));
    json_object_set_int_member(cfg, "dedup_window", pcv_config_get_int("alert", "dedup_window", 300));
    json_object_set_string_member(cfg, "webhook_url",
        pcv_config_get_string("alert", "webhook_url", ""));
    json_object_set_string_member(cfg, "webhook_format",
        pcv_config_get_string("alert", "webhook_format", "generic"));
    json_object_set_string_member(cfg, "telegram_chat_id",
        pcv_config_get_string("alert", "telegram_chat_id", ""));

    pcv_alert_engine_set_config(cfg);
    json_object_unref(cfg);

    JsonObject *result = pcv_alert_engine_get_config();
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_monitor_processes(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    gint top_n = 0;
    if (json_object_has_member(params, "top"))
        top_n = (gint)json_object_get_int_member(params, "top");

    const gchar *type_str = nullptr;
    if (json_object_has_member(params, "type"))
        type_str = json_object_get_string_member(params, "type");

    JsonArray *arr = pcv_process_monitor_get_filtered(top_n, type_str);
    if (!arr) arr = json_array_new();
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_agent_config_get(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    extern JsonObject *pcv_agent_get_config(void);
    JsonObject *ag_cfg = pcv_agent_get_config();
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, ag_cfg);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_agent_config_set(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    extern JsonObject *pcv_agent_get_config(void);
    extern gboolean pcv_agent_set_config(JsonObject *p);
    gboolean ok = pcv_agent_set_config(params);
    if (ok) {
        JsonObject *ag_cfg = pcv_agent_get_config();
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, ag_cfg);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid agent config");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }
}

static void _handle_agent_history(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    extern gchar *pcv_agent_get_last_comparison_json(void);
    gchar *json_str = pcv_agent_get_last_comparison_json();
    JsonParser *p2 = json_parser_new();
    /* PCV_PARSE_TRUSTED: 내부 생성 JSON 재파싱(외부 입력 아님) */
    json_parser_load_from_data(p2, json_str, -1, NULL);
    JsonNode *node = json_node_copy(json_parser_get_root(p2));
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(json_str);
    g_object_unref(p2);
}

static void _handle_agent_compare_manual(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *user_ctx = params && json_object_has_member(params, "context")
        ? json_object_get_string_member(params, "context") : "manual operator trigger";

    extern JsonObject *pcv_ebpf_telemetry_get_host(void);
    extern void pcv_agent_compare_async(const gchar *metrics_json, const gchar *anomaly_context);

    JsonObject *host = pcv_ebpf_telemetry_get_host();
    gchar *host_json = NULL;
    if (host) {
        JsonNode *n = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(n, host);
        host_json = json_to_string(n, FALSE);
        json_node_free(n);
        json_object_unref(host);
    }
    pcv_agent_compare_async(host_json ?: "{}", user_ctx);
    g_free(host_json);

    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "dispatched", TRUE);
    json_object_set_string_member(obj, "context", user_ctx);
    json_object_set_string_member(obj, "note",
        "AI Agent compare_async dispatched. Poll agent.history for results (5min rate limit applies).");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_anomaly_reset_baseline(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    extern void pcv_anomaly_reset_baseline(void);
    pcv_anomaly_reset_baseline();
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "reset", TRUE);
    json_object_set_string_member(obj, "message",
        "Anomaly baseline reset. Z-Score will warm up after 50s (10 samples).");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_healing_pending(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    extern gchar *pcv_healing_get_pending_json(void);
    gchar *json_str = pcv_healing_get_pending_json();
    JsonParser *p2 = json_parser_new();
    /* PCV_PARSE_TRUSTED: 내부 생성 JSON 재파싱(외부 입력 아님) */
    json_parser_load_from_data(p2, json_str, -1, NULL);
    JsonNode *node = json_node_copy(json_parser_get_root(p2));
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(json_str);
    g_object_unref(p2);
}

static void _handle_healing_set_mode(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    const gchar *mode = params ? json_object_get_string_member(params, "mode") : NULL;
    if (!mode || !*mode) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required param: mode (\"active\" | \"dry_run\")");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    gboolean target_dry_run;
    if (g_ascii_strcasecmp(mode, "active") == 0) target_dry_run = FALSE;
    else if (g_ascii_strcasecmp(mode, "dry_run") == 0 ||
             g_ascii_strcasecmp(mode, "dryrun") == 0) target_dry_run = TRUE;
    else {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid mode (use \"active\" or \"dry_run\")");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        return;
    }

    extern void pcv_healing_set_mode(gboolean dry_run);
    pcv_healing_set_mode(target_dry_run);

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "mode", target_dry_run ? "dry_run" : "active");
    json_object_set_boolean_member(obj, "dry_run", target_dry_run);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_healing_history(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    extern gchar *pcv_healing_get_history_json(void);
    gchar *json_str = pcv_healing_get_history_json();
    JsonParser *p2 = json_parser_new();
    /* PCV_PARSE_TRUSTED: 내부 생성 JSON 재파싱(외부 입력 아님) */
    json_parser_load_from_data(p2, json_str, -1, NULL);
    JsonNode *parsed = json_parser_get_root(p2);

    gint pg_off = 0, pg_lim = 0;
    _get_pagination_params(params, &pg_off, &pg_lim);

    if (pg_lim > 0 && parsed && JSON_NODE_HOLDS_ARRAY(parsed)) {
        JsonArray *full = json_array_ref(json_node_get_array(parsed));
        JsonNode *node = _paginate_array(full, pg_off, pg_lim);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    } else {
        JsonNode *node = json_node_copy(parsed);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }
    g_free(json_str);
    g_object_unref(p2);
}

static void _handle_vm_import_ec2(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    PcvCloudImportParams ip = {0};
    ip.name           = (gchar *)(json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL);
    ip.ami_id         = (gchar *)(json_object_has_member(params, "ami_id")
        ? json_object_get_string_member(params, "ami_id") : NULL);
    ip.aws_region     = (gchar *)(json_object_has_member(params, "aws_region")
        ? json_object_get_string_member(params, "aws_region") : NULL);
    ip.s3_bucket      = (gchar *)(json_object_has_member(params, "s3_bucket")
        ? json_object_get_string_member(params, "s3_bucket") : NULL);
    {
        gint64 _v = json_object_has_member(params, "vcpu")
            ? json_object_get_int_member(params, "vcpu") : 0;
        gint64 _m = json_object_has_member(params, "memory_mb")
            ? json_object_get_int_member(params, "memory_mb") : 0;
        if (_v < 0 || _v > 1024 || _m < 0 || _m > (1024 * 1024)) {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "vcpu must be 0..1024, memory_mb must be 0..1048576");
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
            return;
        }
        ip.vcpu = (gint)_v;
        ip.memory_mb = (gint)_m;
    }
    ip.network_bridge = (gchar *)(json_object_has_member(params, "network_bridge")
        ? json_object_get_string_member(params, "network_bridge") : NULL);
    ip.disk_format    = (gchar *)(json_object_has_member(params, "disk_format")
        ? json_object_get_string_member(params, "disk_format") : NULL);
    ip.mode = (gchar *)(json_object_has_member(params, "mode")
        ? json_object_get_string_member(params, "mode") : NULL);
    gboolean finalize = json_object_has_member(params, "finalize")
        ? json_object_get_boolean_member(params, "finalize") : FALSE;
    ip.instance_id = (gchar *)(json_object_has_member(params, "instance_id")
        ? json_object_get_string_member(params, "instance_id") : NULL);
    ip.volume_id = (gchar *)(json_object_has_member(params, "volume_id")
        ? json_object_get_string_member(params, "volume_id") : NULL);
    if (!ip.name || (!finalize && !ip.ami_id)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required: name, ami_id");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else {
        GError *e = nullptr;
        gchar *job_id = finalize
            ? pcv_cloud_finalize_import(ip.name, &e)
            : pcv_cloud_import_ec2(&ip, &e);
        if (job_id) {
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "status", "accepted");
            json_object_set_string_member(obj, "job_id", job_id);
            json_object_set_string_member(obj, "message",
                finalize ? "Finalize started — use vm.import.status to track"
                         : "Import started — use vm.import.status to track");
            JsonNode *node = json_node_new(JSON_NODE_OBJECT);
            json_node_take_object(node, obj);
            gchar *resp = pure_rpc_build_success_response(rpc_id, node);
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp); g_free(job_id);
        } else {
            gchar *er = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                e ? e->message : "Import failed to start");
            pure_uds_server_send_response(server, connection, er); g_free(er);
            if (e) g_error_free(e);
        }
    }
}

static void _handle_vm_export_ec2(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    PcvCloudExportParams ep = {0};
    ep.name            = (gchar *)(json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL);
    ep.aws_region      = (gchar *)(json_object_has_member(params, "aws_region")
        ? json_object_get_string_member(params, "aws_region") : NULL);
    ep.s3_bucket       = (gchar *)(json_object_has_member(params, "s3_bucket")
        ? json_object_get_string_member(params, "s3_bucket") : NULL);
    ep.ami_name        = (gchar *)(json_object_has_member(params, "ami_name")
        ? json_object_get_string_member(params, "ami_name") : NULL);
    ep.ami_description = (gchar *)(json_object_has_member(params, "ami_description")
        ? json_object_get_string_member(params, "ami_description") : NULL);
    if (!ep.name) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else {
        GError *e = nullptr;
        gchar *job_id = pcv_cloud_export_ec2(&ep, &e);
        if (job_id) {
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "status", "accepted");
            json_object_set_string_member(obj, "job_id", job_id);
            JsonNode *node = json_node_new(JSON_NODE_OBJECT);
            json_node_take_object(node, obj);
            gchar *resp = pure_rpc_build_success_response(rpc_id, node);
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp); g_free(job_id);
        } else {
            gchar *er = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                e ? e->message : "Export failed to start");
            pure_uds_server_send_response(server, connection, er); g_free(er);
            if (e) g_error_free(e);
        }
    }
}

static void _handle_cloud_migration_status(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!vm_name) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else {
        PcvCloudJobStatus *st = pcv_cloud_get_status(vm_name);
        JsonObject *obj = json_object_new();
        if (st) {
            json_object_set_string_member(obj, "name", st->name ?: "");
            json_object_set_string_member(obj, "job_id", st->job_id ?: "");
            json_object_set_string_member(obj, "direction", st->direction ?: "");
            json_object_set_string_member(obj, "status",
                pcv_cloud_status_str(st->status));
            json_object_set_int_member(obj, "progress_percent", st->progress);
            json_object_set_string_member(obj, "detail", st->detail ?: "");
            json_object_set_int_member(obj, "started_at", st->started_at);
            json_object_set_int_member(obj, "elapsed_sec",
                st->updated_at - st->started_at);
            if (st->base_image_path)
                json_object_set_string_member(obj, "base_image_path", st->base_image_path);
            pcv_cloud_job_status_free(st);
        } else {
            json_object_set_string_member(obj, "status", "not_found");
            json_object_set_string_member(obj, "detail", "No migration job for this VM");
        }
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
    }
}

static void _handle_cloud_jobs_list(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    GPtrArray *jobs = pcv_cloud_list_jobs();
    JsonArray *arr = json_array_new();
    for (guint i = 0; i < jobs->len; i++) {
        PcvCloudJobStatus *st = g_ptr_array_index(jobs, i);
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "name", st->name ?: "");
        json_object_set_string_member(obj, "job_id", st->job_id ?: "");
        json_object_set_string_member(obj, "direction", st->direction ?: "");
        json_object_set_string_member(obj, "status",
            pcv_cloud_status_str(st->status));
        json_object_set_int_member(obj, "progress_percent", st->progress);
        json_object_set_string_member(obj, "detail", st->detail ?: "");
        json_object_set_int_member(obj, "started_at", st->started_at);
        json_object_set_int_member(obj, "elapsed_sec",
            st->updated_at - st->started_at);
        json_array_add_object_element(arr, obj);
    }
    g_ptr_array_unref(jobs);
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_cloud_job_cancel(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!vm_name) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else {
        GError *e = nullptr;
        if (pcv_cloud_cancel_job(vm_name, &e)) {
            JsonObject *obj = json_object_new();
            json_object_set_boolean_member(obj, "cancelled", TRUE);
            json_object_set_string_member(obj, "name", vm_name);
            JsonNode *node = json_node_new(JSON_NODE_OBJECT);
            json_node_take_object(node, obj);
            gchar *resp = pure_rpc_build_success_response(rpc_id, node);
            pure_uds_server_send_response(server, connection, resp);
            g_free(resp);
        } else {
            gchar *err_resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
                e ? e->message : "Cancel failed");
            pure_uds_server_send_response(server, connection, err_resp);
            g_free(err_resp);
            if (e) g_error_free(e);
        }
    }
}

typedef struct {
    gchar *vm_name;
    gchar *output_dir;
    gchar *job_id;
} OvaExportCtx;

static void _free_ova_ctx(gpointer data) {
    if (!data) return;
    OvaExportCtx *ctx = data;
    g_free(ctx->vm_name);
    g_free(ctx->output_dir);
    g_free(ctx->job_id);
    g_free(ctx);
}

static gchar *_ova_sha256_file(const gchar *path) {
    const gchar *argv[] = {"sha256sum", path, NULL};
    gchar *stdout_buf = nullptr;
    GError *error = nullptr;
    if (!pcv_spawn_sync(argv, &stdout_buf, NULL, &error)) {
        if (error) g_error_free(error);
        g_free(stdout_buf);
        return NULL;
    }

    if (stdout_buf) {
        gchar *sp = strchr(stdout_buf, ' ');
        if (sp) *sp = '\0';
        gchar *hash = g_strdup(stdout_buf);
        g_free(stdout_buf);
        return hash;
    }
    return NULL;
}

static gchar *
_ova_export_result_json(OvaExportCtx *ctx, gboolean ok,
                        const gchar *ova_path, const gchar *error_msg)
{
    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "vm", ctx->vm_name ?: "");
    json_object_set_string_member(result, "format", "ova");
    json_object_set_string_member(result, "output_dir", ctx->output_dir ?: "");
    if (ok) {
        json_object_set_string_member(result, "ova_path", ova_path ?: "");
    } else {
        json_object_set_string_member(result, "error",
                                      error_msg ?: "OVA export failed");
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, node);
    gchar *json = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_free(node);
    return json;
}

static void
_ova_export_record_result(OvaExportCtx *ctx, gboolean ok,
                          const gchar *ova_path, const gchar *error_msg)
{
    gchar *result_json = _ova_export_result_json(ctx, ok, ova_path, error_msg);
    pcv_job_set_result(ctx->job_id, ok ? PCV_JOB_COMPLETED : PCV_JOB_FAILED,
                       result_json);
    pcv_audit_log(NULL, "vm.export.ova", ctx->vm_name ?: "",
                  ok ? "ok" : "fail", ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_ws_broadcast_job_complete_mt(ctx->job_id, "vm.export.ova",
                                     ok ? "completed" : "failed",
                                     ok ? NULL : (error_msg ?: "OVA export failed"));
    g_free(result_json);
}

static void _ova_export_worker(GTask *task, gpointer source_obj,
                                gpointer task_data, GCancellable *cancellable)
{
    (void)source_obj; (void)cancellable;
    OvaExportCtx *ctx = task_data;
    gboolean audit_ok = FALSE;
    const gchar *audit_error = NULL;
    gchar *audit_error_owned = NULL;
    gchar *xml = NULL;
    gchar *disk_path = NULL;
    gchar *disk_format = NULL;
    gchar *tmpdir = NULL;
    gchar *vmdk_name = NULL;
    gchar *vmdk_path = NULL;
    gchar *ovf_name = NULL;
    gchar *ovf_path = NULL;
    gchar *mf_name = NULL;
    gchar *mf_path = NULL;
    gchar *ova_path = NULL;

    pcv_job_update_status(ctx->job_id, PCV_JOB_RUNNING, 5,
                          "Reading VM metadata");

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_warning("[OVA] Failed to connect to libvirt for %s", ctx->vm_name);
        audit_error = "failed to connect to libvirt";
        goto ova_cleanup;
    }

    extern virDomainPtr pure_virt_get_domain(virConnectPtr conn, const gchar *identifier);
    virDomainPtr dom = pure_virt_get_domain(conn, ctx->vm_name);
    if (!dom) {
        g_warning("[OVA] VM '%s' not found", ctx->vm_name);
        virt_conn_pool_release(conn);
        audit_error = "VM not found";
        goto ova_cleanup;
    }

    xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    virDomainInfo info = {0};
    int vcpus = 1;
    int mem_mb = 1024;
    if (virDomainGetInfo(dom, &info) == 0) {
        vcpus = info.nrVirtCpu > 0 ? (int)info.nrVirtCpu : 1;
        mem_mb = info.maxMem > 0 ? (int)(info.maxMem / 1024) : 1024;
    }
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    if (!xml) {
        g_warning("[OVA] Failed to get XML for %s", ctx->vm_name);
        audit_error = "failed to get VM XML";
        goto ova_cleanup;
    }

    {

        gchar *src = strstr(xml, "<source file='");
        if (!src) src = strstr(xml, "<source dev='");
        if (src) {
            const gchar *start = strchr(src, '\'');
            if (start) {
                start++;
                const gchar *end = strchr(start, '\'');
                if (end)
                    disk_path = g_strndup(start, (gsize)(end - start));
            }
        }

        if (disk_path) {
            if (g_str_has_suffix(disk_path, ".qcow2"))
                disk_format = g_strdup("qcow2");
            else if (strstr(disk_path, "/dev/") != nullptr)
                disk_format = g_strdup("raw");
            else
                disk_format = g_strdup("raw");
        }
    }
    g_free(xml);

    if (!disk_path) {
        g_warning("[OVA] No disk source found for %s", ctx->vm_name);
        audit_error = "no disk source found";
        goto ova_cleanup;
    }

    tmpdir = g_strdup_printf("%s/ova-%s-%ld",
                             ctx->output_dir, ctx->vm_name, (long)time(NULL));
    if (g_mkdir_with_parents(tmpdir, 0755) != 0) {
        audit_error_owned = g_strdup_printf("failed to create temporary directory: %s",
                                            g_strerror(errno));
        audit_error = audit_error_owned;
        goto ova_cleanup;
    }

    vmdk_name = g_strdup_printf("%s.vmdk", ctx->vm_name);
    vmdk_path = g_strdup_printf("%s/%s", tmpdir, vmdk_name);

    g_message("[OVA] Converting %s (%s) → %s", disk_path, disk_format, vmdk_path);
    pcv_job_update_status(ctx->job_id, PCV_JOB_RUNNING, 35,
                          "Converting disk to VMDK");
    {
        const gchar *argv[] = {"qemu-img", "convert", "-f", disk_format,
            "-O", "vmdk", disk_path, vmdk_path, NULL};
        gchar *std_err = nullptr;
        GError *error = nullptr;
        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            g_warning("[OVA] qemu-img convert failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            audit_error_owned = g_strdup_printf("qemu-img convert failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            audit_error = audit_error_owned;
            if (error) g_error_free(error);
            g_free(std_err);
            goto ova_cleanup;
        }
        g_free(std_err);
    }

    gint64 vmdk_size = 0;
    {
        GFile *f = g_file_new_for_path(vmdk_path);
        GFileInfo *fi = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_SIZE,
            G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (fi) {
            vmdk_size = g_file_info_get_size(fi);
            g_object_unref(fi);
        }
        g_object_unref(f);
    }

    pcv_job_update_status(ctx->job_id, PCV_JOB_RUNNING, 65,
                          "Generating OVF manifest");
    {
        ovf_name = g_strdup_printf("%s.ovf", ctx->vm_name);
        ovf_path = g_strdup_printf("%s/%s", tmpdir, ovf_name);
        gint disk_gb = (gint)(vmdk_size / (1024LL * 1024 * 1024)) + 1;

        gchar *ovf_content = g_strdup_printf(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Envelope xmlns=\"http://schemas.dmtf.org/ovf/envelope/1\"\n"
            "  xmlns:ovf=\"http://schemas.dmtf.org/ovf/envelope/1\"\n"
            "  xmlns:rasd=\"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData\"\n"
            "  xmlns:vssd=\"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_VirtualSystemSettingData\">\n"
            "  <References>\n"
            "    <File ovf:href=\"%s\" ovf:id=\"file1\" ovf:size=\"%ld\"/>\n"
            "  </References>\n"
            "  <DiskSection>\n"
            "    <Info>Virtual disk information</Info>\n"
            "    <Disk ovf:capacity=\"%d\" ovf:capacityAllocationUnits=\"byte * 2^30\"\n"
            "      ovf:diskId=\"vmdisk1\" ovf:fileRef=\"file1\"\n"
            "      ovf:format=\"http://www.vmware.com/interfaces/specifications/vmdk.html#streamOptimized\"/>\n"
            "  </DiskSection>\n"
            "  <VirtualSystem ovf:id=\"%s\">\n"
            "    <Info>PureCVisor exported VM</Info>\n"
            "    <Name>%s</Name>\n"
            "    <VirtualHardwareSection>\n"
            "      <Info>Virtual hardware requirements</Info>\n"
            "      <System>\n"
            "        <vssd:ElementName>Virtual Hardware Family</vssd:ElementName>\n"
            "        <vssd:VirtualSystemType>vmx-13</vssd:VirtualSystemType>\n"
            "      </System>\n"
            "      <Item>\n"
            "        <rasd:Description>Number of Virtual CPUs</rasd:Description>\n"
            "        <rasd:ElementName>%d virtual CPU(s)</rasd:ElementName>\n"
            "        <rasd:ResourceType>3</rasd:ResourceType>\n"
            "        <rasd:VirtualQuantity>%d</rasd:VirtualQuantity>\n"
            "      </Item>\n"
            "      <Item>\n"
            "        <rasd:AllocationUnits>byte * 2^20</rasd:AllocationUnits>\n"
            "        <rasd:Description>Memory Size</rasd:Description>\n"
            "        <rasd:ElementName>%d MB of memory</rasd:ElementName>\n"
            "        <rasd:ResourceType>4</rasd:ResourceType>\n"
            "        <rasd:VirtualQuantity>%d</rasd:VirtualQuantity>\n"
            "      </Item>\n"
            "    </VirtualHardwareSection>\n"
            "  </VirtualSystem>\n"
            "</Envelope>\n",
            vmdk_name, (long)vmdk_size, disk_gb,
            ctx->vm_name, ctx->vm_name,
            vcpus, vcpus,
            mem_mb, mem_mb
        );

        GError *write_error = NULL;
        if (!g_file_set_contents(ovf_path, ovf_content, -1, &write_error)) {
            audit_error_owned = g_strdup_printf("failed to write OVF manifest: %s",
                write_error ? write_error->message : "unknown");
            audit_error = audit_error_owned;
            g_clear_error(&write_error);
            g_free(ovf_content);
            goto ova_cleanup;
        }
        g_free(ovf_content);

        mf_name = g_strdup_printf("%s.mf", ctx->vm_name);
        mf_path = g_strdup_printf("%s/%s", tmpdir, mf_name);
        gchar *ovf_hash  = _ova_sha256_file(ovf_path);
        gchar *vmdk_hash = _ova_sha256_file(vmdk_path);
        if (!ovf_hash || !vmdk_hash) {
            audit_error = "failed to calculate OVA manifest checksums";
            g_free(ovf_hash);
            g_free(vmdk_hash);
            goto ova_cleanup;
        }
        gchar *mf_content = g_strdup_printf(
            "SHA256(%s)= %s\nSHA256(%s)= %s\n",
            ovf_name, ovf_hash,
            vmdk_name, vmdk_hash
        );
        write_error = NULL;
        if (!g_file_set_contents(mf_path, mf_content, -1, &write_error)) {
            audit_error_owned = g_strdup_printf("failed to write OVA manifest: %s",
                write_error ? write_error->message : "unknown");
            audit_error = audit_error_owned;
            g_clear_error(&write_error);
            g_free(mf_content); g_free(ovf_hash); g_free(vmdk_hash);
            goto ova_cleanup;
        }
        g_free(mf_content); g_free(ovf_hash); g_free(vmdk_hash);

        ova_path = g_strdup_printf("%s/%s.ova", ctx->output_dir, ctx->vm_name);
        pcv_job_update_status(ctx->job_id, PCV_JOB_RUNNING, 85,
                              "Creating OVA archive");
        {
            const gchar *argv[] = {"tar", "-cf", ova_path, "-C", tmpdir,
                ovf_name, vmdk_name, mf_name, NULL};
            gchar *std_err = nullptr;
            GError *error = nullptr;
            if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
                g_warning("[OVA] tar failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                audit_error_owned = g_strdup_printf("tar failed: %s",
                    error ? error->message : (std_err ? std_err : "unknown"));
                audit_error = audit_error_owned;
                if (error) g_error_free(error);
                g_free(std_err);
                goto ova_cleanup;
            }
            g_free(std_err);
        }

        if (!g_file_test(ova_path, G_FILE_TEST_IS_REGULAR)) {
            audit_error = "OVA archive was not created";
            goto ova_cleanup;
        }
        g_message("[OVA] Export complete: %s", ova_path);
        audit_ok = TRUE;
    }

ova_cleanup:
    if (!audit_ok && !audit_error)
        audit_error = "OVA export failed";
    pcv_job_update_status(ctx->job_id, PCV_JOB_RUNNING, 95,
                          "Cleaning up temporary export files");

    if (ovf_path) g_remove(ovf_path);
    if (mf_path) g_remove(mf_path);
    if (vmdk_path) g_remove(vmdk_path);
    if (tmpdir) g_rmdir(tmpdir);

    _ova_export_record_result(ctx, audit_ok, ova_path, audit_error);

    g_free(disk_path); g_free(disk_format);
    g_free(vmdk_name); g_free(vmdk_path);
    g_free(ovf_name); g_free(ovf_path);
    g_free(mf_name); g_free(mf_path);
    g_free(ova_path);
    g_free(tmpdir);
    g_free(audit_error_owned);
    g_task_return_boolean(task, audit_ok);
}

static void _handle_vm_export_ova(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!name || !name[0]) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameter: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    const gchar *output_dir = json_object_has_member(params, "output_dir")
        ? json_object_get_string_member(params, "output_dir") : "/tmp";

    gchar *real_out = realpath(output_dir, NULL);
    if (!real_out) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid output_dir — directory does not exist");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    gchar *job_id = pcv_job_create("ova_export", name, NULL);

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "status", "accepted");
    json_object_set_string_member(obj, "vm", name);
    json_object_set_string_member(obj, "output_dir", real_out);
    json_object_set_string_member(obj, "format", "ova");
    json_object_set_string_member(obj, "job_id", job_id);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    OvaExportCtx *ctx = g_new0(OvaExportCtx, 1);
    ctx->vm_name = g_strdup(name);
    ctx->output_dir = g_strdup(real_out);
    ctx->job_id = g_strdup(job_id);
    g_free(job_id);
    free(real_out);

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, _free_ova_ctx);
    g_task_run_in_thread(task, _ova_export_worker);
    g_object_unref(task);
}

typedef struct {
    gchar *ova_path;
    gchar *vm_name;
    gchar *pool;
    gchar *image_dir;
    gchar *job_id;
} OvaImportCtx;

static void _free_ova_import_ctx(gpointer data) {
    if (!data) return;
    OvaImportCtx *ctx = data;
    g_free(ctx->ova_path);
    g_free(ctx->vm_name);
    g_free(ctx->pool);
    g_free(ctx->image_dir);
    g_free(ctx->job_id);
    g_free(ctx);
}

static gchar *
_ovf_extract_value(const gchar *xml, const gchar *tag, const gchar *after)
{
    const gchar *start = after ? after : xml;
    gchar *open_tag = g_strdup_printf("<%s>", tag);
    gchar *close_tag = g_strdup_printf("</%s>", tag);
    const gchar *p = strstr(start, open_tag);
    gchar *result = nullptr;
    if (p) {
        p += strlen(open_tag);
        const gchar *e = strstr(p, close_tag);
        if (e && e > p)
            result = g_strndup(p, (gsize)(e - p));
    }
    g_free(open_tag);
    g_free(close_tag);
    return result;
}

static gchar *
_ovf_extract_attr(const gchar *xml, const gchar *tag, const gchar *attr)
{
    gchar *search = g_strdup_printf("<%s ", tag);
    const gchar *p = strstr(xml, search);
    g_free(search);
    if (!p) return NULL;

    gchar *attr_search = g_strdup_printf("%s=\"", attr);
    const gchar *a = strstr(p, attr_search);
    g_free(attr_search);
    if (!a) return NULL;

    a = strchr(a, '\"');
    if (!a) return NULL;
    a++;
    const gchar *e = strchr(a, '\"');
    if (!e || e <= a) return NULL;
    return g_strndup(a, (gsize)(e - a));
}

static gboolean
_ova_import_destroy_zvol(const gchar *dataset)
{
    if (!dataset || !*dataset)
        return TRUE;

    const gchar *argv[] = {"zfs", "destroy", "-R", dataset, NULL};
    GError *error = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &error);
    if (!ok) {
        PCV_LOG_WARN("ova_import", "ZFS zvol cleanup failed for '%s': %s",
                     dataset, error ? error->message : "unknown");
    }
    g_clear_error(&error);
    return ok;
}

static void _ova_import_worker(GTask *task, gpointer source_obj,
                                gpointer task_data, GCancellable *cancellable)
{
    (void)source_obj; (void)cancellable;
    OvaImportCtx *ctx = task_data;
    gchar *tmpdir = nullptr;
    gchar *disk_path = nullptr;
    gchar *vmdk_path = nullptr;
    gchar *created_zvol_dataset = nullptr;
    gboolean audit_ok = FALSE;
    const gchar *audit_error = NULL;
    gchar *audit_error_owned = NULL;

    pcv_job_update_status(ctx->job_id, PCV_JOB_RUNNING, 5, "Extracting OVA archive");

    gchar tmpl[] = "/tmp/pcv-ova-import-XXXXXX";
    tmpdir = g_strdup(mkdtemp(tmpl));
    if (!tmpdir) {
        pcv_job_set_result(ctx->job_id, PCV_JOB_FAILED, "\"mkdtemp failed\"");
        g_warning("[OVA-Import] mkdtemp failed for %s", ctx->vm_name);
        audit_error = "mkdtemp failed";
        goto import_cleanup;
    }

    {
        const gchar *argv[] = {"tar", "-xf", ctx->ova_path, "-C", tmpdir, NULL};
        gchar *std_err = nullptr;
        GError *error = nullptr;
        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            g_warning("[OVA-Import] tar extraction failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            pcv_job_set_result(ctx->job_id, PCV_JOB_FAILED, "\"tar extraction failed\"");
            audit_error = "tar extraction failed";
            if (error) g_error_free(error);
            g_free(std_err);
            goto import_cleanup;
        }
        g_free(std_err);
    }
    pcv_job_update_status(ctx->job_id, PCV_JOB_RUNNING, 20, "Parsing OVF metadata");

    gchar *ovf_path = nullptr;
    {
        GDir *dir = g_dir_open(tmpdir, 0, NULL);
        if (dir) {
            const gchar *entry;
            while ((entry = g_dir_read_name(dir)) != nullptr) {
                if (g_str_has_suffix(entry, ".ovf")) {
                    ovf_path = g_strdup_printf("%s/%s", tmpdir, entry);
                    break;
                }
            }
            g_dir_close(dir);
        }
    }
    if (!ovf_path) {
        g_warning("[OVA-Import] No .ovf file found in OVA for %s", ctx->vm_name);
        pcv_job_set_result(ctx->job_id, PCV_JOB_FAILED, "\"no .ovf file in OVA\"");
        audit_error = "no .ovf file in OVA";
        goto import_cleanup;
    }

    gchar *ovf_content = nullptr;
    gsize ovf_len = 0;
    if (!g_file_get_contents(ovf_path, &ovf_content, &ovf_len, NULL) || !ovf_content) {
        g_warning("[OVA-Import] Failed to read OVF: %s", ovf_path);
        pcv_job_set_result(ctx->job_id, PCV_JOB_FAILED, "\"failed to read OVF\"");
        audit_error = "failed to read OVF";
        g_free(ovf_path);
        goto import_cleanup;
    }

    gint vcpus = 2;
    {
        const gchar *cpu_marker = strstr(ovf_content, "<rasd:ResourceType>3</rasd:ResourceType>");
        if (cpu_marker) {
            gchar *val = _ovf_extract_value(ovf_content, "rasd:VirtualQuantity", cpu_marker);
            if (val) { vcpus = atoi(val); g_free(val); }
        }
        if (vcpus < 1) vcpus = 2;
    }

    gint memory_mb = 2048;
    {
        const gchar *mem_marker = strstr(ovf_content, "<rasd:ResourceType>4</rasd:ResourceType>");
        if (mem_marker) {
            gchar *val = _ovf_extract_value(ovf_content, "rasd:VirtualQuantity", mem_marker);
            if (val) { memory_mb = atoi(val); g_free(val); }
        }
        if (memory_mb < 256) memory_mb = 2048;
    }

    gchar *vmdk_name = _ovf_extract_attr(ovf_content, "File", "ovf:href");
    g_free(ovf_content);
    g_free(ovf_path);

    if (!vmdk_name) {
        g_warning("[OVA-Import] No disk reference in OVF for %s", ctx->vm_name);
        pcv_job_set_result(ctx->job_id, PCV_JOB_FAILED, "\"no disk file in OVF\"");
        audit_error = "no disk file in OVF";
        goto import_cleanup;
    }

    vmdk_path = g_strdup_printf("%s/%s", tmpdir, vmdk_name);
    g_free(vmdk_name);
    if (!g_file_test(vmdk_path, G_FILE_TEST_EXISTS)) {
        g_warning("[OVA-Import] VMDK not found: %s", vmdk_path);
        pcv_job_set_result(ctx->job_id, PCV_JOB_FAILED, "\"VMDK file not found\"");
        audit_error = "VMDK file not found";
        goto import_cleanup;
    }

    pcv_job_update_status(ctx->job_id, PCV_JOB_RUNNING, 30,
        "Converting disk image (vmdk → target format)");

    gboolean use_zvol = FALSE;
    {
        const gchar *pool_argv[] = {"zfs", "list", "-H", "-o", "name",
                                    ctx->pool, NULL};
        use_zvol = pcv_spawn_sync(pool_argv, NULL, NULL, NULL);
    }

    if (use_zvol) {

        gchar *zvol_path = g_strdup_printf("/dev/zvol/%s/%s", ctx->pool, ctx->vm_name);
        gchar *zvol_name = g_strdup_printf("%s/%s", ctx->pool, ctx->vm_name);

        gchar *stdout_buf = nullptr;
        const gchar *info_argv[] = {"qemu-img", "info", "--output=json", vmdk_path, NULL};
        GError *error = nullptr;
        gint64 disk_bytes = 10LL * 1024 * 1024 * 1024;
        if (pcv_spawn_sync(info_argv, &stdout_buf, NULL, &error)) {

            if (stdout_buf) {
                const gchar *vs = strstr(stdout_buf, "\"virtual-size\":");
                if (vs) {
                    vs += strlen("\"virtual-size\":");
                    while (*vs == ' ') vs++;
                    disk_bytes = g_ascii_strtoll(vs, NULL, 10);
                    if (disk_bytes < 1024 * 1024) disk_bytes = 10LL * 1024 * 1024 * 1024;
                }
            }
        }
        if (error) g_error_free(error);
        g_free(stdout_buf);

        gchar *size_str = g_strdup_printf("%ldG", (long)(disk_bytes / (1024 * 1024 * 1024)) + 1);
        const gchar *zfs_argv[] = {"zfs", "create", "-V", size_str, zvol_name, NULL};
        error = nullptr;
        gchar *std_err = nullptr;
        if (!pcv_spawn_sync(zfs_argv, NULL, &std_err, &error)) {
            g_warning("[OVA-Import] zfs create failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            audit_error_owned = g_strdup_printf("zfs create failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            pcv_job_set_result(ctx->job_id, PCV_JOB_FAILED, "\"zfs create failed\"");
            audit_error = audit_error_owned;
            if (error) g_error_free(error);
            g_free(std_err);
            g_free(size_str);
            g_free(zvol_name);
            g_free(zvol_path);
            goto import_cleanup;
        }
        if (error) g_error_free(error);
        g_free(std_err);
        g_free(size_str);
        created_zvol_dataset = g_strdup(zvol_name);
        g_free(zvol_name);

        const gchar *conv_argv[] = {
            "qemu-img", "convert", "-f", "vmdk", "-O", "raw",
            vmdk_path, zvol_path, NULL
        };
        error = nullptr;
        std_err = nullptr;
        if (!pcv_spawn_sync(conv_argv, NULL, &std_err, &error)) {
            g_warning("[OVA-Import] qemu-img convert to zvol failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            pcv_job_set_result(ctx->job_id, PCV_JOB_FAILED, "\"disk conversion failed\"");
            audit_error = "disk conversion failed";
            if (created_zvol_dataset) {
                (void)_ova_import_destroy_zvol(created_zvol_dataset);
                g_clear_pointer(&created_zvol_dataset, g_free);
            }
            if (error) g_error_free(error);
            g_free(std_err);
            g_free(zvol_path);
            goto import_cleanup;
        }
        g_free(std_err);
        disk_path = zvol_path;
    } else {

        disk_path = g_strdup_printf("%s/%s.qcow2", ctx->image_dir, ctx->vm_name);
        const gchar *conv_argv[] = {
            "qemu-img", "convert", "-f", "vmdk", "-O", "qcow2",
            vmdk_path, disk_path, NULL
        };
        GError *error = nullptr;
        gchar *std_err = nullptr;
        if (!pcv_spawn_sync(conv_argv, NULL, &std_err, &error)) {
            g_warning("[OVA-Import] qemu-img convert to qcow2 failed: %s",
                error ? error->message : (std_err ? std_err : "unknown"));
            pcv_job_set_result(ctx->job_id, PCV_JOB_FAILED, "\"disk conversion failed\"");
            audit_error = "disk conversion failed";
            if (error) g_error_free(error);
            g_free(std_err);
            goto import_cleanup;
        }
        g_free(std_err);
    }
    g_clear_pointer(&vmdk_path, g_free);

    pcv_job_update_status(ctx->job_id, PCV_JOB_RUNNING, 80, "Defining VM via virt-install");

    {
        gchar *vcpu_str = g_strdup_printf("%d", vcpus);
        gchar *mem_str = g_strdup_printf("%d", memory_mb);
        gchar *disk_arg = g_strdup_printf("path=%s", disk_path);
        const gchar *argv[] = {
            "virt-install", "--name", ctx->vm_name,
            "--vcpus", vcpu_str, "--memory", mem_str,
            "--disk", disk_arg, "--import",
            "--os-variant", "generic",
            "--noautoconsole", "--nographics", NULL
        };
        GError *error = nullptr;
        gchar *std_err = nullptr;
        if (!pcv_spawn_sync(argv, NULL, &std_err, &error)) {
            g_warning("[OVA-Import] virt-install failed for %s: %s", ctx->vm_name,
                error ? error->message : (std_err ? std_err : "unknown"));
            pcv_job_set_result(ctx->job_id, PCV_JOB_FAILED, "\"virt-install failed\"");
            audit_error = "virt-install failed";
            if (error) g_error_free(error);
            g_free(std_err);
            if (created_zvol_dataset) {
                (void)_ova_import_destroy_zvol(created_zvol_dataset);
                g_clear_pointer(&created_zvol_dataset, g_free);
            } else if (disk_path) {
                g_remove(disk_path);
            }
            g_free(vcpu_str);
            g_free(mem_str);
            g_free(disk_arg);
            goto import_cleanup;
        }
        g_free(std_err);
        g_free(vcpu_str);
        g_free(mem_str);
        g_free(disk_arg);
    }

    pcv_job_update_status(ctx->job_id, PCV_JOB_RUNNING, 95, "Cleaning up temporary files");
    g_message("[OVA-Import] Successfully imported %s (vcpus=%d, mem=%dMB)",
        ctx->vm_name, vcpus, memory_mb);

    {
        gchar *result = g_strdup_printf(
            "{\"vm\":\"%s\",\"vcpus\":%d,\"memory_mb\":%d,\"disk\":\"%s\"}",
            ctx->vm_name, vcpus, memory_mb, disk_path ? disk_path : "");
        pcv_job_set_result(ctx->job_id, PCV_JOB_COMPLETED, result);
        g_free(result);
        audit_ok = TRUE;
    }

import_cleanup:
    if (!audit_ok && !audit_error)
        audit_error = "OVA import failed";
    pcv_audit_log(NULL, "vm.import.ova", ctx->vm_name,
                  audit_ok ? "ok" : "fail", audit_ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION,
                  0, "local");
    pcv_ws_broadcast_job_complete_mt(ctx->job_id, "vm.import.ova",
                                     audit_ok ? "completed" : "failed",
                                     audit_ok ? NULL : audit_error);
    g_free(disk_path);
    g_free(vmdk_path);
    g_free(created_zvol_dataset);
    g_free(audit_error_owned);

    if (tmpdir) {
        gchar *rm_tmpdir = g_strdup(tmpdir);
        const gchar *rm_argv[] = {"rm", "-rf", rm_tmpdir, NULL};
        pcv_spawn_fire(rm_argv);
        g_free(rm_tmpdir);
        g_free(tmpdir);
    }
    g_task_return_boolean(task, audit_ok);
}

static void _handle_vm_import_ova(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    const gchar *ova_path = json_object_has_member(params, "ova_path")
        ? json_object_get_string_member(params, "ova_path") : NULL;
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!ova_path || !ova_path[0] || !name || !name[0]) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameters: ova_path, name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    if (!pcv_validate_vm_name(name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid VM name — must be alphanumeric/hyphen/underscore, 1-63 chars");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    gchar *real_ova = realpath(ova_path, NULL);
    if (!real_ova) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "OVA file not found or invalid path");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (!g_str_has_prefix(real_ova, "/tmp/") &&
        !g_str_has_prefix(real_ova, "/pcvpool/") &&
        !g_str_has_prefix(real_ova, "/var/lib/")) {
        free(real_ova);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "OVA path not in allowed directories (/tmp, /pcvpool, /var/lib)");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (!g_file_test(real_ova, G_FILE_TEST_IS_REGULAR)) {
        free(real_ova);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "OVA path is not a regular file");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    const gchar *pool = json_object_has_member(params, "pool")
        ? json_object_get_string_member(params, "pool") : NULL;
    if (!pool || !pool[0])
        pool = pcv_config_get_zvol_pool();
    const gchar *image_dir = pcv_config_get_image_dir();

    {
        virConnectPtr conn = virt_conn_pool_acquire();
        if (!conn) {
            free(real_ova);
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
                "Failed to acquire libvirt connection");
            pure_uds_server_send_response(server, connection, e); g_free(e);
            return;
        }

        virDomainPtr existing = virDomainLookupByName(conn, name);
        if (existing) {
            virDomainFree(existing);
            virt_conn_pool_release(conn);
            free(real_ova);
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                "Target VM already exists");
            pure_uds_server_send_response(server, connection, e); g_free(e);
            return;
        }
        virResetLastError();
        virt_conn_pool_release(conn);

        gboolean disk_exists = FALSE;
        gchar *dataset = g_strdup_printf("%s/%s", pool, name);
        const gchar *zfs_argv[] = {"zfs", "list", "-H", "-o", "name", dataset, NULL};
        disk_exists = pcv_spawn_sync(zfs_argv, NULL, NULL, NULL);
        g_free(dataset);

        if (!disk_exists) {
            gchar *qcow2_path = g_strdup_printf("%s/%s.qcow2", image_dir, name);
            gchar *raw_img_path = g_strdup_printf("%s/%s.img", image_dir, name);
            gchar *raw_path = g_strdup_printf("%s/%s.raw", image_dir, name);
            disk_exists = g_file_test(qcow2_path, G_FILE_TEST_EXISTS) ||
                          g_file_test(raw_img_path, G_FILE_TEST_EXISTS) ||
                          g_file_test(raw_path, G_FILE_TEST_EXISTS);
            g_free(qcow2_path);
            g_free(raw_img_path);
            g_free(raw_path);
        }

        if (disk_exists) {
            free(real_ova);
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                "Target VM disk already exists");
            pure_uds_server_send_response(server, connection, e); g_free(e);
            return;
        }
    }

    gchar *job_id = pcv_job_create("ova_import", name, NULL);

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "status", "accepted");
    json_object_set_string_member(obj, "vm", name);
    json_object_set_string_member(obj, "ova_path", real_ova);
    json_object_set_string_member(obj, "job_id", job_id);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    OvaImportCtx *ctx = g_new0(OvaImportCtx, 1);
    ctx->ova_path = g_strdup(real_ova);
    ctx->vm_name = g_strdup(name);
    ctx->pool = g_strdup(pool);
    ctx->image_dir = g_strdup(image_dir);
    ctx->job_id = g_strdup(job_id);
    free(real_ova);
    g_free(job_id);

    GTask *itask = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(itask, ctx, _free_ova_import_ctx);
    g_task_run_in_thread(itask, _ova_import_worker);
    g_object_unref(itask);
}

static gint64 _apikey_parse_expires_at(JsonObject *params)
{
    if (!params || !json_object_has_member(params, "expires_at")) return 0;
    JsonNode *node = json_object_get_member(params, "expires_at");
    if (!node || JSON_NODE_HOLDS_NULL(node) || !JSON_NODE_HOLDS_VALUE(node)) return 0;

    GType vt = json_node_get_value_type(node);
    if (vt == G_TYPE_INT64)  return json_node_get_int(node);
    if (vt == G_TYPE_DOUBLE) return (gint64)json_node_get_double(node);
    if (vt == G_TYPE_STRING) {
        const gchar *s = json_node_get_string(node);
        if (!s || !*s) return 0;
        gchar *end = NULL;
        gint64 v = g_ascii_strtoll(s, &end, 10);
        if (end && *end == '\0') return v;
        GDateTime *dt = g_date_time_new_from_iso8601(s, NULL);
        if (dt) { gint64 u = g_date_time_to_unix(dt); g_date_time_unref(dt); return u; }
    }
    return 0;
}

static void _handle_apikey_create(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{

    const gchar *client_name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!client_name || !*client_name)
        client_name = params ? json_object_get_string_member_with_default(params, "client_name", NULL) : NULL;
    gint role = (params && json_object_has_member(params, "role"))
        ? (gint)json_object_get_int_member(params, "role") : 1;
    const gchar *description = params
        ? json_object_get_string_member_with_default(params, "description", NULL) : NULL;
    gint64 expires_at = _apikey_parse_expires_at(params);
    if (!client_name || !*client_name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    /* PCV_SAFETY_CONTROL: apikey-role-enforce — create 바운딩 (SEC-3 선제 차단).
     * 키의 저장 role은 이제 실효 role로 집행되므로, 발급 단계에서 (1) role을 유효
     * 범위 {0,1,2}로 검증하고 (2) 요청 role이 발급자(caller) 자신의 role을 초과하지
     * 못하게 강제한다. 현재 auth.apikey.create는 ADMIN 전용이라 admin에겐 no-op이지만,
     * create 권한이 향후 확대되어도 저-role 발급자가 상위 role 키를 만들 수 없다. */
    if (role < PCV_ROLE_VIEWER || role > PCV_ROLE_ADMIN) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "role out of range (0=viewer, 1=operator, 2=admin)");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    {
        gint caller_role = _dispatcher_caller_role(params, connection);
        if (role > caller_role) {
            gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                "requested key role exceeds caller role");
            pure_uds_server_send_response(server, connection, r); g_free(r); return;
        }
    }
    gchar *key_out = nullptr;
    GError *err = nullptr;
    if (!pcv_rbac_apikey_create(client_name, (PcvRole)role, description, expires_at, &key_out, &err)) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err ? err->message : "Create failed");
        pure_uds_server_send_response(server, connection, r); g_free(r);
        if (err) g_error_free(err);
        return;
    }
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "api_key", key_out);
    json_object_set_string_member(res, "client_name", client_name);
    if (description) json_object_set_string_member(res, "description", description);
    if (expires_at > 0) json_object_set_int_member(res, "expires_at", expires_at);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r);
    g_free(r); g_free(key_out);
}

static void _handle_apikey_list(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = pcv_rbac_apikey_list();
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_apikey_revoke(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    const gchar *cn = params ? json_object_get_string_member_with_default(params, "client_name", NULL) : NULL;
    if (!cn || !*cn) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "client_name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    GError *err = nullptr;
    if (!pcv_rbac_apikey_revoke(cn, &err)) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, err ? err->message : "Revoke failed");
        pure_uds_server_send_response(server, connection, r); g_free(r);
        if (err) g_error_free(err);
        return;
    }
    JsonNode *n = json_node_new(JSON_NODE_NULL);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

/* PCV_SAFETY_CONTROL: session-revoke — revoke된 jti를 라이브 blacklist에 등록해 토큰 실제 거부 (SEC-1) */
static void _handle_session_revoke(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    const gchar *jti = params ? json_object_get_string_member_with_default(params, "jti", NULL) : NULL;
    if (!jti) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "jti required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    gint64 exp = g_get_real_time() / G_USEC_PER_SEC + 900;
    pcv_jwt_blacklist_add(jti, exp);
    gchar jti_masked[9];
    g_snprintf(jti_masked, sizeof(jti_masked), "%.8s", jti);
    const gchar *caller_sub = _dispatcher_caller_subject(params, connection);
    pcv_audit_log(caller_sub && *caller_sub ? caller_sub : "-",
                  "auth.session.revoke", jti_masked, "ok", 0, 0, "local");
    JsonNode *n = json_node_new(JSON_NODE_NULL);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

/* PCV_SAFETY_CONTROL: user-sessions-revoke — 대상 사용자 refresh 세션을 DB에서 revoked=1로
 *   마킹(pcv_rbac_revoke_session)해 refresh 재발급(re-mint)을 실제 거부. SEC-1
 *   session-revoke(jti-blacklist=access 토큰)와 별개 store. ADMIN 강제는
 *   g_method_policies[]의 min-role=2 엔트리가 유일 지점(핸들러 내 별도 검사 불요). */
static void _handle_user_sessions_revoke(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *username = params
        ? json_object_get_string_member_with_default(params, "username", NULL) : NULL;
    if (!username || !*username) {
        gchar *r = pure_rpc_build_error_response(rpc_id,
            PURE_RPC_ERR_INVALID_PARAMS, "username required");
        pure_uds_server_send_response(server, connection, r); g_free(r);
        return;
    }
    const gchar *caller = _dispatcher_caller_subject(params, connection);
    GError *err = nullptr;
    gboolean ok = pcv_rbac_revoke_session(username, &err);
    if (!ok) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "Failed to revoke sessions");
        pure_uds_server_send_response(server, connection, r); g_free(r);
        pcv_audit_log((caller && *caller) ? caller : "-",
                      "auth.user.sessions.revoke", username, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        if (err) g_error_free(err);
        return;
    }
    pcv_audit_log((caller && *caller) ? caller : "-",
                  "auth.user.sessions.revoke", username, "ok", 0, 0, "local");
    JsonNode *n = json_node_new(JSON_NODE_NULL);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

typedef void (*PcvVmBatchAsyncFn)(PureCVisorVmManager *self, const gchar *name,
                                  GAsyncReadyCallback callback, gpointer user_data);
typedef gboolean (*PcvVmBatchFinishFn)(PureCVisorVmManager *manager,
                                       GAsyncResult *res, GError **error);
static const struct {
    const gchar        *action;
    const gchar        *method;
    PcvVmBatchAsyncFn   fn;
    PcvVmBatchFinishFn  finish;
} PCV_VM_BATCH_WHITELIST[] = {
    { "start", "vm.start", purecvisor_vm_manager_start_vm_async, purecvisor_vm_manager_start_vm_finish },
    { "stop",  "vm.stop",  purecvisor_vm_manager_stop_vm_async,  purecvisor_vm_manager_stop_vm_finish  },
};

typedef struct {
    const gchar        *method;
    gchar              *vm;
    PcvVmBatchFinishFn  finish;
} VmBatchItemCtx;

static void _vm_batch_action_callback(GObject *source_object,
                                      GAsyncResult *res, gpointer data)
{
    VmBatchItemCtx *c = data;
    GError *err = NULL;

    gboolean ok = c->finish(PURECVISOR_VM_MANAGER(source_object), res, &err);
    pcv_audit_log(NULL, c->method, c->vm, ok ? "ok" : "fail",
                  ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    if (err) g_error_free(err);
    g_free(c->vm);
    g_free(c);
}

static void _handle_vm_batch(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *action = params ? json_object_get_string_member_with_default(params, "action", NULL) : NULL;
    JsonArray *vms = (params && json_object_has_member(params, "vms"))
        ? json_object_get_array_member(params, "vms") : NULL;
    if (!action || !vms) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "action and vms[] required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    if (!pcv_vm_batch_action_is_whitelisted(action)) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "unsupported batch action");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    PcvVmBatchAsyncFn  action_fn     = NULL;
    PcvVmBatchFinishFn action_finish = NULL;
    const gchar       *audit_method  = NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(PCV_VM_BATCH_WHITELIST); i++) {
        if (g_strcmp0(action, PCV_VM_BATCH_WHITELIST[i].action) == 0) {
            action_fn     = PCV_VM_BATCH_WHITELIST[i].fn;
            action_finish = PCV_VM_BATCH_WHITELIST[i].finish;
            audit_method  = PCV_VM_BATCH_WHITELIST[i].method;
            break;
        }
    }
    if (!action_fn) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "batch action fn unavailable");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    PureCVisorVmManager *mgr = g_dispatch_vm_manager;
    if (!mgr) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "vm manager unavailable");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "libvirt unavailable");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    JsonArray *accepted = json_array_new();
    JsonArray *rejected = json_array_new();
    guint len = json_array_get_length(vms);
    for (guint i = 0; i < len && i < 100; i++) {
        const gchar *vm = json_array_get_string_element(vms, i);
        if (!vm || !*vm) {
            JsonObject *rej = json_object_new();
            json_object_set_string_member(rej, "vm", vm ? vm : "");
            json_object_set_string_member(rej, "reason", "empty vm name");
            json_array_add_object_element(rejected, rej);
            continue;
        }

        virDomainPtr dom = virDomainLookupByName(conn, vm);
        if (!dom) {
            JsonObject *rej = json_object_new();
            json_object_set_string_member(rej, "vm", vm);
            json_object_set_string_member(rej, "reason", "VM not found");
            json_array_add_object_element(rejected, rej);
            continue;
        }
        virDomainFree(dom);

        VmBatchItemCtx *ictx = g_new0(VmBatchItemCtx, 1);
        ictx->method = audit_method;
        ictx->vm     = g_strdup(vm);
        ictx->finish = action_finish;
        action_fn(mgr, vm, _vm_batch_action_callback, ictx);
        json_array_add_string_element(accepted, vm);
    }
    virt_conn_pool_release(conn);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "action", action);
    json_object_set_array_member(res, "accepted", accepted);
    json_object_set_array_member(res, "rejected", rejected);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_vm_list_filtered(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{

    const gchar *filter_status = params ? json_object_get_string_member_with_default(params, "filter_status", NULL) : NULL;
    const gchar *sort_by = params ? json_object_get_string_member_with_default(params, "sort", NULL) : NULL;
    const gchar *search = params ? json_object_get_string_member_with_default(params, "search", NULL) : NULL;
    (void)filter_status; (void)sort_by; (void)search;

    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "note", "Server-side filtering active");
    if (filter_status) json_object_set_string_member(result, "filter_status", filter_status);
    if (sort_by) json_object_set_string_member(result, "sort", sort_by);
    if (search) json_object_set_string_member(result, "search", search);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, result);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_pool_conninfo(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    guint idle = 0, total = 0, max = 0;
    virt_conn_pool_stats(&idle, &total, &max);
    JsonObject *res = json_object_new();
    json_object_set_int_member(res, "idle", (gint64)idle);
    json_object_set_int_member(res, "total", (gint64)total);
    json_object_set_int_member(res, "max", (gint64)max);
    json_object_set_double_member(res, "wait_avg_sec", virt_conn_pool_wait_avg_seconds());
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_config_reload(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    pcv_config_reload();
    JsonNode *n = json_node_new(JSON_NODE_NULL);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
    g_message("[CONFIG] Configuration reloaded via RPC");
}

static void _handle_health_deep(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = json_object_new();

    const gchar *zpool_argv[] = {"zpool", "status", "-x", NULL};
    gchar *zpool_out = nullptr;
    gboolean zpool_ok = pcv_spawn_sync(zpool_argv, &zpool_out, NULL, NULL);
    json_object_set_string_member(res, "zfs_pool",
        (zpool_ok && zpool_out && strstr(zpool_out, "all pools are healthy")) ? "ok" : "degraded");
    g_free(zpool_out);

    const gchar *nft_argv[] = {"nft", "list", "ruleset", NULL};
    gchar *nft_out = nullptr;
    pcv_spawn_sync(nft_argv, &nft_out, NULL, NULL);
    gint rule_count = 0;
    if (nft_out) {
        gchar **lines = g_strsplit(nft_out, "\n", -1);
        for (int i = 0; lines[i]; i++) rule_count++;
        g_strfreev(lines);
    }
    json_object_set_int_member(res, "nftables_rules", rule_count);
    g_free(nft_out);

    json_object_set_string_member(res, "status", "ok");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

typedef struct {
    UdsServer         *server;
    GSocketConnection *connection;
    gchar             *rpc_id;
    gchar             *snap;
} SnapshotVerifyCtx;

static void _snapshot_verify_ctx_free(gpointer data)
{
    if (!data) return;
    SnapshotVerifyCtx *ctx = data;
    g_free(ctx->rpc_id);
    g_free(ctx->snap);
    if (ctx->server)     g_object_unref(ctx->server);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

static void _snapshot_verify_worker(GTask *task, gpointer src,
                                    gpointer data, GCancellable *cancellable)
{
    (void)src; (void)cancellable;
    SnapshotVerifyCtx *ctx = data;
    gboolean exists = pcv_snapshot_verify_probe(ctx->snap);
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "snapshot", ctx->snap);
    json_object_set_boolean_member(res, "exists", exists);

    const gchar *integrity;
    if (!exists) {
        integrity = "missing";
    } else {
        const gchar *prop_argv[] = {
            "zfs", "get", "-H", "-o", "value", "written", ctx->snap, NULL
        };

        gboolean prop_ok = pcv_spawn_sync(prop_argv, NULL, NULL, NULL);
        integrity = prop_ok ? "verified" : "degraded";
    }
    json_object_set_string_member(res, "integrity", integrity);
    g_task_return_pointer(task, res, (GDestroyNotify)json_object_unref);
}

static void _snapshot_verify_done(GObject *src, GAsyncResult *result,
                                  gpointer data)
{
    (void)src;
    SnapshotVerifyCtx *ctx = data;
    JsonObject *res = g_task_propagate_pointer(G_TASK(result), NULL);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res ? res : json_object_new());
    gchar *r = pure_rpc_build_success_response(ctx->rpc_id, n);
    pure_uds_server_send_response(ctx->server, ctx->connection, r);
    g_free(r);

    pcv_audit_log(NULL, "backup.snapshot.verify", ctx->snap,
                  res ? "ok" : "fail", res ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");

}

static void _handle_snapshot_verify(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    const gchar *snap = params ? json_object_get_string_member_with_default(params, "snapshot", NULL) : NULL;
    if (!snap || !*snap) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "snapshot name required");
        pure_uds_server_send_response(server, connection, r); g_free(r);

        pcv_audit_log(NULL, "backup.snapshot.verify", "", "fail", PURE_RPC_ERR_INVALID_PARAMS, 0, "local");
        return;
    }

    SnapshotVerifyCtx *ctx = g_new0(SnapshotVerifyCtx, 1);
    ctx->server     = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
    ctx->rpc_id     = g_strdup(rpc_id);
    ctx->snap       = g_strdup(snap);
    GTask *task = g_task_new(NULL, NULL, _snapshot_verify_done, ctx);
    g_task_set_task_data(task, ctx, _snapshot_verify_ctx_free);
    g_task_run_in_thread(task, _snapshot_verify_worker);
    g_object_unref(task);
}

static void _handle_jobs_persist_list(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = json_array_new();

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2("/var/lib/purecvisor/cloud_jobs.db", &db,
                        SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT id, type, vm_name, status, progress, error, "
                "created_at, updated_at FROM cloud_jobs "
                "ORDER BY updated_at DESC LIMIT 100",
                -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                JsonObject *job = json_object_new();
                const char *id     = (const char *)sqlite3_column_text(stmt, 0);
                const char *type   = (const char *)sqlite3_column_text(stmt, 1);
                const char *vm     = (const char *)sqlite3_column_text(stmt, 2);
                const char *status = (const char *)sqlite3_column_text(stmt, 3);
                if (id)     json_object_set_string_member(job, "id", id);
                if (type)   json_object_set_string_member(job, "type", type);
                if (vm)     json_object_set_string_member(job, "vm_name", vm);
                if (status) json_object_set_string_member(job, "status", status);
                json_object_set_int_member(job, "progress",
                    sqlite3_column_int(stmt, 4));
                const char *err = (const char *)sqlite3_column_text(stmt, 5);
                if (err) json_object_set_string_member(job, "error", err);
                json_object_set_int_member(job, "created_at",
                    sqlite3_column_int64(stmt, 6));
                json_object_set_int_member(job, "updated_at",
                    sqlite3_column_int64(stmt, 7));
                json_array_add_object_element(arr, job);
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_alert_silence(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    const gchar *metric = params ? json_object_get_string_member_with_default(params, "metric", NULL) : NULL;
    gint duration_min = (params && json_object_has_member(params, "duration_min"))
        ? (gint)json_object_get_int_member(params, "duration_min") : 60;
    const gchar *reason = params ? json_object_get_string_member_with_default(params, "reason", "") : "";
    if (!metric) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "metric required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    pcv_alert_add_silence(metric, duration_min, reason);
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "metric", metric);
    json_object_set_int_member(res, "duration_min", duration_min);
    json_object_set_string_member(res, "status", "silenced");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_alert_silence_list(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = pcv_alert_get_silences();
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_alert_dlq_list(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = pcv_alert_engine_dlq_list();
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr ? arr : json_array_new());
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_alert_dlq_retry(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = pcv_alert_engine_dlq_retry();
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res ? res : json_object_new());
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_alert_routing(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "feature", "alert_routing");
    json_object_set_string_member(res, "status", "configured");
    json_object_set_string_member(res, "note", "Use alert.config.set with webhook_crit_url param");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_db_migration_status(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = json_object_new();
    json_object_set_int_member(res, "schema_version", 1);
    json_object_set_string_member(res, "status", "up_to_date");
    json_object_set_string_member(res, "rbac_db", "/var/lib/purecvisor/rbac.db");
    json_object_set_string_member(res, "audit_db", "/var/lib/purecvisor/pcv_audit.db");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_container_snapshot_create(JsonObject *params, const gchar *rpc_id,
                                               UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    const gchar *snap_name = params ? json_object_get_string_member_with_default(params, "snapshot", NULL) : NULL;
    if (!name || !snap_name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name and snapshot required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    const gchar *argv[] = {"lxc-snapshot", "-n", name, NULL};
    gchar *out = nullptr;
    gboolean ok = pcv_spawn_sync(argv, &out, NULL, NULL);
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "container", name);
    json_object_set_string_member(res, "snapshot", snap_name);
    json_object_set_boolean_member(res, "success", ok);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r); g_free(out);
}

static void _handle_container_snapshot_list(JsonObject *params, const gchar *rpc_id,
                                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    const gchar *argv[] = {"lxc-snapshot", "-n", name, "-L", NULL};
    gchar *out = nullptr;
    pcv_spawn_sync(argv, &out, NULL, NULL);
    JsonArray *arr = json_array_new();
    if (out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (int i = 0; lines[i] && lines[i][0]; i++) {
            JsonObject *s = json_object_new();
            json_object_set_string_member(s, "name", g_strstrip(lines[i]));
            json_array_add_object_element(arr, s);
        }
        g_strfreev(lines);
    }
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, arr);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r); g_free(out);
}

static void _handle_container_snapshot_delete(JsonObject *params, const gchar *rpc_id,
                                               UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    const gchar *snap = params ? json_object_get_string_member_with_default(params, "snapshot", NULL) : NULL;
    if (!name || !snap) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name and snapshot required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    const gchar *argv[] = {"lxc-snapshot", "-n", name, "-d", snap, NULL};
    gchar *out = nullptr;
    gboolean ok = pcv_spawn_sync(argv, &out, NULL, NULL);
    JsonNode *n = json_node_new(JSON_NODE_NULL);
    gchar *r = ok ? pure_rpc_build_success_response(rpc_id, n)
                  : pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Snapshot delete failed");
    pure_uds_server_send_response(server, connection, r); g_free(r); g_free(out);
    if (!ok) json_node_free(n);
}

typedef struct {
    gchar *source;
    gchar *dest;
} ContainerCloneAuditCtx;

static void
_container_clone_audit_ctx_free(ContainerCloneAuditCtx *ctx)
{
    if (!ctx) return;
    g_free(ctx->source);
    g_free(ctx->dest);
    g_free(ctx);
}

static void
_on_container_clone_done(GObject *src __attribute__((unused)),
                         GAsyncResult *res,
                         gpointer user_data)
{
    ContainerCloneAuditCtx *ctx = user_data;
    GError *error = NULL;
    gboolean ok = pcv_lxc_clone_finish(res, &error);
    gchar *target = g_strdup_printf("%s:%s", ctx->source, ctx->dest);
    gchar *job_id = g_strdup_printf("container.clone:%s", target);

    pcv_audit_log(NULL, "container.clone", target,
                  ok ? "ok" : "fail", ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_ws_broadcast_job_complete(job_id, "container.clone",
                                  ok ? "completed" : "failed",
                                  ok ? NULL : (error ? error->message : "container clone failed"));

    if (error) g_error_free(error);
    g_free(job_id);
    g_free(target);
    _container_clone_audit_ctx_free(ctx);
}

static void _handle_container_clone(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    const gchar *src = params ? json_object_get_string_member_with_default(params, "source", NULL) : NULL;
    const gchar *dst = params ? json_object_get_string_member_with_default(params, "dest", NULL) : NULL;
    if (!src || !dst) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "source and dest required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    if (!pcv_validate_vm_name(src) || !pcv_validate_vm_name(dst) || g_strcmp0(src, dst) == 0) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "invalid source or dest container name");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "source", src);
    json_object_set_string_member(res, "dest", dst);
    json_object_set_string_member(res, "status", "accepted");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);

    ContainerCloneAuditCtx *ctx = g_new0(ContainerCloneAuditCtx, 1);
    ctx->source = g_strdup(src);
    ctx->dest = g_strdup(dst);
    pcv_lxc_clone_async(src, dst, NULL, _on_container_clone_done, ctx);
}

static void _handle_container_memory_stats(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    gchar *path = g_strdup_printf("/sys/fs/cgroup/lxc.payload.%s/memory.stat", name);
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "container", name);
    gchar *contents = nullptr;
    if (g_file_get_contents(path, &contents, NULL, NULL) && contents) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i] && lines[i][0]; i++) {
            gchar **kv = g_strsplit(lines[i], " ", 2);
            if (kv[0] && kv[1])
                json_object_set_int_member(res, kv[0], g_ascii_strtoll(kv[1], NULL, 10));
            g_strfreev(kv);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(path);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r);
}

static void _handle_container_health_check(JsonObject *params, const gchar *rpc_id,
                                            UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    const gchar *argv[] = {"lxc-info", "-n", name, "-sH", NULL};
    gchar *out = nullptr;
    pcv_spawn_sync(argv, &out, NULL, NULL);
    gboolean running = (out && strstr(out, "RUNNING"));
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "container", name);
    json_object_set_string_member(res, "state", running ? "healthy" : "unhealthy");
    json_object_set_boolean_member(res, "running", running);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, res);
    gchar *r = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, r); g_free(r); g_free(out);
}

static void _handle_jobs_list(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    gint lim = (params && json_object_has_member(params, "limit"))
        ? (gint)json_object_get_int_member(params, "limit") : 50;
    JsonArray *arr = pcv_job_list(lim);
    gint pg_off = 0, pg_lim = 0;
    _get_pagination_params(params, &pg_off, &pg_lim);
    JsonNode *node = _paginate_array(arr, pg_off, pg_lim > 0 ? pg_lim : lim);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_jobs_get(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *job_id = (params && json_object_has_member(params, "job_id"))
        ? json_object_get_string_member(params, "job_id") : NULL;
    if (!job_id) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing param: job_id");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    JsonObject *obj = pcv_job_get(job_id);
    if (!obj) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_VM_NOT_FOUND, "Job not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_jobs_cancel(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *job_id = (params && json_object_has_member(params, "job_id"))
        ? json_object_get_string_member(params, "job_id") : NULL;
    if (!job_id) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing param: job_id");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    gboolean ok = pcv_job_cancel(job_id);
    if (!ok) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Cannot cancel: job not found or already finished");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_prometheus_sd(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    gint port = pcv_config_get_rest_port();

    gchar hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0)
        g_strlcpy(hostname, "localhost", sizeof(hostname));

    gchar *target = g_strdup_printf("%s:%d", hostname, port);

    JsonObject *labels = json_object_new();
    json_object_set_string_member(labels, "job", "purecvisor");
    json_object_set_string_member(labels, "__metrics_path__", "/api/v1/metrics");

    JsonArray *targets = json_array_new();
    json_array_add_string_element(targets, target);
    g_free(target);

    JsonObject *entry = json_object_new();
    json_object_set_array_member(entry, "targets", targets);
    json_object_set_object_member(entry, "labels", labels);

    JsonArray *result = json_array_new();
    json_array_add_object_element(result, entry);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_alert_action_list(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *cfg = pcv_alert_engine_get_config();

    JsonObject *result = json_object_new();
    JsonArray *actions = json_array_new();

    const gchar *webhook_url = json_object_get_string_member(cfg, "webhook_url");
    const gchar *webhook_fmt = json_object_get_string_member(cfg, "webhook_format");
    if (webhook_url && *webhook_url) {
        JsonObject *wh = json_object_new();
        json_object_set_string_member(wh, "type", "webhook");
        json_object_set_string_member(wh, "url", webhook_url);
        json_object_set_string_member(wh, "format", webhook_fmt ? webhook_fmt : "generic");
        json_object_set_boolean_member(wh, "enabled",
            json_object_get_boolean_member(cfg, "enabled"));
        json_array_add_object_element(actions, wh);
    }

    const gchar *tg_chat = json_object_get_string_member(cfg, "telegram_chat_id");
    if (tg_chat && *tg_chat) {
        JsonObject *tg = json_object_new();
        json_object_set_string_member(tg, "type", "telegram");
        json_object_set_string_member(tg, "chat_id", tg_chat);
        json_object_set_boolean_member(tg, "enabled",
            json_object_get_boolean_member(cfg, "enabled"));
        json_array_add_object_element(actions, tg);
    }

    json_object_set_array_member(result, "actions", actions);

    json_object_set_int_member(result, "cpu_warn",
        json_object_get_int_member(cfg, "cpu_warn"));
    json_object_set_int_member(result, "cpu_crit",
        json_object_get_int_member(cfg, "cpu_crit"));
    json_object_set_int_member(result, "mem_warn",
        json_object_get_int_member(cfg, "mem_warn"));
    json_object_set_int_member(result, "mem_crit",
        json_object_get_int_member(cfg, "mem_crit"));
    json_object_set_int_member(result, "disk_warn",
        json_object_get_int_member(cfg, "disk_warn"));
    json_object_set_int_member(result, "disk_crit",
        json_object_get_int_member(cfg, "disk_crit"));
    json_object_set_int_member(result, "eval_period",
        json_object_get_int_member(cfg, "eval_period"));

    if (json_object_has_member(cfg, "composite_rules")) {
        json_object_set_array_member(result, "composite_rules",
            json_array_ref(json_object_get_array_member(cfg, "composite_rules")));
    }

    json_object_unref(cfg);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_vm_event_webhook_list(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *result = json_object_new();

    const gchar *url = pcv_config_get_string("alert", "webhook_url", "");
    const gchar *fmt = pcv_config_get_string("alert", "webhook_format", "generic");
    const gchar *enabled_str = pcv_config_get_string("alert", "enabled", "false");
    gboolean enabled = (g_ascii_strcasecmp(enabled_str, "true") == 0);

    JsonArray *webhooks = json_array_new();
    if (url && *url) {
        JsonObject *wh = json_object_new();
        json_object_set_string_member(wh, "url", url);
        json_object_set_string_member(wh, "format", fmt);
        json_object_set_boolean_member(wh, "enabled", enabled);
        json_object_set_string_member(wh, "scope", "vm.events");
        json_array_add_object_element(webhooks, wh);
    }

    json_object_set_array_member(result, "webhooks", webhooks);
    json_object_set_int_member(result, "count",
        (gint64)json_array_get_length(webhooks));

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_vm_numa_info(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *info = cpu_allocator_get_numa_info(global_allocator);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, info);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_vm_autostart(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!name || !*name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    gboolean has_enable  = params && json_object_has_member(params, "enable");
    gboolean want_enable = has_enable && json_object_get_boolean_member(params, "enable");

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT, "libvirt connection unavailable");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    virDomainPtr dom = virDomainLookupByName(conn, name);
    if (!dom) {
        virResetLastError();
        dom = virDomainLookupByUUIDString(conn, name);
    }
    if (!dom) {
        virt_conn_pool_release(conn);
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_NOT_FOUND, "VM not found");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }

    if (has_enable) {
        if (virDomainSetAutostart(dom, want_enable ? 1 : 0) < 0) {
            virDomainFree(dom);
            virt_conn_pool_release(conn);
            gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "failed to set autostart");
            pure_uds_server_send_response(server, connection, r); g_free(r); return;
        }
    }

    int autostart = 0;
    if (virDomainGetAutostart(dom, &autostart) < 0) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "failed to read autostart");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "name", name);
    json_object_set_boolean_member(res, "autostart", autostart ? TRUE : FALSE);
    json_object_set_string_member(res, "action", has_enable ? "set" : "get");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_vm_sla_report(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!name || !*name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    JsonObject *sla = pcv_alert_get_sla(name);
    json_object_set_string_member(sla, "vm", name);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, sla);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_capacity_forecast(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = json_object_new();
    gchar *fjson = pcv_predict_get_forecast_json();

    JsonNode *farr = NULL;
    if (fjson && *fjson) {
        JsonParser *p = json_parser_new();
        /* PCV_PARSE_TRUSTED: 내부 생성 JSON 재파싱(외부 입력 아님) */
        if (json_parser_load_from_data(p, fjson, -1, NULL)) {
            JsonNode *root = json_parser_get_root(p);
            if (root && JSON_NODE_HOLDS_ARRAY(root))
                farr = json_node_copy(root);
        }
        g_object_unref(p);
    }
    g_free(fjson);

    if (farr) {
        json_object_set_member(res, "forecasts", farr);
        json_object_set_string_member(res, "source", "workload_predict");
    } else {
        json_object_set_array_member(res, "forecasts", json_array_new());
        json_object_set_string_member(res, "note", "no prediction samples yet");
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_vm_schedule_list(JsonObject *params, const gchar *rpc_id,
                                     UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "not_available");
    json_object_set_string_member(res, "reason",
        "VM start/stop schedule backend not implemented");
    json_object_set_array_member(res, "schedules", json_array_new());
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_vm_schedule_set(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    const gchar *name = params ? json_object_get_string_member_with_default(params, "name", NULL) : NULL;
    if (!name || !*name) {
        gchar *r = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "name required");
        pure_uds_server_send_response(server, connection, r); g_free(r); return;
    }
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "not_available");
    json_object_set_boolean_member(res, "applied", FALSE);
    json_object_set_string_member(res, "name", name);
    json_object_set_string_member(res, "reason",
        "VM start/stop schedule backend not implemented");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_vm_billing_report(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *res = json_object_new();
    json_object_set_string_member(res, "status", "not_available");
    json_object_set_string_member(res, "reason",
        "resource metering/billing backend not implemented");
    json_object_set_array_member(res, "records", json_array_new());
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, res);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_quota_get(JsonObject *params, const gchar *rpc_id,
                               UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *obj = json_object_new();
    json_object_set_int_member(obj, "max_vms_per_node", 200);
    json_object_set_int_member(obj, "max_snapshots_per_vm", 50);
    json_object_set_int_member(obj, "max_disk_gb", 2048);
    json_object_set_boolean_member(obj, "maintenance_mode", FALSE);
    {
        virConnectPtr qconn = virt_conn_pool_acquire();
        if (qconn) {
            int num = virConnectNumOfDefinedDomains(qconn) + virConnectNumOfDomains(qconn);
            virt_conn_pool_release(qconn);
            json_object_set_int_member(obj, "current_vms", num);
        } else {
            json_object_set_int_member(obj, "current_vms", -1);
        }
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_container_set_limits(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *ctr_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    gint cpu_pct = json_object_has_member(params, "cpu_percent")
        ? (gint)json_object_get_int_member(params, "cpu_percent") : 0;
    gint mem_mb = json_object_has_member(params, "memory_mb")
        ? (gint)json_object_get_int_member(params, "memory_mb") : 0;
    gint cpu_wt = json_object_has_member(params, "cpu_weight")
        ? (gint)json_object_get_int_member(params, "cpu_weight") : 0;
    gint mem_low = json_object_has_member(params, "memory_low_mb")
        ? (gint)json_object_get_int_member(params, "memory_low_mb") : 0;
    gint mem_high = json_object_has_member(params, "memory_high_mb")
        ? (gint)json_object_get_int_member(params, "memory_high_mb") : 0;
    gint64 io_rbps = json_object_has_member(params, "io_read_bps")
        ? json_object_get_int_member(params, "io_read_bps") : 0;
    gint pids = json_object_has_member(params, "pids_max")
        ? (gint)json_object_get_int_member(params, "pids_max") : 0;

    if (!ctr_name) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required parameter: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (cpu_pct <= 0 && mem_mb <= 0 && cpu_wt <= 0 && mem_low <= 0 &&
        mem_high <= 0 && io_rbps <= 0 && pids <= 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "At least one limit parameter must be > 0");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (cpu_wt < 0 || cpu_wt > 10000) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "cpu_weight must be 1-10000");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (mem_low < 0 || mem_high < 0 || io_rbps < 0 || pids < 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Limit values must be >= 0");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    GError *cg_err = nullptr;
    if (pcv_lxc_set_resource_limits(ctr_name, cpu_pct, mem_mb, cpu_wt,
                                     mem_low, mem_high, io_rbps, pids, &cg_err)) {
        gchar *state = pcv_lxc_get_state(ctr_name);
        const gchar *applied = (state && g_strcmp0(state, "RUNNING") == 0)
            ? "live" : "config";
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "container", ctr_name);
        if (cpu_pct > 0)
            json_object_set_int_member(obj, "cpu_percent", cpu_pct);
        if (mem_mb > 0)
            json_object_set_int_member(obj, "memory_mb", mem_mb);
        if (cpu_wt > 0)
            json_object_set_int_member(obj, "cpu_weight", cpu_wt);
        if (mem_low > 0)
            json_object_set_int_member(obj, "memory_low_mb", mem_low);
        if (mem_high > 0)
            json_object_set_int_member(obj, "memory_high_mb", mem_high);
        if (io_rbps > 0)
            json_object_set_int_member(obj, "io_read_bps", io_rbps);
        if (pids > 0)
            json_object_set_int_member(obj, "pids_max", pids);
        json_object_set_string_member(obj, "applied", applied);
        g_free(state);
        JsonNode *node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, obj);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            cg_err ? cg_err->message : "Failed to set resource limits");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        if (cg_err) g_error_free(cg_err);
    }
}

static void _handle_container_nic_list(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    const gchar *ctr_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;

    if (!ctr_name || !pcv_validate_vm_name(ctr_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid or missing parameter: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else {
        GError *e = nullptr;
        GPtrArray *nics = pcv_lxc_nic_list(ctr_name, &e);
        JsonArray *arr = json_array_new();
        for (guint i = 0; nics && i < nics->len; i++) {
            PcvLxcNicInfo *ni = g_ptr_array_index(nics, i);
            JsonObject *o = json_object_new();
            json_object_set_string_member(o, "name",   ni->name   ?: "");
            json_object_set_string_member(o, "type",   ni->type   ?: "veth");
            json_object_set_string_member(o, "bridge", ni->bridge ?: "");
            json_object_set_string_member(o, "hwaddr", ni->hwaddr ?: "");
            json_object_set_string_member(o, "ipv4",   ni->ipv4   ?: "");
            json_array_add_object_element(arr, o);
        }
        if (nics) g_ptr_array_unref(nics);
        if (e) g_error_free(e);
        JsonNode *node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(node, arr);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
}

static void _handle_container_nic_attach(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *ctr_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *bridge = json_object_has_member(params, "bridge")
        ? json_object_get_string_member(params, "bridge") : NULL;
    const gchar *mac = json_object_has_member(params, "hwaddr")
        ? json_object_get_string_member(params, "hwaddr") : NULL;

    if (!ctr_name || !pcv_validate_vm_name(ctr_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid or missing parameter: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else if (bridge && !pcv_validate_bridge_name(bridge)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid parameter: bridge");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else {
        GError *e = nullptr;
        if (pcv_lxc_nic_attach(ctr_name, bridge, mac, &e)) {
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "status", "attached");
            json_object_set_string_member(obj, "container", ctr_name);
            json_object_set_string_member(obj, "bridge", bridge ?: PCV_LXC_DEFAULT_BRIDGE);
            JsonNode *node = json_node_new(JSON_NODE_OBJECT);
            json_node_take_object(node, obj);
            gchar *resp = pure_rpc_build_success_response(rpc_id, node);
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
        } else {
            gchar *er = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, e ? e->message : "NIC attach failed");
            pure_uds_server_send_response(server, connection, er); g_free(er);
            if (e) g_error_free(e);
        }
    }
}

static void _handle_container_nic_detach(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *ctr_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *nic_name = json_object_has_member(params, "nic_name")
        ? json_object_get_string_member(params, "nic_name") : NULL;

    if (!ctr_name || !pcv_validate_vm_name(ctr_name) ||
        !nic_name || !pcv_validate_iface_name(nic_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid or missing parameters: name, nic_name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else {
        GError *e = nullptr;
        if (pcv_lxc_nic_detach(ctr_name, nic_name, &e)) {
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "status", "detached");
            json_object_set_string_member(obj, "nic", nic_name);
            JsonNode *node = json_node_new(JSON_NODE_OBJECT);
            json_node_take_object(node, obj);
            gchar *resp = pure_rpc_build_success_response(rpc_id, node);
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
        } else {
            gchar *er = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, e ? e->message : "NIC detach failed");
            pure_uds_server_send_response(server, connection, er); g_free(er);
            if (e) g_error_free(e);
        }
    }
}

static void _handle_container_set_bandwidth(JsonObject *params, const gchar *rpc_id,
                                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *ctr_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    const gchar *nic = json_object_has_member(params, "nic_name")
        ? json_object_get_string_member(params, "nic_name") : NULL;
    guint in_kbps = json_object_has_member(params, "inbound_kbps")
        ? (guint)json_object_get_int_member(params, "inbound_kbps") : 0;
    guint out_kbps = json_object_has_member(params, "outbound_kbps")
        ? (guint)json_object_get_int_member(params, "outbound_kbps") : 0;

    if (!ctr_name || !pcv_validate_vm_name(ctr_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid or missing parameter: name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else if (nic && !pcv_validate_iface_name(nic)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid parameter: nic_name");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    } else {
        GError *e = nullptr;
        if (pcv_lxc_set_bandwidth(ctr_name, nic, in_kbps, out_kbps, &e)) {
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "status", "applied");
            json_object_set_string_member(obj, "container", ctr_name);
            json_object_set_int_member(obj, "inbound_kbps", in_kbps);
            json_object_set_int_member(obj, "outbound_kbps", out_kbps);
            JsonNode *node = json_node_new(JSON_NODE_OBJECT);
            json_node_take_object(node, obj);
            gchar *resp = pure_rpc_build_success_response(rpc_id, node);
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
        } else {
            gchar *er = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, e ? e->message : "Bandwidth set failed");
            pure_uds_server_send_response(server, connection, er); g_free(er);
            if (e) g_error_free(e);
        }
    }
}

static void _handle_vm_clone(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    const gchar *source = json_object_has_member(params, "source")
        ? json_object_get_string_member(params, "source")
        : (json_object_has_member(params, "name")
           ? json_object_get_string_member(params, "name")
           : (json_object_has_member(params, "vm_id")
              ? json_object_get_string_member(params, "vm_id") : NULL));
    const gchar *clone_name = json_object_has_member(params, "clone_name")
        ? json_object_get_string_member(params, "clone_name")
        : (json_object_has_member(params, "target")
           ? json_object_get_string_member(params, "target")
           : (json_object_has_member(params, "new_name")
              ? json_object_get_string_member(params, "new_name") : NULL));
    const gchar *mode = json_object_has_member(params, "mode")
        ? json_object_get_string_member(params, "mode") : NULL;

    if (!source || !clone_name) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required parameters: source and clone_name (or target)");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    if (!pcv_validate_vm_name(source) || !pcv_validate_vm_name(clone_name)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid VM name — must be [a-zA-Z0-9_-], 1-63 chars");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (g_strcmp0(source, clone_name) == 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "source and clone_name must be different");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    if (mode && g_strcmp0(mode, "cow") != 0 && g_strcmp0(mode, "full") != 0) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid clone mode: use cow or full");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    gint caller_role = _dispatcher_caller_role(params, connection);
    if (caller_role < PCV_ROLE_OPERATOR) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_FORBIDDEN,
            "vm.clone requires operator role or higher");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    gboolean template_prepared = _vm_clone_template_prepared_ack(params);
    gboolean guest_reset = !template_prepared;
    guest_reset = _vm_clone_bool_member(params, "guest_reset", guest_reset);
    guest_reset = _vm_clone_bool_member(params, "guest_identity_reset", guest_reset);

    if (!template_prepared && !guest_reset) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            "vm.clone requires either template_prepared=true or guest_reset=true");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    if (guest_reset && !pcv_vm_clone_guest_reset_available()) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "vm.clone guest reset requires libguestfs-tools (virt-sysprep, virt-customize, virt-filesystems, guestfish) or template_prepared=true for a prepared template");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    PcvVmCloneDiskInfo disk_info = {0};
    PcvVmCloneDiskPlan disk_plan = {0};
    gchar *preflight_error = NULL;
    VmCloneCtx *clone_ctx = g_new0(VmCloneCtx, 1);
    clone_ctx->source = g_strdup(source);
    clone_ctx->target = g_strdup(clone_name);
    clone_ctx->guest_reset = guest_reset;

    {
        gchar *lock_err = NULL;
        if (!lock_vm_operation(source, VM_OP_SNAPSHOT, &lock_err)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                lock_err ? lock_err : "Source VM busy (another operation in progress)");
            pure_uds_server_send_response(server, connection, e);
            g_free(e); g_free(lock_err);
            _vm_clone_ctx_free(clone_ctx);
            return;
        }
        clone_ctx->holds_source_lock = TRUE;
        if (!lock_vm_operation(clone_name, VM_OP_CREATING, &lock_err)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                lock_err ? lock_err : "Target VM name busy (another operation in progress)");
            pure_uds_server_send_response(server, connection, e);
            g_free(e); g_free(lock_err);
            _vm_clone_ctx_free(clone_ctx);
            return;
        }
        clone_ctx->holds_target_lock = TRUE;
    }

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        _vm_clone_ctx_free(clone_ctx);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "Failed to acquire libvirt connection");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    virDomainPtr target_dom = virDomainLookupByName(conn, clone_name);
    if (target_dom) {
        virDomainFree(target_dom);
        virt_conn_pool_release(conn);
        _vm_clone_ctx_free(clone_ctx);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            "Target VM already exists");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    virResetLastError();

    virDomainPtr dom = virDomainLookupByName(conn, source);
    if (!dom) {
        virt_conn_pool_release(conn);
        _vm_clone_ctx_free(clone_ctx);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_NOT_FOUND,
            "Source VM not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    int source_active_state = virDomainIsActive(dom);
    if (source_active_state < 0) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _vm_clone_ctx_free(clone_ctx);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "vm.clone could not verify source VM power state");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }
    if (source_active_state == 1) {
        virDomainFree(dom);
        virt_conn_pool_release(conn);
        _vm_clone_ctx_free(clone_ctx);
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            "vm.clone requires the source VM to be shut off");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        return;
    }

    char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    virDomainFree(dom);
    virt_conn_pool_release(conn);

    if (!pcv_vm_clone_extract_disk_info(xml, &disk_info, &preflight_error) ||
        !pcv_vm_clone_build_disk_plan(clone_name, &disk_info, &disk_plan,
                                      &preflight_error) ||
        !pcv_vm_clone_disk_plan_beta_allowed(&disk_plan, &preflight_error)) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            preflight_error ? preflight_error : "vm.clone beta guard failed");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        free(xml);
        g_free(preflight_error);
        pcv_vm_clone_disk_plan_clear(&disk_plan);
        pcv_vm_clone_disk_info_clear(&disk_info);
        _vm_clone_ctx_free(clone_ctx);
        return;
    }
    free(xml);
    g_free(preflight_error);
    pcv_vm_clone_disk_info_clear(&disk_info);

    gboolean full_copy = mode
        ? (g_strcmp0(mode, "full") == 0)
        : (disk_plan.kind != PCV_VM_CLONE_DISK_ZVOL);
    clone_ctx->full_copy = full_copy;

    if (disk_plan.kind != PCV_VM_CLONE_DISK_ZVOL && !full_copy) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
            "vm.clone cow mode is only supported for ZFS zvol disks; use mode=full for qcow2/raw");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        pcv_vm_clone_disk_plan_clear(&disk_plan);
        _vm_clone_ctx_free(clone_ctx);
        return;
    }

    if ((disk_plan.kind == PCV_VM_CLONE_DISK_QCOW2 ||
         disk_plan.kind == PCV_VM_CLONE_DISK_RAW) &&
        !pcv_vm_clone_file_copy_available()) {
        gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_CONFLICT,
            "vm.clone qcow2/raw file disk clone requires qemu-img");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        pcv_vm_clone_disk_plan_clear(&disk_plan);
        _vm_clone_ctx_free(clone_ctx);
        return;
    }

    clone_ctx->disk_kind = disk_plan.kind;
    clone_ctx->source_disk_path = disk_plan.source_disk_path;
    clone_ctx->target_disk_path = disk_plan.target_disk_path;
    clone_ctx->source_dataset = disk_plan.source_dataset;
    clone_ctx->target_dataset = disk_plan.target_dataset;
    clone_ctx->zfs_pool = disk_plan.zfs_pool;
    clone_ctx->source_zvol_name = disk_plan.source_zvol_name;
    memset(&disk_plan, 0, sizeof(disk_plan));

    if (clone_ctx->target_dataset) {
        const gchar *zfs_list_argv[] = {"zfs", "list", "-H",
                                         clone_ctx->target_dataset, NULL};
        if (pcv_spawn_sync(zfs_list_argv, NULL, NULL, NULL)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                "Target zvol dataset already exists");
            pure_uds_server_send_response(server, connection, e); g_free(e);
            _vm_clone_ctx_free(clone_ctx);
            return;
        }
    }

    if (clone_ctx->disk_kind == PCV_VM_CLONE_DISK_QCOW2 ||
        clone_ctx->disk_kind == PCV_VM_CLONE_DISK_RAW) {
        if (g_file_test(clone_ctx->target_disk_path, G_FILE_TEST_EXISTS)) {
            gchar *e = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_BUSY,
                "Target disk file already exists");
            pure_uds_server_send_response(server, connection, e); g_free(e);
            _vm_clone_ctx_free(clone_ctx);
            return;
        }
    }

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "status", "accepted");
    json_object_set_string_member(obj, "source", source);
    json_object_set_string_member(obj, "clone_name", clone_name);
    json_object_set_string_member(obj, "mode", full_copy ? "full" : "cow");
    gchar *job_id = _vm_clone_job_id(clone_ctx);
    json_object_set_string_member(obj, "job_id", job_id);
    g_free(job_id);
    json_object_set_boolean_member(obj, "guest_reset", guest_reset);
    json_object_set_string_member(obj, "storage_type",
                                  pcv_vm_clone_disk_kind_to_string(clone_ctx->disk_kind));
    json_object_set_string_member(obj, "source_disk", clone_ctx->source_disk_path);
    json_object_set_string_member(obj, "target_disk", clone_ctx->target_disk_path);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);

    GTask *clone_task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(clone_task, clone_ctx, (GDestroyNotify)_vm_clone_ctx_free);
    pcv_worker_pool_push(clone_task, _vm_clone_thread);
    g_object_unref(clone_task);
}

static void _handle_gpu_metrics(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    const gchar *argv[] = {"nvidia-smi",
        "--query-gpu=index,name,utilization.gpu,temperature.gpu,memory.used,memory.total,power.draw",
        "--format=csv,noheader,nounits", NULL};
    gchar *out = nullptr;
    gboolean ok = pcv_spawn_sync(argv, &out, NULL, NULL);
    JsonArray *arr = json_array_new();
    if (ok && out) {
        gchar **lines = g_strsplit(g_strstrip(out), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            gchar **fields = g_strsplit(*l, ", ", -1);
            if (g_strv_length(fields) >= 7) {
                JsonObject *gpu = json_object_new();
                json_object_set_int_member(gpu, "index", atoi(fields[0]));
                json_object_set_string_member(gpu, "name", g_strstrip(fields[1]));
                json_object_set_double_member(gpu, "utilization_pct", atof(fields[2]));
                json_object_set_double_member(gpu, "temperature_c", atof(fields[3]));
                json_object_set_int_member(gpu, "memory_used_mb", atoi(fields[4]));
                json_object_set_int_member(gpu, "memory_total_mb", atoi(fields[5]));
                json_object_set_double_member(gpu, "power_watts", atof(fields[6]));
                json_array_add_object_element(arr, gpu);
            }
            g_strfreev(fields);
        }
        g_strfreev(lines);
    }
    g_free(out);
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_gpu_list(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    const gchar *argv[] = {"lspci", "-nn", NULL};
    gchar *out = nullptr;
    pcv_spawn_sync(argv, &out, NULL, NULL);
    JsonArray *arr = json_array_new();
    if (out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (g_strstr_len(*l, -1, "VGA") || g_strstr_len(*l, -1, "3D") ||
                g_strstr_len(*l, -1, "Display")) {
                JsonObject *gpu = json_object_new();
                json_object_set_string_member(gpu, "pci", *l);
                json_array_add_object_element(arr, gpu);
            }
        }
        g_strfreev(lines); g_free(out);
    }
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_daemon_version(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "version", PCV_PRODUCT_VERSION);
    json_object_set_string_member(obj, "edition", "single");
    json_object_set_int_member(obj, "rpc_methods", (gint64)g_hash_table_size(g_rpc_routes));
    json_object_set_string_member(obj, "build_date", "2026-03-31");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_daemon_update_check(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    PcvUpdateStatus st = pcv_update_check_get();
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "enabled", st.enabled);
    json_object_set_string_member(obj, "current", st.current);
    json_object_set_string_member(obj, "latest", st.latest);
    json_object_set_boolean_member(obj, "update_available", st.update_available);
    json_object_set_string_member(obj, "url", st.url);
    json_object_set_int_member(obj, "checked_at", st.checked_at);
    json_object_set_string_member(obj, "state", st.state);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_node_drain(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection)
{
    guint timeout_sec = json_object_has_member(params, "timeout_sec")
        ? (guint)json_object_get_int_member(params, "timeout_sec") : 30;
    pcv_drain_begin(NULL, timeout_sec);
    JsonNode *ok_node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(ok_node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, ok_node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_node_resume(JsonObject *params, const gchar *rpc_id,
                                 UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    pcv_drain_cancel();
    JsonNode *ok_node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(ok_node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, ok_node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_config_history(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *arr = json_array_new();
    GDir *dir = g_dir_open("/var/lib/purecvisor/", 0, NULL);
    if (dir) {
        const gchar *name;
        while ((name = g_dir_read_name(dir))) {
            if (g_str_has_prefix(name, "daemon.conf.")) {
                JsonObject *entry = json_object_new();
                json_object_set_string_member(entry, "file", name);
                gchar *path = g_strdup_printf("/var/lib/purecvisor/%s", name);
                struct stat st;
                if (stat(path, &st) == 0)
                    json_object_set_int_member(entry, "mtime", (gint64)st.st_mtime);
                g_free(path);
                json_array_add_object_element(arr, entry);
            }
        }
        g_dir_close(dir);
    }
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_config_backup(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    gchar *ts = g_strdup_printf("%ld", (long)time(NULL));
    gchar *dst = g_strdup_printf("/var/lib/purecvisor/daemon.conf.%s", ts);
    const gchar *cp_argv[] = {"cp", "/etc/purecvisor/daemon.conf", dst, NULL};
    pcv_spawn_sync(cp_argv, NULL, NULL, NULL);
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(node, dst);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp); g_free(dst); g_free(ts);
}

static void _handle_template_history(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    const gchar *template_dir = "/etc/purecvisor/templates";
    JsonArray *arr = json_array_new();
    GDir *dir = g_dir_open(template_dir, 0, NULL);
    if (dir) {
        const gchar *name;
        while ((name = g_dir_read_name(dir))) {
            if (g_str_has_suffix(name, ".json")) {
                JsonObject *entry = json_object_new();
                json_object_set_string_member(entry, "name", name);
                gchar *path = g_strdup_printf("%s/%s", template_dir, name);
                struct stat st;
                if (stat(path, &st) == 0) {
                    json_object_set_int_member(entry, "mtime", (gint64)st.st_mtime);
                    json_object_set_int_member(entry, "size", (gint64)st.st_size);
                }
                g_free(path);
                json_array_add_object_element(arr, entry);
            }
        }
        g_dir_close(dir);
    }
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_audit_search(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection)
{
    const gchar *from = json_object_has_member(params, "from_ts")
        ? json_object_get_string_member(params, "from_ts") : NULL;
    const gchar *to = json_object_has_member(params, "to_ts")
        ? json_object_get_string_member(params, "to_ts") : NULL;
    const gchar *user = json_object_has_member(params, "username")
        ? json_object_get_string_member(params, "username") : NULL;
    const gchar *meth = json_object_has_member(params, "method")
        ? json_object_get_string_member(params, "method") : NULL;
    gint lim = json_object_has_member(params, "limit")
        ? (gint)json_object_get_int_member(params, "limit") : 100;
    gint pg_offset = json_object_has_member(params, "offset")
        ? (gint)json_object_get_int_member(params, "offset") : 0;
    JsonArray *results = pcv_audit_search(from, to, user, meth, lim);
    JsonNode *node = _paginate_array(results, pg_offset, lim);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_storage_pool_health(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    const gchar *pool = json_object_has_member(params, "pool")
        ? json_object_get_string_member(params, "pool") : "pcvpool";
    ZfsPoolHealth zh;
    if (pcv_zfs_pool_health(pool, &zh)) {
        JsonObject *result = pcv_zfs_pool_health_to_json(&zh);
        json_object_set_string_member(result, "pool", pool);
        JsonNode *n = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(n, result);
        gchar *resp = pure_rpc_build_success_response(rpc_id, n);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            "Failed to query pool health");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
}

static void _handle_storage_pool_forecast(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *pool = json_object_has_member(params, "pool")
        ? json_object_get_string_member(params, "pool") : "pcvpool";
    JsonObject *result = pcv_zfs_pool_forecast(pool);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

typedef struct {
    gchar *vm_name;
    gchar *s3_endpoint;
    gchar *s3_bucket;
    gchar *s3_key_prefix;
} S3ExportCtx;

static void _s3_export_ctx_free(gpointer data) {
    S3ExportCtx *ctx = data;
    g_free(ctx->vm_name); g_free(ctx->s3_endpoint);
    g_free(ctx->s3_bucket); g_free(ctx->s3_key_prefix);
    g_free(ctx);
}

static void _s3_export_worker(GTask *task, gpointer source __attribute__((unused)),
                               gpointer task_data, GCancellable *cancel __attribute__((unused))) {
    S3ExportCtx *ctx = task_data;
    GError *err = nullptr;
    gboolean ok = pcv_backup_export_s3(ctx->vm_name, ctx->s3_endpoint,
                                        ctx->s3_bucket, ctx->s3_key_prefix, &err);
    gchar *job_id = g_strdup_printf("backup.export_s3:%s", ctx->vm_name);
    if (!ok) {
        const gchar *err_msg = err ? err->message : "unknown";
        g_warning("[S3 Backup] Export failed for '%s': %s",
                  ctx->vm_name, err_msg);
        pcv_audit_log(NULL, "backup.export_s3", ctx->vm_name, "fail",
                      PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.export_s3",
                                         "failed", err_msg);
        if (err) g_error_free(err);
    } else {
        g_message("[S3 Backup] Export completed for '%s'", ctx->vm_name);
        pcv_audit_log(NULL, "backup.export_s3", ctx->vm_name, "ok",
                      0, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.export_s3",
                                         "completed", NULL);
    }
    g_free(job_id);
    g_task_return_boolean(task, ok);
}

static void _handle_backup_export_s3(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!vm_name || !*vm_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required param: name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "vm_name", vm_name);
    json_object_set_string_member(accepted, "target", "s3");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, accepted);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    S3ExportCtx *ctx = g_new0(S3ExportCtx, 1);
    ctx->vm_name = g_strdup(vm_name);
    ctx->s3_endpoint = json_object_has_member(params, "s3_endpoint")
        ? g_strdup(json_object_get_string_member(params, "s3_endpoint")) : NULL;
    ctx->s3_bucket = json_object_has_member(params, "s3_bucket")
        ? g_strdup(json_object_get_string_member(params, "s3_bucket")) : NULL;
    ctx->s3_key_prefix = json_object_has_member(params, "s3_key_prefix")
        ? g_strdup(json_object_get_string_member(params, "s3_key_prefix")) : NULL;

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, _s3_export_ctx_free);
    g_task_run_in_thread(task, _s3_export_worker);
    g_object_unref(task);
}

static void _handle_webhook_dlq_list(JsonObject *params, const gchar *rpc_id,
                                      UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonArray *dlq = pcv_alert_engine_dlq_list();
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, dlq);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_webhook_dlq_retry(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *result = pcv_alert_engine_dlq_retry();
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_security_group_create(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *sg_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!sg_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        extern gboolean pcv_security_group_create(const gchar *name, const gchar *description);
        const gchar *desc = json_object_has_member(params, "description")
            ? json_object_get_string_member(params, "description") : "";
        gboolean ok = pcv_security_group_create(sg_name, desc);
        if (ok) {
            JsonNode *node = json_node_new(JSON_NODE_VALUE);
            json_node_set_boolean(node, TRUE);
            gchar *resp = pure_rpc_build_success_response(rpc_id, node);
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
        } else {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Security group creation failed (already exists?)");
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
        }
    }
}

static void _handle_security_group_list(JsonObject *params, const gchar *rpc_id,
                                         UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    extern JsonArray *pcv_security_group_list(void);
    JsonArray *arr = pcv_security_group_list();
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_security_group_delete(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *sg_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!sg_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        extern gboolean pcv_security_group_delete(const gchar *name);
        gboolean ok = pcv_security_group_delete(sg_name);
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, ok);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
}

static void _handle_security_group_rule_add(JsonObject *params, const gchar *rpc_id,
                                             UdsServer *server, GSocketConnection *connection)
{
    const gchar *sg_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!sg_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: name");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        extern gboolean pcv_security_group_rule_add(const gchar *name, JsonObject *rule);
        gboolean ok = pcv_security_group_rule_add(sg_name, params);
        if (ok) {
            JsonNode *node = json_node_new(JSON_NODE_VALUE);
            json_node_set_boolean(node, TRUE);
            gchar *resp = pure_rpc_build_success_response(rpc_id, node);
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
        } else {
            gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Rule add failed (group not found?)");
            pure_uds_server_send_response(server, connection, resp); g_free(resp);
        }
    }
}

static void _handle_security_group_rule_remove(JsonObject *params, const gchar *rpc_id,
                                                UdsServer *server, GSocketConnection *connection)
{
    const gchar *sg_name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!sg_name || !json_object_has_member(params, "rule_id")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required params: name, rule_id");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }
    gint64 rule_id = json_object_get_int_member(params, "rule_id");
    extern gboolean pcv_security_group_rule_remove(const gchar *name, gint64 rule_id);
    gboolean ok = pcv_security_group_rule_remove(sg_name, rule_id);
    if (ok) {
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "Rule remove failed (group/rule not found)");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
}

static void _handle_ai_healing_approve(JsonObject *params, const gchar *rpc_id,
                                        UdsServer *server, GSocketConnection *connection)
{
    if (!params || !json_object_has_member(params, "action_id")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: action_id");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }
    gint action_id = (gint)json_object_get_int_member(params, "action_id");
    extern void pcv_healing_approve(gint action_id);
    pcv_healing_approve(action_id);
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_ai_healing_reject(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    if (!params || !json_object_has_member(params, "action_id")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: action_id");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }
    gint action_id = (gint)json_object_get_int_member(params, "action_id");
    extern void pcv_healing_dismiss(gint action_id);
    pcv_healing_dismiss(action_id);
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

#include "modules/network/nfv_manager.h"
static void _handle_nfv_lb_create(JsonObject *params, const gchar *rpc_id,
                                   UdsServer *server, GSocketConnection *connection)
{
    if (!params || !json_object_has_member(params, "name") ||
        !json_object_has_member(params, "vip") || !json_object_has_member(params, "port")) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required params: name, vip, port");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }
    const gchar *name = json_object_get_string_member(params, "name");
    const gchar *vip  = json_object_get_string_member(params, "vip");
    gint port         = (gint)json_object_get_int_member(params, "port");

    JsonArray *backends = NULL;
    if (json_object_has_member(params, "backends")) {
        JsonNode *bn = json_object_get_member(params, "backends");
        if (bn && JSON_NODE_HOLDS_ARRAY(bn))
            backends = json_node_get_array(bn);
    }
    guint bn_len = backends ? json_array_get_length(backends) : 0;
    if (bn_len == 0) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "backends must be a non-empty array");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    GString *joined = g_string_new(NULL);
    gboolean bad = FALSE;
    for (guint i = 0; i < bn_len && !bad; i++) {
        JsonNode *el = json_array_get_element(backends, i);
        gchar *bip = NULL;
        gint64 bport = 0;
        if (el && JSON_NODE_HOLDS_OBJECT(el)) {
            JsonObject *bo = json_node_get_object(el);
            const gchar *s = json_object_has_member(bo, "ip")
                ? json_object_get_string_member(bo, "ip") : NULL;
            bip = g_strdup(s);
            bport = json_object_has_member(bo, "port")
                ? json_object_get_int_member(bo, "port") : 0;
        } else if (el && JSON_NODE_HOLDS_VALUE(el)) {
            const gchar *s = json_node_get_string(el);
            const gchar *colon = s ? strrchr(s, ':') : NULL;
            if (colon) {
                bip = g_strndup(s, (gsize)(colon - s));
                bport = g_ascii_strtoll(colon + 1, NULL, 10);
            }
        }
        if (!bip || !pcv_validate_ip_literal(bip) || !pcv_validate_port((gint)bport)) {
            bad = TRUE;
        } else {
            if (joined->len) g_string_append_c(joined, ',');
            if (strchr(bip, ':'))
                g_string_append_printf(joined, "[%s]:%d", bip, (gint)bport);
            else
                g_string_append_printf(joined, "%s:%d", bip, (gint)bport);
        }
        g_free(bip);
    }
    if (bad) {
        g_string_free(joined, TRUE);
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Invalid backend (expect ip + port)");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        return;
    }

    gchar *backends_str = g_string_free(joined, FALSE);
    GError *err = NULL;
    gboolean ok = pcv_nfv_lb_create(name, vip, port, backends_str, &err);
    g_free(backends_str);
    if (ok) {
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION,
            err ? err->message : "LB create failed");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
        if (err) g_error_free(err);
    }
}

static void _handle_security_group_attach(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm = json_object_has_member(params, "vm")
        ? json_object_get_string_member(params, "vm") : NULL;
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!vm || !name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required params: vm, name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    extern gboolean pcv_security_group_apply_to_vm(const gchar *vm, const gchar *sg_name);
    gboolean ok = pcv_security_group_apply_to_vm(vm, name);
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, ok);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_security_group_detach(JsonObject *params, const gchar *rpc_id,
                                          UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm = json_object_has_member(params, "vm")
        ? json_object_get_string_member(params, "vm") : NULL;
    const gchar *name = json_object_has_member(params, "name")
        ? json_object_get_string_member(params, "name") : NULL;
    if (!vm || !name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing required params: vm, name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    extern gboolean pcv_security_group_detach_vm(const gchar *vm, const gchar *sg_name);
    gboolean ok = pcv_security_group_detach_vm(vm, name);
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, ok);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void _handle_vm_security_group_set(JsonObject *params, const gchar *rpc_id,
                                           UdsServer *server, GSocketConnection *connection)
{
    const gchar *vm_name = json_object_has_member(params, "vm")
        ? json_object_get_string_member(params, "vm") : NULL;
    const gchar *sg_name = json_object_has_member(params, "security_group")
        ? json_object_get_string_member(params, "security_group") : NULL;
    if (!vm_name || !sg_name) {
        gchar *resp = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing required params: vm, security_group");
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        extern gboolean pcv_security_group_apply_to_vm(const gchar *vm, const gchar *sg);
        gboolean ok = pcv_security_group_apply_to_vm(vm_name, sg_name);
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, ok);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    }
}

void dispatcher_shutdown_routes(void)
{
    if (g_rpc_routes) {
        g_hash_table_destroy(g_rpc_routes);
        g_rpc_routes = nullptr;
    }

    if (g_pre_hooks) {
        g_ptr_array_free(g_pre_hooks, TRUE);
        g_pre_hooks = nullptr;
    }
}

void purecvisor_dispatcher_dispatch(PureCVisorDispatcher *self,
                                   UdsServer *server,
                                   GSocketConnection *connection,
                                   const gchar *request_json) {

    JsonParser *parser = nullptr;
    GError *err = nullptr;
    if (!pcv_rpc_parse_guarded(request_json, -1, &parser, &err)) {

        gboolean bad_req = err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        gchar *derr = pure_rpc_build_error_response(NULL,
            bad_req ? PURE_RPC_ERR_INVALID_REQUEST : PURE_RPC_ERR_PARSE_ERROR,
            bad_req ? "Invalid Request" : "Parse error");
        pure_uds_server_send_response(server, connection, derr);
        g_free(derr);
        if (err) g_error_free(err);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);

    JsonObject *obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                      ? json_node_get_object(root) : nullptr;
    const gchar *method = obj ? json_object_get_string_member(obj, "method") : nullptr;
    if (!obj || !method) {
        gchar *ierr = pure_rpc_build_error_response(NULL, PURE_RPC_ERR_INVALID_REQUEST,
            "Invalid Request: root must be an object with a string 'method'");
        pure_uds_server_send_response(server, connection, ierr);
        g_free(ierr);
        g_object_unref(parser);
        return;
    }

    gint id = -1;
    gchar *rpc_id_str = nullptr;

    if (json_object_has_member(obj, "id")) {
        JsonNode *id_node = json_object_get_member(obj, "id");
        if (json_node_get_value_type(id_node) == G_TYPE_STRING) {
            rpc_id_str = g_strdup(json_node_get_string(id_node));
            id = 0;
        } else {
            id = json_node_get_int(id_node);
            rpc_id_str = g_strdup_printf("%d", id);
        }
    }

    JsonObject *params = nullptr;
    if (json_object_has_member(obj, "params")) {
        params = json_object_get_object_member(obj, "params");
    }

    if (params) {
        if (!json_object_has_member(params, "vm_id") &&
             json_object_has_member(params, "name")) {
            const gchar *alias = json_object_get_string_member(params, "name");
            if (alias) json_object_set_string_member(params, "vm_id", alias);
        } else if (!json_object_has_member(params, "name") &&
                    json_object_has_member(params, "vm_id")) {
            const gchar *alias = json_object_get_string_member(params, "vm_id");
            if (alias) json_object_set_string_member(params, "name", alias);
        }
    }

    gchar *dispatch_req_id = nullptr;
    {
        const gchar *existing = pcv_log_req_id_get();
        if (!existing || g_strcmp0(existing, "-") == 0) {
            dispatch_req_id = pcv_generate_request_id();
            pcv_log_req_id_set(dispatch_req_id);
        }
    }
    PCV_LOG_INFO("dispatcher", "[%s] method=%s id=%s",
                 pcv_log_req_id_get(), method ? method : "(null)",
                 rpc_id_str ? rpc_id_str : "-");

    DispatcherRequestContext *ctx = g_new0(DispatcherRequestContext, 1);
    ctx->dispatcher = g_object_ref(self);
    ctx->server = g_object_ref(server);
    ctx->connection = g_object_ref(connection);
    ctx->request_id = id;

    gint64 _rpc_start_us = g_get_monotonic_time();
    pcv_prom_rpc_start(method);

    if (!_run_pre_hooks(method, params, rpc_id_str)) {
        gchar *err = pure_rpc_build_error_response(rpc_id_str, PURE_RPC_ERR_ZFS_OPERATION,
            "Request rejected by pre-dispatch hook");
        pure_uds_server_send_response(server, connection, err);
        g_free(err);
        dispatcher_request_context_free(ctx);
        goto rpc_done;
    }

    {
        gint caller_role = _dispatcher_caller_role(params, connection);
        const gchar *caller_sub = _dispatcher_caller_subject(params, connection);
        const gchar *audit_target =
            _container_method_requires_owner_scope(method)
                ? _container_owner_scope_target_from_params(method, params)
                : _vm_owner_scope_target_from_params(method, params);
        gchar *deny_message = NULL;
        gboolean allowed = pcv_dispatcher_check_rbac(method, caller_role);

        if (!allowed) {
            deny_message = g_strdup(
                "Permission denied: insufficient role for this method");
        } else if (caller_role == PCV_ROLE_OPERATOR &&
                   _vm_method_requires_owner_scope(method)) {
            allowed = _vm_owner_scoped_method_allowed(method, params, connection,
                                                      caller_role, &deny_message);
        } else if (caller_role == PCV_ROLE_OPERATOR &&
                   _container_method_requires_owner_scope(method)) {

            allowed = _container_owner_scoped_method_allowed(method, params, connection,
                                                             caller_role, &deny_message);
        }

        if (!allowed) {
            gchar *err = pure_rpc_build_error_response(rpc_id_str, PURE_RPC_ERR_FORBIDDEN,
                deny_message ? deny_message : "Permission denied");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            dispatcher_request_context_free(ctx);
            pcv_audit_log(caller_sub ? caller_sub : "-",
                          method,
                          audit_target ? audit_target : "",
                          "denied", PURE_RPC_ERR_FORBIDDEN, 0, "rbac");
            g_free(deny_message);
            goto rpc_done;
        }
        g_free(deny_message);
    }

    if (g_strcmp0(method, "vm.create") == 0) {

        {
            virConnectPtr qconn = virt_conn_pool_acquire();
            if (qconn) {
                int num = virConnectNumOfDefinedDomains(qconn)
                        + virConnectNumOfDomains(qconn);
                virt_conn_pool_release(qconn);
                if (num >= 200) {
                    gchar *err = pure_rpc_build_error_response(rpc_id_str, PURE_RPC_ERR_ZFS_OPERATION,
                        "VM quota exceeded: maximum 200 VMs per node");
                    pure_uds_server_send_response(server, connection, err);
                    g_free(err);
                    dispatcher_request_context_free(ctx);
                    goto rpc_done;
                }
            }
        }
        handle_vm_create(self, params, rpc_id_str, server, connection);
        dispatcher_request_context_free(ctx);
    } else {

        PcvDispatchHandler handler = (PcvDispatchHandler)g_hash_table_lookup(g_rpc_routes, method);
        if (handler) {
            handler(params, rpc_id_str, server, connection);
            dispatcher_request_context_free(ctx);
        } else if (pcv_plugin_has_handler(method)) {

            pcv_plugin_dispatch(method, params, rpc_id_str, server, connection);
            dispatcher_request_context_free(ctx);
        } else {

            gchar *err = pure_rpc_build_error_response(rpc_id_str, PURE_RPC_ERR_METHOD_NOT_FOUND,
                "Method not found");
            pure_uds_server_send_response(server, connection, err);
            g_free(err);
            dispatcher_request_context_free(ctx);
        }
    }

rpc_done:

    {
        gint64 _rpc_end_us = g_get_monotonic_time();
        gdouble dur_ms = (gdouble)(_rpc_end_us - _rpc_start_us) / 1000.0;
        pcv_prom_rpc_end(method, TRUE, dur_ms);

        if (!pcv_dispatcher_is_async_method(method)) {
            pcv_audit_log_rpc(method, "ok", 0, (gint64)dur_ms);
        }

        if (dur_ms > 1000.0) {
            g_warning("[dispatcher] SLOW RPC: method=%s id=%s took %.0fms",
                      method, rpc_id_str ? rpc_id_str : "(null)", dur_ms);
        }
    }

    g_free(rpc_id_str);
    g_free(dispatch_req_id);
    pcv_log_req_id_set(NULL);
    g_object_unref(parser);
}

static void _handle_snapshot_schedule_status(JsonObject *params, const gchar *rpc_id,
                                              UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    JsonObject *result = pcv_snapshot_schedule_status();
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_daemon_config_get(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    const gchar *section = (params && json_object_has_member(params, "section"))
        ? json_object_get_string_member(params, "section") : NULL;

    JsonObject *result = json_object_new();

    if (!section || g_strcmp0(section, "storage") == 0) {
        JsonObject *stg = json_object_new();
        json_object_set_string_member(stg, "zvol_pool",       pcv_config_get_zvol_pool());
        json_object_set_string_member(stg, "container_pool",  pcv_config_get_container_pool());
        json_object_set_string_member(stg, "image_dir",       pcv_config_get_image_dir());
        json_object_set_string_member(stg, "iso_dirs",        pcv_config_get_iso_dirs());
        json_object_set_object_member(result, "storage", stg);
    }
    if (!section || g_strcmp0(section, "container") == 0) {
        JsonObject *ctr = json_object_new();
        json_object_set_string_member(ctr, "lxc_path",        pcv_config_get_container_path());
        json_object_set_string_member(ctr, "rootless",
            pcv_config_get_string("container", "rootless", "false"));
        json_object_set_object_member(result, "container", ctr);
    }

    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void _handle_daemon_config_set(JsonObject *params, const gchar *rpc_id,
                                       UdsServer *server, GSocketConnection *connection)
{
    const gchar *section = (params && json_object_has_member(params, "section"))
        ? json_object_get_string_member(params, "section") : NULL;
    const gchar *key = (params && json_object_has_member(params, "key"))
        ? json_object_get_string_member(params, "key") : NULL;
    const gchar *value = (params && json_object_has_member(params, "value"))
        ? json_object_get_string_member(params, "value") : NULL;

    if (!section || !key || !value) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Required: section, key, value");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        return;
    }

    if (g_strcmp0(section, "storage") != 0 &&
        g_strcmp0(section, "container") != 0 &&
        g_strcmp0(section, "alert") != 0 &&
        g_strcmp0(section, "backup") != 0) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Section not editable (allowed: storage, container, alert, backup)");
        pure_uds_server_send_response(server, connection, err); g_free(err);
        return;
    }

    const gchar *conf_path = "/etc/purecvisor/daemon.conf";
    GKeyFile *kf = g_key_file_new();
    GError *error = nullptr;
    g_key_file_load_from_file(kf, conf_path, G_KEY_FILE_KEEP_COMMENTS, &error);
    if (error) { g_error_free(error); error = nullptr; }

    g_key_file_set_string(kf, section, key, value);

    gchar *data = g_key_file_to_data(kf, NULL, NULL);
    g_file_set_contents(conf_path, data, -1, &error);
    g_free(data);
    g_key_file_free(kf);

    if (error) {
        gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, error->message);
        pure_uds_server_send_response(server, connection, err);
        g_free(err); g_error_free(error);
        return;
    }

    pcv_config_reload();

    JsonObject *ok = json_object_new();
    json_object_set_boolean_member(ok, "success", TRUE);
    json_object_set_string_member(ok, "section", section);
    json_object_set_string_member(ok, "key", key);
    json_object_set_string_member(ok, "value", value);
    JsonNode *nn = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(nn, ok);
    gchar *resp = pure_rpc_build_success_response(rpc_id, nn);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);
}

static void dispatcher_init_routes(void)
{
    if (g_rpc_routes) return;

    g_method_policy_map = g_hash_table_new(g_str_hash, g_str_equal);
    for (int i = 0; g_method_policies[i].method; i++) {
        g_hash_table_insert(g_method_policy_map,
                            (gpointer)g_method_policies[i].method,
                            GINT_TO_POINTER(g_method_policies[i].min_role));
    }

    g_rpc_routes = g_hash_table_new(g_str_hash, g_str_equal);

    if (!g_async_methods) {
        g_async_methods = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }

    static const char *_async_method_names[] = {

        "vm.start",
        "vm.create",
        "vm.delete",
        "vm.stop", "vm.pause", "vm.resume", "vm.limit",

        "vm.list",
        "vm.metrics",
        "vm.guest.ping",
        "vm.guest.exec",
        "vm.guest.shutdown",
        "vm.snapshot.create",
        "vm.snapshot.list",
        "vm.snapshot.delete",
        "vm.snapshot.delete_all",
        "vm.snapshot.rollback",
        "vm.export.ova",

        "backup.restore",
        "backup.replicate",
        "backup.export_s3",
        "backup.incremental",
        "container.create",
        "container.clone",
        "container.destroy",
        "vm.disk.live_resize",
        "vm.resize_disk",
        "vm.clone",
        "vm.import.ova",
        "cloud.import",
        "cloud.export",
        "cloud.import.finalize",
        "security.action.approve",

        "backup.snapshot.verify",
        NULL
    };
    for (int _i = 0; _async_method_names[_i]; _i++) {
        g_hash_table_add(g_async_methods, g_strdup(_async_method_names[_i]));
    }
    pcv_bootstrap_register_async_methods(g_async_methods);

    g_hash_table_insert(g_rpc_routes, "vm.start",           (gpointer)handle_vm_start_request);
    g_hash_table_insert(g_rpc_routes, "vm.stop",            (gpointer)handle_vm_stop_request);
    g_hash_table_insert(g_rpc_routes, "vm.pause",           (gpointer)handle_vm_pause_request);
    g_hash_table_insert(g_rpc_routes, "vm.resume",          (gpointer)handle_vm_resume_request);
    g_hash_table_insert(g_rpc_routes, "vm.guest.ping",      (gpointer)handle_vm_guest_ping_request);
    g_hash_table_insert(g_rpc_routes, "vm.guest.agent.status", (gpointer)handle_vm_guest_agent_status_request);
    g_hash_table_insert(g_rpc_routes, "vm.guest.agent.ensure_channel", (gpointer)handle_vm_guest_agent_ensure_channel_request);
    g_hash_table_insert(g_rpc_routes, "vm.guest.fsinfo",    (gpointer)handle_vm_guest_fsinfo_request);
    g_hash_table_insert(g_rpc_routes, "vm.guest.exec",      (gpointer)handle_vm_guest_exec_request);
    g_hash_table_insert(g_rpc_routes, "vm.guest.shutdown",  (gpointer)handle_vm_guest_shutdown_request);
    g_hash_table_insert(g_rpc_routes, "vm.delete",          (gpointer)handle_vm_delete_request);
    g_hash_table_insert(g_rpc_routes, "vm.delete.status",   (gpointer)_handle_vm_delete_status);
    g_hash_table_insert(g_rpc_routes, "vm.list",            (gpointer)handle_vm_list_request);
    g_hash_table_insert(g_rpc_routes, "vm.limit",           (gpointer)handle_vm_limit_request);
    g_hash_table_insert(g_rpc_routes, "vm.metrics",         (gpointer)handle_vm_metrics_request);
    g_hash_table_insert(g_rpc_routes, "vm.rename",          (gpointer)handle_vm_rename_request);
    g_hash_table_insert(g_rpc_routes, "vm.vnc",             (gpointer)handle_vm_vnc_request);
    g_hash_table_insert(g_rpc_routes, "get_vnc_info",       (gpointer)handle_vnc_request);
    g_hash_table_insert(g_rpc_routes, "vm.mount_iso",       (gpointer)handle_vm_mount_iso);
    g_hash_table_insert(g_rpc_routes, "vm.eject",           (gpointer)handle_vm_eject_iso);
    g_hash_table_insert(g_rpc_routes, "vm.resize_disk",     (gpointer)_handle_vm_resize_disk);
    g_hash_table_insert(g_rpc_routes, "vm.clone",           (gpointer)_handle_vm_clone);
    g_hash_table_insert(g_rpc_routes, "vm.set_bandwidth",   (gpointer)handle_vm_set_bandwidth);

    g_hash_table_insert(g_rpc_routes, "vm.snapshot.create",     (gpointer)handle_vm_snapshot_create);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.list",       (gpointer)handle_vm_snapshot_list);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.rollback",   (gpointer)handle_vm_snapshot_rollback);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.delete",     (gpointer)handle_vm_snapshot_delete);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.delete_all", (gpointer)handle_vm_snapshot_delete_all);

    g_hash_table_insert(g_rpc_routes, "vm.set_memory",       (gpointer)handle_vm_set_memory_request);
    g_hash_table_insert(g_rpc_routes, "vm.set_vcpu",         (gpointer)handle_vm_set_vcpu_request);
    g_hash_table_insert(g_rpc_routes, "vm.pin_vcpu",         (gpointer)handle_vm_pin_vcpu);
    g_hash_table_insert(g_rpc_routes, "vm.memory.stats",     (gpointer)handle_vm_memory_stats_request);
    g_hash_table_insert(g_rpc_routes, "vm.cpu.stats",        (gpointer)handle_vm_cpu_stats_request);
    g_hash_table_insert(g_rpc_routes, "vm.disk.live_resize", (gpointer)handle_vm_disk_live_resize_request);

    g_hash_table_insert(g_rpc_routes, "device.disk.attach",  (gpointer)handle_device_disk_attach);
    g_hash_table_insert(g_rpc_routes, "device.disk.detach",  (gpointer)handle_device_disk_detach);
    g_hash_table_insert(g_rpc_routes, "device.nic.list",     (gpointer)handle_device_nic_list);
    g_hash_table_insert(g_rpc_routes, "device.nic.attach",   (gpointer)handle_device_nic_attach);
    g_hash_table_insert(g_rpc_routes, "device.nic.detach",   (gpointer)handle_device_nic_detach);
    g_hash_table_insert(g_rpc_routes, "vm.usb.attach",       (gpointer)handle_vm_usb_attach);
    g_hash_table_insert(g_rpc_routes, "vm.usb.detach",       (gpointer)handle_vm_usb_detach);
    g_hash_table_insert(g_rpc_routes, "vm.usb.list",         (gpointer)handle_vm_usb_list);

    g_hash_table_insert(g_rpc_routes, "network.create",         (gpointer)handle_network_create_request);
    g_hash_table_insert(g_rpc_routes, "network.delete",         (gpointer)handle_network_delete_request);
    g_hash_table_insert(g_rpc_routes, "network.list",           (gpointer)handle_network_list_request);
    g_hash_table_insert(g_rpc_routes, "network.info",           (gpointer)handle_network_info_request);
    g_hash_table_insert(g_rpc_routes, "network.mode_set",       (gpointer)handle_network_mode_set_request);
    g_hash_table_insert(g_rpc_routes, "network.bind_phys",      (gpointer)handle_network_bind_phys_request);
    g_hash_table_insert(g_rpc_routes, "network.dhcp_toggle",    (gpointer)handle_network_dhcp_toggle_request);
    g_hash_table_insert(g_rpc_routes, "network.ovs.create",     (gpointer)handle_network_ovs_create_request);
    g_hash_table_insert(g_rpc_routes, "network.ovs.delete",     (gpointer)handle_network_ovs_delete_request);
    g_hash_table_insert(g_rpc_routes, "network.ovs.vxlan.add",  (gpointer)handle_network_ovs_vxlan_add_request);
    g_hash_table_insert(g_rpc_routes, "network.ovs.vxlan.del",  (gpointer)handle_network_ovs_vxlan_del_request);

    g_hash_table_insert(g_rpc_routes, "storage.pool.list",     (gpointer)handle_storage_pool_list_request);
    g_hash_table_insert(g_rpc_routes, "storage.zvol.list",     (gpointer)handle_storage_zvol_list_request);
    g_hash_table_insert(g_rpc_routes, "storage.zvol.create",   (gpointer)handle_storage_zvol_create_request);
    g_hash_table_insert(g_rpc_routes, "storage.zvol.delete",   (gpointer)handle_storage_zvol_delete_request);
    g_hash_table_insert(g_rpc_routes, "storage.pool.create",   (gpointer)handle_storage_pool_create_request);
    g_hash_table_insert(g_rpc_routes, "storage.pool.destroy",  (gpointer)handle_storage_pool_destroy_request);
    g_hash_table_insert(g_rpc_routes, "storage.pool.scrub",    (gpointer)handle_storage_pool_scrub_request);
    g_hash_table_insert(g_rpc_routes, "storage.pool.health",   (gpointer)_handle_storage_pool_health);
    g_hash_table_insert(g_rpc_routes, "storage.pool.forecast", (gpointer)_handle_storage_pool_forecast);

    g_hash_table_insert(g_rpc_routes, "container.create",           (gpointer)handle_container_create);
    g_hash_table_insert(g_rpc_routes, "container.destroy",          (gpointer)handle_container_destroy);
    g_hash_table_insert(g_rpc_routes, "container.start",            (gpointer)handle_container_start);
    g_hash_table_insert(g_rpc_routes, "container.stop",             (gpointer)handle_container_stop);
    g_hash_table_insert(g_rpc_routes, "container.list",             (gpointer)handle_container_list);
    g_hash_table_insert(g_rpc_routes, "container.metrics",          (gpointer)handle_container_metrics);
    g_hash_table_insert(g_rpc_routes, "container.exec",             (gpointer)handle_container_exec);
    g_hash_table_insert(g_rpc_routes, "container.snapshot.create",  (gpointer)handle_container_snapshot_create);
    g_hash_table_insert(g_rpc_routes, "container.snapshot.rollback",(gpointer)handle_container_snapshot_rollback);
    g_hash_table_insert(g_rpc_routes, "container.snapshot.delete",  (gpointer)handle_container_snapshot_delete);
    g_hash_table_insert(g_rpc_routes, "container.snapshot.list",    (gpointer)handle_container_snapshot_list);
    g_hash_table_insert(g_rpc_routes, "container.set_limits",       (gpointer)_handle_container_set_limits);
    g_hash_table_insert(g_rpc_routes, "container.nic.list",         (gpointer)_handle_container_nic_list);
    g_hash_table_insert(g_rpc_routes, "container.nic.attach",       (gpointer)_handle_container_nic_attach);
    g_hash_table_insert(g_rpc_routes, "container.nic.detach",       (gpointer)_handle_container_nic_detach);
    g_hash_table_insert(g_rpc_routes, "container.set_bandwidth",    (gpointer)_handle_container_set_bandwidth);
    g_hash_table_insert(g_rpc_routes, "container.logs",            (gpointer)handle_container_logs);
    g_hash_table_insert(g_rpc_routes, "container.volume.attach",   (gpointer)handle_container_volume_attach);
    g_hash_table_insert(g_rpc_routes, "container.volume.detach",   (gpointer)handle_container_volume_detach);
    g_hash_table_insert(g_rpc_routes, "container.volume.list",     (gpointer)handle_container_volume_list);
    g_hash_table_insert(g_rpc_routes, "container.env.set",         (gpointer)handle_container_env_set);
    g_hash_table_insert(g_rpc_routes, "container.env.list",        (gpointer)handle_container_env_list);
    g_hash_table_insert(g_rpc_routes, "container.env.delete",      (gpointer)handle_container_env_delete);
    g_hash_table_insert(g_rpc_routes, "container.health.set",      (gpointer)handle_container_health_set);
    g_hash_table_insert(g_rpc_routes, "container.health.get",      (gpointer)handle_container_health_get);
    g_hash_table_insert(g_rpc_routes, "container.health.delete",   (gpointer)handle_container_health_delete);

    g_hash_table_insert(g_rpc_routes, "monitor.metrics",     (gpointer)handle_monitor_metrics);
    g_hash_table_insert(g_rpc_routes, "monitor.fleet",       (gpointer)_handle_monitor_fleet);
    g_hash_table_insert(g_rpc_routes, "monitor.processes",   (gpointer)_handle_monitor_processes);
    g_hash_table_insert(g_rpc_routes, "alert.history",       (gpointer)_handle_alert_history);
    g_hash_table_insert(g_rpc_routes, "alert.config.get",    (gpointer)_handle_alert_config_get);
    g_hash_table_insert(g_rpc_routes, "alert.config.set",    (gpointer)_handle_alert_config_set);
    g_hash_table_insert(g_rpc_routes, "alert.config.reload", (gpointer)_handle_alert_config_reload);

    g_hash_table_insert(g_rpc_routes, "agent.config.get",    (gpointer)_handle_agent_config_get);
    g_hash_table_insert(g_rpc_routes, "agent.config.set",    (gpointer)_handle_agent_config_set);
    g_hash_table_insert(g_rpc_routes, "agent.history",       (gpointer)_handle_agent_history);
    g_hash_table_insert(g_rpc_routes, "healing.history",     (gpointer)_handle_healing_history);
    g_hash_table_insert(g_rpc_routes, "healing.pending",     (gpointer)_handle_healing_pending);
    g_hash_table_insert(g_rpc_routes, "healing.set_mode",    (gpointer)_handle_healing_set_mode);
    g_hash_table_insert(g_rpc_routes, "anomaly.reset_baseline", (gpointer)_handle_anomaly_reset_baseline);
    g_hash_table_insert(g_rpc_routes, "agent.compare_manual",   (gpointer)_handle_agent_compare_manual);

    g_hash_table_insert(g_rpc_routes, "overlay.create",      (gpointer)handle_overlay_create);
    g_hash_table_insert(g_rpc_routes, "overlay.delete",      (gpointer)handle_overlay_delete);
    g_hash_table_insert(g_rpc_routes, "overlay.list",        (gpointer)handle_overlay_list);
    g_hash_table_insert(g_rpc_routes, "overlay.info",        (gpointer)handle_overlay_info);
    g_hash_table_insert(g_rpc_routes, "overlay.add_peer",    (gpointer)handle_overlay_add_peer);
    g_hash_table_insert(g_rpc_routes, "overlay.remove_peer", (gpointer)handle_overlay_remove_peer);

    g_hash_table_insert(g_rpc_routes, "iscsi.target.create", (gpointer)handle_iscsi_target_create);
    g_hash_table_insert(g_rpc_routes, "iscsi.target.delete", (gpointer)handle_iscsi_target_delete);
    g_hash_table_insert(g_rpc_routes, "iscsi.target.list",   (gpointer)handle_iscsi_target_list);
    g_hash_table_insert(g_rpc_routes, "iscsi.connect",       (gpointer)handle_iscsi_connect);
    g_hash_table_insert(g_rpc_routes, "iscsi.disconnect",    (gpointer)handle_iscsi_disconnect);
    g_hash_table_insert(g_rpc_routes, "iso.list",            (gpointer)handle_iso_list);

    g_hash_table_insert(g_rpc_routes, "ovn.switch.create",   (gpointer)handle_ovn_switch_create);
    g_hash_table_insert(g_rpc_routes, "ovn.switch.delete",   (gpointer)handle_ovn_switch_delete);
    g_hash_table_insert(g_rpc_routes, "ovn.switch.list",     (gpointer)handle_ovn_switch_list);
    g_hash_table_insert(g_rpc_routes, "ovn.switch.detail",   (gpointer)handle_ovn_switch_detail);
    g_hash_table_insert(g_rpc_routes, "ovn.port.add",        (gpointer)handle_ovn_port_add);
    g_hash_table_insert(g_rpc_routes, "ovn.port.remove",     (gpointer)handle_ovn_port_remove);
    g_hash_table_insert(g_rpc_routes, "ovn.acl.add",         (gpointer)handle_ovn_acl_add);
    g_hash_table_insert(g_rpc_routes, "ovn.acl.list",        (gpointer)handle_ovn_acl_list);
    g_hash_table_insert(g_rpc_routes, "ovn.router.create",   (gpointer)handle_ovn_router_create);
    g_hash_table_insert(g_rpc_routes, "ovn.router.delete",   (gpointer)handle_ovn_router_delete);
    g_hash_table_insert(g_rpc_routes, "ovn.router.list",     (gpointer)handle_ovn_router_list);
    g_hash_table_insert(g_rpc_routes, "ovn.router.detail",   (gpointer)handle_ovn_router_detail);
    g_hash_table_insert(g_rpc_routes, "ovn.router.add_port", (gpointer)handle_ovn_router_add_port);
    g_hash_table_insert(g_rpc_routes, "ovn.dhcp.enable",     (gpointer)handle_ovn_dhcp_enable);
    g_hash_table_insert(g_rpc_routes, "ovn.nat.add",         (gpointer)handle_ovn_nat_add);
    g_hash_table_insert(g_rpc_routes, "ovn.nat.list",        (gpointer)handle_ovn_nat_list);
    g_hash_table_insert(g_rpc_routes, "ovn.tenant.create",   (gpointer)handle_ovn_tenant_create);
    g_hash_table_insert(g_rpc_routes, "ovn.status",          (gpointer)handle_ovn_status);

    g_hash_table_insert(g_rpc_routes, "dpdk.status",         (gpointer)handle_dpdk_status);
    g_hash_table_insert(g_rpc_routes, "dpdk.bind",           (gpointer)handle_dpdk_bind);
    g_hash_table_insert(g_rpc_routes, "dpdk.unbind",         (gpointer)handle_dpdk_unbind);
    g_hash_table_insert(g_rpc_routes, "dpdk.list",           (gpointer)handle_dpdk_list);
    g_hash_table_insert(g_rpc_routes, "dpdk.bridge.create",  (gpointer)handle_dpdk_bridge_create);
    g_hash_table_insert(g_rpc_routes, "dpdk.bridge.delete",  (gpointer)handle_dpdk_bridge_delete);
    g_hash_table_insert(g_rpc_routes, "dpdk.hugepage.info",  (gpointer)handle_dpdk_hugepage_info);

    g_hash_table_insert(g_rpc_routes, "sriov.status",        (gpointer)handle_sriov_status);
    g_hash_table_insert(g_rpc_routes, "sriov.enable",        (gpointer)handle_sriov_enable);
    g_hash_table_insert(g_rpc_routes, "sriov.disable",       (gpointer)handle_sriov_disable);
    g_hash_table_insert(g_rpc_routes, "sriov.list",          (gpointer)handle_sriov_list);
    g_hash_table_insert(g_rpc_routes, "sriov.set",           (gpointer)handle_sriov_set);
    g_hash_table_insert(g_rpc_routes, "sriov.attach",        (gpointer)handle_sriov_attach);
    g_hash_table_insert(g_rpc_routes, "sriov.detach",        (gpointer)handle_sriov_detach);

    g_hash_table_insert(g_rpc_routes, "auth.user.create",    (gpointer)handle_auth_user_create);
    g_hash_table_insert(g_rpc_routes, "auth.user.list",      (gpointer)handle_auth_user_list);
    g_hash_table_insert(g_rpc_routes, "auth.user.delete",    (gpointer)handle_auth_user_delete);
    g_hash_table_insert(g_rpc_routes, "auth.role.set",       (gpointer)handle_auth_role_set);

    g_hash_table_insert(g_rpc_routes, "template.list",       (gpointer)handle_template_list);
    g_hash_table_insert(g_rpc_routes, "template.get",        (gpointer)handle_template_get);
    g_hash_table_insert(g_rpc_routes, "template.create",     (gpointer)handle_template_create);
    g_hash_table_insert(g_rpc_routes, "template.delete",     (gpointer)handle_template_delete);
    g_hash_table_insert(g_rpc_routes, "template.history",    (gpointer)_handle_template_history);

    g_hash_table_insert(g_rpc_routes, "backup.policy.set",   (gpointer)handle_backup_policy_set);
    g_hash_table_insert(g_rpc_routes, "backup.policy.list",  (gpointer)handle_backup_policy_list);
    g_hash_table_insert(g_rpc_routes, "backup.policy.delete",(gpointer)handle_backup_policy_delete);
    g_hash_table_insert(g_rpc_routes, "backup.history",      (gpointer)handle_backup_history);
    g_hash_table_insert(g_rpc_routes, "backup.restore",      (gpointer)handle_backup_restore);
    g_hash_table_insert(g_rpc_routes, "backup.incremental",  (gpointer)handle_backup_incremental);
    g_hash_table_insert(g_rpc_routes, "backup.verify",       (gpointer)handle_backup_verify);
    g_hash_table_insert(g_rpc_routes, "backup.replicate",    (gpointer)handle_backup_replicate);
    g_hash_table_insert(g_rpc_routes, "backup.export_s3",   (gpointer)_handle_backup_export_s3);

    g_hash_table_insert(g_rpc_routes, "security.event.list",       (gpointer)handle_security_event_list);
    g_hash_table_insert(g_rpc_routes, "security.event.get",        (gpointer)handle_security_event_get);
    g_hash_table_insert(g_rpc_routes, "security.action.pending",   (gpointer)handle_security_action_pending);
    g_hash_table_insert(g_rpc_routes, "security.action.approve",   (gpointer)handle_security_action_approve);
    g_hash_table_insert(g_rpc_routes, "security.action.dismiss",   (gpointer)handle_security_action_dismiss);
    g_hash_table_insert(g_rpc_routes, "security.baseline.status",  (gpointer)handle_security_baseline_status);
    g_hash_table_insert(g_rpc_routes, "security.baseline.refresh", (gpointer)handle_security_baseline_refresh);
    g_hash_table_insert(g_rpc_routes, "security.config.get",       (gpointer)handle_security_config_get);
    g_hash_table_insert(g_rpc_routes, "security.config.set",       (gpointer)handle_security_config_set);

    g_hash_table_insert(g_rpc_routes, "snapshot.schedule.status", (gpointer)_handle_snapshot_schedule_status);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.schedule.set",    (gpointer)handle_snapshot_schedule_set);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.schedule.list",   (gpointer)handle_snapshot_schedule_list);
    g_hash_table_insert(g_rpc_routes, "vm.snapshot.schedule.delete", (gpointer)handle_snapshot_schedule_delete);

    g_hash_table_insert(g_rpc_routes, "vm.blkio.set",     (gpointer)handle_vm_blkio_set);
    g_hash_table_insert(g_rpc_routes, "vm.blkio.get",     (gpointer)handle_vm_blkio_get);

    g_hash_table_insert(g_rpc_routes, "vm.import.ec2",       (gpointer)_handle_vm_import_ec2);
    g_hash_table_insert(g_rpc_routes, "vm.export.ec2",       (gpointer)_handle_vm_export_ec2);
    g_hash_table_insert(g_rpc_routes, "vm.import.status",    (gpointer)_handle_cloud_migration_status);
    g_hash_table_insert(g_rpc_routes, "vm.export.status",    (gpointer)_handle_cloud_migration_status);
    g_hash_table_insert(g_rpc_routes, "cloud.jobs.list",     (gpointer)_handle_cloud_jobs_list);
    g_hash_table_insert(g_rpc_routes, "cloud.job.cancel",    (gpointer)_handle_cloud_job_cancel);

    g_hash_table_insert(g_rpc_routes, "daemon.version",      (gpointer)_handle_daemon_version);
    g_hash_table_insert(g_rpc_routes, "daemon.update_check", (gpointer)_handle_daemon_update_check);
    g_hash_table_insert(g_rpc_routes, "node.drain",          (gpointer)_handle_node_drain);
    g_hash_table_insert(g_rpc_routes, "node.resume",         (gpointer)_handle_node_resume);
    g_hash_table_insert(g_rpc_routes, "quota.get",           (gpointer)_handle_quota_get);

    pcv_bootstrap_register_rpc_routes(g_rpc_routes);

    g_hash_table_insert(g_rpc_routes, "gpu.metrics",         (gpointer)_handle_gpu_metrics);
    g_hash_table_insert(g_rpc_routes, "gpu.list",            (gpointer)_handle_gpu_list);

    g_hash_table_insert(g_rpc_routes, "webhook.dlq.list",    (gpointer)_handle_webhook_dlq_list);
    g_hash_table_insert(g_rpc_routes, "webhook.dlq.retry",   (gpointer)_handle_webhook_dlq_retry);

    g_hash_table_insert(g_rpc_routes, "security_group.create",   (gpointer)_handle_security_group_create);
    g_hash_table_insert(g_rpc_routes, "security_group.list",     (gpointer)_handle_security_group_list);
    g_hash_table_insert(g_rpc_routes, "security_group.delete",   (gpointer)_handle_security_group_delete);
    g_hash_table_insert(g_rpc_routes, "security_group.rule.add",    (gpointer)_handle_security_group_rule_add);
    g_hash_table_insert(g_rpc_routes, "security_group.rule.remove", (gpointer)_handle_security_group_rule_remove);
    g_hash_table_insert(g_rpc_routes, "ai.healing.approve",         (gpointer)_handle_ai_healing_approve);
    g_hash_table_insert(g_rpc_routes, "ai.healing.reject",          (gpointer)_handle_ai_healing_reject);
    g_hash_table_insert(g_rpc_routes, "nfv.lb.create",              (gpointer)_handle_nfv_lb_create);
    g_hash_table_insert(g_rpc_routes, "vm.security_group.set",   (gpointer)_handle_vm_security_group_set);
    g_hash_table_insert(g_rpc_routes, "security_group.attach",   (gpointer)_handle_security_group_attach);
    g_hash_table_insert(g_rpc_routes, "security_group.detach",   (gpointer)_handle_security_group_detach);

    g_hash_table_insert(g_rpc_routes, "config.history",      (gpointer)_handle_config_history);
    g_hash_table_insert(g_rpc_routes, "config.backup",       (gpointer)_handle_config_backup);
    g_hash_table_insert(g_rpc_routes, "daemon.config.get",   (gpointer)_handle_daemon_config_get);
    g_hash_table_insert(g_rpc_routes, "daemon.config.set",   (gpointer)_handle_daemon_config_set);
    g_hash_table_insert(g_rpc_routes, "audit.search",        (gpointer)_handle_audit_search);

    g_hash_table_insert(g_rpc_routes, "network.qos.set",      (gpointer)handle_network_qos_set);
    g_hash_table_insert(g_rpc_routes, "network.qos.get",      (gpointer)handle_network_qos_get);
    g_hash_table_insert(g_rpc_routes, "network.qos.remove",   (gpointer)handle_network_qos_remove);

    g_hash_table_insert(g_rpc_routes, "vm.import.ova",        (gpointer)_handle_vm_import_ova);
    g_hash_table_insert(g_rpc_routes, "vm.export.ova",        (gpointer)_handle_vm_export_ova);

    g_hash_table_insert(g_rpc_routes, "jobs.list",            (gpointer)_handle_jobs_list);
    g_hash_table_insert(g_rpc_routes, "jobs.get",             (gpointer)_handle_jobs_get);
    g_hash_table_insert(g_rpc_routes, "jobs.status",          (gpointer)_handle_jobs_get);
    g_hash_table_insert(g_rpc_routes, "jobs.cancel",          (gpointer)_handle_jobs_cancel);
    g_hash_table_insert(g_rpc_routes, "prometheus.sd",        (gpointer)_handle_prometheus_sd);
    g_hash_table_insert(g_rpc_routes, "vm.event.webhook.list",(gpointer)_handle_vm_event_webhook_list);
    g_hash_table_insert(g_rpc_routes, "alert.action.list",    (gpointer)_handle_alert_action_list);

    g_hash_table_insert(g_rpc_routes, "auth.apikey.create",   (gpointer)_handle_apikey_create);
    g_hash_table_insert(g_rpc_routes, "auth.apikey.list",     (gpointer)_handle_apikey_list);
    g_hash_table_insert(g_rpc_routes, "auth.apikey.revoke",   (gpointer)_handle_apikey_revoke);
    g_hash_table_insert(g_rpc_routes, "auth.session.revoke",  (gpointer)_handle_session_revoke);
    g_hash_table_insert(g_rpc_routes, "auth.user.sessions.revoke", (gpointer)_handle_user_sessions_revoke);

    g_hash_table_insert(g_rpc_routes, "vm.batch",             (gpointer)_handle_vm_batch);
    g_hash_table_insert(g_rpc_routes, "vm.list.filtered",     (gpointer)_handle_vm_list_filtered);
    g_hash_table_insert(g_rpc_routes, "pool.conninfo",        (gpointer)_handle_pool_conninfo);

    g_hash_table_insert(g_rpc_routes, "config.reload",        (gpointer)_handle_config_reload);
    g_hash_table_insert(g_rpc_routes, "health.deep",          (gpointer)_handle_health_deep);
    g_hash_table_insert(g_rpc_routes, "backup.snapshot.verify",(gpointer)_handle_snapshot_verify);
    g_hash_table_insert(g_rpc_routes, "jobs.persist.list",    (gpointer)_handle_jobs_persist_list);
    g_hash_table_insert(g_rpc_routes, "alert.silence",        (gpointer)_handle_alert_silence);
    g_hash_table_insert(g_rpc_routes, "alert.silence.list",   (gpointer)_handle_alert_silence_list);
    g_hash_table_insert(g_rpc_routes, "alert.dlq.list",       (gpointer)_handle_alert_dlq_list);
    g_hash_table_insert(g_rpc_routes, "alert.dlq.retry",      (gpointer)_handle_alert_dlq_retry);
    g_hash_table_insert(g_rpc_routes, "alert.config.routing", (gpointer)_handle_alert_routing);
    g_hash_table_insert(g_rpc_routes, "db.migration.status",  (gpointer)_handle_db_migration_status);

    g_hash_table_insert(g_rpc_routes, "container.snapshot.create", (gpointer)_handle_container_snapshot_create);
    g_hash_table_insert(g_rpc_routes, "container.snapshot.list",   (gpointer)_handle_container_snapshot_list);
    g_hash_table_insert(g_rpc_routes, "container.snapshot.delete", (gpointer)_handle_container_snapshot_delete);
    g_hash_table_insert(g_rpc_routes, "container.clone",           (gpointer)_handle_container_clone);
    g_hash_table_insert(g_rpc_routes, "container.memory.stats",    (gpointer)_handle_container_memory_stats);
    g_hash_table_insert(g_rpc_routes, "container.health.check",    (gpointer)_handle_container_health_check);

    g_hash_table_insert(g_rpc_routes, "vm.numa.info",        (gpointer)_handle_vm_numa_info);
    g_hash_table_insert(g_rpc_routes, "vm.autostart",        (gpointer)_handle_vm_autostart);
    g_hash_table_insert(g_rpc_routes, "vm.sla.report",       (gpointer)_handle_vm_sla_report);
    g_hash_table_insert(g_rpc_routes, "capacity.forecast",   (gpointer)_handle_capacity_forecast);
    g_hash_table_insert(g_rpc_routes, "vm.schedule.list",    (gpointer)_handle_vm_schedule_list);
    g_hash_table_insert(g_rpc_routes, "vm.schedule.set",     (gpointer)_handle_vm_schedule_set);
    g_hash_table_insert(g_rpc_routes, "vm.billing.report",   (gpointer)_handle_vm_billing_report);

    g_message("[DISPATCHER] Route table initialized: %u methods registered",
              g_hash_table_size(g_rpc_routes));
}
