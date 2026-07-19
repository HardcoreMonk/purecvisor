
#include <libvirt/libvirt.h>

#include "modules/dispatcher/hotplug_affect_policy.h"

unsigned int pcv_hotplug_compute_affect_flags(gboolean is_active, gboolean config_only)
{
    return VIR_DOMAIN_AFFECT_CONFIG
         | ((is_active && !config_only) ? VIR_DOMAIN_AFFECT_LIVE : 0u);
}
