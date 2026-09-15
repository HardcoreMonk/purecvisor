











#include "modules/daemons/pcv_telemetry_reconnect_guard.h"

gboolean
pcv_telemetry_reconnect_required(gboolean connection_present,
                                 gint stats_result,
                                 gboolean stats_list_present)
{
    return !connection_present || stats_result < 0 || !stats_list_present;
}
