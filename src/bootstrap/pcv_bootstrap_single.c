#include "pcv_bootstrap.h"

#include "modules/network/ovn_manager.h"
#include "modules/network/ovs_overlay.h"
#include "utils/pcv_config.h"















const gchar *
pcv_bootstrap_get_daemon_binary_path(void)
{
    return "/usr/local/bin/purecvisorsd";
}

void
pcv_bootstrap_init_cluster_manager(void)
{
    g_message("[init] Single Edge mode — cluster manager bootstrap skipped");
}

void
pcv_bootstrap_init_scheduler_proxy(void)
{
    g_message("[init] Single Edge mode — scheduler/proxy bootstrap skipped");
}

void
pcv_bootstrap_init_federation(void)
{
    g_message("[init] Single Edge mode — federation bootstrap skipped");
}

void
pcv_bootstrap_init_runtime_network(void)
{
    if (pcv_ovn_is_available()) {
        GError *ovn_local_err = NULL;
        if (pcv_ovn_single_prepare_local(&ovn_local_err)) {
            g_message("[init] OVN local controller prepared in Single Edge");
        } else {
            g_warning("[init] OVN local controller prepare failed: %s",
                      ovn_local_err ? ovn_local_err->message : "unknown");
            g_clear_error(&ovn_local_err);
        }
    }

    const gchar *ovl_br = pcv_config_get_string("overlay", "default_bridge", "");
    if (!ovl_br || !*ovl_br)
        return;

    GError *ovl_err = NULL;
    pcv_overlay_create(ovl_br,
                       pcv_config_get_int("overlay", "default_vni", 100),
                       pcv_config_get_string("overlay", "default_cidr", ""),
                       &ovl_err);
    if (ovl_err) {
        g_warning("Overlay auto-create: %s", ovl_err->message);
        g_error_free(ovl_err);
    }

    g_message("OVS overlay '%s' auto-provisioned (VNI=%d)",
              ovl_br,
              pcv_config_get_int("overlay", "default_vni", 100));
}

void
pcv_bootstrap_shutdown_cluster_stack(void)
{
}
