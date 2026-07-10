#ifndef PURECVISOR_URING_H
#define PURECVISOR_URING_H

/**
 * @file pcv_uring.h
 * @brief io_uring 비동기 I/O 인프라 — 공개 헤더 (구조체, 콜백, API 선언)
 *
 * ──────────────────────────────────────────────────────────────
 * [io_uring 배경 지식 — 주니어 개발자용]
 *
 *   io_uring은 Linux 5.1+에서 도입된 고성능 비동기 I/O 인터페이스이다.
 *   기존 epoll/select 기반 I/O와 달리 커널-유저 공유 링 버퍼를 통해
 *   시스템 콜 오버헤드를 최소화한다.
 *
 *   핵심 개념:
 *   ┌─────────────────────────────────────────────────┐
 *   │ SQ (Submission Queue) - 유저→커널 I/O 요청 제출 │
 *   │   SQE (Submission Queue Entry): 개별 I/O 요청   │
 *   │     - op: READ/WRITE/ACCEPT/CONNECT/SEND/RECV   │
 *   │     - fd: 대상 파일 디스크립터                    │
 *   │     - buf: 데이터 버퍼 포인터                     │
 *   │     - user_data: 완료 시 식별용 (우리는 seq ID)   │
 *   ├─────────────────────────────────────────────────┤
 *   │ CQ (Completion Queue) - 커널→유저 완료 통지      │
 *   │   CQE (Completion Queue Entry): 완료된 I/O 결과  │
 *   │     - res: 결과값 (양수=바이트수, 음수=-errno)    │
 *   │     - user_data: 제출 시 설정한 값 (seq ID)       │
 *   └─────────────────────────────────────────────────┘
 *
 *   PureCVisor에서의 통합 방식:
 *   ┌──────────┐   eventfd   ┌────────────┐
 *   │ io_uring │ ──notify──→ │ GMainLoop  │
 *   │  (CQE)   │             │ (callback) │
 *   └──────────┘             └────────────┘
 *   eventfd를 io_uring에 등록하면, CQE가 도착할 때 eventfd에 알림이 오고,
 *   GMainLoop의 g_unix_fd_add()가 이를 감지하여 콜백을 호출한다.
 *   이렇게 io_uring과 GLib 이벤트 루프가 자연스럽게 통합된다.
 *
 * [아키텍처 내 위치]
 *   Phase U-1~U-5에서 도입된 io_uring 서브시스템의 중앙 헤더 파일.
 *   pcv_uring.c, pcv_uring_buf.c, pcv_uring_socket.c 세 모듈이 공유한다.
 *
 *   파일 구성:
 *     pcv_uring.h          ← 이 파일 (공개 API + 타입 정의)
 *     pcv_uring.c          : ring 초기화, eventfd 브릿지, CQE 수확, 파일 I/O
 *     pcv_uring_buf.c      : mmap 기반 고정 크기 버퍼 풀 (인덱스 스택)
 *     pcv_uring_socket.c   : 소켓 I/O (accept/connect/send/recv)
 *
 * [Feature Flag 2단계 체계]
 *   HAVE_LIBURING    : Makefile에서 pkg-config로 liburing 존재 여부 탐지
 *   PCV_URING_ENABLED: 빌드 시 명시적 활성화 플래그
 *
 *   두 조건 모두 충족 → PCV_USE_URING=1 (io_uring 경로 활성화)
 *   하나라도 미충족   → PCV_USE_URING=0 (GLib GSocketService fallback)
 *
 *   2단계인 이유: liburing이 설치되어 있더라도 의도적으로 비활성화할 수 있어야
 *   하기 때문. 예를 들어 구형 커널에서 liburing 헤더만 있고 실제 io_uring
 *   시스콜이 없는 경우를 방지한다.
 *
 * [핵심 타입]
 *   PcvUringCtx       : ring + eventfd + pending 해시테이블 + 버퍼 풀을 묶은 컨텍스트
 *   PcvUringBufPool   : mmap 기반 고정 크기 버퍼 풀 (LIFO 인덱스 스택)
 *   PcvUringCallback  : CQE 완료 시 호출되는 콜백 (result=cqe->res, 음수=-errno)
 *   PcvUringPendingOp : 대기 중인 I/O 작업 (콜백 + user_data + 버퍼 인덱스)
 *
 * [API 분류]
 *   Lifecycle : pcv_uring_new() / pcv_uring_free() / pcv_uring_is_available()
 *   File I/O  : pcv_uring_submit_read() / pcv_uring_submit_write()
 *   Socket I/O: pcv_uring_submit_accept/connect/send/recv()
 *   Buffer    : pcv_uring_buf_pool_new/free(), pcv_uring_buf_alloc/release/get()
 *
 * [사용 예시 — UDS 서버에서 accept 루프]
 *   GError *err = NULL;
 *   PcvUringCtx *ctx = pcv_uring_new(PCV_URING_DEFAULT_QUEUE_DEPTH, &err);
 *   // 운영 기본 SQ/CQ 깊이 사용
 *   pcv_uring_submit_accept(ctx, listen_fd, NULL, NULL, on_accept, NULL);
 *   // GMainLoop에서 CQE 도착 시 on_accept 콜백 자동 호출
 *   // on_accept 내에서 다시 submit_accept 호출 → 연속 accept 루프
 *   pcv_uring_free(ctx);  // 종료 시 ring + eventfd + 버퍼 풀 일괄 정리
 *
 * [실측 성능 (2026-03-23 벤치마크)]
 *   UDS vm.list: p50=3ms, 226 RPS
 *   REST /vms:   p95=8ms
 *   etcd:        p50=49ms
 *   RSS 오버헤드: +460KB
 *
 * [주의사항]
 *   - PCV_USE_URING=0일 때 PcvUringCtx는 불투명 전방 선언만 존재 (stub)
 *   - pcv_uring_is_available() stub은 항상 FALSE → 컴파일러가 constant folding
 *   - BUF_SIZE=64KB: UDS JSON-RPC + libvirt domain XML 최대 ~40KB 수용 기준
 *   - send 시 buf 포인터는 CQE 완료 전까지 유효해야 함 (use-after-free 주의)
 *   - recv result=0은 EOF(피어 연결 종료)를 의미
 * ──────────────────────────────────────────────────────────────
 */

#include <glib.h>

/* Feature Flag 2단계 체계:
 * HAVE_LIBURING (pkg-config 탐지) + PCV_URING_ENABLED (명시적 활성화)
 * 둘 다 정의되어야 io_uring 코드 경로가 활성화된다 */
#if defined(HAVE_LIBURING) && defined(PCV_URING_ENABLED)
    #define PCV_USE_URING 1
    #include <liburing.h>
#else
    #define PCV_USE_URING 0
#endif

G_BEGIN_DECLS

#if PCV_USE_URING

/* ================================================================
 * 버퍼 풀 (pcv_uring_buf.c)
 *
 * io_uring I/O에 사용할 고정 크기 버퍼를 사전 할당하여 풀링한다.
 * 매 I/O마다 malloc/free 하는 대신, mmap으로 큰 블록을 한번 할당하고
 * 인덱스 기반 LIFO 스택으로 할당/반환을 O(1)에 수행한다.
 *
 * 메모리 레이아웃:
 *   base ─→ [ buf[0] | buf[1] | ... | buf[count-1] ]
 *            ↑ 64KB  ↑ 64KB         ↑ 64KB
 *   총 크기: PCV_URING_BUF_SIZE * PCV_URING_BUF_COUNT = 4MB
 *
 * LIFO 스택(free_list):
 *   free_list[free_top] = 다음 할당할 버퍼 인덱스
 *   alloc: free_list[--free_top] → 인덱스 반환
 *   release: free_list[free_top++] = 반환할 인덱스
 *   → 항상 가장 최근 반환된 버퍼를 재사용 (CPU 캐시 친화적)
 * ================================================================ */

/** 개별 버퍼 크기 — UDS JSON-RPC XML 페이로드 수용 (libvirt XML 최대 ~40KB) */
#define PCV_URING_BUF_SIZE    65536   /* 64KB per buffer */
/** 풀 엔트리 수 — 동시 진행 가능한 I/O 수 상한 */
#define PCV_URING_BUF_COUNT   64
/** 운영 기본 SQ/CQ 깊이 — UDS worker burst 중 EAGAIN 재시도 진입을 낮춘다. */
#define PCV_URING_DEFAULT_QUEUE_DEPTH 1024U

/* ── C23 컴파일 타임 검증 ─────────────────────────────────── */
static_assert(PCV_URING_BUF_COUNT >= 1);
static_assert(PCV_URING_BUF_SIZE >= 4096);
static_assert(PCV_URING_DEFAULT_QUEUE_DEPTH >= 1024);

/**
 * @brief mmap 기반 고정 크기 버퍼 풀
 *
 * 스레드 안전: mu 뮤텍스로 보호됨. 여러 스레드에서 동시 alloc/release 가능.
 */
typedef struct {
    void     *base;         /**< mmap으로 할당된 연속 메모리 블록 (buf_size * count 바이트) */
    gsize     buf_size;     /**< 개별 버퍼 크기 (바이트, 기본 65536) */
    gint      count;        /**< 총 버퍼 수 (기본 64) */
    gint     *free_list;    /**< 사용 가능 인덱스 LIFO 스택 (크기: count) */
    gint      free_top;     /**< 스택 탑 인덱스 (0=모두 사용 중, count=모두 사용 가능) */
    /* B11-M5: O(1) 이중 반환 검출용 비트필드 (free_list O(n) 스캔 대체) */
    gboolean *in_use;       /**< in_use[i]=TRUE: 버퍼 i가 현재 할당됨(사용 중) */
    GMutex    mu;           /**< 스레드 안전을 위한 뮤텍스 */
} PcvUringBufPool;

/**
 * @brief 버퍼 풀을 생성한다.
 * @param count    버퍼 수 (0이면 기본 PCV_URING_BUF_COUNT)
 * @param buf_size 개별 버퍼 크기 (0이면 기본 PCV_URING_BUF_SIZE)
 * @param error    실패 시 GError 설정 (mmap 실패 등)
 * @return 새 버퍼 풀 포인터. 실패 시 NULL.
 */
PcvUringBufPool *pcv_uring_buf_pool_new(gint count, gsize buf_size, GError **error);

/** @brief 버퍼 풀을 해제한다 (munmap + 뮤텍스 해제). */
void             pcv_uring_buf_pool_free(PcvUringBufPool *pool);

/**
 * @brief 사용 가능한 버퍼 인덱스를 할당한다.
 * @return 버퍼 인덱스 (0~count-1). 모두 사용 중이면 -1.
 *         반환된 인덱스로 pcv_uring_buf_get()을 호출하여 실제 포인터를 얻는다.
 */
gint             pcv_uring_buf_alloc(PcvUringBufPool *pool);

/**
 * @brief 사용 완료된 버퍼를 풀에 반환한다.
 * @param index  pcv_uring_buf_alloc()에서 받은 인덱스
 * 주의: 같은 인덱스를 두 번 release하면 double-free와 유사한 상태가 된다.
 */
void             pcv_uring_buf_release(PcvUringBufPool *pool, gint index);

/**
 * @brief 인덱스에 대응하는 버퍼 메모리 포인터를 반환한다.
 * @param index  버퍼 인덱스 (0~count-1)
 * @return base + (index * buf_size) 위치의 포인터
 */
void            *pcv_uring_buf_get(PcvUringBufPool *pool, gint index);

/* ================================================================
 * 콜백 타입
 * ================================================================ */

/** PcvUringCtx 전방 선언 (콜백 시그니처에서 참조) */
typedef struct _PcvUringCtx PcvUringCtx;

/**
 * @brief io_uring CQE(Completion Queue Entry) 완료 시 호출되는 콜백 함수 타입
 *
 * @param ctx       uring 컨텍스트 (추가 I/O 제출에 사용 가능)
 * @param result    cqe->res 값:
 *                  - accept: 새 소켓 fd (양수) 또는 -errno
 *                  - connect: 0(성공) 또는 -errno
 *                  - send/write: 전송된 바이트 수 또는 -errno
 *                  - recv/read: 수신된 바이트 수, 0=EOF(피어 종료), -errno=에러
 * @param user_data submit 시 전달한 사용자 데이터 포인터
 */
typedef void (*PcvUringCallback)(PcvUringCtx *ctx, gint result, gpointer user_data);

/* ================================================================
 * 대기 작업 (pending op)
 *
 * submit 시 SQE의 user_data에 시퀀스 ID를 설정하고,
 * 이 구조체를 pending 해시테이블에 저장한다.
 * CQE 도착 시 user_data로 이 구조체를 조회하여 콜백을 호출한다.
 * ================================================================ */

typedef struct {
    PcvUringCallback  callback;   /**< CQE 완료 시 호출할 콜백 함수 */
    gpointer          user_data;  /**< 콜백에 전달할 사용자 데이터 */
    gint              buf_index;  /**< 버퍼 풀 인덱스 (-1 = 풀 버퍼 미사용, 사용자 버퍼) */
} PcvUringPendingOp;

/* ================================================================
 * Ring 컨텍스트
 *
 * io_uring ring, eventfd, GMainLoop 브릿지, 버퍼 풀, pending 추적을
 * 하나의 구조체로 묶어 관리한다.
 *
 * 소유권: pcv_uring_new()가 생성, pcv_uring_free()가 해제.
 * 스레드 모델: submit은 submit_mu로 직렬화, CQE 수확은 GMainLoop 스레드.
 * ================================================================ */

struct _PcvUringCtx {
    struct io_uring   ring;           /**< liburing ring 인스턴스 (SQ+CQ) */
    int               event_fd;       /**< eventfd FD — io_uring CQE 도착을 GMainLoop에 알림
                                       *   io_uring_register_eventfd()로 등록하면
                                       *   CQE가 완료될 때 커널이 eventfd에 write(1) →
                                       *   GMainLoop의 g_unix_fd_add()가 감지 → 콜백 호출 */
    guint             glib_source_id; /**< g_unix_fd_add()의 반환값 — 정리 시 g_source_remove() */
    PcvUringBufPool  *buf_pool;       /**< 소유한 버퍼 풀 (NULL 가능 — 외부 버퍼 사용 시) */
    GHashTable       *pending;        /**< guint sqe_id → PcvUringPendingOp* 매핑
                                       *   submit 시 등록, CQE 수확 시 조회+제거 */
    /* B11-M1: next_id를 volatile guint(32-bit)로 변경.
     * 이전 guint64는 g_atomic_int_add()(32-bit 전용)와 타입 불일치였고,
     * overflow 비교 `> G_MAXUINT`가 guint64 값에서는 dead code였다.
     * guint로 변경하면 32-bit 오버플로우가 자연스럽게 0으로 wrap-around되며
     * g_atomic_int_add() 호환성이 보장된다. */
    volatile guint    next_id;        /**< SQE user_data 시퀀스 번호 (단조 증가, 32-bit) */
    GMutex            submit_mu;      /**< SQE 제출 직렬화 뮤텍스 (여러 스레드에서 submit 가능) */
    gboolean          running;        /**< FALSE이면 CQE 수확 루프 중단 (shutdown 시) */
};

/* ================================================================
 * Lifecycle API
 * ================================================================ */

/**
 * @brief io_uring 컨텍스트를 생성한다.
 *
 * 내부적으로:
 *   1. io_uring_queue_init(queue_depth, &ring, 0) — ring 초기화
 *   2. eventfd(0, EFD_NONBLOCK) — eventfd 생성
 *   3. io_uring_register_eventfd(&ring, event_fd) — CQE→eventfd 연결
 *   4. g_unix_fd_add(event_fd, ...) — eventfd→GMainLoop 브릿지
 *   5. pcv_uring_buf_pool_new() — 버퍼 풀 생성
 *
 * @param queue_depth  SQ/CQ 깊이 (동시 진행 가능한 I/O 수, 보통 256)
 * @param error        실패 시 GError 설정
 * @return 새 컨텍스트 포인터. 실패 시 NULL.
 */
PcvUringCtx *pcv_uring_new(guint queue_depth, GError **error);

/**
 * @brief io_uring 컨텍스트를 해제한다.
 *
 * ring 해제, eventfd close, GSource 제거, 버퍼 풀 해제, pending 정리.
 * pending에 남은 미완료 작업이 있으면 경고 로그 출력.
 */
void         pcv_uring_free(PcvUringCtx *ctx);

/**
 * @brief io_uring 사용 가능 여부를 반환한다.
 * @return PCV_USE_URING=1이면 TRUE, 아니면 FALSE
 */
gboolean     pcv_uring_is_available(void);

/* ================================================================
 * File I/O Submit API (pcv_uring.c)
 * ================================================================ */

/**
 * @brief 비동기 파일 읽기를 io_uring에 제출한다 (IORING_OP_READ).
 * @param fd      읽을 파일 디스크립터
 * @param buf     데이터를 받을 버퍼 (CQE 완료 전까지 유효해야 함)
 * @param len     읽을 최대 바이트 수
 * @param offset  파일 오프셋 (-1이면 현재 위치)
 * @param cb      완료 콜백 (result=읽은 바이트 수 또는 -errno)
 * @param data    콜백에 전달할 사용자 데이터
 * @return TRUE: 제출 성공, FALSE: SQ full 또는 ring 에러
 */
gboolean pcv_uring_submit_read(PcvUringCtx *ctx, int fd, void *buf, gsize len,
                                off_t offset, PcvUringCallback cb, gpointer data);

/**
 * @brief 비동기 파일 쓰기를 io_uring에 제출한다 (IORING_OP_WRITE).
 * @param buf     쓸 데이터 버퍼 (CQE 완료 전까지 유효해야 함)
 * @param len     쓸 바이트 수
 * @param offset  파일 오프셋 (-1이면 현재 위치)
 * @param cb      완료 콜백 (result=쓴 바이트 수 또는 -errno)
 */
gboolean pcv_uring_submit_write(PcvUringCtx *ctx, int fd, const void *buf, gsize len,
                                 off_t offset, PcvUringCallback cb, gpointer data);

/* ================================================================
 * Socket I/O Submit API (pcv_uring_socket.c)
 *
 * UDS 서버(uds_server.c)에서 GSocketService 대신 사용한다.
 * accept 루프 패턴: submit_accept → 콜백에서 새 fd 처리 → 다시 submit_accept
 * ================================================================ */

/**
 * @brief 비동기 소켓 accept를 제출한다 (IORING_OP_ACCEPT).
 * @param listen_fd  리스닝 소켓 fd
 * @param addr       클라이언트 주소를 받을 구조체 (NULL 가능)
 * @param addrlen    addr 크기 포인터 (NULL 가능)
 * @param cb         완료 콜백 (result=새 클라이언트 소켓 fd 또는 -errno)
 */
gboolean pcv_uring_submit_accept(PcvUringCtx *ctx, int listen_fd,
                                  struct sockaddr *addr, socklen_t *addrlen,
                                  PcvUringCallback cb, gpointer data);

/**
 * @brief 비동기 소켓 connect를 제출한다 (IORING_OP_CONNECT).
 * @param fd    소켓 fd
 * @param addr  연결 대상 주소
 * @param cb    완료 콜백 (result=0 성공 또는 -errno)
 */
gboolean pcv_uring_submit_connect(PcvUringCtx *ctx, int fd,
                                   const struct sockaddr *addr, socklen_t addrlen,
                                   PcvUringCallback cb, gpointer data);

/**
 * @brief 비동기 소켓 send를 제출한다 (IORING_OP_SEND).
 * @param buf  전송할 데이터 (CQE 완료 전까지 유효해야 함 — use-after-free 주의!)
 * @param len  전송할 바이트 수
 * @param cb   완료 콜백 (result=전송된 바이트 수 또는 -errno)
 */
gboolean pcv_uring_submit_send(PcvUringCtx *ctx, int fd, const void *buf, gsize len,
                                PcvUringCallback cb, gpointer data);

/**
 * @brief 비동기 소켓 recv를 제출한다 (IORING_OP_RECV).
 * @param buf  수신 버퍼 (CQE 완료 전까지 유효해야 함)
 * @param len  수신 버퍼 크기
 * @param cb   완료 콜백 (result=수신 바이트 수, 0=EOF 피어 종료, -errno=에러)
 */
gboolean pcv_uring_submit_recv(PcvUringCtx *ctx, int fd, void *buf, gsize len,
                                PcvUringCallback cb, gpointer data);

#else /* !PCV_USE_URING */

/* ================================================================
 * Stub — io_uring 미사용 빌드 시
 *
 * PCV_USE_URING=0이면 이 섹션만 컴파일된다.
 * PcvUringCtx는 불투명 전방 선언만 존재하여 포인터 타입으로만 사용 가능.
 * pcv_uring_is_available()은 항상 FALSE를 반환하는 inline stub이다.
 * 컴파일러의 constant folding에 의해 if (pcv_uring_is_available()) 분기가
 * 완전히 제거되어 런타임 오버헤드가 0이다.
 * ================================================================ */

typedef struct _PcvUringCtx PcvUringCtx;
static inline gboolean pcv_uring_is_available(void) { return FALSE; }

#endif /* PCV_USE_URING */

G_END_DECLS

#endif /* PURECVISOR_URING_H */
