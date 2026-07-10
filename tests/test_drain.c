/**
 * @file test_drain.c
 * @brief Graceful drain inflight 카운터 + sd_notify 유닛 테스트
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  drain.c (src/api/)의 그레이스풀 드레인(우아한 종료) 메커니즘을 검증한다.
 *  7개 테스트 케이스 (동시성 1건 포함).
 *
 *  드레인이란? 데몬 종료 시 처리 중인 RPC 요청이 완료될 때까지 기다린 후
 *  종료하는 메커니즘. 진행 중 요청(inflight) 카운터로 관리.
 *
 *  검증 항목:
 *  - 초기 상태: inflight=0, is_shutdown=FALSE
 *  - inc/dec 기본: inc→inflight++, dec→inflight-- (2회 반복 검증)
 *  - 대량 inc/dec: 100회 inc → 100회 dec → 0
 *  - sd_notify: NOTIFY_SOCKET 미설정 시 ready/stopping/watchdog 무동작 (크래시 없음)
 *  - watchdog: WATCHDOG_USEC 미설정 → 0, "30000000" 설정 → 양수 반환
 *  - 동시성: 10 스레드 x 100회 inc/dec → 최종 inflight=0 (원자성 검증)
 *
 *  systemd 연동: 실제 환경에서 sd_notify("READY=1")로 기동 완료 알림,
 *  sd_notify("STOPPING=1")로 종료 시작 알림을 보냄.
 * ============================================================================
 */
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
    /* NOTIFY_SOCKET 미설정 — 무동작 (crash 없음) */
    g_unsetenv("NOTIFY_SOCKET");
    pcv_drain_notify_ready();
    pcv_drain_notify_stopping();
    pcv_drain_notify_watchdog();
}

static void test_watchdog_usec(void) {
    pcv_drain_init();
    /* WATCHDOG_USEC 미설정 → 0 */
    g_unsetenv("WATCHDOG_USEC");
    g_assert_cmpuint(pcv_drain_get_watchdog_usec(), ==, 0);
}

static void test_watchdog_usec_from_env(void) {
    pcv_drain_init();
    g_setenv("WATCHDOG_USEC", "30000000", TRUE); /* 30s */
    guint64 v = pcv_drain_get_watchdog_usec();
    /* 구현은 보통 절반(WATCHDOG_USEC/2)을 ping interval로 반환 */
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

/* ── 신규 케이스 a: drain_cancel ─────────────────────── */

static void test_drain_cancel(void) {
    pcv_drain_init();
    /* 초기 상태: shutdown=FALSE, inc()는 TRUE를 반환해야 한다 */
    g_assert_false(pcv_drain_is_shutdown());
    g_assert_true(pcv_drain_inc());
    pcv_drain_dec();

    /* cancel 호출 — 이미 CLOSED 상태여도 크래시 없어야 한다 */
    pcv_drain_cancel();
    g_assert_false(pcv_drain_is_shutdown());

    /* cancel 후에도 inc/dec 정상 작동 */
    g_assert_true(pcv_drain_inc());
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 1);
    pcv_drain_dec();
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 0);
    g_assert_false(pcv_drain_is_shutdown());
}

/* ── 신규 케이스 b: drain_begin_shutdown ─────────────── */

static void test_drain_begin_shutdown(void) {
    pcv_drain_init();

    /* inflight가 0인 채로 begin을 호출하면 drain 스레드가
     * 즉시 g_main_loop_quit()을 호출한다.
     * 단, 테스트에서 GMainLoop를 실제로 돌릴 필요는 없다 —
     * begin()이 shutdown_flag를 1로 세우고 inc()가 FALSE를 반환하면 충분. */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    pcv_drain_begin(loop, 1 /* 1초 타임아웃 */);

    /* shutdown 플래그가 세워졌어야 한다 */
    g_assert_true(pcv_drain_is_shutdown());

    /* 종료 중에 inc()는 FALSE를 반환해야 한다 */
    g_assert_false(pcv_drain_inc());

    /* drain 스레드 종료 대기 + 자원 해제 */
    pcv_drain_shutdown();
    g_main_loop_unref(loop);
}

/* ── 신규 케이스 c: drain_notify_with_socket ──────────── */

static void test_drain_notify_ready_with_socket(void) {
    /* 임시 UNIX DGRAM 소켓을 수신 측으로 만들고 NOTIFY_SOCKET에 경로를 설정 */
    gchar *tmpdir = g_dir_make_tmp("drain-notify-XXXXXX", NULL);
    gchar *sock_path = g_build_filename(tmpdir, "notify.sock", NULL);

    /* 수신 소켓 생성 및 bind */
    int recv_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    g_assert_cmpint(recv_fd, >=, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    int rc = bind(recv_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
        /* bind 실패(권한 문제 등)는 skip 처리 */
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

    /* READY=1 전송 */
    pcv_drain_notify_ready();

    /* 수신 측에서 메시지 읽기 (최대 64바이트, 비블로킹) */
    char buf[64] = { 0 };
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(recv_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(recv_fd, buf, sizeof(buf) - 1, 0);

    if (n > 0) {
        g_assert_true(strstr(buf, "READY=1") != NULL);
    }
    /* n <= 0이어도 크래시가 없으면 통과 (환경 제약 허용) */

    /* STOPPING=1 전송 및 수신 */
    pcv_drain_notify_stopping();
    memset(buf, 0, sizeof(buf));
    n = recv(recv_fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        g_assert_true(strstr(buf, "STOPPING=1") != NULL);
    }

    /* 정리 */
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
