/**
 * @file ws_server.c
 * @brief WebSocket 서버 — 실시간 이벤트 스트림 + VNC 프록시
 *
 * ═══════════════════════════════════════════════════════════════════
 * 아키텍처 위치:
 *   rest_server.c의 pcv_rest_server_start()에서 pcv_ws_server_init()을
 *   호출하여 SoupServer에 WebSocket 핸들러를 등록합니다.
 *   REST 서버와 동일한 SoupServer(포트 8080)를 공유하며,
 *   HTTP → WebSocket 프로토콜 업그레이드를 통해 양방향 통신을 제공합니다.
 *
 * 데이터 흐름:
 *
 *   [이벤트 스트림] /api/v1/ws/events
 *     Web UI (JavaScript) ←────── WebSocket ──────→ 이 파일
 *     텔레메트리/VM 상태 변경 시 pcv_ws_broadcast()로 전체 클라이언트에 push
 *     클라이언트→서버 메시지는 무시 (서버 push 전용, 단방향)
 *
 *   [VNC 프록시] /api/v1/ws/vnc?port=5900
 *     브라우저(noVNC) ←── WebSocket(:8080) ──→ [이 파일] ←── TCP ──→ QEMU VNC(:5900+N)
 *     바이너리 프레임을 양방향 브릿지하여 websockify(외부 도구) 없이 VNC 접속 가능
 *
 * 핵심 패턴:
 *   - 이벤트 스트림: GPtrArray로 연결된 클라이언트 목록 관리 + GMutex로 스레드 안전
 *   - VNC 프록시: VncProxy 구조체가 WebSocket↔TCP 양방향 브릿지 소유
 *     WS→TCP: "message" GObject 시그널 콜백에서 binary frame을 TCP write
 *     TCP→WS: GIOChannel watch에서 TCP readable 이벤트 감지 → WS binary frame 전송
 *   - 리소스 정리: WebSocket close 또는 TCP 끊김 시 VncProxy 전체 자원 해제
 *
 * 주의사항:
 *   - VNC 프록시는 localhost 연결만 지원 (QEMU VNC가 같은 호스트에서 실행)
 *   - 이벤트 스트림은 인증 없이 접근 가능 (방화벽으로 외부 차단 권장)
 *   - VncProxy의 closing 플래그로 이중 해제(double-free) 방지
 * ═══════════════════════════════════════════════════════════════════
 */
#include "ws_server.h"
#include "utils/pcv_log.h"
#include "utils/pcv_jwt.h"
#include "utils/pcv_config.h"
#include "../modules/dispatcher/rpc_utils.h"   /* pcv_rpc_parse_guarded — 사전인증 파싱 가드 */
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

/* Forward declaration */
static gboolean _ws_auth_callback(SoupServerMessage *msg, gpointer user_data);
#define WS_PATH    "/api/v1/ws/events"   // 실시간 이벤트 스트림 경로
#define VNC_PATH           "/api/v1/ws/vnc"      // VNC 프록시 경로
/* Fix 8: compile-time defaults — overridden by daemon.conf [ws] at init */
#define WS_DEF_MAX_CONNECTIONS  1000
#define WS_DEF_MAX_PER_IP       100
#define WS_DEF_IDLE_TIMEOUT     300
#define WS_DEF_IDLE_CHECK       60
#define WS_DEF_PING_INTERVAL    30
#define WS_DEF_PONG_TIMEOUT     60
#define WS_DEF_MAX_PENDING      500

/* Runtime-configurable values (read once at init from daemon.conf [ws] section) */
static gint ws_max_connections   = WS_DEF_MAX_CONNECTIONS;
static gint ws_max_per_ip        = WS_DEF_MAX_PER_IP;
static gint ws_idle_timeout_sec  = WS_DEF_IDLE_TIMEOUT;
static gint ws_idle_check_interval = WS_DEF_IDLE_CHECK;
static gint ws_ping_interval_sec = WS_DEF_PING_INTERVAL;
static gint ws_pong_timeout_sec  = WS_DEF_PONG_TIMEOUT;
static gint ws_max_pending_msgs  = WS_DEF_MAX_PENDING;

/*
 * 전역 상태 — 이벤트 스트림 클라이언트 관리
 *
 * clients: 현재 연결된 모든 WebSocket 클라이언트의 배열.
 *          pcv_ws_broadcast() 호출 시 이 배열의 모든 클라이언트에게 메시지를 전송합니다.
 * mu: 멀티스레드 환경에서 clients 배열 접근을 보호하는 뮤텍스.
 *     텔레메트리 스레드에서 broadcast하고, REST 스레드에서 연결/해제가 발생하므로 필수.
 * initialized: pcv_ws_server_init() 호출 여부. 초기화 전 broadcast 호출 방지.
 */
static struct {
    GPtrArray  *clients;   /* SoupWebsocketConnection* 배열 — g_object_unref로 자동 해제 */
    GHashTable *ip_counts; /* IP(gchar*) → count(GINT_TO_POINTER) — per-IP 연결 추적 */
    GMutex      mu;        /* 클라이언트 배열 접근 보호용 뮤텍스 */
    gboolean    initialized;
} G = {0};

/**
 * WebSocket 클라이언트 연결 종료 콜백
 *
 * SoupWebsocketConnection의 "closed" 시그널에 연결됩니다.
 * 클라이언트가 연결을 닫으면 clients 배열에서 제거합니다.
 *
 * @param conn 종료된 WebSocket 연결
 * @param data 사용자 데이터 (미사용)
 */
static void
_on_ws_closed(SoupWebsocketConnection *conn, gpointer data __attribute__((unused)))
{
    g_mutex_lock(&G.mu);
    /* disc_ip를 복사 — g_ptr_array_remove가 conn을 unref하면
     * g_object_get_data 반환 포인터가 dangling될 수 있음 */
    gchar *disc_ip = g_strdup(g_object_get_data(G_OBJECT(conn), "client-ip"));
    g_ptr_array_remove(G.clients, conn);
    /* Per-IP 카운트 감소 */
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

/**
 * 이벤트 스트림 클라이언트→서버 메시지 수신 콜백
 *
 * 현재는 서버 push 전용이므로 클라이언트 메시지를 무시합니다.
 * 향후 구독 필터링(특정 이벤트 타입만 수신) 등을 구현할 때 활용할 수 있습니다.
 */
static void
_on_ws_message(SoupWebsocketConnection *conn,
               SoupWebsocketDataType type __attribute__((unused)),
               GBytes *message,
               gpointer data __attribute__((unused)))
{
    /* Update last_active timestamp on any client message */
    gint now = (gint)time(NULL);
    g_object_set_data(G_OBJECT(conn), "last_active",
                      GINT_TO_POINTER(now));

    /* Backpressure: reset pending counter — client is actively consuming
     * Fix 3: use atomic operations to avoid TOCTOU with broadcast thread */
    gint *pending = g_object_get_data(G_OBJECT(conn), "pcv-pending");
    if (pending) g_atomic_int_set(pending, 0);

    gsize sz = 0;
    const gchar *text = g_bytes_get_data(message, &sz);

    /* ADR-0010: 프로토콜 레벨 인증 — 첫 메시지로 {"type":"auth","token":"JWT"} 수신
     * query string 인증 미완료 시(pcv-ws-authed=0) 여기서 인증 처리. */
    gint authed = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(conn), "pcv-ws-authed"));
    if (!authed && sz > 0 && text) {
        JsonParser *p = NULL;
        GError *perr = NULL;
        if (pcv_rpc_parse_guarded(text, (gssize)sz, &p, &perr)) {
            JsonObject *obj = json_node_get_object(json_parser_get_root(p));
            if (obj) {
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
        } else {
            /* 깊이/크기/문법 거부 — 사전인증이라 조용히 무시(응답 없음). DISP-1: 깊은
             * 중첩 텍스트 프레임이 여기서 파싱 전 거부되어 스택오버플로우 크래시 불가. */
            if (perr) g_error_free(perr);
        }
        if (!authed) { /* auth 메시지 처리 완료 — 다른 로직 스킵 */
            return;
        }
    }

    /* Track pong responses — any message from client counts as pong */
    if (sz > 0 && text && sz < 256 && g_strstr_len(text, (gssize)sz, "pong")) {
        g_object_set_data(G_OBJECT(conn), "last_pong",
                          GINT_TO_POINTER(now));
    }
}

/**
 * 이벤트 스트림 WebSocket 업그레이드 완료 콜백
 *
 * HTTP → WebSocket 프로토콜 업그레이드가 성공하면 SoupServer가 이 함수를 호출합니다.
 * 새 WebSocket 연결을 clients 배열에 추가하고, closed/message 시그널을 연결합니다.
 *
 * 참조 카운팅: g_object_ref(conn)으로 참조를 증가시켜 clients 배열이 소유합니다.
 * 해제는 _on_ws_closed()에서 배열에서 제거될 때 g_object_unref가 자동 호출됩니다
 * (GPtrArray 생성 시 free_func로 g_object_unref를 등록했기 때문).
 *
 * @param server SoupServer 인스턴스
 * @param msg    원본 HTTP 요청 메시지
 * @param path   요청 경로 (/api/v1/ws/events)
 * @param conn   업그레이드된 WebSocket 연결
 * @param data   사용자 데이터 (미사용)
 */
static void
_on_ws_connected(SoupServer *server __attribute__((unused)),
                 SoupServerMessage *msg,
                 const char *path __attribute__((unused)),
                 SoupWebsocketConnection *conn,
                 gpointer data __attribute__((unused)))
{
    /* WebSocket 인증 — ADR-0010: query string + 프로토콜 레벨 이중 지원
     * 1) URL ?token= 있으면 즉시 인증 (하위 호환)
     * 2) 없으면 미인증 상태로 연결 허용 → 5초 내 {"type":"auth"} 메시지 필요 */
    gboolean ws_authed = _ws_auth_callback(msg, NULL);
    g_object_set_data(G_OBJECT(conn), "pcv-ws-authed", GINT_TO_POINTER(ws_authed ? 1 : 0));
    if (!ws_authed) {
        /* 5초 내 프로토콜 인증 미완료 시 연결 종료 — 타임아웃 체크는 idle cleanup에서 */
        g_object_set_data(G_OBJECT(conn), "pcv-ws-auth-deadline",
                          GINT_TO_POINTER((gint)time(NULL) + 5));
    }

    /* 클라이언트 IP 추출 (핸드셰이크 HTTP 요청에서) */
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
    /* Per-IP 연결 제한 — 단일 IP가 전체 슬롯을 점유하는 DoS 방지 */
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
    /* conn에 IP를 저장하여 해제 시 카운트 감소에 사용 */
    g_object_set_data_full(G_OBJECT(conn), "client-ip",
                           g_strdup(client_ip), g_free);
    g_free(client_ip);
    /* P1-1: Set last_active/last_pong BEFORE adding to clients array.
     * Previously these were set after g_mutex_unlock, creating a race where
     * _ws_idle_cleanup could see the connection with last_active==0 (NULL)
     * and skip it via the `last > 0` guard — leaving it immune to idle timeout. */
    gint connect_time = (gint)time(NULL);
    g_object_set_data(G_OBJECT(conn), "last_active",
                      GINT_TO_POINTER(connect_time));
    g_object_set_data(G_OBJECT(conn), "last_pong",
                      GINT_TO_POINTER(connect_time));
    g_ptr_array_add(G.clients, g_object_ref(conn));
    g_mutex_unlock(&G.mu);

    /* Backpressure: per-client pending message counter (starts at 0) */
    gint *pending = g_new0(gint, 1);
    g_object_set_data_full(G_OBJECT(conn), "pcv-pending", pending, g_free);

    g_signal_connect(conn, "closed", G_CALLBACK(_on_ws_closed), NULL);
    g_signal_connect(conn, "message", G_CALLBACK(_on_ws_message), NULL);

    PCV_LOG_INFO(WS_LOG_DOM, "WebSocket client connected (total=%d)",
                 G.clients->len);
}

/* ══════════════════════════════════════════════════════════════════════════
 * VNC WebSocket-to-TCP 프록시 (websockify 대체)
 *
 * 브라우저(noVNC) ←WebSocket binary frames→ 이 핸들러 ←TCP→ QEMU VNC
 *
 * 구조: VncProxy 컨텍스트가 WebSocket 연결과 TCP 소켓을 모두 소유.
 *   - WS→TCP: on_vnc_ws_message 콜백에서 binary frame을 TCP로 write
 *   - TCP→WS: GIOChannel watch에서 TCP 데이터를 읽어 WS binary frame으로 전송
 * ══════════════════════════════════════════════════════════════════════════*/

/**
 * VNC 프록시 컨텍스트 — WebSocket↔TCP 양방향 브릿지의 핵심 구조체
 *
 * 하나의 VNC 세션(브라우저 1개 ↔ QEMU VNC 1개)에 대한 모든 상태를 보유합니다.
 * WebSocket 또는 TCP 어느 쪽이든 연결이 끊기면 _vnc_proxy_free()로 전체 자원을 해제합니다.
 *
 * 생명주기:
 *   _on_vnc_connected()에서 생성 → _vnc_ws_closed() 또는 _vnc_tcp_readable()에서 해제
 *
 * closing 플래그:
 *   이중 해제(double-free) 방지용. TCP와 WS가 거의 동시에 닫힐 수 있으므로,
 *   첫 번째 close 경로에서 closing=TRUE로 설정하여 두 번째 경로를 차단합니다.
 */
typedef struct {
    SoupWebsocketConnection *ws;          /* noVNC 브라우저 WebSocket 연결 */
    int                      tcp_fd;      /* QEMU VNC 서버 TCP 소켓 FD */
    GIOChannel              *tcp_chan;    /* tcp_fd를 GLib 이벤트 루프에서 감시하기 위한 채널 */
    guint                    tcp_watch_id; /* GIOChannel watch의 소스 ID (해제 시 g_source_remove용) */
    gboolean                 closing;     /* 이중 해제 방지 플래그 */
} VncProxy;

/**
 * VNC 프록시 자원 해제 — WebSocket과 TCP 양쪽 모두 정리합니다.
 *
 * 해제 순서가 중요합니다:
 *   1. closing 플래그 설정 (다른 콜백에서의 이중 해제 방지)
 *   2. GIOChannel watch 해제 (더 이상 TCP readable 이벤트를 받지 않음)
 *   3. GIOChannel 해제
 *   4. TCP 소켓 닫기
 *   5. WebSocket 참조 해제
 *   6. VncProxy 구조체 메모리 해제
 *
 * @param vp 해제할 VNC 프록시 (NULL 안전)
 */
static void
_vnc_proxy_free(VncProxy *vp)
{
    if (!vp) return;
    vp->closing = TRUE;  // 다른 콜백에서의 재진입 방지
    if (vp->tcp_watch_id) { g_source_remove(vp->tcp_watch_id); vp->tcp_watch_id = 0; }
    if (vp->tcp_chan) { g_io_channel_unref(vp->tcp_chan); vp->tcp_chan = NULL; }
    if (vp->tcp_fd >= 0) { close(vp->tcp_fd); vp->tcp_fd = -1; }
    if (vp->ws) { g_object_unref(vp->ws); vp->ws = NULL; }
    g_free(vp);
}

/**
 * TCP→WS 방향: QEMU VNC 서버에서 데이터 도착 시 WebSocket binary frame으로 전송
 *
 * GIOChannel watch 콜백으로, GMainLoop가 TCP 소켓에 읽을 데이터가 있으면 호출합니다.
 * VNC 프로토콜의 화면 업데이트 데이터를 WebSocket 바이너리 프레임으로 변환하여
 * 브라우저(noVNC)에 전달합니다.
 *
 * 종료 조건:
 *   - G_IO_HUP/ERR/NVAL: QEMU VNC 서버가 연결을 닫거나 에러 발생
 *   - read() 반환값 <= 0: EOF 또는 에러
 *   이 경우 WebSocket도 닫고 VncProxy 전체를 해제합니다.
 *
 * @param chan  GIOChannel (TCP 소켓 래퍼)
 * @param cond  발생한 I/O 조건 (G_IO_IN, G_IO_HUP 등)
 * @param data  VncProxy 포인터
 * @return G_SOURCE_CONTINUE=계속 감시, G_SOURCE_REMOVE=watch 해제
 */
static gboolean
_vnc_tcp_readable(GIOChannel *chan, GIOCondition cond, gpointer data)
{
    VncProxy *vp = (VncProxy *)data;
    if (vp->closing) return G_SOURCE_REMOVE;  // 이미 종료 중이면 즉시 반환

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        PCV_LOG_INFO(WS_LOG_DOM, "VNC TCP connection closed");
        if (vp->ws && soup_websocket_connection_get_state(vp->ws) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(vp->ws, SOUP_WEBSOCKET_CLOSE_GOING_AWAY, "VNC disconnected");
        vp->tcp_watch_id = 0;
        _vnc_proxy_free(vp);
        return G_SOURCE_REMOVE;
    }

    guint8 buf[65536];  // 64KB — VNC 화면 업데이트 한 번에 수용하기 충분한 크기
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

/**
 * WS→TCP 방향: 브라우저(noVNC)에서 바이너리 프레임이 오면 QEMU VNC TCP로 전달
 *
 * noVNC가 키보드/마우스 입력을 VNC 프로토콜로 인코딩한 바이너리 데이터를
 * WebSocket으로 보내면, 이 콜백이 그대로 TCP 소켓에 write합니다.
 * 텍스트 프레임은 VNC 프로토콜이 아니므로 무시합니다.
 *
 * 부분 쓰기(partial write) 처리:
 *   write() 시스템콜이 요청한 크기보다 적게 쓸 수 있으므로
 *   전체 데이터가 전송될 때까지 루프를 돌립니다.
 *
 * @param conn    WebSocket 연결 (미사용)
 * @param type    데이터 타입 (BINARY만 처리)
 * @param message 수신된 바이너리 데이터
 * @param data    VncProxy 포인터
 */
static void
_vnc_ws_message(SoupWebsocketConnection *conn __attribute__((unused)),
                SoupWebsocketDataType    type,
                GBytes                  *message,
                gpointer                 data)
{
    VncProxy *vp = (VncProxy *)data;
    if (vp->closing || vp->tcp_fd < 0) return;
    if (type != SOUP_WEBSOCKET_DATA_BINARY) return;  // 텍스트 프레임은 VNC가 아님

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

/**
 * VNC WebSocket 연결 종료 콜백 — 브라우저가 탭을 닫거나 연결을 끊을 때 호출
 *
 * TCP 쪽도 함께 정리하여 자원 누수를 방지합니다.
 * _vnc_tcp_readable()에서 TCP가 먼저 끊긴 경우에도 여기가 호출될 수 있으므로,
 * _vnc_proxy_free()의 closing 플래그로 이중 해제를 방지합니다.
 */
static void
_vnc_ws_closed(SoupWebsocketConnection *conn __attribute__((unused)), gpointer data)
{
    VncProxy *vp = (VncProxy *)data;
    PCV_LOG_INFO(WS_LOG_DOM, "VNC WebSocket closed");
    _vnc_proxy_free(vp);
}

/**
 * VNC WebSocket 업그레이드 완료 콜백 — TCP 연결 수립 + 양방향 브릿지 시작
 *
 * 브라우저가 /api/v1/ws/vnc?port=5900 으로 WebSocket 업그레이드를 요청하면,
 * SoupServer가 업그레이드를 처리한 후 이 콜백을 호출합니다.
 *
 * 동작 순서:
 *   1. URL query string에서 VNC 포트 번호 추출 (?port=5900)
 *   2. localhost:{port}로 TCP 연결 수립 (QEMU VNC 서버)
 *   3. VncProxy 구조체 생성 (WebSocket + TCP 소켓 모두 소유)
 *   4. TCP→WS: GIOChannel watch 등록 (TCP readable → WS binary frame)
 *   5. WS→TCP: "message" 시그널 연결 (WS binary frame → TCP write)
 *   6. "closed" 시그널 연결 (연결 종료 시 자원 정리)
 *
 * 실패 시: WebSocket을 GOING_AWAY 코드로 닫고 반환
 *
 * @param server SoupServer 인스턴스 (미사용)
 * @param msg    원본 HTTP 요청 (query string에서 port 추출용)
 * @param path   요청 경로 (미사용)
 * @param conn   업그레이드된 WebSocket 연결
 * @param data   사용자 데이터 (미사용)
 */
static void
_on_vnc_connected(SoupServer              *server __attribute__((unused)),
                  SoupServerMessage        *msg,
                  const char              *path __attribute__((unused)),
                  SoupWebsocketConnection *conn,
                  gpointer                 data __attribute__((unused)))
{
    /* query string에서 port 추출 (?port=5900) */
    GUri *uri = soup_server_message_get_uri(msg);
    const gchar *query = g_uri_get_query(uri);
    int vnc_port = 5900; /* 기본값 */
    if (query) {
        GHashTable *params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
        if (params) {
            const gchar *p = g_hash_table_lookup(params, "port");
            if (p) vnc_port = atoi(p);
            g_hash_table_unref(params);
        }
    }

    /* P1-2: Validate VNC port range — QEMU VNC uses 5900-6100.
     * Without this check, an attacker could probe arbitrary localhost ports
     * via the WebSocket proxy (SSRF on loopback). */
    if (vnc_port < 5900 || vnc_port > 6100) {
        PCV_LOG_WARN(WS_LOG_DOM,
            "VNC proxy: rejected port %d — outside allowed range 5900-6100", vnc_port);
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION,
                                        "VNC port out of range");
        return;
    }

    PCV_LOG_INFO(WS_LOG_DOM, "VNC proxy: connecting to localhost:%d", vnc_port);

    /* QEMU VNC 서버로 TCP 연결 — localhost만 허용 (보안) */
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);  // SOCK_CLOEXEC: fork 시 자동 close
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

    /*
     * TCP→WS 방향: GIOChannel watch로 TCP 소켓의 readable 이벤트를 감시합니다.
     *
     * GIOChannel은 GLib의 파일 디스크립터 이벤트 래퍼로,
     * GMainLoop와 통합하여 논블로킹 I/O 이벤트를 처리합니다.
     *
     * encoding=NULL: 바이너리 모드 (문자 인코딩 변환 없음)
     * buffered=FALSE: 버퍼링 없이 즉시 전달 (VNC 실시간 성능 확보)
     */
    vp->tcp_chan = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(vp->tcp_chan, NULL, NULL);   // 바이너리 모드
    g_io_channel_set_buffered(vp->tcp_chan, FALSE);        // 무버퍼 — 지연 최소화
    vp->tcp_watch_id = g_io_add_watch(vp->tcp_chan,
                                       G_IO_IN | G_IO_HUP | G_IO_ERR,
                                       _vnc_tcp_readable, vp);

    /* WS→TCP 방향: message 시그널 연결 */
    g_signal_connect(conn, "message", G_CALLBACK(_vnc_ws_message), vp);
    g_signal_connect(conn, "closed",  G_CALLBACK(_vnc_ws_closed), vp);
}

/**
 * WebSocket 서버 초기화 — REST 서버의 SoupServer에 WebSocket 핸들러를 등록합니다.
 *
 * rest_server.c의 pcv_rest_server_start()에서 호출됩니다.
 * 동일한 SoupServer(포트 8080)를 공유하므로 별도 포트가 필요 없습니다.
 *
 * 등록되는 핸들러:
 *   - /api/v1/ws/events: 이벤트 스트림 (텔레메트리, VM 상태 변경 push)
 *   - /api/v1/ws/vnc: VNC 프록시 (noVNC 브라우저 → QEMU VNC TCP 브릿지)
 *
 * @param soup REST 서버의 SoupServer 인스턴스 (공유)
 */
/* ── WebSocket JWT 인증 콜백 ──────────────────────────────────
 * soup_server_add_websocket_handler의 callback 전에 호출됩니다.
 * URL 쿼리 파라미터 ?token=<JWT> 로 인증합니다.
 * 토큰이 없거나 검증 실패 시 연결을 거부합니다.
 *
 * 예: ws://host/api/v1/ws/events?token=eyJhbGci...
 * ──────────────────────────────────────────────────────────── */
static gboolean
_ws_auth_callback(SoupServerMessage *msg,
                  gpointer           user_data __attribute__((unused)))
{
    GUri *uri = soup_server_message_get_uri(msg);
    const gchar *query = g_uri_get_query(uri);

    /* 쿼리 파라미터에서 token 추출 */
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
        /* ADR-0010: token 없으면 FALSE 반환 — 프로토콜 레벨 인증으로 폴백.
         * 401 상태를 설정하지 않음 (WS 업그레이드는 허용). */
        g_free(token);
        return FALSE;
    }

    /* JWT 검증 */
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

/**
 * WebSocket Ping/Pong 하트비트 콜백
 *
 * ws_ping_interval_sec(30초)마다 실행되며 모든 연결된 클라이언트에
 * JSON ping 메시지를 전송합니다. last_pong 타임스탬프가
 * ws_pong_timeout_sec(60초) 이상 경과한 연결은 종료합니다.
 */
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

        /* Check for stale connections — no pong within timeout */
        gint last_pong = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(conn), "last_pong"));
        if (last_pong > 0 && (now - (time_t)last_pong) > ws_pong_timeout_sec) {
            g_ptr_array_add(stale, g_object_ref(conn));
            continue;
        }

        /* Send ping */
        soup_websocket_connection_send_text(conn, ping_msg);
    }
    g_mutex_unlock(&G.mu);

    /* Close stale connections outside mutex */
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

/**
 * 유휴 WebSocket 연결 정리 타이머 콜백
 *
 * ws_idle_check_interval(60초)마다 실행되며, last_active 타임스탬프가
 * ws_idle_timeout_sec(300초) 이상 경과한 연결을 닫습니다.
 * 브로드캐스트 수신 시에도 last_active가 갱신되므로, 실제로 서버 push도
 * 받지 않는 완전한 유휴 연결만 정리됩니다.
 */
/* B6-C1: ADR-0010 미인증 5초 deadline을 강제하기 위한 1초 주기 빠른 검사.
 * _ws_idle_cleanup은 60초 주기라서 미인증 connection이 최대 65초까지 살 수 있다.
 * 이 fast check는 미인증 connection만 검사하므로 비용이 작다 (오버헤드 무시 가능). */
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
        /* ADR-0010: 미인증 연결 5초 타임아웃 */
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

    /* Close idle connections outside the mutex to avoid deadlock */
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

/* ══════════════════════════════════════════════════════════════════════════
 * R-8: 스토리지/클러스터 메트릭 주기적 WS 푸시
 *
 * 10초 주기로 CPU/메모리/디스크 사용률을 연결된 모든 WS 클라이언트에 push합니다.
 * Web UI 대시보드에서 실시간 메트릭 갱신에 사용됩니다.
 * ══════════════════════════════════════════════════════════════════════════*/

/* Forward declaration — ebpf_telemetry.h 의존 없이 약결합 */
extern JsonObject *pcv_ebpf_telemetry_get_host(void);

static gboolean
_ws_push_metrics_cb(gpointer user_data __attribute__((unused)))
{
    if (!G.initialized) return G_SOURCE_CONTINUE;

    /* 클라이언트가 없으면 직렬화 비용을 아낌 */
    g_mutex_lock(&G.mu);
    gboolean has_clients = (G.clients && G.clients->len > 0);
    g_mutex_unlock(&G.mu);
    if (!has_clients) return G_SOURCE_CONTINUE;

    /* 메트릭 수집 — CPU/메모리 (텔레메트리 모듈에서) */
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

    /* 디스크 사용률 (루트 파티션) */
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        guint64 total  = (guint64)vfs.f_blocks * vfs.f_frsize;
        guint64 free_b = (guint64)vfs.f_bfree  * vfs.f_frsize;
        if (total > 0)
            json_object_set_double_member(payload, "disk_percent",
                100.0 * (1.0 - (gdouble)free_b / (gdouble)total));
    }

    /* JSON 직렬화 후 브로드캐스트 */
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
    /* Fix 8: load WS tuning from daemon.conf [ws] section (once at init) */
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

    /* VNC WebSocket-to-TCP 프록시 (websockify 대체) */
    soup_server_add_websocket_handler(soup, VNC_PATH,
                                       NULL, NULL,
                                       _on_vnc_connected, NULL, NULL);

    /* WebSocket 인증은 _on_ws_connected / _on_vnc_connected 내부에서
     * URL 쿼리 파라미터 ?token=<JWT>를 검증합니다.
     * _ws_auth_callback()은 연결 수립 전에 호출되어 인증을 수행합니다. */

    /* Idle connection cleanup timer — every 60s, close connections idle > 5min */
    g_timeout_add_seconds(ws_idle_check_interval, _ws_idle_cleanup, NULL);

    /* B6-C1: ADR-0010 미인증 5초 deadline 강제용 1초 주기 fast check */
    g_timeout_add_seconds(1, _ws_auth_deadline_check, NULL);

    /* Ping/Pong heartbeat timer — every 30s, send ping + close stale connections */
    g_timeout_add_seconds(ws_ping_interval_sec, _ws_heartbeat_cb, NULL);

    /* R-8: 스토리지/호스트 메트릭 WS 푸시 — 10초 주기 */
    g_timeout_add_seconds(10, _ws_push_metrics_cb, NULL);

    PCV_LOG_INFO(WS_LOG_DOM, "WebSocket handler registered at %s, %s (idle timeout=%ds, ping=%ds, metrics push=10s)",
                 WS_PATH, VNC_PATH, ws_idle_timeout_sec, ws_ping_interval_sec);
}

/**
 * WebSocket 서버 종료 — 모든 클라이언트 연결을 정리합니다.
 *
 * REST 서버 종료 시 호출됩니다.
 * clients 배열 크기를 0으로 설정하면 GPtrArray의 free_func(g_object_unref)가
 * 각 WebSocket 연결에 대해 자동 호출되어 참조가 해제됩니다.
 */
void
pcv_ws_server_shutdown(void)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    g_ptr_array_set_size(G.clients, 0);  // 모든 연결 참조 해제 (free_func 호출)
    g_mutex_unlock(&G.mu);
    g_ptr_array_unref(G.clients);        // 배열 자체 해제
    g_mutex_clear(&G.mu);                // 뮤텍스 정리
    G.initialized = FALSE;
}

/**
 * 이벤트 브로드캐스트 — 연결된 모든 WebSocket 클라이언트에게 메시지를 전송합니다.
 *
 * 텔레메트리 모듈(telemetry.c), VM 이벤트 모듈(virt_events.c) 등에서
 * 상태 변경 시 이 함수를 호출하여 Web UI에 실시간 업데이트를 push합니다.
 *
 * 메시지 형식 (JSON):
 *   {"type": "telemetry", "ts": 1711234567, "payload": {...}}
 *   type: 이벤트 종류 (telemetry, vm_state, container_state 등)
 *   ts: Unix 타임스탬프 (초)
 *   payload: 이벤트별 데이터
 *
 * 스레드 안전: 뮤텍스로 clients 배열 접근을 보호합니다.
 * 닫힌 연결은 건너뜁니다 (OPEN 상태인 연결에만 전송).
 *
 * @param type         이벤트 타입 문자열 (NULL이면 무시)
 * @param payload_json 페이로드 JSON 문자열 (NULL이면 "{}" 사용)
 */
void
pcv_ws_broadcast(const gchar *type, const gchar *payload_json)
{
    if (!G.initialized || !type) return;

    /* JSON 메시지 조립: {type, ts, payload} */
    gint64 ts = (gint64)time(NULL);
    gchar *msg = g_strdup_printf("{\"type\":\"%s\",\"ts\":%ld,\"payload\":%s}",
                                  type, (long)ts,
                                  payload_json ? payload_json : "{}");

    /* 뮤텍스 보호 하에 모든 OPEN 상태 클라이언트에게 텍스트 프레임 전송 */
    g_mutex_lock(&G.mu);
    for (guint i = 0; i < G.clients->len; /* increment below */) {
        SoupWebsocketConnection *conn = g_ptr_array_index(G.clients, i);
        if (soup_websocket_connection_get_state(conn) != SOUP_WEBSOCKET_STATE_OPEN) {
            i++;
            continue;
        }
        /* Backpressure: check per-client pending message count
         * Fix 3: use atomic operations for TOCTOU-safe read/increment */
        gint *pending = g_object_get_data(G_OBJECT(conn), "pcv-pending");
        if (pending && g_atomic_int_get(pending) >= ws_max_pending_msgs) {
            const gchar *bp_ip = g_object_get_data(G_OBJECT(conn), "client-ip");
            PCV_LOG_WARN(WS_LOG_DOM,
                "WebSocket backpressure exceeded (%d msgs) for %s, closing",
                g_atomic_int_get(pending), bp_ip ? bp_ip : "unknown");
            soup_websocket_connection_close(conn,
                SOUP_WEBSOCKET_CLOSE_GOING_AWAY, "backpressure");
            /* _on_ws_closed will remove from array; don't increment i */
            /* But closed signal fires async, so remove now to avoid re-send */
            g_ptr_array_remove_index(G.clients, i);
            continue;
        }
        soup_websocket_connection_send_text(conn, msg);
        if (pending) g_atomic_int_add(pending, 1);
        /* Refresh last_active — connection is alive if receiving broadcasts */
        g_object_set_data(G_OBJECT(conn), "last_active",
                          GINT_TO_POINTER((gint)time(NULL)));
        i++;
    }
    g_mutex_unlock(&G.mu);

    g_free(msg);
}

/**
 * 현재 연결된 WebSocket 클라이언트 수 반환
 *
 * 모니터링/디버깅 목적으로 사용됩니다.
 * 스레드 안전하게 뮤텍스로 보호합니다.
 *
 * @return 연결된 클라이언트 수 (초기화 전이면 0)
 */
gint
pcv_ws_client_count(void)
{
    if (!G.initialized) return 0;
    g_mutex_lock(&G.mu);
    gint n = (gint)G.clients->len;
    g_mutex_unlock(&G.mu);
    return n;
}

/* ── ADR-0012: 비동기 작업 완료 브로드캐스트 ────────────────────────
 * fire-and-forget 핸들러의 GTask 완료 콜백에서 호출합니다.
 * pcv_ws_broadcast()를 래핑하여 job.complete 이벤트 페이로드를 조립합니다. */
void
pcv_ws_broadcast_job_complete(const gchar *job_id, const gchar *method,
                               const gchar *status, const gchar *error_msg)
{
    if (!job_id || !method || !status) return;

    gchar *payload;
    if (error_msg && *error_msg) {
        /* JSON 문자열에서 특수문자를 이스케이프 — " 와 \ 를 처리 */
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

/* ── A2-2: fire-and-forget 워커 스레드용 마샬링 래퍼 ────────────────────
 * libsoup 연결(SoupWebsocketConnection)은 생성 GMainContext(=메인 스레드)에
 * 스레드 어피니티가 있어, GTask 워커 스레드(g_task_run_in_thread / 워커 풀)에서
 * soup_websocket_connection_send_text()를 직접 호출하면 어피니티 위반이다.
 * pcv_ws_broadcast_job_complete()는 내부에서 pcv_ws_broadcast() →
 * send_text()를 직접 호출하므로, 워커 바디에서 브로드캐스트할 때는 이 _mt
 * 변형을 사용한다. 모든 인자 문자열을 heap 으로 복사(g_strdup)한 뒤
 * g_main_context_invoke(NULL, ...)로 메인 컨텍스트에 마샬링하고, 실제 전송은
 * 메인 스레드에서 원 pcv_ws_broadcast_job_complete()가 수행한다. */
typedef struct {
    gchar *job_id;
    gchar *method;
    gchar *status;
    gchar *error_msg;   /* NULL 허용 (성공 시) */
} WsJobCompleteMt;

static gboolean
_ws_broadcast_job_complete_mt_cb(gpointer user_data)
{
    WsJobCompleteMt *d = user_data;
    /* 메인 스레드 컨텍스트에서 실행됨 — send_text 어피니티 만족 */
    pcv_ws_broadcast_job_complete(d->job_id, d->method, d->status, d->error_msg);
    g_free(d->job_id);
    g_free(d->method);
    g_free(d->status);
    g_free(d->error_msg);
    g_free(d);
    return G_SOURCE_REMOVE;
}

void
pcv_ws_broadcast_job_complete_mt(const gchar *job_id, const gchar *method,
                                  const gchar *status, const gchar *error_msg)
{
    if (!job_id || !method || !status) return;

    WsJobCompleteMt *d = g_new0(WsJobCompleteMt, 1);
    d->job_id    = g_strdup(job_id);
    d->method    = g_strdup(method);
    d->status    = g_strdup(status);
    d->error_msg = g_strdup(error_msg);   /* g_strdup(NULL) == NULL 이므로 NULL 보존 */

    /* NULL context == 스레드 기본(메인) 컨텍스트. 다음 메인 루프 반복에서 _cb 실행. */
    g_main_context_invoke(NULL, _ws_broadcast_job_complete_mt_cb, d);
}
