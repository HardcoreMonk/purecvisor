






#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "modules/daemons/pcv_trace.h"

static const gchar *fixture_root;
static const gchar *fixture_mode;
static gchar *audit_path;
static GSubprocess *capture_proc;
static guint force_calls, wait_calls, criticals;
static gboolean wait_error_injected;


#undef PCV_TRACE_OUT_ROOT
#define PCV_TRACE_OUT_ROOT fixture_root
#ifndef PCV_TRACE_SOURCE
#define PCV_TRACE_SOURCE "modules/daemons/pcv_trace.c"
#endif
#include PCV_TRACE_SOURCE


void __real_g_subprocess_force_exit(GSubprocess *proc);
gboolean __real_g_subprocess_wait_finish(GSubprocess *proc, GAsyncResult *result, GError **error);
void __wrap_g_subprocess_force_exit(GSubprocess *proc)
{
    force_calls++;
    __real_g_subprocess_force_exit(proc);
}
gboolean __wrap_g_subprocess_wait_finish(GSubprocess *proc, GAsyncResult *result, GError **error)
{
    wait_calls++;
    gboolean ok = __real_g_subprocess_wait_finish(proc, result, error);
    if (ok && !wait_error_injected && g_strcmp0(fixture_mode, "wait-error") == 0) {
        wait_error_injected = TRUE;
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fixture wait result unavailable");
        return FALSE;
    }
    return ok;
}


void pcv_audit_log(const gchar *username, const gchar *method, const gchar *target,
                   const gchar *result, gint code, gint64 duration, const gchar *src_ip)
{
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "method", method);
    json_object_set_string_member(obj, "result", result);
    json_object_set_int_member(obj, "code", code);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *text = json_to_string(node, FALSE);
    FILE *stream = fopen(audit_path, "a");
    g_assert_nonnull(stream);
    fprintf(stream, "%s\n", text);
    fclose(stream);
    g_free(text); json_node_free(node);
}


void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *format, ...)
{
    va_list args;
    va_start(args, format);
    gchar *message = g_strdup_vprintf(format, args);
    va_end(args);
    fprintf(stderr, "%s: %s\n", domain, message);
    g_free(message);
}


static void critical_log(const gchar *domain, GLogLevelFlags level,
                         const gchar *message, gpointer data)
{
    criticals++;
    fprintf(stderr, "%s CRITICAL: %s\n", domain, message);
}


gint pcv_config_get_int(const gchar *section, const gchar *key, gint fallback) { return fallback; }
gboolean pcv_tenant_overlay_get_member_ep(const gchar *tenant, const gchar *vm, gchar **ep)
{
    *ep = NULL;
    return FALSE;
}


GSubprocess *pcv_spawn_newv(const gchar *const *argv, GSubprocessFlags flags, GError **error)
{
    GSubprocess *proc = g_subprocess_newv(argv, flags, error);
    if (proc) capture_proc = g_object_ref(proc);
    return proc;
}
gboolean pcv_spawn_sync_timeout(const gchar *const *argv, gchar **out, gchar **err,
                                guint timeout, GError **error)
{
    g_assert_cmpstr(argv[0], ==, "retis");
    g_assert_cmpstr(argv[1], ==, "--version");
    GSubprocess *proc = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, error);
    if (!proc) return FALSE;
    gboolean ok = g_subprocess_wait_check(proc, NULL, error);
    g_object_unref(proc);
    return ok;
}


static int child_main(int argc, char **argv)
{
    const gchar *data_path = NULL;
    for (int i = 2; i + 1 < argc; i++)
        if (g_str_equal(argv[i], "-o")) data_path = argv[i + 1];
    g_assert_nonnull(data_path);
    g_assert_true(g_file_set_contents(data_path, "fixture capture\n", -1, NULL));
    gchar *ready = g_strconcat(data_path, ".ready", NULL);
    gchar *pid = g_strdup_printf("%d", getpid());
    g_assert_true(g_file_set_contents(ready, pid, -1, NULL));
    g_free(pid); g_free(ready);
    const gchar *mode = g_getenv("PCV_TRACE_CHILD_MODE");
    if (g_strcmp0(mode, "exit") == 0) return 0;
    if (g_strcmp0(mode, "natural") == 0 || g_strcmp0(mode, "error") == 0) {
        g_usleep(300 * 1000);
        return g_strcmp0(mode, "error") == 0 ? 7 : 0;
    }
    for (;;) pause();
}


static void observe(const gchar *phase, const gchar *id, gint64 pid, gboolean action_ok)
{
    JsonObject *obj = pcv_trace_status(id);
    gchar *marker = g_build_filename(fixture_root, id, PCV_TRACE_RUNNING_MARKER, NULL);
    gboolean acquired = pcv_trace_try_acquire();
    if (acquired) pcv_trace_release();
    json_object_set_string_member(obj, "phase", phase);
    json_object_set_string_member(obj, "marker", marker);
    json_object_set_int_member(obj, "pid", pid);
    json_object_set_boolean_member(obj, "guard_available", acquired);
    json_object_set_boolean_member(obj, "action_ok", action_ok);
    json_object_set_int_member(obj, "force_calls", force_calls);
    json_object_set_int_member(obj, "wait_calls", wait_calls);
    json_object_set_int_member(obj, "criticals", criticals);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *text = json_to_string(node, FALSE);
    g_print("%s\n", text); fflush(stdout);
    g_free(text); json_node_free(node); g_free(marker);
}


static void control(void)
{
    char text[32];
    g_assert_nonnull(fgets(text, sizeof text, stdin));
}


static void drain(const gchar *id, gboolean expect_wait_error)
{
    gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, FALSE);
        JsonObject *status = pcv_trace_status(id);
        gboolean idle = g_str_equal(json_object_get_string_member(status, "state"), "idle");
        json_object_unref(status);
        if (idle || (expect_wait_error && wait_error_injected)) return;
        g_usleep(1000);
    }
    g_error("fixture callback deadline exceeded");
}


static void backstop(const gchar *id)
{
    if (g_trace && g_trace->backstop_id) g_source_remove(g_trace->backstop_id);
    _trace_backstop_cb((gpointer)id);
}


int main(int argc, char **argv)
{
    if (argc > 1 && g_str_equal(argv[1], "--version")) return 0;
    if (argc > 1 && g_str_equal(argv[1], "collect")) return child_main(argc, argv);
    if (argc != 3) return 2;
    fixture_root = argv[1]; fixture_mode = argv[2];
    audit_path = g_build_filename(fixture_root, "audit.jsonl", NULL);
    g_assert_cmpint(g_mkdir_with_parents(fixture_root, 0700), ==, 0);
    g_log_set_handler("GLib-GIO", G_LOG_LEVEL_CRITICAL, critical_log, NULL);
    PcvTraceFilter filter = { .timebox_sec = 60 };
    GError *error = NULL;
    gchar *id = pcv_trace_start(&filter, "fixture", &error);
    g_assert_no_error(error); g_assert_nonnull(id);
    gchar *ready = g_build_filename(fixture_root, id, "run.data.ready", NULL);
    gint64 deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
    while (!g_file_test(ready, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline) g_usleep(1000);
    g_assert_true(g_file_test(ready, G_FILE_TEST_EXISTS));

    gchar *pid_text = NULL;
    g_assert_true(g_file_get_contents(ready, &pid_text, NULL, NULL));
    gint64 pid = g_ascii_strtoll(pid_text, NULL, 10);
    g_assert_cmpint(pid, >, 0);
    g_free(pid_text); g_free(ready);
    if (g_str_equal(fixture_mode, "already-reaped")) g_assert_true(g_subprocess_wait(capture_proc, NULL, NULL));
    observe("before", id, pid, TRUE);
    control();
    gboolean ok = TRUE;
    if (g_str_equal(fixture_mode, "stale-callback")) {
        const gchar *old_argv[] = { "true", NULL };
        GSubprocess *old = g_subprocess_newv(old_argv, G_SUBPROCESS_FLAGS_NONE, &error);
        g_assert_no_error(error); g_assert_nonnull(old);
        g_subprocess_wait_async(old, NULL, _trace_wait_done, g_strdup(id));
        gint64 stale_deadline = g_get_monotonic_time() + G_USEC_PER_SEC;
        while (!wait_calls && g_get_monotonic_time() < stale_deadline) {
            g_main_context_iteration(NULL, FALSE); g_usleep(1000);
        }
        g_assert_cmpuint(wait_calls, >, 0); g_object_unref(old);
    } else if (g_str_has_prefix(fixture_mode, "backstop")) {
        backstop(id);
        if (g_str_equal(fixture_mode, "backstop-then-stop")) ok = pcv_trace_stop(id, &error);
    } else if (g_str_equal(fixture_mode, "wrong-id") || g_str_equal(fixture_mode, "empty-id")) {
        ok = pcv_trace_stop(g_str_equal(fixture_mode, "wrong-id") ? "other-id" : "", &error);
        g_assert_false(ok); g_clear_error(&error);
    } else if (!g_str_equal(fixture_mode, "natural") && !g_str_equal(fixture_mode, "error-exit")) {
        ok = pcv_trace_stop(id, &error);
        if (g_str_equal(fixture_mode, "repeat-stop") && ok) ok = pcv_trace_stop(id, &error);
        if (g_str_equal(fixture_mode, "stop-then-backstop")) backstop(id);
    }
    g_clear_error(&error);
    observe("pending", id, pid, ok);
    control();
    if (g_trace && (g_str_equal(fixture_mode, "stale-callback") || g_str_equal(fixture_mode, "wrong-id") ||
        g_str_equal(fixture_mode, "empty-id"))) {
        g_assert_true(pcv_trace_stop(id, &error)); g_assert_no_error(error);
    }
    drain(id, g_str_equal(fixture_mode, "wait-error"));
    observe("after", id, pid, TRUE);
    control();

    if (g_trace) {
        g_subprocess_wait_async(capture_proc, NULL, _trace_wait_done, g_strdup(id));
        drain(id, FALSE);
    }

    __real_g_subprocess_force_exit(capture_proc);
    g_assert_true(g_subprocess_wait(capture_proc, NULL, NULL));
    g_clear_object(&capture_proc); g_free(audit_path); g_free(id);
    return 0;
}
