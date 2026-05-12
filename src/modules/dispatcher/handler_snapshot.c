





































#include "handler_snapshot.h"
#include "rpc_utils.h"
#include "../storage/zfs_driver.h"
#include "../../api/uds_server.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_spawn.h"
#include "../../utils/pcv_config.h"
#include "../../modules/virt/virt_conn_pool.h"
#include "../audit/pcv_audit.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <string.h>

#define SNAP_LOG_DOM "snapshot"




static gboolean _zfs_dataset_exists(const gchar *vm_id) {
    gchar *dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), vm_id);
    const gchar *argv[] = {"zfs", "list", "-H", "-o", "name", dataset, NULL};
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, NULL);
    g_free(dataset);
    return ok;
}


static void _libvirt_snapshot_create(const gchar *vm_id, const gchar *snap_name,
                                      const gchar *rpc_id, UdsServer *server,
                                      GSocketConnection *connection)
{
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, "Hypervisor connection failed");
        pure_uds_server_send_response(server, connection, e); g_free(e); return;
    }
    virDomainPtr dom = virDomainLookupByName(conn, vm_id);
    if (!dom) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, "VM not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virt_conn_pool_release(conn); return;
    }
    gchar *xml = g_strdup_printf(
        "<domainsnapshot><name>%s</name><description>PureCVisor snapshot</description></domainsnapshot>",
        snap_name);
    virDomainSnapshotPtr snap = virDomainSnapshotCreateXML(dom, xml, 0);
    g_free(xml);

    if (snap) {
        pcv_audit_log(NULL, "vm.snapshot.create", vm_id, "ok", 0, 0, "local");
        virDomainSnapshotFree(snap);
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        virErrorPtr verr = virGetLastError();
        pcv_audit_log(NULL, "vm.snapshot.create", vm_id, "fail", -32000, 0, "local");
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000,
            verr ? verr->message : "libvirt snapshot creation failed");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    }
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}


static void _libvirt_snapshot_list(const gchar *vm_id, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, "Hypervisor connection failed");
        pure_uds_server_send_response(server, connection, e); g_free(e); return;
    }
    virDomainPtr dom = virDomainLookupByName(conn, vm_id);
    if (!dom) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, "VM not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virt_conn_pool_release(conn); return;
    }

    char **names = NULL;
    int count = virDomainSnapshotNum(dom, 0);
    JsonArray *arr = json_array_new();

    if (count > 0) {
        names = g_new0(char *, count);
        count = virDomainSnapshotListNames(dom, names, count, 0);
        for (int i = 0; i < count; i++) {
            JsonObject *entry = json_object_new();
            json_object_set_string_member(entry, "snapshot", names[i]);
            virDomainSnapshotPtr snap = virDomainSnapshotLookupByName(dom, names[i], 0);
            if (snap) {
                gchar *xml = virDomainSnapshotGetXMLDesc(snap, 0);
                if (xml) {

                    const gchar *ct = strstr(xml, "<creationTime>");
                    if (ct) {
                        gint64 epoch = g_ascii_strtoll(ct + 14, NULL, 10);
                        GDateTime *dt = g_date_time_new_from_unix_local(epoch);
                        if (dt) {
                            gchar *ts = g_date_time_format(dt, "%Y-%m-%d %H:%M");
                            json_object_set_string_member(entry, "creation", ts);
                            g_free(ts);
                            g_date_time_unref(dt);
                        }
                    }
                    free(xml);
                }
                virDomainSnapshotFree(snap);
            }
            json_array_add_object_element(arr, entry);
            free(names[i]);
        }
        g_free(names);
    }

    pcv_audit_log(NULL, "vm.snapshot.list", vm_id, "ok", 0, 0, "local");

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp); g_free(resp);

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}


static void _libvirt_snapshot_delete(const gchar *vm_id, const gchar *snap_name,
                                      const gchar *rpc_id, UdsServer *server,
                                      GSocketConnection *connection)
{
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, "Hypervisor connection failed");
        pure_uds_server_send_response(server, connection, e); g_free(e); return;
    }
    virDomainPtr dom = virDomainLookupByName(conn, vm_id);
    if (!dom) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, "VM not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virt_conn_pool_release(conn); return;
    }
    virDomainSnapshotPtr snap = virDomainSnapshotLookupByName(dom, snap_name, 0);
    if (!snap) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, "Snapshot not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virDomainFree(dom); virt_conn_pool_release(conn); return;
    }
    int rc = virDomainSnapshotDelete(snap, 0);
    virDomainSnapshotFree(snap);

    if (rc == 0) {
        pcv_audit_log(NULL, "vm.snapshot.delete", vm_id, "ok", 0, 0, "local");
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        pcv_audit_log(NULL, "vm.snapshot.delete", vm_id, "fail", -32000, 0, "local");
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, "Failed to delete snapshot");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    }
    virDomainFree(dom);
    virt_conn_pool_release(conn);
}


static void _libvirt_snapshot_rollback(const gchar *vm_id, const gchar *snap_name,
                                        const gchar *rpc_id, UdsServer *server,
                                        GSocketConnection *connection)
{
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, "Hypervisor connection failed");
        pure_uds_server_send_response(server, connection, e); g_free(e); return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, vm_id);
    if (!dom) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, "VM not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virt_conn_pool_release(conn); return;
    }

    int state = 0, reason = 0;
    gboolean was_running = FALSE;
    virDomainGetState(dom, &state, &reason, 0);
    was_running = (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED);

    if (was_running) {
        virDomainShutdown(dom);
        for (int i = 0; i < 50; i++) {
            g_usleep(100 * 1000);
            virDomainGetState(dom, &state, &reason, 0);
            if (state != VIR_DOMAIN_RUNNING &&
                state != VIR_DOMAIN_PAUSED) break;
        }

        virDomainGetState(dom, &state, &reason, 0);
        if (state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_PAUSED)
            virDomainDestroy(dom);
    }

    virDomainSnapshotPtr snap = virDomainSnapshotLookupByName(dom, snap_name, 0);
    if (!snap) {
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000, "Snapshot not found");
        pure_uds_server_send_response(server, connection, e); g_free(e);
        virDomainFree(dom); virt_conn_pool_release(conn); return;
    }

    int rc = virDomainRevertToSnapshot(snap, 0);
    virDomainSnapshotFree(snap);

    if (rc == 0 && was_running) {
        if (virDomainCreate(dom) != 0) {
            virErrorPtr verr = virGetLastError();
            PCV_LOG_WARN(SNAP_LOG_DOM,
                         "libvirt rollback restart failed for '%s': %s",
                         vm_id, verr ? verr->message : "unknown");
        }
    }

    if (rc == 0) {
        pcv_audit_log(NULL, "vm.snapshot.rollback", vm_id, "ok", 0, 0, "local");
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        gchar *resp = pure_rpc_build_success_response(rpc_id, node);
        pure_uds_server_send_response(server, connection, resp); g_free(resp);
    } else {
        virErrorPtr verr = virGetLastError();
        pcv_audit_log(NULL, "vm.snapshot.rollback", vm_id, "fail", -32000, 0, "local");
        gchar *e = pure_rpc_build_error_response(rpc_id, -32000,
            verr ? verr->message : "Failed to rollback snapshot");
        pure_uds_server_send_response(server, connection, e); g_free(e);
    }

    virDomainFree(dom);
    virt_conn_pool_release(conn);
}


static gboolean pcv_validate_zfs_token(const gchar *s) {
    if (!s || *s == '\0' || strlen(s) > 128) return FALSE;
    for (const gchar *p = s; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
            return FALSE;
    }
    return TRUE;
}


static const gchar *_get_param(JsonObject *params,
                                const gchar *primary,
                                const gchar *fallback)
{
    if (json_object_has_member(params, primary))
        return json_object_get_string_member(params, primary);
    if (fallback && json_object_has_member(params, fallback))
        return json_object_get_string_member(params, fallback);
    return NULL;
}

#define VALIDATE_SNAPSHOT_PARAMS(params, rpc_id, server, conn)                \
    do {                                                                        \
        const gchar *_vid = _get_param(params, "name", "vm_id");               \
        const gchar *_sn  = _get_param(params, "snapshot_name", "snap_name");  \
        if (!_vid || !_sn) {                                                    \
            gchar *_e = pure_rpc_build_error_response(                          \
                rpc_id, PURE_RPC_ERR_INVALID_PARAMS,                            \
                "Missing vm name or snapshot_name");                            \
            pure_uds_server_send_response(server, conn, _e);                    \
            g_free(_e); return;                                                 \
        }                                                                       \
        if (!pcv_validate_zfs_token(_vid) || !pcv_validate_zfs_token(_sn)) {   \
            gchar *_e = pure_rpc_build_error_response(                          \
                rpc_id, PURE_RPC_ERR_INVALID_PARAMS,                            \
                "Invalid characters in vm name or snapshot_name");              \
            pure_uds_server_send_response(server, conn, _e);                    \
            g_free(_e); return;                                                 \
        }                                                                       \
    } while (0)

#define VALIDATE_VM_ID_PARAM(params, rpc_id, server, conn)                     \
    do {                                                                        \
        const gchar *_vid = _get_param(params, "name", "vm_id");               \
        if (!_vid || !pcv_validate_zfs_token(_vid)) {                           \
            gchar *_e = pure_rpc_build_error_response(                          \
                rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing or invalid name");\
            pure_uds_server_send_response(server, conn, _e);                    \
            g_free(_e); return;                                                 \
        }                                                                       \
    } while (0)












typedef struct {
    gchar *vm_name;
    gchar *snap_name;

    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
} RollbackTaskData;

static void _rollback_task_data_free(gpointer p)
{
    RollbackTaskData *d = (RollbackTaskData *)p;
    g_free(d->vm_name);
    g_free(d->snap_name);
    g_free(d->rpc_id);
    if (d->server)     g_object_unref(d->server);
    if (d->connection) g_object_unref(d->connection);
    g_free(d);
}

static void
_rollback_worker(GTask        *task,
                 gpointer      source __attribute__((unused)),
                 gpointer      task_data,
                 GCancellable *cancel __attribute__((unused)))
{
    RollbackTaskData *d = (RollbackTaskData *)task_data;
    GError *err = NULL;


    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to acquire libvirt connection");
        return;
    }

    virDomainPtr dom = virDomainLookupByName(conn, d->vm_name);
    gboolean was_running = FALSE;

    if (dom) {

        int state = 0, reason = 0;
        virDomainGetState(dom, &state, &reason, 0);
        was_running = (state == VIR_DOMAIN_RUNNING ||
                       state == VIR_DOMAIN_PAUSED);

        if (was_running) {
            PCV_LOG_INFO(SNAP_LOG_DOM,
                         "Rollback: shutting down VM '%s' before ZFS rollback",
                         d->vm_name);


            virDomainShutdown(dom);
            for (int i = 0; i < 50; i++) {
                g_usleep(100 * 1000);
                virDomainGetState(dom, &state, &reason, 0);
                if (state != VIR_DOMAIN_RUNNING &&
                    state != VIR_DOMAIN_PAUSED) break;
            }


            virDomainGetState(dom, &state, &reason, 0);
            if (state == VIR_DOMAIN_RUNNING ||
                state == VIR_DOMAIN_PAUSED) {
                PCV_LOG_WARN(SNAP_LOG_DOM,
                             "Rollback: graceful shutdown timeout — "
                             "force-destroying VM '%s'", d->vm_name);
                virDomainDestroy(dom);
            }
        }
        virDomainFree(dom);
    }



    gchar *dataset = g_strdup_printf("%s/%s@%s",
                                      pcv_config_get_zvol_pool(), d->vm_name, d->snap_name);
    PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS rollback: %s", dataset);

    const gchar *zfs_argv[] = {"zfs", "rollback", "-r", dataset, NULL};
    gchar *stderr_buf = NULL;
    gboolean zfs_ok = pcv_spawn_sync(zfs_argv, NULL, &stderr_buf, &err);
    g_free(dataset);

    if (!zfs_ok) {
        const gchar *errmsg = err ? err->message
                            : (stderr_buf ? stderr_buf : "ZFS rollback failed");
        PCV_LOG_WARN(SNAP_LOG_DOM, "ZFS rollback failed: %s", errmsg);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "%s", errmsg);
        g_free(stderr_buf);
        if (err) g_error_free(err);
        virt_conn_pool_release(conn);
        return;
    }
    g_free(stderr_buf);
    PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS rollback complete for '%s'", d->vm_name);


    if (was_running) {
        dom = virDomainLookupByName(conn, d->vm_name);
        if (dom) {
            PCV_LOG_INFO(SNAP_LOG_DOM,
                         "Rollback: restarting VM '%s'", d->vm_name);
            if (virDomainCreate(dom) != 0) {
                virErrorPtr ve = virGetLastError();
                PCV_LOG_WARN(SNAP_LOG_DOM,
                             "Rollback: VM restart failed: %s",
                             ve ? ve->message : "unknown");

            }
            virDomainFree(dom);
        }
    }

    virt_conn_pool_release(conn);
    g_task_return_boolean(task, TRUE);
}

static void
_on_rollback_done(GObject *src __attribute__((unused)),
                  GAsyncResult *res,
                  gpointer user_data)
{
    RollbackTaskData *d = (RollbackTaskData *)user_data;
    GError *err = NULL;
    gchar  *resp;
    gboolean _ok = g_task_propagate_boolean(G_TASK(res), &err);

    {
        gchar *_target = g_strdup_printf("%s:%s", d->vm_name ?: "?", d->snap_name ?: "?");
        pcv_audit_log(NULL, "vm.snapshot.rollback", _target,
                      _ok ? "ok" : "fail",
                      _ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        g_free(_target);
    }
    if (_ok) {
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        resp = pure_rpc_build_success_response(d->rpc_id, node);
        PCV_LOG_INFO(SNAP_LOG_DOM,
                     "Snapshot rollback OK: %s@%s",
                     d->vm_name, d->snap_name);
    } else {
        resp = pure_rpc_build_error_response(d->rpc_id,
                   PURE_RPC_ERR_ZFS_OPERATION,
                   err ? err->message : "Rollback failed");
        if (err) g_error_free(err);
    }

    pure_uds_server_send_response(d->server, d->connection, resp);
    g_free(resp);


}










typedef struct {
    gchar    *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;
    GPtrArray *argv;
    gboolean  capture_stdout;
    gchar    *audit_method;
    gchar    *audit_target;

    gboolean  ok;
    gchar    *stdout_buf;
    gchar    *err_msg;
} SnapSyncCtx;

static void _snap_sync_ctx_free(gpointer p) {
    SnapSyncCtx *c = p;
    g_free(c->rpc_id); g_free(c->audit_method); g_free(c->audit_target);
    g_free(c->stdout_buf); g_free(c->err_msg);
    if (c->argv) g_ptr_array_unref(c->argv);
    if (c->server) g_object_unref(c->server);
    if (c->connection) g_object_unref(c->connection);
    g_free(c);
}

static void
_snap_sync_worker(GTask *task, gpointer src __attribute__((unused)),
                  gpointer task_data, GCancellable *cancel __attribute__((unused)))
{
    SnapSyncCtx *c = task_data;
    GError *error = NULL;
    gchar *stderr_buf = NULL;

    c->ok = pcv_spawn_sync((const gchar * const *)c->argv->pdata,
                            c->capture_stdout ? &c->stdout_buf : NULL,
                            &stderr_buf, &error);



    if (c->audit_method) {
        pcv_audit_log(NULL, c->audit_method, c->audit_target ?: "",
                      c->ok ? "ok" : "fail",
                      c->ok ? 0 : PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    }
    if (!c->ok) {
        c->err_msg = g_strdup(error ? error->message
                              : (stderr_buf ? stderr_buf : "Unknown ZFS error"));
    }
    g_free(stderr_buf);
    if (error) g_error_free(error);
    g_task_return_boolean(task, TRUE);
}

static void
_snap_sync_callback(GObject *src __attribute__((unused)),
                    GAsyncResult *res __attribute__((unused)),
                    gpointer user_data)
{
    SnapSyncCtx *c = user_data;
    gchar *resp;

    if (!c->ok) {
        resp = pure_rpc_build_error_response(c->rpc_id,
                   PURE_RPC_ERR_ZFS_OPERATION, c->err_msg);
    } else if (c->capture_stdout && c->stdout_buf) {

        JsonArray *arr  = json_array_new();
        gchar    **lines = g_strsplit(g_strstrip(c->stdout_buf), "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (!**l) continue;
            gchar **cols = g_strsplit(*l, "\t", -1);
            guint ncols = g_strv_length(cols);
            if (ncols >= 2) {

                JsonObject *entry = json_object_new();
                json_object_set_string_member(entry, "snapshot", cols[0]);
                json_object_set_string_member(entry, "creation", cols[1]);
                json_array_add_object_element(arr, entry);
            } else if (ncols == 1) {

                JsonObject *entry = json_object_new();
                json_object_set_string_member(entry, "snapshot", cols[0]);
                json_array_add_object_element(arr, entry);
            }
            g_strfreev(cols);
        }
        g_strfreev(lines);

        JsonNode *node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(node, arr);
        resp = pure_rpc_build_success_response(c->rpc_id, node);
    } else {
        JsonNode *node = json_node_new(JSON_NODE_VALUE);
        json_node_set_boolean(node, TRUE);
        resp = pure_rpc_build_success_response(c->rpc_id, node);
    }

    pure_uds_server_send_response(c->server, c->connection, resp);
    g_free(resp);

}


static GPtrArray *_argv_new(void) {
    return g_ptr_array_new_with_free_func(g_free);
}
static void _argv_add(GPtrArray *a, const gchar *s) {
    g_ptr_array_add(a, g_strdup(s));
}
static void _argv_finish(GPtrArray *a) {
    g_ptr_array_add(a, NULL);
}

static void _snap_sync_dispatch(SnapSyncCtx *c) {
    GTask *task = g_task_new(NULL, NULL, _snap_sync_callback, c);
    g_task_set_task_data(task, c, _snap_sync_ctx_free);
    g_task_run_in_thread(task, _snap_sync_worker);
    g_object_unref(task);
}





void handle_vm_snapshot_create(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection)
{
    VALIDATE_SNAPSHOT_PARAMS(params, rpc_id, server, connection);
    const gchar *vm_id     = _get_param(params, "name", "vm_id");
    const gchar *snap_name = _get_param(params, "snapshot_name", "snap_name");


    if (!_zfs_dataset_exists(vm_id)) {
        PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS dataset not found for '%s', using libvirt snapshot", vm_id);
        _libvirt_snapshot_create(vm_id, snap_name, rpc_id, server, connection);
        return;
    }


    {
        gchar *dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), vm_id);
        if (!purecvisor_zfs_check_snapshot_quota(dataset, 50)) {
            g_free(dataset);
            gchar *e = pure_rpc_build_error_response(rpc_id,
                PURE_RPC_ERR_ZFS_OPERATION,
                "Snapshot quota exceeded: maximum 50 snapshots per VM");
            pure_uds_server_send_response(server, connection, e);
            g_free(e);
            return;
        }
        g_free(dataset);
    }

    SnapSyncCtx *c = g_new0(SnapSyncCtx, 1);
    c->rpc_id       = g_strdup(rpc_id);
    c->server       = g_object_ref(server);
    c->connection   = g_object_ref(connection);
    c->capture_stdout = FALSE;
    c->audit_method = g_strdup("vm.snapshot.create");
    c->audit_target = g_strdup_printf("%s:%s", vm_id, snap_name);
    c->argv = _argv_new();
    _argv_add(c->argv, "zfs"); _argv_add(c->argv, "snapshot");
    gchar *ds = g_strdup_printf("%s/%s@%s", pcv_config_get_zvol_pool(), vm_id, snap_name);
    _argv_add(c->argv, ds); g_free(ds);
    _argv_finish(c->argv);

    _snap_sync_dispatch(c);
}

void handle_vm_snapshot_list(JsonObject *params, const gchar *rpc_id,
                              UdsServer *server, GSocketConnection *connection)
{
    VALIDATE_VM_ID_PARAM(params, rpc_id, server, connection);
    const gchar *vm_id = _get_param(params, "name", "vm_id");


    if (!_zfs_dataset_exists(vm_id)) {
        PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS dataset not found for '%s', using libvirt snapshot list", vm_id);
        _libvirt_snapshot_list(vm_id, rpc_id, server, connection);
        return;
    }

    SnapSyncCtx *c = g_new0(SnapSyncCtx, 1);
    c->rpc_id       = g_strdup(rpc_id);
    c->server       = g_object_ref(server);
    c->connection   = g_object_ref(connection);
    c->capture_stdout = TRUE;
    c->audit_method = g_strdup("vm.snapshot.list");
    c->audit_target = g_strdup(vm_id);
    c->argv = _argv_new();
    _argv_add(c->argv, "zfs"); _argv_add(c->argv, "list");
    _argv_add(c->argv, "-H"); _argv_add(c->argv, "-o");
    _argv_add(c->argv, "name,creation");
    _argv_add(c->argv, "-t"); _argv_add(c->argv, "snapshot");
    _argv_add(c->argv, "-r");
    gchar *ds = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), vm_id);
    _argv_add(c->argv, ds); g_free(ds);
    _argv_finish(c->argv);

    _snap_sync_dispatch(c);
}


void handle_vm_snapshot_rollback(JsonObject *params, const gchar *rpc_id,
                                  UdsServer *server, GSocketConnection *connection)
{
    VALIDATE_SNAPSHOT_PARAMS(params, rpc_id, server, connection);
    const gchar *vm_name   = _get_param(params, "name", "vm_id");
    const gchar *snap_name = _get_param(params, "snapshot_name", "snap_name");


    if (!_zfs_dataset_exists(vm_name)) {
        PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS dataset not found for '%s', using libvirt snapshot rollback", vm_name);
        _libvirt_snapshot_rollback(vm_name, snap_name, rpc_id, server, connection);
        return;
    }

    RollbackTaskData *d = g_new0(RollbackTaskData, 1);
    d->vm_name   = g_strdup(vm_name);
    d->snap_name = g_strdup(snap_name);
    d->rpc_id    = g_strdup(rpc_id);
    d->server    = g_object_ref(server);
    d->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _on_rollback_done, d);
    g_task_set_task_data(task, d, _rollback_task_data_free);
    g_task_run_in_thread(task, _rollback_worker);
    g_object_unref(task);
}

void handle_vm_snapshot_delete(JsonObject *params, const gchar *rpc_id,
                                UdsServer *server, GSocketConnection *connection)
{
    VALIDATE_SNAPSHOT_PARAMS(params, rpc_id, server, connection);
    const gchar *vm_id     = _get_param(params, "name", "vm_id");
    const gchar *snap_name = _get_param(params, "snapshot_name", "snap_name");


    if (!_zfs_dataset_exists(vm_id)) {
        PCV_LOG_INFO(SNAP_LOG_DOM, "ZFS dataset not found for '%s', using libvirt snapshot delete", vm_id);
        _libvirt_snapshot_delete(vm_id, snap_name, rpc_id, server, connection);
        return;
    }

    SnapSyncCtx *c = g_new0(SnapSyncCtx, 1);
    c->rpc_id       = g_strdup(rpc_id);
    c->server       = g_object_ref(server);
    c->connection   = g_object_ref(connection);
    c->capture_stdout = FALSE;
    c->audit_method = g_strdup("vm.snapshot.delete");
    c->audit_target = g_strdup_printf("%s:%s", vm_id, snap_name);
    c->argv = _argv_new();
    _argv_add(c->argv, "zfs"); _argv_add(c->argv, "destroy");
    gchar *ds = g_strdup_printf("%s/%s@%s", pcv_config_get_zvol_pool(), vm_id, snap_name);
    _argv_add(c->argv, ds); g_free(ds);
    _argv_finish(c->argv);

    _snap_sync_dispatch(c);
}




typedef struct {
    gchar *vm_id;
    gchar *prefix;
    gint   keep;
    gchar *rpc_id;
    UdsServer *server;
    GSocketConnection *connection;

    gint   total;
    gint   to_delete;
    gint   del_count;
    gchar *err_msg;
} DeleteAllCtx;

static void _delete_all_ctx_free(gpointer p) {
    DeleteAllCtx *d = p;
    g_free(d->vm_id); g_free(d->prefix); g_free(d->rpc_id); g_free(d->err_msg);
    if (d->server) g_object_unref(d->server);
    if (d->connection) g_object_unref(d->connection);
    g_free(d);
}

static void
_delete_all_worker(GTask *task, gpointer src __attribute__((unused)),
                   gpointer task_data, GCancellable *cancel __attribute__((unused)))
{
    DeleteAllCtx *d = task_data;


    gchar *dataset = g_strdup_printf("%s/%s", pcv_config_get_zvol_pool(), d->vm_id);
    const gchar *list_argv[] = {"zfs", "list", "-H", "-o", "name",
                                 "-t", "snapshot", "-s", "creation", "-r", dataset, NULL};
    gchar *out = NULL, *err_out = NULL;
    GError *err = NULL;
    if (!pcv_spawn_sync(list_argv, &out, &err_out, &err)) {
        d->err_msg = g_strdup(err ? err->message : "Failed to list snapshots");
        g_free(dataset); g_free(out); g_free(err_out);
        if (err) g_error_free(err);
        g_task_return_boolean(task, FALSE);
        return;
    }
    g_free(dataset);


    gchar **lines = g_strsplit(out, "\n", -1);
    g_free(out); g_free(err_out);
    GPtrArray *targets = g_ptr_array_new_with_free_func(g_free);
    for (gint i = 0; lines[i]; i++) {
        if (!lines[i][0]) continue;
        if (d->prefix && d->prefix[0]) {
            const gchar *at = strrchr(lines[i], '@');
            if (!at || !g_str_has_prefix(at + 1, d->prefix)) continue;
        }
        g_ptr_array_add(targets, g_strdup(lines[i]));
    }
    g_strfreev(lines);


    d->total = (gint)targets->len;
    d->to_delete = (d->keep > 0) ? (d->total > d->keep ? d->total - d->keep : 0) : d->total;
    d->del_count = 0;
    for (gint i = 0; i < d->to_delete && i < d->total; i++) {
        const gchar *snap = g_ptr_array_index(targets, i);
        const gchar *del_argv[] = {"zfs", "destroy", snap, NULL};
        gchar *del_stderr = NULL; GError *del_err = NULL;
        if (pcv_spawn_sync(del_argv, NULL, &del_stderr, &del_err)) {
            d->del_count++;
        } else {

            PCV_LOG_WARN(SNAP_LOG_DOM, "zfs destroy failed: %s — %s",
                         snap, del_err ? del_err->message
                                       : (del_stderr ?: "unknown"));
        }
        g_free(del_stderr);
        if (del_err) g_error_free(del_err);
    }
    g_ptr_array_unref(targets);
    g_task_return_boolean(task, TRUE);
}

static void
_delete_all_callback(GObject *src __attribute__((unused)),
                     GAsyncResult *res, gpointer user_data)
{
    DeleteAllCtx *d = user_data;
    GError *err = NULL;
    gboolean ok = g_task_propagate_boolean(G_TASK(res), &err);

    if (!ok || d->err_msg) {
        gchar *resp = pure_rpc_build_error_response(d->rpc_id, -32000,
            d->err_msg ? d->err_msg : (err ? err->message : "delete_all failed"));
        pure_uds_server_send_response(d->server, d->connection, resp);
        g_free(resp);
        pcv_audit_log(NULL, "vm.snapshot.delete_all", d->vm_id, "fail",
                      PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        if (err) g_error_free(err);
        return;
    }


    JsonObject *obj = json_object_new();
    json_object_set_int_member(obj, "deleted", d->del_count);
    json_object_set_int_member(obj, "total_before", d->total);
    json_object_set_int_member(obj, "remaining", d->total - d->del_count);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(d->rpc_id, node);
    pure_uds_server_send_response(d->server, d->connection, resp);
    g_free(resp);


    const gchar *audit_result;
    gint audit_code;
    if (d->to_delete == 0 || d->del_count == d->to_delete) {
        audit_result = "ok"; audit_code = 0;
    } else if (d->del_count == 0) {
        audit_result = "fail"; audit_code = PURE_RPC_ERR_ZFS_OPERATION;
    } else {
        audit_result = "partial_fail"; audit_code = PURE_RPC_ERR_ZFS_OPERATION;
    }
    pcv_audit_log(NULL, "vm.snapshot.delete_all", d->vm_id,
                  audit_result, audit_code, 0, "local");

    PCV_LOG_INFO(SNAP_LOG_DOM, "Bulk delete: vm=%s prefix=%s keep=%d deleted=%d/%d",
                 d->vm_id, d->prefix ?: "*", d->keep, d->del_count, d->total);
}

void handle_vm_snapshot_delete_all(JsonObject *params, const gchar *rpc_id,
                                    UdsServer *server, GSocketConnection *connection)
{
    VALIDATE_VM_ID_PARAM(params, rpc_id, server, connection);
    const gchar *vm_id = _get_param(params, "name", "vm_id");
    const gchar *prefix = json_object_has_member(params, "prefix")
        ? json_object_get_string_member(params, "prefix") : NULL;
    gint keep = json_object_has_member(params, "keep_recent")
        ? (gint)json_object_get_int_member(params, "keep_recent") : 0;



    {
        virConnectPtr qconn = virt_conn_pool_acquire();
        if (qconn) {
            virDomainPtr dom = virDomainLookupByName(qconn, vm_id);
            if (!dom) {
                virt_conn_pool_release(qconn);
                gchar *err = pure_rpc_build_error_response(rpc_id, -32000,
                    "VM not found — cannot bulk delete snapshots for non-existent VM");
                pure_uds_server_send_response(server, connection, err);
                g_free(err);
                return;
            }
            virDomainFree(dom);
            virt_conn_pool_release(qconn);
        }
    }

    DeleteAllCtx *d = g_new0(DeleteAllCtx, 1);
    d->vm_id = g_strdup(vm_id);
    d->prefix = g_strdup(prefix);
    d->keep = keep;
    d->rpc_id = g_strdup(rpc_id);
    d->server = g_object_ref(server);
    d->connection = g_object_ref(connection);

    GTask *task = g_task_new(NULL, NULL, _delete_all_callback, d);
    g_task_set_task_data(task, d, _delete_all_ctx_free);
    g_task_run_in_thread(task, _delete_all_worker);
    g_object_unref(task);
}
