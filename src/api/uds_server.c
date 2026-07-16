/**
 * @file uds_server.c
 * @brief Unix Domain Socket (UDS) JSON-RPC 2.0 서버
 *
 * 아키텍처 위치:
 *   main.c가 uds_server_new() + uds_server_start()로 생성/시작합니다.
 *   클라이언트(pcvctl CLI, REST 브릿지)로부터 JSON-RPC 요청을 수신하고,
 *   dispatcher.c의 purecvisor_dispatcher_dispatch()에 전달합니다.
 *   모든 RPC 요청의 최초 진입점이며, REST API도 결국 이 UDS 소켓을 경유합니다.
 *
 *   [pcvctl]        ─┐
 *                     ├─ UDS 소켓 ─→ [이 파일] ─→ [디스패처] ─→ [핸들러]
 *   [REST 서버]     ─┘
 *
 * 통신 프로토콜:
 *   소켓 경로: /var/run/purecvisor/daemon.sock (SOCK_STREAM)
 *   프로토콜: JSON-RPC 2.0 (개행 '\n' 구분자)
 *   연결 모델: Short-lived — 요청 1개 수신 → 응답 1개 전송 → 연결 종료
 *   이 모델 덕분에 fire-and-forget 패턴이 자연스럽게 동작합니다:
 *   핸들러가 응답을 보내면 소켓이 즉시 닫히므로, 비동기 GTask 콜백에서
 *   응답을 보내면 이미 닫힌 소켓에 쓰는 크래시가 발생합니다.
 *
 * 주요 흐름:
 *   uds_server_start()
 *     1. 기존 소켓 파일 삭제 (이전 크래시 잔여물 정리)
 *     2. 런타임 디렉터리 생성 (/var/run/purecvisor/)
 *     3. GSocketService 생성 + GUnixSocketAddress 바인딩
 *     4. "incoming" 시그널에 _on_incoming_connection 콜백 연결
 *     5. 소켓 파일 권한 설정 (chmod 0660)
 *
 *   _on_incoming_connection() — 클라이언트 연결 수락 시:
 *     1. pcv_drain_inc()로 inflight 카운터 증가 (종료 중이면 거부)
 *     2. GInputStream에서 비동기 read 시작 (g_input_stream_read_async)
 *     3. 읽기 완료 콜백에서 JSON 문자열을 디스패처에 전달
 *     4. 디스패처가 핸들러를 호출하고, 핸들러가 pure_uds_server_send_response()로 응답
 *     5. pcv_drain_dec()로 inflight 카운터 감소
 *
 *   pure_uds_server_send_response() — 핸들러가 호출:
 *     응답 JSON 문자열을 GOutputStream에 동기 쓰기 후 연결 종료
 *
 * io_uring 모드 (Phase U-2, PCV_USE_URING 플래그):
 *   GSocketService 대신 raw Unix socket + io_uring ACCEPT/READ 루프 사용.
 *   eventfd로 GMainLoop와 브릿지하여 기존 디스패처 파이프라인을 그대로 활용합니다.
 *   성능: UDS vm.list p50=3ms, 226 RPS (GSocketService 대비 동등 또는 우수).
 *
 * Socket Activation (systemd fd=3 계승):
 *   _sd_listen_fds()로 systemd가 전달한 FD를 감지할 수 있으나,
 *   소켓 경쟁 이슈로 현재는 비활성화하고 직접 소켓 생성 모드를 사용합니다.
 *
 * 주의사항:
 *   - pure_uds_server_send_response()는 응답 전송 후 소켓을 닫습니다.
 *     따라서 핸들러에서 이 함수를 2번 호출하면 두 번째 호출에서 크래시합니다.
 *   - 비동기 핸들러(fire-and-forget)는 반드시 응답을 먼저 보낸 후
 *     GTask로 백그라운드 작업을 시작해야 합니다.
 *   - 읽기 버퍼 크기는 64KB로, 이보다 큰 요청은 잘립니다.
 *
 * GObject 상속: UdsServer → GObject
 *   service, socket_path, dispatcher, connection_count 멤버 보유.
 *   io_uring 모드 시 uring, listen_fd, uring_mode 멤버 추가.
 */

/* ── 헤더 ─────────────────────────────────────────────────────── */
#include "uds_server.h"               /* UdsServer 공개 API 선언 */
#include "dispatcher.h"               /* purecvisor_dispatcher_dispatch() — RPC 라우팅 */
#include "drain.h"                    /* pcv_drain_inc()/dec() — graceful-drain inflight 카운터 (DISP-4) */
#include "../modules/dispatcher/rpc_utils.h"  /* pure_rpc_build_error_response() — 종료 중 거부 응답 */
#include <gio/gio.h>                  /* GSocketService, GInputStream 등 GIO 네트워킹 */
#include <gio/gunixsocketaddress.h>   /* g_unix_socket_address_new() — UDS 주소 */
#include <sys/stat.h>                 /* chmod() — 소켓 파일 권한 설정 */
#include <glib.h>                     /* g_message(), g_warning(), g_new0() 등 */
#include <unistd.h>                   /* unlink(), getpid(), close() */
#include <sys/socket.h>               /* getsockopt(), SO_PEERCRED, struct ucred — UDS 피어 신원 게이트 */
#include <errno.h>                    /* errno — getsockopt 실패 진단 */
#include <string.h>                   /* strerror() */
#include "../utils/pcv_log.h"         /* PCV_LOG_ERROR — SECURITY 마커 로그 */

#include "io/pcv_uring.h"            /* Phase U-2: io_uring 비동기 I/O */
#if PCV_USE_URING
#include <glib-unix.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

/* ═══════════════════════════════════════════════════════════════════
 * SO_PEERCRED root-only 접근 게이트 (Wave C Item 1 / A01·V8)
 *
 * 데몬·pcvctl·gRPC 는 전부 root 로 동작하므로, UDS 제어평면 소켓
 * (/var/run/purecvisor/daemon.sock)에 정당하게 붙는 피어는 root 뿐이다.
 * 소켓 0660(umask 0117) + 디렉토리 0700 으로 DAC 를 좁힌 위에, accept 된 각
 * 클라이언트 fd 의 피어 UID 를 커널이 보증하는 SO_PEERCRED 로 확인해 비-root 를
 * 차단한다(UID 위조 불가). GSocketService·io_uring 두 accept 경로 모두에 적용한다.
 *
 * [설계 결정] 이 게이트는 '접근'만 통제하고 caller role 은 설정하지 않는다. 기존 role
 *   로직(connection 데이터 없으면 params/기본 ADMIN)을 그대로 둬 gRPC 의 params
 *   operator 주입(Wave B)을 보존한다 — root 만 연결 가능해지면 params role 위조는
 *   무의미하다(root 는 이미 신뢰 경계 안).
 * [fail-closed] getsockopt 실패 시에도 거부한다.
 * ═══════════════════════════════════════════════════════════════════ */
#define UDS_LOG_DOM "uds_server"

/* accept 된 client fd 의 피어가 root(uid==0)면 TRUE, 아니면 FALSE(거부).
 * 조회 실패도 FALSE(fail-closed). role 은 건드리지 않는다(접근 게이트 전용). */
static gboolean _uds_peer_is_root(int fd)
{
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        PCV_LOG_ERROR(UDS_LOG_DOM,
                      "SECURITY: UDS 피어 자격 조회 실패 (SO_PEERCRED: %s) — 연결 거부",
                      strerror(errno));
        return FALSE;   /* fail-closed */
    }
    if (cred.uid != 0) {
        PCV_LOG_ERROR(UDS_LOG_DOM, "SECURITY: UDS 비-root 피어 거부 uid=%d", (int)cred.uid);
        return FALSE;
    }
    return TRUE;
}

/* ── graceful-drain 화이트리스트 (Task 5, DISP-4 후속) ────────────────────
 * DISP-4 의 수락-시 pcv_drain_inc() 게이트는 shutdown_flag 가 서면 모든 신규 연결을
 * 거부한다. 그러나 node.drain(=pcv_drain_begin(NULL,30), 프로세스 미종료)을 되돌리는
 * node.resume(=pcv_drain_cancel) 자기 연결까지 거부되면 RPC 로 복구 불가한 제어평면
 * brick 이 된다 — node.drain 문서("node.resume 으로 재개 가능")와 모순.
 *
 * _is_drain_exempt_method 는 거부 직전 요청 버퍼에서 method 를 최소 추출해 node.resume
 * 이면 예외(거부 skip → 정상 dispatch → pcv_drain_cancel 로 flag 리셋 → 이후 연결
 * 재수락)를 허용한다. method 추출은 디스패처 진입점(purecvisor_dispatcher_dispatch)과
 * 동일한 유일 sanctioned 파싱 경로 pcv_rpc_parse_guarded 를 재사용한다(full dispatch
 * 파싱 복제 금지). 파싱 실패·비객체·method 부재/비문자열은 모두 비예외(FALSE)로 폴백
 * → 기존대로 -32000 거부(무결성 우선, 크래시 없음). 할당한 JsonParser 는 항상 해제한다.
 *
 * [bounded-safe] node.resume 무조건 허용은 SIGTERM 드레인에도 안전하다: pcv_drain_cancel
 * 은 flag 만 리셋하며, SIGTERM 드레인 스레드는 실 GMainLoop 를 보유해 inflight==0 에서
 * g_main_loop_quit() 하므로 프로세스는 여전히 종료된다(drain-mode 구분 불요).
 *
 * @param buf  read 로 소비한 요청 JSON (NUL 종단 불요 — len 으로 경계 지정)
 * @param len  buf 의 유효 바이트 수 (>0)
 */
static gboolean _is_drain_exempt_method(const char *buf, gssize len)
{
    if (!buf || len <= 0)
        return FALSE;
    JsonParser *parser = NULL;                       /* FALSE 시 pcv_rpc_parse_guarded 가 NULL 로 세팅 → unref 불요 */
    if (!pcv_rpc_parse_guarded(buf, len, &parser, NULL))
        return FALSE;                                /* 파싱 실패/과대/과심 → 비예외(거부 유지) */
    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                      ? json_node_get_object(root) : NULL;
    const gchar *method = obj
        ? json_object_get_string_member_with_default(obj, "method", NULL)
        : NULL;                                       /* 부재/비문자열 → NULL (critical 없음) */
    gboolean exempt = (g_strcmp0(method, "node.resume") == 0);
    g_object_unref(parser);
    return exempt;
}

/* ═══════════════════════════════════════════════════════════════════
 * GObject 구조체 정의
 *
 * UdsServer는 GObject를 상속받는 C 객체입니다.
 * GObject 시스템은 참조 카운팅, 시그널, 프로퍼티 등을 제공합니다.
 *
 * 멤버 설명:
 *   service: GSocketService — GLib의 소켓 서비스 (accept 이벤트 루프)
 *   socket_path: 소켓 파일 경로 (기본: /var/run/purecvisor/daemon.sock)
 *   dispatcher: RPC 디스패처 (메서드 이름 → 핸들러 함수 매핑)
 *   connection_count: 현재까지 수락한 연결 수 (통계용)
 * ═══════════════════════════════════════════════════════════════════ */
struct _UdsServer {
    GObject parent_instance;           /* GObject 상속 — 반드시 첫 번째 멤버 */
    GSocketService *service;           /* GLib 소켓 서비스 (incoming 시그널 제공) */
    gchar *socket_path;                /* UDS 소켓 파일 경로 */
    PureCVisorDispatcher *dispatcher;  /* RPC 메서드 라우터 */
    guint16 connection_count;          /* 누적 연결 수 */
#if PCV_USE_URING
    PcvUringCtx *uring;               /* io_uring 컨텍스트 (Phase U-2) */
    int listen_fd;                     /* raw UDS listen socket fd */
    gboolean uring_mode;              /* TRUE = io_uring, FALSE = GSocketService */
#endif
};

/**
 * G_DEFINE_TYPE 매크로 — GObject 타입 시스템 등록
 *
 * 이 매크로가 자동으로 생성하는 것들:
 *   - uds_server_get_type() 함수
 *   - PURECVISOR_UDS_SERVER() 캐스팅 매크로
 *   - PURECVISOR_TYPE_UDS_SERVER 타입 상수
 *   - uds_server_parent_class 포인터
 */
G_DEFINE_TYPE(UdsServer, uds_server, G_TYPE_OBJECT)

/* ═══════════════════════════════════════════════════════════════════
 * GObject 라이프사이클 함수
 * ═══════════════════════════════════════════════════════════════════ */

/* ─────────────────────────────────────────────
 * uds_server_finalize — GObject 소멸자 (참조 카운트 0 → 자동 호출)
 *
 * [동작 흐름]
 *   1. io_uring 자원 해제 (uring 모드인 경우)
 *   2. GSocketService 중지 + 참조 해제
 *   3. socket_path 문자열 메모리 해제
 *   4. dispatcher 참조 해제
 *   5. 부모 클래스(GObject) finalize 체이닝
 *
 * [주의]
 *   - 부모 finalize 체이닝을 빠뜨리면 GObject 내부 메모리가 누수됩니다.
 *     G_OBJECT_CLASS(parent_class)->finalize(object)는 절대 생략하지 마세요.
 *   - finalize 순서가 중요합니다: 자식 자원 먼저 해제 → 부모 finalize 마지막.
 *   - g_object_unref()는 finalize를 호출할 수도, 안 할 수도 있습니다.
 *     참조 카운트가 1일 때만 finalize가 실행됩니다.
 *
 * [관련 함수] uds_server_init, uds_server_class_init
 * ─────────────────────────────────────────────*/
static void uds_server_finalize(GObject *object) {
    UdsServer *self = PURECVISOR_UDS_SERVER(object);

#if PCV_USE_URING
    if (self->uring) {
        pcv_uring_free(self->uring);
        self->uring = NULL;
    }
    if (self->listen_fd >= 0) {
        close(self->listen_fd);
        self->listen_fd = -1;
    }
#endif

    /* [왜?] io_uring 자원은 커널 링 버퍼를 점유하므로 반드시 명시적 해제가 필요합니다.
     * listen_fd도 커널에서 소켓을 유지하고 있으므로 close()로 반환합니다.
     * NULL/음수 체크는 이미 해제된 자원의 이중 해제(double-free)를 방지합니다. */

    /* 소켓 서비스 중지 + 해제 */
    if (self->service) {
        g_socket_service_stop(self->service);
        g_object_unref(self->service);
    }

    /* 소켓 경로 문자열 해제 */
    g_free(self->socket_path);

    /* 디스패처 참조 해제 */
    if (self->dispatcher) g_object_unref(self->dispatcher);

    /* 부모 클래스(GObject)의 finalize 체이닝 — 필수! */
    G_OBJECT_CLASS(uds_server_parent_class)->finalize(object);
}

/* ─────────────────────────────────────────────
 * uds_server_class_init — GObject 클래스 초기화 (타입당 1회만 호출)
 *
 * [동작 흐름]
 *   1. GObjectClass의 finalize 가상 함수를 오버라이드
 *
 * [주의]
 *   - 이 함수는 타입 시스템에 의해 자동 호출됩니다. 직접 호출하지 마세요.
 *   - G_DEFINE_TYPE 매크로가 이 함수의 호출을 자동 등록합니다.
 *   - 새 GObject 프로퍼티나 시그널을 추가하려면 이 함수에서 등록합니다.
 *
 * [관련 함수] uds_server_init, uds_server_finalize, G_DEFINE_TYPE
 * ─────────────────────────────────────────────*/
static void uds_server_class_init(UdsServerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = uds_server_finalize;
}

/* ─────────────────────────────────────────────
 * uds_server_init — GObject 인스턴스 초기화 (g_object_new() 시 자동 호출)
 *
 * [동작 흐름]
 *   1. 모든 포인터 멤버를 NULL로, 정수 멤버를 0/-1로 초기화
 *
 * [주의]
 *   - g_object_new()가 이미 구조체를 0으로 memset하지만,
 *     명시적 초기화로 의도를 드러내고 가독성을 높입니다.
 *   - listen_fd = -1은 "아직 열리지 않은 fd"를 의미합니다.
 *     0은 유효한 fd(stdin)이므로 -1을 사용해야 합니다.
 *   - 실제 자원 할당(소켓 생성, dispatcher 연결)은
 *     uds_server_start()와 uds_server_set_dispatcher()에서 수행됩니다.
 *
 * [관련 함수] uds_server_new, uds_server_start
 * ─────────────────────────────────────────────*/
static void uds_server_init(UdsServer *self) {
    self->service = NULL;
    self->socket_path = NULL;
    self->dispatcher = NULL;
    self->connection_count = 0;
#if PCV_USE_URING
    self->uring = NULL;
    self->listen_fd = -1;
    self->uring_mode = FALSE;
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 * io_uring ACCEPT→READ→WRITE 루프 (Phase U-2)
 *
 * PCV_USE_URING=1이면 GSocketService 대신 raw fd + io_uring을 사용합니다.
 * 연결 상태 머신: ACCEPT → READ → dispatch → WRITE → close → re-ACCEPT
 * ═══════════════════════════════════════════════════════════════════ */
#if PCV_USE_URING

/* [왜?] io_uring은 비동기이므로, ACCEPT → READ → WRITE 단계 사이에
 * 연결 정보를 유지할 구조체가 필요합니다. GLib 경로의 ReadCtx와 동일한 역할이지만,
 * raw fd를 사용하므로 GSocketConnection 대신 int fd를 저장합니다.
 * 각 클라이언트 연결마다 하나씩 할당되고, 처리 완료 후 g_free()됩니다. */
typedef struct {
    UdsServer *server;        /* UDS 서버 인스턴스 — dispatcher 접근용 */
    int        fd;            /* client fd (accept에서 반환된 새 소켓) */
    gchar     *buffer;        /* 읽기 버퍼 (64KB, 클라이언트 JSON-RPC 요청 수신용) */
    gsize      buf_size;      /* 버퍼 크기 */
    gchar     *response;      /* dispatcher가 생성한 응답 (향후 WRITE 단계에서 사용) */
    gsize      resp_len;      /* 응답 길이 */
    gboolean   inflight_held; /* graceful-drain: 수락 시 pcv_drain_inc() 성공 여부.
                               * TRUE=inflight 증가함(정상 dispatch, read_cb 에서 dec 필요),
                               * FALSE=종료 중(요청 read 후 -32000 거부, dec 안 함). (DISP-4) */
} UringConnCtx;

/*
 * io_uring 콜백 함수 전방 선언
 *
 * io_uring은 비동기 I/O 완료 통지(CQE)를 콜백으로 전달합니다.
 * 상태 머신: ACCEPT → READ → (dispatch) → WRITE → close
 * 각 콜백이 다음 단계의 SQE(Submission Queue Entry)를 등록합니다.
 */
static void _uring_accept_cb(PcvUringCtx *uring, gint result, gpointer data);
static void _uring_read_cb(PcvUringCtx *uring, gint result, gpointer data);
static void _uring_write_cb(PcvUringCtx *uring, gint result, gpointer data);
static gboolean _uring_reaccept_retry(gpointer data);  /* DISP-3: accept 재무장 지연 재시도 */

/* [DISP-3] accept 재무장 실패(SQ 포화)를 즉시 재제출하면 포화된 SQ 에서 타이트루프가
 * 되므로, 이 지연 후 재시도한다(deferred re-arm). 재시도 콜백은 shutdown 가드된다. */
#define URING_ACCEPT_RETRY_MS 50

/* ─────────────────────────────────────────────
 * _uring_post_accept — io_uring ACCEPT 요청을 커널에 제출
 *
 * [동작 흐름]
 *   1. pcv_uring_submit_accept()로 SQE(Submission Queue Entry)를 큐에 추가
 *   2. 커널이 클라이언트 연결 수락 완료 시 _uring_accept_cb() 호출
 *
 * [왜?]
 *   io_uring은 시스템콜을 배치 처리하여 context switch 비용을 줄입니다.
 *   GSocketService의 "incoming" 시그널과 동일한 역할이지만,
 *   커널-유저 스페이스 전환이 최소화되어 고부하 환경에서 유리합니다.
 *
 * [주의]
 *   - 이 함수는 _uring_accept_cb() 내부에서도 호출됩니다 (재귀적 패턴).
 *     accept 완료 → 다음 accept 즉시 등록으로 끊김 없는 수락 루프를 형성합니다.
 *   - SQE 등록만 하고 즉시 반환합니다 (논블로킹).
 *
 * [관련 함수] _uring_accept_cb, _uring_listen_start
 * ─────────────────────────────────────────────*/
static void
_uring_post_accept(UdsServer *self)
{
    if (!pcv_uring_submit_accept(self->uring, self->listen_fd,
                                  NULL, NULL,
                                  _uring_accept_cb, self)) {
        /* [DISP-3] accept SQE 미제출(SQ 포화 등) → _uring_accept_cb 미발화 → accept
         * 루프 정지(liveness 결함, 누수 아님). 즉시 재제출은 포화 SQ 에서 타이트루프이므로
         * 지연 재시도를 스케줄한다. 이 3개 호출부(_uring_listen_start 첫 accept,
         * accept 에러 재무장, 성공 후 재무장) 전부 이 경로로 복구된다. */
        g_warning("[uring-uds] accept re-arm failed; scheduling retry in %dms",
                  URING_ACCEPT_RETRY_MS);
        g_timeout_add(URING_ACCEPT_RETRY_MS, _uring_reaccept_retry, self);
    }
}

/* ─────────────────────────────────────────────
 * _uring_reaccept_retry — accept 재무장 지연 재시도 (DISP-3, deferred re-arm)
 *
 * _uring_post_accept 가 SQ 포화로 실패하면 g_timeout_add 로 이 콜백을 예약한다.
 * one-shot: 성공하면 종료, 다시 실패하면 _uring_post_accept 가 새 타이머를 건다.
 * 단일스레드 GMainLoop 라 동시에 계류하는 재시도 타이머는 최대 1개(정지된 SQ 는 accept
 * CB 를 발화시키지 않으므로 재진입 없음) → 타임아웃 스톰 없음.
 *
 * [shutdown 가드 — 필수] pcv_drain_begin 후 uring ctx->running=FALSE 라 submit_accept
 * 는 영구 FALSE 를 반환한다. 가드 없이 무조건 리스케줄하면 테어다운 중 타임아웃 스톰이
 * 된다. shutdown 이거나 io_uring 경로가 이미 내려갔으면 재시도를 중단한다.
 * ─────────────────────────────────────────────*/
static gboolean
_uring_reaccept_retry(gpointer data)
{
    UdsServer *self = data;
    if (pcv_drain_is_shutdown() || !self->uring || !self->uring_mode)
        return G_SOURCE_REMOVE;   /* 재시도 중단 (타임아웃 스톰 방지) */
    _uring_post_accept(self);     /* 실패 시 내부에서 새 타이머 재등록 */
    return G_SOURCE_REMOVE;       /* one-shot */
}

/* ─────────────────────────────────────────────
 * _uring_accept_cb — io_uring ACCEPT 완료 콜백 (새 클라이언트 수락)
 *
 * [동작 흐름]
 *   1. result 검사: 양수 = 새 클라이언트 fd, 음수 = accept 에러
 *   2. UringConnCtx 할당 (서버, fd, 64KB 읽기 버퍼)
 *   3. READ SQE 등록 → 클라이언트가 보낸 JSON-RPC 데이터 읽기 시작
 *   4. _uring_post_accept() → 다음 ACCEPT 즉시 재등록 (파이프라인)
 *
 * [주의]
 *   - ECANCELED(-125)는 서버 종료 시 정상적으로 발생하는 에러입니다.
 *     이때는 경고 로그를 남기지 않고 조용히 재등록합니다.
 *   - 65536(64KB) 버퍼: 대부분의 JSON-RPC 요청을 수용하지만,
 *     초대형 VM XML이 포함된 요청은 잘릴 수 있습니다.
 *   - accept 실패 시에도 항상 _uring_post_accept()를 재호출합니다.
 *     재등록하지 않으면 서버가 영구적으로 새 연결을 받지 못합니다.
 *
 * [관련 함수] _uring_post_accept, _uring_read_cb
 * ─────────────────────────────────────────────*/
static void
_uring_accept_cb(PcvUringCtx *uring __attribute__((unused)), gint result, gpointer data)
{
    UdsServer *self = data;

    if (result < 0) {
        /* [에러 처리] accept 실패 시나리오:
         * -EMFILE: 프로세스 fd 한계 도달 → ulimit -n 증가 필요
         * -ENOMEM: 메모리 부족
         * -ECANCELED: 서버 종료로 인한 정상 취소 → 경고 불필요 */
        if (result != -ECANCELED)
            g_warning("[uring-uds] accept failed: %s", strerror(-result));
        _uring_post_accept(self);
        return;
    }

    int client_fd = result;

    /* ── SO_PEERCRED root-only 접근 게이트 (Wave C Item 1 / A01·V8) ──────
     * 비-root/조회실패 피어는 여기서 거부한다. 이 시점 client_fd 는 아직 어떤 GSocket
     * 에도 래핑되지 않았으므로 직접 close 로 소유권을 정리한다(drain inc 이전이라
     * inflight 미접촉). 거부가 accept 루프를 죽이면 안 되므로 반드시 재무장 후 return. */
    if (!_uds_peer_is_root(client_fd)) {
        close(client_fd);
        _uring_post_accept(self);
        return;
    }

    self->connection_count++;

    /* ── graceful-drain 게이트 (DISP-4, io_uring 경로) ──────────────────
     * [불변식] 연결 수락 시 pcv_drain_inc() ↔ _uring_read_cb 공통 cleanup 의
     *   pcv_drain_dec() 가 1:1로 짝을 이룬다(GLib 경로 on_incoming_connection ↔
     *   on_read_done 과 동일 규율, Option A: 수락 시 inc — 단일스레드 GMainLoop 에서
     *   inflight>0 이 drain-wait 유효 신호가 되는 지점).
     * [왜 read 후 거부인가] 종료 중이라도 여기서 즉시 write+close 로 거부하면, 요청을
     *   먼저 보내는 클라이언트(pcvctl/REST/`echo|nc` 전부)가 write 시 EPIPE 로 죽어
     *   거부 응답을 못 읽고 드롭된다(실측 확인). inc 결과를 ctx 에 실어 두고, 정상
     *   dispatch 와 동일하게 요청을 read 로 소비한 뒤 _uring_read_cb 에서 -32000 을
     *   전송해야 거부가 실제로 전달된다.
     * [주의] inc 이후 recv 제출 전에 조기 return 을 추가하면 dec 가 실행되지 않아
     *   inflight 가 누수되고 SIGTERM 이 drain timeout 까지 hang 한다. */
    gboolean inflight_held = pcv_drain_inc();

    /* 연결 컨텍스트 생성 */
    UringConnCtx *ctx = g_new0(UringConnCtx, 1);
    ctx->server        = self;
    ctx->fd            = client_fd;
    ctx->inflight_held = inflight_held;  /* FALSE면 read 후 -32000 거부, dec 안 함 */
    ctx->buf_size      = 65536;
    ctx->buffer        = g_malloc(ctx->buf_size);

    /* READ SQE 등록 (종료 중이어도 요청을 읽어 소비 → 거부 전달의 전제) */
    if (!pcv_uring_submit_recv(self->uring, client_fd,
                               ctx->buffer, ctx->buf_size - 1,
                               _uring_read_cb, ctx)) {
        /* [DISP-3] submit FALSE → _uring_read_cb 미발화(래퍼 계약: FALSE 면 pending 부재
         * → CQE 드롭). 콜백이 유일한 다른 소비자이므로 이중해제 없음 — 호출자가 fd·버퍼·
         * ctx·drain inflight 를 100% 소유·정리한다. fd 는 아직 GSocket 미래핑이라 아무도
         * 닫지 않으므로 여기서 close 한다. dec 는 inflight_held 일 때만(:375 pcv_drain_inc
         * 와 1:1, cleanup 라벨 :518 미러) — 무조건 dec 는 언더플로, 생략은 inflight 누수 →
         * SIGTERM 이 drain timeout 까지 hang. */
        close(client_fd);
        if (inflight_held)
            pcv_drain_dec();
        g_free(ctx->buffer);
        g_free(ctx);
        /* 조기 return 금지: 일시적 recv 제출 실패가 accept 루프까지 죽이면 안 된다.
         * 아래 _uring_post_accept 로 반드시 재무장까지 진행한다(connection_count 는 통계용
         * 이라 감소 불요). */
    }

    /* 다음 ACCEPT 즉시 재등록 (파이프라인) */
    _uring_post_accept(self);
}

/* ─────────────────────────────────────────────
 * _uring_read_cb — io_uring READ 완료 콜백 (JSON-RPC 요청 수신)
 *
 * [동작 흐름]
 *   1. result 검사: 양수 = 읽은 바이트 수, 0 = EOF, 음수 = 에러
 *   2. buffer에 NULL 종료 문자 추가 (C 문자열화)
 *   3. raw fd → GSocket → GSocketConnection 래핑 (기존 API 호환)
 *   4. dispatcher에 전달 → 핸들러 실행 → 응답 전송 → 소켓 닫힘
 *   5. UringConnCtx 메모리 해제
 *
 * [왜? — 핵심 설계 결정 (Phase U-2)]
 *   io_uring 모드에서도 기존 dispatcher 인터페이스를 유지하기 위해
 *   raw fd를 GSocketConnection으로 래핑합니다.
 *   이렇게 하면 200개+ 핸들러를 전혀 수정하지 않고 io_uring을 도입합니다.
 *   래핑 오버헤드(GSocket 객체 생성)는 핸들러 전면 수정 대비 무시할 수 있습니다.
 *
 * [주의]
 *   - g_socket_new_from_fd()가 fd의 소유권을 가져갑니다.
 *     이후 GSocketConnection이 close될 때 fd도 자동으로 닫힙니다.
 *     따라서 g_object_unref(conn) 후 close(ctx->fd)를 호출하면 안 됩니다.
 *   - result == 0 (EOF): 클라이언트가 연결만 맺고 데이터 없이 끊은 경우.
 *     pcvctl이 비정상 종료하면 이 경로를 탑니다.
 *
 * [관련 함수] _uring_accept_cb, purecvisor_dispatcher_dispatch, pure_uds_server_send_response
 * ─────────────────────────────────────────────*/
static void
_uring_read_cb(PcvUringCtx *uring __attribute__((unused)), gint result, gpointer data)
{
    UringConnCtx *ctx = data;

    if (result <= 0) {
        /* [에러 처리] EOF(0) 또는 읽기 에러(음수).
         * GSocket으로 래핑하기 전이므로 close(fd)를 직접 호출합니다. */
        close(ctx->fd);
        goto cleanup;  /* graceful-drain: 단일 dec 수렴점(DISP-4) */
    }

    ctx->buffer[result] = '\0';

    /* dispatcher 부재는 정상 처리 경로에서만 조기 종료. 거부 경로(inflight_held FALSE)는
     * dispatcher 없이도 -32000 을 보내야 하므로 아래 래핑으로 진행한다. */
    if (ctx->inflight_held && !ctx->server->dispatcher) {
        close(ctx->fd);
        goto cleanup;  /* graceful-drain: 단일 dec 수렴점(DISP-4) */
    }

    /*
     * dispatcher 호출 — 기존 GSocketConnection 기반 API와 호환 문제.
     *
     * 현재 dispatcher는 (UdsServer*, GSocketConnection*, buffer) 시그니처.
     * io_uring 모드에서는 GSocketConnection이 없으므로,
     * raw fd에서 GSocketConnection을 래핑하여 전달합니다.
     * send_response()에서 io_uring WRITE 대신 기존 write_all을 사용합니다.
     *
     * Phase U-2 전략: dispatcher 인터페이스는 변경하지 않고,
     * GSocket → GSocketConnection 래핑으로 호환성 유지.
     * 이렇게 하면 118개 핸들러를 수정하지 않아도 됩니다.
     */
    GError *sock_err = NULL;
    /* [왜?] raw fd → GSocket 래핑. g_socket_new_from_fd()는 fd의 소유권을 가져갑니다.
     * 이 시점부터 fd는 GSocket이 관리하므로 직접 close()하면 안 됩니다.
     * 실패하면 fd 소유권이 이전되지 않으므로 직접 close()해야 합니다. */
    GSocket *gsock = g_socket_new_from_fd(ctx->fd, &sock_err);
    if (!gsock) {
        /* [에러 처리] GSocket 래핑 실패 — 보통 잘못된 fd일 때 발생.
         * fd 소유권이 이전되지 않았으므로 직접 close() 필요. */
        if (sock_err) {
            g_warning("[uring-uds] GSocket wrap failed: %s", sock_err->message);
            g_error_free(sock_err);
        }
        close(ctx->fd);
        goto cleanup;  /* graceful-drain: 단일 dec 수렴점(DISP-4) */
    }

    /* [왜?] GSocket → GSocketConnection 래핑. 팩토리 패턴으로 소켓 타입에 맞는
     * GSocketConnection 서브클래스를 자동 생성합니다.
     * g_object_unref(gsock): conn이 gsock을 참조하므로 여기서 해제해도 안전합니다.
     * GObject 참조 카운팅 덕분에 conn이 해제될 때까지 gsock은 유지됩니다. */
    GSocketConnection *conn = g_socket_connection_factory_create_connection(gsock);
    g_object_unref(gsock);

    if (!conn) {
        close(ctx->fd);
        goto cleanup;  /* graceful-drain: 단일 dec 수렴점(DISP-4) */
    }

    /* graceful-drain 화이트리스트 (Task 5): 종료 중(inflight_held FALSE)이라도 method 가
     * node.resume 이면 거부 대신 정상 dispatch 로 흘려보내 제어평면을 복구한다. dispatcher
     * 부재 시엔 dispatch 불가하므로 거부로 폴백한다(위 no-dispatcher 조기종료는 inflight_held
     * TRUE 에만 걸리므로 여기서 명시 확인). 이 연결은 inflight_held FALSE 라 inflight 미카운트
     * — 아래 cleanup 의 dec skip 이 그대로 유지되어 drain-wait 불변식을 깨지 않는다. */
    gboolean drain_exempt = !ctx->inflight_held && ctx->server->dispatcher &&
                            _is_drain_exempt_method(ctx->buffer, result);

    if (!ctx->inflight_held && !drain_exempt) {
        /* ── graceful-drain 거부 (DISP-4) ──────────────────────────────
         * 수락 시 pcv_drain_inc() 가 FALSE(종료 중)였다. 요청을 read 로 소비한 뒤
         * (클라이언트 write EPIPE 방지) -32000 을 정상 응답 경로(write+close)로 전송.
         * inflight 미증가라 아래 cleanup 의 dec 는 건너뛴다(inflight_held FALSE). */
        gchar *rej = pure_rpc_build_error_response(NULL, PURE_RPC_ERR_ZFS_OPERATION,
                                                   "server is shutting down");
        pure_uds_server_send_response(ctx->server, conn, rej);
        g_free(rej);
    } else {
        /* dispatcher 호출 (기존 send_response가 write + close 처리).
         * inflight_held TRUE(정상 경로) 또는 drain 예외 node.resume — 어느 쪽이든
         * dispatcher 는 non-NULL(정상 경로는 위 no-dispatcher 조기종료 가드가, 예외는
         * drain_exempt 의 ctx->server->dispatcher 항이 보장). */
        purecvisor_dispatcher_dispatch(ctx->server->dispatcher,
                                       ctx->server,
                                       conn,
                                       ctx->buffer);
    }

    /* [왜?] dispatcher/거부 → send_response()가 g_io_stream_close()를 호출하므로
     * 소켓은 이미 닫힌 상태입니다. g_object_unref(conn)은 GObject 메모리만 해제합니다.
     * fd는 GSocketConnection이 close될 때 이미 커널에 반환되었습니다. */
    g_object_unref(conn);

cleanup:
    /* graceful-drain(DISP-4): _uring_accept_cb 의 pcv_drain_inc() 와 1:1 짝. 이 콜백의
     * 모든 반환경로(EOF/에러·no-dispatcher·래핑실패·conn 실패·dispatch·거부)가 이 단일
     * 지점으로 수렴한다. dec 는 inc 가 성공(inflight_held)했을 때만 — 거부 연결은
     * inflight 를 올리지 않았으므로 dec 하면 언더플로. */
    if (ctx->inflight_held)
        pcv_drain_dec();
    g_free(ctx->buffer);
    g_free(ctx);
}

/* ─────────────────────────────────────────────
 * _uring_listen_start — io_uring 모드 UDS 소켓 생성 및 리스닝 시작
 *
 * [동작 흐름]
 *   1. 기존 소켓 파일 삭제 (이전 크래시 잔여물 정리)
 *   2. socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
 *   3. umask(0117) → bind() → umask 복원 (소켓 권한 0660 원자 설정)
 *   4. listen(fd, 128) — backlog=128: 동시 대기 가능한 연결 수
 *   5. pcv_uring_new(PCV_URING_DEFAULT_QUEUE_DEPTH) — io_uring 커널 링 생성
 *   6. _uring_post_accept() — 첫 ACCEPT SQE 등록 (이후 연쇄 재등록)
 *
 * [왜?]
 *   GSocketService는 내부적으로 poll()/epoll()을 사용하는데,
 *   io_uring은 커널 공유 링 버퍼로 시스템콜 수를 줄여 성능이 우수합니다.
 *   실패 시 GSocketService로 자동 폴백하여 안정성을 보장합니다.
 *
 * [주의]
 *   - SOCK_NONBLOCK: io_uring은 논블로킹 fd를 요구합니다.
 *     블로킹 fd를 사용하면 io_uring 스레드가 멈출 수 있습니다.
 *   - SOCK_CLOEXEC: exec() 시 fd 누수를 방지합니다.
 *     자식 프로세스(pcv_spawn_sync)가 서버 소켓을 상속받으면 안 됩니다.
 *   - umask 방식으로 소켓 권한을 설정하는 이유:
 *     bind() → chmod() 사이에 다른 프로세스가 접근하는 TOCTOU 취약점 방지.
 *   - 이 함수가 FALSE를 반환하면 호출자(uds_server_start)가
 *     GSocketService 모드로 폴백합니다. 서버가 멈추지 않습니다.
 *
 * [관련 함수] uds_server_start, _uring_post_accept
 * ─────────────────────────────────────────────*/
static gboolean
_uring_listen_start(UdsServer *self, GError **error)
{
    /* 기존 소켓 파일 삭제 */
    if (g_file_test(self->socket_path, G_FILE_TEST_EXISTS))
        unlink(self->socket_path);

    /* [왜?] AF_UNIX: 같은 호스트 내 프로세스 간 통신 (네트워크 스택 우회, 고성능)
     * SOCK_STREAM: TCP처럼 순서 보장 + 바이트 스트림 (vs SOCK_DGRAM 데이터그램)
     * SOCK_NONBLOCK: io_uring이 자체적으로 블로킹을 관리하므로 fd는 논블로킹이어야 함
     * SOCK_CLOEXEC: fork+exec 시 자식에게 fd가 누수되지 않도록 방지 */
    self->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (self->listen_fd < 0) {
        g_set_error(error, g_quark_from_static_string("uds"), 1,
                    "socket() failed: %s", strerror(errno));
        return FALSE;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, self->socket_path, sizeof(addr.sun_path));

    /* umask(0117)로 소켓 권한을 원자적으로 0660(rw-rw----)으로 설정 (TOCTOU 방지).
     * root-only 접근은 SO_PEERCRED 게이트가 강제하고, 0660 은 심층 방어(DAC). */
    mode_t old_umask = umask(0117);

    if (bind(self->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        umask(old_umask);
        g_set_error(error, g_quark_from_static_string("uds"), 2,
                    "bind(%s) failed: %s", self->socket_path, strerror(errno));
        close(self->listen_fd);
        return FALSE;
    }

    umask(old_umask);

    if (listen(self->listen_fd, 128) < 0) {  // backlog=128: 동시 대기 연결 수
        g_set_error(error, g_quark_from_static_string("uds"), 3,
                    "listen() failed: %s", strerror(errno));
        close(self->listen_fd);
        return FALSE;
    }

    /* [왜?] queue_depth=1024: 동시에 커널에 제출할 수 있는 I/O 요청 수.
     * worker burst 중 SQE 고갈과 submit EAGAIN 재시도 진입을 낮추는 운영 기본값.
     * 너무 크면 커널 메모리를 낭비하고, 너무 작으면 I/O 요청이 대기합니다. */
    GError *uring_err = NULL;
    self->uring = pcv_uring_new(PCV_URING_DEFAULT_QUEUE_DEPTH, &uring_err);
    if (!self->uring) {
        g_set_error(error, g_quark_from_static_string("uds"), 4,
                    "io_uring init failed: %s",
                    uring_err ? uring_err->message : "unknown");
        if (uring_err) g_error_free(uring_err);
        close(self->listen_fd);
        return FALSE;
    }

    /* 첫 ACCEPT SQE 등록 */
    _uring_post_accept(self);

    self->uring_mode = TRUE;
    g_message("UDS Server listening on %s (io_uring mode, queue_depth=%u)",
              self->socket_path, PCV_URING_DEFAULT_QUEUE_DEPTH);
    return TRUE;
}

/* ─────────────────────────────────────────────
 * _uring_write_cb — io_uring WRITE 완료 콜백 (현재 미사용, 향후 확장용)
 *
 * [왜 비어있는가?]
 *   현재 응답 전송은 GSocketConnection의 write_all()을 사용합니다.
 *   io_uring 모드에서도 READ만 io_uring으로 처리하고,
 *   WRITE는 기존 GLib 경로를 그대로 사용합니다.
 *   향후 send_response()를 io_uring WRITE로 전환하면 이 콜백이 활성화됩니다.
 *
 * [주의]
 *   - __attribute__((unused)): GCC에게 "의도적으로 미사용"임을 알려 경고를 억제합니다.
 *   - 전방 선언(_uring_write_cb)은 위에서 이미 되어 있으므로 링커 에러는 없습니다.
 *
 * [관련 함수] pure_uds_server_send_response
 * ─────────────────────────────────────────────*/
static void __attribute__((unused))
_uring_write_cb(PcvUringCtx *uring __attribute__((unused)), gint result __attribute__((unused)),
                gpointer data __attribute__((unused)))
{
}

#endif /* PCV_USE_URING */

/* ═══════════════════════════════════════════════════════════════════
 * 비동기 읽기 컨텍스트 (ReadCtx) — GLib 경로
 *
 * 문제:
 *   on_incoming_connection()은 GMainLoop 스레드에서 실행됩니다.
 *   여기서 동기 read를 호출하면 main loop가 블로킹되어
 *   다른 GTask 콜백(vm.start 완료 등)이 실행되지 못합니다.
 *   → 데드락 발생!
 *
 * 해결:
 *   g_input_stream_read_async()로 비동기 읽기를 시작하고,
 *   읽기 완료 시 on_read_done() 콜백에서 디스패처를 호출합니다.
 *   ReadCtx 구조체에 서버/연결/버퍼 정보를 저장하여
 *   콜백에서 접근할 수 있게 합니다.
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    UdsServer         *server;      /* UDS 서버 인스턴스 (dispatcher 접근용) */
    GSocketConnection *connection;  /* 클라이언트 연결 (응답 전송용) */
    gchar             *buffer;      /* 읽기 버퍼 (64KB) */
    gsize              buf_size;    /* 버퍼 크기 */
    gboolean           inflight_held; /* graceful-drain: 수락 시 pcv_drain_inc() 성공 여부.
                                       * TRUE=정상 dispatch(on_read_done 에서 dec),
                                       * FALSE=종료 중(요청 read 후 -32000 거부, dec 안 함). (DISP-4) */
} ReadCtx;

/* ─────────────────────────────────────────────
 * on_read_done — 비동기 읽기 완료 콜백 (GLib 경로)
 *
 * [동작 흐름]
 *   1. g_input_stream_read_finish()로 읽기 결과 확인
 *   2. bytes_read > 0: NULL 종료 추가 → dispatcher에 JSON-RPC 문자열 전달
 *   3. bytes_read == 0: 클라이언트가 데이터 없이 연결 종료 (EOF)
 *   4. bytes_read < 0: 읽기 에러 (네트워크 장애 등)
 *   5. ReadCtx의 모든 자원(connection, buffer, ctx) 해제
 *
 * [주의]
 *   - 이 콜백은 GMainLoop 스레드에서 실행됩니다.
 *     dispatcher 내부에서 무거운 동기 작업을 하면 main loop가 블로킹됩니다.
 *     그래서 핸들러들은 GTask(fire-and-forget)로 비동기 실행합니다.
 *   - 메모리 소유권: ReadCtx가 connection과 buffer를 소유합니다.
 *     이 콜백에서 반드시 모든 것을 해제해야 합니다 (누수 방지).
 *   - g_object_unref(connection)를 dispatcher 호출 후에 해야 합니다.
 *     dispatcher가 connection을 사용하기 때문입니다.
 *
 * [관련 함수] on_incoming_connection, purecvisor_dispatcher_dispatch
 * ─────────────────────────────────────────────*/
static void on_read_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    ReadCtx *ctx = (ReadCtx *)user_data;
    GError *error = NULL;

    /* 비동기 읽기 결과 수신 */
    gssize bytes_read = g_input_stream_read_finish(G_INPUT_STREAM(source), res, &error);

    /* graceful-drain 화이트리스트 (Task 5): 종료 중이라도 method 가 node.resume 이면 거부
     * 대신 정상 dispatch(아래 else-if 분기)로 흘려보내 제어평면을 복구한다. 요청 버퍼는
     * 아직 NUL 종단 전이므로 bytes_read 로 경계를 지정한다. dispatcher 부재/read 실패(≤0)
     * 시엔 예외 불가하므로 거부로 폴백. 예외 연결은 inflight_held FALSE 라 미카운트 유지. */
    gboolean drain_exempt = !ctx->inflight_held && bytes_read > 0 &&
                            ctx->server->dispatcher &&
                            _is_drain_exempt_method(ctx->buffer, bytes_read);

    if (!ctx->inflight_held && !drain_exempt) {
        /* ── graceful-drain 거부 (DISP-4) ──────────────────────────────
         * 수락 시 pcv_drain_inc() 가 FALSE(종료 중)였다. 요청을 read 로 소비한 뒤
         * (클라이언트 write EPIPE 방지) -32000 을 정상 응답 경로(write+close)로 전송.
         * 수락 시점엔 요청 id 미수신이므로 id:null 이 JSON-RPC 2.0 규격상 정확하다.
         * inflight 미증가라 아래 cleanup 의 dec 는 건너뛴다. */
        gchar *rej = pure_rpc_build_error_response(NULL, PURE_RPC_ERR_ZFS_OPERATION,
                                                   "server is shutting down");
        pure_uds_server_send_response(ctx->server, ctx->connection, rej);
        g_free(rej);
    } else if (bytes_read > 0) {
        /* 읽기 성공: NULL 종료 문자 추가 후 디스패처에 전달 */
        ctx->buffer[bytes_read] = '\0';

        if (ctx->server->dispatcher) {
            /*
             * 디스패처 호출 — JSON-RPC 파싱 → 메서드 라우팅 → 핸들러 실행
             *
             * 파라미터:
             *   server: 응답 전송 시 pure_uds_server_send_response()에 전달
             *   connection: 응답을 보낼 소켓 연결
             *   buffer: JSON-RPC 요청 문자열
             */
            purecvisor_dispatcher_dispatch(ctx->server->dispatcher,
                                           ctx->server,
                                           ctx->connection,
                                           ctx->buffer);
        } else {
            g_warning("No dispatcher set for UdsServer");
            g_io_stream_close(G_IO_STREAM(ctx->connection), NULL, NULL);
        }
    } else {
        /* 읽기 실패 또는 EOF */
        if (bytes_read < 0 && error) {
            g_warning("UDS read error: %s", error->message);
        }
        /* bytes_read == 0: 클라이언트가 데이터 없이 연결을 닫음 */
        g_io_stream_close(G_IO_STREAM(ctx->connection), NULL, NULL);
    }

    if (error)
        g_error_free(error);

    /* 메모리 정리 — ReadCtx가 소유한 모든 자원 해제.
     * [graceful-drain, DISP-4] on_read_done 의 모든 분기(거부 · dispatch · no-dispatcher ·
     * read 오류/EOF)가 조기 return 없이 이 단일 지점으로 수렴한다. dec 는 수락 시 inc 가
     * 성공(inflight_held)했을 때만 — 거부 연결은 inflight 를 올리지 않았으므로 dec 하면
     * 언더플로. inc 성공 연결에 대해 여기 한 번의 dec 가 정확히 1:1 상쇄한다. */
    if (ctx->inflight_held)
        pcv_drain_dec();              /* 수락 시 inc 와 1:1 (on_incoming_connection 참조) */
    g_object_unref(ctx->connection);  /* 연결 참조 해제 */
    g_free(ctx->buffer);              /* 버퍼 메모리 해제 */
    g_free(ctx);                      /* 컨텍스트 구조체 해제 */
}

/* ─────────────────────────────────────────────
 * on_incoming_connection — 클라이언트 연결 수락 콜백 (GLib 경로)
 *
 * [동작 흐름]
 *   1. ReadCtx 할당 (서버, 연결, 64KB 버퍼)
 *   2. connection의 참조 카운트 증가 (비동기 콜백에서 안전하게 사용하기 위해)
 *   3. GInputStream에서 비동기 read 시작 (g_input_stream_read_async)
 *   4. 즉시 TRUE 반환 (main loop 블로킹 없음)
 *
 * [왜?]
 *   GSocketService의 "incoming" 시그널 핸들러입니다.
 *   GMainLoop의 이벤트 루프에서 호출되므로, 여기서 블로킹 작업을 하면
 *   전체 데몬(텔레메트리, REST, WebSocket 등)이 멈춥니다.
 *   반드시 비동기 read만 시작하고 즉시 반환해야 합니다.
 *
 * [주의]
 *   - g_object_ref(connection): GSocketService가 이 함수 반환 후
 *     connection을 해제할 수 있으므로, 비동기 콜백에서 사용하려면
 *     참조 카운트를 증가시켜야 합니다. on_read_done()에서 unref합니다.
 *   - buf_size - 1: NULL 종료 문자('\0')를 위한 1바이트 예약.
 *     이걸 빼먹으면 buffer overflow가 발생합니다.
 *   - return TRUE: FALSE를 반환하면 서비스가 중지되어 새 연결을 받지 못합니다.
 *
 * [관련 함수] on_read_done, uds_server_start (시그널 연결)
 * ─────────────────────────────────────────────*/
static gboolean on_incoming_connection(GSocketService *service,
                                       GSocketConnection *connection,
                                       GObject *source_object,
                                       gpointer user_data) {
    UdsServer *self = PURECVISOR_UDS_SERVER(user_data);

    (void)service;        /* 미사용 파라미터 — 컴파일러 경고 억제 */
    (void)source_object;

    /* ── SO_PEERCRED root-only 접근 게이트 (Wave C Item 1 / A01·V8) ──────
     * 비-root/조회실패 피어는 여기서 연결을 닫아 거부한다(drain inc 이전이라 inflight
     * 미접촉, ref 미증가). TRUE 를 반환해 서비스는 계속 새 연결을 수락한다. */
    int peer_fd = g_socket_get_fd(g_socket_connection_get_socket(connection));
    if (!_uds_peer_is_root(peer_fd)) {
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        return TRUE;
    }

    /* ── graceful-drain 게이트 (DISP-4, GLib 경로) ──────────────────────
     * [불변식] 연결 수락 시 pcv_drain_inc() ↔ on_read_done 공통 cleanup 의
     *   pcv_drain_dec() 가 1:1로 짝을 이룬다. 단일스레드 GMainLoop 에서 inflight>0 은
     *   "수락되었으나 아직 응답 완료 전"을 뜻하며, SIGTERM 시 drain 스레드가 이 카운터가
     *   0이 될 때까지 대기한다(Option A: 수락 시 inc — 스펙 Option B '디스패치 직전 inc'는
     *   동기 dispatch 중 SIGTERM starve 로 drain-wait 무효라 채택하지 않음).
     * [왜 read 후 거부인가] 종료 중이라도 수락 즉시 write+close 로 거부하면, 요청을 먼저
     *   보내는 클라이언트가 write 시 EPIPE 로 죽어 거부 응답을 못 읽고 드롭된다(실측 확인).
     *   inc 결과를 ctx 에 실어 두고 정상 경로처럼 요청을 read 로 소비한 뒤 on_read_done 에서
     *   -32000 을 전송해야 거부가 실제 전달된다.
     * [경고] inc 이후 read_async 시작 전에 조기 return 을 추가하면 dec 가 실행되지 않아
     *   inflight 가 누수되고 SIGTERM 이 drain timeout 까지 hang 한다. */
    gboolean inflight_held = pcv_drain_inc();

    /* ReadCtx 할당 — 비동기 콜백에서 사용할 컨텍스트 */
    ReadCtx *ctx = g_new0(ReadCtx, 1);
    ctx->server        = self;
    ctx->connection    = g_object_ref(connection);  /* 참조 카운트 증가 — 콜백에서 사용 */
    ctx->inflight_held = inflight_held;              /* FALSE면 read 후 -32000 거부, dec 안 함 */
    ctx->buf_size      = 65536;                      /* 64KB — 대형 VM XML이 포함된 RPC 수용 */
    ctx->buffer        = g_malloc(ctx->buf_size);

    /* 비동기 읽기 시작 — main loop 블로킹 없이 데이터 수신 */
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    g_input_stream_read_async(input,
                               ctx->buffer,
                               ctx->buf_size - 1,   /* NULL 종료 문자를 위해 -1 */
                               G_PRIORITY_DEFAULT,   /* 기본 우선순위 */
                               NULL,                 /* GCancellable — 취소 불필요 */
                               on_read_done,         /* 완료 콜백 */
                               ctx);                 /* 콜백에 전달할 데이터 */

    return TRUE;  /* TRUE: 계속 새 연결을 수락 */
}

/* ═══════════════════════════════════════════════════════════════════
 * 공개 API
 * ═══════════════════════════════════════════════════════════════════ */

/* ─────────────────────────────────────────────
 * uds_server_new — UDS 서버 인스턴스 생성 (팩토리 함수)
 *
 * [동작 흐름]
 *   1. g_object_new()로 GObject 인스턴스 할당 + uds_server_init() 자동 호출
 *   2. socket_path를 g_strdup()으로 복사하여 저장
 *
 * [주의]
 *   - g_strdup()으로 문자열을 복사합니다. 호출자의 socket_path가 해제되어도
 *     UdsServer는 자체 복사본을 유지합니다 (방어적 복사).
 *   - 반환된 객체는 호출자가 g_object_unref()로 해제해야 합니다.
 *     해제하지 않으면 finalize가 호출되지 않아 소켓 파일이 남습니다.
 *   - 이 시점에서는 소켓이 생성되지 않습니다. 실제 소켓 생성은
 *     uds_server_start()에서 수행됩니다.
 *
 * [관련 함수] uds_server_start, uds_server_set_dispatcher, uds_server_finalize
 * ─────────────────────────────────────────────*/
UdsServer *uds_server_new(const gchar *socket_path) {
    UdsServer *self = g_object_new(PURECVISOR_TYPE_UDS_SERVER, NULL);
    self->socket_path = g_strdup(socket_path);  /* 경로 문자열 복사 (소유권 이전) */
    return self;
}

/* ─────────────────────────────────────────────
 * uds_server_set_dispatcher — RPC 디스패처 연결
 *
 * [동작 흐름]
 *   1. 기존 디스패처가 있으면 참조 해제 (교체 시나리오)
 *   2. 새 디스패처의 참조 카운트 증가 후 저장
 *
 * [왜?]
 *   UDS 서버가 디스패처를 "소유"해야 합니다.
 *   g_object_ref()로 참조 카운트를 증가시키면,
 *   외부에서 디스패처를 unref해도 UDS 서버가 살아있는 동안 유지됩니다.
 *   이것이 GObject의 참조 카운팅 소유권 패턴입니다.
 *
 * [주의]
 *   - uds_server_start() 전에 반드시 호출해야 합니다.
 *     디스패처 없이 요청을 받으면 "No dispatcher set" 경고가 발생합니다.
 *
 * [관련 함수] uds_server_new, purecvisor_dispatcher_dispatch
 * ─────────────────────────────────────────────*/
void uds_server_set_dispatcher(UdsServer *self, PureCVisorDispatcher *dispatcher) {
    /* 기존 디스패처가 있으면 참조 해제 */
    if (self->dispatcher) g_object_unref(self->dispatcher);
    /* 새 디스패처의 참조 카운트 증가 — UdsServer가 살아있는 동안 유지 */
    self->dispatcher = g_object_ref(dispatcher);
}

/* ═══════════════════════════════════════════════════════════════════
 * Systemd Socket Activation 감지
 *
 * systemd의 Socket Activation은 서비스 시작 전에 systemd가
 * 소켓을 미리 열어 FD 3번으로 전달하는 메커니즘입니다.
 *
 * 장점: 서비스 재시작 시 클라이언트 연결이 끊기지 않음
 * 환경변수:
 *   LISTEN_PID: 이 FD를 받아야 할 프로세스의 PID
 *   LISTEN_FDS: 전달된 FD 수
 *
 * 현재 상태: 비활성화 (legacy socket unit disabled)
 * 이유: systemd와 현재 에디션 데몬이 소켓을 동시 소유하면
 *       클라이언트(nc)의 연결이 거부되는 문제 발생
 * ═══════════════════════════════════════════════════════════════════ */
/* ─────────────────────────────────────────────
 * _sd_listen_fds — systemd Socket Activation 감지
 *
 * [동작 흐름]
 *   1. LISTEN_PID 환경변수 확인 (systemd가 설정)
 *   2. LISTEN_FDS 환경변수 확인 (전달된 fd 개수)
 *   3. PID가 현재 프로세스와 일치하는지 검증
 *   4. 전달된 fd 수 반환 (0이면 Socket Activation 아님)
 *
 * [왜?]
 *   systemd Socket Activation은 서비스가 시작되기 전에
 *   systemd가 소켓을 미리 열어 놓는 메커니즘입니다.
 *   서비스 재시작 시 클라이언트 연결이 끊기지 않는 장점이 있지만,
 *   현재 PureCVisor에서는 소켓 소유권 충돌 문제로 비활성화되어 있습니다.
 *   (legacy socket unit이 disabled 상태)
 *
 * [주의]
 *   - SD_LISTEN_FDS_START = 3: systemd 규약으로 fd 번호 3부터 시작합니다.
 *     0=stdin, 1=stdout, 2=stderr이므로 3이 첫 번째 전달 fd입니다.
 *   - PID 확인을 하지 않으면, 부모 프로세스의 환경변수가 상속되어
 *     잘못된 fd를 사용하는 보안 문제가 발생할 수 있습니다.
 *
 * [관련 함수] uds_server_start (Socket Activation 분기)
 * ─────────────────────────────────────────────*/
static int _sd_listen_fds(void) {
    const gchar *pid_str = g_getenv("LISTEN_PID");
    const gchar *fds_str = g_getenv("LISTEN_FDS");

    /* 환경변수가 없으면 Socket Activation 아님 */
    if (!pid_str || !fds_str) return 0;

    /* PID가 현재 프로세스와 일치하는지 확인 */
    pid_t expected = (pid_t)g_ascii_strtoll(pid_str, NULL, 10);
    if (expected != getpid()) return 0;

    /* 전달된 FD 수 반환 */
    int n = (int)g_ascii_strtoll(fds_str, NULL, 10);
    return n > 0 ? n : 0;
}

/* ─────────────────────────────────────────────
 * uds_server_start — UDS 서버 시작 (소켓 생성 + 리스닝 + 시그널 연결)
 *
 * [동작 흐름]
 *   1. [PCV_USE_URING] io_uring 모드 우선 시도 → 성공하면 즉시 반환
 *   2. [PCV_USE_URING 실패 또는 미정의] GSocketService 모드로 폴백
 *   3. _sd_listen_fds()로 Socket Activation 감지
 *      a. Socket Activation: FD 3을 GSocket으로 래핑
 *      b. 직접 생성: 기존 소켓 파일 삭제 → 새 소켓 바인딩
 *   4. "incoming" 시그널에 on_incoming_connection 콜백 연결
 *   5. g_socket_service_start()로 accept 시작
 *
 * [왜?]
 *   이 함수는 데몬 초기화(daemon.c)에서 한 번만 호출됩니다.
 *   io_uring → GSocketService 폴백 전략으로, 커널이 io_uring을 지원하지 않는
 *   환경(오래된 커널, 컨테이너 등)에서도 동작을 보장합니다.
 *
 * [주의]
 *   - uds_server_set_dispatcher()를 먼저 호출해야 합니다.
 *   - 기존 소켓 파일(이전 크래시 잔여물)을 삭제하지 않으면
 *     bind()가 EADDRINUSE로 실패합니다.
 *   - error 파라미터는 GLib의 에러 전파 패턴입니다.
 *     실패 시 *error에 에러 정보가 설정되고 FALSE를 반환합니다.
 *
 * [관련 함수] uds_server_new, uds_server_set_dispatcher, uds_server_stop
 * ─────────────────────────────────────────────*/
gboolean uds_server_start(UdsServer *self, GError **error) {
    GError *err = NULL;

#if PCV_USE_URING
    /* [왜?] io_uring 모드를 먼저 시도하고, 실패하면 GSocketService로 폴백합니다.
     * 이 "try → fallback" 패턴은 운영 안정성을 위한 것입니다.
     * 예: 오래된 커널(5.1 미만), 컨테이너(seccomp 차단), 메모리 부족 등으로
     * io_uring 초기화가 실패할 수 있지만 서버는 정상 동작해야 합니다. */
    {
        GError *uring_err = NULL;
        if (_uring_listen_start(self, &uring_err)) {
            return TRUE;  /* io_uring 모드 성공 — GSocketService 불필요 */
        }
        /* [에러 처리] io_uring 실패는 치명적이지 않습니다. GLib 경로로 폴백합니다. */
        g_message("io_uring UDS init failed (%s) — falling back to GSocketService",
                  uring_err ? uring_err->message : "unknown");
        if (uring_err) g_error_free(uring_err);
        self->uring_mode = FALSE;
    }
#endif

    /* GSocketService 생성 — GLib의 소켓 accept 이벤트 루프 */
    self->service = g_socket_service_new();

    int sd_fds = _sd_listen_fds();
    if (sd_fds > 0) {
        /* ── Socket Activation 모드 ──────────────────────────────
         * systemd가 이미 소켓을 열어서 FD 3으로 전달함.
         * 이 FD를 GSocket으로 감싸서 GSocketService에 등록합니다.
         * SD_LISTEN_FDS_START = 3 (systemd 규약)
         * ────────────────────────────────────────────────────── */
        GSocket *sock = g_socket_new_from_fd(3, &err);
        if (!sock) {
            g_propagate_error(error, err);
            return FALSE;
        }
        if (!g_socket_listener_add_socket(G_SOCKET_LISTENER(self->service),
                                           sock, NULL, &err)) {
            g_propagate_error(error, err);
            g_object_unref(sock);
            return FALSE;
        }
        g_object_unref(sock);
        g_message("UDS Server using systemd socket activation (fd=3)");

    } else {
        /* ── 직접 소켓 생성 모드 ─────────────────────────────────
         * 기존 소켓 파일이 있으면 삭제 후 새로 생성합니다.
         * ────────────────────────────────────────────────────── */

        /* 이전 실행에서 남은 소켓 파일 삭제 */
        if (g_file_test(self->socket_path, G_FILE_TEST_EXISTS)) {
            unlink(self->socket_path);
        }

        /* Unix 소켓 주소 생성 + bind + listen */
        GSocketAddress *address = g_unix_socket_address_new(self->socket_path);

        /* umask(0117)로 소켓 권한을 원자적으로 0660(rw-rw----)으로 설정 (TOCTOU 방지).
         * root-only 접근은 SO_PEERCRED 게이트가 강제하고, 0660 은 심층 방어(DAC). */
        mode_t old_umask = umask(0117);

        if (!g_socket_listener_add_address(G_SOCKET_LISTENER(self->service),
                                           address,
                                           G_SOCKET_TYPE_STREAM,      /* TCP 스트림 */
                                           G_SOCKET_PROTOCOL_DEFAULT, /* 기본 프로토콜 */
                                           NULL, NULL, &err)) {
            umask(old_umask);
            g_propagate_error(error, err);
            g_object_unref(address);
            return FALSE;
        }

        umask(old_umask);
        g_object_unref(address);

        g_message("UDS Server listening on %s", self->socket_path);
    }

    /*
     * "incoming" 시그널 연결
     *
     * 새 클라이언트가 connect()하면 GSocketService가
     * "incoming" GObject 시그널을 emit합니다.
     * on_incoming_connection()이 호출되어 비동기 read를 시작합니다.
     */
    g_signal_connect(self->service, "incoming", G_CALLBACK(on_incoming_connection), self);

    /* 서비스 시작 — 이후부터 새 연결을 수락합니다 */
    g_socket_service_start(self->service);

    return TRUE;
}

/* ─────────────────────────────────────────────
 * uds_server_stop — UDS 서버 중지 (새 연결 수락 중단)
 *
 * [동작 흐름]
 *   1. [io_uring 모드] uring 컨텍스트 해제 + listen_fd 닫기
 *   2. [GLib 모드] g_socket_service_stop()으로 accept 중지
 *
 * [왜?]
 *   그레이스풀 셧다운(graceful shutdown) 시 호출됩니다.
 *   새 연결은 거부하지만, 이미 진행 중인 RPC 요청은 완료될 때까지 대기합니다.
 *   pcv_drain_wait()이 inflight 카운터가 0이 될 때까지 기다린 후
 *   이 함수를 호출합니다.
 *
 * [주의]
 *   - finalize와 중복 해제되지 않도록 NULL/음수로 초기화합니다.
 *   - io_uring 모드에서 listen_fd를 닫지 않으면 소켓 파일이 점유된 채로
 *     남아 다음 실행 시 bind()가 실패합니다.
 *
 * [관련 함수] uds_server_start, uds_server_finalize, pcv_drain_wait (drain.c)
 * ─────────────────────────────────────────────*/
void uds_server_stop(UdsServer *self) {
#if PCV_USE_URING
    if (self->uring_mode && self->uring) {
        pcv_uring_free(self->uring);
        self->uring = NULL;
        if (self->listen_fd >= 0) {
            close(self->listen_fd);
            self->listen_fd = -1;
        }
        return;
    }
#endif
    if (self->service)
        g_socket_service_stop(self->service);
}

/* ─────────────────────────────────────────────
 * pure_uds_server_send_response — RPC 응답 전송 + 소켓 종료
 *
 * [동작 흐름]
 *   1. GOutputStream 획득 (connection에서)
 *   2. g_output_stream_write_all()로 전체 응답 전송 (부분 쓰기 방지)
 *   3. g_io_stream_close()로 소켓 연결 종료
 *
 * [왜?]
 *   PureCVisor의 UDS 통신은 Short-lived connection 모델입니다.
 *   요청 1개 → 응답 1개 → 연결 종료. HTTP/1.0과 유사합니다.
 *   이 모델은 fire-and-forget 비동기 패턴과 자연스럽게 호환됩니다.
 *
 * [주의 — 가장 흔한 주니어 실수]
 *   - 이 함수를 호출하면 소켓이 닫힙니다. 2번 호출하면 크래시합니다!
 *   - GTask 콜백(비동기 완료)에서 이 함수를 호출하면 안 됩니다.
 *     소켓은 이미 닫혀 있으므로 write가 실패하고 UB(정의되지 않은 동작)가 발생합니다.
 *
 *   ✅ 올바른 fire-and-forget 패턴:
 *     void handle_xxx(...) {
 *         resp = pure_rpc_build_success_response(id, result);
 *         pure_uds_server_send_response(server, conn, resp);  // 여기서 소켓 닫힘
 *         g_free(resp);
 *         // 이후 GTask로 비동기 작업 시작 (결과는 로그에만 기록)
 *     }
 *
 *   ❌ 잘못된 패턴:
 *     void task_callback(...) {
 *         pure_uds_server_send_response(server, conn, resp);  // 크래시! 소켓 이미 닫힘
 *     }
 *
 * [관련 함수] purecvisor_dispatcher_dispatch, on_read_done, CLAUDE.md "비동기 패턴" 참조
 * ─────────────────────────────────────────────*/
void pure_uds_server_send_response(UdsServer *self, GSocketConnection *connection, const gchar *response) {
    (void)self;  /* 현재 미사용 */

    /* 출력 스트림 가져오기 */
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GError *error = NULL;

    /* [왜?] write()는 요청한 바이트 수보다 적게 쓸 수 있습니다 (short write).
     * write_all()은 전체 데이터가 전송될 때까지 반복 호출합니다.
     * JSON-RPC 응답이 잘려서 전송되면 클라이언트가 파싱에 실패합니다. */
    if (!g_output_stream_write_all(output, response, strlen(response), NULL, NULL, &error)) {
        g_warning("Failed to send response: %s", error->message);
        g_error_free(error);
    }

    /*
     * 응답 전송 후 연결 종료 (Short-lived connection 모델)
     *
     * 각 RPC 요청마다 새 연결을 맺고 응답 후 닫습니다.
     * Keep-Alive 방식에 비해 연결 오버헤드가 있지만,
     * fire-and-forget 비동기 패턴과 호환되고 구현이 단순합니다.
     */
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
}
