/**
 * @file pcv_uring.c
 * @brief io_uring 비동기 I/O 인프라 — ring 초기화, eventfd 브릿지, CQE 디스패치
 *
 * Phase U-1에서 도입된 io_uring 핵심 모듈입니다.
 * PureCVisor 데몬의 기존 GMainLoop 이벤트 루프를 유지하면서
 * 커널 5.6+ io_uring 비동기 I/O의 성능 이점을 가져옵니다.
 *
 * [아키텍처 위치]
 *   GMainLoop (GLib) ←── eventfd ←── io_uring CQE (커널)
 *   pcv_uring.c (본 파일)  : ring 생성, eventfd 브릿지, CQE 디스패치, read/write 제출
 *   pcv_uring_buf.c        : 버퍼 풀 (mmap 기반 사전 할당)
 *   pcv_uring_socket.c     : 소켓 전용 SQE 래퍼 (accept/connect/send/recv)
 *
 * [핵심 흐름: SQE 제출 → CQE 완료 → 콜백 호출]
 *   1. 호출자가 pcv_uring_submit_read() 등을 호출
 *   2. SQE(Submission Queue Entry)를 ring에 추가하고 io_uring_submit()
 *   3. 커널이 I/O 완료 시 CQE(Completion Queue Entry)를 ring에 넣고 eventfd에 신호
 *   4. GMainLoop이 eventfd 감지 → _on_uring_ready() 콜백 진입
 *   5. CQE를 배치(batch)로 순회하며 pending 해시테이블에서 콜백을 찾아 호출
 *   6. 사용한 버퍼 풀 인덱스가 있으면 자동 반환
 *
 * [스레드 안전 모델]
 *   - submit_mu 뮤텍스: SQE 제출 직렬화 (GTask 워커 스레드에서 동시 제출 가능)
 *   - pending 해시테이블: GMainLoop 스레드에서만 접근 (CQE 처리)
 *   - next_id: GMainLoop 스레드에서만 증가 (단조 증가 시퀀스)
 *
 * [빌드 조건]
 *   PCV_USE_URING=1 (HAVE_LIBURING + PCV_URING_ENABLED) 일 때만 컴파일됩니다.
 *   liburing-dev 미설치 시 PCV_USE_URING=0 → 기존 GLib GSocketService fallback.
 *
 * [io_uring이 epoll보다 나은 점 — 주니어 필독]
 *   epoll:    시스템 콜 2회 (epoll_wait + read/write) + 유저↔커널 전환 2회
 *   io_uring: 시스템 콜 0~1회 (SQ/CQ 공유 링 버퍼로 배치 제출+완료)
 *
 *   성능 차이의 핵심: 시스템 콜 오버헤드 제거
 *   - epoll_wait(): 매번 커널 진입 (컨텍스트 스위칭 ~1-2us)
 *   - io_uring: SQE를 링 버퍼에 쓰면 커널이 폴링으로 감지 (0 syscall 가능)
 *   - 배치 제출: 여러 I/O를 한 번의 io_uring_submit()으로 처리
 *   - 배치 완료: 여러 CQE를 한 번의 콜백에서 처리 (_on_uring_ready)
 *
 *   PureCVisor에서의 실측: UDS 소켓 I/O 처리량 ~2배 향상
 *
 * [eventfd→GMainLoop 브릿지 — 주니어 필독]
 *   문제: io_uring은 자체 완료 큐(CQ)를 사용하지만, PureCVisor는 GMainLoop 이벤트 루프 기반.
 *   해법: eventfd를 중간 다리로 사용합니다.
 *   1. io_uring에 eventfd를 등록 (io_uring_register_eventfd)
 *   2. 커널이 CQE를 넣으면 eventfd에 알림을 씁니다
 *   3. GMainLoop이 eventfd의 G_IO_IN을 감지 → _on_uring_ready() 콜백 호출
 *   4. 콜백에서 CQE를 배치 처리
 *   결과: GMainLoop의 기존 이벤트 흐름에 io_uring을 자연스럽게 통합
 *
 * [주의사항]
 *   - _register_pending(), _submit_ring()은 non-static: pcv_uring_socket.c에서 extern 참조
 *   - buf_pool이 NULL이어도 동작 (비치명적 — 버퍼 풀 없이 직접 버퍼 사용)
 *   - shutdown 시 pending ops가 남아 있으면 경고 로그 출력 (누수 진단용)
 */
#include "pcv_uring.h"

#if PCV_USE_URING

#include "utils/pcv_log.h"
#include <glib-unix.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define URING_LOG_DOM "io_uring"

/* ── pending op 관리 ──────────────────────────────────── */

/**
 * @brief 대기 중 I/O 오퍼레이션(pending op) 구조체를 할당하고 초기화
 *
 * SQE를 제출할 때마다 하나의 pending op을 생성하여 해시테이블에 등록합니다.
 * CQE 완료 시 이 구조체에서 콜백 함수와 버퍼 인덱스를 가져옵니다.
 *
 * @param cb       I/O 완료 시 호출될 콜백 함수 (NULL 허용 — 결과 무시 시)
 * @param data     콜백에 전달할 사용자 데이터
 * @param buf_idx  버퍼 풀 인덱스 (-1이면 버퍼 풀 미사용)
 * @return 새로 할당된 PcvUringPendingOp (g_free로 해제)
 */
static PcvUringPendingOp *
_pending_new(PcvUringCallback cb, gpointer data, gint buf_idx)
{
    PcvUringPendingOp *op = g_new0(PcvUringPendingOp, 1);
    op->callback  = cb;
    op->user_data = data;
    op->buf_index = buf_idx;
    return op;
}

/**
 * @brief pending op 해제 콜백 (GHashTable의 value_destroy_func)
 * @param p PcvUringPendingOp 포인터
 */
static void
_pending_free(gpointer p)
{
    g_free(p);
}

/* ── eventfd 콜백 (GMainLoop 스레드) ─────────────────── */

/**
 * @brief io_uring CQE 완료 처리 콜백 — GMainLoop에서 eventfd 이벤트 수신 시 호출
 *
 * [동작 흐름]
 *   1. eventfd를 read()하여 커널의 CQE 도착 알림을 소비합니다.
 *   2. io_uring_for_each_cqe 매크로로 완료된 CQE를 배치(batch) 순회합니다.
 *   3. 각 CQE의 sqe_id(user_data)로 pending 해시테이블에서 콜백을 찾아 호출합니다.
 *   4. 버퍼 풀을 사용한 경우 해당 인덱스를 자동 반환합니다.
 *   5. io_uring_cq_advance()로 처리된 CQE를 ring에서 제거합니다.
 *
 * [왜 eventfd 브릿지인가?]
 *   io_uring의 CQE 알림을 GMainLoop의 fd 감시 메커니즘(g_unix_fd_add)에
 *   통합하기 위해 eventfd를 사용합니다. 커널이 CQE를 넣으면 eventfd에 쓰고,
 *   GMainLoop이 이를 감지하여 이 콜백을 호출합니다.
 *
 * @param fd    eventfd 파일 디스크립터
 * @param cond  GIOCondition (G_IO_IN — 읽기 가능)
 * @param data  PcvUringCtx 포인터
 * @return G_SOURCE_CONTINUE — 다음 이벤트도 계속 감시
 */
static gboolean
_on_uring_ready(gint fd, GIOCondition cond __attribute__((unused)), gpointer data)
{
    PcvUringCtx *ctx = data;

    /* B11-C2: eventfd spurious wakeup 또는 부분 소비 대비.
     * read() 결과와 무관하게 CQ ring을 언제나 드레인한다.
     * 과거 구현은 read() 실패 시 early-return 해서 이미 도착한 CQE가
     * 다음 eventfd 신호까지 밀리는 증상(= CQE 손실처럼 보임)이 있었다.
     * 또한 EFD_SEMAPHORE가 아닌 카운터 모드이므로 read()가 성공해도
     * 그 시점 이후 커널이 새로 넣은 CQE는 drain 루프가 처리해야 한다. */
    uint64_t val;
    ssize_t __attribute__((unused)) _rn = read(fd, &val, sizeof(val));
    /* 결과 무시 — 아래에서 무조건 CQ 드레인 */

    /* B11-M2: CQE 배치 처리 — io_uring_cqe_seen() per entry 방식으로 통일.
     * 이전 구현은 for_each_cqe(cq_advance 방식) + peek 루프(cqe_seen 방식)를
     * 혼용하여 이중 처리(double-processing) 가능성이 있었다.
     * 수정: for_each_cqe 루프에서 각 엔트리마다 io_uring_cqe_seen()을 호출하고
     * 별도 peek 루프는 제거한다. cqe_seen()은 내부적으로 cq_advance(1)과 동일하므로
     * 기능은 동일하되 per-entry 시맨틱이 명확하고 이중 처리가 불가능하다. */
    struct io_uring_cqe *cqe;
    unsigned head;    /* CQ ring의 현재 head 위치 (매크로 내부에서 사용) */

    io_uring_for_each_cqe(&ctx->ring, head, cqe) {
        /* CQE에 저장된 sqe_id로 등록된 pending op을 조회 */
        guint sqe_id = (guint)io_uring_cqe_get_data64(cqe);

        PcvUringPendingOp *op = g_hash_table_lookup(ctx->pending,
                                                     GUINT_TO_POINTER(sqe_id));
        if (op) {
            /* 콜백 호출: cqe->res는 커널 반환값 (바이트 수 또는 -errno) */
            if (op->callback)
                op->callback(ctx, cqe->res, op->user_data);

            /* 버퍼 풀 자동 반환 — 호출자가 직접 반환할 필요 없음 */
            if (op->buf_index >= 0 && ctx->buf_pool)
                pcv_uring_buf_release(ctx->buf_pool, op->buf_index);

            g_hash_table_remove(ctx->pending, GUINT_TO_POINTER(sqe_id));
        }
        /* 각 CQE를 즉시 소비하여 이중 처리 방지 */
        io_uring_cqe_seen(&ctx->ring, cqe);
    }

    return G_SOURCE_CONTINUE;
}

/* ── 내부: SQE 제출 공통 ──────────────────────────────── */

/**
 * @brief 콜백을 pending 해시테이블에 등록하고 고유 sqe_id를 반환
 *
 * SQE 제출 전에 호출하여 CQE 완료 시 어떤 콜백을 호출할지 등록합니다.
 * 반환된 id는 io_uring_sqe_set_data64()로 SQE에 저장됩니다.
 *
 * @param ctx     io_uring 컨텍스트
 * @param cb      완료 콜백
 * @param data    콜백에 전달할 사용자 데이터
 * @param buf_idx 버퍼 풀 인덱스 (-1이면 미사용)
 * @return 단조 증가하는 고유 ID (SQE user_data에 저장)
 *
 * @note non-static: pcv_uring_socket.c에서 extern으로 참조합니다.
 * @note next_id는 g_atomic_int_add로 원자적으로 증가하여 GTask 워커 스레드 안전.
 *
 * [왜 atomic next_id가 필요한가? — 주니어 필독]
 *   여러 GTask 워커 스레드가 동시에 I/O를 제출할 수 있습니다.
 *   각 SQE에 고유한 ID를 부여해야 CQE 완료 시 어떤 콜백을 호출할지 식별합니다.
 *   일반 next_id++는 레이스 조건(두 스레드가 같은 값을 읽고 같은 ID 부여)이 발생합니다.
 *   g_atomic_int_add()는 CPU의 원자적 명령어(lock xadd)를 사용하여
 *   뮤텍스 없이도 안전하게 ID를 증가시킵니다. GMutex보다 ~100배 빠릅니다.
 */
guint64
_register_pending(PcvUringCtx *ctx, PcvUringCallback cb, gpointer data, gint buf_idx)
{
    /* B11-M1: next_id는 이제 volatile guint (32-bit). g_atomic_int_add()와
     * 타입이 일치하므로 캐스트 불필요. 32-bit wrap-around 시 자연스럽게 0으로
     * 귀환하므로 > G_MAXUINT 비교(dead code였던)는 제거한다.
     *
     * B11-C1 (Phase 5): rollover 후 기존 pending과 충돌 시 skip하여
     * misrouted callback 방지. */
    guint id = (guint)g_atomic_int_add((gint *)&ctx->next_id, 1);
    if (id == 0) {
        /* 0은 예약 ID(실패 반환값) — 1로 건너뜀 */
        id = (guint)g_atomic_int_add((gint *)&ctx->next_id, 1);
    }

    /* rollover 후 충돌 방지 — 최대 1024번 skip (실제로는 거의 즉시 찾음) */
    int skip = 0;
    while (g_hash_table_contains(ctx->pending, GUINT_TO_POINTER(id)) && skip < 1024) {
        id = (guint)g_atomic_int_add((gint *)&ctx->next_id, 1);
        if (id == 0)
            id = (guint)g_atomic_int_add((gint *)&ctx->next_id, 1);
        skip++;
    }
    if (skip > 0) {
        PCV_LOG_WARN("uring", "ID collision skip=%d (pending=%u)",
                     skip, g_hash_table_size(ctx->pending));
    }
    /* B11-C3: 1024회 skip 후에도 충돌이면 등록 거부 — 기존 op overwrite 방지.
     * hash table replace는 이전 value의 destroy_notify를 호출하므로, 콜백을
     * 받기 전 op가 해제되어 완료된 CQE가 misrouted 된다. 호출자는 FALSE 경로로
     * 간주하고 요청을 드롭해야 한다 (0 반환은 예약 ID). */
    if (g_hash_table_contains(ctx->pending, GUINT_TO_POINTER(id))) {
        PCV_LOG_WARN("uring", "pending full — refusing registration (size=%u)",
                     g_hash_table_size(ctx->pending));
        return 0;
    }
    PcvUringPendingOp *op = _pending_new(cb, data, buf_idx);
    g_hash_table_insert(ctx->pending, GUINT_TO_POINTER(id), op);
    return (guint64)id;
}

/**
 * @brief io_uring SQE 큐를 커널에 제출 (submit)
 *
 * io_uring_submit()을 호출하여 준비된 SQE들을 커널에 전달합니다.
 * 실패 시 경고 로그를 출력하고 FALSE를 반환합니다.
 *
 * [EAGAIN 재시도 — P5-3]
 *   SQ ring이 가득 찬 경우 io_uring_submit()이 -EAGAIN을 반환합니다.
 *   이때 커널이 SQE를 소비할 시간(1ms)을 주고 1회 재시도합니다.
 *   재시도 후에도 실패하면 FALSE를 반환하여 호출자가 pending op을
 *   정리할 수 있게 합니다. EAGAIN이 아닌 에러는 즉시 실패합니다.
 *   운영 기본 queue depth는 PCV_URING_DEFAULT_QUEUE_DEPTH로 고정해
 *   worker burst 중 이 방어 경로에 들어갈 확률을 낮춥니다.
 *
 * @param ctx io_uring 컨텍스트
 * @return TRUE 성공, FALSE 실패 (SQE가 커널에 전달되지 않음)
 *
 * @note non-static: pcv_uring_socket.c에서 extern으로 참조합니다.
 * @note 호출자가 ctx->submit_mu를 잡은 상태에서 호출해야 합니다.
 */
gboolean
_submit_ring(PcvUringCtx *ctx)
{
    int ret = io_uring_submit(&ctx->ring);
    if (ret >= 0)
        return TRUE;

    /* EAGAIN: SQ ring full — 커널이 SQE를 소비할 시간을 주고 1회 재시도 */
    if (ret == -EAGAIN) {
        /* SQE는 호출자가 submit_mu 아래에서 완성한 상태다. 여기서 잠금을 풀면
         * 다른 submitter가 같은 ring을 제출할 수 있어 pending rollback 판단이
         * 모호해진다. 대신 운영 기본 queue depth를 1024로 올려 이 경로를 최후의
         * 짧은 fail-closed 방어선으로만 남긴다. */
        PCV_LOG_WARN(URING_LOG_DOM, "io_uring_submit returned EAGAIN, retrying after 1ms yield");
        g_usleep(1000);  /* 1ms yield — 커널이 SQ를 drain할 시간 확보 (lock 보유 중) */
        ret = io_uring_submit(&ctx->ring);
        if (ret >= 0)
            return TRUE;
        PCV_LOG_WARN(URING_LOG_DOM, "io_uring_submit retry failed: %s", strerror(-ret));
        return FALSE;
    }

    PCV_LOG_WARN(URING_LOG_DOM, "io_uring_submit failed: %s", strerror(-ret));
    return FALSE;
}

/* ── Lifecycle ────────────────────────────────────────── */

/**
 * @brief io_uring 컨텍스트 생성 — ring + eventfd + GMainLoop 브릿지 + 버퍼 풀
 *
 * 5단계 초기화를 수행합니다:
 *   1. io_uring ring 초기화 (커널 SQ/CQ 매핑)
 *   2. eventfd 생성 + ring에 등록 (CQE 알림용)
 *   3. GMainLoop에 eventfd 감시 등록 (g_unix_fd_add)
 *   4. pending 해시테이블 생성 (sqe_id → 콜백 매핑)
 *   5. 버퍼 풀 생성 (비치명적 — 실패해도 계속 동작)
 *
 * 어느 단계에서든 실패하면 이전 단계에서 할당한 자원을 해제하고 NULL을 반환합니다.
 *
 * @param queue_depth SQ/CQ 엔트리 수 (2의 거듭제곱 권장,
 *                    운영 기본값: PCV_URING_DEFAULT_QUEUE_DEPTH)
 * @param error       실패 시 에러 정보 반환 (NULL 허용)
 * @return 성공 시 PcvUringCtx 포인터, 실패 시 NULL
 *
 * @note queue_depth가 클수록 동시 I/O 처리량이 증가하지만 커널 메모리 사용도 증가합니다.
 */
PcvUringCtx *
pcv_uring_new(guint queue_depth, GError **error)
{
    PcvUringCtx *ctx = g_new0(PcvUringCtx, 1);
    g_mutex_init(&ctx->submit_mu);
    ctx->event_fd = -1;
    ctx->glib_source_id = 0;
    ctx->next_id = 1;

    /* 1. io_uring ring 초기화
     *
     * [DISP-3 불변식 — 반드시 non-SQPOLL] setup flags 는 0 이어야 한다.
     * pcv_uring_socket.c 의 submit 래퍼들은 _submit_ring 실패 시 잔존 SQE 를
     * io_uring_prep_nop 로 덮어써 무해화하는데(해제 buf 로의 recv-write UAF / 유령
     * accept-fd 누수 차단), 이 덮어쓰기는 "커널은 io_uring_enter 시점에만 SQE 를 읽고
     * 모든 제출이 submit_mu 로 직렬화된다"(=non-SQPOLL)는 전제에서만 race-free 하다.
     * IORING_SETUP_SQPOLL 을 추가하면 커널 폴러 스레드가 슬롯을 비동기로 읽어 그 무해화가
     * 깨지고 모든 io_uring I/O 의 실패경로가 UAF 로 회귀한다 → 절대 금지. 아래 assert 가
     * 런타임에 이 불변식을 강제한다(누군가 flags 를 바꾸면 즉시 실패). */
    const unsigned int setup_flags = 0;   /* non-SQPOLL — 위 불변식 참조 */
    int ret = io_uring_queue_init(queue_depth, &ctx->ring, setup_flags);
    if (ret < 0) {
        g_set_error(error, g_quark_from_static_string("uring"), 1,
                    "io_uring_queue_init(%u) failed: %s", queue_depth, strerror(-ret));
        g_free(ctx);
        return NULL;
    }
    g_assert((ctx->ring.flags & IORING_SETUP_SQPOLL) == 0);  /* DISP-3 non-SQPOLL 불변식 */

    /* 2. eventfd 생성 + ring에 등록 */
    ctx->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ctx->event_fd < 0) {
        g_set_error(error, g_quark_from_static_string("uring"), 2,
                    "eventfd() failed: %s", strerror(errno));
        io_uring_queue_exit(&ctx->ring);
        g_free(ctx);
        return NULL;
    }

    ret = io_uring_register_eventfd(&ctx->ring, ctx->event_fd);
    if (ret < 0) {
        g_set_error(error, g_quark_from_static_string("uring"), 3,
                    "io_uring_register_eventfd failed: %s", strerror(-ret));
        close(ctx->event_fd);
        io_uring_queue_exit(&ctx->ring);
        g_free(ctx);
        return NULL;
    }

    /* 3. GMainLoop에 eventfd 등록 */
    ctx->glib_source_id = g_unix_fd_add(ctx->event_fd, G_IO_IN,
                                         _on_uring_ready, ctx);

    /* 4. pending 해시테이블 */
    ctx->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, _pending_free);

    /* 5. 버퍼 풀 */
    GError *buf_err = NULL;
    ctx->buf_pool = pcv_uring_buf_pool_new(PCV_URING_BUF_COUNT,
                                            PCV_URING_BUF_SIZE, &buf_err);
    if (!ctx->buf_pool) {
        PCV_LOG_WARN(URING_LOG_DOM, "Buffer pool init failed: %s",
                     buf_err ? buf_err->message : "unknown");
        if (buf_err) g_error_free(buf_err);
        /* 비치명적: 버퍼 풀 없이도 동작 가능 */
    }

    ctx->running = TRUE;
    PCV_LOG_INFO(URING_LOG_DOM, "Initialized (queue_depth=%u, eventfd=%d, buf_pool=%s)",
                 queue_depth, ctx->event_fd,
                 ctx->buf_pool ? "OK" : "NONE");
    return ctx;
}

/**
 * @brief io_uring 컨텍스트 해제 — 생성 역순으로 자원 정리
 *
 * 해제 순서: GMainLoop 소스 → eventfd → pending 해시테이블 → 버퍼 풀 → ring → 뮤텍스
 * pending ops가 남아있으면 경고 로그를 출력합니다 (누수 진단용).
 *
 * @param ctx io_uring 컨텍스트 (NULL이면 무시 — 안전한 호출)
 */
void
pcv_uring_free(PcvUringCtx *ctx)
{
    if (!ctx) return;
    ctx->running = FALSE;  /* 추가 SQE 제출 차단 */

    /* GMainLoop 소스 제거 */
    if (ctx->glib_source_id > 0)
        g_source_remove(ctx->glib_source_id);

    /* eventfd 닫기 */
    if (ctx->event_fd >= 0)
        close(ctx->event_fd);

    /* pending 정리 */
    if (ctx->pending) {
        guint remaining = g_hash_table_size(ctx->pending);
        if (remaining > 0)
            PCV_LOG_WARN(URING_LOG_DOM, "Shutdown with %u pending ops", remaining);
        g_hash_table_destroy(ctx->pending);
    }

    /* 버퍼 풀 해제 */
    if (ctx->buf_pool)
        pcv_uring_buf_pool_free(ctx->buf_pool);

    /* ring 해제 */
    io_uring_queue_exit(&ctx->ring);

    g_mutex_clear(&ctx->submit_mu);
    g_free(ctx);
    PCV_LOG_INFO(URING_LOG_DOM, "Shutdown complete");
}

/**
 * @brief io_uring 사용 가능 여부 조회
 *
 * PCV_USE_URING=1로 컴파일되었을 때 항상 TRUE를 반환합니다.
 * PCV_USE_URING=0이면 이 파일 자체가 컴파일되지 않으므로
 * 헤더의 static inline 버전이 FALSE를 반환합니다.
 *
 * @return TRUE (io_uring 활성화됨)
 */
gboolean
pcv_uring_is_available(void)
{
    return TRUE;
}

/* ── Submit: read/write ───────────────────────────────── */

/**
 * @brief 비동기 파일 읽기(read) SQE를 io_uring에 제출
 *
 * [내부 동작]
 *   1. pending 해시테이블에 콜백 등록 → sqe_id 획득
 *   2. SQE 슬롯 획득 (SQ full이면 pending 롤백 후 실패)
 *   3. io_uring_prep_read()로 SQE 설정
 *   4. io_uring_submit()으로 커널에 제출
 *
 * @param ctx     io_uring 컨텍스트
 * @param fd      읽을 파일 디스크립터
 * @param buf     데이터를 받을 버퍼 (CQE 완료까지 유효해야 함)
 * @param len     읽을 최대 바이트 수
 * @param offset  파일 오프셋 (0이면 현재 위치, 소켓은 무시)
 * @param cb      완료 콜백 — result는 읽은 바이트 수 또는 -errno
 * @param data    콜백에 전달할 사용자 데이터
 * @return TRUE 제출 성공, FALSE 실패 (ctx NULL, 종료 중, SQ 만석)
 */
gboolean
pcv_uring_submit_read(PcvUringCtx *ctx, int fd, void *buf, gsize len,
                       off_t offset, PcvUringCallback cb, gpointer data)
{
    if (!ctx || !ctx->running) return FALSE;

    g_mutex_lock(&ctx->submit_mu);  /* SQE 제출 직렬화 */

    guint64 id = _register_pending(ctx, cb, data, -1);  /* buf_idx=-1: 버퍼 풀 미사용 */
    if (id == 0) {
        g_mutex_unlock(&ctx->submit_mu);
        return FALSE;  /* B11-C3: pending 테이블 포화 — 요청 드롭 */
    }

    /* [SQ full 백프레셔 동작 — 주니어 필독]
     *   SQ(Submission Queue)는 고정 크기 링 버퍼입니다 (queue_depth 크기).
     *   모든 SQE 슬롯이 사용 중이면 io_uring_get_sqe()가 NULL을 반환합니다.
     *   이때의 대응 전략:
     *   1. io_uring_submit()으로 대기 중인 SQE를 커널에 전달 → SQ 슬롯 회수
     *   2. 다시 io_uring_get_sqe() 시도
     *   3. 여전히 NULL이면 → 커널이 아직 처리 중 → 요청을 드롭 (FALSE 반환)
     *   호출자는 FALSE를 받으면 RPC 에러를 반환하거나 나중에 재시도해야 합니다.
     */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        /* SQ full — 백프레셔: submit 후 재시도 */
        io_uring_submit(&ctx->ring);
        sqe = io_uring_get_sqe(&ctx->ring);
        if (!sqe) {
            /* submit 후에도 SQ full — pending 등록을 롤백하고 실패 반환 */
            g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
            g_mutex_unlock(&ctx->submit_mu);
            PCV_LOG_WARN(URING_LOG_DOM, "SQ still full after submit (read) — dropping request");
            return FALSE;
        }
    }

    io_uring_prep_read(sqe, fd, buf, (unsigned)len, offset);
    io_uring_sqe_set_data64(sqe, id);  /* CQE에서 이 id로 pending op을 찾음 */

    gboolean ok = _submit_ring(ctx);
    if (!ok) {
        /* submit 실패 — pending op을 정리하여 orphan 방지 (P5-3) */
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
    }
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

/**
 * @brief 비동기 파일 쓰기(write) SQE를 io_uring에 제출
 *
 * pcv_uring_submit_read()와 동일한 패턴이지만 io_uring_prep_write()를 사용합니다.
 *
 * @param ctx     io_uring 컨텍스트
 * @param fd      쓸 파일 디스크립터
 * @param buf     쓸 데이터 버퍼 (CQE 완료까지 유효해야 함)
 * @param len     쓸 바이트 수
 * @param offset  파일 오프셋 (0이면 현재 위치)
 * @param cb      완료 콜백 — result는 쓴 바이트 수 또는 -errno
 * @param data    콜백에 전달할 사용자 데이터
 * @return TRUE 제출 성공, FALSE 실패
 */
gboolean
pcv_uring_submit_write(PcvUringCtx *ctx, int fd, const void *buf, gsize len,
                        off_t offset, PcvUringCallback cb, gpointer data)
{
    if (!ctx || !ctx->running) return FALSE;

    g_mutex_lock(&ctx->submit_mu);

    guint64 id = _register_pending(ctx, cb, data, -1);
    if (id == 0) {
        g_mutex_unlock(&ctx->submit_mu);
        return FALSE;  /* B11-C3: pending 테이블 포화 */
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        /* SQ full — 백프레셔: submit 후 재시도 */
        io_uring_submit(&ctx->ring);
        sqe = io_uring_get_sqe(&ctx->ring);
        if (!sqe) {
            g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
            g_mutex_unlock(&ctx->submit_mu);
            PCV_LOG_WARN(URING_LOG_DOM, "SQ still full after submit (write) — dropping request");
            return FALSE;
        }
    }

    io_uring_prep_write(sqe, fd, buf, (unsigned)len, offset);
    io_uring_sqe_set_data64(sqe, id);

    gboolean ok = _submit_ring(ctx);
    if (!ok) {
        /* submit 실패 — pending op을 정리하여 orphan 방지 (P5-3) */
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
    }
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

#endif /* PCV_USE_URING */
