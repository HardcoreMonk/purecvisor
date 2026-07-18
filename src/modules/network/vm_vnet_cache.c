/**
 * @file vm_vnet_cache.c
 * @brief VM→vnet 이름 캐시 — 삭제 시점에 사라진 domiflist 를 대신할 마지막 스냅샷.
 *
 * [무엇을 캐시하는가]
 *   VM 이 살아 있을 때 관찰한 host-side vnet/tap 이름 목록을 vm_name 키로 보관한다.
 *   VM 을 destroy 하면 libvirt 의 domiflist 가 비어버려 정리해야 할 tap 장치를 더는
 *   조회할 수 없다. 이 캐시는 그 직전 스냅샷을 제공해 네트워크 정리(FDB/OVS 포트
 *   제거 등)가 대상 인터페이스를 잃지 않게 한다.
 *
 * [무효화 시점]
 *   - put: VM 관찰 시마다 최신 목록으로 덮어쓴다(기존 값은 free-func 로 해제).
 *   - evict: 정리가 끝나 더는 필요 없을 때 호출자가 명시적으로 비운다.
 *   프로세스 메모리 전용(영속 아님) — 데몬 재시작이면 캐시는 비고, 그 경우
 *   호출자는 libvirt 실시간 조회로 폴백해야 한다.
 *
 * [동시성]
 *   전용 뮤텍스로 보호하며 이 락은 락 순서 최내측(다른 락을 잡은 채 진입 가능,
 *   이 락을 잡은 채 다른 락을 잡지 않는다). get 은 락 안에서 스냅샷을 복사해
 *   반환하므로 호출자는 락 밖에서 안전하게 순회할 수 있다.
 */
#include "vm_vnet_cache.h"

/* vm_name → GPtrArray<gchar*>(vnet 이름들). 전용 뮤텍스로 보호 (락 순서 최내측). */
static GHashTable *g_vnet_cache = NULL;   /* key/value free-func 등록 */
static GMutex      g_vnet_cache_mu;

static void _ensure(void) {
    if (!g_vnet_cache)
        g_vnet_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, (GDestroyNotify)g_ptr_array_unref);
}

/* 깊은 복사: 캐시 내부 배열과 반환 스냅샷이 문자열을 공유하지 않도록 각 이름을
 * 새로 dup 한다. 공유하면 한쪽 free 가 다른 쪽을 dangling 시킨다. */
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
