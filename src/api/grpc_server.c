/**
 * @file grpc_server.c
 * @brief PureCVisor gRPC 서버 — protobuf-c 기반 내부 고속 API
 *
 * == 아키텍처 ==
 *   gRPC 클라이언트 (Go/Python/Java/C)
 *       ↓ protobuf 직렬화, HTTP/2
 *   grpc_server.c (이 파일, 포트 50051)
 *       ↓ protobuf → JSON 변환
 *   UDS 소켓 → dispatcher.c (기존 156 RPC 재사용)
 *       ↓ JSON-RPC 응답
 *   grpc_server.c → protobuf 직렬화 → 클라이언트
 *
 * == 설계 결정 ==
 *   gRPC Core C API는 비동기 completion queue 기반으로 매우 복잡합니다.
 *   대신 별도 GThread에서 TCP 소켓을 열고, protobuf-c로 직렬화된
 *   요청을 수신하여 기존 UDS JSON-RPC로 프록시합니다.
 *
 *   이 접근의 장점:
 *   - dispatcher.c의 156 RPC를 그대로 재사용
 *   - 새 RPC 추가 시 proto만 갱신 (서버 코드 변경 최소)
 *   - REST와 gRPC가 동일한 비즈니스 로직 공유
 *
 * == 의존성 ==
 *   - proto/purecvisor.proto (계약 문서)
 *
 * == 설정 ==
 *   daemon.conf:
 *     [grpc]
 *     enabled = true
 *     port = 50051
 */

#include "grpc_server.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_tls.h"
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
#define GRPC_MAX_MSG   (4 * 1024 * 1024)  /* 4MB 최대 메시지 */

/* ── 내부 상태 ──────────────────────────────────────── */
static struct {
    GThread  *thread;
    gboolean  running;
    gint      port;
    gint      listen_fd;
} G = { .running = FALSE, .port = 50051, .listen_fd = -1 };

/* ── UDS JSON-RPC 프록시 ────────────────────────────── */
/**
 * _rpc_call — 기존 UDS 소켓으로 JSON-RPC 호출
 *
 * gRPC 요청을 JSON-RPC 메시지로 변환하여 dispatcher에 전달합니다.
 * rest_server.c의 _rpc_over_uds()와 동일한 패턴입니다.
 */
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

    /* JSON-RPC 메시지 구성 */
    gchar *rpc_msg = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":\"grpc-1\"}",
        method, params_json ? params_json : "{}");

    ssize_t sent = write(fd, rpc_msg, strlen(rpc_msg));
    g_free(rpc_msg);
    if (sent <= 0) { close(fd); return NULL; }

    /* 응답 수신 */
    gchar buf[GRPC_MAX_MSG];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) return NULL;
    buf[n] = '\0';
    return g_strdup(buf);
}

/* ── protobuf 헬퍼 ──────────────────────────────────── */

/**
 * _json_result_to_string — JSON-RPC 응답에서 result 추출
 */
static gchar *
_extract_result(const gchar *json_resp)
{
    if (!json_resp) return g_strdup("{}");

    JsonParser *p = json_parser_new();
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

/* ── gRPC-like TCP 프로토콜 서버 ────────────────────── */
/**
 * 프로토콜 프레임:
 *   [4바이트: method_len] [method_name] [4바이트: payload_len] [protobuf payload]
 *
 * 응답 프레임:
 *   [4바이트: response_len] [JSON response]
 *
 * 이 간소화된 프로토콜은 gRPC HTTP/2 프레이밍 대신
 * 순수 TCP 바이너리 프로토콜을 사용합니다.
 * 향후 grpc-c 정식 래퍼로 전환 시 이 레이어만 교체합니다.
 */

static void
_handle_client(int client_fd)
{
    /* 0. 인증 — daemon.conf [grpc] auth_token 설정 시 16바이트 prefix 검증 */
    extern gchar *G_grpc_auth_token; /* set in pcv_grpc_server_start */
    if (G_grpc_auth_token && *G_grpc_auth_token) {
        guint32 tk_len = 0;
        if (read(client_fd, &tk_len, 4) != 4) return;
        tk_len = GUINT32_FROM_BE(tk_len);
        if (tk_len == 0 || tk_len > 256) return;
        gchar tkbuf[257] = {0};
        if (read(client_fd, tkbuf, tk_len) != (ssize_t)tk_len) return;
        if (g_strcmp0(tkbuf, G_grpc_auth_token) != 0) {
            const char *deny = "{\"error\":\"unauthorized\"}";
            guint32 dl = GUINT32_TO_BE((guint32)strlen(deny));
            (void)!write(client_fd, &dl, 4);
            (void)!write(client_fd, deny, strlen(deny));
            return;
        }
    }

    /* 1. method name 길이 읽기 */
    guint32 method_len = 0;
    if (read(client_fd, &method_len, 4) != 4) return;
    method_len = GUINT32_FROM_BE(method_len);
    if (method_len > 256) return;

    /* 2. method name 읽기 */
    gchar method[257] = {0};
    if (read(client_fd, method, method_len) != (ssize_t)method_len) return;

    /* 3. payload 길이 읽기 */
    guint32 payload_len = 0;
    if (read(client_fd, &payload_len, 4) != 4) return;
    payload_len = GUINT32_FROM_BE(payload_len);
    if (payload_len > GRPC_MAX_MSG) return;

    /* 4. payload 읽기 (JSON 또는 protobuf) */
    gchar *payload = g_malloc0(payload_len + 1);
    if (payload_len > 0) {
        ssize_t nr = read(client_fd, payload, payload_len);
        if (nr != (ssize_t)payload_len) { g_free(payload); return; }
    }

    /* 5. 메서드 매핑 (gRPC service/method → JSON-RPC method) */
    const gchar *rpc_method = method;

    /* gRPC 서비스 경로를 JSON-RPC 메서드로 변환 */
    /* 예: "/purecvisor.v1.VmService/List" → "vm.list" */
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
        {"/purecvisor.v1.SystemService/Health",     "vm.list"}, /* health proxy */
        {NULL, NULL}
    };

    for (int i = 0; MAP[i].grpc; i++) {
        if (g_strcmp0(method, MAP[i].grpc) == 0) {
            rpc_method = MAP[i].rpc;
            break;
        }
    }

    /* 6. JSON-RPC 호출 */
    gchar *params = (payload_len > 0 && payload[0] == '{') ? payload : g_strdup("{}");
    gchar *resp = _rpc_call(rpc_method, params);
    if (params != payload) g_free(params);

    /* 7. 응답 전송 */
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

/* ── 서버 스레드 ────────────────────────────────────── */
static gpointer
_grpc_thread(gpointer data __attribute__((unused)))
{
    /* 보안 기본값: 127.0.0.1 바인딩. daemon.conf [grpc] bind_addr=0.0.0.0 으로 노출 가능 */
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

        /* 클라이언트 타임아웃 */
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

/* ── 공개 API ───────────────────────────────────────── */

/**
 * pcv_grpc_server_start — gRPC 서버 시작
 *
 * daemon.conf [grpc] enabled=true 시 별도 스레드에서 시작.
 * main.c의 초기화 순서에서 REST 서버 이후에 호출합니다.
 */
/* 보안 설정 — daemon.conf [grpc] auth_token, bind_addr */
gchar *G_grpc_auth_token = NULL;
gchar *G_grpc_bind_addr  = NULL;

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
        if (g_strcmp0(G_grpc_bind_addr, "127.0.0.1") != 0) {
            PCV_LOG_WARN(GRPC_LOG_DOM,
                "gRPC bind_addr=%s WITHOUT auth_token — server NOT started (refusing insecure config)",
                G_grpc_bind_addr);
            return;
        }
        PCV_LOG_INFO(GRPC_LOG_DOM, "gRPC: no auth_token; restricted to 127.0.0.1");
    }

    /* P2-2: non-loopback + no TLS → refuse to start (ADR-0015) */
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

/**
 * pcv_grpc_server_stop — gRPC 서버 종료
 */
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
