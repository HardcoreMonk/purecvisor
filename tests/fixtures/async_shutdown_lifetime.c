



#include <stdio.h>
#include <sqlite3.h>
#include "src/modules/dispatcher/handler_backup.c"
#include "src/api/drain.h"
#include "src/utils/pcv_worker_pool.h"
#include "src/utils/pcv_config.h"

static GMutex gate;
static GCond changed;
static gboolean started, released, worker_done, callback_entered, callback_done;
static gboolean destructor_entered, destructor_done, loop_returned;
static gint backend_effect, audit_calls, ws_calls, child_done;
static const char *mode;
static GMainLoop *main_loop;
static GMainContext *reply_context;
static gchar *job_id;

void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...)
{ (void)level; (void)domain; (void)fmt; }
const gchar *pcv_config_get_string(const gchar *section, const gchar *key,
                                  const gchar *fallback)
{ (void)section; (void)key; return fallback; }
gint pcv_config_get_int(const gchar *section, const gchar *key, gint fallback)
{ (void)section; (void)key; return fallback; }

static gboolean is_mode(const char *value) { return g_str_equal(mode, value); }



gboolean __real_g_thread_pool_push(GThreadPool *, gpointer, GError **);
gboolean __wrap_g_thread_pool_push(GThreadPool *pool, gpointer data, GError **error)
{
    gboolean pushed = __real_g_thread_pool_push(pool, data, error);
    if (pushed && is_mode("pool-push-error")) {
        g_set_error_literal(error, G_THREAD_ERROR, G_THREAD_ERROR_AGAIN, "injected queued error");
        return FALSE;
    }
    return pushed;
}

static void wait_flag(gboolean *flag)
{
    gint64 deadline = g_get_monotonic_time() + 4 * G_TIME_SPAN_SECOND;
    g_mutex_lock(&gate);
    while (!*flag) g_assert_true(g_cond_wait_until(&changed, &gate, deadline));
    g_mutex_unlock(&gate);
}

static void set_flag(gboolean *flag)
{
    g_mutex_lock(&gate);
    *flag = TRUE;
    g_cond_broadcast(&changed);
    g_mutex_unlock(&gate);
}

gboolean pcv_backup_restore(const gchar *vm, const gchar *snapshot, GError **error)
{
    (void)vm; (void)snapshot; (void)error;
    set_flag(&started);
    if (!is_mode("callback") && !is_mode("destructor")) wait_flag(&released);
    g_atomic_int_inc(&backend_effect);
    if (is_mode("failure")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fixture backend failure");
        return FALSE;
    }
    return TRUE;
}

void pcv_audit_log(const gchar *user, const gchar *method, const gchar *target,
                   const gchar *result, gint error_code, gint64 duration,
                   const gchar *source_ip)
{
    (void)user; (void)method; (void)target; (void)result;
    (void)error_code; (void)duration; (void)source_ip;
    g_atomic_int_inc(&audit_calls);
}

static gboolean deliver_ws(gpointer unused)
{
    (void)unused;
    g_atomic_int_inc(&ws_calls);
    return G_SOURCE_REMOVE;
}

void pcv_ws_broadcast_job_complete_mt(const gchar *id, const gchar *method,
                                     const gchar *status, const gchar *error)
{
    (void)id; (void)method; (void)status; (void)error;
    pcv_drain_idle(reply_context, deliver_ws, NULL, NULL);
}

static void probe_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    _restore_worker(task, source, data, cancel);
    set_flag(&worker_done);
}

static void no_return_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)task; (void)source; (void)data; (void)cancel;
    set_flag(&started);
    wait_flag(&released);
    pcv_job_set_result(job_id, PCV_JOB_COMPLETED,
                      "{\"vm_name\":\"audit-probe\",\"snapshot_name\":\"snapshot\"}");
    g_atomic_int_inc(&backend_effect);
    g_atomic_int_inc(&audit_calls);
    pcv_drain_idle(NULL, deliver_ws, NULL, NULL);
    set_flag(&worker_done);

}

static void child_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)data; (void)cancel;
    g_usleep(30000);
    g_atomic_int_inc(&child_done);
    g_task_return_boolean(task, TRUE);
}

static void on_complete(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; (void)result; (void)data;
    set_flag(&callback_entered);
    if (is_mode("callback")) wait_flag(&released);
    if (is_mode("chain")) {
        GTask *child = pcv_drain_task_new(NULL, NULL, NULL, NULL);
        g_task_run_in_thread(child, child_worker);
        g_object_unref(child);
    }
    set_flag(&callback_done);
}

static void data_free(gpointer data)
{
    set_flag(&destructor_entered);
    if (is_mode("destructor")) wait_flag(&released);
    _restore_task_data_free(data);
    set_flag(&destructor_done);
}



static gpointer release_controller(gpointer unused)
{
    (void)unused;
    if (is_mode("callback")) wait_flag(&callback_entered);
    else if (is_mode("destructor")) wait_flag(&destructor_entered);
    else if (is_mode("cancel")) wait_flag(&callback_done);
    else wait_flag(&started);
    g_usleep(80000);
    g_assert_cmpint(pcv_drain_get_work(), >, 0);
    g_mutex_lock(&gate);
    g_assert_false(loop_returned);
    released = TRUE;
    g_cond_broadcast(&changed);
    g_mutex_unlock(&gate);
    return NULL;
}

static gboolean release_ref(gpointer task)
{
    if (!worker_done || !callback_done) return G_SOURCE_CONTINUE;
    g_assert_false(destructor_done);
    g_assert_cmpint(pcv_drain_get_work(), >, 0);
    g_object_unref(task);
    return G_SOURCE_REMOVE;
}

static gpointer reply_thread(gpointer loop)
{
    g_main_context_push_thread_default(reply_context);
    g_main_loop_run(loop);
    g_main_context_pop_thread_default(reply_context);
    return NULL;
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 3);
    mode = argv[1];
    g_assert_false(g_file_test(argv[2], G_FILE_TEST_EXISTS));
    g_unsetenv("NOTIFY_SOCKET");
    g_setenv("PCV_JOBS_DB_PATH", argv[2], TRUE);
    pcv_drain_init();
    pcv_job_queue_init();
    job_id = pcv_job_create("backup.restore", "audit-probe@snapshot", NULL);
    if (is_mode("manual")) {
        pcv_drain_begin(NULL, 1);
        g_assert_true(pcv_drain_is_shutdown());
        g_assert_false(pcv_drain_is_terminating());
        pcv_drain_cancel();
        g_assert_true(pcv_drain_inc());
        pcv_drain_dec();
        pcv_drain_begin(NULL, 1);
    }
    main_loop = g_main_loop_new(NULL, FALSE);
    GMainLoop *other_loop = NULL;
    GThread *other_thread = NULL;
    if (is_mode("context")) {
        reply_context = g_main_context_new();
        other_loop = g_main_loop_new(reply_context, FALSE);
        g_main_context_push_thread_default(reply_context);
    }
    RestoreTaskData *data = g_new0(RestoreTaskData, 1);
    data->vm_name = g_strdup("audit-probe");
    data->snapshot_name = g_strdup("snapshot");
    data->job_id = g_strdup(job_id);
    GCancellable *cancel = g_cancellable_new();
    gboolean pool_mode = is_mode("pool") || is_mode("pool-push-error");
    gboolean null_callback = is_mode("shared") || pool_mode || is_mode("fallback") || is_mode("no-return");
    GTask *task = pool_mode || is_mode("fallback")
        ? g_task_new(NULL, cancel, NULL, NULL)
        : pcv_drain_task_new(NULL, cancel, null_callback ? NULL : on_complete, NULL);
    if (is_mode("context")) {
        g_main_context_pop_thread_default(reply_context);
        other_thread = g_thread_new("reply-context", reply_thread, other_loop);
    }
    if (is_mode("cancel")) g_task_set_return_on_cancel(task, TRUE);
    g_task_set_task_data(task, data, data_free);
    if (pool_mode) pcv_worker_pool_init();
    if (pool_mode || is_mode("fallback")) pcv_worker_pool_push(task, probe_worker);
    else g_task_run_in_thread(task, is_mode("no-return") ? no_return_worker : probe_worker);
    wait_flag(&started);
    if (is_mode("held-ref")) g_timeout_add(200, release_ref, task);
    else g_object_unref(task);
    if (is_mode("cancel")) g_cancellable_cancel(cancel);
    GThread *controller = is_mode("timeout") ? NULL
        : g_thread_new("barrier-release", release_controller, NULL);
    pcv_drain_begin(main_loop, is_mode("timeout") ? 1 : 5);
    g_main_loop_run(main_loop);
    set_flag(&loop_returned);
    g_assert_false(is_mode("timeout"));
    g_thread_join(controller);
    g_assert_true(worker_done);
    g_assert_true(destructor_done);
    if (!null_callback) g_assert_true(callback_done);
    g_assert_cmpint(pcv_drain_get_work(), ==, 0);
    g_assert_cmpint(backend_effect, ==, 1);
    g_assert_cmpint(audit_calls, ==, 1);
    g_assert_cmpint(ws_calls, ==, 1);
    if (is_mode("chain")) g_assert_cmpint(child_done, ==, 1);
    pcv_worker_pool_shutdown();
    pcv_job_queue_shutdown();
    if (other_thread) {
        g_main_loop_quit(other_loop);
        g_thread_join(other_thread);
        g_main_loop_unref(other_loop);
        g_main_context_unref(reply_context);
    }
    pcv_drain_shutdown();
    g_object_unref(cancel);
    g_main_loop_unref(main_loop);
    g_print("PASS %s job=%s destructor=%d callback=%d ws=%d\n",
            mode, job_id, destructor_done, callback_done, ws_calls);
    g_free(job_id);
    return 0;
}
