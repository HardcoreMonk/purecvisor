
#include "vm_vnet_cache.h"

static GHashTable *g_vnet_cache = NULL;
static GMutex      g_vnet_cache_mu;

static void _ensure(void) {
    if (!g_vnet_cache)
        g_vnet_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, (GDestroyNotify)g_ptr_array_unref);
}

static GPtrArray *_copy(GPtrArray *src) {
    GPtrArray *dst = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; src && i < src->len; i++)
        g_ptr_array_add(dst, g_strdup(g_ptr_array_index(src, i)));
    return dst;
}

void
pcv_vm_vnet_cache_put(const gchar *vm, GPtrArray *vnets)
{
    if (!vm) return;
    g_mutex_lock(&g_vnet_cache_mu);
    _ensure();
    g_hash_table_insert(g_vnet_cache, g_strdup(vm), _copy(vnets));
    g_mutex_unlock(&g_vnet_cache_mu);
}

GPtrArray *
pcv_vm_vnet_cache_get(const gchar *vm)
{
    if (!vm) return NULL;
    g_mutex_lock(&g_vnet_cache_mu);
    _ensure();
    GPtrArray *hit = g_hash_table_lookup(g_vnet_cache, vm);
    GPtrArray *snap = hit ? _copy(hit) : NULL;
    g_mutex_unlock(&g_vnet_cache_mu);
    return snap;
}

void
pcv_vm_vnet_cache_evict(const gchar *vm)
{
    if (!vm) return;
    g_mutex_lock(&g_vnet_cache_mu);
    _ensure();
    g_hash_table_remove(g_vnet_cache, vm);
    g_mutex_unlock(&g_vnet_cache_mu);
}
