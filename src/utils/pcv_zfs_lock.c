










#include "pcv_zfs_lock.h"
#include <string.h>
#include <gio/gio.h>
#include "pcv_log.h"









extern void pcv_prom_gauge_set_labels(const gchar *name, const gchar *labels, gdouble value);

#define ZFS_LOCK_DOM "zfs_lock"

static GHashTable *g_pool_mutexes = NULL;
static GMutex      g_registry_mu;
static gboolean    g_initialized = FALSE;
static gint        g_contentions = 0;

static gchar *
_pool_head(const gchar *pool)
{

    if (!pool || !*pool) return g_strdup("");
    const gchar *slash = strchr(pool, '/');
    if (slash) return g_strndup(pool, (gsize)(slash - pool));
    return g_strdup(pool);
}

void
pcv_zfs_pool_lock_init(void)
{
    if (g_initialized) return;
    g_mutex_init(&g_registry_mu);
    g_pool_mutexes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, NULL);
    g_initialized = TRUE;
    PCV_LOG_INFO(ZFS_LOCK_DOM, "ZFS pool lock module initialized");
}

void
pcv_zfs_pool_lock_shutdown(void)
{
    if (!g_initialized) return;
    g_mutex_lock(&g_registry_mu);
    if (g_pool_mutexes) {

        GHashTableIter iter;
        gpointer k, v;
        g_hash_table_iter_init(&iter, g_pool_mutexes);
        while (g_hash_table_iter_next(&iter, &k, &v)) {
            GMutex *mu = (GMutex *)v;
            if (mu) { g_mutex_clear(mu); g_free(mu); }
        }
        g_hash_table_unref(g_pool_mutexes);
        g_pool_mutexes = NULL;
    }
    g_mutex_unlock(&g_registry_mu);
    g_mutex_clear(&g_registry_mu);
    g_initialized = FALSE;
}

static GMutex *
_get_or_create_mutex(const gchar *pool_head)
{
    g_mutex_lock(&g_registry_mu);
    GMutex *mu = (GMutex *)g_hash_table_lookup(g_pool_mutexes, pool_head);
    if (!mu) {
        mu = g_new0(GMutex, 1);
        g_mutex_init(mu);
        g_hash_table_insert(g_pool_mutexes, g_strdup(pool_head), mu);
    }
    g_mutex_unlock(&g_registry_mu);
    return mu;
}

gboolean
pcv_zfs_pool_lock(const gchar *pool, const gchar *op,
                   gint timeout_ms, GError **error)
{
    if (!g_initialized) pcv_zfs_pool_lock_init();
    if (!pool || !*pool) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "pcv_zfs_pool_lock: pool is NULL/empty");
        return FALSE;
    }

    gchar *head = _pool_head(pool);
    GMutex *mu = _get_or_create_mutex(head);


    if (g_mutex_trylock(mu)) {
        gchar lbl[128];
        g_snprintf(lbl, sizeof(lbl), "pool=\"%s\"", head);
        pcv_prom_gauge_set_labels("purecvisor_zfs_pool_lock_wait_ms", lbl, 0.0);
        g_free(head);
        return TRUE;
    }


    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    gint64 poll_us = 20000;
    gint64 waited_ms = 0;
    while (g_get_monotonic_time() < deadline) {
        g_usleep((gulong)poll_us);
        waited_ms += poll_us / 1000;
        if (g_mutex_trylock(mu)) {
            if (waited_ms >= 1000) {
                PCV_LOG_INFO(ZFS_LOCK_DOM,
                    "pool=%s op=%s acquired after %" G_GINT64_FORMAT "ms wait",
                    head, op ?: "?", waited_ms);
            }

            gchar lbl[128];
            g_snprintf(lbl, sizeof(lbl), "pool=\"%s\"", head);
            pcv_prom_gauge_set_labels("purecvisor_zfs_pool_lock_wait_ms", lbl,
                                       (gdouble)waited_ms);
            g_free(head);
            return TRUE;
        }
    }


    g_atomic_int_inc(&g_contentions);
    pcv_prom_gauge_set_labels("purecvisor_zfs_pool_lock_contentions_total",
                                "", (gdouble)g_atomic_int_get(&g_contentions));
    PCV_LOG_WARN(ZFS_LOCK_DOM,
        "pool=%s op=%s timeout after %dms (another op holds the lock)",
        head, op ?: "?", timeout_ms);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                "zfs pool lock timeout: pool=%s op=%s after %dms",
                head, op ?: "?", timeout_ms);
    g_free(head);
    return FALSE;
}

void
pcv_zfs_pool_unlock(const gchar *pool)
{
    if (!g_initialized || !pool || !*pool) return;
    gchar *head = _pool_head(pool);
    g_mutex_lock(&g_registry_mu);
    GMutex *mu = (GMutex *)g_hash_table_lookup(g_pool_mutexes, head);
    g_mutex_unlock(&g_registry_mu);
    if (mu) g_mutex_unlock(mu);
    else PCV_LOG_WARN(ZFS_LOCK_DOM,
        "pool=%s unlock called but no mutex registered", head);
    g_free(head);
}

void
pcv_zfs_pool_get_stats(gint *registered, gint64 *contentions)
{
    if (registered) {
        g_mutex_lock(&g_registry_mu);
        *registered = g_pool_mutexes ? (gint)g_hash_table_size(g_pool_mutexes) : 0;
        g_mutex_unlock(&g_registry_mu);
    }
    if (contentions) *contentions = (gint64)g_atomic_int_get(&g_contentions);
}
