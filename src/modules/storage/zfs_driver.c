// src/modules/storage/zfs_driver.c 내부 스냅샷 관련 함수들 교체

#include "zfs_driver.h"
#include <string.h>

static void 
on_zfs_command_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) 
{
    GSubprocess *proc = G_SUBPROCESS(source_object);
    GTask *task = G_TASK(user_data);
    GError *error = NULL;

    if (!g_subprocess_wait_check_finish(proc, res, &error)) {
        g_task_return_error(task, error);
    } else {
        g_task_return_boolean(task, TRUE);
    }
    g_object_unref(task);
}

// 1. Create
void purecvisor_zfs_snapshot_create_async(const gchar *pool_name, const gchar *vm_name, const gchar *snap_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) 
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    GError *error = NULL;
    gchar *target = g_strdup_printf("%s/vms/%s@%s", pool_name, vm_name, snap_name);
    const gchar *argv[] = {"zfs", "snapshot", target, NULL};

    GSubprocess *proc = g_subprocess_newv((const gchar * const *)argv, G_SUBPROCESS_FLAGS_NONE, &error);
    g_free(target);
    if (error) { g_task_return_error(task, error); g_object_unref(task); return; }
    g_subprocess_wait_check_async(proc, cancellable, on_zfs_command_ready, task);
    g_object_unref(proc);
}

gboolean purecvisor_zfs_snapshot_create_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

// 2. Rollback
void purecvisor_zfs_snapshot_rollback_async(const gchar *pool_name, const gchar *vm_name, const gchar *snap_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) 
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    GError *error = NULL;
    gchar *target = g_strdup_printf("%s/vms/%s@%s", pool_name, vm_name, snap_name);
    const gchar *argv[] = {"zfs", "rollback", "-r", target, NULL}; // -r: 강제 롤백(중간 스냅샷 무시)

    GSubprocess *proc = g_subprocess_newv((const gchar * const *)argv, G_SUBPROCESS_FLAGS_NONE, &error);
    g_free(target);
    if (error) { g_task_return_error(task, error); g_object_unref(task); return; }
    g_subprocess_wait_check_async(proc, cancellable, on_zfs_command_ready, task);
    g_object_unref(proc);
}

gboolean purecvisor_zfs_snapshot_rollback_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

// 3. Delete
void purecvisor_zfs_snapshot_delete_async(const gchar *pool_name, const gchar *vm_name, const gchar *snap_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) 
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    GError *error = NULL;
    gchar *target = g_strdup_printf("%s/vms/%s@%s", pool_name, vm_name, snap_name);
    const gchar *argv[] = {"zfs", "destroy", target, NULL};

    GSubprocess *proc = g_subprocess_newv((const gchar * const *)argv, G_SUBPROCESS_FLAGS_NONE, &error);
    g_free(target);
    if (error) { g_task_return_error(task, error); g_object_unref(task); return; }
    g_subprocess_wait_check_async(proc, cancellable, on_zfs_command_ready, task);
    g_object_unref(proc);
}

gboolean purecvisor_zfs_snapshot_delete_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_boolean(G_TASK(res), error);
}

// 4. List 
static void on_zfs_list_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GSubprocess *proc = G_SUBPROCESS(source_object);
    GTask *task = G_TASK(user_data);
    GError *error = NULL;
    gchar *stdout_buf = NULL;
    if (!g_subprocess_communicate_utf8_finish(proc, res, &stdout_buf, NULL, &error)) {
        g_task_return_error(task, error);
        g_free(stdout_buf); g_object_unref(task); return;
    }
    GPtrArray *snapshots = g_ptr_array_new_with_free_func(g_free);
    if (g_subprocess_get_successful(proc) && stdout_buf) {
        gchar **lines = g_strsplit(stdout_buf, "\n", -1);
        for (gint i = 0; lines[i] != NULL; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (strlen(line) > 0) {
                gchar *at_sign = g_strrstr(line, "@");
                if (at_sign && *(at_sign + 1) != '\0') g_ptr_array_add(snapshots, g_strdup(at_sign + 1));
            }
        }
        g_strfreev(lines);
    }
    g_task_return_pointer(task, snapshots, (GDestroyNotify)g_ptr_array_unref);
    g_free(stdout_buf); g_object_unref(task);
}

void purecvisor_zfs_snapshot_list_async(const gchar *pool_name, const gchar *vm_name, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) 
{
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    GError *error = NULL;
    gchar *target = g_strdup_printf("%s/vms/%s", pool_name, vm_name);
    const gchar *argv[] = {"zfs", "list", "-t", "snapshot", "-H", "-o", "name", target, NULL};

    GSubprocess *proc = g_subprocess_newv((const gchar * const *)argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error);
    g_free(target);
    if (error) { g_task_return_error(task, error); g_object_unref(task); return; }
    g_subprocess_communicate_utf8_async(proc, NULL, cancellable, on_zfs_list_ready, task);
    g_object_unref(proc);
}

GPtrArray* purecvisor_zfs_snapshot_list_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_pointer(G_TASK(res), error);
}

/* ========================================================================= */
/* Phase 5: ZFS Volume Provisioning (복구된 영역)                            */
/* ========================================================================= */

gboolean 
purecvisor_zfs_create_volume(const gchar *pool_name, const gchar *vm_name, const gchar *size_str, GError **error) 
{
    gboolean success;
    gchar *target_dataset = g_strdup_printf("%s/%s", pool_name, vm_name);
    
    // 명령어: zfs create -V <size> <pool>/vms/<vm_name>
    const gchar *argv[] = {"zfs", "create", "-V", size_str, target_dataset, NULL};

    // 스레드 내부이므로 g_spawn_sync를 사용하여 동기식으로 안전하게 실행
    success = g_spawn_sync(NULL, (gchar **)argv, NULL, 
                           G_SPAWN_SEARCH_PATH, 
                           NULL, NULL, NULL, NULL, NULL, error);
    
    g_free(target_dataset);
    return success;
}

gboolean 
purecvisor_zfs_destroy_volume(const gchar *pool_name, const gchar *vm_name, GError **error) 
{
    gboolean success;
    gchar *target_dataset = g_strdup_printf("%s/%s", pool_name, vm_name);
    
    // 명령어: zfs destroy -r <pool>/vms/<vm_name>
    const gchar *argv[] = {"zfs", "destroy", "-r", target_dataset, NULL};

    success = g_spawn_sync(NULL, (gchar **)argv, NULL, 
                           G_SPAWN_SEARCH_PATH, 
                           NULL, NULL, NULL, NULL, NULL, error);
    
    g_free(target_dataset);
    return success;
}