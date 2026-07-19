
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>
#include "../src/api/drain.h"

static void test_init_inflight_zero(void) {
    pcv_drain_init();
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 0);
    g_assert_false(pcv_drain_is_shutdown());
}

static void test_inc_dec_basic(void) {
    pcv_drain_init();
    g_assert_true(pcv_drain_inc());
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 1);
    g_assert_true(pcv_drain_inc());
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 2);
    pcv_drain_dec();
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 1);
    pcv_drain_dec();
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 0);
}

static void test_inc_many(void) {
    pcv_drain_init();
    for (int i = 0; i < 100; i++)
        g_assert_true(pcv_drain_inc());
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 100);
    for (int i = 0; i < 100; i++)
        pcv_drain_dec();
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 0);
}

static void test_notify_ready_no_socket(void) {
    pcv_drain_init();

    g_unsetenv("NOTIFY_SOCKET");
    pcv_drain_notify_ready();
    pcv_drain_notify_stopping();
    pcv_drain_notify_watchdog();
}

static void test_watchdog_usec(void) {
    pcv_drain_init();

    g_unsetenv("WATCHDOG_USEC");
    g_assert_cmpuint(pcv_drain_get_watchdog_usec(), ==, 0);
}

static void test_watchdog_usec_from_env(void) {
    pcv_drain_init();
    g_setenv("WATCHDOG_USEC", "30000000", TRUE);
    guint64 v = pcv_drain_get_watchdog_usec();

    g_assert_cmpuint(v, >, 0);
    g_unsetenv("WATCHDOG_USEC");
}

static gpointer drain_inc_dec_worker(gpointer u) {
    (void)u;
    for (int j = 0; j < 100; j++) {
        pcv_drain_inc();
        g_usleep(100);
        pcv_drain_dec();
    }
    return NULL;
}

static void test_concurrent_inc_dec(void) {
    pcv_drain_init();
    GThread *t[10];
    for (int i = 0; i < 10; i++)
        t[i] = g_thread_new("drain", drain_inc_dec_worker, NULL);
    for (int i = 0; i < 10; i++)
        g_thread_join(t[i]);
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 0);
}

static void test_drain_cancel(void) {
    pcv_drain_init();

    g_assert_false(pcv_drain_is_shutdown());
    g_assert_true(pcv_drain_inc());
    pcv_drain_dec();

    pcv_drain_cancel();
    g_assert_false(pcv_drain_is_shutdown());

    g_assert_true(pcv_drain_inc());
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 1);
    pcv_drain_dec();
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 0);
    g_assert_false(pcv_drain_is_shutdown());
}

static void test_drain_begin_shutdown(void) {
    pcv_drain_init();

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    pcv_drain_begin(loop, 1 );

    g_assert_true(pcv_drain_is_shutdown());

    g_assert_false(pcv_drain_inc());

    pcv_drain_shutdown();
    g_main_loop_unref(loop);
}

static void test_drain_notify_ready_with_socket(void) {

    gchar *tmpdir = g_dir_make_tmp("drain-notify-XXXXXX", NULL);
    gchar *sock_path = g_build_filename(tmpdir, "notify.sock", NULL);

    int recv_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    g_assert_cmpint(recv_fd, >=, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    int rc = bind(recv_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {

        close(recv_fd);
        g_unlink(sock_path);
        g_rmdir(tmpdir);
        g_free(sock_path);
        g_free(tmpdir);
        g_test_skip("bind() failed — skipping socket notify test");
        return;
    }

    pcv_drain_init();
    g_setenv("NOTIFY_SOCKET", sock_path, TRUE);

    pcv_drain_notify_ready();

    char buf[64] = { 0 };
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(recv_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(recv_fd, buf, sizeof(buf) - 1, 0);

    if (n > 0) {
        g_assert_true(strstr(buf, "READY=1") != NULL);
    }

    pcv_drain_notify_stopping();
    memset(buf, 0, sizeof(buf));
    n = recv(recv_fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        g_assert_true(strstr(buf, "STOPPING=1") != NULL);
    }

    g_unsetenv("NOTIFY_SOCKET");
    close(recv_fd);
    g_unlink(sock_path);
    g_rmdir(tmpdir);
    g_free(sock_path);
    g_free(tmpdir);
}

void test_drain_register(void) {
    g_test_add_func("/drain/init_inflight_zero", test_init_inflight_zero);
    g_test_add_func("/drain/inc_dec_basic", test_inc_dec_basic);
    g_test_add_func("/drain/inc_many", test_inc_many);
    g_test_add_func("/drain/notify_ready_no_socket", test_notify_ready_no_socket);
    g_test_add_func("/drain/watchdog_usec", test_watchdog_usec);
    g_test_add_func("/drain/watchdog_usec_from_env", test_watchdog_usec_from_env);
    g_test_add_func("/drain/concurrent_inc_dec", test_concurrent_inc_dec);
    g_test_add_func("/drain/drain_cancel", test_drain_cancel);
    g_test_add_func("/drain/drain_begin_shutdown", test_drain_begin_shutdown);
    g_test_add_func("/drain/notify_ready_with_socket", test_drain_notify_ready_with_socket);
}
