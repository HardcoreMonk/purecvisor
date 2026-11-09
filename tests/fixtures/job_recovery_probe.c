                                                                
                                                               
                                                             
                                                    
                                
#include "utils/pcv_job_queue.h"
#include "utils/pcv_job_recovery.h"
#include "utils/pcv_log.h"
#include "api/drain.h"
#include <gio/gio.h>
#include <sqlite3.h>
#include "job_queue_legacy.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdatomic.h>

static const gchar *mode;
static _Atomic gboolean fault_active;
static gboolean fast_retry, delivered;
static _Atomic gint64 fake_time;
static _Atomic guint attempts;
static _Atomic guint reconcile_binds;
static guint heartbeats, deliveries;
static _Atomic guint io_active, io_peak;
void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...)
{ (void)level; (void)domain; (void)fmt; }
const gchar *pcv_config_get_string(const gchar *section, const gchar *key, const gchar *fallback)
{ (void)section; (void)key; return fallback; }

ssize_t __real_write(int fd, const void *data, size_t size);
int __real_fsync(int fd);
int __real_renameat(int oldfd, const char *old, int newfd, const char *new);
int __real_unlinkat(int fd, const char *name, int flags);
int __real_sqlite3_bind_text(sqlite3_stmt *stmt, int index, const char *value, int length, void (*destroy)(void *));
int __real_sqlite3_exec(sqlite3 *db, const char *sql, int (*cb)(void*,int,char**,char**), void *data, char **error);
gint64 __real_g_get_monotonic_time(void);
guint __real_g_timeout_add_seconds(guint seconds, GSourceFunc callback, gpointer data);

                                                             
ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    if (fault_active && g_str_equal(mode, "enospc")) { errno = ENOSPC; return -1; }
    if (fault_active && g_str_equal(mode, "crash-partial")) {
        (void)__real_write(fd, data, size / 2); _exit(80);
    }
    if (fault_active && g_str_equal(mode, "short-write") && size > 7) size = 7;
    return __real_write(fd, data, size);
}
int __wrap_fsync(int fd)
{
    if (fault_active && g_str_equal(mode, "fsync-fail")) { errno = EIO; return -1; }
    gboolean observe = fault_active && g_str_equal(mode, "burst");
    if (observe) {
        guint active = atomic_fetch_add(&io_active, 1) + 1;
        guint peak = atomic_load(&io_peak);
        while (active > peak && !atomic_compare_exchange_weak(&io_peak, &peak, active)) { }
        g_usleep(5000);
    }
    if (fault_active && g_str_equal(mode, "slow-writer")) g_usleep(100000);
    int rc = __real_fsync(fd);
    if (observe) atomic_fetch_sub(&io_active, 1);
    return rc;
}
int __wrap_renameat(int oldfd, const char *old, int newfd, const char *new)
{
    int rc = __real_renameat(oldfd, old, newfd, new);
    if (fault_active && g_str_equal(mode, "crash-rename") && rc == 0) _exit(83);
    return rc;
}
int __wrap_unlinkat(int fd, const char *name, int flags)
{
    int rc = __real_unlinkat(fd, name, flags);
    if (fault_active && g_str_equal(mode, "crash-delete") && rc == 0) _exit(86);
    return rc;
}
int __wrap_sqlite3_exec(sqlite3 *db, const char *sql,
                       int (*cb)(void*,int,char**,char**), void *data, char **error)
{
    if (fault_active && g_str_equal(sql, "BEGIN IMMEDIATE")) {
        attempts++;
        if (fast_retry) printf("REPLAY %u %lld\n", attempts, (long long)atomic_load(&fake_time));
    }
    if (fault_active && g_str_equal(sql, "COMMIT") &&
        (g_str_equal(mode, "crash-db-before") || g_str_equal(mode, "crash-admit-before"))) _exit(84);
    int rc = __real_sqlite3_exec(db, sql, cb, data, error);
    if (fault_active && rc == SQLITE_OK && g_str_equal(sql, "COMMIT") &&
        (g_str_equal(mode, "crash-db-after") || g_str_equal(mode, "crash-admit-after"))) _exit(85);
    return rc;
}
                                                       
int __wrap_sqlite3_bind_text(sqlite3_stmt *stmt, int index, const char *value,
                             int length, void (*destroy)(void *))
{
    const char *sql = sqlite3_sql(stmt);
    if (fault_active && g_str_equal(mode, "startup-reconcile-bind") && index == 2 &&
        g_str_has_prefix(sql, "UPDATE jobs SET status=5") &&
        atomic_fetch_add(&reconcile_binds, 1) % 2 == 1) return SQLITE_NOMEM;
    if (fault_active && ((g_str_equal(mode, "bind-admit") && index == 3 && g_str_has_prefix(sql, "INSERT INTO jobs")) ||
        (g_str_equal(mode, "bind-result") && index == 2 && g_str_has_prefix(sql, "UPDATE jobs SET status=?,progress=100,result="))))
        return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(stmt, index, value, length, destroy);
}
gint64 __wrap_g_get_monotonic_time(void)
{
    return fast_retry ? atomic_load(&fake_time) : __real_g_get_monotonic_time();
}
guint __wrap_g_timeout_add_seconds(guint seconds, GSourceFunc callback, gpointer data)
{
    return fast_retry ? g_timeout_add(2, callback, data) : __real_g_timeout_add_seconds(seconds, callback, data);
}
static gboolean pulse(gpointer unused G_GNUC_UNUSED)
{
    heartbeats++;
    if (fast_retry) atomic_fetch_add(&fake_time, G_USEC_PER_SEC);
    return G_SOURCE_CONTINUE;
}
static void finished(const PcvJobRef *ref, PcvJobStatus status, const gchar *error,
                      gboolean persisted, gboolean pending, const gchar *reason,
                      gpointer unused G_GNUC_UNUSED)
{
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "job_id", pcv_job_id(ref));
    json_object_set_int_member(object, "status_code", status);
    json_object_set_boolean_member(object, "result_persisted", persisted);
    json_object_set_boolean_member(object, "recovery_pending", pending);
    if (error) json_object_set_string_member(object, "error", error);
    if (reason) json_object_set_string_member(object, "reason", reason);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, object);
    gchar *text = json_to_string(node, FALSE); printf("EVENT %s\n", text); fflush(stdout);
    g_free(text); json_node_free(node); deliveries++; delivered = TRUE;
}
static void dump(void)
{
    JsonArray *rows = pcv_job_list(1000);
    JsonNode *node = json_node_new(JSON_NODE_ARRAY); json_node_take_array(node, rows);
    gchar *text = json_to_string(node, FALSE); puts(text); g_free(text); json_node_free(node);
}
static void effect(const gchar *db_path)
{
    gchar *path = g_strconcat(db_path, ".effects", NULL);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(__real_write(fd, "effect\n", 7), ==, 7);
    g_assert_cmpint(__real_fsync(fd), ==, 0); close(fd); g_free(path);
}
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    mode = argv[1]; fast_retry = g_str_equal(mode, "retry-exhaust") || g_str_equal(mode, "startup-reconcile-bind");
    fault_active = g_str_equal(mode, "startup-reconcile-bind");
    g_setenv("PCV_JOBS_DB_PATH", argv[2], TRUE);
    pcv_drain_init(); pcv_job_queue_init();
    guint pulse_id = g_timeout_add(1, pulse, NULL);
    if (g_str_equal(mode, "admit")) {
        gchar *id = pcv_job_create("probe", "identity", NULL);
        g_assert_nonnull(id); puts(id); g_free(id);
    } else if (g_str_equal(mode, "read") || g_str_equal(mode, "cleanup")) {
        if (g_str_equal(mode, "cleanup")) pcv_job_queue_cleanup_old(-1);
        dump();
    } else if (g_str_equal(mode, "startup-locked")) {
        g_assert_null(pcv_job_admit("probe", "identity", NULL, "probe.finish"));
        puts("STARTUP_WAIT"); fflush(stdout);
        gint64 stop_at = __real_g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        while (__real_g_get_monotonic_time() < stop_at) {
            JsonArray *rows = pcv_job_list(10);
            gboolean reconciled = json_array_get_length(rows) == 1 &&
                g_str_equal(json_object_get_string_member(json_array_get_object_element(rows, 0), "status"), "interrupted");
            json_array_unref(rows);
            if (reconciled) break;
            g_main_context_iteration(NULL, TRUE);
        }
        dump();
        PcvJobRef *current = pcv_job_admit("probe", "current-process", NULL, "probe.finish");
        g_assert_nonnull(current); puts("ADMISSION_RESUMED");
        stop_at = __real_g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
        while (__real_g_get_monotonic_time() < stop_at) g_main_context_iteration(NULL, TRUE);
        JsonObject *row = pcv_job_get(pcv_job_id(current));
        g_assert_cmpstr(json_object_get_string_member(row, "status"), ==, "pending");
        json_object_unref(row); pcv_job_unref(current); puts("CURRENT_JOB_PENDING");
    } else if (g_str_equal(mode, "startup-reconcile-bind")) {
        while (attempts < 32) g_main_context_iteration(NULL, TRUE);
        g_assert_null(pcv_job_admit("probe", "identity", NULL, "probe.finish"));
        printf("RECONCILE_RETRIES %u\n", attempts); dump(); fault_active = FALSE;
    } else if (g_str_equal(mode, "burst")) {
                                                                                 
        for (guint i = 0; i < 128; i++) {
            PcvJobRef *ref = pcv_job_admit("probe", "identity", NULL, "probe.finish");
            g_assert_nonnull(ref); effect(argv[2]); fault_active = TRUE;
            pcv_job_finish_async(ref, PCV_JOB_COMPLETED, "{\"effect\":1}", NULL, finished, NULL, NULL);
            pcv_job_unref(ref);
        }
        while (deliveries < 128) g_main_context_iteration(NULL, TRUE);
        printf("IO_PEAK %u DELIVERIES %u\n", atomic_load(&io_peak), deliveries);
        fault_active = FALSE;
    } else if (g_str_equal(mode, "capacity")) {
        GPtrArray *refs = g_ptr_array_new_with_free_func((GDestroyNotify)pcv_job_unref);
        for (guint i = 0; i < 128; i++) {
            PcvJobRef *ref = pcv_job_admit("probe", "identity", NULL, "probe.finish");
            g_assert_nonnull(ref); g_ptr_array_add(refs, ref);
        }
        g_assert_null(pcv_job_admit("probe", "identity", NULL, "probe.finish"));
        printf("CAPACITY %u\n", refs->len); g_ptr_array_unref(refs);
    } else {
        if (g_str_has_prefix(mode, "crash-admit") || g_str_equal(mode, "bind-admit")) fault_active = TRUE;
        PcvJobRef *ref = pcv_job_admit("probe", "identity", "{\"token\":\"request-canary\"}", "probe.finish");
        if (g_str_equal(mode, "reject") || g_str_equal(mode, "bind-admit")) {
            g_assert_null(ref); puts("REJECTED");
        } else {
            g_assert_nonnull(ref);
            printf("ADMITTED %s %s\n", pcv_job_id(ref), pcv_job_generation(ref)); fflush(stdout);
            if (g_str_equal(mode, "tracked")) {
                dump(); pcv_job_unref(ref);
            } else {
                g_assert_true(pcv_job_progress(ref, 10, "worker running"));
                if (g_str_equal(mode, "cancel")) {
                    g_assert_true(pcv_job_cancel(pcv_job_id(ref)));
                    g_assert_false(pcv_job_progress(ref, 50, "late progress"));
                    g_assert_false(pcv_job_set_result(pcv_job_id(ref), PCV_JOB_COMPLETED, NULL));
                }
                sqlite3 *external = NULL;
                gboolean locked = g_str_equal(mode, "locked") || fast_retry;
                if (locked) {
                    g_assert_cmpint(sqlite3_open(argv[2], &external), ==, SQLITE_OK);
                    g_assert_cmpint(sqlite3_exec(external, "BEGIN IMMEDIATE", NULL, NULL, NULL), ==, SQLITE_OK);
                }
                effect(argv[2]);
                if (g_str_equal(mode, "crash-effect")) _exit(81);
                gchar *oversize = g_str_equal(mode, "oversize") ? g_strnfill(65537, 'x') : NULL;
                fault_active = TRUE;
                pcv_job_finish_async(ref, g_str_equal(mode, "failure") ? PCV_JOB_FAILED : PCV_JOB_COMPLETED,
                    oversize ? oversize : "{\"effect\":1}",
                    g_str_equal(mode, "failure") ? "backend failed\n\"quoted\" \\path" : NULL,
                    finished, NULL, NULL);
                g_free(oversize); pcv_job_unref(ref);
                gint64 deadline = __real_g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
                while (!delivered && __real_g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, TRUE);
                g_assert_true(delivered);
                if (fast_retry) {
                    while (attempts < 32 && __real_g_get_monotonic_time() < deadline) g_main_context_iteration(NULL, TRUE);
                    g_assert_cmpuint(attempts, ==, 32);
                    while (pcv_drain_get_work() > 0) g_main_context_iteration(NULL, TRUE);
                    sqlite3_exec(external, "ROLLBACK", NULL, NULL, NULL);
                    gint64 stop = __real_g_get_monotonic_time() + 100000;
                    while (__real_g_get_monotonic_time() < stop) g_main_context_iteration(NULL, TRUE);
                    g_assert_cmpuint(attempts, ==, 32);
                    dump(); printf("RETRIES %u\n", attempts);
                }
                fault_active = FALSE;
                if (external) { if (!fast_retry) sqlite3_exec(external, "ROLLBACK", NULL, NULL, NULL); sqlite3_close(external); }
                printf("HEARTBEATS %u\n", heartbeats);
            }
        }
    }
    g_source_remove(pulse_id);
    while (pcv_drain_get_work() > 0) g_main_context_iteration(NULL, TRUE);
    pcv_job_queue_shutdown(); pcv_drain_shutdown();
    return 0;
}
