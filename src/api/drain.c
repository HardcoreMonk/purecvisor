/**
 * @file drain.c
 * @brief Graceful Drain — 종료 시 진행 중 RPC 안전 완료 보장 (259 LOC)
 *
 * 아키텍처 위치:
 *   main.c의 시그널 핸들러(on_signal_received)에서 pcv_drain_begin()을 호출하고,
 *   uds_server.c의 요청 수신 경로에서 pcv_drain_inc()/dec()으로 inflight를 추적합니다.
 *   디스패처나 핸들러는 이 모듈을 직접 호출하지 않습니다.
 *
 *   [SIGTERM] → main.c on_signal_received()
 *                  → pcv_drain_begin(loop, 30)
 *                      → shutdown_flag = 1 (새 요청 거부)
 *                      → sd_notify STOPPING=1
 *                      → drain 스레드 기동 (inflight==0 대기)
 *                          → g_main_loop_quit()
 *
 * 주요 흐름 (요청 생명주기와의 관계):
 *   정상 운영 중:
 *     uds_server.c: 요청 수신 → pcv_drain_inc() [TRUE 반환] → 디스패처 호출
 *     핸들러: 응답 전송 → uds_server.c: pcv_drain_dec()
 *   종료 중:
 *     uds_server.c: 요청 수신 → pcv_drain_inc() [FALSE 반환] → -32000 에러 즉시 반환
 *
 * 핵심 패턴:
 *   - 원자적 카운터: g_atomic_int 사용으로 락 없이 inc/dec (성능 우선)
 *   - 조건 변수 대기: inflight==0이 되면 g_cond_broadcast()로 drain 스레드를 깨움
 *   - 타임아웃 강제 종료: g_cond_wait_until()로 최대 대기 시간(기본 30초) 보장
 *   - sd_notify 네이티브 구현: libsystemd 링크 의존성 없이 NOTIFY_SOCKET 환경변수로
 *     Unix datagram 소켓에 직접 "STOPPING=1" 또는 "READY=1" 전송
 *     (abstract namespace '@' 접두사도 지원)
 *
 * 주의사항:
 *   - pcv_drain_begin()은 멱등합니다 (중복 호출 시 두 번째부터 무시).
 *   - drain 스레드는 GMainLoop 외부에서 실행됩니다. GMainLoop 콜백 안에서
 *     g_main_loop_quit()을 직접 호출하면 콜백 반환 전에 루프가 종료되므로,
 *     별도 스레드에서 대기 후 quit()하는 구조입니다.
 *   - pcv_drain_shutdown()에서 g_thread_join()으로 drain 스레드 완료를 보장합니다.
 */

#include "drain.h"

#include <glib.h>
#include <glib-unix.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

/* ── 모듈 내부 상태 ───────────────────────────────────── */

typedef struct {
    volatile gint  inflight;     /* 처리 중인 요청 수 (g_atomic_int) */
    volatile gint  shutdown_flag;/* 종료 플래그 (g_atomic_int, 0/1)  */
    GMutex         mutex;
    GCond          cond;         /* inflight → 0 신호용              */
    GThread       *drain_thread; /* drain 대기 스레드                 */
    GMainLoop     *loop;         /* quit() 대상                       */
    guint          timeout_sec;
    gboolean       initialized;
} DrainState;

static DrainState g_drain = { 0 };

/* ── 공개 API ─────────────────────────────────────────── */

void
pcv_drain_init(void)
{
    g_mutex_init(&g_drain.mutex);
    g_cond_init(&g_drain.cond);
    g_atomic_int_set(&g_drain.inflight,      0);
    g_atomic_int_set(&g_drain.shutdown_flag, 0);
    g_drain.drain_thread = NULL;
    g_drain.loop         = NULL;
    g_drain.timeout_sec  = 30;
    g_drain.initialized  = TRUE;
    g_message("[drain] Initialized.");
}

/**
 * pcv_drain_inc — 요청 처리 시작 시 inflight 카운터를 증가.
 *
 * @return TRUE: 정상 처리 가능. FALSE: 종료 중이므로 요청 거부해야 함.
 *
 * [왜 뮤텍스인가 — 이전 TOCTOU 버그 설명]
 *   이전 코드:
 *     if (g_atomic_int_get(&shutdown_flag)) return FALSE;  ← (A)
 *     g_atomic_int_inc(&inflight);                          ← (B)
 *   (A)와 (B) 사이에 pcv_drain_begin()이 shutdown_flag=1을 세우면,
 *   drain 스레드가 inflight==0을 관찰하여 g_main_loop_quit()을 호출.
 *   그런데 (B)에서 inflight가 1이 되어, 이미 quit된 루프에서
 *   핸들러가 실행되는 use-after-quit 레이스가 발생했다.
 *   뮤텍스로 (A)+(B)를 원자화하여 이 틈을 차단.
 */
gboolean
pcv_drain_inc(void)
{
    /* shutdown_flag 확인과 inflight 증가를 뮤텍스로 원자화하여
     * TOCTOU 레이스 방지 (위 docstring 참조) */
    g_mutex_lock(&g_drain.mutex);

    if (g_atomic_int_get(&g_drain.shutdown_flag)) {
        g_mutex_unlock(&g_drain.mutex);
        return FALSE;
    }

    g_atomic_int_inc(&g_drain.inflight);
    g_mutex_unlock(&g_drain.mutex);
    return TRUE;
}

/**
 * pcv_drain_dec — 요청 처리 완료 시 inflight 카운터 감소.
 *
 * 핸들러가 응답을 전송한 후 uds_server.c에서 호출합니다.
 * inflight가 0이 되면 drain 스레드를 깨워 종료를 진행합니다.
 *
 * [왜 dec에는 뮤텍스가 없는가?]
 *   inc()와 달리, dec→broadcast 사이에 레이스가 없다.
 *   drain 스레드는 g_cond_wait_until()에서 대기하며 spurious wakeup도 안전하게 처리.
 *   g_atomic_int_dec_and_test()는 단일 원자 연산이므로 뮤텍스 불필요.
 */
void
pcv_drain_dec(void)
{
    /* dec 후 0이 되면 대기 중인 drain 스레드 깨우기 */
    if (g_atomic_int_dec_and_test(&g_drain.inflight)) {
        g_mutex_lock(&g_drain.mutex);
        g_cond_broadcast(&g_drain.cond);
        g_mutex_unlock(&g_drain.mutex);
    }
}

gboolean
pcv_drain_is_shutdown(void)
{
    return g_atomic_int_get(&g_drain.shutdown_flag) != 0;
}

gint
pcv_drain_get_inflight(void)
{
    return g_atomic_int_get(&g_drain.inflight);
}

/* ── sd_notify (libsystemd 없이 직접 구현) ───────────────
 *
 * systemd 는 NOTIFY_SOCKET 환경변수에 Unix datagram 소켓 경로를
 * (또는 "@..." abstract 소켓 이름을) 설정합니다.
 * "STOPPING=1\n" 을 한 패킷으로 전송하면 systemd 가 인식합니다.
 * ──────────────────────────────────────────────────────── */

void
pcv_drain_notify_stopping(void)
{
    const gchar *notify_socket = g_getenv("NOTIFY_SOCKET");
    if (!notify_socket || notify_socket[0] == '\0') {
        g_debug("[drain] NOTIFY_SOCKET not set — skipping sd_notify.");
        return;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        g_warning("[drain] sd_notify: socket() failed: %s", g_strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (notify_socket[0] == '@') {
        /* abstract namespace: '@' → '\0' */
        addr.sun_path[0] = '\0';
        strncpy(addr.sun_path + 1, notify_socket + 1,
                sizeof(addr.sun_path) - 2);
    } else {
        strncpy(addr.sun_path, notify_socket,
                sizeof(addr.sun_path) - 1);
    }

    socklen_t addrlen = offsetof(struct sockaddr_un, sun_path)
                        + (notify_socket[0] == '@'
                               ? strlen(notify_socket)   /* '\0' + rest */
                               : strlen(notify_socket) + 1);

    const char *msg = "STOPPING=1\n";
    if (sendto(fd, msg, strlen(msg), MSG_NOSIGNAL,
               (struct sockaddr *)&addr, addrlen) < 0) {
        g_warning("[drain] sd_notify STOPPING=1 failed: %s", g_strerror(errno));
    } else {
        g_message("[drain] sd_notify STOPPING=1 sent.");
    }

    close(fd);
}

/* ── sd_notify READY=1 (Type=notify 서비스 기동 완료 통지) ── */

void
pcv_drain_notify_ready(void)
{
    const gchar *notify_socket = g_getenv("NOTIFY_SOCKET");
    if (!notify_socket || notify_socket[0] == '\0') {
        g_debug("[drain] NOTIFY_SOCKET not set — skipping sd_notify READY.");
        return;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        g_warning("[drain] sd_notify READY: socket() failed: %s", g_strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (notify_socket[0] == '@') {
        addr.sun_path[0] = '\0';
        strncpy(addr.sun_path + 1, notify_socket + 1,
                sizeof(addr.sun_path) - 2);
    } else {
        strncpy(addr.sun_path, notify_socket,
                sizeof(addr.sun_path) - 1);
    }

    socklen_t addrlen = offsetof(struct sockaddr_un, sun_path)
                        + (notify_socket[0] == '@'
                               ? strlen(notify_socket)
                               : strlen(notify_socket) + 1);

    const char *msg = "READY=1\n";
    if (sendto(fd, msg, strlen(msg), MSG_NOSIGNAL,
               (struct sockaddr *)&addr, addrlen) < 0) {
        g_warning("[drain] sd_notify READY=1 failed: %s", g_strerror(errno));
    } else {
        g_message("[drain] sd_notify READY=1 sent.");
    }

    close(fd);
}

/* ── sd_notify WATCHDOG=1 (주기적 heartbeat) ─────────── */

void
pcv_drain_notify_watchdog(void)
{
    const gchar *notify_socket = g_getenv("NOTIFY_SOCKET");
    if (!notify_socket || notify_socket[0] == '\0')
        return;

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (notify_socket[0] == '@') {
        addr.sun_path[0] = '\0';
        strncpy(addr.sun_path + 1, notify_socket + 1,
                sizeof(addr.sun_path) - 2);
    } else {
        strncpy(addr.sun_path, notify_socket,
                sizeof(addr.sun_path) - 1);
    }

    socklen_t addrlen = offsetof(struct sockaddr_un, sun_path)
                        + (notify_socket[0] == '@'
                               ? strlen(notify_socket)
                               : strlen(notify_socket) + 1);

    const char *msg = "WATCHDOG=1\n";
    (void)sendto(fd, msg, strlen(msg), MSG_NOSIGNAL,
                 (struct sockaddr *)&addr, addrlen);
    close(fd);
}

guint64
pcv_drain_get_watchdog_usec(void)
{
    const gchar *val = g_getenv("WATCHDOG_USEC");
    if (!val || val[0] == '\0')
        return 0;
    guint64 usec = (guint64)g_ascii_strtoull(val, NULL, 10);
    return usec;
}

/* ── drain 스레드 ─────────────────────────────────────── */

typedef struct {
    GMainLoop *loop;
    guint      timeout_sec;
} DrainThreadData;

static gpointer
_drain_thread_func(gpointer user_data)
{
    DrainThreadData *td = (DrainThreadData *)user_data;
    gint inflight = pcv_drain_get_inflight();

    if (inflight > 0) {
        g_message("[drain] Waiting for %d in-flight request(s) to complete "
                  "(timeout: %us)...", inflight, td->timeout_sec);

        gint64 deadline = g_get_monotonic_time()
                          + (gint64)td->timeout_sec * G_TIME_SPAN_SECOND;

        g_mutex_lock(&g_drain.mutex);
        while (g_atomic_int_get(&g_drain.inflight) > 0) {
            if (!g_cond_wait_until(&g_drain.cond, &g_drain.mutex, deadline)) {
                /* 타임아웃 */
                g_warning("[drain] Timeout after %us — %d request(s) still "
                          "in-flight. Forcing shutdown.",
                          td->timeout_sec,
                          g_atomic_int_get(&g_drain.inflight));
                break;
            }
        }
        g_mutex_unlock(&g_drain.mutex);
    }

    g_message("[drain] All requests drained. Quitting main loop.");
    g_main_loop_quit(td->loop);
    g_free(td);
    return NULL;
}

/**
 * pcv_drain_begin — 그레이스풀 셧다운 시작. SIGTERM 핸들러에서 호출.
 *
 * @param loop        종료할 GMainLoop (drain 완료 후 g_main_loop_quit 호출)
 * @param timeout_sec 최대 대기 시간 (초). 초과 시 inflight 무시하고 강제 종료.
 *
 * [호출 컨텍스트]
 *   main.c의 g_unix_signal_add() 시그널 핸들러에서 호출됨.
 *   시그널 핸들러는 GMainLoop 이벤트로 전달되므로 메인 스레드에서 실행된다.
 *   (raw signal handler가 아니므로 async-signal-safe 제약 없음)
 *
 * [왜 별도 스레드인가?]
 *   이 함수는 메인 루프 콜백 내에서 실행됨 → 여기서 직접 quit()을 호출하면
 *   콜백이 반환되기 전에 루프가 종료되어 불안정. drain 스레드에서 inflight==0
 *   대기 후 quit()을 호출하면 메인 루프가 정상적으로 마지막 이터레이션을 완료.
 */
void
pcv_drain_begin(GMainLoop *loop, guint timeout_sec)
{
    /* 멱등 보호: CAS로 한 번만 실행. SIGTERM 중복 수신 시 안전. */
    if (g_atomic_int_compare_and_exchange(&g_drain.shutdown_flag, 0, 1) == FALSE) {
        g_debug("[drain] drain_begin called again — ignoring.");
        return;
    }

    g_drain.loop        = loop;
    g_drain.timeout_sec = timeout_sec;

    g_message("[drain] Shutdown initiated. inflight=%d",
              pcv_drain_get_inflight());

    /* 1. systemd 통지 */
    pcv_drain_notify_stopping();

    /* 2. drain 스레드 기동 (GMainLoop 는 이 콜백이 반환해야 계속 도는 구조이므로
     *    스레드에서 대기 후 quit() 호출) */
    DrainThreadData *td = g_new0(DrainThreadData, 1);
    td->loop        = loop;
    td->timeout_sec = timeout_sec;

    g_drain.drain_thread = g_thread_new("drain-waiter", _drain_thread_func, td);
}

void
pcv_drain_cancel(void)
{
    /* shutdown_flag를 0으로 리셋하여 새 요청 수락 재개 */
    g_atomic_int_set(&g_drain.shutdown_flag, 0);
    g_message("[drain] Drain cancelled — accepting requests again.");
}

/**
 * pcv_drain_shutdown — drain 서브시스템 자원 해제. 데몬 종료 시 호출.
 *
 * [왜 g_thread_join이 필요한가?]
 *   drain 스레드가 아직 실행 중일 때 mutex/cond를 clear하면 UB.
 *   join으로 스레드 종료를 보장한 후 자원을 해제한다.
 */
void
pcv_drain_shutdown(void)
{
    if (g_drain.drain_thread) {
        g_thread_join(g_drain.drain_thread);
        g_drain.drain_thread = NULL;
    }
    if (g_drain.initialized) {
        g_mutex_clear(&g_drain.mutex);
        g_cond_clear(&g_drain.cond);
        g_drain.initialized = FALSE;
    }
    g_message("[drain] Resources released.");
}
