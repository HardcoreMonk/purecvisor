
#include "virt_conn_pool.h"
#include "circuit_breaker.h"
#include "../../utils/pcv_log.h"
#include <glib.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#define POOL_URI "qemu:///system"

typedef struct {
    GQueue  *idle;
    GMutex   mutex;
    GCond    cond;
    guint    max_size;
    guint    total;
    gboolean shutdown;
    gdouble  wait_total_us;
    guint64  wait_count;
} ConnPool;

static ConnPool g_pool = { 0 };

static virConnectPtr pool_new_conn(void) {

    if (cb_is_open()) {
        PCV_LOG_WARN("conn_pool",
                     "Circuit OPEN — libvirt connection blocked (state=%s, failures=%d)",
                     cb_get_state_str(), cb_get_failure_count());
        return NULL;
    }

    const gchar *uri_env = g_getenv("PCV_LIBVIRT_URI");
    virConnectPtr conn = virConnectOpen((uri_env && *uri_env) ? uri_env : POOL_URI);
    if (!conn) {
        PCV_LOG_WARN("conn_pool", "virConnectOpen failed: %s",
                     virGetLastError() ? virGetLastError()->message : "unknown");
        cb_record_failure();
        return NULL;
    }

    cb_record_success();
    return conn;
}

static gboolean
_pool_idle_reaper(gpointer user_data)
{
    (void)user_data;
    g_mutex_lock(&g_pool.mutex);

    if (g_pool.shutdown) {
        g_mutex_unlock(&g_pool.mutex);
        return G_SOURCE_REMOVE;
    }

    guint len = g_queue_get_length(g_pool.idle);
    for (guint i = 0; i < len; i++) {
        virConnectPtr conn = g_queue_pop_head(g_pool.idle);
        if (!conn) continue;

        if (virConnectIsAlive(conn) == 1) {
            g_queue_push_tail(g_pool.idle, conn);
        } else {
            PCV_LOG_INFO("conn_pool", "Reaping stale idle connection (slot %u/%u)", i, len);
            g_mutex_unlock(&g_pool.mutex);
            virConnectClose(conn);

            virConnectPtr fresh = pool_new_conn();
            g_mutex_lock(&g_pool.mutex);
            if (fresh) {
                g_queue_push_tail(g_pool.idle, fresh);
            } else {
                PCV_LOG_WARN("conn_pool", "Reconnect failed during idle reap");
                g_pool.total--;
            }
        }
    }

    g_mutex_unlock(&g_pool.mutex);
    return G_SOURCE_CONTINUE;
}

void virt_conn_pool_init(guint max_size) {
    cb_init();
    g_mutex_init(&g_pool.mutex);
    g_cond_init(&g_pool.cond);
    g_pool.idle     = g_queue_new();
    g_pool.max_size = (max_size > 0) ? max_size : 8;
    g_pool.total    = 0;
    g_pool.shutdown = FALSE;

    virConnectPtr conn = pool_new_conn();
    if (conn) {
        g_queue_push_tail(g_pool.idle, conn);
        g_pool.total = 1;
    }

    g_timeout_add_seconds(30, _pool_idle_reaper, NULL);

    PCV_LOG_INFO("conn_pool", "Initialized (max=%u, pre-opened=%u)",
              g_pool.max_size, g_pool.total);
}

virConnectPtr virt_conn_pool_acquire(void) {
    g_mutex_lock(&g_pool.mutex);

    while (!g_pool.shutdown) {

        if (!g_queue_is_empty(g_pool.idle)) {
            virConnectPtr conn = g_queue_pop_head(g_pool.idle);

            if (conn && virConnectIsAlive(conn) == 1) {
                unsigned long ver = 0;
                if (virConnectGetVersion(conn, &ver) >= 0) {
                    cb_record_success();
                    g_mutex_unlock(&g_pool.mutex);
                    return conn;
                }
            }

            if (conn) {
                PCV_LOG_WARN("conn_pool",
                             "Stale connection detected during acquire — reconnecting...");
                virConnectClose(conn);
            }
            virConnectPtr fresh = pool_new_conn();
            if (fresh) {
                g_mutex_unlock(&g_pool.mutex);
                return fresh;
            }

            g_pool.total--;
            g_mutex_unlock(&g_pool.mutex);
            return NULL;
        }

        if (g_pool.total < g_pool.max_size) {
            g_pool.total++;
            g_mutex_unlock(&g_pool.mutex);

            virConnectPtr conn = pool_new_conn();
            if (!conn) {
                g_mutex_lock(&g_pool.mutex);
                g_pool.total--;
                g_mutex_unlock(&g_pool.mutex);
                return NULL;
            }
            return conn;
        }

        gint64 wait_start = g_get_monotonic_time();
        gint64 deadline = wait_start + 10 * G_TIME_SPAN_SECOND;
        if (!g_cond_wait_until(&g_pool.cond, &g_pool.mutex, deadline)) {

            gint64 waited = g_get_monotonic_time() - wait_start;
            g_pool.wait_total_us += (gdouble)waited;
            g_pool.wait_count++;
            PCV_LOG_WARN("conn_pool", "acquire() timed out waiting for a connection");
            g_mutex_unlock(&g_pool.mutex);
            return NULL;
        }

        {
            gint64 waited = g_get_monotonic_time() - wait_start;
            g_pool.wait_total_us += (gdouble)waited;
            g_pool.wait_count++;
        }
    }

    g_mutex_unlock(&g_pool.mutex);
    return NULL;
}

void virt_conn_pool_release(virConnectPtr conn) {
    if (!conn) return;

    g_mutex_lock(&g_pool.mutex);

    if (g_pool.shutdown) {

        g_pool.total--;
        g_mutex_unlock(&g_pool.mutex);
        virConnectClose(conn);
        return;
    }

    g_queue_push_tail(g_pool.idle, conn);
    g_cond_signal(&g_pool.cond);

    g_mutex_unlock(&g_pool.mutex);
}

void virt_conn_pool_shutdown(void) {
    g_mutex_lock(&g_pool.mutex);
    g_pool.shutdown = TRUE;

    g_cond_broadcast(&g_pool.cond);
    g_mutex_unlock(&g_pool.mutex);

    virConnectPtr conn;
    while ((conn = g_queue_pop_head(g_pool.idle)) != NULL) {
        virConnectClose(conn);
        g_pool.total--;
    }

    g_queue_free(g_pool.idle);
    g_pool.idle = NULL;
    g_mutex_clear(&g_pool.mutex);
    g_cond_clear(&g_pool.cond);

    cb_shutdown();
    PCV_LOG_INFO("conn_pool", "Shutdown complete.");
}

void virt_conn_pool_stats(guint *out_idle, guint *out_total, guint *out_max) {
    g_mutex_lock(&g_pool.mutex);
    if (out_idle)  *out_idle  = g_queue_get_length(g_pool.idle);
    if (out_total) *out_total = g_pool.total;
    if (out_max)   *out_max   = g_pool.max_size;
    g_mutex_unlock(&g_pool.mutex);
}

gdouble virt_conn_pool_wait_avg_seconds(void) {
    g_mutex_lock(&g_pool.mutex);
    gdouble avg = (g_pool.wait_count > 0)
                  ? (g_pool.wait_total_us / (gdouble)g_pool.wait_count) / 1e6
                  : 0.0;
    g_mutex_unlock(&g_pool.mutex);
    return avg;
}
