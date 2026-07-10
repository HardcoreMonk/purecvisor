#include "vm_vnet_cache.h"

/* vm_name → GPtrArray<gchar*>(vnet 이름들). 전용 뮤텍스로 보호 (락 순서 최내측). */
static GHashTable *g_vnet_cache = NULL;   /* key/value free-func 등록 */
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
    g_hash_table_insert(g_vnet_cache, g_strdup(vm), _copy(vnets));  /* 기존 키 값은 free-func 해제 */
    g_mutex_unlock(&g_vnet_cache_mu);
}

GPtrArray *
pcv_vm_vnet_cache_get(const gchar *vm)
{
    if (!vm) return NULL;
    g_mutex_lock(&g_vnet_cache_mu);
    _ensure();
    GPtrArray *hit = g_hash_table_lookup(g_vnet_cache, vm);
    GPtrArray *snap = hit ? _copy(hit) : NULL;   /* 스냅샷 복사 — 호출자는 락 밖에서 안전 */
    g_mutex_unlock(&g_vnet_cache_mu);
    return snap;
}

void
pcv_vm_vnet_cache_evict(const gchar *vm)
{
    if (!vm) return;
    g_mutex_lock(&g_vnet_cache_mu);
    _ensure();
    g_hash_table_remove(g_vnet_cache, vm);   /* 없는 키는 무해 */
    g_mutex_unlock(&g_vnet_cache_mu);
}
