#ifndef PURECVISOR_LXC_STORAGE_H
#define PURECVISOR_LXC_STORAGE_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    PCV_LXC_STORAGE_ZFS,
    PCV_LXC_STORAGE_BTRFS
} PcvLxcStorageKind;

typedef struct {
    PcvLxcStorageKind kind;
    gchar *dataset;
} PcvLxcStorage;

gboolean pcv_lxc_storage_zfs_mount_allowed(const gchar *name, const gchar *dataset, gboolean rootfs_mount);
gboolean pcv_lxc_storage_default(PcvLxcStorageKind *kind, GError **error);
gboolean pcv_lxc_storage_prepare_create(const gchar *name, PcvLxcStorageKind kind, GError **error);
gboolean pcv_lxc_storage_record(const gchar *name, PcvLxcStorageKind kind, const gchar *dataset, GError **error);
gboolean pcv_lxc_storage_resolve(const gchar *name, PcvLxcStorage *storage, GError **error);
gboolean pcv_lxc_storage_resolve_for_destroy(const gchar *name, PcvLxcStorage *storage, GError **error);
void pcv_lxc_storage_clear(PcvLxcStorage *storage);
gboolean pcv_lxc_storage_recover(const gchar *name, GError **error);
gboolean pcv_lxc_storage_btrfs_validate_copy(const gchar *name, GError **error);
gboolean pcv_lxc_storage_btrfs_snapshot_create(const gchar *name, const gchar *snapshot, GError **error);
gboolean pcv_lxc_storage_btrfs_snapshot_rollback(const gchar *name, const gchar *snapshot, GError **error);
gboolean pcv_lxc_storage_btrfs_snapshot_delete(const gchar *name, const gchar *snapshot, GError **error);
GPtrArray *pcv_lxc_storage_btrfs_snapshot_list(const gchar *name, GError **error);
gboolean pcv_lxc_storage_btrfs_destroy(const gchar *name, GError **error);

G_END_DECLS

#endif
