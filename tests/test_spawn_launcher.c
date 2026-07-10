/* tests/test_spawn_launcher.c
 *
 * 대상 모듈: src/utils/pcv_spawn.c — 안전한 프로세스 스폰 (system/popen 대체)
 *
 * 이 테스트가 검증하는 것:
 *   pcv_spawn_sync (동기 실행 + stdout/stderr 캡처)와
 *   pcv_spawn_fire (fire-and-forget), pcv_spawn_pipe_sync(스트리밍 파이프)의
 *   정상/실패/폴백 동작을 검사한다.
 *   init/shutdown 사이클, 이중 init 방어, launcher 미초기화 시 폴백 포함.
 *
 * 실행: sudo ./test_runner -p /spawn_launcher
 *
 * 외부 의존: /bin/true, /bin/echo, /bin/false, /bin/ls, /usr/bin/printf,
 *             /usr/bin/wc (시스템 바이너리)
 */

#include <glib.h>
#include <gio/gio.h>
#include "../src/utils/pcv_spawn.h"

/* ── 테스트 케이스 ──────────────────────────────────── */

/* 1. launcher init/shutdown 사이클 — crash 없음 */
static void
test_launcher_init_shutdown(void)
{
    pcv_spawn_launcher_init();
    pcv_spawn_launcher_shutdown();
    /* 재초기화도 안전해야 함 */
    pcv_spawn_launcher_init();
    pcv_spawn_launcher_shutdown();
}

/* 2. 이중 init — WARN 출력 후 무시, crash 없음 */
static void
test_launcher_double_init(void)
{
    pcv_spawn_launcher_init();
    pcv_spawn_launcher_init();   /* 2회 — WARN 후 무시 */
    pcv_spawn_launcher_shutdown();
}

/* 3. pcv_spawn_sync: /bin/true → TRUE 반환 */
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

/* 4. pcv_spawn_sync: /bin/false → FALSE + GError 설정 */
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

/* 5. pcv_spawn_sync: stdout 캡처 — /bin/echo hello */
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
    /* echo 는 개행 포함 출력 */
    g_assert_true(g_str_has_prefix(out, "hello-purecvisor"));

    g_free(out);
    pcv_spawn_launcher_shutdown();
}

/* 6. pcv_spawn_sync: stderr 캡처 — /bin/ls 없는경로 */
static void
test_sync_stderr_capture(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/ls", "/nonexistent-path-xyz", NULL };
    GError *err   = NULL;
    gchar  *serr  = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &serr, &err);

    g_assert_false(ok);
    /* stderr 에 무언가 출력됐거나 GError 에 메시지가 있어야 함 */
    g_assert_true(serr != NULL || err != NULL);

    g_free(serr);
    if (err) g_error_free(err);
    pcv_spawn_launcher_shutdown();
}

/* 7. pcv_spawn_fire: /bin/true — crash/error 없음 */
static void
test_fire_ok(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/true", NULL };
    pcv_spawn_fire(argv);   /* 반환값 없음, crash 없어야 함 */

    /* 짧게 대기 — fire-and-forget 이므로 자식 종료를 기다리지 않음 */
    g_usleep(50000);  /* 50ms */

    pcv_spawn_launcher_shutdown();
}

/* 8. pcv_spawn_fire: 존재하지 않는 바이너리 — WARN 만 출력, crash 없음 */
static void
test_fire_nonexistent(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/nonexistent/binary", NULL };
    pcv_spawn_fire(argv);   /* WARN 로그만 출력, crash 없어야 함 */

    pcv_spawn_launcher_shutdown();
}

/* 9. launcher 없는 폴백: shutdown 상태에서 sync 호출 */
static void
test_sync_without_launcher(void)
{
    /* launcher 를 초기화하지 않은 상태 — 폴백 경로로 동작해야 함 */
    const gchar *argv[] = { "/bin/true", NULL };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);

    g_assert_true(ok);
    g_assert_null(err);
}

/* 10. pcv_spawn_pipe_sync: producer stdout → consumer stdin */
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

/* 11. pcv_spawn_pipe_sync: consumer 실패 시 FALSE + GError */
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

/* [R5] 타임아웃 발화: sleep 60 을 1s 타임아웃으로 → ~1s 내 FALSE + TIMED_OUT */
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
    g_assert_cmpfloat(elapsed, <, 10.0);   /* 60s 안 기다림 — 타임아웃 발화 증명 */
    g_clear_error(&err);
    pcv_spawn_launcher_shutdown();
}

/* [R5] 타임아웃 미발화: 빠른 명령 + 넉넉한 타임아웃 → TRUE */
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

/* [R5] timeout=0 무제한: 기존 경로 무변경 회귀 */
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

/* ── 등록 함수 ──────────────────────────────────────── */
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
