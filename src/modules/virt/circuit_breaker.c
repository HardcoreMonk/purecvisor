



































#include "circuit_breaker.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"

#include <glib.h>
#include <stddef.h>

#define CB_LOG_DOM "circuit_breaker"



typedef struct {
    CbState  state;
    GMutex   mutex;

    gint     failure_count;
    gint     failure_threshold;
    gint     backoff_ms;


    gint64   open_until_us;


    gboolean probe_in_flight;


    gint     consecutive_successes;
    gint     success_threshold;
} CbInternal;

static CbInternal g_cb = { 0 };



static void
_transition_to_open(void)
{
    g_cb.state        = CB_STATE_OPEN;
    g_cb.open_until_us = g_get_monotonic_time()
                         + (gint64)g_cb.backoff_ms * G_TIME_SPAN_MILLISECOND;
    g_cb.probe_in_flight = FALSE;
    g_cb.consecutive_successes = 0;

    PCV_LOG_WARN(CB_LOG_DOM,
                 "State → OPEN (failures=%d, backoff=%dms, retry_after=%.1fs)",
                 g_cb.failure_count,
                 g_cb.backoff_ms,
                 (double)g_cb.backoff_ms / 1000.0);
}

static void
_transition_to_half_open(void)
{
    g_cb.state           = CB_STATE_HALF_OPEN;
    g_cb.probe_in_flight = FALSE;
    g_cb.consecutive_successes = 0;
    PCV_LOG_INFO(CB_LOG_DOM, "State → HALF_OPEN (sending probe request, need %d successes)",
                 g_cb.success_threshold);
}

static void
_transition_to_closed(void)
{
    g_cb.state         = CB_STATE_CLOSED;
    g_cb.failure_count = 0;
    g_cb.backoff_ms    = CB_BACKOFF_INITIAL_MS;
    g_cb.probe_in_flight = FALSE;
    g_cb.consecutive_successes = 0;
    PCV_LOG_INFO(CB_LOG_DOM, "State → CLOSED (libvirt connection restored)");
}




























void
cb_init(void)
{
    g_mutex_init(&g_cb.mutex);
    g_cb.state           = CB_STATE_CLOSED;
    g_cb.failure_count   = 0;
    g_cb.backoff_ms      = CB_BACKOFF_INITIAL_MS;
    g_cb.open_until_us   = 0;
    g_cb.probe_in_flight = FALSE;
    g_cb.consecutive_successes = 0;
    g_cb.success_threshold = 3;


    gint threshold = pcv_config_get_int("libvirt", "cb_failure_threshold",
                                        CB_FAILURE_THRESHOLD_DEFAULT);
    if (threshold < 1)  threshold = 1;
    if (threshold > 50) threshold = 50;
    g_cb.failure_threshold = threshold;

    PCV_LOG_INFO(CB_LOG_DOM,
                 "Initialized (threshold=%d, initial_backoff=%dms, max_backoff=%dms)"
                 " — configurable via [libvirt] cb_failure_threshold",
                 g_cb.failure_threshold, CB_BACKOFF_INITIAL_MS, CB_BACKOFF_MAX_MS);
}













gboolean
cb_is_open(void)
{
    g_mutex_lock(&g_cb.mutex);

    switch (g_cb.state) {

    case CB_STATE_CLOSED:
        g_mutex_unlock(&g_cb.mutex);
        return FALSE;

    case CB_STATE_OPEN:

        if (g_get_monotonic_time() >= g_cb.open_until_us) {
            _transition_to_half_open();

            g_cb.probe_in_flight = TRUE;
            g_mutex_unlock(&g_cb.mutex);
            return FALSE;
        }
        g_mutex_unlock(&g_cb.mutex);
        return TRUE;

    case CB_STATE_HALF_OPEN:



        if (g_cb.probe_in_flight) {
            g_mutex_unlock(&g_cb.mutex);
            return TRUE;
        }



        g_mutex_unlock(&g_cb.mutex);
        return (g_cb.state != CB_STATE_CLOSED);
    }
    unreachable();
}









void
cb_record_success(void)
{
    g_mutex_lock(&g_cb.mutex);

    if (g_cb.state == CB_STATE_HALF_OPEN) {



        g_cb.consecutive_successes++;
        if (g_cb.consecutive_successes >= g_cb.success_threshold) {
            PCV_LOG_INFO(CB_LOG_DOM,
                "HALF_OPEN: %d consecutive successes (threshold=%d) — transitioning to CLOSED",
                g_cb.consecutive_successes, g_cb.success_threshold);
            _transition_to_closed();
        } else {

            g_cb.probe_in_flight = FALSE;
            PCV_LOG_INFO(CB_LOG_DOM,
                "HALF_OPEN: success %d/%d — need more probes",
                g_cb.consecutive_successes, g_cb.success_threshold);
        }
    } else if (g_cb.state == CB_STATE_CLOSED) {



        g_cb.failure_count = 0;
    }


    g_mutex_unlock(&g_cb.mutex);
}












void
cb_record_failure(void)
{
    g_mutex_lock(&g_cb.mutex);

    switch (g_cb.state) {

    case CB_STATE_CLOSED:
        g_cb.failure_count++;
        PCV_LOG_WARN(CB_LOG_DOM,
                     "libvirt failure %d/%d",
                     g_cb.failure_count, g_cb.failure_threshold);

        if (g_cb.failure_count >= g_cb.failure_threshold) {
            _transition_to_open();
        }
        break;

    case CB_STATE_HALF_OPEN:



        g_cb.failure_count++;
        g_cb.backoff_ms = MIN(g_cb.backoff_ms * 2, CB_BACKOFF_MAX_MS);
        PCV_LOG_WARN(CB_LOG_DOM,
                     "Probe failed — returning to OPEN (next_backoff=%dms)",
                     g_cb.backoff_ms);
        _transition_to_open();
        break;

    case CB_STATE_OPEN:


        break;
    }

    g_mutex_unlock(&g_cb.mutex);
}


CbState
cb_get_state(void)
{
    return g_cb.state;
}

const gchar *
cb_get_state_str(void)
{
    switch (g_cb.state) {
        case CB_STATE_CLOSED:    return "CLOSED";
        case CB_STATE_OPEN:      return "OPEN";
        case CB_STATE_HALF_OPEN: return "HALF_OPEN";
    }
    unreachable();
}

gint
cb_get_failure_count(void)
{
    return g_atomic_int_get(&g_cb.failure_count);
}

void
cb_set_failure_threshold(gint threshold)
{
    if (threshold < 1)  threshold = 1;
    if (threshold > 50) threshold = 50;
    g_mutex_lock(&g_cb.mutex);
    g_cb.failure_threshold = threshold;
    g_mutex_unlock(&g_cb.mutex);
    PCV_LOG_INFO(CB_LOG_DOM, "Failure threshold updated to %d", threshold);
}

gint
cb_get_failure_threshold(void)
{
    return g_atomic_int_get(&g_cb.failure_threshold);
}






















static GHashTable *g_cb_instances = NULL;
static GMutex      g_cb_instances_mu;




static void
_cb_instance_free(gpointer p)
{
    CbInternal *cb = p;
    if (cb) {
        g_mutex_clear(&cb->mutex);
        g_free(cb);
    }
}









CbState
cb_get_named_state(const gchar *name)
{
    if (!name) return CB_STATE_CLOSED;

    g_mutex_lock(&g_cb_instances_mu);
    if (!g_cb_instances) {
        g_cb_instances = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, _cb_instance_free);
    }

    CbInternal *cb = g_hash_table_lookup(g_cb_instances, name);
    if (!cb) {
        cb = g_new0(CbInternal, 1);
        g_mutex_init(&cb->mutex);
        cb->state = CB_STATE_CLOSED;
        cb->failure_threshold = CB_FAILURE_THRESHOLD_DEFAULT;
        cb->backoff_ms = CB_BACKOFF_INITIAL_MS;
        cb->success_threshold = 3;
        g_hash_table_insert(g_cb_instances, g_strdup(name), cb);
        PCV_LOG_INFO(CB_LOG_DOM, "Named CB instance created: '%s'", name);
    }

    CbState state = cb->state;
    g_mutex_unlock(&g_cb_instances_mu);
    return state;
}










gchar *
cb_get_prometheus_metrics(void)
{
    GString *buf = g_string_new("");
    g_string_append_printf(buf,
        "# HELP purecvisor_cb_state Circuit breaker state (0=CLOSED,1=OPEN,2=HALF_OPEN)\n"
        "# TYPE purecvisor_cb_state gauge\n"
        "purecvisor_cb_state{subsystem=\"libvirt\"} %d\n"
        "# HELP purecvisor_cb_failures Current consecutive failure count\n"
        "# TYPE purecvisor_cb_failures gauge\n"
        "purecvisor_cb_failures{subsystem=\"libvirt\"} %d\n"
        "# HELP purecvisor_cb_failure_threshold Failure threshold for OPEN transition\n"
        "# TYPE purecvisor_cb_failure_threshold gauge\n"
        "purecvisor_cb_failure_threshold{subsystem=\"libvirt\"} %d\n",
        (gint)g_cb.state,
        g_cb.failure_count,
        g_cb.failure_threshold);
    return g_string_free(buf, FALSE);
}

void
cb_shutdown(void)
{
    g_mutex_clear(&g_cb.mutex);


    g_mutex_lock(&g_cb_instances_mu);
    if (g_cb_instances) {
        g_hash_table_destroy(g_cb_instances);
        g_cb_instances = NULL;
    }
    g_mutex_unlock(&g_cb_instances_mu);

    PCV_LOG_INFO(CB_LOG_DOM, "Shutdown.");
}
