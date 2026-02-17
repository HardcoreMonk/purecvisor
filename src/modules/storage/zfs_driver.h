/*
 * src/modules/storage/zfs_driver.h
 *
 * Description:
 * Asynchronous ZFS Volume (ZVol) manager.
 * Executes 'zfs' commands via GSubprocess without blocking the main loop.
 *
 * Author: PureCVisor Architect
 */

#ifndef PURECVISOR_ZFS_DRIVER_H
#define PURECVISOR_ZFS_DRIVER_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define PURECVISOR_TYPE_ZFS_DRIVER (purecvisor_zfs_driver_get_type())

G_DECLARE_FINAL_TYPE(PureCVisorZfsDriver, purecvisor_zfs_driver, PURECVISOR, ZFS_DRIVER, GObject)

PureCVisorZfsDriver *purecvisor_zfs_driver_new(void);

/**
 * purecvisor_zfs_driver_create_vol_async:
 * @pool_name: Name of the ZFS pool (e.g., "tank")
 * @vol_name: Name of the volume to create (e.g., "vm-100-disk0")
 * @size_bytes: Size of the volume in bytes
 * @cancellable: (Nullable): GCancellable or NULL
 * @callback: Callback function
 * @user_data: User data
 *
 * Executes: zfs create -V [size] [pool]/[vol]
 */
void purecvisor_zfs_driver_create_vol_async(PureCVisorZfsDriver *self,
                                            const gchar *pool_name,
                                            const gchar *vol_name,
                                            guint64 size_bytes,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);

gboolean purecvisor_zfs_driver_create_vol_finish(PureCVisorZfsDriver *self,
                                                 GAsyncResult *res,
                                                 gchar **out_full_path, /* Returns /dev/zvol/... */
                                                 GError **error);

/**
 * purecvisor_zfs_driver_destroy_vol_async:
 * Used for Rollback or VM Deletion.
 *
 * Executes: zfs destroy [pool]/[vol]
 */
void purecvisor_zfs_driver_destroy_vol_async(PureCVisorZfsDriver *self,
                                             const gchar *pool_name,
                                             const gchar *vol_name,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data);

gboolean purecvisor_zfs_driver_destroy_vol_finish(PureCVisorZfsDriver *self,
                                                  GAsyncResult *res,
                                                  GError **error);

G_END_DECLS

#endif /* PURECVISOR_ZFS_DRIVER_H */