#ifndef PURECVISOR_URING_H
#define PURECVISOR_URING_H

#include <glib.h>

#if defined(HAVE_LIBURING) && defined(PCV_URING_ENABLED)
    #define PCV_USE_URING 1
    #include <liburing.h>
#else
    #define PCV_USE_URING 0
#endif

G_BEGIN_DECLS

#if PCV_USE_URING

#define PCV_URING_BUF_SIZE    65536

#define PCV_URING_BUF_COUNT   64

#define PCV_URING_DEFAULT_QUEUE_DEPTH 1024U

static_assert(PCV_URING_BUF_COUNT >= 1);
static_assert(PCV_URING_BUF_SIZE >= 4096);
static_assert(PCV_URING_DEFAULT_QUEUE_DEPTH >= 1024);

typedef struct {
    void     *base;
    gsize     buf_size;
    gint      count;
    gint     *free_list;
    gint      free_top;

    gboolean *in_use;
    GMutex    mu;
} PcvUringBufPool;

PcvUringBufPool *pcv_uring_buf_pool_new(gint count, gsize buf_size, GError **error);

void             pcv_uring_buf_pool_free(PcvUringBufPool *pool);

gint             pcv_uring_buf_alloc(PcvUringBufPool *pool);

void             pcv_uring_buf_release(PcvUringBufPool *pool, gint index);

void            *pcv_uring_buf_get(PcvUringBufPool *pool, gint index);

typedef struct _PcvUringCtx PcvUringCtx;

typedef void (*PcvUringCallback)(PcvUringCtx *ctx, gint result, gpointer user_data);

typedef struct {
    PcvUringCallback  callback;
    gpointer          user_data;
    gint              buf_index;
} PcvUringPendingOp;

struct _PcvUringCtx {
    struct io_uring   ring;
    int               event_fd;
    guint             glib_source_id;
    PcvUringBufPool  *buf_pool;
    GHashTable       *pending;

    volatile guint    next_id;
    GMutex            submit_mu;
    gboolean          running;
};

PcvUringCtx *pcv_uring_new(guint queue_depth, GError **error);

void         pcv_uring_free(PcvUringCtx *ctx);

gboolean     pcv_uring_is_available(void);

gboolean pcv_uring_submit_read(PcvUringCtx *ctx, int fd, void *buf, gsize len,
                                off_t offset, PcvUringCallback cb, gpointer data);

gboolean pcv_uring_submit_write(PcvUringCtx *ctx, int fd, const void *buf, gsize len,
                                 off_t offset, PcvUringCallback cb, gpointer data);

gboolean pcv_uring_submit_accept(PcvUringCtx *ctx, int listen_fd,
                                  struct sockaddr *addr, socklen_t *addrlen,
                                  PcvUringCallback cb, gpointer data);

gboolean pcv_uring_submit_connect(PcvUringCtx *ctx, int fd,
                                   const struct sockaddr *addr, socklen_t addrlen,
                                   PcvUringCallback cb, gpointer data);

gboolean pcv_uring_submit_send(PcvUringCtx *ctx, int fd, const void *buf, gsize len,
                                PcvUringCallback cb, gpointer data);

gboolean pcv_uring_submit_recv(PcvUringCtx *ctx, int fd, void *buf, gsize len,
                                PcvUringCallback cb, gpointer data);

#else

typedef struct _PcvUringCtx PcvUringCtx;
static inline gboolean pcv_uring_is_available(void) { return FALSE; }

#endif

G_END_DECLS

#endif
