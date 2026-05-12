





































#include "pcv_uring.h"

#if PCV_USE_URING

#include "utils/pcv_log.h"
#include <string.h>
#include <sys/mman.h>

#define BUF_LOG_DOM "uring_buf"















PcvUringBufPool *
pcv_uring_buf_pool_new(gint count, gsize buf_size, GError **error)
{
    if (count <= 0 || buf_size == 0) {
        g_set_error(error, g_quark_from_static_string("uring_buf"), 1,
                    "Invalid pool params: count=%d buf_size=%zu", count, buf_size);
        return NULL;
    }





    if (buf_size != 0 && (gsize)count > G_MAXSIZE / buf_size) {
        g_set_error(error, g_quark_from_static_string("uring_buf"), 3,
                    "Pool size overflow: count=%d * buf_size=%zu exceeds gsize max",
                    count, buf_size);
        return NULL;
    }

    PcvUringBufPool *pool = g_new0(PcvUringBufPool, 1);
    g_mutex_init(&pool->mu);
    pool->buf_size = buf_size;
    pool->count    = count;


    gsize total = (gsize)count * buf_size;
    pool->base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pool->base == MAP_FAILED) {
        g_set_error(error, g_quark_from_static_string("uring_buf"), 2,
                    "mmap(%zu) failed", total);
        g_mutex_clear(&pool->mu);
        g_free(pool);
        return NULL;
    }


    pool->free_list = g_new(gint, count);
    for (gint i = 0; i < count; i++)
        pool->free_list[i] = i;
    pool->free_top = count;


    pool->in_use = g_new0(gboolean, count);

    PCV_LOG_INFO(BUF_LOG_DOM, "Pool created: %d x %zu = %zu bytes",
                 count, buf_size, total);
    return pool;
}








void
pcv_uring_buf_pool_free(PcvUringBufPool *pool)
{
    if (!pool) return;

    if (pool->base && pool->base != MAP_FAILED) {
        gsize total = (gsize)pool->count * pool->buf_size;
        munmap(pool->base, total);
    }
    g_free(pool->free_list);
    g_free(pool->in_use);
    g_mutex_clear(&pool->mu);
    g_free(pool);
}














gint
pcv_uring_buf_alloc(PcvUringBufPool *pool)
{
    if (!pool) return -1;

    g_mutex_lock(&pool->mu);
    gint idx = -1;
    if (pool->free_top > 0) {

        pool->free_top--;
        idx = pool->free_list[pool->free_top];

        pool->in_use[idx] = TRUE;
    }
    g_mutex_unlock(&pool->mu);

    if (idx < 0) {

        static gint64 last_warn_us = 0;
        gint64 now = g_get_monotonic_time();
        if (now - last_warn_us > G_USEC_PER_SEC) {
            last_warn_us = now;
            PCV_LOG_WARN(BUF_LOG_DOM, "Buffer pool exhausted (0/%d)",
                         pool->count);
        }
    }
    return idx;
}












void
pcv_uring_buf_release(PcvUringBufPool *pool, gint index)
{
    if (!pool || index < 0 || index >= pool->count) return;

    g_mutex_lock(&pool->mu);



    if (!pool->in_use[index]) {
        g_mutex_unlock(&pool->mu);
        PCV_LOG_WARN(BUF_LOG_DOM,
                     "Double release detected (O(1)): buf idx=%d — ignoring", index);
        return;
    }

    pool->in_use[index] = FALSE;
    if (pool->free_top < pool->count) {

        pool->free_list[pool->free_top] = index;
        pool->free_top++;
    }
    g_mutex_unlock(&pool->mu);
}











void *
pcv_uring_buf_get(PcvUringBufPool *pool, gint index)
{
    if (!pool || index < 0 || index >= pool->count) return NULL;
    return (char *)pool->base + ((gsize)index * pool->buf_size);
}

#endif
