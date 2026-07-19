
#ifndef PURECVISOR_ZFS_LOCK_H
#define PURECVISOR_ZFS_LOCK_H

#include <glib.h>

G_BEGIN_DECLS

void pcv_zfs_pool_lock_init(void);

void pcv_zfs_pool_lock_shutdown(void);

gboolean pcv_zfs_pool_lock(const gchar *pool, const gchar *op,
                            gint timeout_ms, GError **error);

void pcv_zfs_pool_unlock(const gchar *pool);

void pcv_zfs_pool_get_stats(gint *registered, gint64 *contentions);

G_END_DECLS

#endif
