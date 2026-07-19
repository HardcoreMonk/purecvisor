
#include "pcv_uring.h"

#if PCV_USE_URING

#include "utils/pcv_log.h"
#include <glib-unix.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define URING_LOG_DOM "io_uring"

static PcvUringPendingOp *
_pending_new(PcvUringCallback cb, gpointer data, gint buf_idx)
{
    PcvUringPendingOp *op = g_new0(PcvUringPendingOp, 1);
    op->callback  = cb;
    op->user_data = data;
    op->buf_index = buf_idx;
    return op;
}

static void
_pending_free(gpointer p)
{
    g_free(p);
}

static gboolean
_on_uring_ready(gint fd, GIOCondition cond __attribute__((unused)), gpointer data)
{
    PcvUringCtx *ctx = data;

    uint64_t val;
    ssize_t __attribute__((unused)) _rn = read(fd, &val, sizeof(val));

    struct io_uring_cqe *cqe;
    unsigned head;

    io_uring_for_each_cqe(&ctx->ring, head, cqe) {

        guint sqe_id = (guint)io_uring_cqe_get_data64(cqe);

        PcvUringPendingOp *op = g_hash_table_lookup(ctx->pending,
                                                     GUINT_TO_POINTER(sqe_id));
        if (op) {

            if (op->callback)
                op->callback(ctx, cqe->res, op->user_data);

            if (op->buf_index >= 0 && ctx->buf_pool)
                pcv_uring_buf_release(ctx->buf_pool, op->buf_index);

            g_hash_table_remove(ctx->pending, GUINT_TO_POINTER(sqe_id));
        }

        io_uring_cqe_seen(&ctx->ring, cqe);
    }

    return G_SOURCE_CONTINUE;
}

guint64
_register_pending(PcvUringCtx *ctx, PcvUringCallback cb, gpointer data, gint buf_idx)
{

    guint id = (guint)g_atomic_int_add((gint *)&ctx->next_id, 1);
    if (id == 0) {

        id = (guint)g_atomic_int_add((gint *)&ctx->next_id, 1);
    }

    int skip = 0;
    while (g_hash_table_contains(ctx->pending, GUINT_TO_POINTER(id)) && skip < 1024) {
        id = (guint)g_atomic_int_add((gint *)&ctx->next_id, 1);
        if (id == 0)
            id = (guint)g_atomic_int_add((gint *)&ctx->next_id, 1);
        skip++;
    }
    if (skip > 0) {
        PCV_LOG_WARN("uring", "ID collision skip=%d (pending=%u)",
                     skip, g_hash_table_size(ctx->pending));
    }

    if (g_hash_table_contains(ctx->pending, GUINT_TO_POINTER(id))) {
        PCV_LOG_WARN("uring", "pending full — refusing registration (size=%u)",
                     g_hash_table_size(ctx->pending));
        return 0;
    }
    PcvUringPendingOp *op = _pending_new(cb, data, buf_idx);
    g_hash_table_insert(ctx->pending, GUINT_TO_POINTER(id), op);
    return (guint64)id;
}

gboolean
_submit_ring(PcvUringCtx *ctx)
{
    int ret = io_uring_submit(&ctx->ring);
    if (ret >= 0)
        return TRUE;

    if (ret == -EAGAIN) {

        PCV_LOG_WARN(URING_LOG_DOM, "io_uring_submit returned EAGAIN, retrying after 1ms yield");
        g_usleep(1000);
        ret = io_uring_submit(&ctx->ring);
        if (ret >= 0)
            return TRUE;
        PCV_LOG_WARN(URING_LOG_DOM, "io_uring_submit retry failed: %s", strerror(-ret));
        return FALSE;
    }

    PCV_LOG_WARN(URING_LOG_DOM, "io_uring_submit failed: %s", strerror(-ret));
    return FALSE;
}

PcvUringCtx *
pcv_uring_new(guint queue_depth, GError **error)
{
    PcvUringCtx *ctx = g_new0(PcvUringCtx, 1);
    g_mutex_init(&ctx->submit_mu);
    ctx->event_fd = -1;
    ctx->glib_source_id = 0;
    ctx->next_id = 1;

    const unsigned int setup_flags = 0;
    int ret = io_uring_queue_init(queue_depth, &ctx->ring, setup_flags);
    if (ret < 0) {
        g_set_error(error, g_quark_from_static_string("uring"), 1,
                    "io_uring_queue_init(%u) failed: %s", queue_depth, strerror(-ret));
        g_free(ctx);
        return NULL;
    }
    g_assert((ctx->ring.flags & IORING_SETUP_SQPOLL) == 0);

    ctx->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ctx->event_fd < 0) {
        g_set_error(error, g_quark_from_static_string("uring"), 2,
                    "eventfd() failed: %s", strerror(errno));
        io_uring_queue_exit(&ctx->ring);
        g_free(ctx);
        return NULL;
    }

    ret = io_uring_register_eventfd(&ctx->ring, ctx->event_fd);
    if (ret < 0) {
        g_set_error(error, g_quark_from_static_string("uring"), 3,
                    "io_uring_register_eventfd failed: %s", strerror(-ret));
        close(ctx->event_fd);
        io_uring_queue_exit(&ctx->ring);
        g_free(ctx);
        return NULL;
    }

    ctx->glib_source_id = g_unix_fd_add(ctx->event_fd, G_IO_IN,
                                         _on_uring_ready, ctx);

    ctx->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, _pending_free);

    GError *buf_err = NULL;
    ctx->buf_pool = pcv_uring_buf_pool_new(PCV_URING_BUF_COUNT,
                                            PCV_URING_BUF_SIZE, &buf_err);
    if (!ctx->buf_pool) {
        PCV_LOG_WARN(URING_LOG_DOM, "Buffer pool init failed: %s",
                     buf_err ? buf_err->message : "unknown");
        if (buf_err) g_error_free(buf_err);

    }

    ctx->running = TRUE;
    PCV_LOG_INFO(URING_LOG_DOM, "Initialized (queue_depth=%u, eventfd=%d, buf_pool=%s)",
                 queue_depth, ctx->event_fd,
                 ctx->buf_pool ? "OK" : "NONE");
    return ctx;
}

void
pcv_uring_free(PcvUringCtx *ctx)
{
    if (!ctx) return;
    ctx->running = FALSE;

    if (ctx->glib_source_id > 0)
        g_source_remove(ctx->glib_source_id);

    if (ctx->event_fd >= 0)
        close(ctx->event_fd);

    if (ctx->pending) {
        guint remaining = g_hash_table_size(ctx->pending);
        if (remaining > 0)
            PCV_LOG_WARN(URING_LOG_DOM, "Shutdown with %u pending ops", remaining);
        g_hash_table_destroy(ctx->pending);
    }

    if (ctx->buf_pool)
        pcv_uring_buf_pool_free(ctx->buf_pool);

    io_uring_queue_exit(&ctx->ring);

    g_mutex_clear(&ctx->submit_mu);
    g_free(ctx);
    PCV_LOG_INFO(URING_LOG_DOM, "Shutdown complete");
}

gboolean
pcv_uring_is_available(void)
{
    return TRUE;
}

gboolean
pcv_uring_submit_read(PcvUringCtx *ctx, int fd, void *buf, gsize len,
                       off_t offset, PcvUringCallback cb, gpointer data)
{
    if (!ctx || !ctx->running) return FALSE;

    g_mutex_lock(&ctx->submit_mu);

    guint64 id = _register_pending(ctx, cb, data, -1);
    if (id == 0) {
        g_mutex_unlock(&ctx->submit_mu);
        return FALSE;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {

        io_uring_submit(&ctx->ring);
        sqe = io_uring_get_sqe(&ctx->ring);
        if (!sqe) {

            g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
            g_mutex_unlock(&ctx->submit_mu);
            PCV_LOG_WARN(URING_LOG_DOM, "SQ still full after submit (read) — dropping request");
            return FALSE;
        }
    }

    io_uring_prep_read(sqe, fd, buf, (unsigned)len, offset);
    io_uring_sqe_set_data64(sqe, id);

    gboolean ok = _submit_ring(ctx);
    if (!ok) {

        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
    }
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

gboolean
pcv_uring_submit_write(PcvUringCtx *ctx, int fd, const void *buf, gsize len,
                        off_t offset, PcvUringCallback cb, gpointer data)
{
    if (!ctx || !ctx->running) return FALSE;

    g_mutex_lock(&ctx->submit_mu);

    guint64 id = _register_pending(ctx, cb, data, -1);
    if (id == 0) {
        g_mutex_unlock(&ctx->submit_mu);
        return FALSE;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {

        io_uring_submit(&ctx->ring);
        sqe = io_uring_get_sqe(&ctx->ring);
        if (!sqe) {
            g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
            g_mutex_unlock(&ctx->submit_mu);
            PCV_LOG_WARN(URING_LOG_DOM, "SQ still full after submit (write) — dropping request");
            return FALSE;
        }
    }

    io_uring_prep_write(sqe, fd, buf, (unsigned)len, offset);
    io_uring_sqe_set_data64(sqe, id);

    gboolean ok = _submit_ring(ctx);
    if (!ok) {

        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
    }
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

#endif
