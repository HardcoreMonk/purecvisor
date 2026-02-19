/* src/modules/storage/zfs_driver.h */

#ifndef PURECVISOR_ZFS_DRIVER_H
#define PURECVISOR_ZFS_DRIVER_H

#include <glib.h>

gboolean purecvisor_zfs_create_volume(const gchar *pool, const gchar *name, const gchar *size, GError **error);
gboolean purecvisor_zfs_destroy_volume(const gchar *pool, const gchar *name, GError **error);

#endif