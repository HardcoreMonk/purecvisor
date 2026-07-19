
#ifndef PURECVISOR_DISPATCHER_HOTPLUG_AFFECT_POLICY_H
#define PURECVISOR_DISPATCHER_HOTPLUG_AFFECT_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

unsigned int pcv_hotplug_compute_affect_flags(gboolean is_active, gboolean config_only);

G_END_DECLS

#endif
