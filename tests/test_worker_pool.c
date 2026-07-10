/**
 * @file test_worker_pool.c
 * @brief pcv_worker_pool 유닛 테스트
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  pcv_worker_pool.h (src/utils/)의 GThreadPool 기반 워커 풀을 검증한다.
 *  7개 테스트 케이스.
 *
 *  워커 풀은 데몬의 비동기 RPC 처리에 사용된다 (fire-and-forget 패턴).
 *  예: vm.create 요청이 들어오면 → 풀에서 스레드 하나를 꺼내 실행 → 완료 후 반환
 *
 *  검증 항목:
 *  - init/shutdown 멱등성: 여러 번 호출해도 안전
 *  - push 실행: 워커 함수가 실제로 실행되고 결과를 올바르게 누적
 *  - 미초기화 폴백: 풀이 없으면 g_task_run_in_thread()으로 자동 폴백
 *  - 동시 실행: 10개 태스크를 동시 push → 전부 완료 확인
 *  - shutdown 드레인: 종료 시 대기 중인 태스크를 모두 완료한 후 종료
 *
 *  동기화 방법: GMutex + GCond + deadline으로 워커 완료를 대기.
 *  g_atomic_int_add로 스레드 안전한 카운터 증가.
 * ============================================================================
 */
#include <glib.h>
#include <gio/gio.h>
#include "../src/utils/pcv_worker_pool.h"

/* ── 워커 함수: counter를 atomic 증가 후 task 완료 ── */
static gint g_worker_count = 0;
static GMutex g_done_mutex;
static GCond g_done_cond;
static gint g_done_count = 0;

static void
worker_increment(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    (void)src; (void)c;
    gint delta = data ? GPOINTER_TO_INT(data) : 1;
    g_atomic_int_add(&g_worker_count, delta);
    g_mutex_lock(&g_done_mutex);
    g_done_count++;
    g_cond_broadcast(&g_done_cond);
    g_mutex_unlock(&g_done_mutex);
    g_task_return_boolean(task, TRUE);
}

static void
wait_for_n(int target, int timeout_ms) {
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    g_mutex_lock(&g_done_mutex);
    while (g_done_count < target && g_cond_wait_until(&g_done_cond, &g_done_mutex, deadline)) {}
    g_mutex_unlock(&g_done_mutex);
}

static void
reset_counters(void) {
    g_atomic_int_set(&g_worker_count, 0);
    g_mutex_lock(&g_done_mutex);
    g_done_count = 0;
    g_mutex_unlock(&g_done_mutex);
}

/* ── 케이스 ──────────────────────────────────────────────── */

static void test_init_idempotent(void) {
    pcv_worker_pool_init();
    pcv_worker_pool_init();  /* 재호출 — warn 로그 + 무시 */
    g_assert_cmpuint(pcv_worker_pool_get_pending(), ==, 0);
    pcv_worker_pool_shutdown();
}

static void test_get_pending_uninitialized(void) {
    /* shutdown 상태에서 0 반환 */
    g_assert_cmpuint(pcv_worker_pool_get_pending(), ==, 0);
}

static void test_push_uninitialized_fallback(void) {
    /* 풀 미초기화 시 g_task_run_in_thread 폴백 (warn 로그) */
    reset_counters();
    GTask *t = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(t, GINT_TO_POINTER(7), NULL);
    pcv_worker_pool_push(t, worker_increment);
    g_object_unref(t);
    wait_for_n(1, 2000);
    g_assert_cmpint(g_atomic_int_get(&g_worker_count), ==, 7);
}

static void test_push_executes_worker(void) {
    reset_counters();
    pcv_worker_pool_init();

    GTask *t = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(t, GINT_TO_POINTER(3), NULL);
    pcv_worker_pool_push(t, worker_increment);
    g_object_unref(t);

    wait_for_n(1, 2000);
    g_assert_cmpint(g_atomic_int_get(&g_worker_count), ==, 3);

    pcv_worker_pool_shutdown();
}

static void test_push_multiple_tasks(void) {
    reset_counters();
    pcv_worker_pool_init();

    const int N = 10;
    for (int i = 0; i < N; i++) {
        GTask *t = g_task_new(NULL, NULL, NULL, NULL);
        g_task_set_task_data(t, GINT_TO_POINTER(1), NULL);
        pcv_worker_pool_push(t, worker_increment);
        g_object_unref(t);
    }
    wait_for_n(N, 5000);
    g_assert_cmpint(g_atomic_int_get(&g_worker_count), ==, N);

    pcv_worker_pool_shutdown();
}

static void test_shutdown_drains_pending(void) {
    reset_counters();
    pcv_worker_pool_init();
    const int N = 5;
    for (int i = 0; i < N; i++) {
        GTask *t = g_task_new(NULL, NULL, NULL, NULL);
        g_task_set_task_data(t, GINT_TO_POINTER(2), NULL);
        pcv_worker_pool_push(t, worker_increment);
        g_object_unref(t);
    }
    /* shutdown은 큐 드레인 후 종료 (immediate=FALSE) */
    pcv_worker_pool_shutdown();
    g_assert_cmpint(g_atomic_int_get(&g_worker_count), ==, N * 2);
    /* shutdown 후 pending = 0 */
    g_assert_cmpuint(pcv_worker_pool_get_pending(), ==, 0);
}

static void test_shutdown_idempotent(void) {
    pcv_worker_pool_shutdown();  /* 미초기화 상태에서 호출 */
    pcv_worker_pool_init();
    pcv_worker_pool_shutdown();
    pcv_worker_pool_shutdown();  /* 재호출 안전 */
}

void test_worker_pool_register(void) {
    g_mutex_init(&g_done_mutex);
    g_cond_init(&g_done_cond);
    g_test_add_func("/worker_pool/init_idempotent", test_init_idempotent);
    g_test_add_func("/worker_pool/get_pending_uninitialized", test_get_pending_uninitialized);
    g_test_add_func("/worker_pool/push_uninitialized_fallback", test_push_uninitialized_fallback);
    g_test_add_func("/worker_pool/push_executes_worker", test_push_executes_worker);
    g_test_add_func("/worker_pool/push_multiple_tasks", test_push_multiple_tasks);
    g_test_add_func("/worker_pool/shutdown_drains_pending", test_shutdown_drains_pending);
    g_test_add_func("/worker_pool/shutdown_idempotent", test_shutdown_idempotent);
}
