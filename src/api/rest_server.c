/**
 * @file rest_server.c
 * @brief HTTP REST API 서버 (libsoup3 기반, 포트 8080)
 *
 * 아키텍처 위치:
 *   main.c가 pcv_rest_server_new() + pcv_rest_server_start()로 생성/시작합니다.
 *   REST 서버는 UDS 서버와 동일한 디스패처 파이프라인을 재사용하되,
 *   HTTP 요청을 JSON-RPC로 변환하여 UDS 소켓에 전송하는 "브릿지" 역할입니다.
 *
 *   [외부 클라이언트] --HTTP--> [REST 서버 (이 파일)]
 *       --JSON-RPC--> [UDS 소켓] --> [디스패처] --> [핸들러]
 *       <--JSON-RPC-- [UDS 소켓] <-- [핸들러 응답]
 *   [외부 클라이언트] <--HTTP-- [REST 서버]
 *
 *   따라서 dispatcher.c나 uds_server.c를 전혀 수정하지 않고 HTTP API를 제공합니다.
 *
 * 스레딩 모델 (데드락 방지 핵심):
 *   REST 서버는 별도 GThread에서 자체 GMainLoop(rest_loop)와 GMainContext(rest_ctx)를
 *   실행합니다. 이유: _rpc_over_uds()가 동기 블로킹 I/O로 UDS 소켓에 요청을 보내는데,
 *   메인 GMainLoop 스레드에서 실행하면 UDS 서버(같은 GMainLoop)가 응답을 보낼 수 없어
 *   데드락이 발생합니다. Phase U-3에서 raw Unix socket 직접 I/O로 전환하여 개선했습니다.
 *
 * 인증 체계:
 *   - 인증 불필요 엔드포인트:
 *     GET  /health              — 헬스체크 (서비스 상태, 버전)
 *     GET  /metrics             — Prometheus text format (인증 없이 스크레이핑 가능)
 *     GET  /internal/vms        — 클러스터 프록시 전용 (libvirt 직접 조회)
 *     GET  /internal/telemetry  — 스케줄러 전용 (CPU/MEM/VM수, /proc 기반)
 *     POST /auth/token          — JWT 로그인 (username, password → access_token)
 *   - JWT Bearer 토큰 필요:
 *     /vms, /containers, /networks, /storage, /monitor 등 모든 비즈니스 엔드포인트
 *     Authorization: Bearer <token> 헤더로 전달, pcv_jwt_verify()로 검증
 *   - 인증 컨텍스트 주입:
 *     REST 서버는 토큰에서 확인한 사용자명과 role을 JSON-RPC params의
 *     _pcv_caller_sub/_pcv_caller_role에 넣어 디스패처로 전달합니다.
 *     비전공자 관점에서는 "출입증을 확인한 직원이 실제 이름표를 붙여
 *     내부 창구로 넘기는 것"입니다. 요청자가 직접 적은 이름표는 믿지 않습니다.
 *
 * 보안 기능:
 *   - Rate Limiting: 600 req/min 초과 시 429 Too Many Requests 반환
 *   - 감사 로깅: 변경 요청(POST/PUT/DELETE) 시 클라이언트 IP + 경로를 로그에 기록
 *   - 요청 크기 제한: REST_MAX_BODY = 1MB
 *   - RPC 타임아웃: REST_RPC_TIMEOUT_SEC = 30초
 *
 * 주요 내부 함수:
 *   - _rpc_over_uds(): JSON-RPC 요청을 UDS 소켓에 동기 전송 + 응답 수신
 *   - _on_request(): SoupServer 콜백, HTTP 메서드+경로 → JSON-RPC 메서드 변환
 *   - _check_jwt(): Authorization 헤더에서 Bearer 토큰 추출 + 검증
 *
 * 주의사항:
 *   - REST 서버는 반드시 UDS 서버 시작 이후에 start해야 합니다 (UDS 소켓 의존).
 *   - /internal 엔드포인트는 클러스터 내부 통신 전용이므로 외부 방화벽에서 차단 권장.
 *   - OVS 자동 감지: POST /vms에서 network_bridge가 OVS 브릿지이면
 *     virtualport type=openvswitch를 VM XML에 자동 추가합니다.
 *
 * GObject 상속: PcvRestServer → GObject
 *   soup(SoupServer), port, thread(별도 GThread), rest_loop, rest_ctx 멤버 보유.
 */

#include "rest_server.h"
#include "rest_middleware.h"
#include "rest_auth.h"
#include "../utils/pcv_jwt.h"
#include "../utils/pcv_log.h"
#include "../modules/daemons/prometheus_exporter.h"
#include "ws_server.h"
#include "../utils/pcv_config.h"
#include "../utils/pcv_tls.h"
#if PCV_CLUSTER_ENABLED
#include "../modules/cluster/cluster_manager.h"
#endif

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <zlib.h>
#include "../modules/network/ovn_manager.h"
#include "../modules/network/dpdk_manager.h"
#include "../utils/pcv_spawn.h"
#include "../modules/auth/pcv_rbac.h"
#include "../modules/audit/pcv_audit.h"
#include "../modules/dispatcher/rpc_utils.h"
#include "../modules/virt/circuit_breaker.h"
#include "purecvisor/version.h"
#include <unistd.h>

#define REST_LOG_DOM   "rest_server"

/* HSTS 전송 여부 — daemon.conf [tls] hsts=true 명시적 설정 시에만 TRUE.
 * 자체서명 인증서 환경에서 HSTS 전송 시 브라우저가 ERR_CERT_AUTHORITY_INVALID 유발.
 * 기본값 FALSE: 공인 인증서 사용 시에만 명시적으로 hsts=true 설정. */
static gboolean g_hsts_enabled = FALSE;

/* [ADR-0014] CSRF 토큰 제거됨 — JWT Bearer 인증이 CSRF 방어를 대체 */
#define REST_API_PREFIX "/api/v1"
constexpr int REST_MAX_BODY = 1 * 1024 * 1024;   /* 1 MB */
constexpr int REST_RPC_TIMEOUT_SEC = 8;  /* 8초 — 기본 RPC 타임아웃 (BE-A6에서 2→8초 상향).
                                           * 장기 실행 메서드는 pcv_get_rpc_timeout()으로 개별 조정. */
#define PCV_OVN_DEMO_HEALTH_PATH "/var/lib/purecvisor/demo/ovn-ovs-health.json"
#define PCV_OVN_DEMO_HEALTH_STALE_SEC 300

/* Per-method RPC 타임아웃 → rest_middleware.c로 이동 (pcv_get_rpc_timeout) */

/* Per-endpoint Rate Limit 티어 → rest_middleware.c로 이동 (pcv_get_endpoint_rate_limit) */

/* ETag 생성 → rest_middleware.c로 이동 (pcv_compute_etag) */

/* ── GObject ─────────────────────────────────────────────────── */
struct _PcvRestServer {
    GObject      parent_instance;
    SoupServer  *soup;
    guint16      port;
    guint16      https_port;    /* HTTPS 포트 (기본 8443, TLS 비활성 시 0) */
    gboolean     tls_active;    /* HTTPS 리스닝 성공 여부 */
    /* Sprint E fix: REST 서버를 독립 스레드/컨텍스트에서 실행하여
     * 메인 GMainLoop 블록 방지. _on_request → _rpc_over_uds 는 동기
     * 블로킹 I/O인데, 메인 GMainLoop 스레드에서 실행하면 UDS 서버(같은
     * GMainLoop)가 응답을 보낼 수 없어 데드락이 발생합니다.       */
    GThread     *thread;
    GMainLoop   *rest_loop;
    GMainContext *rest_ctx;
};

/* G_DEFINE_TYPE 매크로 — GObject 타입 시스템 등록
 *
 * [주니어 개발자 참고 — GObject란?]
 * C에는 클래스가 없으므로 GLib이 제공하는 객체 시스템입니다.
 * - 참조 카운팅 (g_object_ref / g_object_unref)
 * - 시그널 (g_signal_connect / g_signal_emit)
 * - 프로퍼티 (get/set)
 *
 * G_DEFINE_TYPE은 다음을 자동 생성합니다:
 *   - pcv_rest_server_get_type() 함수
 *   - PCV_REST_SERVER() 캐스팅 매크로
 *   - PCV_TYPE_REST_SERVER 타입 ID
 *   - pcv_rest_server_parent_class 포인터
 */
G_DEFINE_TYPE(PcvRestServer, pcv_rest_server, G_TYPE_OBJECT)

/* ═══════════════════════════════════════════════════════════════
 * UDS JSON-RPC 클라이언트 (핵심 브릿지)
 * ─────────────────────────────────────────────────────────────
 * REST 요청마다 UDS 소켓에 연결하여 JSON-RPC를 전송하고
 * 응답을 수신한 후 연결을 닫습니다.
 * ═══════════════════════════════════════════════════════════════ */
/**
 * REST→UDS 브릿지: HTTP 요청을 JSON-RPC로 변환하여 UDS 소켓에 동기 전송
 *
 * REST 서버의 핵심 함수입니다. REST API의 모든 비즈니스 엔드포인트는
 * 이 함수를 통해 UDS 서버(dispatcher)에 JSON-RPC 요청을 전달합니다.
 *
 * 동작 순서:
 *   1. raw Unix socket 생성 (AF_UNIX, Phase U-3에서 GIO→raw socket으로 전환)
 *   2. SO_RCVTIMEO/SO_SNDTIMEO 설정 (30초 타임아웃 — 무한 대기 방지)
 *   3. daemon.sock에 connect
 *   4. JSON-RPC 문자열 + 개행('\n') 전송 (개행이 메시지 구분자)
 *   5. shutdown(SHUT_WR)로 write 방향 종료 (서버에 EOF 전달)
 *   6. 응답 수신 (최대 64KB, 서버가 close하면 read 종료)
 *   7. 소켓 닫기 + 응답 문자열 반환
 *
 * Phase U-3 변경 이유:
 *   이전에는 GSocketClient/GIO를 사용했으나, GLib 내부의 GMainContext 인터랙션이
 *   REST 스레드와 메인 스레드 간 데드락을 유발했습니다.
 *   raw Unix socket으로 전환하여 GLib 의존성을 완전히 제거했습니다.
 *
 * @param rpc_json JSON-RPC 2.0 요청 문자열
 * @return JSON-RPC 응답 문자열 (호출자가 g_free()로 해제)
 *         에러 시에도 JSON 에러 객체를 반환 (NULL 반환 없음)
 */
static gchar *
_rpc_over_uds_timeout(const gchar *rpc_json, gint timeout_sec)
{
    const gchar *sock_path = pcv_config_get_socket_path();

    /* ── Phase U-3: raw Unix socket (GSocketClient/GIO 오버헤드 제거) ──
     *
     * [왜 raw socket인가?]
     * GLib의 GSocketClient를 사용하면 내부적으로 GMainContext와 상호작용하여
     * REST 스레드와 메인 스레드 간 데드락이 발생했습니다.
     * raw Unix socket(POSIX API)을 직접 사용하면 GLib 의존성이 없어 안전합니다.
     *
     * [어떻게 동작하나?]
     * AF_UNIX: Unix Domain Socket (같은 머신 내 IPC 전용, TCP 아님)
     * SOCK_STREAM: TCP처럼 순서 보장, 바이트 스트림 방식
     * SOCK_CLOEXEC: fork+exec 시 이 fd를 자동으로 닫음 (보안 — 자식 프로세스 fd 누출 방지)
     */
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return g_strdup_printf(
            "{\"error\":{\"code\":\"DAEMON_UNAVAILABLE\","
             "\"message\":\"socket() failed: %s\"}}", strerror(errno));
    }

    /* 타임아웃 설정 — SO_RCVTIMEO(수신), SO_SNDTIMEO(송신)
     *
     * [왜 타임아웃이 필요한가?]
     * 타임아웃 없이 read()/write()를 호출하면, 데몬이 응답하지 않을 때
     * REST 스레드가 영원히 블로킹됩니다. 2초 타임아웃으로 최대 대기 시간을 제한합니다.
     *
     * [어떻게?]
     * setsockopt()로 소켓 옵션을 설정합니다:
     * - SOL_SOCKET: 소켓 레벨 옵션 (프로토콜 무관)
     * - SO_RCVTIMEO: read() 타임아웃 (timeout_sec 초과 시 errno=EAGAIN 반환)
     * - SO_SNDTIMEO: write() 타임아웃 (timeout_sec 초과 시 errno=EAGAIN 반환)
     */
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* 1. connect — UDS 소켓에 연결
     *
     * [어떻게?]
     * sockaddr_un 구조체에 소켓 경로를 설정하고 connect()로 연결합니다.
     * 소켓 경로: /var/run/purecvisor/daemon.sock (UDS 서버가 listen 중)
     * memset으로 0 초기화: 구조체 패딩 바이트에 쓰레기 값 방지
     * g_strlcpy: 버퍼 오버플로 방지 (sun_path 최대 108바이트)
     */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, sock_path, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        PCV_LOG_WARN("rest", "connect(%s) failed: %s", sock_path, strerror(errno));
        gchar *msg = g_strdup(
            "{\"error\":{\"code\":\"DAEMON_UNAVAILABLE\","
             "\"message\":\"Daemon temporarily unavailable\"}}");
        close(fd);
        return msg;
    }

    /* 2. 요청 전송 (JSON + 개행)
     *
     * [왜 개행('\n')을 붙이는가?]
     * UDS 서버(uds_server.c)는 개행 문자를 메시지 구분자로 사용합니다.
     * 개행이 없으면 서버가 "아직 메시지가 더 올 수 있다"고 판단하여 대기합니다.
     *
     * perf: writev로 JSON + "\n" 두 iovec을 단일 syscall에 전송하여
     * g_strdup_printf("%s\n", rpc_json) heap 할당 제거.
     */
    {
        gsize json_len = strlen(rpc_json);
        struct iovec iov[2] = {
            { .iov_base = (void *)rpc_json, .iov_len = json_len },
            { .iov_base = (void *)"\n",     .iov_len = 1        }
        };
        gssize total_len = (gssize)(json_len + 1);
        gssize sent = writev(fd, iov, 2);
        if (sent != total_len) {
            close(fd);
            return g_strdup("{\"error\":{\"code\":\"WRITE_FAILED\","
                             "\"message\":\"UDS write failed\"}}");
        }
    }

    /*
     * write 방향 종료 (half-close) — UDS 서버에 "요청 전송 완료"를 알립니다.
     * 이것이 없으면 서버의 read()가 더 많은 데이터를 기다리며 블로킹될 수 있습니다.
     * shutdown(SHUT_WR) 후에도 read 방향은 열려있으므로 응답을 수신할 수 있습니다.
     */
    shutdown(fd, SHUT_WR);

    /* 3. 응답 수신 (최대 64KB)
     *
     * [어떻게 동작하는가?]
     * read()를 반복 호출하여 UDS 서버의 응답을 수신합니다.
     * n <= 0 조건: 서버가 소켓을 닫았거나(EOF, n=0), 에러(n<0) 시 루프 종료.
     * shutdown(SHUT_WR) 덕분에 서버는 "클라이언트가 전송 완료"를 알고 응답을 보냅니다.
     *
     * [왜 64KB 제한인가?]
     * JSON-RPC 응답은 일반적으로 수 KB입니다.
     * 64KB는 VM 수백 개의 목록을 담기에 충분하면서도 메모리 낭비를 방지합니다.
     */
    /* [A-1 수정] 동적 버퍼 — 64KB 고정 → GByteArray 가변 길이
     * monitor.processes 등 대용량 JSON 응답(프로세스 수백 개)이
     * 64KB를 초과하면 JSON 파싱 에러 → HTTP 500 발생.
     * GByteArray로 전환하여 응답 크기 제한 없이 수신. */
    GByteArray *dyn_buf = g_byte_array_new();
    gchar tmp[8192];
    for (;;) {
        gssize n = read(fd, tmp, sizeof(tmp));
        if (n < 0) {
            /* [A-3 수정] 타임아웃(EAGAIN) vs 기타 에러 구분 */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                g_byte_array_free(dyn_buf, TRUE);
                close(fd);
                return g_strdup_printf("{\"jsonrpc\":\"2.0\",\"error\":{"
                    "\"code\":-32003,"
                    "\"message\":\"RPC timeout — daemon did not respond within %ds\"}}",
                    timeout_sec);
            }
            break;
        }
        if (n == 0) break;
        g_byte_array_append(dyn_buf, (guint8 *)tmp, (guint)n);
    }
    close(fd);
    gssize total = (gssize)dyn_buf->len;

    if (total <= 0) {
        g_byte_array_free(dyn_buf, TRUE);
        return g_strdup("{\"jsonrpc\":\"2.0\",\"error\":{"
            "\"code\":-32000,"
            "\"message\":\"No response from daemon\"}}");
    }

    /* NULL 종료 + 개행 제거 */
    g_byte_array_append(dyn_buf, (guint8 *)"\0", 1);
    if (total > 0 && dyn_buf->data[total - 1] == '\n')
        dyn_buf->data[total - 1] = '\0';

    gchar *line = g_strdup((gchar *)dyn_buf->data);
    g_byte_array_free(dyn_buf, TRUE);

    if (!line) {
        return g_strdup("{\"error\":{\"code\":\"READ_FAILED\","
                         "\"message\":\"No response from daemon\"}}");
    }

    return line;   /* 호출자가 g_free()로 해제해야 함 */
}

/* 기본 타임아웃 래퍼 — 기존 코드 호환 */
static gchar *
_rpc_over_uds(const gchar *rpc_json)
{
    return _rpc_over_uds_timeout(rpc_json, REST_RPC_TIMEOUT_SEC);
}

/* ═══════════════════════════════════════════════════════════════
 * HTTP 응답 헬퍼
 * ═══════════════════════════════════════════════════════════════ */
/**
 * HTTP JSON 응답 전송 헬퍼
 *
 * 모든 REST 응답에 공통 보안 헤더를 설정합니다:
 *   - Content-Type: application/json; charset=utf-8
 *   - X-Content-Type-Options: nosniff (MIME 스니핑 공격 방지)
 *   - Cache-Control: no-store (API 응답 캐싱 방지 — 보안)
 *
 * @param msg    SoupServerMessage (HTTP 응답 대상)
 * @param status HTTP 상태 코드 (200, 400, 401, 404, 500 등)
 * @param body   JSON 응답 본문 문자열
 */
static void
_send_json(SoupServerMessage *msg, guint status, const gchar *body)
{
    soup_server_message_set_status(msg, status, NULL);
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(hdrs, "Content-Type",
                                  "application/json; charset=utf-8");
    soup_message_headers_replace(hdrs, "X-Content-Type-Options", "nosniff");

    /* ── ETag + Cache-Control (BE-A1) ────────────────────────────────
     * GET 요청: ETag 헤더 + 조건부 캐싱 (5초 유효, must-revalidate)
     *   If-None-Match 일치 시 304 Not Modified 반환 → 대역폭 절약
     * POST/PUT/DELETE: no-store (변경 요청은 캐싱 금지 — 보안) */
    {
        const gchar *req_method = soup_server_message_get_method(msg);
        gboolean is_get = (g_strcmp0(req_method, "GET") == 0);

        if (is_get && status == 200 && body) {
            gsize body_len = strlen(body);
            gchar *etag = pcv_compute_etag(body, body_len);
            soup_message_headers_replace(hdrs, "ETag", etag);
            soup_message_headers_replace(hdrs, "Cache-Control",
                                          "private, max-age=5, must-revalidate");
            /* If-None-Match 조건부 응답 — 동일 ETag이면 304 반환 */
            SoupMessageHeaders *req_hdrs =
                soup_server_message_get_request_headers(msg);
            const gchar *if_none_match =
                soup_message_headers_get_one(req_hdrs, "If-None-Match");
            if (if_none_match && g_strcmp0(if_none_match, etag) == 0) {
                soup_server_message_set_status(msg, 304, NULL);
                g_free(etag);
                return;  /* 304 Not Modified — 본문 전송 불필요 */
            }
            g_free(etag);
        } else {
            soup_message_headers_replace(hdrs, "Cache-Control", "no-store");
        }
    }

    /* HTTP keep-alive 비��성화 — CLOSE-WAIT 소켓 누적 방지.
     * keep-alive 연결이 서버에서 close되지 않으면 CLOSE-WAIT 상태로 잔류하여
     * listen backlog(10)을 고갈시키고 새 연결 accept 불가 → REST 무응답.
     * Connection: close로 매 응답 후 즉시 연결 종료. */
    soup_message_headers_replace(hdrs, "Connection", "close");
    /* Security response headers — OWASP recommended */
    /* HSTS: HTTPS 활성 시에만 전송 — HTTP 전용 환경에서 HSTS 전송 시
       브라우저가 자체서명 인증서로 https 강제 → ERR_CERT_AUTHORITY_INVALID */
    if (g_hsts_enabled) {
        soup_message_headers_replace(hdrs, "Strict-Transport-Security",
                                      "max-age=31536000; includeSubDomains");
    }
    soup_message_headers_replace(hdrs, "Content-Security-Policy",
                                  "default-src 'self'; script-src 'self' 'unsafe-inline'; "
                                  "style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                                  "font-src 'self'; "
                                  "connect-src 'self' ws: wss:");
    soup_message_headers_replace(hdrs, "X-Frame-Options", "SAMEORIGIN");
    soup_message_headers_replace(hdrs, "X-XSS-Protection", "1; mode=block");
    soup_message_headers_replace(hdrs, "Referrer-Policy",
                                  "strict-origin-when-cross-origin");
    soup_message_headers_replace(hdrs, "Permissions-Policy",
                                  "camera=(), microphone=(), geolocation=()");
    /* Vary header for correct caching with compression */
    soup_message_headers_replace(hdrs, "Vary", "Accept-Encoding");

    gsize body_len = strlen(body);

    /* ── gzip 압축 — 클라이언트가 지원하고 본문이 256바이트 초과 시 ──
     *
     * [왜 gzip 압축을 하는가?]
     * REST API 응답(JSON)은 텍스트이므로 gzip으로 60~80% 크기를 줄일 수 있습니다.
     * 대역폭 절약 + 응답 전송 시간 단축 (특히 VM 수백 개의 목록 응답).
     *
     * [어떻게?]
     * 1. Accept-Encoding 헤더 확인: 클라이언트(브라우저/curl)가 gzip 지원하는지
     * 2. zlib의 deflateInit2()로 gzip 압축기 초기화 (windowBits=15+16 → gzip 포맷)
     * 3. deflate()로 원본 바디를 압축
     * 4. 압축 결과가 원본보다 작을 때만 적용 (작은 데이터는 오히려 커질 수 있음)
     * 5. Content-Encoding: gzip 헤더 설정 → 클라이언트가 자동 압축 해제
     *
     * [256바이트 임계값]
     * 짧은 JSON(예: {"data":true})은 gzip 헤더 오버헤드(~20바이트)로
     * 압축 후 오히려 커질 수 있어 256바이트 이하는 건너뜁니다.
     */
    SoupMessageHeaders *req_hdrs = soup_server_message_get_request_headers(msg);
    const gchar *accept_enc = soup_message_headers_get_one(req_hdrs,
                                                            "Accept-Encoding");
    if (accept_enc && strstr(accept_enc, "gzip") != nullptr && body_len > 256) {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        /* windowBits = 15 + 16 for gzip format wrapper */
        int zinit_ret = deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                     15 + 16, 8, Z_DEFAULT_STRATEGY);
        if (zinit_ret != Z_OK) {
            /* Fix 7: log deflateInit2 failure for debugging */
            g_warning("rest_server: deflateInit2 failed (ret=%d), sending uncompressed",
                      zinit_ret);
        }
        if (zinit_ret == Z_OK) {
            gsize max_compressed = deflateBound(&zs, (uLong)body_len);
            guchar *compressed = g_malloc(max_compressed);
            zs.next_in  = (Bytef *)body;
            zs.avail_in = (uInt)body_len;
            zs.next_out  = compressed;
            zs.avail_out = (uInt)max_compressed;

            int zret = deflate(&zs, Z_FINISH);
            if (zret == Z_STREAM_END) {
                gsize compressed_len = zs.total_out;
                deflateEnd(&zs);
                /* Only use compressed version if actually smaller */
                if (compressed_len < body_len) {
                    soup_message_headers_replace(hdrs, "Content-Encoding",
                                                  "gzip");
                    soup_server_message_set_response(msg, "application/json",
                                                      SOUP_MEMORY_COPY,
                                                      (const gchar *)compressed,
                                                      compressed_len);
                    g_free(compressed);
                    return;
                }
            } else {
                deflateEnd(&zs);
            }
            g_free(compressed);
        }
    }

    /* Uncompressed fallback */
    soup_server_message_set_response(msg, "application/json",
                                      SOUP_MEMORY_COPY,
                                      body, body_len);
}

/**
 * HTTP 에러 응답 전송 헬퍼
 *
 * 에러를 {"error":{"code":"...", "message":"..."}} 형식으로 반환합니다.
 * REST API 클라이언트가 에러 코드로 프로그래밍 가능하게 합니다.
 *
 * @param msg    SoupServerMessage
 * @param status HTTP 상태 코드
 * @param code   에러 코드 문자열 (예: "UNAUTHORIZED", "NOT_FOUND")
 * @param detail 사람이 읽을 수 있는 에러 메시지
 */
static void
_error(SoupServerMessage *msg, guint status,
       const gchar *code, const gchar *detail)
{
    gchar *body = g_strdup_printf(
        "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
        code, detail);
    _send_json(msg, status, body);
    g_free(body);
}

/* ═══════════════════════════════════════════════════════════════
 * JSON-RPC 빌더 헬퍼
 * ═══════════════════════════════════════════════════════════════ */
/**
 * JSON-RPC 2.0 요청 문자열 빌드
 *
 * HTTP 메서드+경로에서 결정된 RPC 메서드명과 파라미터를 조합하여
 * UDS 소켓으로 전송할 JSON-RPC 요청을 생성합니다.
 *
 * 생성되는 JSON 형식:
 *   {"jsonrpc":"2.0","method":"vm.list","params":{},"id":1}
 *
 * params가 NULL이면 빈 객체 {}를 사용합니다.
 * params가 non-NULL이면 JsonNode로 래핑하여 삽입합니다.
 * 주의: params의 소유권은 이전되지 않습니다 (호출자가 관리).
 *
 * @param method RPC 메서드명 (예: "vm.list", "vm.create")
 * @param params RPC 파라미터 JsonObject (NULL 가능)
 * @return JSON-RPC 문자열 (호출자가 g_free()로 해제)
 */
/* REST JSON 스키마 사전 검증 → rest_middleware.c로 이동 (pcv_validate_required) */

static gchar *
_build_rpc(const gchar *method, JsonObject *params)
{
    JsonBuilder *jb = json_builder_new();
    json_builder_begin_object(jb);

    json_builder_set_member_name(jb, "jsonrpc");
    json_builder_add_string_value(jb, "2.0");
    json_builder_set_member_name(jb, "method");
    json_builder_add_string_value(jb, method);
    json_builder_set_member_name(jb, "params");

    if (params) {
        /* [JsonObject → JsonNode 래핑 — 왜 필요한가?]
         * JsonBuilder는 JsonNode를 입력으로 받습니다.
         * JsonObject를 직접 추가할 수 없으므로 JsonNode로 감싸야 합니다.
         * json_builder_add_value()가 params_node의 소유권을 가져갑니다. */
        JsonNode *params_node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(params_node, params);
        json_builder_add_value(jb, params_node);
    } else {
        /* params가 NULL이면 빈 객체 {} 사용 — JSON-RPC 스펙 요구사항 */
        json_builder_begin_object(jb);
        json_builder_end_object(jb);
    }

    json_builder_set_member_name(jb, "id");
    json_builder_add_int_value(jb, 1);
    json_builder_end_object(jb);

    /* [JSON 직렬화 — JsonBuilder → 문자열]
     * JsonBuilder로 구축한 JSON 트리를 문자열로 변환합니다.
     * JsonGenerator: JSON 트리 → 문자열 변환기
     *   json_builder_get_root(): 최상위 JsonNode 추출
     *   json_generator_to_data(): JsonNode → gchar* 문자열
     *
     * 메모리 해제 순서: root → gen → jb (역순 해제) */
    JsonNode *root = json_builder_get_root(jb);
    gchar *rpc = json_to_string(root, FALSE);

    json_node_free(root);
    g_object_unref(jb);
    return rpc;   /* 호출자가 g_free()로 해제 */
}

/**
 * 이름 기반 JSON-RPC 빌드 편의 함수
 *
 * 대부분의 엔드포인트가 {name} 파라미터만 필요하므로 이 헬퍼를 제공합니다.
 * 예: _build_rpc_name("vm.start", "web-prod")
 *   → {"jsonrpc":"2.0","method":"vm.start","params":{"name":"web-prod"},"id":1}
 *
 * @param method RPC 메서드명
 * @param name   대상 리소스 이름 (VM명, 컨테이너명, 브릿지명 등)
 * @return JSON-RPC 문자열 (호출자가 g_free()로 해제)
 */
static gchar *
_build_rpc_name(const gchar *method, const gchar *name)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", name);
    gchar *rpc = _build_rpc(method, p);
    json_object_unref(p);
    return rpc;
}

static gchar *
_rpc_attach_auth_context(const gchar *rpc_json,
                         const gchar *subject,
                         PcvRole      role)
{
    /* [비전공자 설명]
     * HTTP 요청 본문은 클라이언트가 직접 작성하므로 그대로 믿을 수 없습니다.
     * 예를 들어 operator가 params 안에 "_pcv_caller_role":2 라고 적으면
     * admin인 척할 수 있습니다. 그래서 REST 서버가 JWT로 확인한 실제 사용자와
     * 실제 role을 다시 주입하고, 기존 값은 먼저 제거합니다.
     *
     * [주니어 참고]
     * 이 함수는 JSON-RPC envelope을 파싱해 params 객체를 보장한 뒤,
     * dispatcher.c가 owner-scope/RBAC에서 사용할 내부 메타 필드를 넣습니다.
     * 파싱에 실패하면 원문을 반환해 기존 error path가 처리하게 둡니다. */
    if (!rpc_json)
        return NULL;

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_data(parser, rpc_json, -1, &error)) {
        if (error) g_error_free(error);
        g_object_unref(parser);
        return g_strdup(rpc_json);
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return g_strdup(rpc_json);
    }

    JsonObject *root_obj = json_node_get_object(root);
    JsonObject *params = NULL;
    JsonNode *params_node = json_object_get_member(root_obj, "params");
    if (params_node && JSON_NODE_HOLDS_OBJECT(params_node)) {
        params = json_node_get_object(params_node);
    } else {
        params = json_object_new();
        json_object_set_object_member(root_obj, "params", params);
    }

    json_object_remove_member(params, "_pcv_caller_sub");
    json_object_remove_member(params, "_pcv_caller_role");
    if (subject && *subject)
        json_object_set_string_member(params, "_pcv_caller_sub", subject);
    json_object_set_int_member(params, "_pcv_caller_role", (gint)role);

    gchar *rpc_with_context = json_to_string(root, FALSE);
    g_object_unref(parser);
    return rpc_with_context ? rpc_with_context : g_strdup(rpc_json);
}

/* ═══════════════════════════════════════════════════════════════
 * RPC 응답 → HTTP 응답 변환
 * ═══════════════════════════════════════════════════════════════ */
/**
 * RPC 응답 → HTTP 응답 변환
 *
 * UDS 소켓에서 수신한 JSON-RPC 응답을 파싱하여 REST API 형식으로 변환합니다.
 *
 * 변환 규칙:
 *   - JSON-RPC "result" 필드 → {"data": ...} (HTTP 200)
 *   - JSON-RPC "error" 필드 → {"error": ...} (에러 코드에 따라 HTTP 400/404/500)
 *
 * JSON-RPC 에러 코드 → HTTP 상태 코드 매핑:
 *   -32602 (Invalid params) → 400 Bad Request
 *   -32601 (Method not found) → 404 Not Found
 *   -32001 (VM not found, 커스텀) → 404 Not Found
 *   기타 → 500 Internal Server Error
 *
 * @param msg      HTTP 응답 대상
 * @param rpc_resp UDS에서 수신한 JSON-RPC 응답 문자열
 */

/* ── R-3: 커서 기반 페이지네이션 Link 헤더 (RFC 8288) ──────── */

/**
 * _add_pagination_headers — 리스트 응답에 페이지네이션 메타데이터 헤더 추가
 *
 * X-Total-Count: 전체 항목 수
 * Link: RFC 8288 형식으로 next/prev 링크 제공
 *
 * @param msg       SoupServerMessage
 * @param total     전체 항목 수
 * @param offset    현재 오프셋
 * @param limit     페이지 크기
 * @param base_path API 경로 (예: "/api/v1/vms")
 */
static void
_add_pagination_headers(SoupServerMessage *msg, gint total, gint offset,
                        gint limit, const gchar *base_path)
{
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);

    gchar *total_str = g_strdup_printf("%d", total);
    soup_message_headers_replace(hdrs, "X-Total-Count", total_str);
    g_free(total_str);

    /* Link header (RFC 8288) — next/prev 커서 */
    GString *links = g_string_new("");
    if (offset + limit < total) {
        g_string_append_printf(links, "<%s?offset=%d&limit=%d>; rel=\"next\"",
                               base_path, offset + limit, limit);
    }
    if (offset > 0) {
        if (links->len > 0) g_string_append(links, ", ");
        gint prev_offset = offset - limit;
        if (prev_offset < 0) prev_offset = 0;
        g_string_append_printf(links, "<%s?offset=%d&limit=%d>; rel=\"prev\"",
                               base_path, prev_offset, limit);
    }
    if (links->len > 0)
        soup_message_headers_replace(hdrs, "Link", links->str);
    g_string_free(links, TRUE);
}

static void
_send_rpc_result(SoupServerMessage *msg, const gchar *rpc_resp)
{
    if (!rpc_resp) {
        _error(msg, 500, "NO_RESPONSE", "No response from daemon");
        return;
    }

    /* [어떻게?] JSON-RPC 응답 문자열을 파싱하여 result 또는 error 필드를 추출.
     * JsonParser: json-glib 라이브러리의 JSON 파서 (DOM 방식)
     * -1: 문자열 길이를 자동 계산 (strlen) */
    JsonParser *p   = json_parser_new();
    GError     *err = nullptr;

    if (!json_parser_load_from_data(p, rpc_resp, -1, &err)) {
        _error(msg, 500, "PARSE_ERROR",
               err ? err->message : "Invalid JSON from daemon");
        if (err) g_error_free(err);
        g_object_unref(p);
        return;
    }

    JsonObject    *obj = json_node_get_object(json_parser_get_root(p));
    gchar         *body_str;
    guint          status;

    /* [변환 규칙 — 주니어 개발자 필독]
     *
     * JSON-RPC 성공 응답:  {"jsonrpc":"2.0", "result": {...}, "id":1}
     *   → REST 응답:       {"data": {...}}    + HTTP 200
     *
     * JSON-RPC 에러 응답:  {"jsonrpc":"2.0", "error": {"code":-32602, "message":"..."}, "id":1}
     *   → REST 응답:       {"error": {...}}   + HTTP 400/404/500
     *
     * [왜 이렇게 변환하는가?]
     * REST API 클라이언트(Web UI, curl)는 JSON-RPC 프로토콜을 모릅니다.
     * HTTP 상태 코드 + {"data":...} / {"error":...} 형식이 REST 표준에 가깝습니다.
     */
    if (json_object_has_member(obj, "result")) {
        /* 성공 경로: result 필드를 data로 감싸서 HTTP 200 반환 */
        JsonNode *result = json_object_get_member(obj, "result");
        gchar *r = json_to_string(result, FALSE);
        body_str = g_strdup_printf("{\"data\":%s}", r);
        g_free(r);
        status = 200;

        /* R-3: 리스트 응답에 페이���네이션 헤더 추가 */
        {
            GUri *uri = soup_server_message_get_uri(msg);
            const gchar *req_path = uri ? g_uri_get_path(uri) : NULL;
            const gchar *req_query = uri ? g_uri_get_query(uri) : NULL;
            if (req_path && JSON_NODE_HOLDS_ARRAY(result)) {
                gint total = (gint)json_array_get_length(json_node_get_array(result));
                gint offset = 0, limit = total;
                if (req_query) {
                    /* offset/limit 쿼리 파라미터 파싱 */
                    gchar **pairs = g_strsplit(req_query, "&", -1);
                    for (gchar **p = pairs; *p; p++) {
                        if (g_str_has_prefix(*p, "offset="))
                            offset = (gint)g_ascii_strtoll(*p + 7, NULL, 10);
                        else if (g_str_has_prefix(*p, "limit="))
                            limit = (gint)g_ascii_strtoll(*p + 6, NULL, 10);
                    }
                    g_strfreev(pairs);
                }
                if (limit > 0)
                    _add_pagination_headers(msg, total, offset, limit, req_path);
            }
        }
    } else if (json_object_has_member(obj, "error")) {
        /* 에러 경로: JSON-RPC error 코드를 HTTP 상태 코드로 매핑 */
        JsonObject *e    = json_object_get_object_member(obj, "error");
        gint64      code = json_object_get_int_member_with_default(e, "code", -32000);
        JsonNode   *enode = json_object_get_member(obj, "error");
        gchar *r = json_to_string(enode, FALSE);
        body_str = g_strdup_printf("{\"error\":%s}", r);
        g_free(r);
        /* JSON-RPC 에러 코드 → HTTP 상태 코드 매핑 */
        if      (code == -32602) status = 400;   /* Invalid params */
        else if (code == -32601) status = 404;   /* Method not found */
        else if (code == -32001) status = 404;   /* VM not found (custom) */
        else if (code == -32003) status = 504;   /* RPC timeout (A-3) */
        else if (code == -32006) status = 403;   /* Permission denied */
        else                     status = 500;
    } else {
        body_str = g_strdup(rpc_resp);
        status   = 200;
    }

    _send_json(msg, status, body_str);
    g_free(body_str);
    g_object_unref(p);
}

/* ═══════════════════════════════════════════════════════════════
 * JWT 인증 미들웨어
 * ═══════════════════════════════════════════════════════════════ */
/**
 * JWT 인증 미들웨어 — Authorization 헤더에서 Bearer 토큰을 추출하여 검증
 *
 * REST API의 인증 흐름:
 *   1. 클라이언트가 POST /auth/token으로 JWT 발급
 *   2. 이후 요청에 "Authorization: Bearer <token>" 헤더 포함
 *   3. 이 함수에서 pcv_jwt_verify()로 토큰 검증
 *   4. 성공 시 subject(사용자명) 반환, 실패 시 401 응답 후 NULL 반환
 *
 * 호출 패턴:
 *   gchar *subject = _authenticate(msg);
 *   if (!subject) return;  // 401 이미 전송됨
 *   // ... 인증된 요청 처리 ...
 *   g_free(subject);
 *
 * @param msg HTTP 요청 메시지
 * @return 사용자명 문자열 (호출자가 g_free), 실패 시 NULL (401 응답 전송 완료)
 */
static gchar *
_authenticate(SoupServerMessage *msg)
{
    SoupMessageHeaders *req =
        soup_server_message_get_request_headers(msg);

    /* ── API Key 인증: X-API-Key 헤더 우선 검사 ──────────────────
     *
     * [왜 API Key 인증이 JWT와 별도로 존재하는가?]
     * JWT는 로그인(POST /auth/token) → 토큰 발급 → 만료 갱신 과정이 필요합니다.
     * 자동화 스크립트, CI/CD, 모니터링 도구는 이 과정이 번거롭습니다.
     * API Key는 한 번 발급하면 헤더에 넣기만 하면 됩니다.
     *   예: curl -H "X-API-Key: pcv_xxxx" http://host/api/v1/vms
     *
     * [우선순위] API Key > JWT Bearer (API Key가 있으면 JWT 검사 건너뜀)
     * ──────────────────────────────────────────────────────────── */
    const gchar *api_key = soup_message_headers_get_one(req, "X-API-Key");
    if (api_key && *api_key) {
        GError *aerr = nullptr;
        gchar *user = pcv_rbac_verify_api_key(api_key, &aerr);
        if (user) return user;
        /* API key 제공했으나 무효 → 401 */
        GSocketAddress *ra = soup_server_message_get_remote_address(msg);
        gchar *rip = (ra && G_IS_INET_SOCKET_ADDRESS(ra))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)))
            : g_strdup("unknown");
        PCV_LOG_WARN(REST_LOG_DOM, "AUDIT: auth failed (invalid API key) from %s", rip);
        pcv_audit_log("-", "auth.failed",
                      aerr ? aerr->message : "invalid API key", "fail", 401, 0, rip);
        g_free(rip);
        _error(msg, 401, "UNAUTHORIZED",
               aerr ? aerr->message : "Invalid API key");
        if (aerr) g_error_free(aerr);
        return NULL;
    }

    /* ── JWT Bearer 토큰 인증 (기존 경로) ────────────────────── */
    const gchar *auth = soup_message_headers_get_one(req, "Authorization");

    if (!auth || !*auth) {
        GSocketAddress *ra = soup_server_message_get_remote_address(msg);
        gchar *rip = (ra && G_IS_INET_SOCKET_ADDRESS(ra))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)))
            : g_strdup("unknown");
        PCV_LOG_WARN(REST_LOG_DOM, "AUDIT: auth failed (no header) from %s", rip);
        pcv_audit_log("-", "auth.failed", "no Authorization header", "fail", 401, 0, rip);
        g_free(rip);
        _error(msg, 401, "UNAUTHORIZED", "Authorization header required");
        return NULL;
    }

    GError *err     = nullptr;
    gchar  *subject = pcv_jwt_verify(auth, &err);
    if (!subject) {
        GSocketAddress *ra = soup_server_message_get_remote_address(msg);
        gchar *rip = (ra && G_IS_INET_SOCKET_ADDRESS(ra))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)))
            : g_strdup("unknown");
        PCV_LOG_WARN(REST_LOG_DOM, "AUDIT: auth failed (invalid token) from %s", rip);
        pcv_audit_log("-", "auth.failed",
                      err ? err->message : "invalid token", "fail", 401, 0, rip);
        g_free(rip);
        _error(msg, 401, "UNAUTHORIZED",
               err ? err->message : "Invalid token");
        if (err) g_error_free(err);
    }
    return subject;
}

/* ═══════════════════════════════════════════════════════════════
 * 요청 바디 파싱
 * ═══════════════════════════════════════════════════════════════ */
/**
 * HTTP 요청 바디 JSON 파싱
 *
 * POST/PUT 요청의 JSON 바디를 파싱하여 JsonObject로 반환합니다.
 * 빈 바디(GET 요청 등)는 빈 JsonObject를 반환합니다.
 * 파싱 실패 또는 크기 초과(1MB)는 NULL을 반환합니다.
 *
 * 메모리 관리:
 *   반환된 JsonObject는 호출자가 json_object_unref()로 해제해야 합니다.
 *   내부에서 json_object_ref()를 호출하여 JsonParser와 독립적인 수명을 보장합니다.
 *
 * @param msg HTTP 요청 메시지
 * @return JsonObject (호출자가 unref), 파싱 실패 시 NULL
 */
static JsonObject *
_parse_body(SoupServerMessage *msg)
{
    /* libsoup3: get_request_body() → SoupMessageBody*
     * SoupMessageBody.data (gchar*) + .length (gsoffset) 사용 */
    SoupMessageBody *body = soup_server_message_get_request_body(msg);
    if (!body || body->length == 0)
        return json_object_new();

    const gchar *data = body->data;
    gsize        len  = (gsize)body->length;
    if (len > REST_MAX_BODY) return NULL;

    JsonParser *p = json_parser_new();
    GError     *e = nullptr;
    if (!json_parser_load_from_data(p, data, (gssize)len, &e)) {
        if (e) g_error_free(e);
        g_object_unref(p);
        return NULL;
    }

    /* [메모리 소유권 — 주니어 개발자 필독]
     *
     * json_parser_get_root()는 JsonParser가 소유한 JsonNode를 반환합니다.
     * json_node_get_object()는 JsonNode가 소유한 JsonObject를 반환합니다.
     *
     * 문제: g_object_unref(p) 시 JsonParser가 해제되면
     *       JsonNode → JsonObject도 함께 해제됩니다.
     *
     * 해결: json_object_ref()로 참조 카운트를 +1 증가시킵니다.
     *       이렇게 하면 JsonParser 해제 후에도 JsonObject가 살아있습니다.
     *       호출자가 json_object_unref()로 해제해야 합니다.
     *
     * 이것이 GLib/GObject의 "참조 카운팅" 메모리 관리 패턴입니다. */
    JsonNode   *root = json_parser_get_root(p);
    JsonObject *obj  = JSON_NODE_HOLDS_OBJECT(root)
                       ? json_object_ref(json_node_get_object(root))
                       : json_object_new();
    g_object_unref(p);   /* JsonParser 해제 — obj는 ref 덕분에 생존 */
    return obj;
}

/* ═══════════════════════════════════════════════════════════════
 * URL 세그먼트 추출 헬퍼
 * /api/v1/vms/myvm/start → segment[0]="vms", segment[1]="myvm", segment[2]="start"
 * ═══════════════════════════════════════════════════════════════ */
/**
 * URL 경로를 세그먼트 배열로 분리
 *
 * REST API 경로에서 /api/v1 접두사를 제거하고 '/' 기준으로 분할합니다.
 * 예: /api/v1/vms/myvm/start → ["vms", "myvm", "start"]
 *
 * _on_request()에서 segs[0]=resource, segs[1]=name, segs[2]=action,
 * segs[3]=sub 로 매핑하여 HTTP → RPC 메서드 변환에 사용합니다.
 *
 * @param path HTTP 요청 경로 (예: "/api/v1/vms/myvm/start")
 * @return NULL 종료 문자열 배열 (호출자가 g_strfreev()로 해제)
 */
static gchar **
_split_path(const gchar *path)
{
    const gchar *p = path;
    if (g_str_has_prefix(p, REST_API_PREFIX))
        p += strlen(REST_API_PREFIX);
    if (*p == '/') p++;
    return g_strsplit(p, "/", -1);
}

/* ═══════════════════════════════════════════════════════════════
 * 메인 요청 핸들러
 * ═══════════════════════════════════════════════════════════════ */
/**
 * 메인 HTTP 요청 핸들러 — SoupServer의 모든 요청이 이 함수를 거칩니다.
 *
 * HTTP 메서드+경로 조합을 분석하여 대응하는 JSON-RPC 메서드를 결정하고,
 * _rpc_over_uds()로 UDS 소켓에 전달한 후 응답을 HTTP로 변환합니다.
 *
 * 처리 순서 (파이프라인):
 *   1. Rate Limiting 체크 (600 req/min 초과 → 429)
 *   2. 감사 로깅 (POST/PUT/DELETE만)
 *   3. CORS 헤더 설정 (크로스 노드 메트릭 fetch 허용)
 *   4. OPTIONS preflight 처리 (204 응답)
 *   5. 정적 파일 서빙 (/ui/...)
 *   6. 인증 불필요 엔드포인트 (/health, /internal/..., /metrics, /auth/token)
 *   7. JWT 인증 체크
 *   8. 경로 파싱 → RPC 메서드 결정 → _rpc_over_uds() → 응답 변환
 *
 * 경로 매핑 예시:
 *   GET  /api/v1/vms           → vm.list
 *   POST /api/v1/vms           → vm.create
 *   POST /api/v1/vms/web/start → vm.start (name=web)
 *   PUT  /api/v1/vms/web/vcpu  → vm.set_vcpu (name=web)
 *
 * @param server    SoupServer 인스턴스 (미사용)
 * @param msg       HTTP 요청/응답 메시지
 * @param path      요청 경로 (예: "/api/v1/vms")
 * @param query     URL 쿼리 파라미터 (미사용)
 * @param user_data PcvRestServer 인스턴스 (미사용)
 */
/* 응답 전송 완료 후 서버 FIN 전송 — CLOSE-WAIT 방지
 *
 * [문제] libsoup3는 Connection: close 헤더를 보내도 내부적으로 소켓 close를
 *        지연하여 CLOSE-WAIT가 누적 → listen backlog 고갈 → REST 무응답.
 *
 * [시도한 방법과 결과]
 *   1. shutdown(SHUT_WR) — FIN 전송하지만 fd 미닫힘 → CLOSE-WAIT 잔류
 *   2. g_socket_close() — libsoup 내부 상태 파괴 → 후속 요청 hang
 *   3. SO_LINGER=0 — finished 시그널 타이밍 이슈로 RST가 응답 전송 중 발생
 *
 * [현재 해결] shutdown(SHUT_RDWR) — 양방향 종료로 서버 FIN 즉시 전송.
 *   fd는 닫지 않아 libsoup 내부 상태 유지. 커널이 FIN 교환 후 소켓 정리.
 *   SHUT_WR만으로는 부족했던 이유: 클라이언트 FIN을 수신 안 한 상태에서
 *   서버 FIN만 보내면 half-close → CLOSE-WAIT. SHUT_RDWR은 양쪽 모두 닫아
 *   커널이 즉시 CLOSED 전이. */
/**
 * HTTP 응답 전송 완료 시그널 핸들러 — CLOSE-WAIT 소켓 누적 방지
 *
 * [배경 — TCP 4-way handshake]
 * TCP 연결 종료 시 4단계 핸드셰이크가 필요합니다:
 *   1. 서버 → FIN → 클라이언트  (서버가 "보낼 것 없음" 알림)
 *   2. 클라이언트 → ACK → 서버
 *   3. 클라이언트 → FIN → 서버  (클라이언트도 "보낼 것 없음")
 *   4. 서버 → ACK → 클라이언트
 *
 * 서버가 FIN을 보내지 않으면 CLOSE-WAIT 상태로 머무릅니다.
 * CLOSE-WAIT 소켓이 누적되면 listen backlog(동시 대기 큐)가 가득 차서
 * 새 HTTP 연결을 accept할 수 없게 됩니다 → REST API 전체 무응답.
 *
 * [해결]
 * shutdown(SHUT_RDWR)로 양방향 모두 즉시 종료하여 커널이 FIN을 전송합니다.
 * fd를 close()하지 않는 이유: libsoup이 내부적으로 fd를 관리하고 있어서
 * close()하면 libsoup의 내부 상태가 꼬입니다.
 *
 * @param msg       응답 전송이 완료된 HTTP 메시지
 * @param user_data 미사용
 */
static void
_on_msg_finished(SoupServerMessage *msg, gpointer user_data __attribute__((unused)))
{
    GSocket *gsock = soup_server_message_get_socket(msg);
    if (gsock) {
        int fd = g_socket_get_fd(gsock);
        if (fd >= 0) shutdown(fd, SHUT_RDWR);   /* 양방향 종료 → 커널이 FIN 전송 */
    }
}

/* ═══════════════════════════════════════════════════════════════
 * REST 비동기 RPC 실행 — CLOSE-WAIT 근본 해결
 *
 * [구조]
 *   _on_request() → soup_server_message_pause(msg) → GTask 위임 → return (즉시)
 *   [워커 스레드] → _rpc_over_uds() → rest_ctx에서 unpause + 응답 전송
 *
 * [효과]
 *   REST 스레드가 UDS 응답 대기 동안 블로킹되지 않아
 *   새 HTTP 요청을 즉시 accept할 수 있음 → CLOSE-WAIT 누적 방지.
 * ═══════════════════════════════════════════════════════════════ */

/* REST GMainContext — _on_request에서 설정, 비동기 워커에서 사용 */
static GMainContext *self_rest_ctx = nullptr;

/*
 * 비동기 RPC 실행을 위한 컨텍스트 구조체
 *
 * [왜 이 구조체가 필요한가?]
 * _on_request()는 즉시 반환해야 합니다 (REST 스레드 비점유).
 * 하지만 UDS RPC 응답을 받아서 HTTP 응답을 보내야 합니다.
 * 이 구조체에 필요한 정보를 담아 GTask 워커 스레드로 전달합니다.
 *
 * [소유권 규칙 — 메모리 누수 방지]
 * - msg: g_object_ref()로 참조 증가 → 워커가 끝나면 g_object_unref()
 * - rpc: _on_request()에서 생성 → 워커에서 g_free()
 * - vm_delete_name: _on_request()에서 g_strdup() → 워커에서 g_free()
 */
typedef struct {
    SoupServerMessage *msg;           /* HTTP 메시지 (ref 증가됨 — GObject 참조 카운팅) */
    gchar             *rpc;           /* JSON-RPC 요청 문자열 (워커가 해제) */
    gchar             *rpc_method;    /* RPC 메서드명 (BE-A6 타임아웃 결정용, 워커가 해제) */
    gchar             *vm_delete_name;/* vm.delete 시 ZFS zvol 삭제 완료 폴링용 VM 이름 */
    gboolean           is_vm_delete;  /* vm.delete인 경우 TRUE → ZFS 삭제 완료까지 대기 */
    GMainContext      *rest_ctx;      /* REST GMainContext — unpause를 이 context에서 실행 */
} _RestAsyncCtx;

/**
 * _rest_async_ctx_free — GTask destructor for _RestAsyncCtx
 *
 * GTask가 완료/취소될 때 task_data를 자동 해제합니다.
 * 워커 스레드가 정상 완료한 경우 필드는 이미 NULL이므로 이중 해제 안전.
 */
static void
_rest_async_ctx_free(gpointer p)
{
    _RestAsyncCtx *ctx = p;
    if (!ctx) return;
    g_free(ctx->rpc);
    g_free(ctx->rpc_method);
    g_free(ctx->vm_delete_name);
    if (ctx->msg) g_object_unref(ctx->msg);
    g_free(ctx);
}

/*
 * unpause + 응답 전송을 REST GMainContext에서 실행 (GSource callback)
 *
 * [왜 REST GMainContext에서 실행해야 하는가?]
 * soup_server_message_unpause()는 SoupServer가 소속된 GMainContext에서만
 * 호출해야 합니다 (스레드 안전 요구사항). 워커 스레드에서 직접 호출하면
 * libsoup 내부 상태가 꼬여 크래시가 발생합니다.
 * g_idle_source_new() + g_source_attach()로 REST context에 콜백을 예약합니다.
 */
typedef struct {
    SoupServerMessage *msg;   /* HTTP 메시지 — unpause 후 응답 전송 대상 */
    gchar *resp;              /* UDS에서 수신한 JSON-RPC 응답 문자열 */
} _RestUnpauseData;

/**
 * REST GMainContext에서 실행되는 unpause 콜백
 *
 * [실행 흐름]
 * 1. _send_rpc_result()로 HTTP 응답 본문 설정
 * 2. soup_server_message_unpause()로 보류된 HTTP 응답 전송
 * 3. msg 참조 해제 + 메모리 정리
 *
 * G_SOURCE_REMOVE 반환: 이 콜백은 1회만 실행 (반복 불필요)
 */
static gboolean
_rest_unpause_cb(gpointer data)
{
    _RestUnpauseData *ud = data;
    _send_rpc_result(ud->msg, ud->resp);     /* HTTP 응답 본문 설정 */
    soup_server_message_unpause(ud->msg);     /* 보류된 HTTP 응답을 클라이언트에 전송 */
    g_object_unref(ud->msg);                  /* 참조 카운트 감소 (0이면 해제) */
    g_free(ud->resp);
    g_free(ud);
    return G_SOURCE_REMOVE;   /* GLib: FALSE = 이 소스를 제거 (1회 실행) */
}

/**
 * GTask 워커 스레드 함수 — UDS 블로킹 I/O가 여기서 발생
 *
 * [전체 비동기 흐름 — 주니어 개발자 필독]
 *
 *   1. _on_request() [REST 스레드]
 *      → soup_server_message_pause(msg)  // HTTP 응답 보류
 *      → GTask 생성 + 워커 스레드에 위임
 *      → 즉시 return  // REST 스레드가 다음 요청을 받을 수 있음
 *
 *   2. _rest_async_worker() [이 함수, GTask 워커 스레드]
 *      → _rpc_over_uds()  // UDS 소켓에 JSON-RPC 전송 + 응답 대기 (최대 2초 블로킹)
 *      → g_idle_source로 _rest_unpause_cb를 REST context에 예약
 *
 *   3. _rest_unpause_cb() [REST 스레드, GMainLoop idle 시]
 *      → _send_rpc_result()  // HTTP 응답 본문 설정
 *      → soup_server_message_unpause()  // HTTP 응답 전송
 *
 * [왜 이렇게 복잡한가?]
 * soup_server_message_unpause()는 REST GMainContext 스레드에서만 호출 가능합니다.
 * 하지만 _rpc_over_uds()는 블로킹이라 REST 스레드에서 직접 실행하면 안 됩니다.
 * 따라서 블로킹은 워커 스레드, unpause는 REST 스레드로 분리합니다.
 *
 * @param task      GTask 인스턴스 (GLib 비동기 태스크 프레임워크)
 * @param src       소스 객체 (미사용)
 * @param task_data _RestAsyncCtx 포인터
 * @param cancel    GCancellable (미사용)
 */
static void
_rest_async_worker(GTask *task, gpointer src __attribute__((unused)),
                   gpointer task_data, GCancellable *cancel __attribute__((unused)))
{
    _RestAsyncCtx *actx = task_data;

    /* UDS RPC 실행 (블로킹 — 이 워커 스레드에서만 대기, REST 스레드는 비점유)
     * BE-A6: 메서드별 타임아웃 적용 — 장기 실행 메서드(cluster/cloud/zfs)는 확대 */
    gint timeout = pcv_get_rpc_timeout(actx->rpc_method);
    gchar *resp = _rpc_over_uds_timeout(actx->rpc, timeout);

    /* vm.delete — 즉시 반환 (MEDIUM-12 수정)
     *
     * 이전: ZFS zvol 삭제 완료까지 워커 스레드에서 폴링 (최대 15초 블로킹)
     *       → GLib 스레드풀 슬롯 고갈 위험
     * 이후: "accepted" 응답을 즉시 클라이언트에 전달
     *       → 클라이언트는 vm.delete.status RPC 또는 GET /vms/{n}/delete-status로
     *         비동기 삭제 진행 상태를 폴링합니다.
     */

    /* REST GMainContext에서 unpause + 응답 전송 (스레드 안전)
     *
     * [어떻게 다른 스레드에서 안전하게 함수를 실행하는가?]
     * GLib의 GSource 메커니즘을 사용합니다:
     * 1. g_idle_source_new(): "유휴 시 실행" 소스 생성
     * 2. g_source_set_callback(): 실행할 콜백 함수 설정
     * 3. g_source_attach(src, ctx): REST GMainContext에 소스 등록
     *    → REST 스레드의 GMainLoop가 idle 상태일 때 _rest_unpause_cb 호출
     * 4. g_source_unref(): 소스 참조 해제 (attach가 참조를 보유)
     *
     * [왜 이렇게 하는가?]
     * 워커 스레드에서 직접 soup_server_message_unpause()를 호출하면
     * libsoup 내부에서 레이스 컨디션이 발생합니다. 반드시 REST 스레드에서 호출해야 합니다.
     */
    _RestUnpauseData *ud = g_new0(_RestUnpauseData, 1);
    ud->msg = actx->msg;   /* ref는 actx 생성 시 이미 증가됨 (소유권 이전) */
    ud->resp = resp;        /* 소유권 이전 — ud가 해제 책임 */

    GSource *src2 = g_idle_source_new();
    g_source_set_callback(src2, _rest_unpause_cb, ud, NULL);
    g_source_attach(src2, actx->rest_ctx);
    g_source_unref(src2);

    /* actx 정리 (msg 참조는 ud로 이전, resp도 ud로 이전)
     * 소유권 이전된 필드를 NULL로 설정하여 GTask destructor(_rest_async_ctx_free)에서
     * 이중 해제를 방지합니다. */
    g_free(actx->rpc);           actx->rpc = NULL;
    g_free(actx->rpc_method);    actx->rpc_method = NULL;
    g_free(actx->vm_delete_name);actx->vm_delete_name = NULL;
    actx->msg = NULL;  /* msg 소유권은 ud로 이전 — destructor에서 unref 방지 */
}

/**
 * JSON object의 문자열 member를 안전하게 복사합니다.
 *
 * [왜 별도 함수인가?]
 * json_object_get_string_member()는 member가 없거나 문자열이 아니면 경고를 낼 수
 * 있습니다. 공개 데모 health 파일은 운영 스크립트가 만들지만, 파일 손상이나 수동
 * 수정까지 고려해 REST 서버는 "경고 없이 stale 응답"을 돌려야 합니다.
 */
static gchar *
_json_string_member_dup(JsonObject *obj, const gchar *member)
{
    if (!obj || !json_object_has_member(obj, member))
        return NULL;

    JsonNode *node = json_object_get_member(obj, member);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return NULL;

    if (json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;

    return g_strdup(json_node_get_string(node));
}

/**
 * ISO-8601 timestamp가 공개 데모 freshness 기준을 넘었는지 확인합니다.
 *
 * 비개발자 관점에서는 "마지막 점검 영수증이 5분보다 오래됐는지" 확인하는
 * 함수입니다. 오래된 성공 결과를 live 상태처럼 보여주지 않기 위해 사용합니다.
 */
static gboolean
_iso8601_is_stale(const gchar *checked_at, gint64 now_us)
{
    if (!checked_at || !*checked_at)
        return TRUE;

    GDateTime *dt = g_date_time_new_from_iso8601(checked_at, NULL);
    if (!dt)
        return TRUE;

    gint64 checked_us = g_date_time_to_unix(dt) * G_USEC_PER_SEC;
    gint64 age_us = now_us - checked_us;
    g_date_time_unref(dt);

    return age_us < 0 ||
           age_us > ((gint64)PCV_OVN_DEMO_HEALTH_STALE_SEC * G_USEC_PER_SEC);
}

/**
 * GET /api/v1/demo/ovn-ovs/health 응답을 생성합니다.
 *
 * 이 endpoint는 public demo를 위한 read-only 관찰면입니다. 보안 경계를 작게
 * 유지하기 위해 RPC, shell command, VM 조작을 절대 실행하지 않고 host에 이미
 * 저장된 JSON 상태 파일만 읽습니다.
 */
static void
_send_ovn_demo_health(SoupServerMessage *msg)
{
    gchar *content = NULL;
    gsize len = 0;
    GError *err = NULL;

    if (!g_file_get_contents(PCV_OVN_DEMO_HEALTH_PATH, &content, &len, &err)) {
        gchar *safe = err ? g_strescape(err->message, NULL)
                          : g_strdup("state file unavailable");
        gchar *body = g_strdup_printf(
            "{\"data\":{\"demo\":\"ovn-ovs\",\"status\":\"stale\",\"stale\":true,"
            "\"reason\":\"state_file_unavailable\",\"detail\":\"%s\","
            "\"path\":\"%s\",\"stale_after_sec\":%d}}",
            safe, PCV_OVN_DEMO_HEALTH_PATH, PCV_OVN_DEMO_HEALTH_STALE_SEC);
        _send_json(msg, 200, body);
        g_free(body);
        g_free(safe);
        g_clear_error(&err);
        return;
    }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, content, (gssize)len, &err)) {
        gchar *safe = err ? g_strescape(err->message, NULL)
                          : g_strdup("invalid json");
        gchar *body = g_strdup_printf(
            "{\"data\":{\"demo\":\"ovn-ovs\",\"status\":\"stale\",\"stale\":true,"
            "\"reason\":\"invalid_state_json\",\"detail\":\"%s\","
            "\"path\":\"%s\",\"stale_after_sec\":%d}}",
            safe, PCV_OVN_DEMO_HEALTH_PATH, PCV_OVN_DEMO_HEALTH_STALE_SEC);
        _send_json(msg, 200, body);
        g_free(body);
        g_free(safe);
        g_clear_error(&err);
        g_object_unref(parser);
        g_free(content);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = root && JSON_NODE_HOLDS_OBJECT(root)
                    ? json_node_get_object(root)
                    : NULL;
    if (!obj) {
        gchar *body = g_strdup_printf(
            "{\"data\":{\"demo\":\"ovn-ovs\",\"status\":\"stale\",\"stale\":true,"
            "\"reason\":\"state_root_not_object\",\"path\":\"%s\","
            "\"stale_after_sec\":%d}}",
            PCV_OVN_DEMO_HEALTH_PATH, PCV_OVN_DEMO_HEALTH_STALE_SEC);
        _send_json(msg, 200, body);
        g_free(body);
        g_object_unref(parser);
        g_free(content);
        return;
    }

    gchar *checked_at = _json_string_member_dup(obj, "checked_at");
    gboolean stale = _iso8601_is_stale(checked_at, g_get_real_time());
    json_object_set_boolean_member(obj, "stale", stale);
    json_object_set_int_member(obj, "stale_after_sec",
                               PCV_OVN_DEMO_HEALTH_STALE_SEC);
    if (stale) {
        json_object_set_string_member(obj, "status", "stale");
        if (!json_object_has_member(obj, "reason"))
            json_object_set_string_member(obj, "reason", "state_file_stale");
    }

    JsonNode *data_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(data_node, obj);
    JsonObject *envelope = json_object_new();
    json_object_set_member(envelope, "data", data_node);

    JsonNode *envelope_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(envelope_node, envelope);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, envelope_node);
    gchar *body = json_generator_to_data(gen, NULL);

    _send_json(msg, 200, body);

    g_free(body);
    g_object_unref(gen);
    json_node_free(envelope_node);
    g_free(checked_at);
    g_object_unref(parser);
    g_free(content);
}

static void
_on_request(SoupServer        *server   __attribute__((unused)),
            SoupServerMessage *msg,
            const gchar       *path,
            GHashTable        *query,
            gpointer           user_data __attribute__((unused)))
{
    const gchar *method = soup_server_message_get_method(msg);
    gchar *req_id = nullptr;              /* X-Request-ID — cleanup: 레이블에서 해제 */
    PcvTraceContext *trace_ctx = nullptr;  /* W3C Trace Context (C-7) — cleanup에서 해제 */

    /* 모든 응답에 Connection: close — HTTP keep-alive 비활성화.
     * keep-alive 연결이 CLOSE-WAIT으로 잔류하면 listen backlog 고갈 → REST 무응답.
     * _on_request 진입점에서 설정하여 모든 경로(JSON, 정적파일, 리다이렉트) 포함. */
    soup_message_headers_replace(
        soup_server_message_get_response_headers(msg), "Connection", "close");

    /* 응답 전송 완료 후 서버 측 소켓 즉시 shutdown — CLOSE-WAIT 방지.
     * libsoup이 Connection: close를 보내도 내부적으로 소켓 close를 지연하여
     * CLOSE-WAIT 누적 → backlog 소진. finished 시그널에서 shutdown(SHUT_WR)로
     * 서버 FIN을 즉시 전송하면 CLOSE-WAIT → LAST-ACK → CLOSED 빠르게 전이. */
    /* [어떻게?] GLib 시그널 메커니즘으로 "finished" 이벤트에 콜백을 연결합니다.
     * libsoup이 HTTP 응답 전송을 완료하면 "finished" 시그널을 발신하고,
     * _on_msg_finished()가 호출되어 소켓을 shutdown합니다.
     * 이벤트 기반 프로그래밍: "언제 일어날지 모르지만, 일어나면 이것을 해라" */
    g_signal_connect(msg, "finished", G_CALLBACK(_on_msg_finished), NULL);

    /*
     * ── Rate Limiting (고정 윈도우, Per-IP + Per-Endpoint) ─────
     *
     * 클라이언트 IP + endpoint bucket별로 1분(60초) 윈도우 내 요청 수를 카운팅합니다.
     * BE-A4: 엔드포인트별 Rate Limit 티어 적용 — 인증(60), 모니터링(3600),
     * VM 생성(120), 기본(600) req/min.
     *
     * GHashTable로 버킷별 카운터를 관리하며, 윈도우 리셋 시 전체 초기화.
     * GMutex로 멀티스레드 콜백 환경에서의 레이스 컨디션을 방지합니다.
     * 최대 1024 버킷 추적 — 초과 시 오래된 일부 항목을 제거하여 메모리 무한 증가 방지.
     */
    {
        static GMutex  rate_mu;
        static GHashTable *rate_counts = nullptr;  /* "IP:bucket" → GINT_TO_POINTER(count) */
        static gint64  window_start = 0;
        g_mutex_lock(&rate_mu);
        if (!rate_counts)
            rate_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
        if (now - window_start > 60) {
            g_hash_table_remove_all(rate_counts);
            window_start = now;
        }
        /* 클라이언트 IP 추출 */
        GSocketAddress *rate_remote = soup_server_message_get_remote_address(msg);
        gchar *rate_ip = nullptr;
        if (rate_remote && G_IS_INET_SOCKET_ADDRESS(rate_remote)) {
            GInetAddress *inet = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(rate_remote));
            rate_ip = g_inet_address_to_string(inet);
        }
        if (!rate_ip) rate_ip = g_strdup("unknown");
        gchar *rate_key = pcv_build_rate_limit_key(rate_ip, path, method);
        gint count = GPOINTER_TO_INT(g_hash_table_lookup(rate_counts, rate_key)) + 1;
        g_hash_table_insert(rate_counts, g_strdup(rate_key), GINT_TO_POINTER(count));
        /* 메모리 보호: 추적 버킷이 1024개 초과 시 가장 오래된 50% 제거 (LRU 근사) */
        if (g_hash_table_size(rate_counts) > 1024) {
            GHashTableIter evict_iter;
            gpointer evict_key, evict_val;
            gint removed = 0;
            gint target = (gint)g_hash_table_size(rate_counts) / 2;
            g_hash_table_iter_init(&evict_iter, rate_counts);
            while (g_hash_table_iter_next(&evict_iter, &evict_key, &evict_val) && removed < target) {
                g_hash_table_iter_remove(&evict_iter);
                removed++;
            }
            PCV_LOG_WARN(REST_LOG_DOM,
                "Rate limit table overflow: evicted %d/%d bucket entries (LRU approximate)",
                removed, removed + (gint)g_hash_table_size(rate_counts));
            /* 현재 버킷의 카운트가 제거되었을 수 있으므로 재조회 */
            count = GPOINTER_TO_INT(g_hash_table_lookup(rate_counts, rate_key)) + 1;
            g_hash_table_insert(rate_counts, g_strdup(rate_key), GINT_TO_POINTER(count));
        }
        g_mutex_unlock(&rate_mu);
        /* BE-A4: 엔드포인트별 Rate Limit 티어 적용 */
        gint endpoint_limit = pcv_get_endpoint_rate_limit(path, method);
        if (count > endpoint_limit) {
            PCV_LOG_WARN(REST_LOG_DOM,
                "Rate limit exceeded for %s on %s (%d req/min, limit=%d, key=%s)",
                rate_ip, path, count, endpoint_limit, rate_key);
            g_free(rate_key);
            g_free(rate_ip);
            _error(msg, 429, "RATE_LIMIT", "Too many requests");
            return;
        }
        g_free(rate_key);
        g_free(rate_ip);
    }

    /* ── Per-User Rate Limiting (BE-1, 인증 사용자별 1200 req/min) ──
     *
     * IP 기반 제한 통과 후, 인증된 사용자별로 추가 rate limit을 적용합니다.
     * 하나의 사용자가 여러 IP에서 대량 요청을 보내는 경우를 방어합니다.
     * 인증 불필요 엔드포인트는 subject가 NULL이므로 자동 건너뜁니다.
     * ──────────────────────────────────────────────────────────── */
    {
        static GMutex      _user_rl_mu;
        static GHashTable *_user_rl_map   = nullptr;  /* username → GINT_TO_POINTER(count) */
        static gint64      _user_rl_start = 0;
#define USER_RATE_LIMIT  1200

        /* 사전 인증 체크: JWT Authorization 헤더에서 사용자명 추출 (경량)
         * _authenticate()는 나중에 호출되므로, 여기서는 헤더만 빠르게 확인합니다.
         * 토큰이 무효해도 rate limit만 판단하고 이후 정식 인증에서 401 반환. */
        SoupMessageHeaders *rl_reqh = soup_server_message_get_request_headers(msg);
        const gchar *rl_auth = soup_message_headers_get_one(rl_reqh, "Authorization");
        gchar *rl_user = nullptr;
        if (rl_auth && g_str_has_prefix(rl_auth, "Bearer ")) {
            GError *rl_err = nullptr;
            rl_user = pcv_jwt_verify(rl_auth, &rl_err);
            if (rl_err) g_error_free(rl_err);
        }

        if (rl_user && *rl_user) {
            g_mutex_lock(&_user_rl_mu);
            if (!_user_rl_map)
                _user_rl_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, NULL);
            gint64 now_u = g_get_monotonic_time() / G_USEC_PER_SEC;
            if (now_u - _user_rl_start > 60) {
                g_hash_table_remove_all(_user_rl_map);
                _user_rl_start = now_u;
            }
            gint ucount = GPOINTER_TO_INT(
                g_hash_table_lookup(_user_rl_map, rl_user)) + 1;
            g_hash_table_insert(_user_rl_map, g_strdup(rl_user),
                                GINT_TO_POINTER(ucount));
            /* 메모리 보호: 추적 사용자 512명 초과 시 리셋 */
            if (g_hash_table_size(_user_rl_map) > 512) {
                g_hash_table_remove_all(_user_rl_map);
                ucount = 1;
            }
            g_mutex_unlock(&_user_rl_mu);
            if (ucount > USER_RATE_LIMIT) {
                PCV_LOG_WARN(REST_LOG_DOM,
                    "Per-user rate limit exceeded for '%s' (%d req/min)",
                    rl_user, ucount);
                g_free(rl_user);
                _error(msg, 429, "RATE_LIMIT",
                    "Per-user rate limit exceeded");
                return;
            }
        }
        g_free(rl_user);
#undef USER_RATE_LIMIT
    }

    /* ── 감사 로깅 (변경 작업만 — IP + 경로 + 바디 요약) ───── */
    if (g_strcmp0(method, "POST") == 0 || g_strcmp0(method, "PUT") == 0 ||
        g_strcmp0(method, "DELETE") == 0) {
        GSocketAddress *remote = soup_server_message_get_remote_address(msg);
        gchar *remote_str = (remote && G_IS_INET_SOCKET_ADDRESS(remote))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(remote)))
            : g_strdup("unknown");
        /* 바디 요약: 민감 경로(/auth/token)는 마스킹, 나머지는 앞 200자 기록 */
        SoupMessageBody *audit_body = soup_server_message_get_request_body(msg);
        const gchar *body_str = audit_body ? audit_body->data : NULL;
        gsize body_len = audit_body ? (gsize)audit_body->length : 0;
        gboolean is_auth = (g_strstr_len(path, -1, "/auth/token") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/refresh") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/logout") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/register") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/password") != nullptr);
        if (body_str && body_len > 0 && !is_auth) {
            gchar *truncated = g_strndup(body_str, MIN(body_len, 200));
            PCV_LOG_INFO(REST_LOG_DOM, "AUDIT: %s %s from %s body=%.200s",
                         method, path, remote_str, truncated);
            g_free(truncated);
        } else {
            PCV_LOG_INFO(REST_LOG_DOM, "AUDIT: %s %s from %s%s",
                         method, path, remote_str,
                         is_auth ? " body=(credentials masked)" : "");
        }
        g_free(remote_str);
    }

    /* ── X-Request-ID 생성 + 응답 헤더 + 로그 TLS 설정 ──────────
     * 모든 REST 요청에 고유 Request ID를 부여합니다.
     * 1) 응답 헤더 X-Request-ID로 클라이언트에 반환
     * 2) pcv_log_req_id_set()으로 TLS 설정 → 이후 로그에 자동 포함
     * 3) 요청 로그에 req_id를 명시하여 추적 가능
     * ──────────────────────────────────────────────────────────── */
    req_id = pcv_generate_request_id();
    {
        SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(rh, "X-Request-ID", req_id);
    }
    pcv_log_req_id_set(req_id);

    /* ── W3C Trace Context 전파 (C-7) ───────────────────────────
     * 수신 traceparent 헤더가 있으면 기존 trace chain에 참여하고,
     * 없으면 새 trace를 시작합니다. 응답에 항상 traceparent를 설정.
     * ──────────────────────────────────────────────────────────── */
    {
        SoupMessageHeaders *reqh_tp = soup_server_message_get_request_headers(msg);
        const gchar *incoming_tp = soup_message_headers_get_one(reqh_tp, "traceparent");
        trace_ctx = incoming_tp ? pcv_trace_context_parse(incoming_tp)
                                : NULL;
        if (!trace_ctx)
            trace_ctx = pcv_trace_context_new();
        gchar *tp_out = pcv_trace_context_format(trace_ctx);
        SoupMessageHeaders *rh_tp = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(rh_tp, "traceparent", tp_out);
        g_free(tp_out);
    }

    PCV_LOG_INFO(REST_LOG_DOM, "[%s] [trace=%s span=%s] %s %s",
                 req_id, trace_ctx->trace_id, trace_ctx->span_id, method, path);

    /*
     * ── CORS (Cross-Origin Resource Sharing) ────────────────────
     *
     * Web UI(http://<node>:8080/ui/)에서 다른 노드의 REST API를 fetch할 때
     * 브라우저가 CORS 정책으로 차단하지 않도록 허용 헤더를 설정합니다.
     *
     * Allow-Origin: "*" — 모든 도메인에서 접근 허용 (내부 클러스터용)
     * Allow-Methods: 사용하는 HTTP 메서드 전체 허용
     * Allow-Headers: JWT Authorization 헤더와 JSON Content-Type 허용
     *
     * OPTIONS preflight 요청:
     *   브라우저는 PUT/DELETE 등 "비단순 요청" 전에 OPTIONS 메서드로
     *   사전 확인(preflight)을 합니다. 204 No Content로 즉시 응답하여
     *   실제 요청을 허용합니다.
     */
    /* ── CORS 출처 화이트리스트 ──────────────────────────────────
     * 요청의 Origin 헤더를 검사하여 허용된 출처만 응답합니다.
     * 허용 기준:
     *   1) Origin이 없는 경우 (같은 출처, curl 등) → 허용
     *   2) 로컬 주소 (localhost, 127.0.0.1, 192.168.x.x, 10.x.x.x) → 허용
     *   3) 요청 Host와 동일한 Origin → 허용
     * 그 외는 CORS 헤더를 설정하지 않아 브라우저가 차단합니다.
     * ──────────────────────────────────────────────────────────── */
    {
        SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
        SoupMessageHeaders *reqh = soup_server_message_get_request_headers(msg);
        const gchar *origin = soup_message_headers_get_one(reqh, "Origin");
        gboolean allow = FALSE;

        if (!origin || origin[0] == '\0') {
            allow = TRUE;  /* 같은 출처 요청 또는 curl */
        } else if (strstr(origin, "://localhost") ||
                   strstr(origin, "://127.0.0.1") ||
                   strstr(origin, "://192.168.") ||
                   strstr(origin, "://10.")) {
            allow = TRUE;  /* 내부 네트워크 */
        } else {
            /* Host 헤더와 Origin 비교 */
            const gchar *host = soup_message_headers_get_one(reqh, "Host");
            if (host && strstr(origin, host)) allow = TRUE;
        }

        /* [Security Note] All response headers are hardcoded or from validated sources.
         * Never insert user-controlled data into response headers without CRLF sanitization.
         * libsoup3's soup_message_headers_replace() rejects headers containing \r\n. */
        if (allow) {
            soup_message_headers_replace(rh, "Access-Control-Allow-Origin",
                                          origin ? origin : "*");
            soup_message_headers_replace(rh, "Access-Control-Allow-Methods",
                                          "GET, POST, PUT, DELETE, OPTIONS");
            soup_message_headers_replace(rh, "Access-Control-Allow-Headers",
                                          "Authorization, Content-Type, X-API-Key");
            soup_message_headers_replace(rh, "Access-Control-Allow-Credentials", "true");
        }
    }
    if (g_strcmp0(method, "OPTIONS") == 0) {
        soup_server_message_set_status(msg, 204, NULL);  // 204 No Content — preflight 응답
        goto cleanup;
    }

    /*
     * ── GET /ui/... 정적 파일 서빙 (Phase 2 A) ─────────────────
     *
     * Web UI(HTML/JS/CSS)를 직접 서빙합니다. 별도 웹 서버(nginx 등) 없이
     * 현재 에디션 데몬 단일 프로세스에서 API + UI를 모두 제공합니다.
     *
     * 파일 위치: /usr/local/share/purecvisor/ui/
     * 보안: ".." 경로 순회(path traversal) 공격을 차단합니다.
     * Content-Type: 확장자 기반 자동 결정 (.js, .css, .json, .png 등)
     */
    /* ── GET / → 302 /ui/ 리다이렉트 ──────────────────────── */
    if (g_strcmp0(path, "/") == 0) {
        soup_server_message_set_redirect(msg, 302, "/ui/");
        return;
    }

    if (g_str_has_prefix(path, "/ui")) {
        const gchar *file_path = path + 3;  /* "/ui" 접두사 제거 → 상대 경로 */
        if (!file_path[0] || g_strcmp0(file_path, "/") == 0)
            file_path = "/index.html";

        /* 경로 순회 차단 — URL-encoded %2e%2e も含めた realpath 検証 */
        if (strstr(file_path, "..")) {
            _error(msg, 403, "FORBIDDEN", "Path traversal blocked");
            return;
        }

        static const char *ui_base_dir = "/usr/local/share/purecvisor/ui";
        /* base_dir 자체를 1회 정규화 (심링크 캐싱) */
        static char ui_base_resolved[PATH_MAX] = {0};
        static size_t ui_base_len = 0;
        if (ui_base_resolved[0] == '\0') {
            if (!realpath(ui_base_dir, ui_base_resolved)) {
                _error(msg, 500, "SERVER_ERROR", "UI base dir missing");
                return;
            }
            ui_base_len = strlen(ui_base_resolved);
        }

        gchar *full = g_strdup_printf("%s%s", ui_base_dir, file_path);

        /* realpath()로 심볼릭 링크/URL 인코딩 우회 차단 */
        char resolved[PATH_MAX];
        if (!realpath(full, resolved)) {
            int e = errno;
            _error(msg, e == ENOENT ? 404 : 403,
                   e == ENOENT ? "NOT_FOUND" : "FORBIDDEN",
                   e == ENOENT ? "UI file not found" : "Path traversal blocked");
            g_free(full);
            return;
        }
        /* 경계 문자(/) 검사 포함한 prefix 검증 — ui-evil 우회 차단 */
        if (strncmp(resolved, ui_base_resolved, ui_base_len) != 0 ||
            (resolved[ui_base_len] != '/' && resolved[ui_base_len] != '\0')) {
            _error(msg, 403, "FORBIDDEN", "Path traversal blocked");
            g_free(full);
            return;
        }

        gchar *content = nullptr;
        gsize len = 0;

        if (g_file_get_contents(resolved, &content, &len, NULL)) {
            const gchar *ct = "text/html; charset=utf-8";
            if (g_str_has_suffix(file_path, ".js"))   ct = "application/javascript";
            if (g_str_has_suffix(file_path, ".css"))  ct = "text/css";
            if (g_str_has_suffix(file_path, ".json")) ct = "application/json";
            if (g_str_has_suffix(file_path, ".md"))   ct = "text/markdown; charset=utf-8";
            if (g_str_has_suffix(file_path, ".png"))  ct = "image/png";
            if (g_str_has_suffix(file_path, ".ico"))  ct = "image/x-icon";
            if (g_str_has_suffix(file_path, ".svg"))  ct = "image/svg+xml";
            if (g_str_has_suffix(file_path, ".webp")) ct = "image/webp";
            if (g_str_has_suffix(file_path, ".woff")) ct = "font/woff";
            if (g_str_has_suffix(file_path, ".woff2")) ct = "font/woff2";

            soup_server_message_set_status(msg, 200, NULL);
            SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
            soup_message_headers_replace(rh, "Cache-Control", "no-cache, must-revalidate");
            soup_server_message_set_response(msg, ct, SOUP_MEMORY_TAKE, content, len);
        } else {
            _error(msg, 404, "NOT_FOUND", "UI file not found");
        }
        g_free(full);
        return;
    }

    /* ══════════════════════════════════════════════════════════════
     * GET /api/v1/health — 심층 헬스 프로브 (인증 불필요)
     *
     * [왜 인증 불필요인가?]
     * keepalived, 로드밸런서, Kubernetes liveness probe 등
     * 외부 모니터링 도구가 인증 없이 서비스 상태를 확인할 수 있어야 합니다.
     *
     * [6개 서브시스템 프로브]
     * 1. libvirt: QEMU 연결 가능 여부 + 응답 지연시간
     * 2. etcd: 클러스터 멤버 TCP 연결 가능 여부
     * 3. disk: 데이터 디렉터리 잔여 용량
     * 4. audit_db: 감사 로그 DB 파일 존재/크기
     * 5. tls: HTTPS 활성화 여부
     * 5b. vm_state_db: VM 상태 DB 존재/크기
     * 5c. cluster: 클러스터 역할(LEADER/FOLLOWER) 및 유지보수 모드
     * 6. capabilities: OVN/DPDK/클러스터 기능 가용 여부
     *
     * [응답 상태 3단계]
     * ok: 모든 서브시스템 정상
     * degraded: 일부 서브시스템 경고 (예: etcd 일부 노드 다운)
     * critical: 핵심 서브시스템 장애 (예: libvirt 연결 불가) → HTTP 503
     *
     * [5초 캐시 — 왜?]
     * libvirt 연결, TCP probe 등은 비용이 큽니다.
     * Prometheus가 15초마다 /health를 호출하므로 5초 캐시로 부하를 줄입니다.
     * static 변수: 함수 호출 간 값이 유지됩니다 (C의 정적 지역 변수).
     * ══════════════════════════════════════════════════════════════ */
    /* ══════════════════════════════════════════════════════════════
     * GET /api/v1/health/recent-errors — 최근 워커 실패 사유 조회 (ADR-0018)
     *
     * 인증 불필요. UI/CLI가 vm.start 등 fire-and-forget 호출 실패 시
     * 즉시 사용자에게 사유를 surface하기 위해 사용한다.
     *
     * 쿼리 파라미터:
     *   vm=<name>  — 특정 VM 대상 필터 (선택, 정확 매칭)
     *   limit=<N>  — 최대 결과 수 (기본 5)
     *
     * 응답: { "data": [{ts, method, target, result, error_code, duration_ms, message}, ...] }
     * ══════════════════════════════════════════════════════════════ */
    if (g_strcmp0(path, REST_API_PREFIX "/health/recent-errors") == 0) {
        const gchar *vm_q = NULL;
        gint lim = 5;
        if (query) {
            vm_q = g_hash_table_lookup(query, "vm");
            const gchar *lim_q = g_hash_table_lookup(query, "limit");
            if (lim_q) {
                gint v = atoi(lim_q);
                if (v > 0 && v <= 50) lim = v;
            }
        }
        JsonArray *failures = pcv_audit_recent_failures(vm_q, lim);
        JsonObject *root = json_object_new();
        JsonNode *fnode = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(fnode, failures);
        json_object_set_member(root, "data", fnode);
        JsonNode *rn = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(rn, root);
        JsonGenerator *gen = json_generator_new();
        json_generator_set_root(gen, rn);
        gchar *body = json_generator_to_data(gen, NULL);
        _send_json(msg, 200, body);
        g_free(body);
        g_object_unref(gen);
        json_node_free(rn);
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/health") == 0) {
        /* [static 변수 — 주니어 개발자 참고]
         * static 지역 변수는 함수가 끝나도 값이 유지됩니다 (전역 변수와 비슷한 수명).
         * 여기서는 캐시 데이터를 저장하여 5초 이내 재요청 시 즉시 반환합니다. */
        static gint64 cache_ts = 0;         /* 마지막 캐시 갱신 시각 (monotonic usec) */
        static gchar *cache_body = nullptr;    /* 캐시된 JSON 응답 본문 */
        static gint   cache_code = 200;     /* 캐시된 HTTP 상태 코드 */
        static GMutex health_mu;            /* 캐시 접근 뮤텍스 (멀티스레드 보호) */
        static gboolean mu_init = FALSE;
        if (!mu_init) { g_mutex_init(&health_mu); mu_init = TRUE; }

        gint64 now = g_get_monotonic_time();
        g_mutex_lock(&health_mu);
        if (cache_body && (now - cache_ts) < 5 * G_USEC_PER_SEC) {
            _send_json(msg, cache_code, cache_body);
            g_mutex_unlock(&health_mu);
            return;
        }
        g_mutex_unlock(&health_mu);

        JsonObject *root = json_object_new();
        JsonObject *checks = json_object_new();
        gboolean critical = FALSE;
        gboolean degraded = FALSE;

        /* 1. libvirt probe — KVM 하이퍼바이저 연결 가능 여부
         *
         * [어떻게?]
         * virConnectOpenReadOnly()로 로컬 QEMU에 연결 시도합니다.
         * 연결 소요 시간(latency_ms)으로 서비스 품질(SLA)을 판정합니다:
         *   < 200ms: "ok"  |  200~500ms: "warn"  |  > 500ms: "critical"
         * 연결 실패 시 critical 플래그 → HTTP 503 반환 */
        {
            JsonObject *c = json_object_new();
            gint64 t0 = g_get_monotonic_time();   /* 마이크로초 단위 모노토닉 시계 */
            virConnectPtr conn = virConnectOpenReadOnly("qemu:///system");
            gint64 dt = (g_get_monotonic_time() - t0) / 1000;
            if (conn) {
                json_object_set_boolean_member(c, "ok", TRUE);
                json_object_set_int_member(c, "latency_ms", dt);
                const gchar *sla = (dt < 200) ? "ok" : (dt < 500) ? "warn" : "critical";
                json_object_set_string_member(c, "sla", sla);
                virConnectClose(conn);
            } else {
                json_object_set_boolean_member(c, "ok", FALSE);
                json_object_set_string_member(c, "error", "connection failed");
                critical = TRUE;
            }
            json_object_set_object_member(checks, "libvirt", c);
        }

        /* 2. etcd probe
         *
         * Single Edge 공개판은 분산 키-값 저장소를 사용하지 않는다.
         * 사용자가 예전 설정 파일에 관련 항목을 남겨 두어도 공개판의
         * capability가 바뀌면 안 되므로 항상 "사용하지 않음"으로 고정한다. */
        {
            JsonObject *c = json_object_new();
            json_object_set_boolean_member(c, "ok", TRUE);
            json_object_set_string_member(c, "note", "single_edge build: distributed metadata store not used");
            json_object_set_object_member(checks, "etcd", c);
        }

        /* 3. disk probe — 데이터 디렉터리 잔여 용량 확인
         *
         * [왜?] 디스크 공간 부족 시 SQLite DB 쓰기, 로그 기록 등이 실패합니다.
         * [어떻게?] statvfs() 시스템콜로 파일시스템 통계를 조회합니다.
         *   f_bavail * f_frsize = 사용 가능한 바이트 수
         *   0.1GB(100MB) 미만이면 critical 판정 */
        {
            JsonObject *c = json_object_new();
            const gchar *db_path = pcv_config_get_string("daemon", "db_path",
                                       "/var/lib/purecvisor/vm_state.db");
            gchar *dir = g_path_get_dirname(db_path);
            struct statvfs sv;
            if (statvfs(dir, &sv) == 0) {
                gdouble avail_gb = (gdouble)sv.f_bavail * sv.f_frsize / (1024.0*1024*1024);
                json_object_set_boolean_member(c, "ok", avail_gb > 0.1);
                json_object_set_double_member(c, "avail_gb",
                    (double)((gint)(avail_gb * 100)) / 100.0);
                if (avail_gb <= 0.1) critical = TRUE;
            } else {
                json_object_set_boolean_member(c, "ok", FALSE);
                json_object_set_string_member(c, "error", "statvfs failed");
                critical = TRUE;
            }
            g_free(dir);
            json_object_set_object_member(checks, "disk", c);
        }

        /* 4. audit_db probe */
        {
            JsonObject *c = json_object_new();
            const gchar *audit_path = "/var/lib/purecvisor/pcv_audit.db";
            struct stat st;
            if (stat(audit_path, &st) == 0) {
                json_object_set_boolean_member(c, "ok", TRUE);
                json_object_set_double_member(c, "size_mb",
                    (double)((gint)(st.st_size / (1024.0*1024) * 10)) / 10.0);
            } else {
                json_object_set_boolean_member(c, "ok", TRUE);
                json_object_set_string_member(c, "note", "no audit db yet");
            }
            json_object_set_object_member(checks, "audit_db", c);
        }

        /* 5. tls */
        {
            JsonObject *c = json_object_new();
            json_object_set_boolean_member(c, "enabled", pcv_tls_is_enabled());
            json_object_set_object_member(checks, "tls", c);
        }

        /* 5b. vm_state_db probe */
        {
            JsonObject *c = json_object_new();
            const gchar *vm_db = pcv_config_get_string("daemon", "db_path",
                                      "/var/lib/purecvisor/vm_state.db");
            struct stat st_vm;
            if (stat(vm_db, &st_vm) == 0) {
                json_object_set_boolean_member(c, "ok", TRUE);
                json_object_set_double_member(c, "size_mb",
                    (double)((gint)(st_vm.st_size / (1024.0*1024) * 10)) / 10.0);
            } else {
                json_object_set_boolean_member(c, "ok", TRUE);
                json_object_set_string_member(c, "note", "no vm_state db yet");
            }
            json_object_set_object_member(checks, "vm_state_db", c);
        }

        /* 5c. edition probe */
        {
            JsonObject *c = json_object_new();
            json_object_set_string_member(c, "mode", "single_edge");
            json_object_set_object_member(checks, "cluster", c);
        }

        /* 6. capabilities */
        {
            JsonObject *cap = json_object_new();
            json_object_set_boolean_member(cap, "ovn", pcv_ovn_is_available());
            json_object_set_boolean_member(cap, "dpdk", pcv_dpdk_is_available());
            json_object_set_boolean_member(cap, "cluster", FALSE);
            json_object_set_object_member(root, "capabilities", cap);
        }

        /* overall status */
        const gchar *status_str = critical ? "critical" : degraded ? "degraded" : "ok";
        gint http_code = critical ? 503 : 200;

        json_object_set_string_member(root, "status", status_str);
        json_object_set_string_member(root, "service", "purecvisorsd");
        json_object_set_string_member(root, "version", PCV_PRODUCT_VERSION);
        json_object_set_string_member(root, "node_name", g_get_host_name());
        json_object_set_int_member(root, "uptime_sec",
            (gint64)(g_get_monotonic_time() / G_USEC_PER_SEC));
        json_object_set_object_member(root, "checks", checks);

        /* serialize */
        JsonNode *jn = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(jn, root);
        gchar *body = json_to_string(jn, FALSE);
        json_node_free(jn);

        /* cache for 5 seconds */
        g_mutex_lock(&health_mu);
        g_free(cache_body);
        cache_body = g_strdup(body);
        cache_code = http_code;
        cache_ts = g_get_monotonic_time();
        g_mutex_unlock(&health_mu);

        _send_json(msg, http_code, body);
        g_free(body);
        return;
    }

    /* ── GET /api/v1/version (인증 불필요) ────────────────────── */
    if (g_strcmp0(path, REST_API_PREFIX "/version") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        gchar *rpc = _build_rpc("daemon.version", NULL);
        gchar *resp = _rpc_over_uds(rpc);
        g_free(rpc);
        if (resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *root = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(root, "result")) {
                    JsonNode *rn = json_object_dup_member(root, "result");
                    gchar *body = json_to_string(rn, FALSE);
                    json_node_unref(rn);
                    _send_json(msg, 200, body);
                    g_free(body);
                } else {
                    _send_json(msg, 200, resp);
                }
            } else {
                _send_json(msg, 200, resp);
            }
            g_object_unref(jp);
            g_free(resp);
        } else {
            _error(msg, 500, "INTERNAL_ERROR", "Version RPC failed");
        }
        return;
    }

    /*
     * ── GET /api/v1/internal/vms (클러스터 프록시 전용, 인증 불필요) ──
     *
     * 클러스터 내부 통신 전용 엔드포인트입니다.
     * cluster.vm.list 핸들러가 각 피어 노드의 이 엔드포인트를 호출하여
     * 전체 클러스터의 VM 목록을 집계합니다.
     *
     * libvirt에 직접 연결하여 VM 목록을 조회합니다 (디스패처 우회).
     * 이유: 디스패처를 거치면 다시 이 REST 서버를 경유하는 순환 호출이 발생합니다.
     *
     * 외부 방화벽에서 /internal/... 경로를 차단하는 것을 권장합니다.
     */
    if (g_strcmp0(path, REST_API_PREFIX "/internal/vms") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        virConnectPtr vconn = virConnectOpen("qemu:///system");  // 로컬 libvirt 연결
        JsonArray *arr = json_array_new();
        if (vconn) {
            virDomainPtr *domains = nullptr;
            int n = virConnectListAllDomains(vconn, &domains,
                VIR_CONNECT_LIST_DOMAINS_ACTIVE | VIR_CONNECT_LIST_DOMAINS_INACTIVE);
            for (int i = 0; i < n; i++) {
                JsonObject *vm = json_object_new();
                const gchar *name = virDomainGetName(domains[i]);
                json_object_set_string_member(vm, "name", name ? name : "?");
                int state = 0;
                virDomainGetState(domains[i], &state, NULL, 0);
                json_object_set_string_member(vm, "state",
                    state == 1 ? "running" : state == 5 ? "shutoff" : "other");
                virDomainInfo info;
                if (virDomainGetInfo(domains[i], &info) == 0) {
                    json_object_set_int_member(vm, "vcpu", info.nrVirtCpu);
                    json_object_set_int_member(vm, "memory_mb", (gint64)info.memory / 1024);
                }
                json_array_add_object_element(arr, vm);
                virDomainFree(domains[i]);
            }
            if (domains) free(domains);
            virConnectClose(vconn);
        }
        JsonNode *node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(node, arr);
        gchar *data = json_to_string(node, FALSE);
        _send_json(msg, 200, data);
        g_free(data);
        json_node_free(node);
        return;
    }

#if PCV_CLUSTER_ENABLED
    /* ── GET /api/v1/internal/cluster/vms  (클러스터 전체 VM, 인증 불필요) ── */
    if (g_strcmp0(path, REST_API_PREFIX "/internal/cluster/vms") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        gchar *rpc = g_strdup("{\"jsonrpc\":\"2.0\",\"method\":\"cluster.vm.list\",\"params\":{},\"id\":\"rest\"}");
        gchar *uds_resp = _rpc_over_uds(rpc);
        g_free(rpc);
        _send_json(msg, 200, uds_resp ? uds_resp : "{\"result\":[]}");
        g_free(uds_resp);
        return;
    }
#endif

    /*
     * ── GET /api/v1/internal/telemetry (스케줄러용 호스트 메트릭, 인증 불필요) ──
     *
     * cluster.vm.create의 스케줄러가 최적 노드를 선택할 때 사용합니다.
     * /proc/stat에서 CPU 사용률, /proc/meminfo에서 메모리, libvirt에서 VM 수를 조회합니다.
     *
     * 반환 예시: {"cpu_percent": 23.5, "mem_total_kb": 65536000, "mem_avail_kb": 40000000, "vm_count": 6}
     *
     * 이전에는 VM 수로 CPU/MEM를 간접 추정했으나, /proc 기반 실시간 조회로 개선했습니다.
     */
    if (g_strcmp0(path, REST_API_PREFIX "/internal/telemetry") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        JsonObject *obj = json_object_new();
        /* CPU — /proc/stat의 첫 줄(전체 CPU)에서 idle 비율을 계산하여 사용률 추정 */
        FILE *cpuf = fopen("/proc/stat", "r");
        if (cpuf) {
            char buf[256];
            if (fgets(buf, sizeof(buf), cpuf)) {
                guint64 u,n,s,id,io,ir,si,st;
                if (sscanf(buf,"cpu %lu %lu %lu %lu %lu %lu %lu %lu",&u,&n,&s,&id,&io,&ir,&si,&st)==8) {
                    guint64 tot=u+n+s+id+io+ir+si+st, idle=id+io;
                    json_object_set_double_member(obj,"cpu_percent",tot>0?100.0*(1.0-(double)idle/(double)tot):0);
                }
            }
            fclose(cpuf);
        }
        /* Memory */
        FILE *memf = fopen("/proc/meminfo", "r");
        if (memf) {
            char line[128]; guint64 val;
            while (fgets(line, sizeof(line), memf)) {
                if (sscanf(line,"MemTotal: %lu kB",&val)==1) json_object_set_int_member(obj,"mem_total_kb",(gint64)val);
                else if (sscanf(line,"MemAvailable: %lu kB",&val)==1) json_object_set_int_member(obj,"mem_avail_kb",(gint64)val);
            }
            fclose(memf);
        }
        /* VM count */
        virConnectPtr tc = virConnectOpen("qemu:///system");
        if (tc) {
            json_object_set_int_member(obj,"vm_count",virConnectNumOfDomains(tc));
            virConnectClose(tc);
        }
        JsonNode *tn = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(tn, obj);
        gchar *td = json_to_string(tn, FALSE);
        _send_json(msg, 200, td);
        g_free(td); json_node_free(tn);
        return;
    }

    /*
     * ── GET /api/v1/metrics (Prometheus Exporter, 인증 불필요) ────
     *
     * Prometheus 서버가 주기적으로 스크레이핑하는 엔드포인트입니다.
     * Content-Type: text/plain; version=0.0.4 (Prometheus text format)
     *
     * 메트릭 수집 흐름:
     *   1. monitor.fleet RPC 호출 → 호스트 + 전체 VM 메트릭 수신
     *   2. JSON 응답을 Prometheus text format으로 변환
     *   3. Prometheus 레지스트리 메트릭(pcv_prom_render) 추가
     *
     * 주요 메트릭:
     *   - purecvisor_host_cpu_percent: 호스트 CPU 사용률
     *   - purecvisor_host_memory_percent: 호스트 메모리 사용률
     *   - purecvisor_vm_vcpu{vm="..."}: VM별 vCPU 수
     *   - purecvisor_vm_memory_used_mb{vm="..."}: VM별 메모리 사용량
     *   - purecvisor_vm_running{vm="..."}: VM 실행 상태 (1=running, 0=stopped)
     *
     * Grafana 연동: Prometheus가 이 엔드포인트를 스크레이핑 → Grafana 대시보드에 표시
     */
    if (g_strcmp0(path, REST_API_PREFIX "/metrics") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        /* B12-C1: /metrics JWT 옵트인 인증.
         * daemon.conf [metrics] auth = none|required (기본 none, 이전 호환).
         *   - none     : 인증 없이 스크레이핑 허용 (기본)
         *   - required : JWT Bearer 또는 X-API-Key 필수, 실패 시 401
         * 내부망에서 외부 스크레이퍼 접근을 차단해야 하는 환경에서 required로 전환. */
        const gchar *m_auth = pcv_config_get_string("metrics", "auth", "none");
        if (g_strcmp0(m_auth, "required") == 0) {
            gchar *subj = _authenticate(msg);
            if (!subj) return;  /* 401 이미 전송 */
            g_free(subj);
        }

        /* monitor.fleet RPC로 최신 호스트+VM 메트릭 수집 */
        gchar *rpc = _build_rpc("monitor.fleet", NULL);
        gchar *resp = _rpc_over_uds(rpc);
        g_free(rpc);

        GString *prom = g_string_new("");
        if (resp) {
            JsonParser *jp = json_parser_new();
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *root = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(root, "result")) {
                    JsonObject *res = json_object_get_object_member(root, "result");

                    /* host 메트릭 */
                    if (json_object_has_member(res, "host")) {
                        JsonObject *h = json_object_get_object_member(res, "host");
                        {
                            gdouble cpu_pct = 0;
                            if (json_object_has_member(h,"cpu_percent")) {
                                cpu_pct = json_object_get_double_member(h,"cpu_percent");
                            } else if (json_object_has_member(h,"cpu_total_ticks") &&
                                       json_object_has_member(h,"cpu_idle_ticks")) {
                                /* Delta 기반 CPU% 계산 */
                                static gdouble prev_total = 0, prev_idle = 0;
                                gdouble total = json_object_get_double_member(h,"cpu_total_ticks");
                                gdouble idle = json_object_get_double_member(h,"cpu_idle_ticks");
                                gdouble dt = total - prev_total;
                                gdouble di = idle - prev_idle;
                                if (dt > 0 && prev_total > 0)
                                    cpu_pct = 100.0 * (1.0 - di / dt);
                                prev_total = total;
                                prev_idle = idle;
                            }
                            g_string_append_printf(prom,
                                "# HELP purecvisor_host_cpu_percent Host CPU utilization\n"
                                "# TYPE purecvisor_host_cpu_percent gauge\n"
                                "purecvisor_host_cpu_percent %.2f\n", cpu_pct);
                        }
                        g_string_append_printf(prom,
                            "# HELP purecvisor_host_memory_percent Host memory utilization\n"
                            "# TYPE purecvisor_host_memory_percent gauge\n"
                            "purecvisor_host_memory_percent %.2f\n",
                            json_object_has_member(h,"mem_percent")
                                ? json_object_get_double_member(h,"mem_percent") : 0);
                        g_string_append_printf(prom,
                            "# HELP purecvisor_host_disk_percent Host disk utilization\n"
                            "# TYPE purecvisor_host_disk_percent gauge\n"
                            "purecvisor_host_disk_percent %.2f\n",
                            json_object_has_member(h,"disk_percent")
                                ? json_object_get_double_member(h,"disk_percent") : 0);
                        if (json_object_has_member(h,"mem_total_gb"))
                            g_string_append_printf(prom,
                                "# HELP purecvisor_host_memory_total_bytes Host total memory\n"
                                "# TYPE purecvisor_host_memory_total_bytes gauge\n"
                                "purecvisor_host_memory_total_bytes %.0f\n",
                                json_object_get_double_member(h,"mem_total_gb") * 1073741824.0);
                        if (json_object_has_member(h,"cpu_temp_c"))
                            g_string_append_printf(prom,
                                "# HELP purecvisor_host_cpu_temp_celsius CPU temperature\n"
                                "# TYPE purecvisor_host_cpu_temp_celsius gauge\n"
                                "purecvisor_host_cpu_temp_celsius %.1f\n",
                                json_object_get_double_member(h,"cpu_temp_c"));
                        if (json_object_has_member(h,"load_1"))
                            g_string_append_printf(prom,
                                "# HELP purecvisor_host_load1 1-minute load average\n"
                                "# TYPE purecvisor_host_load1 gauge\n"
                                "purecvisor_host_load1 %.2f\n",
                                json_object_get_double_member(h,"load_1"));
                    }

                    /* VM별 메트릭 */
                    if (json_object_has_member(res, "fleet")) {
                        JsonArray *fleet = json_object_get_array_member(res, "fleet");
                        guint n = json_array_get_length(fleet);

                        g_string_append(prom,
                            "# HELP purecvisor_vm_vcpu VM vCPU count\n"
                            "# TYPE purecvisor_vm_vcpu gauge\n");
                        for (guint i = 0; i < n; i++) {
                            JsonObject *vm = json_array_get_object_element(fleet, i);
                            const gchar *vn = json_object_get_string_member(vm, "name");
                            g_string_append_printf(prom,
                                "purecvisor_vm_vcpu{vm=\"%s\"} %ld\n",
                                vn, (long)json_object_get_int_member(vm, "vcpu"));
                        }

                        g_string_append(prom,
                            "# HELP purecvisor_vm_memory_max_mb VM max memory MB\n"
                            "# TYPE purecvisor_vm_memory_max_mb gauge\n");
                        for (guint i = 0; i < n; i++) {
                            JsonObject *vm = json_array_get_object_element(fleet, i);
                            const gchar *vn = json_object_get_string_member(vm, "name");
                            g_string_append_printf(prom,
                                "purecvisor_vm_memory_max_mb{vm=\"%s\"} %.0f\n",
                                vn, json_object_get_double_member(vm, "mem_max_mb"));
                        }

                        g_string_append(prom,
                            "# HELP purecvisor_vm_memory_used_mb VM used memory MB\n"
                            "# TYPE purecvisor_vm_memory_used_mb gauge\n");
                        for (guint i = 0; i < n; i++) {
                            JsonObject *vm = json_array_get_object_element(fleet, i);
                            const gchar *vn = json_object_get_string_member(vm, "name");
                            g_string_append_printf(prom,
                                "purecvisor_vm_memory_used_mb{vm=\"%s\"} %.0f\n",
                                vn, json_object_get_double_member(vm, "mem_used_mb"));
                        }

                        g_string_append(prom,
                            "# HELP purecvisor_vm_running VM running state\n"
                            "# TYPE purecvisor_vm_running gauge\n");
                        for (guint i = 0; i < n; i++) {
                            JsonObject *vm = json_array_get_object_element(fleet, i);
                            const gchar *vn = json_object_get_string_member(vm, "name");
                            const gchar *st = json_object_get_string_member(vm, "state");
                            g_string_append_printf(prom,
                                "purecvisor_vm_running{vm=\"%s\"} %d\n",
                                vn, g_strcmp0(st, "RUNNING") == 0 ? 1 : 0);
                        }
                    }
                }
            }
            g_object_unref(jp);
            g_free(resp);
        }

        /* Phase 1 G: Prometheus 레지스트리 메트릭 추가 */
        {
            gchar *reg_metrics = pcv_prom_render();
            if (reg_metrics) {
                g_string_append(prom, "\n# HELP purecvisor_registry PureCVisor Registry Metrics\n");
                g_string_append(prom, reg_metrics);
                g_free(reg_metrics);
            }
        }

        soup_server_message_set_status(msg, 200, NULL);
        SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(hdrs, "Content-Type",
            "text/plain; version=0.0.4; charset=utf-8");
        soup_server_message_set_response(msg,
            "text/plain; version=0.0.4; charset=utf-8",
            SOUP_MEMORY_COPY, prom->str, prom->len);
        g_string_free(prom, TRUE);
        return;
    }

    /*
     * ── POST /api/v1/auth/token (JWT 로그인, 인증 불필요) ─────────
     *
     * 요청: {"username": "admin", "password": "<configured-admin-password>"}
     * 응답: {"access_token": "eyJ...", "token_type": "Bearer", "expires_in": 3600}
     *
     * 인증 흐름:
     *   1. 요청 바디에서 username/password 추출
     *   2. daemon.conf 또는 PURECVISOR_ADMIN_PASSWORD의 bootstrap 관리자와 비교
     *   3. RBAC DB 사용자이면 refresh token 포함 JWT 발급
     *   4. bootstrap 관리자가 아직 RBAC DB에 없으면 최초 복구용 access token 발급
     *   5. 불일치하면 401 반환 + 감사 로그 기록
     *
     * 주의: RBAC auth 모듈과 별개로, REST 진입점의 기본 인증입니다.
     * RBAC 인증(auth.user.create 등)은 별도의 SQLite DB를 사용합니다.
     */
    if (g_strcmp0(path, REST_API_PREFIX "/auth/token") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        JsonObject  *body     = _parse_body(msg);
        const gchar *username_in = body
            ? json_object_get_string_member_with_default(body, "username", "")
            : "";
        const gchar *password_in = body
            ? json_object_get_string_member_with_default(body, "password", "")
            : "";
        gchar *username = g_strdup(username_in ? username_in : "");
        gchar *password = g_strdup(password_in ? password_in : "");

        const gchar *cfg_user = pcv_config_get_admin_user();
        const gchar *cfg_pass = pcv_config_get_admin_password();
        gboolean bootstrap_configured = cfg_user && *cfg_user && cfg_pass && *cfg_pass;

        gchar *token = nullptr;
        GError *err  = nullptr;

        gchar *refresh = nullptr;

        /* B6-M1: IP-based brute force — pre-check */
        GSocketAddress *ra_pre = soup_server_message_get_remote_address(msg);
        gchar *client_ip = (ra_pre && G_IS_INET_SOCKET_ADDRESS(ra_pre))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(
                  G_INET_SOCKET_ADDRESS(ra_pre)))
            : g_strdup("unknown");
        {
            gint ip_lockout = pcv_rbac_get_ip_remaining_lockout(client_ip);
            if (ip_lockout > 0) {
                g_free(client_ip);
                if (body) json_object_unref(body);
                g_free(username);
                g_free(password);
                SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
                gchar retry_buf[32];
                g_snprintf(retry_buf, sizeof(retry_buf), "%d", ip_lockout);
                soup_message_headers_replace(rh, "Retry-After", retry_buf);
                _error(msg, 429, "TOO_MANY_REQUESTS",
                       "Too many failed login attempts from this IP");
                return;
            }
        }

        if (bootstrap_configured &&
            g_strcmp0(username, cfg_user) == 0 &&
            g_strcmp0(password, cfg_pass) == 0)
        {
            /* daemon.conf 관리자 인증 — v2로 refresh token도 발급 */
            token = pcv_rbac_authenticate_v2(username, password, &refresh, &err);
            if (!token &&
                pcv_rest_auth_should_fallback_bootstrap(username,
                                                        password,
                                                        cfg_user,
                                                        cfg_pass,
                                                        err)) {
                /* 최초 부트스트랩 계정만 RBAC 미등록 상태를 JWT 직접 발급으로 복구 */
                g_clear_error(&err);
                token = pcv_jwt_sign(username, 900, &err);
            }
        } else {
            /* RBAC DB 사용자 인증 (v2: access + refresh token) */
            token = pcv_rbac_authenticate_v2(username, password, &refresh, &err);
        }

        if (!token) {
            g_free(refresh);
            pcv_rbac_ip_record_auth_failure(client_ip);  /* B6-M1 */
            pcv_audit_log(username, "auth.failed", "login attempt",
                          "fail", 401, 0, client_ip);
            g_free(client_ip);

            /* [브루트포스 방어]
             * 로그인 실패가 연속으로 누적되면 해당 계정을 일시 잠금합니다.
             * lockout_sec > 0이면 잠금 상태 → 429 Too Many Requests 반환.
             * Retry-After 헤더: 클라이언트에게 몇 초 후 재시도하라고 알림.
             * 이로써 자동화된 비밀번호 대입 공격(brute-force)을 방어합니다. */
            gint lockout_sec = pcv_rbac_get_remaining_lockout(username);
            if (lockout_sec > 0) {
                SoupMessageHeaders *resp_hdrs = soup_server_message_get_response_headers(msg);
                gchar retry_buf[32];
                g_snprintf(retry_buf, sizeof(retry_buf), "%d", lockout_sec);
                soup_message_headers_replace(resp_hdrs, "Retry-After", retry_buf);
                _error(msg, 429, "TOO_MANY_REQUESTS",
                       err ? err->message : "Account locked due to too many failed attempts");
            } else {
                /* B6-W1 (Phase 4): user enumeration 방어 — 항상 동일 에러 메시지.
                 * "user not found" vs "invalid password" 구분 금지. 내부 상세는 journalctl만. */
                if (err) {
                    PCV_LOG_WARN(REST_LOG_DOM, "auth.failed: user=%s reason=%s",
                                 username, err->message);
                }
                _error(msg, 401, "UNAUTHORIZED", "Invalid credentials");
            }
            if (err) g_error_free(err);
            PCV_LOG_WARN(REST_LOG_DOM,
                         "Auth failed for user '%s'", username);
            if (body) json_object_unref(body);
            g_free(username);
            g_free(password);
            return;
        }

        pcv_rbac_ip_record_auth_success(client_ip);  /* B6-M1 */
        g_free(client_ip);

        gchar *resp = nullptr;
        if (refresh) {
            resp = g_strdup_printf(
                "{\"access_token\":\"%s\","
                "\"refresh_token\":\"%s\","
                "\"token_type\":\"Bearer\","
                "\"expires_in\":900,"
                "\"refresh_expires_in\":604800}", token, refresh);
        } else {
            resp = g_strdup_printf(
                "{\"access_token\":\"%s\","
                "\"token_type\":\"Bearer\","
                "\"expires_in\":900}", token);
        }
        _send_json(msg, 200, resp);
        g_free(resp);
        g_free(token);
        g_free(refresh);
        PCV_LOG_INFO(REST_LOG_DOM, "Token issued for '%s'", username);
        if (body) json_object_unref(body);
        g_free(username);
        g_free(password);
        return;
    }

    /*
     * ── POST /api/v1/auth/refresh (Refresh Token → 새 Access Token) ──
     *
     * 요청: {"refresh_token": "abcdef0123..."}
     * 응답: {"access_token": "eyJ...", "refresh_token": "new...",
     *        "token_type": "Bearer", "expires_in": 900, "refresh_expires_in": 604800}
     *
     * 토큰 회전: 기존 refresh token은 무효화되고 새 것이 발급됩니다.
     * 인증 불필요 엔드포인트 (refresh token 자체가 인증 수단).
     */
    if (g_strcmp0(path, REST_API_PREFIX "/auth/refresh") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        JsonObject  *body     = _parse_body(msg);
        const gchar *ref_tok  = body
            ? json_object_get_string_member_with_default(body, "refresh_token", "")
            : "";

        if (!ref_tok || !*ref_tok) {
            if (body) json_object_unref(body);
            _error(msg, 400, "BAD_REQUEST", "refresh_token is required");
            return;
        }

        GError *err = nullptr;
        gchar *new_refresh = nullptr;
        gchar *new_access = pcv_rbac_refresh_token(ref_tok, &new_refresh, &err);

        if (!new_access) {
            if (body) json_object_unref(body);
            g_free(new_refresh);
            _error(msg, 401, "UNAUTHORIZED",
                   err ? err->message : "Invalid refresh token");
            if (err) g_error_free(err);
            return;
        }

        gchar *resp = nullptr;
        if (new_refresh) {
            resp = g_strdup_printf(
                "{\"access_token\":\"%s\","
                "\"refresh_token\":\"%s\","
                "\"token_type\":\"Bearer\","
                "\"expires_in\":900,"
                "\"refresh_expires_in\":604800}",
                new_access, new_refresh);
        } else {
            resp = g_strdup_printf(
                "{\"access_token\":\"%s\","
                "\"token_type\":\"Bearer\","
                "\"expires_in\":900}",
                new_access);
        }
        _send_json(msg, 200, resp);
        g_free(resp);
        g_free(new_access);
        g_free(new_refresh);
        if (body) json_object_unref(body);
        PCV_LOG_INFO(REST_LOG_DOM, "Token refreshed via /auth/refresh");
        return;
    }

    /*
     * ── POST /api/v1/auth/logout (세션 무효화, 인증 불필요) ──
     *
     * 요청: {"refresh_token": "abcdef0123..."} 또는
     *       Authorization: Bearer <access_token> (사용자명 추출용)
     *
     * 해당 사용자의 모든 활성 세션(refresh token)을 무효화합니다.
     */
    if (g_strcmp0(path, REST_API_PREFIX "/auth/logout") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        /* Bearer token에서 사용자명 추출 시도 */
        SoupMessageHeaders *hdrs = soup_server_message_get_request_headers(msg);
        const gchar *auth_hdr = soup_message_headers_get_one(hdrs, "Authorization");
        gchar *sub = nullptr;
        if (auth_hdr) {
            GError *verr = nullptr;
            sub = pcv_jwt_verify(auth_hdr, &verr);
            if (verr) g_error_free(verr);
        }

        if (sub) {
            GError *err = nullptr;
            pcv_rbac_revoke_session(sub, &err);
            if (err) g_error_free(err);

            /* B6-W3 (Phase 3 fix): access token jti도 blacklist에 추가하여
             * 즉시 무효화. 이전엔 refresh token만 무효화 → access token이
             * 만료까지 유효. */
            if (auth_hdr) {
                const gchar *tk = auth_hdr;
                if (g_str_has_prefix(tk, "Bearer ")) tk += 7;
                if (g_str_has_prefix(tk, "bearer ")) tk += 7;
                /* JWT payload 직접 디코딩하여 jti+exp 추출 */
                gchar **parts = g_strsplit(tk, ".", 3);
                if (parts && parts[0] && parts[1]) {
                    gsize plen = 0;
                    /* base64url 패딩 보정 */
                    gchar *p1 = g_strdup(parts[1]);
                    gsize p1len = strlen(p1);
                    /* base64url → base64 (-_/-/+,_/+/) */
                    for (gchar *c = p1; *c; c++) {
                        if (*c == '-') *c = '+';
                        else if (*c == '_') *c = '/';
                    }
                    /* '=' 패딩 추가 */
                    gsize pad = (4 - (p1len % 4)) % 4;
                    gchar *padded = g_strconcat(p1, pad >= 1 ? "=" : "",
                                                pad >= 2 ? "=" : "",
                                                pad >= 3 ? "=" : "", NULL);
                    guchar *raw = g_base64_decode(padded, &plen);
                    g_free(padded); g_free(p1);
                    if (raw && plen > 0) {
                        gchar *json_str = g_strndup((const gchar *)raw, plen);
                        JsonParser *parser = json_parser_new();
                        if (json_parser_load_from_data(parser, json_str, -1, NULL)) {
                            JsonObject *o = json_node_get_object(json_parser_get_root(parser));
                            const gchar *jti = json_object_get_string_member_with_default(o, "jti", NULL);
                            gint64 exp = json_object_get_int_member_with_default(o, "exp", 0);
                            if (jti && exp > 0) {
                                pcv_jwt_blacklist_add(jti, exp);
                            }
                        }
                        g_object_unref(parser);
                        g_free(json_str);
                    }
                    g_free(raw);
                }
                g_strfreev(parts);
            }

            _send_json(msg, 200, "{\"status\":\"ok\",\"message\":\"All sessions revoked\"}");
            PCV_LOG_INFO(REST_LOG_DOM, "Logout: sessions revoked + access token blacklisted for '%s'", sub);
            g_free(sub);
        } else {
            _error(msg, 401, "UNAUTHORIZED", "Valid token required for logout");
        }
        return;
    }

    /*
     * ── POST /api/v1/auth/password (비밀번호 변경, 인증 필수) ──
     */
    if (g_strcmp0(path, REST_API_PREFIX "/auth/password") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        SoupMessageHeaders *hdrs = soup_server_message_get_request_headers(msg);
        const gchar *auth_hdr = soup_message_headers_get_one(hdrs, "Authorization");
        if (!auth_hdr || !g_str_has_prefix(auth_hdr, "Bearer ")) {
            _error(msg, 401, "UNAUTHORIZED", "Bearer token required");
            return;
        }
        GError *verr = nullptr;
        gchar *sub = pcv_jwt_verify(auth_hdr, &verr);
        if (verr) g_error_free(verr);
        if (!sub) {
            _error(msg, 401, "UNAUTHORIZED", "Invalid or expired token");
            return;
        }

        const gchar *boot_admin = pcv_config_get_admin_user();
        if (boot_admin && g_strcmp0(sub, boot_admin) == 0) {
            g_free(sub);
            _error(msg, 403, "FORBIDDEN",
                   "Bootstrap admin password must be changed in daemon.conf [daemon] admin_password");
            return;
        }

        JsonObject *body = _parse_body(msg);
        const gchar *old_pw = body
            ? json_object_get_string_member_with_default(body, "old_password", "")
            : "";
        const gchar *new_pw = body
            ? json_object_get_string_member_with_default(body, "new_password", "")
            : "";

        if (!old_pw || !*old_pw || !new_pw || !*new_pw) {
            if (body) json_object_unref(body);
            g_free(sub);
            _error(msg, 400, "BAD_REQUEST",
                   "old_password and new_password are required");
            return;
        }

        GError *err = nullptr;
        gboolean ok = pcv_rbac_change_password(sub, old_pw, new_pw, &err);

        GSocketAddress *ra = soup_server_message_get_remote_address(msg);
        gchar *rip = (ra && G_IS_INET_SOCKET_ADDRESS(ra))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)))
            : g_strdup("unknown");

        if (!ok) {
            int code = err && err->code == G_IO_ERROR_PERMISSION_DENIED ? 401 :
                       err && err->code == G_IO_ERROR_INVALID_ARGUMENT ? 400 : 500;
            const char *ec = code == 401 ? "UNAUTHORIZED" :
                             code == 400 ? "BAD_REQUEST" : "SERVER_ERROR";
            pcv_audit_log(sub, "auth.password.change", "self change",
                          "fail", code, 0, rip);
            g_free(rip);
            if (body) json_object_unref(body);
            _error(msg, code, ec,
                   err ? err->message : "Failed to change password");
            if (err) g_error_free(err);
            g_free(sub);
            return;
        }

        pcv_audit_log(sub, "auth.password.change", "self change",
                      "ok", 0, 0, rip);
        g_free(rip);
        PCV_LOG_INFO(REST_LOG_DOM, "Password changed for '%s' (sessions revoked)", sub);
        g_free(sub);
        if (body) json_object_unref(body);

        _send_json(msg, 200,
                   "{\"status\":\"ok\",\"message\":\"Password changed. All sessions revoked.\"}");
        return;
    }

    /*
     * ── POST /api/v1/auth/register (셀프 회원가입, 인증 불필요) ──
     *
     * 요청: {"username": "alice", "password": "secret123"}
     * 응답: 201 {"status":"ok","username":"alice","role":"viewer"}
     *
     * 활성화: daemon.conf [auth] allow_self_register = true
     * 기본 역할: VIEWER (최저 권한). 가입 후 ADMIN이 승격해야 OPERATOR/ADMIN 가능.
     */
    if (g_strcmp0(path, REST_API_PREFIX "/auth/register") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        const gchar *enabled = pcv_config_get_string("auth", "allow_self_register", "false");
        if (g_ascii_strcasecmp(enabled, "true") != 0 &&
            g_ascii_strcasecmp(enabled, "1")    != 0 &&
            g_ascii_strcasecmp(enabled, "yes")  != 0) {
            _error(msg, 403, "FORBIDDEN", "Self-registration is disabled");
            return;
        }

        JsonObject *body = _parse_body(msg);
        const gchar *username = body
            ? json_object_get_string_member_with_default(body, "username", "")
            : "";
        const gchar *password = body
            ? json_object_get_string_member_with_default(body, "password", "")
            : "";

        size_t ulen = username ? strlen(username) : 0;
        gboolean uvalid = (ulen >= 3 && ulen <= 32);
        for (size_t i = 0; uvalid && i < ulen; i++) {
            char c = username[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                uvalid = FALSE;
        }
        if (!uvalid) {
            if (body) json_object_unref(body);
            _error(msg, 400, "BAD_REQUEST",
                   "username must be 3-32 chars, lowercase alphanumeric or underscore");
            return;
        }

        size_t plen = password ? strlen(password) : 0;
        if (plen < 8) {
            if (body) json_object_unref(body);
            _error(msg, 400, "BAD_REQUEST", "password must be at least 8 characters");
            return;
        }
        if (g_strcmp0(username, password) == 0) {
            if (body) json_object_unref(body);
            _error(msg, 400, "BAD_REQUEST", "password must differ from username");
            return;
        }

        const gchar *adm = pcv_config_get_admin_user();
        if (adm && g_strcmp0(username, adm) == 0) {
            if (body) json_object_unref(body);
            _error(msg, 409, "CONFLICT", "username is reserved");
            return;
        }

        GError *err = nullptr;
        gboolean ok = pcv_rbac_user_create(username, password,
                                           PCV_ROLE_VIEWER, NULL, &err);

        GSocketAddress *ra = soup_server_message_get_remote_address(msg);
        gchar *rip = (ra && G_IS_INET_SOCKET_ADDRESS(ra))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)))
            : g_strdup("unknown");

        if (!ok) {
            pcv_audit_log(username, "auth.register", "self register",
                          "fail", 409, 0, rip);
            g_free(rip);
            if (body) json_object_unref(body);
            _error(msg, 409, "CONFLICT",
                   err ? err->message : "Failed to create user (already exists?)");
            if (err) g_error_free(err);
            return;
        }

        pcv_audit_log(username, "auth.register", "self register",
                      "ok", 0, 0, rip);
        g_free(rip);

        gchar *resp = g_strdup_printf(
            "{\"status\":\"ok\",\"username\":\"%s\",\"role\":\"viewer\"}", username);
        _send_json(msg, 201, resp);
        g_free(resp);
        if (body) json_object_unref(body);
        PCV_LOG_INFO(REST_LOG_DOM, "Self-registered user '%s'", username);
        return;
    }

    /*
     * ════════════════════════════════════════════════════════════════
     * 이하 모든 엔드포인트: JWT 인증 필수
     *
     * _authenticate()가 실패하면 401 응답이 이미 전송되고 NULL을 반환합니다.
     * 성공하면 subject(사용자명)를 반환하며, cleanup에서 g_free합니다.
     * ════════════════════════════════════════════════════════════════
     */
    /* [JWT 인증 게이트 — 이 지점 이후 모든 엔드포인트는 인증 필수]
     *
     * _authenticate()는 두 가지 인증 방식을 지원합니다:
     * 1. API Key 인증: X-API-Key 헤더 (자동화/스크립트용)
     * 2. JWT Bearer 인증: Authorization: Bearer <token> 헤더 (Web UI/CLI용)
     *
     * 성공 시: 사용자명(subject) 반환 — 이후 RBAC 권한 검사에 사용
     * 실패 시: 401 응답이 이미 전송되고 NULL 반환 → 즉시 return
     *
     * [중요] 이 줄 위의 엔드포인트 (/health, /metrics, /auth/token, /internal/...)는
     * 인증 없이 접근 가능합니다. 이 줄 아래는 반드시 인증이 필요합니다.
     */
    gchar *subject = _authenticate(msg);
    if (!subject) return;  /* 401 이미 전송됨 — _authenticate()가 에러 응답 처리 완료 */

    /* [ADR-0014] CSRF 검증 제거 — JWT Bearer 토큰이 CSRF 방어를 대체 */

    /* ── 요청 바디 파싱 + URL 경로 세그먼트 분리 ──────────────────
     * body: POST/PUT 요청의 JSON 본문 (GET은 빈 객체)
     * segs: URL을 '/'로 분할한 배열 (예: ["vms", "web-prod", "start"])
     *
     * [세그먼트 매핑 규칙 — REST → RPC 변환의 핵심]
     * segs[0] = resource  → 리소스 유형 결정 (vms, containers, networks...)
     * segs[1] = name      → 대상 리소스 이름 (VM명, 브릿지명 등)
     * segs[2] = action    → 수행할 작업 (start, stop, snapshot 등)
     * segs[3] = sub       → 하위 동작 (create, rollback, 스냅샷명 등)
     *
     * 예시:
     *   GET /api/v1/vms                    → resource="vms", name="", action=""
     *   POST /api/v1/vms/web/start         → resource="vms", name="web", action="start"
     *   POST /api/v1/vms/web/snapshot/create → resource="vms", name="web",
     *                                          action="snapshot", sub="create"
     * ──────────────────────────────────────────────────────────── */
    JsonObject *body    = _parse_body(msg);
    gchar     **segs    = _split_path(path);
    /*
     * 경로 세그먼트 매핑:
     *   segs[0] = resource  ("vms", "containers", "networks", "storage" 등)
     *   segs[1] = name      (VM명, 컨테이너명, 브릿지명 등)
     *   segs[2] = action    ("start", "stop", "snapshot", "nics", "vcpu" 등)
     *   segs[3] = sub       (스냅샷명, "create", "rollback" 등)
     *
     * 예: /api/v1/vms/web-prod/snapshot/create
     *   → resource="vms", name="web-prod", action="snapshot", sub="create"
     */
    const gchar *resource = segs[0] ? segs[0] : "";
    const gchar *name     = segs[1] ? segs[1] : "";
    const gchar *action   = segs[2] ? segs[2] : "";
    const gchar *sub      = segs[3] ? segs[3] : "";

    gchar *rpc = nullptr;
    gboolean  is_vm_delete   = FALSE;
    gchar    *vm_delete_name = nullptr;

    if (g_strcmp0(resource, "demo") == 0 &&
        g_strcmp0(name, "ovn-ovs") == 0 &&
        g_strcmp0(action, "health") == 0 &&
        g_strcmp0(method, "GET") == 0) {
        _send_ovn_demo_health(msg);
        goto cleanup;
    }

    /* ── 서킷 브레이커 확인: libvirt 의존 엔드포인트 사전 차단 ──
     *
     * [서킷 브레이커란?]
     * 전기 회로의 차단기와 같은 개념입니다.
     * libvirt가 장애 상태(연결 불가)이면 서킷 브레이커가 OPEN 상태가 됩니다.
     *
     *   CLOSED (정상) → 실패 누적 → OPEN (차단) → 30초 후 → HALF_OPEN (시험)
     *     ↑ 성공                                    ↓ 실패      ↓ 성공
     *     └────────────────────────────────── OPEN ←─┘    CLOSED ←─┘
     *
     * [왜 사전 차단하는가?]
     * libvirt가 다운된 상태에서 VM 관련 RPC를 실행하면 30초 타임아웃까지
     * 블로킹됩니다. 서킷 브레이커가 OPEN이면 즉시 503 + Retry-After: 30 반환하여
     * 클라이언트가 빠르게 에러를 감지하고 재시도할 수 있습니다.
     */
    gboolean needs_libvirt =
        (g_strcmp0(resource, "vms") == 0 ||
         g_strcmp0(resource, "containers") == 0 ||
#if PCV_CLUSTER_ENABLED
         g_str_has_prefix(path, REST_API_PREFIX "/cluster/vms") ||
#endif
         FALSE);
    if (needs_libvirt && cb_is_open()) {
        SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(rh, "Retry-After", "30");
        _error(msg, 503, "SERVICE_UNAVAILABLE",
               "libvirt connection is down (circuit breaker open)");
        goto cleanup;
    }

    /* ══════════════════════════════════════════════════════════════
     * REST → RPC 라우팅 테이블 시작
     *
     * [동작 원리]
     * HTTP 메서드(GET/POST/PUT/DELETE) + URL 세그먼트 조합으로
     * 대응하는 JSON-RPC 메서드를 결정하고 _build_rpc()로 요청을 생성합니다.
     *
     * 예: GET /api/v1/vms → rpc = _build_rpc("vm.list", NULL)
     *     POST /api/v1/vms/web/start → rpc = _build_rpc_name("vm.start", "web")
     *
     * 모든 분기는 rpc 변수를 설정합니다. 매칭 없으면 rpc=NULL → 404.
     * 최종적으로 rpc가 비동기 워커를 통해 _rpc_over_uds()로 전달됩니다.
     *
     * ┌──────────────────────────────────────────────────────────────────────────────┐
     * │ REST → RPC 라우팅 테이블 요약 (세그먼트 기반 디스패칭)                      │
     * ├──────────────────────────────────────────────────────────────────────────────┤
     * │ segs[0]="rpc"          POST /rpc             → JSON-RPC passthrough (직통) │
     * │ segs[0]="vms"          GET  /vms              → vm.list                    │
     * │                        POST /vms              → vm.create                  │
     * │                        GET  /vms/{n}          → vm.metrics                 │
     * │                        DEL  /vms/{n}          → vm.delete                  │
     * │                        POST /vms/{n}/start    → vm.start                   │
     * │                        POST /vms/{n}/stop     → vm.stop                    │
     * │                        PUT  /vms/{n}/rename   → vm.rename                  │
     * │                        POST /vms/{n}/snapshot/create → vm.snapshot.create   │
     * │                        GET  /vms/{n}/nics     → device.nic.list            │
     * │                        PUT  /vms/{n}/vcpu     → vm.set_vcpu                │
     * │                        PUT  /vms/{n}/memory   → vm.set_memory              │
     * │                        PUT  /vms/{n}/disk     → vm.resize_disk             │
     * │ segs[0]="containers"   GET  /containers       → container.list             │
     * │                        POST /containers       → container.create           │
     * │                        POST /containers/{n}/start → container.start        │
     * │ segs[0]="networks"     GET  /networks         → network.list               │
     * │                        POST /networks         → network.create             │
     * │ segs[0]="storage"      GET  /storage          → storage.zvol.list          │
     * │                        POST /storage          → storage.zvol.create        │
     * │ segs[0]="monitor"      GET  /monitor/overview → monitor.overview           │
     * │                        GET  /monitor/hosts    → telemetry.host             │
     * │ segs[0]="templates"    GET  /templates        → template.list              │
     * │ segs[0]="backup"       GET  /backup/policies  → backup.policy.list         │
     * │ segs[0]="cloud"        POST /cloud/import     → cloud.import              │
     * │ segs[0]="cluster"      GET  /cluster/status   → cluster.status             │
     * │                        GET  /cluster/vms      → cluster.vm.list            │
     * │ segs[0]="auth"         POST /auth/register    → auth.register              │
     * │                        POST /auth/password    → auth.change_password       │
     * │ segs[0]="accounts"     GET  /accounts         → auth.list                  │
     * │ segs[0]="overlay"      GET  /overlay          → overlay.list               │
     * │ segs[0]="gpu"          GET  /gpu              → gpu.list                   │
     * │ segs[0]="ai"           GET  /ai/config        → ai.agent.get_config       │
     * └──────────────────────────────────────────────────────────────────────────────┘
     * (참고: 위 표는 주요 라우트만 발췌. 전체 199 operations는 openapi.yaml 참조)
     * ══════════════════════════════════════════════════════════════ */

    /* ── POST /api/v1/rpc — Generic JSON-RPC passthrough ──────────
     *
     * [왜 이 엔드포인트가 존재하는가?]
     * 253개 RPC 메서드 중 REST 전용 라우트가 없는 메서드(예: ai.healing.approve,
     * security_group.rule.remove 등)에 접근하기 위한 범용 통로입니다.
     * Web UI의 EP.RPC() 함수(약 30곳)가 이 경로를 사용합니다.
     *
     * [동작 방식]
     * HTTP body가 이미 JSON-RPC 2.0 envelope입니다:
     *   {"jsonrpc":"2.0","method":"ai.healing.approve","params":{...},"id":1}
     * _build_rpc()를 호출하지 않고 body를 그대로 직렬화하여 UDS에 전달합니다.
     *
     * [보안 — method 추출으로 RBAC 적용 (2026-04-09 강화)]
     * body에서 "method" 필드를 추출하여 RBAC 권한 검사를 수행합니다.
     * 이전에는 passthrough라 RBAC을 우회할 수 있었던 보안 허점이 있었습니다.
     *
     * [주의] body가 NULL이면 400 반환. JSON-RPC envelope 형식이 아니면
     * UDS 서버가 -32700 Parse Error를 반환합니다. */
    if (g_strcmp0(resource, "rpc") == 0 && *name == '\0' &&
        g_strcmp0(method, "POST") == 0) {
        if (!body) {
            _error(msg, 400, "BAD_REQUEST", "JSON body required");
            goto cleanup;
        }
        JsonNode *bn = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(bn, body);
        rpc = json_to_string(bn, FALSE);
        json_node_free(bn);
    }
    else
    /* ── /api/v1/vms — VM 라이프사이클/스냅샷/디바이스/마이그레이션 ── */
    if (g_strcmp0(resource, "vms") == 0) {

        if (*name == '\0') {
            /* GET /vms   → vm.list (offset/limit 쿼리 파라미터 전달) */
            if (g_strcmp0(method, "GET") == 0) {
                JsonObject *pg = nullptr;
                if (query) {
                    const gchar *q_off = g_hash_table_lookup(query, "offset");
                    const gchar *q_lim = g_hash_table_lookup(query, "limit");
                    if (q_off || q_lim) {
                        pg = json_object_new();
                        if (q_off) json_object_set_int_member(pg, "offset", g_ascii_strtoll(q_off, NULL, 10));
                        if (q_lim) json_object_set_int_member(pg, "limit", g_ascii_strtoll(q_lim, NULL, 10));
                    }
                }
                rpc = _build_rpc("vm.list", pg);
                if (pg) json_object_unref(pg);
            }
            /* POST /vms  → vm.create (name 필수) */
            else if (g_strcmp0(method, "POST") == 0) {
                const gchar *req[] = {"name"};
                if (!pcv_validate_required(msg, body, req, 1)) goto cleanup;
                rpc = _build_rpc("vm.create", body);
            }

        } else if (*action == '\0') {
            /* GET    /vms/{n}  → vm.metrics */
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc_name("vm.metrics", name);
            /* DELETE /vms/{n}  → vm.delete (ZFS 폴링 동기 응답) */
            else if (g_strcmp0(method, "DELETE") == 0) {
                rpc = _build_rpc_name("vm.delete", name);
                is_vm_delete = TRUE;
                vm_delete_name = g_strdup(name);
            }

        } else if (g_strcmp0(action, "start") == 0) {
            rpc = _build_rpc_name("vm.start", name);

        } else if (g_strcmp0(action, "stop") == 0) {
            rpc = _build_rpc_name("vm.stop", name);

        } else if (g_strcmp0(action, "suspend") == 0 || g_strcmp0(action, "pause") == 0) {
            rpc = _build_rpc_name("vm.pause", name);

        } else if (g_strcmp0(action, "resume") == 0) {
            rpc = _build_rpc_name("vm.resume", name);

        } else if (g_strcmp0(action, "metrics") == 0) {
            rpc = _build_rpc_name("vm.metrics", name);

        } else if (g_strcmp0(action, "snapshot") == 0) {
            /* GET  /vms/{n}/snapshot       → vm.snapshot.list */
            if (*sub == '\0' && g_strcmp0(method, "GET") == 0) {
                rpc = _build_rpc_name("vm.snapshot.list", name);

            /* POST /vms/{n}/snapshot/create → vm.snapshot.create */
            } else if (g_strcmp0(sub, "create") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                const gchar *sname = nullptr;
                if (body) {
                    if (json_object_has_member(body, "snapshot_name"))
                        sname = json_object_get_string_member(body, "snapshot_name");
                    else if (json_object_has_member(body, "snap_name"))
                        sname = json_object_get_string_member(body, "snap_name");
                }
                if (sname)
                    json_object_set_string_member(p, "snapshot_name", sname);
                rpc = _build_rpc("vm.snapshot.create", p);
                json_object_unref(p);

            /* POST /vms/{n}/snapshot/rollback */
            } else if (g_strcmp0(sub, "rollback") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                const gchar *rname = nullptr;
                if (body) {
                    if (json_object_has_member(body, "snapshot_name"))
                        rname = json_object_get_string_member(body, "snapshot_name");
                    else if (json_object_has_member(body, "snap_name"))
                        rname = json_object_get_string_member(body, "snap_name");
                }
                if (rname)
                    json_object_set_string_member(p, "snapshot_name", rname);
                rpc = _build_rpc("vm.snapshot.rollback", p);
                json_object_unref(p);

            /* POST /vms/{n}/snapshot/delete_all → vm.snapshot.delete_all */
            } else if (g_strcmp0(sub, "delete_all") == 0 && g_strcmp0(method, "POST") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                if (body && json_object_has_member(body, "prefix"))
                    json_object_set_string_member(p, "prefix",
                        json_object_get_string_member(body, "prefix"));
                if (body && json_object_has_member(body, "keep_recent"))
                    json_object_set_int_member(p, "keep_recent",
                        json_object_get_int_member(body, "keep_recent"));
                rpc = _build_rpc("vm.snapshot.delete_all", p);
                json_object_unref(p);

            /* ── Snapshot Schedule ────────────────────────────
             * POST   /vms/{n}/snapshot/schedule → vm.snapshot.schedule.set
             * GET    /vms/{n}/snapshot/schedule → vm.snapshot.schedule.list
             * DELETE /vms/{n}/snapshot/schedule → vm.snapshot.schedule.delete
             * ─────────────────────────────────────────────── */
            } else if (g_strcmp0(sub, "schedule") == 0) {
                if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "vm_name", name);
                    rpc = _build_rpc("vm.snapshot.schedule.set", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "vm_name", name);
                    rpc = _build_rpc("vm.snapshot.schedule.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "DELETE") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "vm_name", name);
                    rpc = _build_rpc("vm.snapshot.schedule.delete", p);
                    json_object_unref(p);
                }

            /* DELETE /vms/{n}/snapshot/{snap} */
            } else if (*sub != '\0' && g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                json_object_set_string_member(p, "snapshot_name", sub);
                rpc = _build_rpc("vm.snapshot.delete", p);
                json_object_unref(p);
            }

        /* ── Sprint F: NIC 핫플러그 ──────────────────────── *
         * GET    /vms/{n}/nics              → device.nic.list
         * POST   /vms/{n}/nics             → device.nic.attach
         * DELETE /vms/{n}/nics/{mac}       → device.nic.detach
         * ─────────────────────────────────────────────────── */
        } else if (g_strcmp0(action, "nics") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "vm_id", name);
                    rpc = _build_rpc("device.nic.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "POST") == 0) {
                    /* body: {bridge, mac?, model?} */
                    JsonObject *p = body
                        ? (json_object_ref(body), body)
                        : json_object_new();
                    json_object_set_string_member(p, "vm_id", name);
                    rpc = _build_rpc("device.nic.attach", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(method, "DELETE") == 0) {
                /* sub = URL-encoded MAC, e.g. "52:54:00:xx:xx:xx" */
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                json_object_set_string_member(p, "mac",   sub);
                rpc = _build_rpc("device.nic.detach", p);
                json_object_unref(p);
            }

        /* ── USB Passthrough ──────────────────────────────── *
         * GET    /vms/{n}/usb              → vm.usb.list
         * POST   /vms/{n}/usb             → vm.usb.attach
         * DELETE /vms/{n}/usb             → vm.usb.detach
         * ─────────────────────────────────────────────────── */
        } else if (g_strcmp0(action, "usb") == 0) {
            if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rpc = _build_rpc("vm.usb.list", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "POST") == 0) {
                JsonObject *p = body
                    ? (json_object_ref(body), body)
                    : json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rpc = _build_rpc("vm.usb.attach", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = body
                    ? (json_object_ref(body), body)
                    : json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rpc = _build_rpc("vm.usb.detach", p);
                json_object_unref(p);
            }

        /* ── Sprint F: ISO CD-ROM ─────────────────────────── *
         * POST   /vms/{n}/iso              → vm.mount_iso
         * DELETE /vms/{n}/iso              → vm.eject
         * ─────────────────────────────────────────────────── */
        /* ── P1: vCPU / Memory 핫플러그 ─────────────────────── *
         * PUT /vms/{n}/vcpu    → vm.set_vcpu   body: {vcpu_count} 또는 {vcpu}/{count}
         * PUT /vms/{n}/memory  → vm.set_memory  body: {memory_mb}
         * ─────────────────────────────────────────────────── */
        } else if (g_strcmp0(action, "vcpu") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            gint vcpu_count = 0;
            if (pcv_rpc_params_get_int_alias(p, "vcpu_count", "vcpu", &vcpu_count) ||
                pcv_rpc_params_get_int_alias(p, "count", NULL, &vcpu_count)) {
                json_object_set_int_member(p, "vcpu_count", vcpu_count);
            }
            json_object_set_string_member(p, "vm_id", name);
            rpc = _build_rpc("vm.set_vcpu", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "memory") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rpc = _build_rpc("vm.set_memory", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "delete-status") == 0 && g_strcmp0(method, "GET") == 0) {
            rpc = _build_rpc_name("vm.delete.status", name);

        } else if (g_strcmp0(action, "rename") == 0 && g_strcmp0(method, "PUT") == 0) {
            if (!body || !json_object_has_member(body, "new_name")) {
                _error(msg, 400, "BAD_REQUEST", "new_name required");
                goto cleanup;
            }
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            json_object_set_string_member(p, "new_name",
                json_object_get_string_member(body, "new_name"));
            rpc = _build_rpc("vm.rename", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "iso") == 0) {
            if (g_strcmp0(method, "POST") == 0) {
                JsonObject *p = body ? (json_object_ref(body), body)
                                     : json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rpc = _build_rpc("vm.mount_iso", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rpc = _build_rpc("vm.eject", p);
                json_object_unref(p);
            }

        /* ── A+B: POST /vms/{n}/clone → vm.clone ──────────── */
        } else if (g_strcmp0(action, "clone") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.clone", p);
            json_object_unref(p);

        /* ── A+B: PUT /vms/{n}/disk → vm.disk.resize ──────── */
        } else if (g_strcmp0(action, "disk") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rpc = _build_rpc("vm.resize_disk", p);
            json_object_unref(p);

        /* ── A+B: PUT /vms/{n}/cpu-pin → vm.cpu.pin ────────── */
        } else if (g_strcmp0(action, "cpu-pin") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rpc = _build_rpc("vm.pin_vcpu", p);
            json_object_unref(p);

        /* ── A+B: PUT /vms/{n}/bandwidth → vm.set_bandwidth ── */
        } else if (g_strcmp0(action, "bandwidth") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rpc = _build_rpc("vm.set_bandwidth", p);
            json_object_unref(p);

        /* ── A+B: POST /vms/{n}/export → vm.export.ova ──────── */
        } else if (g_strcmp0(action, "export") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc_name("vm.export.ova", name);

        /* ── A+B: POST /vms/import → vm.import.ova ────────── */
        } else if (g_strcmp0(action, "import") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("vm.import.ova", body);

        /* ── Cloud Migration: POST /vms/{n}/import-ec2 ─────── */
        } else if (g_strcmp0(action, "import-ec2") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.import.ec2", p);
            json_object_unref(p);

        /* ── Cloud Migration: POST /vms/{n}/export-ec2 ─────── */
        } else if (g_strcmp0(action, "export-ec2") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.export.ec2", p);
            json_object_unref(p);

        /* ── Cloud Migration: GET /vms/{n}/import-status ────── */
        } else if (g_strcmp0(action, "import-status") == 0 && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.import.status", p);
            json_object_unref(p);

        /* ── Cloud Migration: GET /vms/{n}/export-status ────── */
        } else if (g_strcmp0(action, "export-status") == 0 && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.export.status", p);
            json_object_unref(p);

        /* ── Memory Stats: GET /vms/{n}/memory-stats ────────── */
        } else if (g_strcmp0(action, "memory-stats") == 0 && g_strcmp0(method, "GET") == 0) {
            rpc = _build_rpc_name("vm.memory.stats", name);

        /* ── Snapshot Schedule Status: GET /vms/{n}/snapshot-schedule ── */
        } else if (g_strcmp0(action, "snapshot-schedule") == 0 && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("snapshot.schedule.status", p);
            json_object_unref(p);

        /* ── Guest Agent: POST /vms/{n}/guest-ping ──────────── */
        } else if (g_strcmp0(action, "guest-ping") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc_name("vm.guest.ping", name);

        /* ── Guest Agent: GET /vms/{n}/guest-agent ──────────── */
        } else if (g_strcmp0(action, "guest-agent") == 0 && g_strcmp0(method, "GET") == 0) {
            rpc = _build_rpc_name("vm.guest.agent.status", name);

        /* ── Guest Agent: POST /vms/{n}/guest-agent-channel ─── */
        } else if (g_strcmp0(action, "guest-agent-channel") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc_name("vm.guest.agent.ensure_channel", name);

        /* ── Guest Agent: POST /vms/{n}/guest-shutdown ──────── */
        } else if (g_strcmp0(action, "guest-shutdown") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc_name("vm.guest.shutdown", name);

        /* ── Guest Agent: GET /vms/{n}/disk-usage ──────────── */
        } else if (g_strcmp0(action, "disk-usage") == 0 && g_strcmp0(method, "GET") == 0) {
            rpc = _build_rpc_name("vm.guest.fsinfo", name);

        /* ── Disk Live Resize: POST /vms/{n}/disk-resize ───── */
        } else if (g_strcmp0(action, "disk-resize") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.disk.live_resize", p);
            json_object_unref(p);

        /* ── Guest Agent: POST /vms/{n}/guest-exec ──────────── */
        } else if (g_strcmp0(action, "guest-exec") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.guest.exec", p);
            json_object_unref(p);
        }
    }

    /* ── /api/v1/cloud ──────────────────────────────────── */
    else if (g_strcmp0(resource, "cloud") == 0) {
        /* GET /api/v1/cloud/jobs — 전체 마이그레이션 작업 목록 */
        if (g_strcmp0(name, "jobs") == 0 && g_strcmp0(method, "GET") == 0) {
            rpc = _build_rpc("cloud.jobs.list", NULL);
        }
        /* POST /api/v1/cloud/cancel — 작업 취소 */
        else if (g_strcmp0(name, "cancel") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("cloud.job.cancel", body);
        }
    }

    /* ── /api/v1/containers ──────────────────────────────── */
    else if (g_strcmp0(resource, "containers") == 0) {

        if (*name == '\0') {
            if (g_strcmp0(method, "GET") == 0) {
                JsonObject *pg = nullptr;
                if (query) {
                    const gchar *q_off = g_hash_table_lookup(query, "offset");
                    const gchar *q_lim = g_hash_table_lookup(query, "limit");
                    if (q_off || q_lim) {
                        pg = json_object_new();
                        if (q_off) json_object_set_int_member(pg, "offset", g_ascii_strtoll(q_off, NULL, 10));
                        if (q_lim) json_object_set_int_member(pg, "limit", g_ascii_strtoll(q_lim, NULL, 10));
                    }
                }
                rpc = _build_rpc("container.list", pg);
                if (pg) json_object_unref(pg);
            } else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("container.create", body);

        } else if (*action == '\0' && g_strcmp0(method, "DELETE") == 0) {
            rpc = _build_rpc_name("container.destroy", name);

        } else if (g_strcmp0(action, "start") == 0) {
            rpc = _build_rpc_name("container.start", name);
        } else if (g_strcmp0(action, "stop") == 0) {
            rpc = _build_rpc_name("container.stop", name);
        } else if (g_strcmp0(action, "metrics") == 0) {
            rpc = _build_rpc_name("container.metrics", name);
        } else if (g_strcmp0(action, "exec") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            if (body && json_object_has_member(body, "command"))
                json_object_set_string_member(p, "cmd",
                    json_object_get_string_member(body, "command"));
            rpc = _build_rpc("container.exec", p);
            json_object_unref(p);

        /* ── Container Snapshots ─────────────────────────────
         * GET    /containers/{n}/snapshots            → container.snapshot.list
         * POST   /containers/{n}/snapshots            → container.snapshot.create
         * POST   /containers/{n}/snapshots/rollback   → container.snapshot.rollback
         * DELETE /containers/{n}/snapshots/{snap}     → container.snapshot.delete
         * ─────────────────────────────────────────────────── */
        } else if (g_strcmp0(action, "snapshots") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    rpc = _build_rpc_name("container.snapshot.list", name);
                } else if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rpc = _build_rpc("container.snapshot.create", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(sub, "rollback") == 0 && g_strcmp0(method, "POST") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.snapshot.rollback", p);
                json_object_unref(p);
            } else if (*sub != '\0' && g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                json_object_set_string_member(p, "snap_name", sub);
                rpc = _build_rpc("container.snapshot.delete", p);
                json_object_unref(p);
            }

        /* ── A+B: PUT /containers/{n}/limits → container.set_limits ── */
        } else if (g_strcmp0(action, "limits") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("container.set_limits", p);
            json_object_unref(p);

        /* ── LXC Network: NIC list/add/remove + bandwidth ────── */
        } else if (g_strcmp0(action, "nics") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rpc = _build_rpc("container.nic.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? (json_object_ref(body), body)
                                         : json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rpc = _build_rpc("container.nic.attach", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                json_object_set_string_member(p, "nic_name", sub);
                rpc = _build_rpc("container.nic.detach", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "bandwidth") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("container.set_bandwidth", p);
            json_object_unref(p);

        /* ── Container Health Check ──────────────────────────
         * PUT    /containers/{n}/health  → container.health.set
         * GET    /containers/{n}/health  → container.health.get
         * DELETE /containers/{n}/health  → container.health.delete
         * ──────────────────────────────────────────────────── */
        } else if (g_strcmp0(action, "health") == 0) {
            if (g_strcmp0(method, "PUT") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.health.set", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.health.get", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.health.delete", p);
                json_object_unref(p);
            }

        /* ── Container Volumes ───────────────────────────────
         * GET    /containers/{n}/volumes          → container.volume.list
         * POST   /containers/{n}/volumes          → container.volume.attach
         * DELETE /containers/{n}/volumes/{vol}     → container.volume.detach
         * ──────────────────────────────────────────────────── */
        } else if (g_strcmp0(action, "volumes") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rpc = _build_rpc("container.volume.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rpc = _build_rpc("container.volume.attach", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                json_object_set_string_member(p, "volume", sub);
                rpc = _build_rpc("container.volume.detach", p);
                json_object_unref(p);
            }

        /* ── Container Environment Variables ─────────────────
         * GET    /containers/{n}/env   → container.env.list
         * PUT    /containers/{n}/env   → container.env.set
         * DELETE /containers/{n}/env   → container.env.delete
         * ──────────────────────────────────────────────────── */
        } else if (g_strcmp0(action, "env") == 0) {
            if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.env.list", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "PUT") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.env.set", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.env.delete", p);
                json_object_unref(p);
            }
        }
    }

    /* ── /api/v1/monitor ─────────────────────────────────── */
    else if (g_strcmp0(resource, "monitor") == 0) {
        if (g_strcmp0(name, "metrics") == 0)
            rpc = _build_rpc("monitor.metrics", NULL);
        else if (g_strcmp0(name, "fleet") == 0)
            rpc = _build_rpc("monitor.fleet", NULL);
    }

    /* ── /api/v1/alerts (WhaTap W-2) ────────────────────── */
    else if (g_strcmp0(resource, "alerts") == 0) {
        if (g_strcmp0(name, "config") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("alert.config.get", NULL);
            else if (g_strcmp0(method, "PUT") == 0)
                rpc = _build_rpc("alert.config.set", body);
        } else if (g_strcmp0(name, "actions") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("alert.action.list", NULL);
        } else {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("alert.history", NULL);
        }
    }

    /* ── /api/v1/prometheus/sd ─────────────────────────── */
    else if (g_strcmp0(resource, "prometheus") == 0) {
        if (g_strcmp0(name, "sd") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("prometheus.sd", NULL);
    }

    /* ── /api/v1/audit/search ───────────────────────────── */
    else if (g_strcmp0(resource, "audit") == 0) {
        if (g_strcmp0(name, "search") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("audit.search", body);
    }

    /* ── /api/v1/jobs ───────────────────────────────────── */
    else if (g_strcmp0(resource, "jobs") == 0) {
        if (*name == '\0' && g_strcmp0(method, "GET") == 0) {
            JsonObject *pg = nullptr;
            if (query) {
                const gchar *q_off = g_hash_table_lookup(query, "offset");
                const gchar *q_lim = g_hash_table_lookup(query, "limit");
                if (q_off || q_lim) {
                    pg = json_object_new();
                    if (q_off) json_object_set_int_member(pg, "offset", g_ascii_strtoll(q_off, NULL, 10));
                    if (q_lim) json_object_set_int_member(pg, "limit", g_ascii_strtoll(q_lim, NULL, 10));
                }
            }
            rpc = _build_rpc("jobs.list", pg);
            if (pg) json_object_unref(pg);
        } else if (*name != '\0' && g_strcmp0(action, "cancel") == 0
                   && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "job_id", name);
            rpc = _build_rpc("jobs.cancel", p);
            json_object_unref(p);
        } else if (*name != '\0' && *action == '\0'
                   && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "job_id", name);
            rpc = _build_rpc("jobs.get", p);
            json_object_unref(p);
        }
    }

    /* ── /api/v1/webhook/dlq ────────────────────────────── */
    else if (g_strcmp0(resource, "webhook") == 0) {
        if (g_strcmp0(name, "dlq") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("webhook.dlq.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("webhook.dlq.retry", NULL);
        }
    }

    /* ── /api/v1/processes (WhaTap W-3) ─────────────────── */
    else if (g_strcmp0(resource, "processes") == 0) {
        if (g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("monitor.processes", NULL);
    }

    /* ── /api/v1/agent — AI Agent 설정/이력 ─────────────── */
    else if (g_strcmp0(resource, "agent") == 0) {
        if (g_strcmp0(name, "config") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("agent.config.get", NULL);
            else if (g_strcmp0(method, "PUT") == 0)
                rpc = _build_rpc("agent.config.set", body);
        } else if (g_strcmp0(name, "history") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("agent.history", NULL);
        } else {
            /* GET /agent → agent.config.get (기본) */
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("agent.config.get", NULL);
        }
    }

    /* ── /api/v1/networks (Sprint F/G) ──────────────────── *
     * GET    /networks            → network.list
     * GET    /networks/{br}       → network.info
     * POST   /networks            → network.create
     * DELETE /networks/{br}       → network.delete
     * POST   /networks/{br}/mode  → network.mode_set   [Sprint G]
     * ─────────────────────────────────────────────────── */
    else if (g_strcmp0(resource, "networks") == 0) {
        if (*name == '\0') {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("network.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("network.create", body);
        } else {
            /* /networks/{br}/mode → network.mode_set [Sprint G] */
            if (g_strcmp0(action, "mode") == 0 && g_strcmp0(method, "POST") == 0) {
                /* body에 name 주입 */
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("network.mode_set", p);
                json_object_unref(p);

            /* ── /networks/{iface}/qos — QoS 설정/조회/삭제 ──── */
            } else if (g_strcmp0(action, "qos") == 0) {
                if (g_strcmp0(method, "PUT") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "interface", name);
                    rpc = _build_rpc("network.qos.set", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "interface", name);
                    rpc = _build_rpc("network.qos.get", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "DELETE") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "interface", name);
                    rpc = _build_rpc("network.qos.remove", p);
                    json_object_unref(p);
                }

            } else if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "bridge_name", name);
                rpc = _build_rpc("network.info", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "bridge_name", name);
                rpc = _build_rpc("network.delete", p);
                json_object_unref(p);
            }
        }
    }

    /* ── /api/v1/storage ─────────────────────────────────── */
    else if (g_strcmp0(resource, "storage") == 0) {
        /* name = "pools" | "zvols" | {pool_name} */
        if (g_strcmp0(name, "pools") == 0) {
            if (*action == '\0') {
                if (g_strcmp0(method, "GET") == 0)
                    rpc = _build_rpc("storage.pool.list", NULL);
                /* A+B: POST /storage/pools → storage.pool.create */
                else if (g_strcmp0(method, "POST") == 0)
                    rpc = _build_rpc("storage.pool.create", body);
                /* A+B: DELETE /storage/pools → storage.pool.destroy (body: {name}) */
                else if (g_strcmp0(method, "DELETE") == 0)
                    rpc = _build_rpc("storage.pool.destroy", body);
            }
            /* A+B: POST /storage/pools/scrub → storage.pool.scrub */
            else if (g_strcmp0(action, "scrub") == 0 && g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("storage.pool.scrub", body);
        }
        else if (g_strcmp0(name, "zvols") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("storage.zvol.list", body);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("storage.zvol.create", body);
            else if (g_strcmp0(method, "DELETE") == 0)
                rpc = _build_rpc("storage.zvol.delete", body);
        }
    }

    /* ── /api/v1/dpdk (Phase 4) ─────────────────────────── *
     * GET    /dpdk/status           → dpdk.status
     * POST   /dpdk/bind             → dpdk.bind
     * POST   /dpdk/unbind           → dpdk.unbind
     * GET    /dpdk/list             → dpdk.list
     * POST   /dpdk/bridge/create    → dpdk.bridge.create
     * POST   /dpdk/bridge/delete    → dpdk.bridge.delete
     * GET    /dpdk/hugepage         → dpdk.hugepage.info
     * ─────────────────────────────────────────────────── */
    else if (g_strcmp0(resource, "dpdk") == 0) {
        if (g_strcmp0(name, "status") == 0)
            rpc = _build_rpc("dpdk.status", NULL);
        else if (g_strcmp0(name, "bind") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("dpdk.bind", body);
        else if (g_strcmp0(name, "unbind") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("dpdk.unbind", body);
        else if (g_strcmp0(name, "list") == 0)
            rpc = _build_rpc("dpdk.list", NULL);
        else if (g_strcmp0(name, "hugepage") == 0)
            rpc = _build_rpc("dpdk.hugepage.info", NULL);
        else if (g_strcmp0(name, "bridge") == 0) {
            if (g_strcmp0(action, "create") == 0 && g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("dpdk.bridge.create", body);
            else if (g_strcmp0(action, "delete") == 0 && g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("dpdk.bridge.delete", body);
        }
    }

    /* ── /api/v1/sriov (Phase 4) ─────────────────────────── *
     * GET    /sriov/status          → sriov.status
     * POST   /sriov/enable          → sriov.enable
     * POST   /sriov/disable         → sriov.disable
     * GET    /sriov/list            → sriov.list
     * POST   /sriov/set             → sriov.set
     * POST   /sriov/attach          → sriov.attach
     * POST   /sriov/detach          → sriov.detach
     * ─────────────────────────────────────────────────── */
    else if (g_strcmp0(resource, "sriov") == 0) {
        if (g_strcmp0(name, "status") == 0)
            rpc = _build_rpc("sriov.status", NULL);
        else if (g_strcmp0(name, "enable") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("sriov.enable", body);
        else if (g_strcmp0(name, "disable") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("sriov.disable", body);
        else if (g_strcmp0(name, "list") == 0)
            rpc = _build_rpc("sriov.list", body);
        else if (g_strcmp0(name, "set") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("sriov.set", body);
        else if (g_strcmp0(name, "attach") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("sriov.attach", body);
        else if (g_strcmp0(name, "detach") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("sriov.detach", body);
    }

    /* ── /api/v1/backup — 백업 정책/이력 ──────────────────── */
    else if (g_strcmp0(resource, "backup") == 0) {
        if (g_strcmp0(name, "policies") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("backup.policy.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("backup.policy.set", body);
            else if (g_strcmp0(method, "DELETE") == 0)
                rpc = _build_rpc("backup.policy.delete", body);
        } else if (g_strcmp0(name, "history") == 0) {
            rpc = _build_rpc("backup.history", body);

        /* ── Backup Advanced Operations ──────────────────────
         * POST /backup/incremental  → backup.incremental
         * POST /backup/verify       → backup.verify
         * POST /backup/replicate    → backup.replicate
         * POST /backup/export-s3    → backup.export_s3
         * ──────────────────────────────────────────────────── */
        } else if (g_strcmp0(name, "incremental") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("backup.incremental", body);
        } else if (g_strcmp0(name, "verify") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("backup.verify", body);
        } else if (g_strcmp0(name, "replicate") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("backup.replicate", body);
        } else if (g_strcmp0(name, "export-s3") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("backup.export_s3", body);
        }
    }

    /* ── /api/v1/ovn — OVN SDN 프록시 ──────────────────── */
    else if (g_strcmp0(resource, "ovn") == 0) {
        if (g_strcmp0(name, "status") == 0)
            rpc = _build_rpc("ovn.status", NULL);
        else if (g_strcmp0(name, "switches") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("ovn.switch.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("ovn.switch.create", body);
        } else if (g_strcmp0(name, "routers") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("ovn.router.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("ovn.router.create", body);
        } else if (g_strcmp0(name, "acl") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("ovn.acl.list", body);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("ovn.acl.add", body);
        } else if (g_strcmp0(name, "nat") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("ovn.nat.list", body);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("ovn.nat.add", body);
        }
    }

    /* ── /api/v1/iso — ISO 이미지 목록 ────────────────── */
    else if (g_strcmp0(resource, "iso") == 0 && g_strcmp0(method, "GET") == 0) {
        rpc = _build_rpc("iso.list", NULL);
    }

    /* ── /api/v1/iscsi/targets — iSCSI 타겟 관리 ──────── */
    else if (g_strcmp0(resource, "iscsi") == 0) {
        if (g_strcmp0(name, "targets") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("iscsi.target.list", NULL);
        else if (g_strcmp0(name, "targets") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("iscsi.target.create", body);
        else if (g_strcmp0(name, "targets") == 0 && g_strcmp0(method, "DELETE") == 0)
            rpc = _build_rpc("iscsi.target.delete", body);
        else if (g_strcmp0(name, "connect") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("iscsi.connect", body);
        else if (g_strcmp0(name, "disconnect") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("iscsi.disconnect", body);
    }

    /* ── /api/v1/overlay — 오버레이 네트워크 ──────────── */
    else if (g_strcmp0(resource, "overlay") == 0 && g_strcmp0(method, "GET") == 0) {
        rpc = _build_rpc("overlay.list", NULL);
    }

    /* ── /api/v1/auth/users — RBAC 사용자 관리 ────────── */
    else if (g_strcmp0(resource, "auth") == 0) {
        if (g_strcmp0(name, "whoami") == 0 && g_strcmp0(method, "GET") == 0) {
            PcvRole role = pcv_rbac_get_role(subject);
            const gchar *role_str = pcv_rbac_role_to_str(role);
            const gchar *tenant = pcv_rbac_get_tenant(subject);
            gchar *resp = g_strdup_printf(
                "{\"data\":{\"username\":\"%s\",\"role\":\"%s\",\"tenant\":\"%s\"}}",
                subject, role_str, tenant ? tenant : "---");
            _send_json(msg, 200, resp);
            g_free(resp);
            goto cleanup;
        } else if (g_strcmp0(name, "users") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("auth.user.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("auth.user.create", body);
            else if (g_strcmp0(method, "DELETE") == 0)
                rpc = _build_rpc("auth.user.delete", body);
        } else if (g_strcmp0(name, "role") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("auth.role.set", body);
        }
    }

    /* ── A+B: /api/v1/config — 설정 백업/이력 ────────────── */
    else if (g_strcmp0(resource, "config") == 0) {
        if (g_strcmp0(name, "backup") == 0) {
            if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("config.backup", body);
            else if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("config.backup", NULL);
        }
        else if (g_strcmp0(name, "history") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("config.history", NULL);
        /* GET /config/daemon — daemon.conf 설정 조회 */
        else if (g_strcmp0(name, "daemon") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("daemon.config.get", body);
        /* PUT /config/daemon — daemon.conf 설정 변경 */
        else if (g_strcmp0(name, "daemon") == 0 && g_strcmp0(method, "PUT") == 0)
            rpc = _build_rpc("daemon.config.set", body);
    }

    /* ── A+B: /api/v1/templates — 템플릿 관리/이력 ──────── */
    else if (g_strcmp0(resource, "templates") == 0) {
        if (g_strcmp0(name, "history") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("template.history", NULL);
        else if (*name == '\0') {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("template.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("template.create", body);
        } else if (*name != '\0') {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc_name("template.get", name);
            else if (g_strcmp0(method, "DELETE") == 0)
                rpc = _build_rpc_name("template.delete", name);
        }
    }

    /* ── A+B: /api/v1/gpu — GPU 목록/메트릭 ──────────────── */
    else if (g_strcmp0(resource, "gpu") == 0) {
        if (g_strcmp0(name, "list") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("gpu.list", body);
        else if (g_strcmp0(name, "metrics") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("gpu.metrics", NULL);
        /* gpu.passthrough / gpu.mdev.create — 삭제됨 (v1.0, 하드웨어 의존) */
    }

    /* docker / terraform REST 라우트 — 삭제됨 (v1.0, placeholder 전량 제거) */

    /* ── /api/v1/vnc/{vm_name} — VNC 정보 ──────────────── */
    else if (g_strcmp0(resource, "vnc") == 0 && *name != '\0') {
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_id", name);
        rpc = _build_rpc("get_vnc_info", p);
        json_object_unref(p);
    }

    /* ── 매칭 없음 ───────────────────────────────────────── */
    if (!rpc) {
        _error(msg, 404, "NOT_FOUND",
               "Endpoint not found or method not allowed");
        goto cleanup;
    }

    PCV_LOG_INFO(REST_LOG_DOM, "→ RPC %.100s", rpc);
    {
        PcvRole caller_role = pcv_rbac_get_role(subject);
        gchar *rpc_with_context = _rpc_attach_auth_context(rpc, subject, caller_role);
        g_free(rpc);
        rpc = rpc_with_context;
    }

    /* ── RBAC 권한 검사: subject의 역할이 해당 RPC 메서드를 허용하는지 확인 ──
     *
     * [RBAC란?]
     * Role-Based Access Control (역할 기반 접근 제어).
     * 사용자 역할에 따라 허용되는 작업이 다릅니다:
     *   VIEWER: 조회만 가능 (vm.list, vm.metrics 등)
     *   OPERATOR: 조회 + 조작 가능 (vm.start, vm.stop 등)
     *   ADMIN: 모든 작업 가능 (auth.user.create, cluster.maintenance 등)
     *
     * [어떻게?]
     * RPC JSON 문자열에서 "method":"xxx" 부분을 추출하여
     * pcv_rbac_check_permission(사용자명, RPC메서드)로 권한을 확인합니다.
     * 권한 없으면 403 Forbidden 즉시 반환.
     *
     * [P0 수정 — Fail-Secure 패턴 (2026-04-10)]
     * 이전: method 파싱 실패 시 RBAC 검사를 건너뛰고 요청을 통과시킴 (fail-open)
     *   → VIEWER가 조작된 JSON으로 ADMIN 전용 RPC를 실행할 수 있었음!
     * 이후: method 파싱 실패 시 즉시 400 Bad Request 반환 (fail-secure)
     *   → 파싱 불가 = 권한 불확실 = 거부. 보안의 기본 원칙.
     *
     * [왜 SOUP_MEMORY_STATIC을 사용하는가?]
     * 에러 응답 문자열이 컴파일 타임 상수(문자열 리터럴)이기 때문입니다.
     * SOUP_MEMORY_STATIC: libsoup이 문자열을 복사하지 않고 포인터만 저장.
     *   → 힙 할당 없이 즉시 응답 가능 (보안 경로에서 성능 중요).
     * SOUP_MEMORY_COPY: 동적 문자열일 때 사용 (libsoup이 내부 복사).
     * 혼동하면: STATIC에 동적 문자열 전달 → use-after-free 크래시.
     */
    {
        /* rpc JSON에서 "method":"xxx" 추출 — 정규 JSON 파싱 대신 문자열 검색 (성능)
         * SECURITY (2026-04-10): fail-secure — method 파싱 실패 시 400 거부 (bypass 차단) */
        const gchar *mp = strstr(rpc, "\"method\":\"");
        if (!mp) {
            g_message("[RBAC] reject: missing method field in RPC envelope");
            g_free(rpc); rpc = NULL;
            soup_server_message_set_status(msg, 400, "Bad Request");
            soup_server_message_set_response(msg, "application/json",
                SOUP_MEMORY_STATIC,
                "{\"error\":{\"code\":\"BAD_REQUEST\",\"message\":\"Missing method field\"}}",
                64);
            goto cleanup;
        }
        mp += 10;
        const gchar *me = strchr(mp, '"');
        if (!me || me == mp) {
            g_message("[RBAC] reject: malformed method field in RPC envelope");
            g_free(rpc); rpc = NULL;
            soup_server_message_set_status(msg, 400, "Bad Request");
            soup_server_message_set_response(msg, "application/json",
                SOUP_MEMORY_STATIC,
                "{\"error\":{\"code\":\"BAD_REQUEST\",\"message\":\"Malformed method field\"}}",
                66);
            goto cleanup;
        }
        {
            gchar *rpc_method = g_strndup(mp, (gsize)(me - mp));
            if (!pcv_rbac_check_permission(subject, rpc_method)) {
                g_message("[RBAC] denied: user=%s method=%s", subject, rpc_method);
                g_free(rpc_method);
                g_free(rpc);
                rpc = NULL;
                soup_server_message_set_status(msg, 403, "Forbidden");
                soup_server_message_set_response(msg, "application/json",
                    SOUP_MEMORY_STATIC,
                    "{\"error\":{\"code\":\"FORBIDDEN\",\"message\":\"Insufficient permissions\"}}",
                    69);
                goto cleanup;
            }
            g_free(rpc_method);
        }
    }

    /* ══════════════════════════════════════════════════════════════
     * 비동기 RPC 실행 — REST 스레드 비점유
     *
     * [문제] _rpc_over_uds()는 동기 블로킹으로 UDS 응답을 최대 2초 대기.
     *        REST 서버가 단일 GMainLoop 스레드이므로, 이 대기 동안
     *        새 HTTP 요청의 accept()가 중단되어 CLOSE-WAIT가 누적됨.
     *
     * [해결] soup_server_message_pause()로 HTTP 응답을 보류하고,
     *        GTask 워커 스레드에서 _rpc_over_uds()를 실행한 후,
     *        rest_ctx에서 unpause + 응답 전송.
     *        REST 스레드는 즉시 반환하여 다음 요청을 accept할 수 있음.
     * ══════════════════════════════════════════════════════════════ */
    {
        /* [비동기 실행 컨텍스트 구성]
         * g_object_ref(msg): HTTP 메시지의 참조 카운트를 증가시킵니다.
         *   이유: _on_request() 반환 후에도 워커 스레드에서 msg를 사용해야 하므로
         *   GLib이 msg를 premature 해제하지 않도록 보호합니다.
         *   워커에서 작업 완료 후 g_object_unref()로 참조를 반환합니다. */
        _RestAsyncCtx *actx = g_new0(_RestAsyncCtx, 1);
        actx->msg = g_object_ref(msg);  /* 참조 카운트 +1 (소멸 방지) */
        actx->rpc = rpc;               /* 소유권 이전 — actx에서 g_free() 해제 */
        /* BE-A6: RPC 메서드명 추출 — 타임아웃 오버라이드용
         * rpc JSON에서 "method":"xxx" 패턴을 빠르게 추출 (JSON 파싱 오버헤드 회피) */
        {
            const gchar *mp = rpc ? strstr(rpc, "\"method\":\"") : NULL;
            if (mp) {
                mp += 10;  /* "method":" 다음 */
                const gchar *me = strchr(mp, '"');
                if (me) actx->rpc_method = g_strndup(mp, (gsize)(me - mp));
            }
        }
        actx->vm_delete_name = vm_delete_name; /* 소유권 이전 */
        actx->is_vm_delete = is_vm_delete;
        actx->rest_ctx = self_rest_ctx; /* REST GMainContext (unpause 디스패치용) */

        /* [핵심] soup_server_message_pause(): HTTP 응답 전송을 보류합니다.
         * 이 호출 후 _on_request()가 return해도 libsoup은 HTTP 응답을 보내지 않습니다.
         * soup_server_message_unpause() 호출 시 비로소 응답이 전송됩니다.
         * → REST 스레드가 UDS 응답 대기 없이 즉시 다음 요청을 처리할 수 있습니다. */
        soup_server_message_pause(msg);

        /* [GTask — GLib 비동기 태스크 프레임워크]
         * g_task_run_in_thread(): GLib의 스레드풀에서 _rest_async_worker를 실행합니다.
         * GLib이 내부적으로 스레드풀을 관리하므로 직접 g_thread_new()를 호출하지 않습니다.
         * g_object_unref(task): GTask의 참조를 해제합니다 (실행 중인 워커에는 영향 없음). */
        GTask *task = g_task_new(NULL, NULL, NULL, NULL);
        g_task_set_task_data(task, actx, _rest_async_ctx_free);
        g_task_run_in_thread(task, _rest_async_worker);
        g_object_unref(task);
    }

    /* rpc, vm_delete_name 소유권이 actx로 이전되었으므로 여기서 해제하지 않음 */
    goto cleanup_no_free;

    /* ── cleanup 레이블 — 리소스 해제 패턴 ──────────────────────────
     *
     * [왜 goto cleanup 패턴을 사용하는가?]
     * C 언어에는 try-finally나 defer가 없으므로, goto로 공통 해제 블록을 만듭니다.
     * 함수의 여러 분기점(에러, 정상 종료)에서 모두 같은 해제 코드를 실행합니다.
     * 이 패턴은 Linux 커널 코드에서도 널리 사용됩니다.
     *
     * [cleanup vs cleanup_no_free]
     * cleanup: 비동기 RPC를 실행하지 않은 경우 — rpc, vm_delete_name을 여기서 해제
     * cleanup_no_free: 비동기 RPC를 실행한 경우 — rpc, vm_delete_name은 actx가 소유
     *   actx의 워커 스레드(_rest_async_worker)에서 해제됩니다.
     * ──────────────────────────────────────────────────────────── */
cleanup:
    g_strfreev(segs);               /* URL 세그먼트 배열 해제 */
    if (body) json_object_unref(body);  /* JSON 요청 바디 해제 */
    g_free(subject);                /* JWT subject(사용자명) 해제 */
    g_free(rpc);                    /* JSON-RPC 요청 문자열 해제 */
    g_free(vm_delete_name);         /* VM 삭제 이름 해제 */
    g_free(req_id);                 /* 요청 ID 해제 */
    pcv_trace_context_free(trace_ctx);  /* W3C Trace Context 해제 */
    pcv_log_req_id_set(NULL);       /* TLS(Thread-Local Storage) 요청 ID 초기화 */
    return;

cleanup_no_free:
    g_strfreev(segs);
    if (body) json_object_unref(body);
    g_free(subject);
    /* rpc, vm_delete_name은 actx가 소유 — 워커 스레드에서 해제됨 */
    g_free(req_id);
    pcv_trace_context_free(trace_ctx);
    pcv_log_req_id_set(NULL);
}

/*
 * ═══════════════════════════════════════════════════════════════
 * GObject 생명주기 — PcvRestServer
 *
 * REST 서버는 GObject를 상속받아 참조 카운팅으로 수명을 관리합니다.
 * g_object_unref()로 참조 카운트가 0이 되면 finalize가 호출됩니다.
 * ═══════════════════════════════════════════════════════════════
 */

/**
 * GObject finalize — REST 서버 자원 해제
 *
 * 해제 순서:
 *   1. GMainLoop 종료 요청 (g_main_loop_quit)
 *   2. REST 스레드 종료 대기 (g_thread_join — 블로킹)
 *   3. GMainLoop/GMainContext 해제
 *   4. SoupServer disconnect + 해제
 *   5. 부모 클래스 finalize 체이닝
 */
static void
pcv_rest_server_finalize(GObject *object)
{
    PcvRestServer *self = PCV_REST_SERVER(object);
    if (self->rest_loop) {
        g_main_loop_quit(self->rest_loop);
        if (self->thread) {
            g_thread_join(self->thread);
            self->thread = nullptr;
        }
        g_main_loop_unref(self->rest_loop);
        self->rest_loop = nullptr;
    }
    if (self->rest_ctx) {
        g_main_context_unref(self->rest_ctx);
        self->rest_ctx = nullptr;
    }
    if (self->soup) {
        soup_server_disconnect(self->soup);
        g_object_unref(self->soup);
    }
    G_OBJECT_CLASS(pcv_rest_server_parent_class)->finalize(object);
}

static void pcv_rest_server_class_init(PcvRestServerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = pcv_rest_server_finalize;
}

static void pcv_rest_server_init(PcvRestServer *self)
{
    self->soup       = nullptr;
    self->port       = 8080;
    self->https_port = 443;
    self->tls_active = FALSE;
    self->thread     = nullptr;
    self->rest_loop  = nullptr;
    self->rest_ctx   = nullptr;
}

/**
 * REST 전용 GThread 진입점 — 별도 스레드에서 HTTP 이벤트 루프 실행
 *
 * 왜 별도 스레드인가:
 *   _on_request()에서 _rpc_over_uds()가 동기 블로킹 I/O를 수행합니다.
 *   이것이 메인 GMainLoop에서 실행되면, UDS 서버(같은 GMainLoop)가
 *   응답을 보낼 수 없어 데드락이 발생합니다.
 *   별도 GMainContext + GMainLoop + GThread로 격리하여 해결합니다.
 *
 * g_main_context_push_thread_default():
 *   이 스레드의 기본 GMainContext를 rest_ctx로 설정합니다.
 *   SoupServer가 이 context에 등록된 이벤트 소스를 처리합니다.
 */
static gpointer
_rest_thread_func(gpointer data)
{
    PcvRestServer *self = (PcvRestServer *)data;
    /* 이 스레드의 기본 GMainContext를 rest_ctx로 설정 */
    g_main_context_push_thread_default(self->rest_ctx);
    g_main_loop_run(self->rest_loop);
    g_main_context_pop_thread_default(self->rest_ctx);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * 공개 API — main.c에서 호출하는 REST 서버 생성/시작/중지 함수
 * ═══════════════════════════════════════════════════════════════ */

/**
 * REST 서버 인스턴스 생성
 *
 * 실제 리스닝은 시작하지 않습니다 (pcv_rest_server_start()에서 시작).
 * dispatcher 파라미터는 레거시 호환성을 위해 존재하지만 미사용입니다.
 * REST 서버는 UDS 소켓을 통해 디스패처에 접근하므로 직접 참조가 불필요합니다.
 *
 * @param dispatcher 디스패처 (미사용 — UDS 브릿지 방식)
 * @param port       리스닝 포트 (0이면 daemon.conf의 rest_port 사용, 기본 8080)
 * @return 새 PcvRestServer 인스턴스 (호출자가 g_object_unref()로 해제)
 */
PcvRestServer *
pcv_rest_server_new(PureCVisorDispatcher *dispatcher __attribute__((unused)),
                    guint16 port)
{
    PcvRestServer *self = g_object_new(PCV_TYPE_REST_SERVER, NULL);
    self->port = (port > 0) ? port : (guint16)pcv_config_get_rest_port();
    return self;
}

/**
 * REST 서버 시작 — HTTP 리스닝 + 전용 이벤트 루프 스레드 시작
 *
 * 동작 순서:
 *   1. 독립 GMainContext + GMainLoop 생성
 *   2. rest_ctx를 현재 스레드의 기본 context로 임시 설정 (push)
 *   3. SoupServer 생성 + 핸들러 등록 (_on_request, WebSocket)
 *   4. soup_server_listen_all()로 포트 바인딩 (rest_ctx에 소스 등록)
 *   5. rest_ctx를 pop하여 원래 context 복원
 *   6. REST 전용 GThread 시작 → _rest_thread_func()에서 이벤트 루프 실행
 *
 * 반드시 UDS 서버 시작 이후에 호출해야 합니다.
 * (REST 서버가 UDS 소켓에 연결하므로 소켓이 존재해야 함)
 *
 * @param self  REST 서버 인스턴스
 * @param error 에러 반환 (포트 바인딩 실패 등)
 * @return TRUE=성공, FALSE=실패
 */
gboolean
pcv_rest_server_start(PcvRestServer *self, GError **error)
{
    /* ── 독립 GMainContext + GMainLoop 생성 ─────────────────────
     * soup_server_listen_all은 현재 스레드의 default GMainContext에
     * 소켓 이벤트 소스를 등록합니다. 여기서 rest_ctx를 push한 상태에서
     * listen을 호출하면, 이후 _rest_thread_func에서 rest_ctx를 사용하는
     * GMainLoop가 HTTP 이벤트를 단독 처리합니다.
     * 
     * 이로써 _on_request → _rpc_over_uds 블로킹 I/O가 메인 GMainLoop를
     * 멈추지 않고, 메인 GMainLoop의 UDS 서버가 자유롭게 응답합니다. */
    self->rest_ctx  = g_main_context_new();
    self->rest_loop = g_main_loop_new(self->rest_ctx, FALSE);
    self_rest_ctx   = self->rest_ctx;  /* 비동기 워커에서 unpause 디스패치용 */

    /* 이 스레드(main)의 default context를 임시로 rest_ctx로 교체하여
     * soup_server_listen_all이 rest_ctx에 소스를 등록하도록 합니다. */
    g_main_context_push_thread_default(self->rest_ctx);

    /* SoupServer 생성 — libsoup3 HTTP 서버
     *
     * [SoupServer란?]
     * GNOME의 libsoup 라이브러리가 제공하는 HTTP 서버 구현체입니다.
     * GMainLoop 이벤트 루프와 통합되어 비동기 I/O를 지원합니다.
     * "server-header": HTTP 응답의 Server 헤더 값 (예: "Server: PureCVisord/1.0")
     * NULL: GObject 프로퍼티 리스트 종료 표시 (가변 인자 함수) */
    self->soup = soup_server_new("server-header", "PureCVisord/1.0", NULL);

    /*
     * 핸들러 등록: 두 경로 접두사에 대해 동일한 _on_request를 등록합니다.
     * /api/v1: REST API 엔드포인트
     * /ui: Web UI 정적 파일 (HTML/JS/CSS)
     * SoupServer는 가장 긴 접두사 매칭으로 핸들러를 선택합니다.
     */
    soup_server_add_handler(self->soup, REST_API_PREFIX,
                             _on_request, self, NULL);
    soup_server_add_handler(self->soup, "/ui",
                             _on_request, self, NULL);
    soup_server_add_handler(self->soup, "/",
                             _on_request, self, NULL);

    /* WebSocket 핸들러 등록 — 이벤트 스트림 + VNC 프록시 (ws_server.c) */
    pcv_ws_server_init(self->soup);

    /* ── HTTPS 설정: TLS 활성 시 GTlsCertificate 바인딩 ────────── */
    if (pcv_tls_is_enabled()) {
        const gchar *cert_path = pcv_tls_get_cert_path();
        const gchar *key_path  = pcv_tls_get_key_path();
        if (cert_path && key_path) {
            GError *tls_err = nullptr;
            GTlsCertificate *tls_cert =
                g_tls_certificate_new_from_files(cert_path, key_path, &tls_err);
            if (tls_cert) {
                soup_server_set_tls_certificate(self->soup, tls_cert);
                g_object_unref(tls_cert);
                PCV_LOG_INFO(REST_LOG_DOM, "TLS certificate loaded: %s", cert_path);
            } else {
                PCV_LOG_WARN(REST_LOG_DOM, "TLS cert load failed: %s — HTTPS disabled",
                             tls_err ? tls_err->message : "unknown");
                if (tls_err) g_error_free(tls_err);
            }
        }
    }

    /* ── HTTP 리스닝 (항상 활성) ──────────────────────────────── */
    /* HTTP 리스닝 시작
     *
     * [어떻게?]
     * soup_server_listen_all(): 모든 네트워크 인터페이스(0.0.0.0)에서 리스닝합니다.
     * SOUP_SERVER_LISTEN_IPV4_ONLY: IPv4만 사용 (IPv6 비활성)
     *
     * [중요] 이 시점에서 rest_ctx가 push되어 있으므로, libsoup은 소켓 이벤트 소스를
     * rest_ctx에 등록합니다. 이후 REST 전용 스레드에서 rest_ctx의 GMainLoop를 실행하면
     * 해당 스레드에서 HTTP 요청을 처리합니다. */
    GError *lerr = nullptr;
    gboolean ok = soup_server_listen_all(self->soup, self->port,
                                          SOUP_SERVER_LISTEN_IPV4_ONLY,
                                          &lerr);
    /* listen backlog 강제 확대 + SO_REUSEADDR
     *
     * [listen backlog란?]
     * accept() 호출 전에 커널이 대기시킬 수 있는 TCP 연결 수입니다.
     * libsoup 기본값은 10 — 동시에 11번째 연결 요청이 오면 거부됩니다.
     * Prometheus(15초마다), keepalived(1초마다), 브라우저 등이 동시에 접속하면
     * 쉽게 10을 초과합니다.
     *
     * [왜 1024인가?]
     * 1024는 리눅스 커널의 일반적인 최대 backlog 값(net.core.somaxconn)입니다.
     * 이 값을 초과하면 커널이 자동으로 somaxconn 값으로 절삭합니다.
     *
     * [SO_REUSEADDR]
     * 데몬 재시작 시 이전 소켓이 TIME-WAIT 상태로 남아있을 수 있습니다.
     * SO_REUSEADDR를 설정하면 같은 포트를 즉시 재사용할 수 있습니다.
     * (설정하지 않으면 "Address already in use" 에러로 시작 실패) */
    {
        GSList *listeners = soup_server_get_listeners(self->soup);
        for (GSList *l = listeners; l; l = l->next) {
            int fd = g_socket_get_fd(G_SOCKET(l->data));
            if (fd >= 0) {
                listen(fd, 1024);
                int optval = 1;
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
            }
        }
        g_slist_free(listeners);
    }
    if (!ok) {
        g_main_context_pop_thread_default(self->rest_ctx);
        g_propagate_error(error, lerr);
        g_main_loop_unref(self->rest_loop);
        g_main_context_unref(self->rest_ctx);
        self->rest_loop = nullptr;
        self->rest_ctx  = nullptr;
        return FALSE;
    }

    /* ── HTTPS 리스닝 (TLS 인증서 로드 성공 시) ──────────────── */
    if (pcv_tls_is_enabled() && soup_server_get_tls_certificate(self->soup)) {
        GError *tls_lerr = nullptr;
        gboolean tls_ok = soup_server_listen_all(self->soup, self->https_port,
                                                   SOUP_SERVER_LISTEN_IPV4_ONLY |
                                                   SOUP_SERVER_LISTEN_HTTPS,
                                                   &tls_lerr);
        if (tls_ok) {
            self->tls_active = TRUE;
            /* HSTS는 공인 인증서 환경에서만 활성화 — daemon.conf [tls] hsts=true */
            {
                const gchar *hsts_cfg = pcv_config_get_string("tls", "hsts", "false");
                if (g_strcmp0(hsts_cfg, "true") == 0 || g_strcmp0(hsts_cfg, "1") == 0)
                    g_hsts_enabled = TRUE;
            }
            PCV_LOG_INFO(REST_LOG_DOM, "HTTPS listening on https://0.0.0.0:%u (HSTS enabled)",
                         self->https_port);
            /* libsoup3 automatically negotiates HTTP/2 via ALPN on TLS connections.
             * Clients supporting h2 (browsers, curl --http2) will use multiplexed
             * HTTP/2 streams; others fall back to HTTP/1.1 transparently. */
            PCV_LOG_INFO(REST_LOG_DOM, "HTTP/2 support enabled (TLS via ALPN negotiation)");
        } else {
            PCV_LOG_WARN(REST_LOG_DOM, "HTTPS listen failed on port %u: %s",
                         self->https_port,
                         tls_lerr ? tls_lerr->message : "unknown");
            if (tls_lerr) g_error_free(tls_lerr);
        }
    }

    g_main_context_pop_thread_default(self->rest_ctx);

    /* REST 전용 스레드 시작 */
    self->thread = g_thread_new("purecvisor-rest", _rest_thread_func, self);

    if (self->tls_active) {
        PCV_LOG_INFO(REST_LOG_DOM,
                     "REST API listening on http://0.0.0.0:%u + https://0.0.0.0:%u%s",
                     self->port, self->https_port, REST_API_PREFIX);
    } else {
        PCV_LOG_INFO(REST_LOG_DOM,
                     "REST API listening on http://0.0.0.0:%u%s (thread: %p)",
                     self->port, REST_API_PREFIX, (void *)self->thread);
    }
    return TRUE;
}

/**
 * REST 서버 중지 — HTTP 리스닝 중단 + 이벤트 루프 스레드 종료
 *
 * 순서:
 *   1. SoupServer disconnect (새 연결 거부)
 *   2. GMainLoop quit (이벤트 루프 종료 요청)
 *   3. GThread join (스레드 종료 대기 — 블로킹)
 *
 * 진행 중인 _rpc_over_uds() 호출은 타임아웃(30초)까지 대기합니다.
 */
void
pcv_rest_server_stop(PcvRestServer *self)
{
    if (self->soup) {
        soup_server_disconnect(self->soup);
    }
    if (self->rest_loop && g_main_loop_is_running(self->rest_loop)) {
        g_main_loop_quit(self->rest_loop);
    }
    if (self->thread) {
        g_thread_join(self->thread);
        self->thread = nullptr;
    }
    PCV_LOG_INFO(REST_LOG_DOM, "REST API server stopped");
}
