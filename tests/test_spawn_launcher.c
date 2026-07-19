
#include <glib.h>
#include <gio/gio.h>
#include "../src/utils/pcv_spawn.h"

static void
test_launcher_init_shutdown(void)
{
    pcv_spawn_launcher_init();
    pcv_spawn_launcher_shutdown();

    pcv_spawn_launcher_init();
    pcv_spawn_launcher_shutdown();
}

static void
test_launcher_double_init(void)
{
    pcv_spawn_launcher_init();
    pcv_spawn_launcher_init();
    pcv_spawn_launcher_shutdown();
}

static void
test_sync_true(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/true", NULL };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);

    g_assert_true(ok);
    g_assert_null(err);

    pcv_spawn_launcher_shutdown();
}

static void
test_sync_false(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/false", NULL };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);

    g_assert_false(ok);
    g_assert_nonnull(err);
    g_error_free(err);

    pcv_spawn_launcher_shutdown();
}

static void
test_sync_stdout_capture(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/echo", "hello-purecvisor", NULL };
    GError *err   = NULL;
    gchar  *out   = NULL;
    gboolean ok = pcv_spawn_sync(argv, &out, NULL, &err);

    g_assert_true(ok);
    g_assert_nonnull(out);

    g_assert_true(g_str_has_prefix(out, "hello-purecvisor"));

    g_free(out);
    pcv_spawn_launcher_shutdown();
}

static void
test_sync_stderr_capture(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/ls", "/nonexistent-path-xyz", NULL };
    GError *err   = NULL;
    gchar  *serr  = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &serr, &err);

    g_assert_false(ok);

    g_assert_true(serr != NULL || err != NULL);

    g_free(serr);
    if (err) g_error_free(err);
    pcv_spawn_launcher_shutdown();
}

static void
test_fire_ok(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/true", NULL };
    pcv_spawn_fire(argv);

    g_usleep(50000);

    pcv_spawn_launcher_shutdown();
}

static void
test_fire_nonexistent(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/nonexistent/binary", NULL };
    pcv_spawn_fire(argv);

    pcv_spawn_launcher_shutdown();
}

static void
test_sync_without_launcher(void)
{

    const gchar *argv[] = { "/bin/true", NULL };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);

    g_assert_true(ok);
    g_assert_null(err);
}

static void
test_pipe_sync_counts_streamed_bytes(void)
{
    pcv_spawn_launcher_init();

    const gchar *producer[] = { "/usr/bin/printf", "abcde", NULL };
    const gchar *consumer[] = { "/usr/bin/wc", "-c", NULL };
    GError *err = NULL;
    gchar *out = NULL;
    gchar *stderr_buf = NULL;

    gboolean ok = pcv_spawn_pipe_sync(producer, consumer, &out, &stderr_buf, &err);

    g_assert_true(ok);
    g_assert_null(err);
    g_assert_nonnull(out);
    g_assert_cmpint((gint)g_ascii_strtoll(out, NULL, 10), ==, 5);
    g_assert_nonnull(stderr_buf);
    g_assert_cmpstr(stderr_buf, ==, "");

    g_free(out);
    g_free(stderr_buf);
    pcv_spawn_launcher_shutdown();
}

static void
test_pipe_sync_consumer_failure(void)
{
    pcv_spawn_launcher_init();

    const gchar *producer[] = { "/usr/bin/printf", "abcde", NULL };
    const gchar *consumer[] = { "/bin/false", NULL };
    GError *err = NULL;
    gchar *stderr_buf = NULL;

    gboolean ok = pcv_spawn_pipe_sync(producer, consumer, NULL, &stderr_buf, &err);

    g_assert_false(ok);
    g_assert_nonnull(err);
    g_assert_nonnull(stderr_buf);

    g_error_free(err);
    g_free(stderr_buf);
    pcv_spawn_launcher_shutdown();
}

static void
test_sync_timeout_fires(void)
{
    pcv_spawn_launcher_init();
    const gchar *argv[] = { "/bin/sleep", "60", NULL };
    GError *err = NULL;
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, NULL, 1, &err);
    gdouble elapsed = (g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC;
    g_assert_false(ok);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_cmpfloat(elapsed, <, 10.0);
    g_clear_error(&err);
    pcv_spawn_launcher_shutdown();
}

static void
test_sync_timeout_normal(void)
{
    pcv_spawn_launcher_init();
    const gchar *argv[] = { "/bin/true", NULL };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, NULL, 5, &err);
    g_assert_true(ok);
    g_assert_no_error(err);
    pcv_spawn_launcher_shutdown();
}

static void
test_sync_timeout_zero_unbounded(void)
{
    pcv_spawn_launcher_init();
    const gchar *argv[] = { "/bin/true", NULL };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, NULL, 0, &err);
    g_assert_true(ok);
    g_assert_no_error(err);
    pcv_spawn_launcher_shutdown();
}

void
test_spawn_launcher_register(void)
{
    g_test_add_func("/spawn_launcher/init_shutdown",       test_launcher_init_shutdown);
    g_test_add_func("/spawn_launcher/double_init",         test_launcher_double_init);
    g_test_add_func("/spawn_launcher/sync_true",           test_sync_true);
    g_test_add_func("/spawn_launcher/sync_false",          test_sync_false);
    g_test_add_func("/spawn_launcher/sync_stdout_capture", test_sync_stdout_capture);
    g_test_add_func("/spawn_launcher/sync_stderr_capture", test_sync_stderr_capture);
    g_test_add_func("/spawn_launcher/fire_ok",             test_fire_ok);
    g_test_add_func("/spawn_launcher/fire_nonexistent",    test_fire_nonexistent);
    g_test_add_func("/spawn_launcher/sync_no_launcher",    test_sync_without_launcher);
    g_test_add_func("/spawn_launcher/pipe_sync_counts_bytes",
                    test_pipe_sync_counts_streamed_bytes);
    g_test_add_func("/spawn_launcher/pipe_sync_consumer_failure",
                    test_pipe_sync_consumer_failure);
    g_test_add_func("/spawn_launcher/sync_timeout_fires",          test_sync_timeout_fires);
    g_test_add_func("/spawn_launcher/sync_timeout_normal",         test_sync_timeout_normal);
    g_test_add_func("/spawn_launcher/sync_timeout_zero_unbounded", test_sync_timeout_zero_unbounded);
}
