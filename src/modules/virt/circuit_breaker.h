




















#ifndef PURECVISOR_CIRCUIT_BREAKER_H
#define PURECVISOR_CIRCUIT_BREAKER_H

#include <glib.h>

G_BEGIN_DECLS



typedef enum {
    CB_STATE_CLOSED    = 0,
    CB_STATE_OPEN      = 1,
    CB_STATE_HALF_OPEN = 2,
} CbState;



#define CB_FAILURE_THRESHOLD_DEFAULT  5
#define CB_FAILURE_THRESHOLD   5
#define CB_BACKOFF_INITIAL_MS  200
#define CB_BACKOFF_MAX_MS      30000


static_assert(CB_BACKOFF_MAX_MS >= CB_BACKOFF_INITIAL_MS);
static_assert(CB_FAILURE_THRESHOLD_DEFAULT >= 1);







void cb_init(void);









[[nodiscard]] gboolean cb_is_open(void);






void cb_record_success(void);








void cb_record_failure(void);






CbState cb_get_state(void);





const gchar *cb_get_state_str(void);





gint cb_get_failure_count(void);







void cb_set_failure_threshold(gint threshold);





gint cb_get_failure_threshold(void);







CbState cb_get_named_state(const gchar *name);






gchar *cb_get_prometheus_metrics(void);





void cb_shutdown(void);

G_END_DECLS

#endif
