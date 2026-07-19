
#include "handler_backup.h"
#include "rpc_utils.h"
#include "../backup/backup_scheduler.h"
#include "../audit/pcv_audit.h"
#include "../../api/uds_server.h"
#include "../../api/ws_server.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_validate.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define BACKUP_HANDLER_LOG "backup_handler"

void handle_backup_policy_set(JsonObject       *params,
                               const gchar      *rpc_id,
                               UdsServer        *server,
                               GSocketConnection *connection)
{
    const gchar *vm_name = NULL;
    gint interval_hours  = 0;
    gint retention_count = 0;

    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");

    if (params && json_object_has_member(params, "interval_hours"))
        interval_hours = (gint)json_object_get_int_member(params, "interval_hours");

    if (params && json_object_has_member(params, "retention_count"))
        retention_count = (gint)json_object_get_int_member(params, "retention_count");

    if (!vm_name || interval_hours < 1 || interval_hours > 8760
        || retention_count < 1 || retention_count > 365) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid params: vm_name required, interval_hours 1~8760, retention_count 1~365");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    if (g_strcmp0(vm_name, "*") != 0 && !pcv_validate_vm_name(vm_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid vm_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    gboolean ok = pcv_backup_policy_set(vm_name, interval_hours,
                                         retention_count, &err);
    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Failed to set policy");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_backup_policy_list(JsonObject       *params __attribute__((unused)),
                                const gchar      *rpc_id,
                                UdsServer        *server,
                                GSocketConnection *connection)
{
    GPtrArray *policies = pcv_backup_policy_list();

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < policies->len; i++) {
        PcvBackupPolicy *p = g_ptr_array_index(policies, i);
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "vm_name", p->vm_name);
        json_object_set_int_member(obj, "interval_hours", p->interval_hours);
        json_object_set_int_member(obj, "retention_count", p->retention_count);
        json_object_set_boolean_member(obj, "enabled", p->enabled);
        json_array_add_object_element(arr, obj);
    }
    g_ptr_array_unref(policies);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_backup_policy_delete(JsonObject       *params,
                                  const gchar      *rpc_id,
                                  UdsServer        *server,
                                  GSocketConnection *connection)
{
    const gchar *vm_name = NULL;
    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");

    if (!vm_name || !pcv_validate_vm_name(vm_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid vm_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    gboolean ok = pcv_backup_policy_delete(vm_name, &err);
    if (!ok) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Failed to delete policy");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, TRUE);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

void handle_backup_history(JsonObject       *params,
                            const gchar      *rpc_id,
                            UdsServer        *server,
                            GSocketConnection *connection)
{
    const gchar *vm_name = NULL;
    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");

    if (!vm_name || !pcv_validate_vm_name(vm_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing or invalid vm_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GPtrArray *snaps = pcv_backup_history(vm_name);

    JsonArray *arr = json_array_new();
    for (guint i = 0; i < snaps->len; i++) {
        json_array_add_string_element(arr,
            (const gchar *)g_ptr_array_index(snaps, i));
    }
    g_ptr_array_unref(snaps);

    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

typedef struct {
    gchar *vm_name;
    gchar *snapshot_name;
} RestoreTaskData;

static void _restore_task_data_free(gpointer p)
{
    RestoreTaskData *d = (RestoreTaskData *)p;
    if (!d) return;
    g_free(d->vm_name);
    g_free(d->snapshot_name);
    g_free(d);
}

static void _restore_worker(GTask        *task,
                              gpointer      source __attribute__((unused)),
                              gpointer      task_data,
                              GCancellable *cancel __attribute__((unused)))
{
    RestoreTaskData *d = (RestoreTaskData *)task_data;
    GError *err = NULL;

    gboolean ok = pcv_backup_restore(d->vm_name, d->snapshot_name, &err);
    gchar *target = g_strdup_printf("%s@%s", d->vm_name, d->snapshot_name);
    gchar *job_id = g_strdup_printf("backup.restore:%s", target);
    if (!ok) {
        const gchar *err_msg = err ? err->message : "unknown";
        PCV_LOG_WARN(BACKUP_HANDLER_LOG,
                     "Async restore failed: %s@%s — %s",
                     d->vm_name, d->snapshot_name, err_msg);
        pcv_audit_log(NULL, "backup.restore", target, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.restore", "failed", err_msg);
        if (err) {
            g_task_return_error(task, err);
        } else {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Backup restore failed");
        }
    } else {
        PCV_LOG_INFO(BACKUP_HANDLER_LOG,
                     "Async restore complete: %s@%s",
                     d->vm_name, d->snapshot_name);
        pcv_audit_log(NULL, "backup.restore", target, "ok", 0, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.restore", "completed", NULL);
        g_task_return_boolean(task, TRUE);
    }
    g_free(job_id);
    g_free(target);
}

void handle_backup_restore(JsonObject       *params,
                            const gchar      *rpc_id,
                            UdsServer        *server,
                            GSocketConnection *connection)
{
    const gchar *vm_name       = NULL;
    const gchar *snapshot_name = NULL;

    if (params && json_object_has_member(params, "vm_name"))
        vm_name = json_object_get_string_member(params, "vm_name");
    if (params && json_object_has_member(params, "snapshot_name"))
        snapshot_name = json_object_get_string_member(params, "snapshot_name");

    if (!vm_name || !snapshot_name) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing params: vm_name, snapshot_name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    if (!pcv_validate_vm_name(vm_name) || !pcv_validate_vm_name(snapshot_name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid vm_name or snapshot_name: only [A-Za-z0-9_-] allowed, max 64 chars");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "vm_name", vm_name);
    json_object_set_string_member(accepted, "snapshot_name", snapshot_name);

    JsonNode *accepted_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(accepted_node, accepted);

    gchar *resp = pure_rpc_build_success_response(rpc_id, accepted_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    RestoreTaskData *d = g_new0(RestoreTaskData, 1);
    d->vm_name       = g_strdup(vm_name);
    d->snapshot_name = g_strdup(snapshot_name);

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)_restore_task_data_free);
    g_task_run_in_thread(task, _restore_worker);
    g_object_unref(task);
}

typedef struct {
    gchar *vm_name;
} IncrementalTaskData;

static void _incremental_task_data_free(gpointer p)
{
    IncrementalTaskData *d = (IncrementalTaskData *)p;
    if (!d) return;
    g_free(d->vm_name);
    g_free(d);
}

static void _incremental_worker(GTask        *task,
                                 gpointer      source __attribute__((unused)),
                                 gpointer      task_data,
                                 GCancellable *cancel __attribute__((unused)))
{
    IncrementalTaskData *d = (IncrementalTaskData *)task_data;
    GError *err = NULL;

    JsonObject *result = pcv_backup_incremental(d->vm_name, &err);
    gboolean ok = (result != NULL);
    gchar *job_id = g_strdup_printf("backup.incremental:%s", d->vm_name);
    if (!ok) {
        const gchar *err_msg = err ? err->message : "unknown";
        PCV_LOG_WARN(BACKUP_HANDLER_LOG,
                     "Async incremental failed: %s — %s", d->vm_name, err_msg);
        pcv_audit_log(NULL, "backup.incremental", d->vm_name, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.incremental", "failed", err_msg);
        if (err) {
            g_task_return_error(task, err);
        } else {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Incremental backup failed");
        }
    } else {
        PCV_LOG_INFO(BACKUP_HANDLER_LOG,
                     "Async incremental complete: %s", d->vm_name);
        pcv_audit_log(NULL, "backup.incremental", d->vm_name, "ok", 0, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.incremental", "completed", NULL);
        json_object_unref(result);
        g_task_return_boolean(task, TRUE);
    }
    g_free(job_id);
}

void handle_backup_incremental(JsonObject       *params,
                                const gchar      *rpc_id,
                                UdsServer        *server,
                                GSocketConnection *connection)
{
    const gchar *name = NULL;
    if (params && json_object_has_member(params, "name"))
        name = json_object_get_string_member(params, "name");

    if (!name || *name == '\0') {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS, "Missing param: name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "vm_name", name);

    JsonNode *accepted_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(accepted_node, accepted);

    gchar *resp = pure_rpc_build_success_response(rpc_id, accepted_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    IncrementalTaskData *d = g_new0(IncrementalTaskData, 1);
    d->vm_name = g_strdup(name);

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)_incremental_task_data_free);
    g_task_run_in_thread(task, _incremental_worker);
    g_object_unref(task);
}

void handle_backup_verify(JsonObject       *params,
                           const gchar      *rpc_id,
                           UdsServer        *server,
                           GSocketConnection *connection)
{
    const gchar *name     = NULL;
    const gchar *snapshot = NULL;

    if (params && json_object_has_member(params, "name"))
        name = json_object_get_string_member(params, "name");
    if (params && json_object_has_member(params, "snapshot"))
        snapshot = json_object_get_string_member(params, "snapshot");

    if (!name || !snapshot) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing params: name, snapshot");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    GError *err = NULL;
    JsonObject *result = pcv_backup_verify(name, snapshot, &err);
    if (!result) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
            err ? err->message : "Verify failed");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        if (err) g_error_free(err);
        return;
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, result);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

typedef struct {
    gchar *vm_name;
    gchar *target_node;
    gchar *ssh_user;
} ReplicateTaskData;

static void _replicate_task_data_free(gpointer p)
{
    ReplicateTaskData *d = (ReplicateTaskData *)p;
    if (!d) return;
    g_free(d->vm_name);
    g_free(d->target_node);
    g_free(d->ssh_user);
    g_free(d);
}

static void _replicate_worker(GTask        *task,
                               gpointer      source __attribute__((unused)),
                               gpointer      task_data,
                               GCancellable *cancel __attribute__((unused)))
{
    ReplicateTaskData *d = (ReplicateTaskData *)task_data;
    GError *err = NULL;

    gboolean ok = pcv_backup_replicate(d->vm_name, d->target_node,
                                        d->ssh_user, &err);
    gchar *target = g_strdup_printf("%s:%s", d->vm_name, d->target_node);
    gchar *job_id = g_strdup_printf("backup.replicate:%s", target);
    if (!ok) {
        const gchar *err_msg = err ? err->message : "unknown";
        PCV_LOG_WARN(BACKUP_HANDLER_LOG,
                     "Async replication failed: %s → %s — %s",
                     d->vm_name, d->target_node, err_msg);
        pcv_audit_log(NULL, "backup.replicate", target, "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.replicate", "failed", err_msg);
        if (err) {
            g_task_return_error(task, err);
        } else {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Backup replication failed");
        }
    } else {
        PCV_LOG_INFO(BACKUP_HANDLER_LOG,
                     "Async replication complete: %s → %s",
                     d->vm_name, d->target_node);
        pcv_audit_log(NULL, "backup.replicate", target, "ok", 0, 0, "local");
        pcv_ws_broadcast_job_complete_mt(job_id, "backup.replicate", "completed", NULL);
        g_task_return_boolean(task, TRUE);
    }
    g_free(job_id);
    g_free(target);
}

void handle_backup_replicate(JsonObject       *params,
                              const gchar      *rpc_id,
                              UdsServer        *server,
                              GSocketConnection *connection)
{
    const gchar *name        = NULL;
    const gchar *target_node = NULL;
    const gchar *ssh_user    = NULL;

    if (params && json_object_has_member(params, "name"))
        name = json_object_get_string_member(params, "name");
    if (params && json_object_has_member(params, "target_node"))
        target_node = json_object_get_string_member(params, "target_node");
    if (params && json_object_has_member(params, "ssh_user"))
        ssh_user = json_object_get_string_member(params, "ssh_user");

    if (!name || !target_node) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Missing params: name, target_node");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (!pcv_validate_vm_name(name)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid param: name");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (!pcv_validate_remote_host(target_node)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid param: target_node");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }
    if (ssh_user && *ssh_user && !pcv_validate_ssh_user(ssh_user)) {
        gchar *resp = pure_rpc_build_error_response(
            rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
            "Invalid param: ssh_user");
        pure_uds_server_send_response(server, connection, resp);
        g_free(resp);
        return;
    }

    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "vm_name", name);
    json_object_set_string_member(accepted, "target_node", target_node);

    JsonNode *accepted_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(accepted_node, accepted);

    gchar *resp = pure_rpc_build_success_response(rpc_id, accepted_node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);

    ReplicateTaskData *d = g_new0(ReplicateTaskData, 1);
    d->vm_name     = g_strdup(name);
    d->target_node = g_strdup(target_node);
    d->ssh_user    = g_strdup(ssh_user ? ssh_user : "");

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)_replicate_task_data_free);
    g_task_run_in_thread(task, _replicate_worker);
    g_object_unref(task);
}

void handle_snapshot_schedule_set(JsonObject       *params,
                                  const gchar      *rpc_id,
                                  UdsServer        *server,
                                  GSocketConnection *connection)
{
    handle_backup_policy_set(params, rpc_id, server, connection);
}

void handle_snapshot_schedule_list(JsonObject       *params,
                                   const gchar      *rpc_id,
                                   UdsServer        *server,
                                   GSocketConnection *connection)
{
    handle_backup_policy_list(params, rpc_id, server, connection);
}

void handle_snapshot_schedule_delete(JsonObject       *params,
                                     const gchar      *rpc_id,
                                     UdsServer        *server,
                                     GSocketConnection *connection)
{
    handle_backup_policy_delete(params, rpc_id, server, connection);
}
