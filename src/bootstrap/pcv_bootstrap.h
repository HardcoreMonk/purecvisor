#ifndef PCV_BOOTSTRAP_H
#define PCV_BOOTSTRAP_H

#include <glib.h>

typedef struct {
    const gchar *edition_name;
    gboolean cluster_enabled;
} PcvBootstrapEditionInfo;

const PcvBootstrapEditionInfo *pcv_bootstrap_get_edition_info(void);
const gchar *pcv_bootstrap_get_daemon_binary_path(void);

void pcv_bootstrap_init_cluster_manager(void);
void pcv_bootstrap_init_scheduler_proxy(void);
void pcv_bootstrap_init_federation(void);
void pcv_bootstrap_init_runtime_network(void);
void pcv_bootstrap_register_async_methods(GHashTable *async_methods);
void pcv_bootstrap_register_rpc_routes(GHashTable *rpc_routes);
void pcv_bootstrap_shutdown_cluster_stack(void);

#endif
