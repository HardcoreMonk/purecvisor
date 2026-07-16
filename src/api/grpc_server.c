/**
 * @file grpc_server.c
 * @brief PureCVisor gRPC 서버 — protobuf-c 기반 내부 고속 API
 *
 * == 아키텍처 ==
 *   gRPC 클라이언트 (Go/Python/Java/C)
 *       ↓ protobuf 직렬화, HTTP/2
 *   grpc_server.c (이 파일, 포트 50051)
 *       ↓ protobuf → JSON 변환
 *   UDS 소켓 → dispatcher.c (기존 253 RPC 재사용)
 *       ↓ JSON-RPC 응답
 *   grpc_server.c → protobuf 직렬화 → 클라이언트
 *
 * == 설계 결정 ==
 *   gRPC Core C API는 비동기 completion queue 기반으로 매우 복잡합니다.
 *   대신 별도 GThread에서 TCP 소켓을 열고, protobuf-c로 직렬화된
 *   요청을 수신하여 기존 UDS JSON-RPC로 프록시합니다.
 *
 *   이 접근의 장점:
 *   - dispatcher.c의 253 RPC를 그대로 재사용
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
#include "utils/pcv_crypto.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_tls.h"
#include "modules/auth/pcv_rbac.h"          /* PCV_ROLE_* (bounded caller role 주입) */
#include "modules/dispatcher/rpc_utils.h"   /* pcv_rpc_parse_guarded (외부 입력 가드 파싱) */
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

    /* 응답 수신 — DISP-12b: 4MB 스택 배열은 스택오버 위험 → heap 할당으로 전환 */
    gchar *buf = g_malloc(GRPC_MAX_MSG);
    ssize_t n = read(fd, buf, GRPC_MAX_MSG - 1);
    close(fd);

    if (n <= 0) { g_free(buf); return NULL; }
    buf[n] = '\0';
    gchar *result = g_strdup(buf);
    g_free(buf);
    return result;
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

/**
 * _grpc_inject_caller_identity — UDS params에 bounded caller role/sub 주입
 *
 * Wave B Item 2 (A01/V8): gRPC 요청을 UDS JSON-RPC로 프록시할 때, dispatcher가
 * caller role을 ADMIN으로 기본 가정(_dispatcher_caller_role의 UDS 기본값)하지
 * 않도록 params에 _pcv_caller_role(정수) + _pcv_caller_sub="grpc"를 주입한다.
 * 위조 방지를 위해 클라이언트가 payload에 넣은 동일 키는 먼저 제거한 뒤 서버가
 * 재설정한다(rest_server._rpc_attach_auth_context와 동일 패턴).
 *
 * @param params_json  클라이언트 payload에서 온 params JSON(객체 기대). NULL/비객체면
 *                     주입 필드만 담은 새 객체를 만든다(fail-closed: 항상 bounded role).
 * @param role         PCV_ROLE_* 정수(config [grpc] role 매핑값).
 * @return 새로 할당된 params JSON 문자열(호출자 g_free).
 */
static gchar *
_grpc_inject_caller_identity(const gchar *params_json, gint role)
{
    JsonParser *parser = NULL;
    JsonObject *params = NULL;
    /* 클라이언트 payload는 외부 입력이므로 sanctioned 가드 파서(pcv_rpc_parse_guarded:
     * 크기·깊이 상한 DoS 방어)로 파싱한다. 실패/비객체는 fresh 객체로 fail-closed. */
    if (params_json && *params_json &&
        pcv_rpc_parse_guarded(params_json, -1, &parser, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root))
            params = json_node_get_object(root);
    }

    gchar *out = NULL;
    if (params) {
        /* 위조 방지: 클라이언트가 보낸 값 제거 후 서버가 재설정한다. */
        json_object_remove_member(params, "_pcv_caller_role");
        json_object_remove_member(params, "_pcv_caller_sub");
        json_object_set_int_member(params, "_pcv_caller_role", role);
        json_object_set_string_member(params, "_pcv_caller_sub", "grpc");
        out = json_to_string(json_parser_get_root(parser), FALSE);
    }
    if (parser) g_object_unref(parser);

    if (!out) {
        /* params 파싱 실패/비객체 → 주입 필드만 담은 새 객체(bounded role 보장). */
        out = g_strdup_printf(
            "{\"_pcv_caller_role\":%d,\"_pcv_caller_sub\":\"grpc\"}", role);
    }
    return out;
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
    /* 0. 인증 — daemon.conf [grpc] auth_token 설정 시 length-prefix(4B BE) + 토큰(≤256B) 검증 */
    extern gchar *G_grpc_auth_token; /* set in pcv_grpc_server_start */
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

    /* 6. JSON-RPC 호출 — bounded caller role/sub 주입(위조 방지: 클라이언트가 payload에
     *    넣은 _pcv_caller_role는 덮어쓴다). dispatcher가 ADMIN 기본 대신 이 role 사용. */
    extern gint G_grpc_caller_role;  /* set in pcv_grpc_server_start */
    const gchar *raw_params = (payload_len > 0 && payload[0] == '{') ? payload : "{}";
    gchar *params = _grpc_inject_caller_identity(raw_params, G_grpc_caller_role);
    gchar *resp = _rpc_call(rpc_method, params);
    g_free(params);

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
/* 보안 설정 — daemon.conf [grpc] auth_token, bind_addr, role */
gchar *G_grpc_auth_token = NULL;
gchar *G_grpc_bind_addr  = NULL;
gint   G_grpc_caller_role = PCV_ROLE_OPERATOR;  /* [grpc] role 매핑값 (기본 operator) */

/**
 * _grpc_role_from_string — [grpc] role 문자열 → PCV_ROLE_* 정수.
 * 알 수 없는 값은 최소 운영 권한 operator로 fail-safe.
 */
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

    /* Wave B Item 2 (A01/V8): 무토큰 gRPC 기동 거부 — enabled=true인데 auth_token이
     * 미설정/빈 문자열이면 서버를 시작하지 않는다(무인증 제어평면 금지, P2). 데몬
     * 나머지는 정상 동작한다. (SECURITY 전용 로그 레벨은 없어 CRITICAL+"SECURITY:" 마커) */
    if (!G_grpc_auth_token || !*G_grpc_auth_token) {
        PCV_LOG_ERROR(GRPC_LOG_DOM,
            "SECURITY: gRPC enabled but [grpc] auth_token unset/empty — "
            "server NOT started (unauthenticated control plane refused; "
            "set [grpc] auth_token in daemon.conf)");
        g_free(G_grpc_auth_token); G_grpc_auth_token = NULL;
        g_free(G_grpc_bind_addr);  G_grpc_bind_addr  = NULL;
        return;
    }

    /* Wave B Item 2 (A01/V8): gRPC caller에 부여할 bounded role.
     * [grpc] role = viewer|operator|admin (기본 operator). dispatcher가 params로
     * 주입받은 이 role을 ADMIN 기본 대신 사용한다. */
    G_grpc_caller_role = _grpc_role_from_string(
        pcv_config_get_string("grpc", "role", "operator"));

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
