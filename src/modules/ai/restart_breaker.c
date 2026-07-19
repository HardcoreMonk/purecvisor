
#include "restart_breaker.h"
#include "utils/pcv_log.h"

#include <glib.h>

#define RB_LOG_DOM "restart_breaker"

typedef struct {
    CbState  state;
    gint     failure_count;
    gint64   open_until_us;
    gboolean probe_in_flight;
} RbEntry;

static struct {
    GHashTable *table;
    GMutex      mutex;
    gint        threshold;
    gint        cooldown_sec;
    gboolean    initialized;
} g_rb = { 0 };

static RbEntry *
_rb_entry(const gchar *uuid)
{
    RbEntry *e = g_hash_table_lookup(g_rb.table, uuid);
    if (!e) {
        e = g_new0(RbEntry, 1);
        e->state = CB_STATE_CLOSED;
        g_hash_table_insert(g_rb.table, g_strdup(uuid), e);
    }
    return e;
}

static void
_rb_open(RbEntry *e, const gchar *uuid)
{
    e->state           = CB_STATE_OPEN;
    e->open_until_us    = g_get_monotonic_time()
                          + (gint64)g_rb.cooldown_sec * G_USEC_PER_SEC;
    e->probe_in_flight  = FALSE;
    PCV_LOG_WARN(RB_LOG_DOM,
        "VM '%s' 재시작 브레이커 OPEN (연속 실패 %d/%d, cooldown %ds)",
        uuid, e->failure_count, g_rb.threshold, g_rb.cooldown_sec);
}

void
rb_init(void)
{
    g_mutex_init(&g_rb.mutex);
    g_rb.table        = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, g_free);
    g_rb.threshold    = RESTART_BREAKER_THRESHOLD_DEFAULT;
    g_rb.cooldown_sec = RESTART_BREAKER_COOLDOWN_SEC_DEFAULT;
    g_rb.initialized  = TRUE;
    PCV_LOG_INFO(RB_LOG_DOM,
        "Initialized (threshold=%d, cooldown=%ds) — [ai] restart_breaker_threshold / restart_breaker_cooldown_sec 로 설정",
        g_rb.threshold, g_rb.cooldown_sec);
}

void
rb_shutdown(void)
{
    if (!g_rb.initialized) return;
    g_mutex_lock(&g_rb.mutex);
    if (g_rb.table) {
        g_hash_table_destroy(g_rb.table);
        g_rb.table = NULL;
    }
    g_rb.initialized = FALSE;
    g_mutex_unlock(&g_rb.mutex);
    g_mutex_clear(&g_rb.mutex);
}

void
rb_configure(gint threshold, gint cooldown_sec)
{
    if (threshold < 1)  threshold = 1;
    if (threshold > 50) threshold = 50;
    if (cooldown_sec < 0) cooldown_sec = 0;
    g_mutex_lock(&g_rb.mutex);
    g_rb.threshold    = threshold;
    g_rb.cooldown_sec = cooldown_sec;
    g_mutex_unlock(&g_rb.mutex);
    PCV_LOG_INFO(RB_LOG_DOM, "Configured (threshold=%d, cooldown=%ds)",
                 threshold, cooldown_sec);
}

gint
rb_get_threshold(void)
{
    g_mutex_lock(&g_rb.mutex);
    gint t = g_rb.threshold;
    g_mutex_unlock(&g_rb.mutex);
    return t;
}

gint
rb_get_cooldown_sec(void)
{
    g_mutex_lock(&g_rb.mutex);
    gint c = g_rb.cooldown_sec;
    g_mutex_unlock(&g_rb.mutex);
    return c;
}

/* PCV_SAFETY_CONTROL: restart-breaker — 임계 초과 실패 시 재시작 차단(FALSE 반환) (AIO-2) */
gboolean
rb_allow(const gchar *uuid)
{
    if (!uuid || !*uuid) return TRUE;
    if (!g_rb.initialized) return TRUE;

    g_mutex_lock(&g_rb.mutex);
    RbEntry *e = _rb_entry(uuid);
    gboolean allow;

    switch (e->state) {
    case CB_STATE_CLOSED:
        allow = TRUE;
        break;

    case CB_STATE_OPEN:
        if (g_get_monotonic_time() >= e->open_until_us) {

            e->state           = CB_STATE_HALF_OPEN;
            e->probe_in_flight = TRUE;
            PCV_LOG_INFO(RB_LOG_DOM,
                "VM '%s' 재시작 브레이커 HALF_OPEN — 프로브 1회 허용", uuid);
            allow = TRUE;
        } else {
            allow = FALSE;
        }
        break;

    case CB_STATE_HALF_OPEN:
        if (e->probe_in_flight) {
            allow = FALSE;
        } else {
            e->probe_in_flight = TRUE;
            allow = TRUE;
        }
        break;

    default:
        allow = TRUE;
        break;
    }

    g_mutex_unlock(&g_rb.mutex);
    return allow;
}

void
rb_record(const gchar *uuid, gboolean success)
{
    if (!uuid || !*uuid) return;
    if (!g_rb.initialized) return;

    g_mutex_lock(&g_rb.mutex);
    RbEntry *e = _rb_entry(uuid);

    if (success) {

        gboolean was_open = (e->state != CB_STATE_CLOSED);
        e->state           = CB_STATE_CLOSED;
        e->failure_count   = 0;
        e->probe_in_flight = FALSE;
        e->open_until_us   = 0;
        if (was_open) {
            PCV_LOG_INFO(RB_LOG_DOM,
                "VM '%s' 재시작 성공 — 브레이커 CLOSED 복귀", uuid);
        }
    } else {
        switch (e->state) {
        case CB_STATE_HALF_OPEN:

            e->failure_count++;
            _rb_open(e, uuid);
            break;
        case CB_STATE_CLOSED:
            e->failure_count++;
            if (e->failure_count >= g_rb.threshold)
                _rb_open(e, uuid);
            else
                PCV_LOG_WARN(RB_LOG_DOM,
                    "VM '%s' 재시작 실패 %d/%d", uuid,
                    e->failure_count, g_rb.threshold);
            break;
        case CB_STATE_OPEN:
        default:

            break;
        }
    }

    g_mutex_unlock(&g_rb.mutex);
}

void
rb_release_probe(const gchar *uuid)
{
    if (!uuid || !*uuid) return;
    if (!g_rb.initialized) return;

    g_mutex_lock(&g_rb.mutex);
    RbEntry *e = g_hash_table_lookup(g_rb.table, uuid);
    if (e && e->state == CB_STATE_HALF_OPEN && e->probe_in_flight) {
        e->state          = CB_STATE_OPEN;
        e->open_until_us   = g_get_monotonic_time()
                             + (gint64)g_rb.cooldown_sec * G_USEC_PER_SEC;
        e->probe_in_flight = FALSE;
        PCV_LOG_INFO(RB_LOG_DOM,
            "VM '%s' 프로브 무결과 중단 — OPEN 유지, cooldown 재무장(토큰 회수)", uuid);
    }
    g_mutex_unlock(&g_rb.mutex);
}

CbState
rb_state(const gchar *uuid)
{
    if (!uuid || !*uuid || !g_rb.initialized) return CB_STATE_CLOSED;
    g_mutex_lock(&g_rb.mutex);
    RbEntry *e = g_hash_table_lookup(g_rb.table, uuid);
    CbState s = e ? e->state : CB_STATE_CLOSED;
    g_mutex_unlock(&g_rb.mutex);
    return s;
}

gint
rb_failure_count(const gchar *uuid)
{
    if (!uuid || !*uuid || !g_rb.initialized) return 0;
    g_mutex_lock(&g_rb.mutex);
    RbEntry *e = g_hash_table_lookup(g_rb.table, uuid);
    gint c = e ? e->failure_count : 0;
    g_mutex_unlock(&g_rb.mutex);
    return c;
}
