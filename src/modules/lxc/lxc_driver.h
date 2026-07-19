
#ifndef PURECVISOR_LXC_DRIVER_H
#define PURECVISOR_LXC_DRIVER_H

#include <glib.h>
#include <gio/gio.h>
#include "../../utils/pcv_config.h"

G_BEGIN_DECLS

#define PCV_LXC_PATH        (pcv_config_get_container_path())

#define PCV_LXC_ZFS_BASE    (pcv_config_get_container_pool())

#define PCV_LXC_DEFAULT_BRIDGE "virbr0"

typedef enum {
    PCV_LXC_STATE_STOPPED  = 0,
    PCV_LXC_STATE_STARTING = 1,
    PCV_LXC_STATE_RUNNING  = 2,
    PCV_LXC_STATE_STOPPING = 3,
    PCV_LXC_STATE_FROZEN   = 4,
    PCV_LXC_STATE_UNKNOWN  = 99,
} PcvLxcState;

typedef struct {
    gchar       *name;
    PcvLxcState  state;
    gchar       *state_str;
    gchar       *ip_addr;
    gchar       *image;
} PcvLxcInfo;

typedef struct {
    gchar    *name;
    gchar    *state_str;
    guint64   mem_used_bytes;
    guint64   mem_limit_bytes;
    guint64   cpu_time_ns;
    gdouble   cpu_percent;
    guint64   net_rx_bytes;
    guint64   net_tx_bytes;
    gchar    *ip_addr;
    pid_t     init_pid;
} PcvLxcMetrics;

void     pcv_lxc_create_async   (const gchar        *name,
                                  const gchar        *image,
                                  guint               memory_mb,
                                  guint               vcpu_count,
                                  const gchar        *network_bridge,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);

void     pcv_lxc_create_async_full(const gchar        *name,
                                  const gchar        *image,
                                  guint               memory_mb,
                                  guint               vcpu_count,
                                  const gchar        *network_bridge,
                                  gint                rootless,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);

gboolean pcv_lxc_create_finish  (GAsyncResult *result, GError **error);

void     pcv_lxc_destroy_async  (const gchar        *name,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);
gboolean pcv_lxc_destroy_finish (GAsyncResult *result, GError **error);

void     pcv_lxc_clone_async    (const gchar        *source,
                                  const gchar        *target,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);
gboolean pcv_lxc_clone_finish   (GAsyncResult *result, GError **error);
gboolean pcv_lxc_clone          (const gchar *source, const gchar *target);

#include "lxc_owner.h"

void     pcv_lxc_start_async    (const gchar        *name,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);
gboolean pcv_lxc_start_finish   (GAsyncResult *result, GError **error);

void     pcv_lxc_stop_async     (const gchar        *name,
                                  gboolean            force,
                                  GCancellable       *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer            user_data);
gboolean pcv_lxc_stop_finish    (GAsyncResult *result, GError **error);

GPtrArray      *pcv_lxc_list        (GError **error);

PcvLxcMetrics  *pcv_lxc_get_metrics (const gchar *name, GError **error);

gchar          *pcv_lxc_get_state   (const gchar *name);

void  pcv_lxc_exec_async  (const gchar        *name,
                            const gchar       **argv,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            user_data);
gchar *pcv_lxc_exec_finish (GAsyncResult *result, GError **error);

void     pcv_lxc_snapshot_create_async   (const gchar *name, const gchar *snap_name,
                                           GCancellable *c, GAsyncReadyCallback cb,
                                           gpointer user_data);
gboolean pcv_lxc_snapshot_create_finish  (GAsyncResult *result, GError **error);

void     pcv_lxc_snapshot_rollback_async (const gchar *name, const gchar *snap_name,
                                           GCancellable *c, GAsyncReadyCallback cb,
                                           gpointer user_data);
gboolean pcv_lxc_snapshot_rollback_finish(GAsyncResult *result, GError **error);

void     pcv_lxc_snapshot_delete_async   (const gchar *name, const gchar *snap_name,
                                           GCancellable *c, GAsyncReadyCallback cb,
                                           gpointer user_data);
gboolean pcv_lxc_snapshot_delete_finish  (GAsyncResult *result, GError **error);

void       pcv_lxc_snapshot_list_async  (const gchar *name,
                                          GCancellable *c, GAsyncReadyCallback cb,
                                          gpointer user_data);
GPtrArray *pcv_lxc_snapshot_list_finish (GAsyncResult *result, GError **error);

gboolean pcv_lxc_set_resource_limits(const gchar *name, gint cpu_percent,
                                      gint memory_mb, gint cpu_weight,
                                      gint memory_low_mb, gint memory_high_mb,
                                      gint64 io_read_bps, gint pids_max,
                                      GError **error);

typedef struct {
    gchar *name;
    gchar *type;
    gchar *bridge;
    gchar *hwaddr;
    gchar *ipv4;
    gchar *veth_peer;
} PcvLxcNicInfo;

void pcv_lxc_nic_info_free(PcvLxcNicInfo *nic);

GPtrArray *pcv_lxc_nic_list(const gchar *name, GError **error);

gboolean pcv_lxc_nic_attach(const gchar *name, const gchar *bridge,
                              const gchar *hwaddr, GError **error);

gboolean pcv_lxc_nic_detach(const gchar *name, const gchar *nic_name,
                              GError **error);

gboolean pcv_lxc_set_bandwidth(const gchar *name, const gchar *nic_name,
                                 guint inbound_kbps, guint outbound_kbps,
                                 GError **error);

gboolean pcv_lxc_checkpoint(const gchar *name, const gchar *checkpoint_dir);

gboolean pcv_lxc_restore(const gchar *name, const gchar *checkpoint_dir);

gboolean pcv_lxc_set_seccomp_profile(const gchar *name, const gchar *profile_name);

gchar *pcv_lxc_get_seccomp_profile(const gchar *name);

void pcv_lxc_info_free    (PcvLxcInfo    *info);
void pcv_lxc_metrics_free (PcvLxcMetrics *metrics);

G_END_DECLS

#endif
