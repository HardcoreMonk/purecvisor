
#include "drain.h"

#include <glib.h>
#include <glib-unix.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

typedef struct {
    volatile gint  inflight;
    volatile gint  shutdown_flag;
    GMutex         mutex;
    GCond          cond;
    GThread       *drain_thread;
    GMainLoop     *loop;
    guint          timeout_sec;
    gboolean       initialized;
} DrainState;

static DrainState g_drain = { 0 };

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

gboolean
pcv_drain_inc(void)
{

    g_mutex_lock(&g_drain.mutex);

    if (g_atomic_int_get(&g_drain.shutdown_flag)) {
        g_mutex_unlock(&g_drain.mutex);
        return FALSE;
    }

    g_atomic_int_inc(&g_drain.inflight);
    g_mutex_unlock(&g_drain.mutex);
    return TRUE;
}

void
pcv_drain_dec(void)
{

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

    const char *msg = "STOPPING=1\n";
    if (sendto(fd, msg, strlen(msg), MSG_NOSIGNAL,
               (struct sockaddr *)&addr, addrlen) < 0) {
        g_warning("[drain] sd_notify STOPPING=1 failed: %s", g_strerror(errno));
    } else {
        g_message("[drain] sd_notify STOPPING=1 sent.");
    }

    close(fd);
}

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

/* PCV_SAFETY_CONTROL: graceful-drain — SIGTERM 시 inflight 완료까지 대기 후 종료 (DISP-4) */
void
pcv_drain_begin(GMainLoop *loop, guint timeout_sec)
{

    if (g_atomic_int_compare_and_exchange(&g_drain.shutdown_flag, 0, 1) == FALSE) {
        g_debug("[drain] drain_begin called again — ignoring.");
        return;
    }

    g_drain.loop        = loop;
    g_drain.timeout_sec = timeout_sec;

    g_message("[drain] Shutdown initiated. inflight=%d",
              pcv_drain_get_inflight());

    pcv_drain_notify_stopping();

    DrainThreadData *td = g_new0(DrainThreadData, 1);
    td->loop        = loop;
    td->timeout_sec = timeout_sec;

    g_drain.drain_thread = g_thread_new("drain-waiter", _drain_thread_func, td);
}

void
pcv_drain_cancel(void)
{

    g_atomic_int_set(&g_drain.shutdown_flag, 0);
    g_message("[drain] Drain cancelled — accepting requests again.");
}

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
