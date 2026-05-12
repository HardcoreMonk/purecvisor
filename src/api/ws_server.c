


































#include "ws_server.h"
#include "utils/pcv_log.h"
#include "utils/pcv_jwt.h"
#include "utils/pcv_config.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>
#include <gio/gio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <sys/statvfs.h>

#define WS_LOG_DOM "ws_server"


static gboolean _ws_auth_callback(SoupServerMessage *msg, gpointer user_data);
#define WS_PATH    "/api/v1/ws/events"
#define VNC_PATH           "/api/v1/ws/vnc"

#define WS_DEF_MAX_CONNECTIONS  1000
#define WS_DEF_MAX_PER_IP       100
#define WS_DEF_IDLE_TIMEOUT     300
#define WS_DEF_IDLE_CHECK       60
#define WS_DEF_PING_INTERVAL    30
#define WS_DEF_PONG_TIMEOUT     60
#define WS_DEF_MAX_PENDING      500


static gint ws_max_connections   = WS_DEF_MAX_CONNECTIONS;
static gint ws_max_per_ip        = WS_DEF_MAX_PER_IP;
static gint ws_idle_timeout_sec  = WS_DEF_IDLE_TIMEOUT;
static gint ws_idle_check_interval = WS_DEF_IDLE_CHECK;
static gint ws_ping_interval_sec = WS_DEF_PING_INTERVAL;
static gint ws_pong_timeout_sec  = WS_DEF_PONG_TIMEOUT;
static gint ws_max_pending_msgs  = WS_DEF_MAX_PENDING;










static struct {
    GPtrArray  *clients;
    GHashTable *ip_counts;
    GMutex      mu;
    gboolean    initialized;
} G = {0};










static void
_on_ws_closed(SoupWebsocketConnection *conn, gpointer data __attribute__((unused)))
{
    g_mutex_lock(&G.mu);


    gchar *disc_ip = g_strdup(g_object_get_data(G_OBJECT(conn), "client-ip"));
    g_ptr_array_remove(G.clients, conn);

    if (disc_ip && G.ip_counts) {
        gint cnt = GPOINTER_TO_INT(g_hash_table_lookup(G.ip_counts, disc_ip));
        if (cnt <= 1)
            g_hash_table_remove(G.ip_counts, disc_ip);
        else
            g_hash_table_insert(G.ip_counts, g_strdup(disc_ip),
                                GINT_TO_POINTER(cnt - 1));
    }
    g_mutex_unlock(&G.mu);
    PCV_LOG_INFO(WS_LOG_DOM, "WebSocket client disconnected (total=%d)",
                 G.clients->len);
    g_free(disc_ip);
}







static void
_on_ws_message(SoupWebsocketConnection *conn,
               SoupWebsocketDataType type __attribute__((unused)),
               GBytes *message,
               gpointer data __attribute__((unused)))
{

    gint now = (gint)time(NULL);
    g_object_set_data(G_OBJECT(conn), "last_active",
                      GINT_TO_POINTER(now));



    gint *pending = g_object_get_data(G_OBJECT(conn), "pcv-pending");
    if (pending) g_atomic_int_set(pending, 0);

    gsize sz = 0;
    const gchar *text = g_bytes_get_data(message, &sz);



    gint authed = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-authed"));
    if (!authed && sz > 0 && text) {
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, text, (gssize)sz, NULL)) {
            JsonObject *obj = json_node_get_object(json_parser_get_root(p));
            const gchar *msg_type = json_object_get_string_member_with_default(obj, "type", "");
            if (g_strcmp0(msg_type, "auth") == 0) {
                const gchar *token = json_object_get_string_member_with_default(obj, "token", "");
                GError *jwt_err = NULL;
                gchar *subject = pcv_jwt_verify(token, &jwt_err);
                if (subject) {
                    g_object_set_data(G_OBJECT(conn), "pcv-ws-authed", GINT_TO_POINTER(1));
                    PCV_LOG_INFO(WS_LOG_DOM, "WebSocket protocol-auth OK: user=%s", subject);
                    soup_websocket_connection_send_text(conn, "{\"type\":\"auth_ok\"}");
                    g_free(subject);
                } else {
                    PCV_LOG_WARN(WS_LOG_DOM, "WebSocket protocol-auth failed: %s",
                                 jwt_err ? jwt_err->message : "invalid");
                    soup_websocket_connection_send_text(conn, "{\"type\":\"auth_fail\"}");
                    soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION,
                                                    "authentication failed");
                    if (jwt_err) g_error_free(jwt_err);
                }
            }
        }
        g_object_unref(p);
        if (!authed) {
            return;
        }
    }


    if (sz > 0 && text && sz < 256 && g_strstr_len(text, (gssize)sz, "pong")) {
        g_object_set_data(G_OBJECT(conn), "last_pong",
                          GINT_TO_POINTER(now));
    }
}

















static void
_on_ws_connected(SoupServer *server __attribute__((unused)),
                 SoupServerMessage *msg,
                 const char *path __attribute__((unused)),
                 SoupWebsocketConnection *conn,
                 gpointer data __attribute__((unused)))
{



    gboolean ws_authed = _ws_auth_callback(msg, NULL);
    g_object_set_data(G_OBJECT(conn), "pcv-ws-authed", GINT_TO_POINTER(ws_authed ? 1 : 0));
    if (!ws_authed) {

        g_object_set_data(G_OBJECT(conn), "pcv-ws-auth-deadline",
                          GINT_TO_POINTER((gint)time(NULL) + 5));
    }


    GSocketAddress *ra = soup_server_message_get_remote_address(msg);
    gchar *client_ip = (ra && G_IS_INET_SOCKET_ADDRESS(ra))
        ? g_inet_address_to_string(
              g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)))
        : g_strdup("unknown");

    g_mutex_lock(&G.mu);
    if ((gint)G.clients->len >= ws_max_connections) {
        g_mutex_unlock(&G.mu);
        PCV_LOG_WARN(WS_LOG_DOM,
            "WebSocket connection rejected — global limit reached (%d)", ws_max_connections);
        g_free(client_ip);
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                        "too many connections");
        return;
    }

    gint ip_count = GPOINTER_TO_INT(g_hash_table_lookup(G.ip_counts, client_ip));
    if (ip_count >= ws_max_per_ip) {
        g_mutex_unlock(&G.mu);
        PCV_LOG_WARN(WS_LOG_DOM,
            "WebSocket connection rejected — per-IP limit (%d) for %s",
            ws_max_per_ip, client_ip);
        g_free(client_ip);
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                        "too many connections from this IP");
        return;
    }
    g_hash_table_insert(G.ip_counts, g_strdup(client_ip),
                        GINT_TO_POINTER(ip_count + 1));

    g_object_set_data_full(G_OBJECT(conn), "client-ip",
                           g_strdup(client_ip), g_free);
    g_free(client_ip);




    gint connect_time = (gint)time(NULL);
    g_object_set_data(G_OBJECT(conn), "last_active",
                      GINT_TO_POINTER(connect_time));
    g_object_set_data(G_OBJECT(conn), "last_pong",
                      GINT_TO_POINTER(connect_time));
    g_ptr_array_add(G.clients, g_object_ref(conn));
    g_mutex_unlock(&G.mu);


    gint *pending = g_new0(gint, 1);
    g_object_set_data_full(G_OBJECT(conn), "pcv-pending", pending, g_free);

    g_signal_connect(conn, "closed", G_CALLBACK(_on_ws_closed), NULL);
    g_signal_connect(conn, "message", G_CALLBACK(_on_ws_message), NULL);

    PCV_LOG_INFO(WS_LOG_DOM, "WebSocket client connected (total=%d)",
                 G.clients->len);
}
























typedef struct {
    SoupWebsocketConnection *ws;
    int                      tcp_fd;
    GIOChannel              *tcp_chan;
    guint                    tcp_watch_id;
    gboolean                 closing;
} VncProxy;














static void
_vnc_proxy_free(VncProxy *vp)
{
    if (!vp) return;
    vp->closing = TRUE;
    if (vp->tcp_watch_id) { g_source_remove(vp->tcp_watch_id); vp->tcp_watch_id = 0; }
    if (vp->tcp_chan) { g_io_channel_unref(vp->tcp_chan); vp->tcp_chan = NULL; }
    if (vp->tcp_fd >= 0) { close(vp->tcp_fd); vp->tcp_fd = -1; }
    if (vp->ws) { g_object_unref(vp->ws); vp->ws = NULL; }
    g_free(vp);
}


















static gboolean
_vnc_tcp_readable(GIOChannel *chan, GIOCondition cond, gpointer data)
{
    VncProxy *vp = (VncProxy *)data;
    if (vp->closing) return G_SOURCE_REMOVE;

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        PCV_LOG_INFO(WS_LOG_DOM, "VNC TCP connection closed");
        if (vp->ws && soup_websocket_connection_get_state(vp->ws) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(vp->ws, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, "VNC disconnected");
        vp->tcp_watch_id = 0;
        _vnc_proxy_free(vp);
        return G_SOURCE_REMOVE;
    }

    guint8 buf[65536];
    ssize_t n = read(vp->tcp_fd, buf, sizeof(buf));
    if (n <= 0) {
        if (n == 0) PCV_LOG_INFO(WS_LOG_DOM, "VNC TCP EOF");
        if (vp->ws && soup_websocket_connection_get_state(vp->ws) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(vp->ws, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, "VNC closed");
        vp->tcp_watch_id = 0;
        _vnc_proxy_free(vp);
        return G_SOURCE_REMOVE;
    }

    if (vp->ws && soup_websocket_connection_get_state(vp->ws) == SOUP_WEBSOCKET_STATE_OPEN) {
        soup_websocket_connection_send_binary(vp->ws, buf, (gsize)n);
    }
    return G_SOURCE_CONTINUE;
}

















static void
_vnc_ws_message(SoupWebsocketConnection *conn __attribute__((unused)),
                SoupWebsocketDataType    type,
                GBytes                  *message,
                gpointer                 data)
{
    VncProxy *vp = (VncProxy *)data;
    if (vp->closing || vp->tcp_fd < 0) return;
    if (type != SOUP_WEBSOCKET_DATA_BINARY) return;

    gsize len = 0;
    const guint8 *buf = g_bytes_get_data(message, &len);
    if (len == 0) return;

    gsize written = 0;
    while (written < len) {
        ssize_t w = write(vp->tcp_fd, buf + written, len - written);
        if (w <= 0) {
            PCV_LOG_WARN(WS_LOG_DOM, "VNC TCP write failed: %s", strerror(errno));
            break;
        }
        written += (gsize)w;
    }
}








static void
_vnc_ws_closed(SoupWebsocketConnection *conn __attribute__((unused)), gpointer data)
{
    VncProxy *vp = (VncProxy *)data;
    PCV_LOG_INFO(WS_LOG_DOM, "VNC WebSocket closed");
    _vnc_proxy_free(vp);
}























static void
_on_vnc_connected(SoupServer              *server __attribute__((unused)),
                  SoupServerMessage        *msg,
                  const char              *path __attribute__((unused)),
                  SoupWebsocketConnection *conn,
                  gpointer                 data __attribute__((unused)))
{

    GUri *uri = soup_server_message_get_uri(msg);
    const gchar *query = g_uri_get_query(uri);
    int vnc_port = 5900;
    if (query) {
        GHashTable *params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
        if (params) {
            const gchar *p = g_hash_table_lookup(params, "port");
            if (p) vnc_port = atoi(p);
            g_hash_table_unref(params);
        }
    }




    if (vnc_port < 5900 || vnc_port > 6100) {
        PCV_LOG_WARN(WS_LOG_DOM,
            "VNC proxy: rejected port %d — outside allowed range 5900-6100", vnc_port);
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION,
                                        "VNC port out of range");
        return;
    }

    PCV_LOG_INFO(WS_LOG_DOM, "VNC proxy: connecting to localhost:%d", vnc_port);


    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        PCV_LOG_WARN(WS_LOG_DOM, "VNC proxy: socket() failed: %s", strerror(errno));
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, "socket failed");
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)vnc_port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        PCV_LOG_WARN(WS_LOG_DOM, "VNC proxy: connect(:%d) failed: %s", vnc_port, strerror(errno));
        close(fd);
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, "VNC connect failed");
        return;
    }

    PCV_LOG_INFO(WS_LOG_DOM, "VNC proxy: connected to localhost:%d", vnc_port);

    VncProxy *vp = g_new0(VncProxy, 1);
    vp->ws     = g_object_ref(conn);
    vp->tcp_fd = fd;










    vp->tcp_chan = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(vp->tcp_chan, NULL, NULL);
    g_io_channel_set_buffered(vp->tcp_chan, FALSE);
    vp->tcp_watch_id = g_io_add_watch(vp->tcp_chan,
                                       G_IO_IN | G_IO_HUP | G_IO_ERR,
                                       _vnc_tcp_readable, vp);


    g_signal_connect(conn, "message", G_CALLBACK(_vnc_ws_message), vp);
    g_signal_connect(conn, "closed",  G_CALLBACK(_vnc_ws_closed), vp);
}




















static gboolean
_ws_auth_callback(SoupServerMessage *msg,
                  gpointer           user_data __attribute__((unused)))
{
    GUri *uri = soup_server_message_get_uri(msg);
    const gchar *query = g_uri_get_query(uri);


    gchar *token = NULL;
    if (query) {
        GHashTable *params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
        if (params) {
            const gchar *t = g_hash_table_lookup(params, "token");
            if (t) token = g_strdup(t);
            g_hash_table_unref(params);
        }
    }

    if (!token || token[0] == '\0') {


        g_free(token);
        return FALSE;
    }


    GError *jwt_err = NULL;
    gchar *subject = pcv_jwt_verify(token, &jwt_err);
    if (!subject) {
        PCV_LOG_WARN(WS_LOG_DOM, "WebSocket rejected: %s",
                     jwt_err ? jwt_err->message : "invalid JWT");
        soup_server_message_set_status(msg, 401, "Unauthorized");
        if (jwt_err) g_error_free(jwt_err);
        g_free(token);
        return FALSE;
    }

    PCV_LOG_INFO(WS_LOG_DOM, "WebSocket authenticated: user=%s", subject);
    g_free(subject);
    g_free(token);
    return TRUE;
}








static gboolean
_ws_heartbeat_cb(gpointer data __attribute__((unused)))
{
    if (!G.initialized) return G_SOURCE_REMOVE;

    time_t now = time(NULL);
    GPtrArray *stale = g_ptr_array_new();
    gchar *ping_msg = g_strdup_printf("{\"type\":\"ping\",\"ts\":%ld}", (long)now);

    g_mutex_lock(&G.mu);
    for (guint i = 0; i < G.clients->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);
        if (soup_websocket_connection_get_state(conn) != SOUP_WEBSOCKET_STATE_OPEN)
            continue;


        gint last_pong = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(conn), "last_pong"));
        if (last_pong > 0 && (now - (time_t)last_pong) > ws_pong_timeout_sec) {
            g_ptr_array_add(stale, g_object_ref(conn));
            continue;
        }


        soup_websocket_connection_send_text(conn, ping_msg);
    }
    g_mutex_unlock(&G.mu);


    for (guint i = 0; i < stale->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(stale, i);
        PCV_LOG_INFO(WS_LOG_DOM, "Closing WebSocket — no pong within %ds",
                     ws_pong_timeout_sec);
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                        "pong timeout");
        g_object_unref(conn);
    }
    g_ptr_array_free(stale, TRUE);
    g_free(ping_msg);

    return G_SOURCE_CONTINUE;
}












static gboolean
_ws_auth_deadline_check(gpointer data __attribute__((unused)))
{
    if (!G.initialized) return G_SOURCE_REMOVE;
    time_t now = time(NULL);
    GPtrArray *to_close = g_ptr_array_new();
    g_mutex_lock(&G.mu);
    for (guint i = 0; i < G.clients->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);
        gint authed = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-authed"));
        if (authed) continue;
        gint deadline = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-auth-deadline"));
        if (deadline > 0 && (gint)now > deadline) {
            g_ptr_array_add(to_close, g_object_ref(conn));
        }
    }
    g_mutex_unlock(&G.mu);
    for (guint i = 0; i < to_close->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(to_close, i);
        if (soup_websocket_connection_get_state(conn) == SOUP_WEBSOCKET_STATE_OPEN) {
            PCV_LOG_WARN(WS_LOG_DOM,
                "Closing unauthenticated WebSocket connection (>5s, ADR-0010)");
            soup_websocket_connection_close(conn,
                SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "auth timeout");
        }
        g_object_unref(conn);
    }
    g_ptr_array_free(to_close, TRUE);
    return G_SOURCE_CONTINUE;
}

static gboolean
_ws_idle_cleanup(gpointer data __attribute__((unused)))
{
    if (!G.initialized) return G_SOURCE_REMOVE;

    time_t now = time(NULL);
    GPtrArray *to_close = g_ptr_array_new();

    g_mutex_lock(&G.mu);
    for (guint i = 0; i < G.clients->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);

        gint authed = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-authed"));
        if (!authed) {
            gint deadline = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-auth-deadline"));
            if (deadline > 0 && (gint)now > deadline) {
                g_ptr_array_add(to_close, g_object_ref(conn));
                continue;
            }
        }
        gint last = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "last_active"));
        if (last > 0 && (now - (time_t)last) > ws_idle_timeout_sec) {
            g_ptr_array_add(to_close, g_object_ref(conn));
        }
    }
    g_mutex_unlock(&G.mu);


    for (guint i = 0; i < to_close->len; i++) {
        SoupWebsocketConnection *conn = g_ptr_array_index(to_close, i);
        if (soup_websocket_connection_get_state(conn) == SOUP_WEBSOCKET_STATE_OPEN) {
            PCV_LOG_INFO(WS_LOG_DOM, "Closing idle WebSocket connection (>%ds)",
                         ws_idle_timeout_sec);
            soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                            "idle timeout");
        }
        g_object_unref(conn);
    }
    g_ptr_array_free(to_close, TRUE);

    return G_SOURCE_CONTINUE;
}









extern JsonObject *pcv_ebpf_telemetry_get_host(void);

static gboolean
_ws_push_metrics_cb(gpointer user_data __attribute__((unused)))
{
    if (!G.initialized) return G_SOURCE_CONTINUE;


    g_mutex_lock(&G.mu);
    gboolean has_clients = (G.clients && G.clients->len > 0);
    g_mutex_unlock(&G.mu);
    if (!has_clients) return G_SOURCE_CONTINUE;


    JsonObject *payload = json_object_new();

    JsonObject *host = pcv_ebpf_telemetry_get_host();
    if (host) {
        if (json_object_has_member(host, "cpu_percent"))
            json_object_set_double_member(payload, "cpu_percent",
                json_object_get_double_member(host, "cpu_percent"));
        if (json_object_has_member(host, "mem_percent"))
            json_object_set_double_member(payload, "mem_percent",
                json_object_get_double_member(host, "mem_percent"));
        json_object_unref(host);
    }


    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        guint64 total  = (guint64)vfs.f_blocks * vfs.f_frsize;
        guint64 free_b = (guint64)vfs.f_bfree  * vfs.f_frsize;
        if (total > 0)
            json_object_set_double_member(payload, "disk_percent",
                100.0 * (1.0 - (gdouble)free_b / (gdouble)total));
    }


    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, payload);
    gchar *json_str = json_to_string(node, FALSE);

    pcv_ws_broadcast("metrics-update", json_str);

    g_free(json_str);
    json_node_unref(node);
    json_object_unref(payload);

    return G_SOURCE_CONTINUE;
}

void
pcv_ws_server_init(SoupServer *soup)
{

    ws_max_connections    = pcv_config_get_int("ws", "max_connections",    WS_DEF_MAX_CONNECTIONS);
    ws_max_per_ip         = pcv_config_get_int("ws", "max_per_ip",        WS_DEF_MAX_PER_IP);
    ws_idle_timeout_sec   = pcv_config_get_int("ws", "idle_timeout_sec",  WS_DEF_IDLE_TIMEOUT);
    ws_idle_check_interval = pcv_config_get_int("ws", "idle_check_sec",   WS_DEF_IDLE_CHECK);
    ws_ping_interval_sec  = pcv_config_get_int("ws", "ping_interval_sec", WS_DEF_PING_INTERVAL);
    ws_pong_timeout_sec   = pcv_config_get_int("ws", "pong_timeout_sec",  WS_DEF_PONG_TIMEOUT);
    ws_max_pending_msgs   = pcv_config_get_int("ws", "max_pending_msgs",  WS_DEF_MAX_PENDING);

    g_mutex_init(&G.mu);
    G.clients = g_ptr_array_new_with_free_func(g_object_unref);
    G.ip_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    G.initialized = TRUE;

    soup_server_add_websocket_handler(soup, WS_PATH,
                                       NULL, NULL,
                                       _on_ws_connected, NULL, NULL);


    soup_server_add_websocket_handler(soup, VNC_PATH,
                                       NULL, NULL,
                                       _on_vnc_connected, NULL, NULL);






    g_timeout_add_seconds(ws_idle_check_interval, _ws_idle_cleanup, NULL);


    g_timeout_add_seconds(1, _ws_auth_deadline_check, NULL);


    g_timeout_add_seconds(ws_ping_interval_sec, _ws_heartbeat_cb, NULL);


    g_timeout_add_seconds(10, _ws_push_metrics_cb, NULL);

    PCV_LOG_INFO(WS_LOG_DOM, "WebSocket handler registered at %s, %s (idle timeout=%ds, ping=%ds, metrics push=10s)",
                 WS_PATH, VNC_PATH, ws_idle_timeout_sec, ws_ping_interval_sec);
}








void
pcv_ws_server_shutdown(void)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    g_ptr_array_set_size(G.clients, 0);
    g_mutex_unlock(&G.mu);
    g_ptr_array_unref(G.clients);
    g_mutex_clear(&G.mu);
    G.initialized = FALSE;
}



















void
pcv_ws_broadcast(const gchar *type, const gchar *payload_json)
{
    if (!G.initialized || !type) return;


    gint64 ts = (gint64)time(NULL);
    gchar *msg = g_strdup_printf("{\"type\":\"%s\",\"ts\":%ld,\"payload\":%s}",
                                  type, (long)ts,
                                  payload_json ? payload_json : "{}");


    g_mutex_lock(&G.mu);
    for (guint i = 0; i < G.clients->len; ) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);
        if (soup_websocket_connection_get_state(conn) != SOUP_WEBSOCKET_STATE_OPEN) {
            i++;
            continue;
        }


        gint *pending = g_object_get_data(G_OBJECT(conn), "pcv-pending");
        if (pending && g_atomic_int_get(pending) >= ws_max_pending_msgs) {
            const gchar *bp_ip = g_object_get_data(G_OBJECT(conn), "client-ip");
            PCV_LOG_WARN(WS_LOG_DOM,
                "WebSocket backpressure exceeded (%d msgs) for %s, closing",
                g_atomic_int_get(pending), bp_ip ? bp_ip : "unknown");
            soup_websocket_connection_close(conn,
                SOUP_WEBSOCKET_CLOSE_GOING_AWAY, "backpressure");


            g_ptr_array_remove_index(G.clients, i);
            continue;
        }
        soup_websocket_connection_send_text(conn, msg);
        if (pending) g_atomic_int_add(pending, 1);

        g_object_set_data(G_OBJECT(conn), "last_active",
                          GINT_TO_POINTER((gint)time(NULL)));
        i++;
    }
    g_mutex_unlock(&G.mu);

    g_free(msg);
}









gint
pcv_ws_client_count(void)
{
    if (!G.initialized) return 0;
    g_mutex_lock(&G.mu);
    gint n = (gint)G.clients->len;
    g_mutex_unlock(&G.mu);
    return n;
}




void
pcv_ws_broadcast_job_complete(const gchar *job_id, const gchar *method,
                               const gchar *status, const gchar *error_msg)
{
    if (!job_id || !method || !status) return;

    gchar *payload;
    if (error_msg && *error_msg) {

        GString *escaped = g_string_new(NULL);
        for (const gchar *p = error_msg; *p; p++) {
            if (*p == '"' || *p == '\\')
                g_string_append_c(escaped, '\\');
            g_string_append_c(escaped, *p);
        }
        payload = g_strdup_printf(
            "{\"job_id\":\"%s\",\"method\":\"%s\",\"status\":\"%s\","
            "\"error\":\"%s\"}",
            job_id, method, status, escaped->str);
        g_string_free(escaped, TRUE);
    } else {
        payload = g_strdup_printf(
            "{\"job_id\":\"%s\",\"method\":\"%s\",\"status\":\"%s\"}",
            job_id, method, status);
    }

    pcv_ws_broadcast("job.complete", payload);
    g_free(payload);

    PCV_LOG_INFO(WS_LOG_DOM, "Broadcast job.complete: job_id=%s method=%s status=%s",
                 job_id, method, status);
}
