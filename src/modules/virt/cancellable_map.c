



































#include "cancellable_map.h"
#include "../../utils/pcv_log.h"

#include <glib.h>

#define CMAP_LOG_DOM "cancellable_map"



typedef struct {
    GHashTable *map;
    GMutex      mutex;
    gboolean    initialized;
} CancellableMap;

static CancellableMap g_cmap = { 0 };










void
cmap_init(void)
{
    g_mutex_init(&g_cmap.mutex);

    g_cmap.map = g_hash_table_new_full(
        g_str_hash, g_str_equal,
        g_free,
        (GDestroyNotify)g_object_unref);
    g_cmap.initialized = TRUE;
    PCV_LOG_INFO(CMAP_LOG_DOM, "Initialized.");
}

void
cmap_shutdown(void)
{
    if (!g_cmap.initialized) return;

    g_mutex_lock(&g_cmap.mutex);


    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, g_cmap.map);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GCancellable *c = (GCancellable *)value;
        if (!g_cancellable_is_cancelled(c))
            g_cancellable_cancel(c);
    }
    guint sz = g_hash_table_size(g_cmap.map);
    g_hash_table_destroy(g_cmap.map);
    g_cmap.map = NULL;

    g_mutex_unlock(&g_cmap.mutex);
    g_mutex_clear(&g_cmap.mutex);
    g_cmap.initialized = FALSE;

    PCV_LOG_INFO(CMAP_LOG_DOM, "Shutdown (cancelled %u pending).", sz);
}











void
cmap_register(const gchar *vm_name, GCancellable *cancellable)
{
    g_return_if_fail(vm_name && cancellable);
    if (!g_cmap.initialized) return;

    g_mutex_lock(&g_cmap.mutex);

    g_hash_table_insert(g_cmap.map,
                        g_strdup(vm_name),
                        g_object_ref(cancellable));
    PCV_LOG_DEBUG(CMAP_LOG_DOM, "Registered cancellable for '%s'.", vm_name);
    g_mutex_unlock(&g_cmap.mutex);
}










void
cmap_cancel(const gchar *vm_name)
{
    g_return_if_fail(vm_name);
    if (!g_cmap.initialized) return;

    g_mutex_lock(&g_cmap.mutex);
    GCancellable *c = g_hash_table_lookup(g_cmap.map, vm_name);
    if (c) {
        if (!g_cancellable_is_cancelled(c)) {
            g_cancellable_cancel(c);
            PCV_LOG_INFO(CMAP_LOG_DOM, "Cancelled operation for '%s'.", vm_name);
        }
    } else {
        PCV_LOG_DEBUG(CMAP_LOG_DOM, "cmap_cancel: no entry for '%s'.", vm_name);
    }
    g_mutex_unlock(&g_cmap.mutex);
}

void
cmap_remove(const gchar *vm_name)
{
    g_return_if_fail(vm_name);
    if (!g_cmap.initialized) return;

    g_mutex_lock(&g_cmap.mutex);
    gboolean removed = g_hash_table_remove(g_cmap.map, vm_name);
    g_mutex_unlock(&g_cmap.mutex);

    if (removed)
        PCV_LOG_DEBUG(CMAP_LOG_DOM, "Removed cancellable for '%s'.", vm_name);
}









void
cmap_cancel_all(void)
{
    if (!g_cmap.initialized) return;

    g_mutex_lock(&g_cmap.mutex);
    GHashTableIter iter;
    gpointer key, value;
    guint count = 0;
    g_hash_table_iter_init(&iter, g_cmap.map);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GCancellable *c = (GCancellable *)value;
        if (!g_cancellable_is_cancelled(c)) {
            g_cancellable_cancel(c);
            count++;
        }
    }
    g_mutex_unlock(&g_cmap.mutex);

    if (count > 0)
        PCV_LOG_INFO(CMAP_LOG_DOM, "cmap_cancel_all: cancelled %u operations.", count);
}

guint
cmap_size(void)
{
    if (!g_cmap.initialized) return 0;
    g_mutex_lock(&g_cmap.mutex);
    guint sz = g_hash_table_size(g_cmap.map);
    g_mutex_unlock(&g_cmap.mutex);
    return sz;
}
