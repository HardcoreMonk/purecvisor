/**
 * @file pcv_worker_pool.c
 * @brief 제한된 GThreadPool 기반 워커 스레드 풀 구현
 *
 * == 아키텍처 ==
 *   GThreadPool은 GLib의 스레드 풀 구현으로, 최대 스레드 수를 제한할 수 있습니다.
 *   각 작업은 PoolEntry 구조체로 패키징되어 풀에 push됩니다.
 *   _pool_worker()에서 GTaskThreadFunc를 직접 호출하여 GTask 워커 스레드와
 *   동일한 인터페이스를 제공합니다.
 *
 * == 스레드 안전성 ==
 *   GThreadPool 자체가 내부 락으로 보호되므로 별도의 동기화가 필요 없습니다.
 */

#include "pcv_worker_pool.h"
#include "pcv_config.h"
#include "pcv_log.h"

/* ── 내부 상태 ────────────────────────────────────────────────── */

static GThreadPool *g_pool = NULL;

/**
 * PoolEntry:
 * GThreadPool에 전달되는 작업 단위.
 * GTask와 워커 함수 포인터를 묶어 전달합니다.
 */
typedef struct {
    GTask          *task;   /* GTask 객체 (ref 보유) */
    GTaskThreadFunc func;   /* 실행할 워커 함수 */
} PoolEntry;

/**
 * _pool_worker:
 * GThreadPool의 워커 함수. PoolEntry를 꺼내 GTaskThreadFunc를 호출합니다.
 *
 * GTaskThreadFunc 시그니처:
 *   void (*func)(GTask *task, gpointer source_object,
 *                gpointer task_data, GCancellable *cancellable)
 *
 * @param data      PoolEntry* (이 함수에서 해제)
 * @param user_data 미사용 (GThreadPool 생성 시 NULL 전달)
 */
static void
_pool_worker(gpointer data, gpointer user_data)
{
    (void)user_data;
    PoolEntry *e = (PoolEntry *)data;

    /* GTaskThreadFunc 호출 — g_task_run_in_thread()와 동일한 인터페이스 */
    e->func(e->task,
            g_task_get_source_object(e->task),
            g_task_get_task_data(e->task),
            g_task_get_cancellable(e->task));

    g_object_unref(e->task);
    g_free(e);
}

/* ── 공개 API ─────────────────────────────────────────────────── */

void
pcv_worker_pool_init(void)
{
    if (g_pool) {
        PCV_LOG_WARN("worker_pool", "Already initialized — skipping");
        return;
    }

    gint max_threads = pcv_config_get_int("daemon", "worker_threads", 8);
    if (max_threads < 1) max_threads = 1;
    if (max_threads > 64) max_threads = 64;

    GError *error = NULL;
    g_pool = g_thread_pool_new(_pool_worker, NULL, max_threads, FALSE, &error);
    if (error) {
        PCV_LOG_ERROR("worker_pool", "Failed to create thread pool: %s", error->message);
        g_error_free(error);
        return;
    }

    PCV_LOG_INFO("worker_pool", "Worker thread pool initialized (max_threads=%d)", max_threads);
}

void
pcv_worker_pool_shutdown(void)
{
    if (!g_pool) return;

    /* immediate=FALSE: 대기 중인 작업을 완료한 후 종료
     * wait=TRUE: 모든 스레드가 종료될 때까지 대기 */
    g_thread_pool_free(g_pool, FALSE, TRUE);
    g_pool = NULL;

    PCV_LOG_INFO("worker_pool", "Worker thread pool shutdown complete");
}

void
pcv_worker_pool_push(GTask *task, GTaskThreadFunc func)
{
    g_return_if_fail(task != NULL);
    g_return_if_fail(func != NULL);

    if (!g_pool) {
        /* 풀 미초기화 시 GTask 기본 스레드 풀로 폴백 */
        PCV_LOG_WARN("worker_pool", "Pool not initialized — falling back to g_task_run_in_thread");
        g_task_run_in_thread(task, func);
        return;
    }

    PoolEntry *e = g_new0(PoolEntry, 1);
    e->task = g_object_ref(task);
    e->func = func;

    GError *error = NULL;
    g_thread_pool_push(g_pool, e, &error);
    if (error) {
        PCV_LOG_ERROR("worker_pool", "Failed to push task: %s", error->message);
        g_error_free(error);
        /* 실패 시에도 GTask 기본 경로로 폴백 */
        g_object_unref(e->task);
        g_free(e);
        g_task_run_in_thread(task, func);
    }
}

guint
pcv_worker_pool_get_pending(void)
{
    if (!g_pool) return 0;
    return g_thread_pool_unprocessed(g_pool);
}
