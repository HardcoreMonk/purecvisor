/* tests/test_uring.c
 *
 * 대상 모듈: src/io/pcv_uring.c — io_uring 비동기 I/O 래퍼
 *
 * 이 테스트가 검증하는 것:
 *   PCV_USE_URING=1일 때: ring 생성/해제, 버퍼 풀 alloc/release/경계,
 *   eventfd+GMainLoop 연동, /proc/self/status 비동기 파일 읽기, SQ full 방어.
 *   PCV_USE_URING=0일 때: pcv_uring_is_available() == FALSE만 확인.
 *
 * 실행: sudo ./test_runner -p /uring
 *
 * 외부 의존:
 *   - io_uring 커널 지원 (5.1+), PCV_USE_URING=1 컴파일 플래그
 *   - /proc/self/status, /dev/null (파일 읽기 테스트)
 */

#include <glib.h>
#include <fcntl.h>
#include <unistd.h>
#include "io/pcv_uring.h"

/* ── 가용성 확인 ──────────────────────────────────────── */

static void test_uring_availability(void) {
#if PCV_USE_URING
    g_assert_true(pcv_uring_is_available());
#else
    g_assert_false(pcv_uring_is_available());
#endif
}

#if PCV_USE_URING

/* ── ring 생성/해제 ───────────────────────────────────── */

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

/* ── 버퍼 풀 ──────────────────────────────────────────── */

static void test_uring_buf_pool(void) {
    GError *err = NULL;
    PcvUringBufPool *pool = pcv_uring_buf_pool_new(4, 1024, &err);
    g_assert_no_error(err);
    g_assert_nonnull(pool);

    /* 4개 할당 */
    gint idx0 = pcv_uring_buf_alloc(pool);
    gint idx1 = pcv_uring_buf_alloc(pool);
    gint idx2 = pcv_uring_buf_alloc(pool);
    gint idx3 = pcv_uring_buf_alloc(pool);
    g_assert_cmpint(idx0, >=, 0);
    g_assert_cmpint(idx1, >=, 0);
    g_assert_cmpint(idx2, >=, 0);
    g_assert_cmpint(idx3, >=, 0);

    /* 5번째 할당 → 실패 */
    gint idx4 = pcv_uring_buf_alloc(pool);
    g_assert_cmpint(idx4, ==, -1);

    /* 포인터 접근 */
    void *buf0 = pcv_uring_buf_get(pool, idx0);
    void *buf1 = pcv_uring_buf_get(pool, idx1);
    g_assert_nonnull(buf0);
    g_assert_nonnull(buf1);
    g_assert_true(buf0 != buf1);

    /* 반환 후 재할당 */
    pcv_uring_buf_release(pool, idx0);
    gint idx5 = pcv_uring_buf_alloc(pool);
    g_assert_cmpint(idx5, ==, idx0);

    pcv_uring_buf_pool_free(pool);
}

/* ── 버퍼 풀 경계 조건 ────────────────────────────────── */

static void test_uring_buf_invalid(void) {
    GError *err = NULL;
    /* 잘못된 파라미터 */
    PcvUringBufPool *pool = pcv_uring_buf_pool_new(0, 1024, &err);
    g_assert_null(pool);
    g_assert_nonnull(err);
    g_error_free(err);

    /* NULL 풀 접근 */
    g_assert_cmpint(pcv_uring_buf_alloc(NULL), ==, -1);
    g_assert_null(pcv_uring_buf_get(NULL, 0));
}

/* ── eventfd 등록 확인 ────────────────────────────────── */

static void test_uring_eventfd(void) {
    GError *err = NULL;
    PcvUringCtx *ctx = pcv_uring_new(16, &err);
    g_assert_no_error(err);
    g_assert_nonnull(ctx);

    /* eventfd가 양수이고 GMainLoop 소스가 등록됨 */
    g_assert_cmpint(ctx->event_fd, >, 0);
    g_assert_cmpuint(ctx->glib_source_id, >, 0);

    pcv_uring_free(ctx);
}

/* ── 파일 읽기: /proc/self/status를 io_uring으로 읽고 "Name:" 접두사 확인 ── */

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

    /* /proc/self/status 읽기 (항상 존재) */
    int fd = open("/proc/self/status", O_RDONLY);
    g_assert_cmpint(fd, >=, 0);

    char buf[256] = {0};
    ReadTestData td = {.result = -999, .done = FALSE};

    gboolean ok = pcv_uring_submit_read(ctx, fd, buf, sizeof(buf) - 1, 0,
                                         _read_cb, &td);
    g_assert_true(ok);

    /* CQE 완료 대기 (최대 1초) — eventfd poll */
    for (int i = 0; i < 100 && !td.done; i++) {
        /* GMainLoop iteration 1회 실행 */
        g_main_context_iteration(NULL, FALSE);
        if (!td.done)
            g_usleep(10000); /* 10ms */
    }

    g_assert_true(td.done);
    g_assert_cmpint(td.result, >, 0);
    g_assert_true(g_str_has_prefix(buf, "Name:"));

    close(fd);
    pcv_uring_free(ctx);
}

/* ── pending overflow: ring 깊이보다 많은 pending 등록 시 크래시 없음 ── */

static void test_uring_pending_overflow(void) {
    GError *err = NULL;
    /* 깊이 4의 ring 생성 */
    PcvUringCtx *ctx = pcv_uring_new(4, &err);
    g_assert_no_error(err);
    g_assert_nonnull(ctx);

    int fd = open("/dev/null", O_RDONLY);
    g_assert_cmpint(fd, >=, 0);

    char buf[64] = {0};
    /* ring depth(4)보다 많은 수(6)를 submit — 크래시 없이 graceful 처리 검증 */
    guint success_count = 0;
    for (int i = 0; i < 6; i++) {
        gboolean ok = pcv_uring_submit_read(ctx, fd, buf, sizeof(buf) - 1, 0,
                                             NULL, NULL);
        if (ok)
            success_count++;
    }
    /* 최소 1개는 성공해야 하며, 크래시가 없어야 한다 */
    g_assert_cmpuint(success_count, >=, 1);
    /* pending 해시테이블이 유효한 상태로 남아 있어야 한다 */
    g_assert_nonnull(ctx->pending);

    close(fd);
    pcv_uring_free(ctx);
}

/* ── 버퍼 이중 반환 감지: double-release 시 크래시 없고 이후 alloc 정상 ── */

static void test_uring_buf_double_release(void) {
    GError *err = NULL;
    PcvUringBufPool *pool = pcv_uring_buf_pool_new(4, 1024, &err);
    g_assert_no_error(err);
    g_assert_nonnull(pool);

    /* 버퍼 하나 할당 후 두 번 반환 — 두 번째는 graceful하게 처리되어야 한다 */
    gint idx = pcv_uring_buf_alloc(pool);
    g_assert_cmpint(idx, >=, 0);

    pcv_uring_buf_release(pool, idx);  /* 정상 반환 */
    pcv_uring_buf_release(pool, idx);  /* 이중 반환 — 크래시 없이 무시되어야 함 */

    /* 이중 반환 이후에도 alloc이 정상 동작해야 한다 */
    gint idx2 = pcv_uring_buf_alloc(pool);
    g_assert_cmpint(idx2, >=, 0);

    pcv_uring_buf_pool_free(pool);
}

/* ── SQ full 방어: 작은 ring(2)에 3개 submit 시 크래시 없이 처리 ── */

static void test_uring_sq_full(void) {
    GError *err = NULL;
    /* 매우 작은 ring (2 entries) */
    PcvUringCtx *ctx = pcv_uring_new(2, &err);
    g_assert_no_error(err);

    char buf[64];
    int fd = open("/dev/null", O_RDONLY);

    /* 2개는 성공해야 함 */
    gboolean ok1 = pcv_uring_submit_read(ctx, fd, buf, 1, 0, NULL, NULL);
    gboolean ok2 = pcv_uring_submit_read(ctx, fd, buf, 1, 0, NULL, NULL);
    g_assert_true(ok1);
    g_assert_true(ok2);

    /* CQE 처리 전이라 3번째는 실패할 수 있음 (SQ full) */
    /* 이건 타이밍 의존이므로 결과와 무관하게 크래시만 안 하면 OK */
    pcv_uring_submit_read(ctx, fd, buf, 1, 0, NULL, NULL);

    close(fd);
    pcv_uring_free(ctx);
}

#endif /* PCV_USE_URING */

/* ── 등록 ──────────────────────────────────────────────── */

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
