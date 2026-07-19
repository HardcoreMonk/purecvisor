
#include "pcv_uring.h"

#if PCV_USE_URING

#include "utils/pcv_log.h"
#include <string.h>
#include <errno.h>

#define SOCK_LOG_DOM "uring_socket"

extern guint64 _register_pending(PcvUringCtx *ctx, PcvUringCallback cb,
                                  gpointer data, gint buf_idx);
extern gboolean _submit_ring(PcvUringCtx *ctx);

gboolean
pcv_uring_submit_accept(PcvUringCtx *ctx, int listen_fd,
                         struct sockaddr *addr, socklen_t *addrlen,
                         PcvUringCallback cb, gpointer data)
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
            PCV_LOG_WARN(SOCK_LOG_DOM, "SQ still full after submit (accept) — dropping");
            return FALSE;
        }
    }

    io_uring_prep_accept(sqe, listen_fd, addr, addrlen, 0);
    io_uring_sqe_set_data64(sqe, id);

    gboolean ok = _submit_ring(ctx);
    if (!ok) {
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));

        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data64(sqe, 0);
    }
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

gboolean
pcv_uring_submit_connect(PcvUringCtx *ctx, int fd,
                          const struct sockaddr *addr, socklen_t addrlen,
                          PcvUringCallback cb, gpointer data)
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
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
        g_mutex_unlock(&ctx->submit_mu);
        PCV_LOG_WARN(SOCK_LOG_DOM, "SQ full (connect)");
        return FALSE;
    }

    io_uring_prep_connect(sqe, fd, addr, addrlen);
    io_uring_sqe_set_data64(sqe, id);

    gboolean ok = _submit_ring(ctx);
    if (!ok) {
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));

        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data64(sqe, 0);
    }
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

gboolean
pcv_uring_submit_send(PcvUringCtx *ctx, int fd, const void *buf, gsize len,
                       PcvUringCallback cb, gpointer data)
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
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
        g_mutex_unlock(&ctx->submit_mu);
        PCV_LOG_WARN(SOCK_LOG_DOM, "SQ full (send)");
        return FALSE;
    }

    io_uring_prep_send(sqe, fd, buf, (unsigned)len, 0);
    io_uring_sqe_set_data64(sqe, id);

    gboolean ok = _submit_ring(ctx);
    if (!ok) {
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));

        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data64(sqe, 0);
    }
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

gboolean
pcv_uring_submit_recv(PcvUringCtx *ctx, int fd, void *buf, gsize len,
                       PcvUringCallback cb, gpointer data)
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
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
        g_mutex_unlock(&ctx->submit_mu);
        PCV_LOG_WARN(SOCK_LOG_DOM, "SQ full (recv)");
        return FALSE;
    }

    io_uring_prep_recv(sqe, fd, buf, (unsigned)len, 0);
    io_uring_sqe_set_data64(sqe, id);

    gboolean ok = _submit_ring(ctx);
    if (!ok) {
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));

        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data64(sqe, 0);
    }
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

#endif
