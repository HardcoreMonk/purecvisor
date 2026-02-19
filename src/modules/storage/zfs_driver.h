/* src/modules/storage/zfs_driver.h */

#ifndef PURECVISOR_ZFS_DRIVER_H
#define PURECVISOR_ZFS_DRIVER_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* ========================================================================= */
/* Phase 5: ZFS Volume Provisioning (이 두 줄이 반드시 있어야 합니다!) */
/* ========================================================================= */
gboolean purecvisor_zfs_create_volume(const gchar *pool_name, const gchar *vm_name, const gchar *size_str, GError **error);
gboolean purecvisor_zfs_destroy_volume(const gchar *pool_name, const gchar *vm_name, GError **error);


/* ========================================================================= */
/* Phase 6: Snapshot Management */
/* ========================================================================= */
/* 1. Snapshot Create */
void purecvisor_zfs_snapshot_create_async(const gchar *pool_name,
                                    const gchar *vm_name,
                                    const gchar *snap_name,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);

gboolean purecvisor_zfs_snapshot_create_finish(GAsyncResult *res, GError **error);

/* 2. Snapshot Delete */
void purecvisor_zfs_snapshot_delete_async(const gchar *pool_name,
                                    const gchar *vm_name,
                                    const gchar *snap_name,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);

gboolean purecvisor_zfs_snapshot_delete_finish(GAsyncResult *res, GError **error);

/* 3. Snapshot Rollback */
void purecvisor_zfs_snapshot_rollback_async(const gchar *pool_name,
                                      const gchar *vm_name,
                                      const gchar *snap_name,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

gboolean purecvisor_zfs_snapshot_rollback_finish(GAsyncResult *res, GError **error);

/* 4. Snapshot List (Returns GPtrArray of snapshot names) */
void purecvisor_zfs_snapshot_list_async(const gchar *pool_name,
                                  const gchar *vm_name,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);

GPtrArray* purecvisor_zfs_snapshot_list_finish(GAsyncResult *res, GError **error);

G_END_DECLS

#endif /* PURECVISOR_ZFS_DRIVER_H */
