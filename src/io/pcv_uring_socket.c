/**
 * @file pcv_uring_socket.c
 * @brief io_uring 소켓 전용 SQE 래퍼 — accept / connect / send / recv
 *
 * Phase U-2에서 도입된 소켓 I/O 모듈입니다.
 * UDS 서버(uds_server.c)가 GSocketService 대신 io_uring으로
 * ACCEPT/READ/WRITE 루프를 수행할 때 사용합니다.
 *
 * [아키텍처 위치]
 *   uds_server.c (UDS 서버)
 *      │
 *      ├─ pcv_uring_submit_accept()  → 클라이언트 연결 대기
 *      ├─ pcv_uring_submit_recv()    → JSON-RPC 요청 수신
 *      ├─ pcv_uring_submit_send()    → JSON-RPC 응답 전송
 *      └─ close(fd)                  → 연결 종료
 *   rest_server.c (REST-UDS 브릿지, Phase U-3)
 *      └─ pcv_uring_submit_connect() → UDS 소켓 연결 (raw socket 직접 I/O)
 *
 * [핵심 패턴: pcv_uring.c 내부 함수 extern 참조]
 *   _register_pending() : 콜백+user_data를 pending 해시테이블에 등록, sqe_id 반환
 *   _submit_ring()      : io_uring_submit() 호출, 실패 시 FALSE
 *   이 두 함수는 pcv_uring.c에서 non-static으로 정의되어 있으며,
 *   같은 라이브러리(libpurecvisor) 내부에서만 사용됩니다.
 *
 * [각 함수의 io_uring SQE 매핑]
 *   pcv_uring_submit_accept()  → io_uring_prep_accept()  (IORING_OP_ACCEPT)
 *   pcv_uring_submit_connect() → io_uring_prep_connect() (IORING_OP_CONNECT)
 *   pcv_uring_submit_send()    → io_uring_prep_send()    (IORING_OP_SEND)
 *   pcv_uring_submit_recv()    → io_uring_prep_recv()    (IORING_OP_RECV)
 *
 * [스레드 안전]
 *   모든 함수는 ctx->submit_mu 뮤텍스로 SQE 제출을 직렬화합니다.
 *   SQ(Submission Queue) full 시 pending 등록을 롤백하고 FALSE 반환.
 *
 * [주의사항]
 *   - accept의 콜백 result는 새 클라이언트 fd (양수) 또는 -errno (음수)
 *   - send/recv의 buf 포인터는 CQE 완료까지 유효해야 함 (스택 변수 사용 금지)
 *   - connect의 result는 0(성공) 또는 -errno (음수)
 *   - buf_index=-1: 소켓 I/O는 버퍼 풀을 사용하지 않음 (호출자 관리)
 */
#include "pcv_uring.h"

#if PCV_USE_URING

#include "utils/pcv_log.h"
#include <string.h>
#include <errno.h>

#define SOCK_LOG_DOM "uring_socket"

/* ── 내부: pending 등록 + SQE 제출 (pcv_uring.c에서 노출) ── */
/* pcv_uring.c 내부 함수를 extern으로 사용 — 같은 라이브러리 내 */
extern guint64 _register_pending(PcvUringCtx *ctx, PcvUringCallback cb,
                                  gpointer data, gint buf_idx);
extern gboolean _submit_ring(PcvUringCtx *ctx);

/* ── accept ───────────────────────────────────────────── */

/**
 * @brief 비동기 소켓 accept SQE 제출 — 클라이언트 연결 대기
 *
 * UDS 서버의 listen 소켓에서 새 클라이언트 연결을 비동기로 수락합니다.
 * 콜백의 result는 새 클라이언트 fd(양수) 또는 -errno(음수)입니다.
 *
 * [사용 패턴 (uds_server.c)]
 *   1. pcv_uring_submit_accept() → 연결 대기 SQE 제출
 *   2. 클라이언트 접속 시 콜백 호출 (result = 새 fd)
 *   3. pcv_uring_submit_recv()로 요청 수신
 *   4. 처리 후 pcv_uring_submit_send()로 응답 전송
 *   5. close(fd)
 *   6. 다시 1번으로 (accept 루프)
 *
 * @param ctx        io_uring 컨텍스트
 * @param listen_fd  리슨 소켓 fd (bind + listen 완료 상태)
 * @param addr       클라이언트 주소 저장용 (NULL 허용)
 * @param addrlen    addr 크기 포인터 (NULL 허용)
 * @param cb         완료 콜백 — result=새 클라이언트 fd 또는 -errno
 * @param data       콜백에 전달할 사용자 데이터
 * @return TRUE 제출 성공, FALSE 실패
 */
gboolean
pcv_uring_submit_accept(PcvUringCtx *ctx, int listen_fd,
                         struct sockaddr *addr, socklen_t *addrlen,
                         PcvUringCallback cb, gpointer data)
{
    if (!ctx || !ctx->running) return FALSE;

    g_mutex_lock(&ctx->submit_mu);

    guint64 id = _register_pending(ctx, cb, data, -1);
    if (id == 0) {
        g_mutex_unlock(&ctx->submit_mu);
        return FALSE;  /* B11-C3: pending 테이블 포화 */
    }

    /* B11-M6: accept에 submit-and-retry 추가 — read/write와 동일한 백프레셔 패턴.
     * 이전 구현은 SQ 만석 시 즉시 FALSE를 반환했으나, 대기 중인 SQE를 먼저
     * 커널에 제출(flush)하면 SQ 슬롯이 회수되어 재시도 성공 가능성이 높다.
     * accept는 서버 루프의 핵심 경로이므로 불필요한 실패를 줄이는 것이 중요하다. */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        /* SQ full — 백프레셔: 대기 중인 SQE를 커널에 제출 후 재시도 */
        io_uring_submit(&ctx->ring);
        sqe = io_uring_get_sqe(&ctx->ring);
        if (!sqe) {
            /* submit 후에도 SQ full — pending 등록 롤백 후 실패 반환 */
            g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
            g_mutex_unlock(&ctx->submit_mu);
            PCV_LOG_WARN(SOCK_LOG_DOM, "SQ still full after submit (accept) — dropping");
            return FALSE;
        }
    }

    io_uring_prep_accept(sqe, listen_fd, addr, addrlen, 0);
    io_uring_sqe_set_data64(sqe, id);

    gboolean ok = _submit_ring(ctx);
    if (!ok)
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

/* ── connect ──────────────────────────────────────────── */

/**
 * @brief 비동기 소켓 connect SQE 제출 — UDS 소켓 연결
 *
 * REST 서버가 UDS 소켓에 연결하여 JSON-RPC 요청을 전달할 때 사용합니다.
 * (Phase U-3: REST→UDS 브릿지 전환)
 *
 * @param ctx     io_uring 컨텍스트
 * @param fd      소켓 fd (socket() 호출로 생성된 상태)
 * @param addr    연결 대상 주소 (struct sockaddr_un — UDS 경로)
 * @param addrlen addr 구조체 크기
 * @param cb      완료 콜백 — result=0(성공) 또는 -errno(실패)
 * @param data    콜백에 전달할 사용자 데이터
 * @return TRUE 제출 성공, FALSE 실패
 */
gboolean
pcv_uring_submit_connect(PcvUringCtx *ctx, int fd,
                          const struct sockaddr *addr, socklen_t addrlen,
                          PcvUringCallback cb, gpointer data)
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
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
        g_mutex_unlock(&ctx->submit_mu);
        PCV_LOG_WARN(SOCK_LOG_DOM, "SQ full (connect)");
        return FALSE;
    }

    io_uring_prep_connect(sqe, fd, addr, addrlen);
    io_uring_sqe_set_data64(sqe, id);

    gboolean ok = _submit_ring(ctx);
    if (!ok)
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

/* ── send ─────────────────────────────────────────────── */

/**
 * @brief 비동기 소켓 send SQE 제출 — JSON-RPC 응답 전송
 *
 * @param ctx  io_uring 컨텍스트
 * @param fd   전송 대상 소켓 fd
 * @param buf  전송할 데이터 버퍼 (CQE 완료까지 유효해야 함 — 스택 변수 사용 금지)
 * @param len  전송할 바이트 수
 * @param cb   완료 콜백 — result=전송된 바이트 수 또는 -errno
 * @param data 콜백에 전달할 사용자 데이터
 * @return TRUE 제출 성공, FALSE 실패
 *
 * @warning buf는 커널이 비동기로 읽으므로 CQE 완료 전에 해제하면
 *          use-after-free가 발생합니다. g_strdup() 등으로 힙에 복사하세요.
 */
gboolean
pcv_uring_submit_send(PcvUringCtx *ctx, int fd, const void *buf, gsize len,
                       PcvUringCallback cb, gpointer data)
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
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
        g_mutex_unlock(&ctx->submit_mu);
        PCV_LOG_WARN(SOCK_LOG_DOM, "SQ full (send)");
        return FALSE;
    }

    io_uring_prep_send(sqe, fd, buf, (unsigned)len, 0);
    io_uring_sqe_set_data64(sqe, id);

    gboolean ok = _submit_ring(ctx);
    if (!ok)
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

/* ── recv ─────────────────────────────────────────────── */

/**
 * @brief 비동기 소켓 recv SQE 제출 — JSON-RPC 요청 수신
 *
 * @param ctx  io_uring 컨텍스트
 * @param fd   수신 대상 소켓 fd
 * @param buf  데이터를 받을 버퍼 (CQE 완료까지 유효해야 함)
 * @param len  버퍼 크기 (최대 수신 바이트 수)
 * @param cb   완료 콜백 — result=수신된 바이트 수, 0=연결 종료(EOF), -errno=에러
 * @param data 콜백에 전달할 사용자 데이터
 * @return TRUE 제출 성공, FALSE 실패
 *
 * @note result=0은 상대방이 연결을 닫은 것이므로 close(fd)해야 합니다.
 */
gboolean
pcv_uring_submit_recv(PcvUringCtx *ctx, int fd, void *buf, gsize len,
                       PcvUringCallback cb, gpointer data)
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
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
        g_mutex_unlock(&ctx->submit_mu);
        PCV_LOG_WARN(SOCK_LOG_DOM, "SQ full (recv)");
        return FALSE;
    }

    io_uring_prep_recv(sqe, fd, buf, (unsigned)len, 0);
    io_uring_sqe_set_data64(sqe, id);

    gboolean ok = _submit_ring(ctx);
    if (!ok)
        g_hash_table_remove(ctx->pending, GUINT_TO_POINTER((guint)id));
    g_mutex_unlock(&ctx->submit_mu);
    return ok;
}

#endif /* PCV_USE_URING */
