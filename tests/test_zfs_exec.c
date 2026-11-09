                                                                                                           
                                                                                    
                                                                             
                          
                                                                          
                                                                   
                  
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "../src/utils/pcv_spawn.h"
#include "../src/utils/pcv_zfs_exec.h"
#include "../src/utils/pcv_zfs_lock.h"
#include "../src/modules/dispatcher/handler_storage.h"

void pcv_test_uds_responses_reset(void);
const gchar *pcv_test_last_uds_response(void);
guint pcv_test_uds_response_count(void);
void pcv_test_audit_reset(void);
const gchar *pcv_test_audit_last_method(void);
const gchar *pcv_test_audit_last_target(void);
const gchar *pcv_test_audit_last_result(void);

                                           
typedef struct {
    gchar *root, *bin, *log, *saved_path;
} ZfsEnv;

                                                                                  
                                                           
static void
_env_begin(ZfsEnv *e, const gchar *body)
{
    e->root = g_dir_make_tmp("pcv-zfs-exec-XXXXXX", NULL);
    g_assert_nonnull(e->root);
    e->bin = g_build_filename(e->root, "bin", NULL);
    e->log = g_build_filename(e->root, "calls.log", NULL);
    g_assert_cmpint(g_mkdir_with_parents(e->bin, 0700), ==, 0);
    g_autofree gchar *file = g_build_filename(e->bin, "zfs", NULL);
    g_autofree gchar *qlog = g_shell_quote(e->log);
    g_autofree gchar *script = g_strdup_printf("#!/bin/sh\nprintf '%%s\\n' \"$*\" >> %s\n%s", qlog, body);
    g_assert_true(g_file_set_contents(file, script, -1, NULL));
    g_assert_cmpint(g_chmod(file, 0755), ==, 0);
    e->saved_path = g_strdup(g_getenv("PATH"));
    g_autofree gchar *path = g_strdup_printf("%s:%s", e->bin, e->saved_path ? e->saved_path : "");
    g_setenv("PATH", path, TRUE);
    pcv_spawn_launcher_init();
}

static void
_env_end(ZfsEnv *e)
{
    pcv_zfs_exec_set_lock_wait_for_test(0);
    pcv_spawn_launcher_shutdown();
    if (e->saved_path) g_setenv("PATH", e->saved_path, TRUE); else g_unsetenv("PATH");
    g_autofree gchar *cmd = g_strdup_printf("rm -rf %s", e->root);
    g_assert_true(g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL));
    g_free(e->root); g_free(e->bin); g_free(e->log); g_free(e->saved_path);
}

                                 
static gchar **
_env_calls(const ZfsEnv *e)
{
    g_autofree gchar *text = NULL;
    if (!g_file_get_contents(e->log, &text, NULL, NULL))
        return g_new0(gchar *, 1);
    g_strchomp(text);
    return *text ? g_strsplit(text, "\n", -1) : g_new0(gchar *, 1);
}

                                          
static void
test_argv_shape(void)
{
    ZfsEnv e = {0};
    _env_begin(&e, "exit 0\n");
    GError *error = NULL;
    const gchar *vol[] = {"-V", "10G", NULL};
    const gchar *mnt[] = {"-o", "mountpoint=/x", NULL};
    g_assert_true(pcv_zfs_create_locked(vol, "shape/vms/a", NULL, &error));
    g_assert_true(pcv_zfs_create_locked(mnt, "shape/ctr/a", NULL, &error));
    g_assert_true(pcv_zfs_create_locked(NULL, "shape/plain", NULL, &error));
    g_assert_true(pcv_zfs_destroy_locked("-R", "shape/vms/a", NULL, &error));
    g_assert_true(pcv_zfs_destroy_locked(NULL, "shape/vms/a@s", NULL, &error));
    g_assert_no_error(error);
    g_auto(GStrv) calls = _env_calls(&e);
    g_assert_cmpuint(g_strv_length(calls), ==, 5);
    g_assert_cmpstr(calls[0], ==, "create -V 10G shape/vms/a");
    g_assert_cmpstr(calls[1], ==, "create -o mountpoint=/x shape/ctr/a");
    g_assert_cmpstr(calls[2], ==, "create shape/plain");
    g_assert_cmpstr(calls[3], ==, "destroy -R shape/vms/a");
    g_assert_cmpstr(calls[4], ==, "destroy shape/vms/a@s");
    _env_end(&e);
}

typedef struct { gchar *target; gboolean ok; } DestroyJob;

static gpointer
_destroy_thread(gpointer p)
{
    DestroyJob *j = p;
    j->ok = pcv_zfs_destroy_locked("-r", j->target, NULL, NULL);
    return NULL;
}

                                                                                 
                                                    
static void
test_same_pool_serialized(void)
{
    ZfsEnv e = {0};
    g_autofree gchar *seq = NULL;
    {
        g_autofree gchar *probe = g_dir_make_tmp("pcv-zfs-seq-XXXXXX", NULL);
        seq = g_build_filename(probe, "seq", NULL);
    }
    g_autofree gchar *qseq = g_shell_quote(seq);
    g_autofree gchar *body = g_strdup_printf("echo S >> %s\nsleep 0.1\necho E >> %s\nexit 0\n", qseq, qseq);
    _env_begin(&e, body);
    DestroyJob jobs[4] = {
        { "serial/vms/a", FALSE }, { "serial/vms/b", FALSE },
        { "serial/ctr/c", FALSE }, { "serial/vms/d@s", FALSE },
    };
    GThread *threads[4];
    for (guint i = 0; i < 4; i++)
        threads[i] = g_thread_new("destroy", _destroy_thread, &jobs[i]);
    for (guint i = 0; i < 4; i++) {
        g_thread_join(threads[i]);
        g_assert_true(jobs[i].ok);
    }
    g_autofree gchar *text = NULL;
    g_assert_true(g_file_get_contents(seq, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "S\nE\nS\nE\nS\nE\nS\nE\n");
    g_autofree gchar *dir = g_path_get_dirname(seq);
    g_assert_cmpint(g_unlink(seq), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    _env_end(&e);
}

                                                                 
                                      
static void
test_other_pool_runs_concurrently(void)
{
    ZfsEnv e = {0};
    g_autofree gchar *flags = g_dir_make_tmp("pcv-zfs-flag-XXXXXX", NULL);
    g_autofree gchar *qflags = g_shell_quote(flags);
    g_autofree gchar *body = g_strdup_printf(
        "for last; do :; done\n"
        "pool=${last%%%%/*}\n"
        "touch %s/$pool\n"
        "i=0\n"
        "while [ $i -lt 100 ]; do\n"
        "  [ $(ls %s | wc -l) -ge 2 ] && exit 0\n"
        "  sleep 0.05; i=$((i+1))\n"
        "done\n"
        "exit 1\n", qflags, qflags);
    _env_begin(&e, body);
    DestroyJob jobs[2] = { { "poola/x", FALSE }, { "poolb/x", FALSE } };
    GThread *threads[2];
    for (guint i = 0; i < 2; i++)
        threads[i] = g_thread_new("destroy", _destroy_thread, &jobs[i]);
    for (guint i = 0; i < 2; i++) {
        g_thread_join(threads[i]);
        g_assert_true(jobs[i].ok);
    }
    g_autofree gchar *cmd = g_strdup_printf("rm -rf %s", flags);
    g_assert_true(g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL));
    _env_end(&e);
}

                                                 
                                                            
static void
test_failure_releases_lock(void)
{
    ZfsEnv e = {0};
    _env_begin(&e, "echo 'cannot destroy: dataset is busy' >&2\nexit 1\n");
    pcv_zfs_exec_set_lock_wait_for_test(50);
    GError *error = NULL;
    g_autofree gchar *stderr_text = NULL;
    g_assert_false(pcv_zfs_destroy_locked("-r", "failrel/a", &stderr_text, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "dataset is busy"));
    g_assert_nonnull(strstr(stderr_text, "dataset is busy"));
    g_clear_error(&error);
    g_assert_true(pcv_zfs_pool_lock("failrel", "test", 0, &error));
    g_assert_no_error(error);
    pcv_zfs_pool_unlock("failrel");
    _env_end(&e);
}

                                                      
static void
test_lock_timeout_skips_zfs(void)
{
    ZfsEnv e = {0};
    _env_begin(&e, "exit 0\n");
    GError *error = NULL;
    g_autofree gchar *stderr_text = g_strdup("stale");
    g_assert_true(pcv_zfs_pool_lock("timeout", "holder", 0, &error));
    pcv_zfs_exec_set_lock_wait_for_test(60);
    const gchar *vol[] = {"-V", "1G", NULL};
    g_free(g_steal_pointer(&stderr_text));
    g_assert_false(pcv_zfs_create_locked(vol, "timeout/a", &stderr_text, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_null(stderr_text);
    g_clear_error(&error);
    pcv_zfs_pool_unlock("timeout");
    g_auto(GStrv) calls = _env_calls(&e);
    g_assert_cmpuint(g_strv_length(calls), ==, 0);
    _env_end(&e);
}

typedef struct { GMutex mu; GCond cond; gboolean held, release; } HolderState;

static gpointer
_holder_thread(gpointer p)
{
    HolderState *h = p;
    g_assert_true(pcv_zfs_pool_lock("mainloop", "holder", 0, NULL));
    g_mutex_lock(&h->mu);
    h->held = TRUE;
    g_cond_broadcast(&h->cond);
    while (!h->release)
        g_cond_wait(&h->cond, &h->mu);
    g_mutex_unlock(&h->mu);
    pcv_zfs_pool_unlock("mainloop");
    return NULL;
}

                                                           
                                                              
                                                           
static void
test_main_loop_does_not_wait(void)
{
    ZfsEnv e = {0};
    _env_begin(&e, "exit 0\n");
    HolderState h = {0};
    g_mutex_init(&h.mu);
    g_cond_init(&h.cond);
    GThread *holder = g_thread_new("holder", _holder_thread, &h);
    g_mutex_lock(&h.mu);
    while (!h.held)
        g_cond_wait(&h.cond, &h.mu);
    g_mutex_unlock(&h.mu);

    GMainContext *ctx = g_main_context_default();
    g_assert_true(g_main_context_acquire(ctx));
    GError *error = NULL;
    const gint64 started = g_get_monotonic_time();
    g_assert_false(pcv_zfs_destroy_locked("-r", "mainloop/a", NULL, &error));
    g_assert_cmpint(g_get_monotonic_time() - started, <, 5 * G_USEC_PER_SEC);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error(&error);
    g_main_context_release(ctx);

    g_mutex_lock(&h.mu);
    h.release = TRUE;
    g_cond_broadcast(&h.cond);
    g_mutex_unlock(&h.mu);
    g_thread_join(holder);
    g_mutex_clear(&h.mu);
    g_cond_clear(&h.cond);
    g_auto(GStrv) calls = _env_calls(&e);
    g_assert_cmpuint(g_strv_length(calls), ==, 0);
    _env_end(&e);
}

                                                   
static void
test_lock_key_is_pool_head(void)
{
    g_assert_true(pcv_zfs_pool_lock("headp/vms/a", "test", 0, NULL));
    g_assert_false(pcv_zfs_pool_lock("headp@snap", "test", 0, NULL));
    g_assert_false(pcv_zfs_pool_lock("headp/other@snap", "test", 0, NULL));
    g_assert_true(pcv_zfs_pool_lock("headq@snap", "test", 0, NULL));
    pcv_zfs_pool_unlock("headq");
    pcv_zfs_pool_unlock("headp");
}

                                                                     
                                                                   
static void
test_pool_table_is_bounded(void)
{
    pcv_zfs_pool_lock_shutdown();
    for (guint i = 0; i < 200; i++) {
        g_autofree gchar *name = g_strdup_printf("many%u/x", i);
        g_assert_true(pcv_zfs_pool_lock(name, "test", 0, NULL));
        pcv_zfs_pool_unlock(name);
    }
    gint registered = 0;
    pcv_zfs_pool_get_stats(&registered, NULL);
    g_assert_cmpint(registered, <=, 65);

                                     
    g_assert_true(pcv_zfs_pool_lock("many0/x", "test", 0, NULL));
    g_assert_true(pcv_zfs_pool_lock("many199/x", "test", 0, NULL));
                               
    g_assert_false(pcv_zfs_pool_lock("many198/x", "test", 0, NULL));
    pcv_zfs_pool_unlock("many199/x");
    pcv_zfs_pool_unlock("many0/x");

                                                          
    pcv_zfs_pool_lock_shutdown();
    g_autofree gchar *long_name = g_strnfill(200, 'p');
    g_assert_true(pcv_zfs_pool_lock(long_name, "test", 0, NULL));
    g_autofree gchar *long_other = g_strnfill(150, 'q');
    g_assert_false(pcv_zfs_pool_lock(long_other, "test", 0, NULL));
    pcv_zfs_pool_unlock(long_name);
    g_assert_true(pcv_zfs_pool_lock(long_other, "test", 0, NULL));
    pcv_zfs_pool_unlock(long_other);
    pcv_zfs_pool_lock_shutdown();
}

                                                                       
typedef struct { const gchar *target; gboolean use_short; gboolean ok; GError *error; gint64 elapsed_us; gint done; } WaitJob;

static gpointer
_wait_job_thread(gpointer p)
{
    WaitJob *j = p;
    const gint64 started = g_get_monotonic_time();
    j->ok = j->use_short ? pcv_zfs_destroy_locked_short("-r", j->target, NULL, &j->error)
                         : pcv_zfs_destroy_locked("-r", j->target, NULL, &j->error);
    j->elapsed_us = g_get_monotonic_time() - started;
    g_atomic_int_set(&j->done, 1);
    return NULL;
}

                                
static gboolean
_wait_job_finished(WaitJob *j, gint seconds)
{
    const gint64 deadline = g_get_monotonic_time() + (gint64)seconds * G_USEC_PER_SEC;
    while (!g_atomic_int_get(&j->done) && g_get_monotonic_time() < deadline)
        g_usleep(20000);
    return g_atomic_int_get(&j->done) != 0;
}

                                                                   
                                                      
                                                                       
static void
test_short_wait_gives_up_in_five_seconds(void)
{
    ZfsEnv e = {0};
    _env_begin(&e, "exit 0\n");
    g_assert_true(pcv_zfs_pool_lock("shortw", "holder", 0, NULL));
    WaitJob job = { .target = "shortw/a", .use_short = TRUE };
    GThread *thread = g_thread_new("short", _wait_job_thread, &job);
    g_assert_true(_wait_job_finished(&job, 7));
    g_thread_join(thread);
    pcv_zfs_pool_unlock("shortw");
    g_assert_false(job.ok);
    g_assert_error(job.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_nonnull(strstr(job.error->message, "this zfs command did not run"));
    g_assert_nonnull(strstr(job.error->message, "try again"));
    g_assert_null(strstr(job.error->message, "nothing was changed"));                                  
    g_clear_error(&job.error);
    g_assert_cmpint(job.elapsed_us, >=, 4 * G_USEC_PER_SEC);
    g_auto(GStrv) calls = _env_calls(&e);
    g_assert_cmpuint(g_strv_length(calls), ==, 0);
    _env_end(&e);
}

                                                                                      
static void
test_job_path_waits_past_short_limit(void)
{
    ZfsEnv e = {0};
    _env_begin(&e, "exit 0\n");
    g_assert_true(pcv_zfs_pool_lock("longw", "holder", 0, NULL));
    WaitJob job = { .target = "longw/a" };
    GThread *thread = g_thread_new("long", _wait_job_thread, &job);
    g_usleep(5500 * 1000);
    g_assert_cmpint(g_atomic_int_get(&job.done), ==, 0);
    pcv_zfs_pool_unlock("longw");
    g_assert_true(_wait_job_finished(&job, 5));
    g_thread_join(thread);
    g_assert_true(job.ok);
    g_assert_no_error(job.error);
    g_auto(GStrv) calls = _env_calls(&e);
    g_assert_cmpstr(calls[0], ==, "destroy -r longw/a");
    _env_end(&e);
}

                                                                                                      
                                                                               
                                                
static void
test_settling_stops_on_lock_timeout(void)
{
    ZfsEnv e = {0};
    _env_begin(&e, "exit 0\n");
    g_autofree gchar *udev = g_build_filename(e.bin, "udevadm", NULL);
    g_assert_true(g_file_set_contents(udev, "#!/bin/sh\nexit 0\n", -1, NULL));
    g_assert_cmpint(g_chmod(udev, 0755), ==, 0);
    g_assert_true(pcv_zfs_pool_lock("settle", "holder", 0, NULL));
    pcv_zfs_exec_set_lock_wait_for_test(60);
    GError *error = NULL;
    const gint64 started = g_get_monotonic_time();
    g_assert_false(pcv_zfs_destroy_settling("-f", "settle/a", &error));
    const gint64 elapsed = g_get_monotonic_time() - started;
    pcv_zfs_pool_unlock("settle");
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_nonnull(strstr(error->message, "busy"));                                   
    g_clear_error(&error);
    g_assert_cmpint(elapsed, <, G_USEC_PER_SEC);
    g_auto(GStrv) calls = _env_calls(&e);
    g_assert_cmpuint(g_strv_length(calls), ==, 0);
    _env_end(&e);
}

                                                                                     
static void
test_settling_retries_dataset_busy(void)
{
    ZfsEnv e = {0};
    g_autofree gchar *dir = g_dir_make_tmp("pcv-zfs-settle-XXXXXX", NULL);
    g_autofree gchar *count = g_build_filename(dir, "n", NULL);
    g_autofree gchar *qcount = g_shell_quote(count);
    g_autofree gchar *body = g_strdup_printf(
        "n=$(cat %s 2>/dev/null || echo 0); echo $((n+1)) > %s\n"
        "[ \"$n\" -lt 2 ] && { echo \"cannot destroy 'x': dataset is busy\" >&2; exit 1; }\nexit 0\n", qcount, qcount);
    _env_begin(&e, body);
    g_autofree gchar *udev = g_build_filename(e.bin, "udevadm", NULL);
    g_assert_true(g_file_set_contents(udev, "#!/bin/sh\nexit 0\n", -1, NULL));
    g_assert_cmpint(g_chmod(udev, 0755), ==, 0);
    GError *error = NULL;
    g_assert_true(pcv_zfs_destroy_settling("-f", "settle2/a", &error));
    g_assert_no_error(error);
    g_auto(GStrv) calls = _env_calls(&e);
    g_assert_cmpuint(g_strv_length(calls), ==, 3);
    g_assert_cmpint(g_unlink(count), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    _env_end(&e);
}

                                  
static void
test_settling_other_error_no_retry(void)
{
    ZfsEnv e = {0};
    _env_begin(&e, "echo \"cannot destroy 'x': pool is busy elsewhere\" >&2\nexit 1\n");
    g_autofree gchar *udev = g_build_filename(e.bin, "udevadm", NULL);
    g_assert_true(g_file_set_contents(udev, "#!/bin/sh\nexit 0\n", -1, NULL));
    g_assert_cmpint(g_chmod(udev, 0755), ==, 0);
    GError *error = NULL;
    g_assert_false(pcv_zfs_destroy_settling("-f", "settle3/a", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_auto(GStrv) calls = _env_calls(&e);
    g_assert_cmpuint(g_strv_length(calls), ==, 1);
    _env_end(&e);
}

                                              
static gboolean
_wait_response(gint want, gint seconds)
{
    const gint64 deadline = g_get_monotonic_time() + (gint64)seconds * G_USEC_PER_SEC;
    while (pcv_test_uds_response_count() < (guint)want && g_get_monotonic_time() < deadline)
        g_main_context_iteration(NULL, FALSE), g_usleep(2000);
    return pcv_test_uds_response_count() >= (guint)want;
}

                                                      
static JsonObject *
_zvol_params(const gchar *key, const gchar *path, const gchar *size)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, key, path);
    if (size) json_object_set_string_member(p, "size", size);
    return p;
}

                                                                             
                                                             
                                                                
static void
test_zvol_create_handler_does_not_block(void)
{
    ZfsEnv e = {0};
    g_autofree gchar *dir = g_dir_make_tmp("pcv-zvol-flag-XXXXXX", NULL);
    g_autofree gchar *flag = g_build_filename(dir, "go", NULL);
    g_autofree gchar *qflag = g_shell_quote(flag);
    g_autofree gchar *body = g_strdup_printf(
        "i=0\nwhile [ $i -lt 100 ]; do [ -e %s ] && exit 0; sleep 0.05; i=$((i+1)); done\n"
        "echo 'mock timed out' >&2\nexit 1\n", qflag);
    _env_begin(&e, body);
    pcv_test_uds_responses_reset();
    pcv_test_audit_reset();
    GObject *server = g_object_new(G_TYPE_OBJECT, NULL);
    GObject *conn = g_object_new(G_TYPE_OBJECT, NULL);
    JsonObject *params = _zvol_params("zvol_path", "zh/vms/a", "1G");

    handle_storage_zvol_create_request(params, "7", (UdsServer *)server, (GSocketConnection *)conn);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 0);                                 
    g_assert_true(g_file_set_contents(flag, "", 0, NULL));
    g_assert_true(_wait_response(1, 20));
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"result\""));
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "storage.zvol.create");
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, "zh/vms/a");
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "ok");
    g_auto(GStrv) calls = _env_calls(&e);
    g_assert_cmpstr(calls[0], ==, "create -V 1G zh/vms/a");

    json_object_unref(params);
    g_object_unref(server);
    g_object_unref(conn);
    g_assert_cmpint(g_unlink(flag), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    _env_end(&e);
}

                                                                      
static void
test_zvol_delete_handler_guard(void)
{
    ZfsEnv e = {0};
    _env_begin(&e, "[ \"$1\" = list ] && { echo filesystem; exit 0; }\nexit 0\n");
    pcv_test_uds_responses_reset();
    pcv_test_audit_reset();
    GObject *server = g_object_new(G_TYPE_OBJECT, NULL);
    GObject *conn = g_object_new(G_TYPE_OBJECT, NULL);
    JsonObject *params = _zvol_params("name", "zd/vms", NULL);
    handle_storage_zvol_delete_request(params, "8", (UdsServer *)server, (GSocketConnection *)conn);
    g_assert_true(_wait_response(1, 20));
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "not a zvol"));
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "storage.zvol.delete");
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_auto(GStrv) calls = _env_calls(&e);
    g_assert_cmpuint(g_strv_length(calls), ==, 1);                             
    g_assert_true(g_str_has_prefix(calls[0], "list "));
    json_object_unref(params);
    g_object_unref(server);
    g_object_unref(conn);
    _env_end(&e);
}

                                                                                    
                                                          
static void
test_zvol_handlers_report_pool_busy(void)
{
    ZfsEnv e = {0};
    _env_begin(&e, "[ \"$1\" = list ] && { echo volume; exit 0; }\nexit 0\n");
    pcv_test_uds_responses_reset();
    pcv_test_audit_reset();
    pcv_zfs_exec_set_lock_wait_for_test(60);
    g_assert_true(pcv_zfs_pool_lock("zbusy", "holder", 0, NULL));
    GObject *server = g_object_new(G_TYPE_OBJECT, NULL);
    GObject *conn = g_object_new(G_TYPE_OBJECT, NULL);
    JsonObject *create = _zvol_params("zvol_path", "zbusy/vms/a", "1G");
    handle_storage_zvol_create_request(create, "9", (UdsServer *)server, (GSocketConnection *)conn);
    g_assert_true(_wait_response(1, 20));
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "-32004"));
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "try again"));
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    JsonObject *del = _zvol_params("name", "zbusy/vms/a", NULL);
    handle_storage_zvol_delete_request(del, "10", (UdsServer *)server, (GSocketConnection *)conn);
    g_assert_true(_wait_response(2, 20));
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "-32004"));
    pcv_zfs_pool_unlock("zbusy");
    g_auto(GStrv) calls = _env_calls(&e);
    for (guint i = 0; calls[i]; i++)
        g_assert_true(g_str_has_prefix(calls[i], "list "));                                  
    json_object_unref(del);
    json_object_unref(create);
    g_object_unref(server);
    g_object_unref(conn);
    _env_end(&e);
}

void
test_zfs_exec_register(void)
{
    g_test_add_func("/zfs_exec/argv_shape", test_argv_shape);
    g_test_add_func("/zfs_exec/same_pool_serialized", test_same_pool_serialized);
    g_test_add_func("/zfs_exec/other_pool_runs_concurrently", test_other_pool_runs_concurrently);
    g_test_add_func("/zfs_exec/failure_releases_lock", test_failure_releases_lock);
    g_test_add_func("/zfs_exec/lock_timeout_skips_zfs", test_lock_timeout_skips_zfs);
    g_test_add_func("/zfs_exec/short_wait_gives_up_in_five_seconds", test_short_wait_gives_up_in_five_seconds);
    g_test_add_func("/zfs_exec/job_path_waits_past_short_limit", test_job_path_waits_past_short_limit);
    g_test_add_func("/zfs_exec/settling_stops_on_lock_timeout", test_settling_stops_on_lock_timeout);
    g_test_add_func("/zfs_exec/settling_retries_dataset_busy", test_settling_retries_dataset_busy);
    g_test_add_func("/zfs_exec/settling_other_error_no_retry", test_settling_other_error_no_retry);
    g_test_add_func("/zfs_exec/main_loop_does_not_wait", test_main_loop_does_not_wait);
    g_test_add_func("/zfs_exec/lock_key_is_pool_head", test_lock_key_is_pool_head);
    g_test_add_func("/zfs_exec/pool_table_is_bounded", test_pool_table_is_bounded);
    g_test_add_func("/zfs_exec/zvol_create_handler_does_not_block", test_zvol_create_handler_does_not_block);
    g_test_add_func("/zfs_exec/zvol_delete_handler_guard", test_zvol_delete_handler_guard);
    g_test_add_func("/zfs_exec/zvol_handlers_report_pool_busy", test_zvol_handlers_report_pool_busy);
}
