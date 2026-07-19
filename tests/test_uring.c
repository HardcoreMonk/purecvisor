
#include <glib.h>
#include <fcntl.h>
#include <unistd.h>
#include "io/pcv_uring.h"

static void test_uring_availability(void) {
#if PCV_USE_URING
    g_assert_true(pcv_uring_is_available());
#else
    g_assert_false(pcv_uring_is_available());
#endif
}

#if PCV_USE_URING

static void test_uring_default_queue_depth(void) {
    g_assert_cmpuint(PCV_URING_DEFAULT_QUEUE_DEPTH, >=, 1024);
}

static void test_uring_create_destroy(void) {
    GError *err = NULL;
    PcvUringCtx *ctx = pcv_uring_new(32, &err);
    g_assert_no_error(err);
    g_assert_nonnull(ctx);
    g_assert_true(ctx->running);
    g_assert_cmpint(ctx->event_fd, >=, 0);
    g_assert_nonnull(ctx->pending);
    pcv_uring_free(ctx);
}

static void test_uring_buf_pool(void) {
    GError *err = NULL;
    PcvUringBufPool *pool = pcv_uring_buf_pool_new(4, 1024, &err);
    g_assert_no_error(err);
    g_assert_nonnull(pool);

    gint idx0 = pcv_uring_buf_alloc(pool);
    gint idx1 = pcv_uring_buf_alloc(pool);
    gint idx2 = pcv_uring_buf_alloc(pool);
    gint idx3 = pcv_uring_buf_alloc(pool);
    g_assert_cmpint(idx0, >=, 0);
    g_assert_cmpint(idx1, >=, 0);
    g_assert_cmpint(idx2, >=, 0);
    g_assert_cmpint(idx3, >=, 0);

    gint idx4 = pcv_uring_buf_alloc(pool);
    g_assert_cmpint(idx4, ==, -1);

    void *buf0 = pcv_uring_buf_get(pool, idx0);
    void *buf1 = pcv_uring_buf_get(pool, idx1);
    g_assert_nonnull(buf0);
    g_assert_nonnull(buf1);
    g_assert_true(buf0 != buf1);

    pcv_uring_buf_release(pool, idx0);
    gint idx5 = pcv_uring_buf_alloc(pool);
    g_assert_cmpint(idx5, ==, idx0);

    pcv_uring_buf_pool_free(pool);
}

static void test_uring_buf_invalid(void) {
    GError *err = NULL;

    PcvUringBufPool *pool = pcv_uring_buf_pool_new(0, 1024, &err);
    g_assert_null(pool);
    g_assert_nonnull(err);
    g_error_free(err);

    g_assert_cmpint(pcv_uring_buf_alloc(NULL), ==, -1);
    g_assert_null(pcv_uring_buf_get(NULL, 0));
}

static void test_uring_eventfd(void) {
    GError *err = NULL;
    PcvUringCtx *ctx = pcv_uring_new(16, &err);
    g_assert_no_error(err);
    g_assert_nonnull(ctx);

    g_assert_cmpint(ctx->event_fd, >, 0);
    g_assert_cmpuint(ctx->glib_source_id, >, 0);

    pcv_uring_free(ctx);
}

typedef struct {
    gint result;
    gboolean done;
} ReadTestData;

static void _read_cb(PcvUringCtx *ctx __attribute__((unused)), gint result, gpointer data) {
    ReadTestData *td = data;
    td->result = result;
    td->done = TRUE;
}

static void test_uring_file_read(void) {
    GError *err = NULL;
    PcvUringCtx *ctx = pcv_uring_new(16, &err);
    g_assert_no_error(err);

    int fd = open("/proc/self/status", O_RDONLY);
    g_assert_cmpint(fd, >=, 0);

    char buf[256] = {0};
    ReadTestData td = {.result = -999, .done = FALSE};

    gboolean ok = pcv_uring_submit_read(ctx, fd, buf, sizeof(buf) - 1, 0,
                                         _read_cb, &td);
    g_assert_true(ok);

    for (int i = 0; i < 100 && !td.done; i++) {

        g_main_context_iteration(NULL, FALSE);
        if (!td.done)
            g_usleep(10000);
    }

    g_assert_true(td.done);
    g_assert_cmpint(td.result, >, 0);
    g_assert_true(g_str_has_prefix(buf, "Name:"));

    close(fd);
    pcv_uring_free(ctx);
}

static void test_uring_pending_overflow(void) {
    GError *err = NULL;

    PcvUringCtx *ctx = pcv_uring_new(4, &err);
    g_assert_no_error(err);
    g_assert_nonnull(ctx);

    int fd = open("/dev/null", O_RDONLY);
    g_assert_cmpint(fd, >=, 0);

    char buf[64] = {0};

    guint success_count = 0;
    for (int i = 0; i < 6; i++) {
        gboolean ok = pcv_uring_submit_read(ctx, fd, buf, sizeof(buf) - 1, 0,
                                             NULL, NULL);
        if (ok)
            success_count++;
    }

    g_assert_cmpuint(success_count, >=, 1);

    g_assert_nonnull(ctx->pending);

    close(fd);
    pcv_uring_free(ctx);
}

static void test_uring_buf_double_release(void) {
    GError *err = NULL;
    PcvUringBufPool *pool = pcv_uring_buf_pool_new(4, 1024, &err);
    g_assert_no_error(err);
    g_assert_nonnull(pool);

    gint idx = pcv_uring_buf_alloc(pool);
    g_assert_cmpint(idx, >=, 0);

    pcv_uring_buf_release(pool, idx);
    pcv_uring_buf_release(pool, idx);

    gint idx2 = pcv_uring_buf_alloc(pool);
    g_assert_cmpint(idx2, >=, 0);

    pcv_uring_buf_pool_free(pool);
}

static void test_uring_sq_full(void) {
    GError *err = NULL;

    PcvUringCtx *ctx = pcv_uring_new(2, &err);
    g_assert_no_error(err);

    char buf[64];
    int fd = open("/dev/null", O_RDONLY);

    gboolean ok1 = pcv_uring_submit_read(ctx, fd, buf, 1, 0, NULL, NULL);
    gboolean ok2 = pcv_uring_submit_read(ctx, fd, buf, 1, 0, NULL, NULL);
    g_assert_true(ok1);
    g_assert_true(ok2);

    pcv_uring_submit_read(ctx, fd, buf, 1, 0, NULL, NULL);

    close(fd);
    pcv_uring_free(ctx);
}

#endif

void test_uring_register(void) {
    g_test_add_func("/uring/availability",     test_uring_availability);
#if PCV_USE_URING
    g_test_add_func("/uring/default_queue_depth", test_uring_default_queue_depth);
    g_test_add_func("/uring/create_destroy",   test_uring_create_destroy);
    g_test_add_func("/uring/buf_pool",         test_uring_buf_pool);
    g_test_add_func("/uring/buf_invalid",      test_uring_buf_invalid);
    g_test_add_func("/uring/eventfd",          test_uring_eventfd);
    g_test_add_func("/uring/file_read",        test_uring_file_read);
    g_test_add_func("/uring/sq_full",          test_uring_sq_full);
    g_test_add_func("/uring/pending_overflow", test_uring_pending_overflow);
    g_test_add_func("/uring/buf_double_release", test_uring_buf_double_release);
#endif
}
