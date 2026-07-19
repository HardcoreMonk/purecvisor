
#include "grpc_server.h"
#include "utils/pcv_config.h"
#include "utils/pcv_crypto.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_tls.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/dispatcher/rpc_utils.h"
#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define GRPC_LOG_DOM   "grpc_server"
#define GRPC_MAX_MSG   (4 * 1024 * 1024)

static struct {
    GThread  *thread;
    gboolean  running;
    gint      port;
    gint      listen_fd;
} G = { .running = FALSE, .port = 50051, .listen_fd = -1 };

static gchar *
_rpc_call(const gchar *method, const gchar *params_json)
{
    const gchar *sock_path = pcv_config_get_socket_path();

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return NULL;

    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }

    gchar *rpc_msg = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":\"grpc-1\"}",
        method, params_json ? params_json : "{}");

    ssize_t sent = write(fd, rpc_msg, strlen(rpc_msg));
    g_free(rpc_msg);
    if (sent <= 0) { close(fd); return NULL; }

    gchar *buf = g_malloc(GRPC_MAX_MSG);
    ssize_t n = read(fd, buf, GRPC_MAX_MSG - 1);
    close(fd);

    if (n <= 0) { g_free(buf); return NULL; }
    buf[n] = '\0';
    gchar *result = g_strdup(buf);
    g_free(buf);
    return result;
}

static gchar *
_extract_result(const gchar *json_resp)
{
    if (!json_resp) return g_strdup("{}");

    JsonParser *p = json_parser_new();
    /* PCV_PARSE_TRUSTED: 데몬 JSON-RPC 응답 파싱(UDS 경유 내부 신뢰 출력, 외부 입력 아님) */
    if (!json_parser_load_from_data(p, json_resp, -1, NULL)) {
        g_object_unref(p);
        return g_strdup("{}");
    }

    JsonNode *root = json_parser_get_root(p);
    JsonObject *obj = json_node_get_object(root);

    if (json_object_has_member(obj, "result")) {
        JsonNode *result = json_object_get_member(obj, "result");
        gchar *str = json_to_string(result, FALSE);
        g_object_unref(p);
        return str;
    }

    g_object_unref(p);
    return g_strdup("{}");
}

static gchar *
_grpc_inject_caller_identity(const gchar *params_json, gint role)
{
    JsonParser *parser = NULL;
    JsonObject *params = NULL;

    if (params_json && *params_json &&
        pcv_rpc_parse_guarded(params_json, -1, &parser, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root))
            params = json_node_get_object(root);
    }

    gchar *out = NULL;
    if (params) {

        json_object_remove_member(params, "_pcv_caller_role");
        json_object_remove_member(params, "_pcv_caller_sub");
        json_object_set_int_member(params, "_pcv_caller_role", role);
        json_object_set_string_member(params, "_pcv_caller_sub", "grpc");
        out = json_to_string(json_parser_get_root(parser), FALSE);
    }
    if (parser) g_object_unref(parser);

    if (!out) {

        out = g_strdup_printf(
            "{\"_pcv_caller_role\":%d,\"_pcv_caller_sub\":\"grpc\"}", role);
    }
    return out;
}

static void
_handle_client(int client_fd)
{

    extern gchar *G_grpc_auth_token;
    if (G_grpc_auth_token && *G_grpc_auth_token) {
        guint32 tk_len = 0;
        if (read(client_fd, &tk_len, 4) != 4) return;
        tk_len = GUINT32_FROM_BE(tk_len);
        if (tk_len == 0 || tk_len > 256) return;
        gchar tkbuf[257] = {0};
        if (read(client_fd, tkbuf, tk_len) != (ssize_t)tk_len) return;
        if (!pcv_secret_str_eq(tkbuf, G_grpc_auth_token)) {
            const char *deny = "{\"error\":\"unauthorized\"}";
            guint32 dl = GUINT32_TO_BE((guint32)strlen(deny));
            (void)!write(client_fd, &dl, 4);
            (void)!write(client_fd, deny, strlen(deny));
            return;
        }
    }

    guint32 method_len = 0;
    if (read(client_fd, &method_len, 4) != 4) return;
    method_len = GUINT32_FROM_BE(method_len);
    if (method_len > 256) return;

    gchar method[257] = {0};
    if (read(client_fd, method, method_len) != (ssize_t)method_len) return;

    guint32 payload_len = 0;
    if (read(client_fd, &payload_len, 4) != 4) return;
    payload_len = GUINT32_FROM_BE(payload_len);
    if (payload_len > GRPC_MAX_MSG) return;

    gchar *payload = g_malloc0(payload_len + 1);
    if (payload_len > 0) {
        ssize_t nr = read(client_fd, payload, payload_len);
        if (nr != (ssize_t)payload_len) { g_free(payload); return; }
    }

    const gchar *rpc_method = method;

    static const struct { const gchar *grpc; const gchar *rpc; } MAP[] = {
        {"/purecvisor.v1.VmService/List",       "vm.list"},
        {"/purecvisor.v1.VmService/Create",     "vm.create"},
        {"/purecvisor.v1.VmService/Start",      "vm.start"},
        {"/purecvisor.v1.VmService/Stop",       "vm.stop"},
        {"/purecvisor.v1.VmService/Delete",     "vm.delete"},
        {"/purecvisor.v1.VmService/ResizeDisk", "vm.resize_disk"},
        {"/purecvisor.v1.VmService/Clone",      "vm.clone"},
        {"/purecvisor.v1.NetworkService/List",   "network.list"},
        {"/purecvisor.v1.NetworkService/Create", "network.create"},
        {"/purecvisor.v1.NetworkService/Delete", "network.delete"},
        {"/purecvisor.v1.StorageService/ListPools", "storage.pool.list"},
        {"/purecvisor.v1.MonitorService/Fleet",     "monitor.fleet"},
        {"/purecvisor.v1.MonitorService/AuditSearch","audit.search"},
        {"/purecvisor.v1.SystemService/Version",    "daemon.version"},
        {"/purecvisor.v1.SystemService/Health",     "vm.list"},
        {NULL, NULL}
    };

    for (int i = 0; MAP[i].grpc; i++) {
        if (g_strcmp0(method, MAP[i].grpc) == 0) {
            rpc_method = MAP[i].rpc;
            break;
        }
    }

    extern gint G_grpc_caller_role;
    const gchar *raw_params = (payload_len > 0 && payload[0] == '{') ? payload : "{}";
    gchar *params = _grpc_inject_caller_identity(raw_params, G_grpc_caller_role);
    gchar *resp = _rpc_call(rpc_method, params);
    g_free(params);

    gchar *result = _extract_result(resp);
    g_free(resp);

    guint32 resp_len = GUINT32_TO_BE((guint32)strlen(result));
    if (write(client_fd, &resp_len, 4) < 0 ||
        write(client_fd, result, strlen(result)) < 0) {
        PCV_LOG_WARN("grpc", "Failed to send gRPC response: %s", g_strerror(errno));
    }
    g_free(result);
    g_free(payload);
}

static gpointer
_grpc_thread(gpointer data __attribute__((unused)))
{

    extern gchar *G_grpc_bind_addr;
    struct sockaddr_in saddr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = (G_grpc_bind_addr && g_strcmp0(G_grpc_bind_addr, "0.0.0.0") == 0)
                            ? INADDR_ANY : htonl(INADDR_LOOPBACK),
        .sin_port = htons((uint16_t)G.port),
    };

    G.listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (G.listen_fd < 0) {
        PCV_LOG_WARN(GRPC_LOG_DOM, "socket() failed: %s", strerror(errno));
        return NULL;
    }

    int opt = 1;
    setsockopt(G.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(G.listen_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        PCV_LOG_WARN(GRPC_LOG_DOM, "bind() port %d failed: %s", G.port, strerror(errno));
        close(G.listen_fd);
        G.listen_fd = -1;
        return NULL;
    }

    if (listen(G.listen_fd, 64) < 0) {
        PCV_LOG_WARN(GRPC_LOG_DOM, "listen() failed: %s", strerror(errno));
        close(G.listen_fd);
        G.listen_fd = -1;
        return NULL;
    }

    PCV_LOG_INFO(GRPC_LOG_DOM,
                 "gRPC server listening on port %d (protobuf-c binary protocol)",
                 G.port);

    while (G.running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(G.listen_fd,
                                (struct sockaddr *)&client_addr,
                                &client_len);
        if (client_fd < 0) {
            if (G.running) {
                PCV_LOG_WARN(GRPC_LOG_DOM, "accept() failed: %s", strerror(errno));
            }
            continue;
        }

        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        _handle_client(client_fd);
        close(client_fd);
    }

    close(G.listen_fd);
    G.listen_fd = -1;
    return NULL;
}

gchar *G_grpc_auth_token = NULL;
gchar *G_grpc_bind_addr  = NULL;
gint   G_grpc_caller_role = PCV_ROLE_OPERATOR;

static gint
_grpc_role_from_string(const gchar *s)
{
    if (g_strcmp0(s, "viewer") == 0) return PCV_ROLE_VIEWER;
    if (g_strcmp0(s, "admin")  == 0) return PCV_ROLE_ADMIN;
    return PCV_ROLE_OPERATOR;
}

void
pcv_grpc_server_start(void)
{
    const gchar *enabled = pcv_config_get_string("grpc", "enabled", "false");
    if (g_strcmp0(enabled, "true") != 0 && g_strcmp0(enabled, "1") != 0) {
        PCV_LOG_INFO(GRPC_LOG_DOM,
                     "gRPC server disabled (set [grpc] enabled=true in daemon.conf)");
        return;
    }

    G_grpc_auth_token = g_strdup(pcv_config_get_string("grpc", "auth_token", ""));
    G_grpc_bind_addr  = g_strdup(pcv_config_get_string("grpc", "bind_addr", "127.0.0.1"));

    if (!G_grpc_auth_token || !*G_grpc_auth_token) {
        PCV_LOG_ERROR(GRPC_LOG_DOM,
            "SECURITY: gRPC enabled but [grpc] auth_token unset/empty — "
            "server NOT started (unauthenticated control plane refused; "
            "set [grpc] auth_token in daemon.conf)");
        g_free(G_grpc_auth_token); G_grpc_auth_token = NULL;
        g_free(G_grpc_bind_addr);  G_grpc_bind_addr  = NULL;
        return;
    }

    G_grpc_caller_role = _grpc_role_from_string(
        pcv_config_get_string("grpc", "role", "operator"));

    gboolean is_loopback = (g_strcmp0(G_grpc_bind_addr, "127.0.0.1") == 0 ||
                            g_strcmp0(G_grpc_bind_addr, "::1") == 0);
    if (!is_loopback && !pcv_tls_is_enabled()) {
        PCV_LOG_WARN(GRPC_LOG_DOM,
            "gRPC bind_addr=%s WITHOUT TLS — server NOT started "
            "(non-loopback requires [tls] enabled=true)",
            G_grpc_bind_addr);
        return;
    }

    G.port = pcv_config_get_int("grpc", "port", 50051);
    G.running = TRUE;
    G.thread = g_thread_new("grpc-server", _grpc_thread, NULL);

    PCV_LOG_INFO(GRPC_LOG_DOM,
                 "gRPC server thread started (port %d)", G.port);
}

void
pcv_grpc_server_stop(void)
{
    if (!G.running) return;

    G.running = FALSE;
    if (G.listen_fd >= 0) {
        shutdown(G.listen_fd, SHUT_RDWR);
    }
    if (G.thread) {
        g_thread_join(G.thread);
        G.thread = NULL;
    }

    PCV_LOG_INFO(GRPC_LOG_DOM, "gRPC server stopped");
}
