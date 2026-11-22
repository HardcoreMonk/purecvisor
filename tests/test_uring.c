                                                                                         
                                                                                                     
                                                                          
                                                                         
                          
                     
  
                                                  
  
                 
                                                           
                                                                   
                                                               
  
                                   
  
         
                                                     
                                               
   

#include <glib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
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

static int _fail_submit(struct io_uring *ring __attribute__((unused))) {
    return -EIO;
}

static void _pump_until(gboolean *flag) {
    for (int i = 0; i < 200 && !*flag; i++) {
        g_main_context_iteration(NULL, FALSE);
        if (!*flag) g_usleep(5000);
    }
}

static void test_uring_failed_read_not_replayed(void) {
    GError *err = NULL;
    PcvUringCtx *ctx = pcv_uring_new(16, &err);
    g_assert_no_error(err);
    int p[2];
    g_assert_cmpint(pipe(p), ==, 0);

    char stale[8]; memset(stale, 'X', sizeof(stale));
    pcv_uring_set_submit_fn_for_test(_fail_submit);
    gboolean ok = pcv_uring_submit_read(ctx, p[0], stale, 4, 0, _read_cb, NULL);
    pcv_uring_set_submit_fn_for_test(NULL);
    g_assert_false(ok);
    g_assert_cmpuint(g_hash_table_size(ctx->pending), ==, 0);

    g_assert_cmpint(write(p[1], "abcd", 4), ==, 4);
    char fresh[8] = {0};
    ReadTestData td = {.result = -999, .done = FALSE};
    g_assert_true(pcv_uring_submit_read(ctx, p[0], fresh, 4, 0, _read_cb, &td));
    _pump_until(&td.done);

    g_assert_true(td.done);
    g_assert_cmpint(td.result, ==, 4);
    g_assert_cmpmem(fresh, 4, "abcd", 4);
    for (int i = 0; i < 4; i++) g_assert_cmpint(stale[i], ==, 'X');

    close(p[0]); close(p[1]);
    pcv_uring_free(ctx);
}

static void _write_cb(PcvUringCtx *ctx __attribute__((unused)), gint result, gpointer data) {
    ReadTestData *td = data;
    if (!td) return;
    td->result = result;
    td->done = TRUE;
}

static void test_uring_failed_write_not_replayed(void) {
    GError *err = NULL;
    PcvUringCtx *ctx = pcv_uring_new(16, &err);
    g_assert_no_error(err);
    int p[2];
    g_assert_cmpint(pipe(p), ==, 0);

    pcv_uring_set_submit_fn_for_test(_fail_submit);
    gboolean ok = pcv_uring_submit_write(ctx, p[1], "ZZZZ", 4, 0, _write_cb, NULL);
    pcv_uring_set_submit_fn_for_test(NULL);
    g_assert_false(ok);

    ReadTestData td = {.result = -999, .done = FALSE};
    g_assert_true(pcv_uring_submit_write(ctx, p[1], "ok", 2, 0, _write_cb, &td));
    _pump_until(&td.done);
    g_assert_true(td.done);
    g_assert_cmpint(td.result, ==, 2);

    close(p[1]);
    char got[16] = {0};
    ssize_t n = read(p[0], got, sizeof(got));
    g_assert_cmpint(n, ==, 2);
    g_assert_cmpmem(got, 2, "ok", 2);
    close(p[0]);
    pcv_uring_free(ctx);
}

#define CONC_THREADS 4
#define CONC_PER_THREAD 200

typedef struct { PcvUringCtx *ctx; int fd; gint *done; gint failed; } ConcArg;

static void _conc_cb(PcvUringCtx *ctx __attribute__((unused)), gint result, gpointer data) {
    g_assert_cmpint(result, >=, 0);
    g_atomic_int_inc((gint *)data);
}

static gpointer _conc_thread(gpointer p) {
    ConcArg *a = p;
    static char buf[1];
    for (int i = 0; i < CONC_PER_THREAD; i++) {
        while (!pcv_uring_submit_write(a->ctx, a->fd, buf, 1, 0, _conc_cb, a->done)) {
            a->failed++;
            g_usleep(100);
        }
    }
    return NULL;
}

static void test_uring_concurrent_submit(void) {
    GError *err = NULL;
    PcvUringCtx *ctx = pcv_uring_new(256, &err);
    g_assert_no_error(err);
    int fd = open("/dev/null", O_WRONLY);
    g_assert_cmpint(fd, >=, 0);
    gint done = 0;
    ConcArg args[CONC_THREADS];
    GThread *th[CONC_THREADS];
    for (int t = 0; t < CONC_THREADS; t++) {
        args[t] = (ConcArg){ .ctx = ctx, .fd = fd, .done = &done, .failed = 0 };
        th[t] = g_thread_new("uring-conc", _conc_thread, &args[t]);
    }
    const gint want = CONC_THREADS * CONC_PER_THREAD;
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (g_atomic_int_get(&done) < want && g_get_monotonic_time() < deadline)
        g_main_context_iteration(NULL, FALSE);
    for (int t = 0; t < CONC_THREADS; t++) g_thread_join(th[t]);
    while (g_atomic_int_get(&done) < want && g_get_monotonic_time() < deadline)
        g_main_context_iteration(NULL, FALSE);

    g_assert_cmpint(g_atomic_int_get(&done), ==, want);
    g_mutex_lock(&ctx->submit_mu);
    guint left = g_hash_table_size(ctx->pending);
    g_mutex_unlock(&ctx->submit_mu);
    g_assert_cmpuint(left, ==, 0);
    close(fd);
    pcv_uring_free(ctx);
}

static guint _open_fd_count(void) {
    GDir *d = g_dir_open("/proc/self/fd", 0, NULL);
    g_assert_nonnull(d);
    guint n = 0;
    while (g_dir_read_name(d)) n++;
    g_dir_close(d);
    return n;
}

static int _abstract_listener(struct sockaddr_un *sa, socklen_t *len) {
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    g_assert_cmpint(lfd, >=, 0);
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    int k = snprintf(sa->sun_path + 1, sizeof(sa->sun_path) - 1, "pcv-uring-test-%d-%u",
                     (int)getpid(), g_random_int());
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + k);
    g_assert_cmpint(bind(lfd, (struct sockaddr *)sa, *len), ==, 0);
    g_assert_cmpint(listen(lfd, 4), ==, 0);
    return lfd;
}

static void _accept_must_not_run(PcvUringCtx *ctx __attribute__((unused)), gint result,
                                 gpointer data __attribute__((unused))) {
    g_error("accept callback ran during free (result=%d)", result);
}

static void test_uring_free_reaps_completed_accept(void) {
    struct sockaddr_un sa; socklen_t salen;
    int lfd = _abstract_listener(&sa, &salen);
    GError *err = NULL;
    PcvUringCtx *ctx = pcv_uring_new(16, &err);
    g_assert_no_error(err);
    guint base = _open_fd_count();

    g_assert_true(pcv_uring_submit_accept(ctx, lfd, NULL, NULL, _accept_must_not_run, NULL));
    int cfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    g_assert_cmpint(connect(cfd, (struct sockaddr *)&sa, salen), ==, 0);

    for (int i = 0; i < 100 && io_uring_cq_ready(&ctx->ring) == 0; i++) g_usleep(2000);
    g_assert_cmpuint(io_uring_cq_ready(&ctx->ring), >=, 1);
    g_assert_cmpuint(_open_fd_count(), ==, base + 2);

    pcv_uring_free(ctx);

    g_assert_cmpuint(_open_fd_count(), ==, base - 2 + 1);
    close(cfd); close(lfd);
}

static void test_uring_free_cancels_inflight_accept(void) {
    struct sockaddr_un sa; socklen_t salen;
    int lfd = _abstract_listener(&sa, &salen);
    GError *err = NULL;
    PcvUringCtx *ctx = pcv_uring_new(16, &err);
    g_assert_no_error(err);
    guint base = _open_fd_count();
    g_assert_true(pcv_uring_submit_accept(ctx, lfd, NULL, NULL, _accept_must_not_run, NULL));
    gint64 t0 = g_get_monotonic_time();
    pcv_uring_free(ctx);
    gint64 took = g_get_monotonic_time() - t0;
    g_assert_cmpint(took, <, 1 * G_TIME_SPAN_SECOND);
    g_assert_cmpuint(_open_fd_count(), ==, base - 2);

    int cfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    g_assert_cmpint(connect(cfd, (struct sockaddr *)&sa, salen), ==, 0);
    int afd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    g_assert_cmpint(afd, >=, 0);
    close(afd); close(cfd); close(lfd);
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
    g_test_add_func("/uring/failed_read_not_replayed",  test_uring_failed_read_not_replayed);
    g_test_add_func("/uring/failed_write_not_replayed", test_uring_failed_write_not_replayed);
    g_test_add_func("/uring/concurrent_submit",         test_uring_concurrent_submit);
    g_test_add_func("/uring/free_reaps_completed_accept", test_uring_free_reaps_completed_accept);
    g_test_add_func("/uring/free_cancels_inflight_accept", test_uring_free_cancels_inflight_accept);
#endif
}
