
#include "pcv_worker_pool.h"
#include "pcv_config.h"
#include "pcv_log.h"

static GThreadPool *g_pool = NULL;

typedef struct {
    GTask          *task;
    GTaskThreadFunc func;
} PoolEntry;

static void
_pool_worker(gpointer data, gpointer user_data)
{
    (void)user_data;
    PoolEntry *e = (PoolEntry *)data;

    e->func(e->task,
            g_task_get_source_object(e->task),
            g_task_get_task_data(e->task),
            g_task_get_cancellable(e->task));

    g_object_unref(e->task);
    g_free(e);
}

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
