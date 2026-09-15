













#ifndef PURECVISOR_DAEMONS_PCV_VIRT_LISTENER_GUARD_H
#define PURECVISOR_DAEMONS_PCV_VIRT_LISTENER_GUARD_H

#include <glib.h>

G_BEGIN_DECLS

#define PCV_VIRT_LISTENER_HEALTHCHECK_INTERVAL_MS 1000U








gboolean pcv_virt_listener_guard_wait(guint interval_ms);














gboolean pcv_virt_listener_connection_lost(gboolean connection_present,
                                           gint alive_result,
                                           gint rpc_probe_result);

G_END_DECLS

#endif
