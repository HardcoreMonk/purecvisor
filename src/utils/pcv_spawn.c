








































































#include "pcv_spawn.h"
#include "pcv_log.h"

#include <gio/gio.h>
#include <signal.h>
#include <string.h>


#define SPAWN_LOG_DOM "pcv_spawn"








static GSubprocessLauncher *g_launcher = NULL;






























void
pcv_spawn_launcher_init(void)
{
    if (G_UNLIKELY(g_launcher != NULL)) {
        PCV_LOG_WARN(SPAWN_LOG_DOM, "pcv_spawn_launcher_init() called twice — ignored");
        return;
    }






    g_launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);









    g_subprocess_launcher_setenv(g_launcher, "PATH",
        "/usr/sbin:/usr/bin:/sbin:/bin", TRUE);
    g_subprocess_launcher_setenv(g_launcher, "HOME",
        "/root", TRUE);
    g_subprocess_launcher_setenv(g_launcher, "LANG",
        "C.UTF-8", TRUE);







    g_subprocess_launcher_set_cwd(g_launcher, "/");

    PCV_LOG_INFO(SPAWN_LOG_DOM,
                 "GSubprocessLauncher initialized "
                 "(cwd=/, PATH=/usr/sbin:..., LANG=C.UTF-8)");
}








void
pcv_spawn_launcher_shutdown(void)
{
    if (g_launcher) {
        g_clear_object(&g_launcher);
        PCV_LOG_INFO(SPAWN_LOG_DOM, "GSubprocessLauncher shutdown.");
    }
}











static GSubprocess *
_spawn_with_flags(const gchar * const *argv,
                  GSubprocessFlags     flags,
                  GError             **error)
{
    if (g_launcher != NULL) {










        g_subprocess_launcher_set_flags(g_launcher, flags);
        return g_subprocess_launcher_spawnv(g_launcher, argv, error);
    }


    PCV_LOG_WARN(SPAWN_LOG_DOM,
                 "launcher not initialized, falling back to g_subprocess_newv");
    return g_subprocess_newv(argv, flags, error);
}












































gboolean
pcv_spawn_sync(const gchar * const *argv,
               gchar              **stdout_out,
               gchar              **stderr_out,
               GError             **error)
{

    g_return_val_if_fail(argv && argv[0], FALSE);







    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_NONE;

    if (stdout_out)
        flags |= G_SUBPROCESS_FLAGS_STDOUT_PIPE;
    else
        flags |= G_SUBPROCESS_FLAGS_STDOUT_SILENCE;

    if (stderr_out)
        flags |= G_SUBPROCESS_FLAGS_STDERR_PIPE;
    else
        flags |= G_SUBPROCESS_FLAGS_STDERR_SILENCE;


    GSubprocess *proc = _spawn_with_flags(argv, flags, error);
    if (!proc)
        return FALSE;

    gchar *captured_out = NULL;
    gchar *captured_err = NULL;








    if (!g_subprocess_communicate_utf8(proc, NULL, NULL,
                                       stdout_out ? &captured_out : NULL,
                                       stderr_out ? &captured_err : NULL,
                                       error)) {
        g_object_unref(proc);
        return FALSE;
    }


    if (stdout_out) *stdout_out = captured_out;
    if (stderr_out) *stderr_out = captured_err;


    gboolean success = g_subprocess_get_successful(proc);
    g_object_unref(proc);

    if (!success) {

        if (error && !*error) {
            const gchar *err_msg = (stderr_out && *stderr_out && **stderr_out)
                                   ? g_strstrip(*stderr_out)
                                   : "process exited with non-zero status";
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "%s: %s", argv[0], err_msg);
        }
        return FALSE;
    }

    return TRUE;
}

static gchar *
_read_stream_to_string(GInputStream *stream)
{
    if (!stream)
        return g_strdup("");

    GString *buf = g_string_new(NULL);
    guint8 chunk[4096];
    GError *local_error = NULL;

    for (;;) {
        gssize n = g_input_stream_read(stream, chunk, sizeof(chunk),
                                       NULL, &local_error);
        if (n > 0) {
            g_string_append_len(buf, (const gchar *)chunk, n);
            continue;
        }
        if (n < 0) {
            PCV_LOG_WARN(SPAWN_LOG_DOM, "stream read failed: %s",
                         local_error ? local_error->message : "unknown");
            g_clear_error(&local_error);
        }
        break;
    }

    return g_string_free(buf, FALSE);
}

static gchar *
_combine_pipe_stderr(const gchar *producer_err, const gchar *consumer_err)
{
    gboolean has_producer = producer_err && *producer_err;
    gboolean has_consumer = consumer_err && *consumer_err;

    if (has_producer && has_consumer)
        return g_strdup_printf("producer: %s\nconsumer: %s",
                               producer_err, consumer_err);
    if (has_producer)
        return g_strdup(producer_err);
    if (has_consumer)
        return g_strdup(consumer_err);
    return g_strdup("");
}











gboolean
pcv_spawn_pipe_sync(const gchar * const *producer_argv,
                    const gchar * const *consumer_argv,
                    gchar              **consumer_stdout_out,
                    gchar              **combined_stderr_out,
                    GError             **error)
{
    g_return_val_if_fail(producer_argv && producer_argv[0], FALSE);
    g_return_val_if_fail(consumer_argv && consumer_argv[0], FALSE);

    if (consumer_stdout_out)
        *consumer_stdout_out = NULL;
    if (combined_stderr_out)
        *combined_stderr_out = NULL;

    GSubprocess *producer = NULL;
    GSubprocess *consumer = NULL;
    gchar *producer_err = NULL;
    gchar *consumer_err = NULL;
    gchar *consumer_out = NULL;
    gboolean ok = FALSE;

    producer = _spawn_with_flags(producer_argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        error);
    if (!producer)
        goto cleanup;

    GSubprocessFlags consumer_flags =
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
    consumer_flags |= consumer_stdout_out ? G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                          : G_SUBPROCESS_FLAGS_STDOUT_SILENCE;
    consumer = _spawn_with_flags(consumer_argv, consumer_flags, error);
    if (!consumer) {
        g_subprocess_force_exit(producer);
        (void)g_subprocess_wait(producer, NULL, NULL);
        goto cleanup;
    }

    GInputStream *producer_stdout = g_subprocess_get_stdout_pipe(producer);
    GOutputStream *consumer_stdin = g_subprocess_get_stdin_pipe(consumer);

    struct sigaction old_pipe_action;
    struct sigaction ignore_pipe_action = {0};
    ignore_pipe_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe_action.sa_mask);
    gboolean restore_sigpipe =
        sigaction(SIGPIPE, &ignore_pipe_action, &old_pipe_action) == 0;

    gboolean spliced = g_output_stream_splice(
        consumer_stdin,
        producer_stdout,
        G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE |
        G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
        NULL,
        error);

    if (restore_sigpipe)
        sigaction(SIGPIPE, &old_pipe_action, NULL);

    if (!spliced) {
        g_subprocess_force_exit(producer);
        g_subprocess_force_exit(consumer);
        goto collect_output;
    }

collect_output:
    if (consumer_stdout_out)
        consumer_out = _read_stream_to_string(g_subprocess_get_stdout_pipe(consumer));
    producer_err = _read_stream_to_string(g_subprocess_get_stderr_pipe(producer));
    consumer_err = _read_stream_to_string(g_subprocess_get_stderr_pipe(consumer));

    GError *producer_wait_error = NULL;
    GError *consumer_wait_error = NULL;
    gboolean producer_ok = g_subprocess_wait_check(producer, NULL,
                                                   &producer_wait_error);
    gboolean consumer_ok = g_subprocess_wait_check(consumer, NULL,
                                                   &consumer_wait_error);

    ok = producer_ok && consumer_ok;
    if (!ok && error && !*error) {
        gchar *combined = _combine_pipe_stderr(producer_err, consumer_err);
        const gchar *detail = combined && *combined ? combined
                                                    : "pipeline exited with non-zero status";
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "%s -> %s failed: %s",
                    producer_argv[0], consumer_argv[0], detail);
        g_free(combined);
    }

    g_clear_error(&producer_wait_error);
    g_clear_error(&consumer_wait_error);

cleanup:
    if (combined_stderr_out)
        *combined_stderr_out = _combine_pipe_stderr(producer_err, consumer_err);
    if (consumer_stdout_out)
        *consumer_stdout_out = g_steal_pointer(&consumer_out);

    g_free(producer_err);
    g_free(consumer_err);
    g_free(consumer_out);
    g_clear_object(&producer);
    g_clear_object(&consumer);
    return ok;
}





























void
pcv_spawn_fire(const gchar * const *argv)
{
    g_return_if_fail(argv && argv[0]);

    GError     *error = NULL;
    GSubprocess *proc = _spawn_with_flags(
        argv,
        G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &error);

    if (!proc) {
        PCV_LOG_WARN(SPAWN_LOG_DOM, "fire failed [%s]: %s",
                     argv[0], error->message);
        g_error_free(error);
        return;
    }


    g_object_unref(proc);
}
