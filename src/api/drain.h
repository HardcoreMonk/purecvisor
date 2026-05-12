









































#ifndef PURECVISOR_DRAIN_H
#define PURECVISOR_DRAIN_H

#include <glib.h>

G_BEGIN_DECLS





void pcv_drain_init(void);






gboolean pcv_drain_inc(void);






void pcv_drain_dec(void);





gboolean pcv_drain_is_shutdown(void);





gint pcv_drain_get_inflight(void);







void pcv_drain_notify_stopping(void);









void pcv_drain_begin(GMainLoop *loop, guint timeout_sec);






void pcv_drain_notify_ready(void);







void pcv_drain_notify_watchdog(void);






guint64 pcv_drain_get_watchdog_usec(void);







void pcv_drain_cancel(void);





void pcv_drain_shutdown(void);

G_END_DECLS

#endif
