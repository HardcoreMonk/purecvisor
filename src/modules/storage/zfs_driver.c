/*
 * src/modules/storage/zfs_driver.c
 *
 * Description:
 * Implementation of Non-blocking ZFS Driver.
 * Updated for Phase 3.
 */

#include "zfs_driver.h"
#include <stdio.h>

struct _PureCVisorZfsDriver {
    GObject parent_instance;
};

G_DEFINE_TYPE(PureCVisorZfsDriver, purecvisor_zfs_driver, G_TYPE_OBJECT)

static void
purecvisor_zfs_driver_init(PureCVisorZfsDriver *self)
{
    (void)self; /* Unused */
}

static void
purecvisor_zfs_driver_class_init(PureCVisorZfsDriverClass *klass)
{
    (void)klass; /* Unused */
}

PureCVisorZfsDriver *
purecvisor_zfs_driver_new(void)
{
    return g_object_new(PURECVISOR_TYPE_ZFS_DRIVER, NULL);
}

static gboolean
_zfs_subprocess_finish_helper(GAsyncResult *res, GError **error)
{
    GSubprocess *proc = NULL;
    gboolean success = FALSE;
    GError *local_error = NULL;
    GBytes *stderr_bytes = NULL; /* FIX: Changed from gchar* to GBytes* */

    proc = G_SUBPROCESS(g_async_result_get_source_object(res));

    /* Finish communication */
    /* Note: We pass NULL for stdout_bytes because we don't need it here */
    if (!g_subprocess_communicate_finish(proc, res, NULL, &stderr_bytes, &local_error)) {
        g_propagate_error(error, local_error);
        if (stderr_bytes) g_bytes_unref(stderr_bytes);
        return FALSE;
    }

    /* Check Exit Status */
    if (g_subprocess_get_if_exited(proc) && g_subprocess_get_successful(proc)) {
        success = TRUE;
    } else {
        /* Convert GBytes to string for error message */
        gchar *err_str = NULL;
        if (stderr_bytes) {
            gsize len;
            const gchar *data = g_bytes_get_data(stderr_bytes, &len);
            if (len > 0) {
                err_str = g_strndup(data, len); /* Ensure null-termination */
            }
        }

        gchar *clean_err = (err_str && *err_str) ? g_strstrip(err_str) : "Unknown ZFS error";
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, 
                    "ZFS Command Failed: %s", clean_err);
        
        g_free(err_str);
    }

    /* Cleanup GBytes object */
    if (stderr_bytes) {
        g_bytes_unref(stderr_bytes);
    }

    return success;
}



static void
_on_create_vol_communicate_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source;
    GTask *task = G_TASK(user_data);
    GError *error = NULL;

    if (_zfs_subprocess_finish_helper(res, &error)) {
        gchar *full_path = g_strdup((gchar *)g_task_get_task_data(task));
        g_task_return_pointer(task, full_path, g_free);
    } else {
        g_task_return_error(task, error);
    }

    g_object_unref(task);
}

void
purecvisor_zfs_driver_create_vol_async(PureCVisorZfsDriver *self,
                                       const gchar *pool_name,
                                       const gchar *vol_name,
                                       guint64 size_bytes,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
    g_return_if_fail(PURECVISOR_IS_ZFS_DRIVER(self));

    GTask *task = g_task_new(self, cancellable, callback, user_data);
    GError *error = NULL;

    gchar *zvol_path_arg = g_strdup_printf("%s/%s", pool_name, vol_name);
    gchar *size_str = g_strdup_printf("%lu", size_bytes);
    gchar *dev_path = g_strdup_printf("/dev/zvol/%s/%s", pool_name, vol_name);
    
    g_task_set_task_data(task, dev_path, g_free);

    GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                         &error,
                                         "zfs", "create", "-V", size_str, "-p", zvol_path_arg, NULL);

    g_free(zvol_path_arg);
    g_free(size_str);

    if (!proc) {
        g_task_return_error(task, error);
        g_object_unref(task);
        return;
    }

    g_subprocess_communicate_async(proc, NULL, cancellable, 
                                   _on_create_vol_communicate_complete, task);
    g_object_unref(proc);
}

gboolean
purecvisor_zfs_driver_create_vol_finish(PureCVisorZfsDriver *self,
                                        GAsyncResult *res,
                                        gchar **out_full_path,
                                        GError **error)
{
    g_return_val_if_fail(g_task_is_valid(res, self), FALSE);

    gchar *path = g_task_propagate_pointer(G_TASK(res), error);
    if (path) {
        if (out_full_path) *out_full_path = path;
        else g_free(path);
        return TRUE;
    }
    return FALSE;
}

static void
_on_destroy_vol_communicate_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source;
    GTask *task = G_TASK(user_data);
    GError *error = NULL;

    if (_zfs_subprocess_finish_helper(res, &error)) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, error);
    }
    g_object_unref(task);
}

void
purecvisor_zfs_driver_destroy_vol_async(PureCVisorZfsDriver *self,
                                        const gchar *pool_name,
                                        const gchar *vol_name,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data)
{
    g_return_if_fail(PURECVISOR_IS_ZFS_DRIVER(self));

    GTask *task = g_task_new(self, cancellable, callback, user_data);
    GError *error = NULL;

    gchar *zvol_path_arg = g_strdup_printf("%s/%s", pool_name, vol_name);

    GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                         &error,
                                         "zfs", "destroy", "-r", zvol_path_arg, NULL);

    g_free(zvol_path_arg);

    if (!proc) {
        g_task_return_error(task, error);
        g_object_unref(task);
        return;
    }

    g_subprocess_communicate_async(proc, NULL, cancellable, 
                                   _on_destroy_vol_communicate_complete, task);
    g_object_unref(proc);
}

gboolean
purecvisor_zfs_driver_destroy_vol_finish(PureCVisorZfsDriver *self,
                                         GAsyncResult *res,
                                         GError **error)
{
    g_return_val_if_fail(g_task_is_valid(res, self), FALSE);
    return g_task_propagate_boolean(G_TASK(res), error);
}