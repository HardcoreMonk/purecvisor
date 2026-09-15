












#ifndef PURECVISOR_DAEMONS_PCV_TELEMETRY_RECONNECT_GUARD_H
#define PURECVISOR_DAEMONS_PCV_TELEMETRY_RECONNECT_GUARD_H

#include <glib.h>

G_BEGIN_DECLS

#define PCV_TELEMETRY_POLL_INTERVAL_MS 1000U
#define PCV_TELEMETRY_RECONNECT_INTERVAL_MS 5000U












gboolean pcv_telemetry_reconnect_required(gboolean connection_present,
                                          gint stats_result,
                                          gboolean stats_list_present);

G_END_DECLS

#endif
