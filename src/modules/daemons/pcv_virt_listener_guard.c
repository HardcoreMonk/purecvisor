













#include "modules/daemons/pcv_virt_listener_guard.h"

gboolean
pcv_virt_listener_guard_wait(guint interval_ms)
{
    if (interval_ms == 0U || interval_ms > G_MAXINT)
        return FALSE;

    g_usleep((gulong)interval_ms * G_TIME_SPAN_MILLISECOND);
    return TRUE;
}

gboolean
pcv_virt_listener_connection_lost(gboolean connection_present,
                                  gint alive_result,
                                  gint rpc_probe_result)
{
    return !connection_present || alive_result != 1 || rpc_probe_result != 0;
}
