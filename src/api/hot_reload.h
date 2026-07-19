#ifndef PURECVISOR_HOT_RELOAD_H
#define PURECVISOR_HOT_RELOAD_H

#include <glib.h>

G_BEGIN_DECLS

void pcv_hot_reload_init(const gchar *binary_path, int uds_listen_fd);

typedef enum {
    PCV_UPGRADE_IDLE,
    PCV_UPGRADE_DRAINING,
    PCV_UPGRADE_READY,
    PCV_UPGRADE_EXECUTING
} PcvUpgradeState;

PcvUpgradeState pcv_hot_reload_get_state(void);

gboolean pcv_hot_reload_prepare(GError **error);

const gchar *pcv_hot_reload_get_version(void);

G_END_DECLS

#endif
