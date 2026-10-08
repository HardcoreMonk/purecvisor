








#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#ifndef PCV_VNC_SOURCE
#define PCV_VNC_SOURCE "src/api/ws_server.c"
#endif
#include PCV_VNC_SOURCE

typedef struct {
    GMainContext *context;
    SoupServer *server;
    SoupSession *session;
    SoupWebsocketConnection *browser, *proxy;
    GByteArray *frames;
    guint browser_closed, proxy_closed;
    guint tcp_reads;
    int listener, peer, proxy_fd;
} Fixture;

static Fixture *active;
static const char *mode;


ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *buffer, size_t count)
{
    if (active && fd == active->proxy_fd) active->tcp_reads++;
    return __real_read(fd, buffer, count);
}


void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...)
{ (void)level; (void)domain; (void)fmt; }



void __real_soup_websocket_connection_close(SoupWebsocketConnection *, gushort, const char *);
void __wrap_soup_websocket_connection_close(SoupWebsocketConnection *conn,
                                            gushort code, const char *reason)
{
    if (active && conn == active->proxy && code == SOUP_WEBSOCKET_CLOSE_GOING_AWAY &&
        strcmp(mode, "late-close") != 0) {
        g_assert_cmpuint(g_signal_handler_find(conn, G_SIGNAL_MATCH_FUNC, 0, 0,
                         NULL, G_CALLBACK(_vnc_ws_closed), NULL), ==, 0);
        g_assert_cmpuint(g_signal_handler_find(conn, G_SIGNAL_MATCH_FUNC, 0, 0,
                         NULL, G_CALLBACK(_vnc_ws_message), NULL), ==, 0);
    }
    __real_soup_websocket_connection_close(conn, code, reason);
}


static void received(SoupWebsocketConnection *conn, SoupWebsocketDataType type,
                     GBytes *bytes, gpointer data)
{
    Fixture *f = data;
    (void)conn;
    g_assert_cmpint(type, ==, SOUP_WEBSOCKET_DATA_BINARY);
    gsize size;
    const guint8 *payload = g_bytes_get_data(bytes, &size);
    g_byte_array_append(f->frames, payload, size);
}

static void browser_closed(SoupWebsocketConnection *conn, gpointer data)
{ (void)conn; ((Fixture *)data)->browser_closed++; }
static void proxy_closed(SoupWebsocketConnection *conn, gpointer data)
{ (void)conn; ((Fixture *)data)->proxy_closed++; }


static void upgraded(SoupServer *server, SoupServerMessage *message, const char *path,
                     SoupWebsocketConnection *conn, gpointer data)
{
    Fixture *f = data;
    f->proxy = g_object_ref(conn);
    _on_vnc_connected(server, message, path, conn, NULL);
    g_signal_connect(conn, "closed", G_CALLBACK(proxy_closed), f);
}

static void connected(GObject *session, GAsyncResult *result, gpointer data)
{
    Fixture *f = data;
    GError *error = NULL;
    f->browser = soup_session_websocket_connect_finish(SOUP_SESSION(session), result, &error);
    g_assert_no_error(error);
    g_assert_nonnull(f->browser);
    g_signal_connect(f->browser, "message", G_CALLBACK(received), f);
    g_signal_connect(f->browser, "closed", G_CALLBACK(browser_closed), f);
}


static void step(Fixture *f)
{
    g_main_context_iteration(f->context, FALSE);
    g_usleep(1000);
}


static int find_proxy_fd(int port)
{
    DIR *directory = opendir("/proc/self/fd");
    g_assert_nonnull(directory);
    struct dirent *entry;
    int result = -1;
    while ((entry = readdir(directory))) {
        char *end;
        long number = strtol(entry->d_name, &end, 10);
        if (*end || number < 0) continue;
        struct sockaddr_in addr;
        socklen_t size = sizeof(addr);
        if (!getpeername((int)number, (struct sockaddr *)&addr, &size) &&
            addr.sin_family == AF_INET && ntohs(addr.sin_port) == port &&
            addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
            g_assert_cmpint(result, ==, -1);
            result = (int)number;
        }
    }
    closedir(directory);
    g_assert_cmpint(result, >=, 0);
    return result;
}


static void setup(Fixture *f, gboolean private_context)
{
    memset(f, 0, sizeof(*f));
    f->peer = f->proxy_fd = -1;
    f->context = private_context ? g_main_context_new() : g_main_context_ref(g_main_context_default());
    g_main_context_push_thread_default(f->context);
    active = f;
    f->frames = g_byte_array_new();
    f->listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    g_assert_cmpint(f->listener, >=, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    int port;
    for (port = 5900; port <= 6100; port++) {
        addr.sin_port = htons(port);
        if (!bind(f->listener, (struct sockaddr *)&addr, sizeof(addr))) break;
    }
    g_assert_cmpint(port, <=, 6100);
    g_assert_cmpint(listen(f->listener, 1), ==, 0);
    f->server = soup_server_new(NULL, NULL);
    soup_server_add_websocket_handler(f->server, "/vnc", NULL, NULL, upgraded, f, NULL);
    GError *error = NULL;
    g_assert_true(soup_server_listen_local(f->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
    g_assert_no_error(error);
    GSList *uris = soup_server_get_uris(f->server);
    gchar *url = g_strdup_printf("ws://127.0.0.1:%d/vnc?port=%d",
                               g_uri_get_port(uris->data), port);
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
    f->session = soup_session_new();

    g_object_set(f->session, "proxy-resolver", NULL, NULL);
    SoupMessage *message = soup_message_new("GET", url);
    soup_session_websocket_connect_async(f->session, message, NULL, NULL,
                                        G_PRIORITY_DEFAULT, NULL, connected, f);
    g_object_unref(message);
    g_free(url);
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while ((!f->proxy || !f->browser) && g_get_monotonic_time() < deadline) step(f);
    g_assert_nonnull(f->proxy);
    g_assert_nonnull(f->browser);
    f->peer = accept4(f->listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    g_assert_cmpint(f->peer, >=, 0);
    f->proxy_fd = find_proxy_fd(port);
    g_assert_cmpuint(g_signal_handler_find(f->proxy, G_SIGNAL_MATCH_FUNC, 0, 0,
                     NULL, G_CALLBACK(_vnc_ws_closed), NULL), !=, 0);
}


static void forward(Fixture *f)
{
    const guint8 screen[] = {0, 17, 0xff, 7, 8, 0, 9};
    const guint8 keys[] = {9, 0, 3, 0xfa, 4};
    g_assert_cmpint(send(f->peer, screen, sizeof(screen), MSG_NOSIGNAL), ==, sizeof(screen));
    gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (f->frames->len < sizeof(screen) && g_get_monotonic_time() < deadline) step(f);
    g_assert_cmpmem(f->frames->data, f->frames->len, screen, sizeof(screen));
    soup_websocket_connection_send_binary(f->browser, keys, sizeof(keys));
    guint8 buffer[sizeof(keys)];
    gsize received_size = 0;
    deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (received_size < sizeof(keys) && g_get_monotonic_time() < deadline) {
        step(f);
        ssize_t size = recv(f->peer, buffer + received_size, sizeof(keys) - received_size, MSG_DONTWAIT);
        if (size > 0) received_size += size;
        else if (size < 0) g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
    }
    g_assert_cmpmem(buffer, received_size, keys, sizeof(keys));
    printf("FORWARD: TCP->WS %zu bytes, WS->TCP %zu bytes\n", sizeof(screen), sizeof(keys));
    fflush(stdout);
}


static void terminated(Fixture *f)
{
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while ((!f->browser_closed || !f->proxy_closed) && g_get_monotonic_time() < deadline) step(f);
    g_assert_cmpuint(f->browser_closed, ==, 1);
    g_assert_cmpuint(f->proxy_closed, ==, 1);
    errno = 0;
    g_assert_cmpint(fcntl(f->proxy_fd, F_GETFD), ==, -1);
    g_assert_cmpint(errno, ==, EBADF);
    g_assert_cmpuint(g_signal_handler_find(f->proxy, G_SIGNAL_MATCH_FUNC, 0, 0,
                     NULL, G_CALLBACK(_vnc_ws_closed), NULL), ==, 0);
    g_assert_cmpuint(g_signal_handler_find(f->proxy, G_SIGNAL_MATCH_FUNC, 0, 0,
                     NULL, G_CALLBACK(_vnc_ws_message), NULL), ==, 0);
    if (f->peer >= 0) {
        guint8 byte;
        g_assert_cmpint(recv(f->peer, &byte, 1, MSG_DONTWAIT), ==, 0);
    }

    GBytes *late = g_bytes_new_static("late", 4);
    g_signal_emit_by_name(f->proxy, "message", SOUP_WEBSOCKET_DATA_BINARY, late);
    g_bytes_unref(late);
    for (guint i = 0; i < 5; i++) step(f);
}


static void teardown(Fixture *f)
{
    soup_session_abort(f->session);
    soup_server_disconnect(f->server);
    g_clear_object(&f->browser);
    g_clear_object(&f->proxy);
    g_clear_object(&f->session);
    g_clear_object(&f->server);
    g_byte_array_unref(f->frames);
    if (f->peer >= 0) close(f->peer);
    close(f->listener);
    for (guint i = 0; i < 5; i++) step(f);
    g_main_context_pop_thread_default(f->context);
    g_main_context_unref(f->context);
    active = NULL;
}


int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 2);
    mode = argv[1];
    guint runs = !strcmp(mode, "repeat") ? 10 : 1;
    for (guint i = 0; i < runs; i++) {
        Fixture f;
        gboolean private_context = !g_str_has_prefix(mode, "default") &&
                                   strcmp(mode, "tcp-eof") && strcmp(mode, "late-close");
        setup(&f, private_context);
        if (strstr(mode, "forward") || !strcmp(mode, "repeat")) forward(&f);
        gboolean tcp_first = !strcmp(mode, "tcp-eof") || !strcmp(mode, "late-close") ||
                             !strcmp(mode, "tcp-hup") || !strcmp(mode, "simultaneous") ||
                             (!strcmp(mode, "repeat") && i % 2);
        if (!strcmp(mode, "closing-then-tcp")) {
            soup_websocket_connection_close(f.proxy, SOUP_WEBSOCKET_CLOSE_NORMAL, "fixture");
            tcp_first = TRUE;
        }
        if (!strcmp(mode, "tcp-hup"))
            g_assert_cmpint(shutdown(f.proxy_fd, SHUT_RDWR), ==, 0);
        else if (tcp_first) { close(f.peer); f.peer = -1; }
        if (!tcp_first || !strcmp(mode, "simultaneous"))
            soup_websocket_connection_close(f.browser, SOUP_WEBSOCKET_CLOSE_NORMAL, "fixture");
        terminated(&f);
        if (!strcmp(mode, "tcp-hup")) g_assert_cmpuint(f.tcp_reads, ==, 0);
        teardown(&f);
    }
    printf("PASS %s: peer bytes/EOF, FD closed, signals detached (%u sessions)\n", mode, runs);
    return 0;
}
