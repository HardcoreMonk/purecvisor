
#ifndef PURECVISOR_ZFS_DRIVER_H
#define PURECVISOR_ZFS_DRIVER_H

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

gboolean purecvisor_zfs_create_volume(const gchar *pool_name, const gchar *vm_name, const gchar *size_str, GError **error);
gboolean purecvisor_zfs_destroy_volume(const gchar *pool_name, const gchar *vm_name, GError **error);

void purecvisor_zfs_snapshot_create_async(const gchar *pool_name,
                                    const gchar *vm_name,
                                    const gchar *snap_name,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);

gboolean purecvisor_zfs_snapshot_create_finish(GAsyncResult *res, GError **error);

void purecvisor_zfs_snapshot_delete_async(const gchar *pool_name,
                                    const gchar *vm_name,
                                    const gchar *snap_name,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);

gboolean purecvisor_zfs_snapshot_delete_finish(GAsyncResult *res, GError **error);

void purecvisor_zfs_snapshot_rollback_async(const gchar *pool_name,
                                      const gchar *vm_name,
                                      const gchar *snap_name,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data);

gboolean purecvisor_zfs_snapshot_rollback_finish(GAsyncResult *res, GError **error);

void purecvisor_zfs_snapshot_list_async(const gchar *pool_name,
                                  const gchar *vm_name,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);

GPtrArray* purecvisor_zfs_snapshot_list_finish(GAsyncResult *res, GError **error);

gboolean purecvisor_zfs_create_pool(const gchar *name, const gchar *vdev_type,
                                     const gchar **disks, gint n_disks,
                                     const gchar *compression,
                                     GError **error);
gboolean purecvisor_zfs_destroy_pool(const gchar *name, GError **error);
gboolean purecvisor_zfs_scrub_pool(const gchar *name, GError **error);

gboolean purecvisor_zfs_clone_volume(const gchar *pool_name, const gchar *source_vm,
                                      const gchar *snap_name, const gchar *clone_vm,
                                      GError **error);

gboolean purecvisor_zfs_full_copy(const gchar *pool_name, const gchar *source_vm,
                                   const gchar *snap_name, const gchar *clone_vm,
                                   GError **error);

typedef struct {
    gchar    state[16];
    gint     errors_read;
    gint     errors_write;
    gint     errors_cksum;
    gint64   scrub_age_sec;
    gboolean scrub_running;
    gdouble  capacity_pct;
} ZfsPoolHealth;

gboolean pcv_zfs_pool_health(const gchar *pool_name, ZfsPoolHealth *out);

JsonObject *pcv_zfs_pool_health_to_json(const ZfsPoolHealth *h);

gdouble pcv_zfs_pool_state_metric_val(const gchar *state);

typedef struct {
    gint64 window_start_us;
    gint   attempts;
} ZfsRecoverGuard;

gboolean pcv_zfs_recover_guard_allow(ZfsRecoverGuard *g, gint64 now_us,
                                     gint64 window_us, gint max_attempts);

typedef enum {
    PCV_ZFS_RECOVER_DISABLED = 0,
    PCV_ZFS_RECOVER_NOT_SUSPENDED,
    PCV_ZFS_RECOVER_DEV_UNREADABLE,
    PCV_ZFS_RECOVER_CB_TRIPPED,
    PCV_ZFS_RECOVER_CLEARED,
    PCV_ZFS_RECOVER_CLEAR_FAILED,
} PcvZfsRecoverResult;

PcvZfsRecoverResult pcv_zfs_pool_recover_suspended(const gchar *pool_name,
                                                   ZfsRecoverGuard *guard);

JsonObject *pcv_zfs_pool_forecast(const gchar *pool_name);

void pcv_zfs_capacity_record(const gchar *pool_name);

JsonObject *purecvisor_zfs_pool_health_detail(const gchar *pool_name);

gboolean purecvisor_zfs_promote(const gchar *clone_name);

gboolean purecvisor_zfs_create_zvol_encrypted(const gchar *name, const gchar *size,
                                               const gchar *passphrase, GError **error);

[[nodiscard]] gboolean purecvisor_zfs_check_snapshot_quota(const gchar *dataset, gint max_snapshots);

G_END_DECLS

#endif
