/* src/modules/storage/zfs_driver.c */

#include "zfs_driver.h"
#include <glib.h>
#include <gio/gio.h>    // [Fix] G_IO_ERROR 사용을 위해 필수
#include <stdio.h>

// Helper to run command
static gboolean _run_command(const gchar *cmd, GError **error) {
    gint exit_status;
    gchar *std_err = NULL;
    GError *spawn_err = NULL;
    
    // g_spawn_command_line_sync is blocking but safe for simple CLI tools in this phase
    if (!g_spawn_command_line_sync(cmd, NULL, &std_err, &exit_status, &spawn_err)) {
        g_propagate_error(error, spawn_err);
        g_free(std_err);
        return FALSE;
    }

    if (exit_status != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, 
                    "ZFS Command Failed: %s (Stderr: %s)", cmd, std_err);
        g_free(std_err);
        return FALSE;
    }

    g_free(std_err);
    return TRUE;
}

gboolean purecvisor_zfs_create_volume(const gchar *pool, const gchar *name, const gchar *size, GError **error) {
    // Command: zfs create -V <size> <pool>/<name>
    gchar *cmd = g_strdup_printf("zfs create -V %s %s/%s", size, pool, name);
    gboolean ret = _run_command(cmd, error);
    g_free(cmd);
    return ret;
}

gboolean purecvisor_zfs_destroy_volume(const gchar *pool, const gchar *name, GError **error) {
    // Command: zfs destroy <pool>/<name>
    gchar *cmd = g_strdup_printf("zfs destroy %s/%s", pool, name);
    gboolean ret = _run_command(cmd, error);
    g_free(cmd);
    return ret;
}