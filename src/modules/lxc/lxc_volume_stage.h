#ifndef PURECVISOR_LXC_VOLUME_STAGE_H
#define PURECVISOR_LXC_VOLUME_STAGE_H

#include <glib.h>

G_BEGIN_DECLS

#define PCV_LXC_RUNTIME_CONFIG_DIR "/run/purecvisor/lxc-runtime"

#define PCV_LXC_MOUNT_RO        (1u << 0)
#define PCV_LXC_MOUNT_RECURSIVE (1u << 1)

GQuark pcv_lxc_volume_error_quark(void);
#define PCV_LXC_VOLUME_ERROR (pcv_lxc_volume_error_quark())
typedef enum {
    PCV_LXC_VOLUME_ERROR_INTERNAL,
    PCV_LXC_VOLUME_ERROR_STAGING_MISMATCH,
} PcvLxcVolumeError;

gboolean pcv_lxc_volume_mount_clone(int src_fd, int target_fd, guint flags, guint64 *mnt_id_out, GError **error);

gboolean pcv_lxc_volume_start_prepare(const gchar *lxc_path, const gchar *name, GError **error);

void pcv_lxc_volume_start_abort(const gchar *lxc_path, const gchar *name);

void pcv_lxc_runtime_config_remove(const gchar *name);

gchar *pcv_lxc_runtime_config_path(const gchar *name);

void pcv_lxc_volstage_clear(const gchar *name);

void pcv_lxc_volume_set_roots_for_test(const gchar *volstage_root, const gchar *runtime_root);

G_END_DECLS

#endif
