/**
 * @file pcv_uring_buf.c
 * @brief io_uring 고정 버퍼 풀 — mmap 사전 할당 + 인덱스 스택 관리
 *
 * Phase U-1에서 도입된 버퍼 풀 모듈입니다.
 * io_uring SQE 제출 시 사용할 버퍼를 사전 할당하여
 * 매 I/O마다 malloc/free 오버헤드를 제거합니다.
 *
 * [아키텍처 위치]
 *   pcv_uring.c가 PcvUringCtx 생성 시 buf_pool을 함께 초기화합니다.
 *   CQE 완료 콜백 후 사용된 버퍼가 자동 반환됩니다 (pcv_uring.c _on_uring_ready).
 *
 * [핵심 자료구조: 인덱스 스택]
 *   base[0..count-1] : mmap으로 할당된 연속 메모리 (count x buf_size)
 *   free_list[0..count-1] : 사용 가능한 인덱스를 담는 스택
 *   free_top : 스택 탑 포인터
 *     - alloc: free_top-- → free_list[free_top] 인덱스 반환
 *     - release: free_list[free_top] = index → free_top++
 *
 * [주요 흐름]
 *   1. pcv_uring_buf_pool_new(64, 65536) → 64개 x 64KB = 4MB mmap 할당
 *   2. pcv_uring_buf_alloc() → 인덱스 반환 (예: 7)
 *   3. pcv_uring_buf_get(pool, 7) → base + 7*65536 포인터 반환
 *   4. I/O 완료 후 pcv_uring_buf_release(pool, 7) → 인덱스 스택에 반환
 *
 * [스레드 안전]
 *   GMutex mu로 alloc/release를 보호합니다.
 *   풀 고갈 시 -1 반환 + 경고 로그 (호출자가 직접 버퍼 할당 필요).
 *
 * [기본 설정] (pcv_uring.h에서 정의)
 *   PCV_URING_BUF_SIZE  = 65536 (64KB) — UDS JSON-RPC + libvirt XML 수용
 *   PCV_URING_BUF_COUNT = 64           — 동시 I/O 64건 커버
 *
 * [주의사항]
 *   - mmap MAP_PRIVATE|MAP_ANONYMOUS 사용 → 페이지 폴트 시 실제 물리 메모리 할당
 *   - pool이 NULL이어도 pcv_uring.c는 동작 (비치명적 경로)
 *   - buf_pool_free 시 mmap 해제 전 free_list를 g_free로 해제
 */
#include "pcv_uring.h"

#if PCV_USE_URING

#include "utils/pcv_log.h"
#include <string.h>
#include <sys/mman.h>

#define BUF_LOG_DOM "uring_buf"

/**
 * @brief 고정 크기 버퍼 풀 생성 — mmap으로 연속 메모리 할당
 *
 * count개의 buf_size 크기 버퍼를 mmap으로 한 번에 할당합니다.
 * 인덱스 스택(free_list)을 초기화하여 O(1) 할당/반환을 지원합니다.
 *
 * @param count    버퍼 개수 (양수 필수)
 * @param buf_size 개별 버퍼 크기 (바이트, 0 불가)
 * @param error    실패 시 에러 정보 (NULL 허용)
 * @return 성공 시 PcvUringBufPool 포인터, 실패 시 NULL
 *
 * @note mmap MAP_PRIVATE|MAP_ANONYMOUS로 할당하므로
 *       실제 물리 메모리는 첫 접근(페이지 폴트) 시 할당됩니다.
 */
PcvUringBufPool *
pcv_uring_buf_pool_new(gint count, gsize buf_size, GError **error)
{
    if (count <= 0 || buf_size == 0) {
        g_set_error(error, g_quark_from_static_string("uring_buf"), 1,
                    "Invalid pool params: count=%d buf_size=%zu", count, buf_size);
        return NULL;
    }

    /* B11-M4: (gsize)count * buf_size 곱셈 오버플로우 방어.
     * count와 buf_size가 모두 크면 gsize(최대 ~18EB on 64-bit) 초과 가능.
     * 실제로는 PCV_URING_BUF_COUNT=64, PCV_URING_BUF_SIZE=64KB이므로
     * 오버플로우는 발생하지 않지만, 외부에서 큰 값을 전달할 수 있으므로 방어한다. */
    if (buf_size != 0 && (gsize)count > G_MAXSIZE / buf_size) {
        g_set_error(error, g_quark_from_static_string("uring_buf"), 3,
                    "Pool size overflow: count=%d * buf_size=%zu exceeds gsize max",
                    count, buf_size);
        return NULL;
    }

    PcvUringBufPool *pool = g_new0(PcvUringBufPool, 1);
    g_mutex_init(&pool->mu);
    pool->buf_size = buf_size;
    pool->count    = count;

    /* 연속 메모리 블록 할당 (mmap aligned) */
    gsize total = (gsize)count * buf_size;
    pool->base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pool->base == MAP_FAILED) {
        g_set_error(error, g_quark_from_static_string("uring_buf"), 2,
                    "mmap(%zu) failed", total);
        g_mutex_clear(&pool->mu);
        g_free(pool);
        return NULL;
    }

    /* free list 초기화 (스택: top=count-1, pop=top--) */
    pool->free_list = g_new(gint, count);
    for (gint i = 0; i < count; i++)
        pool->free_list[i] = i;
    pool->free_top = count;

    /* B11-M5: O(1) 이중 반환 검출용 비트필드 초기화 (모두 사용 가능 = FALSE) */
    pool->in_use = g_new0(gboolean, count);

    PCV_LOG_INFO(BUF_LOG_DOM, "Pool created: %d x %zu = %zu bytes",
                 count, buf_size, total);
    return pool;
}

/**
 * @brief 버퍼 풀 해제 — mmap 영역 해제 + free_list 메모리 해제
 *
 * @param pool 버퍼 풀 (NULL이면 무시 — 안전한 호출)
 *
 * @note 해제 순서: mmap 영역 → free_list 배열 → GMutex → 구조체 자체
 */
void
pcv_uring_buf_pool_free(PcvUringBufPool *pool)
{
    if (!pool) return;

    if (pool->base && pool->base != MAP_FAILED) {
        gsize total = (gsize)pool->count * pool->buf_size;
        munmap(pool->base, total);
    }
    g_free(pool->free_list);
    g_free(pool->in_use);   /* B11-M5: in_use 비트필드 해제 */
    g_mutex_clear(&pool->mu);
    g_free(pool);
}

/**
 * @brief 버퍼 풀에서 버퍼 인덱스 하나를 할당 (O(1) 스택 pop)
 *
 * free_list 스택에서 top을 감소시켜 인덱스를 반환합니다.
 * 스레드 안전합니다 (GMutex 보호).
 *
 * @param pool 버퍼 풀 (NULL이면 -1 반환)
 * @return 버퍼 인덱스 (0~count-1), 풀 고갈 시 -1
 *
 * @note 반환된 인덱스로 pcv_uring_buf_get(pool, idx)을 호출하여
 *       실제 메모리 포인터를 얻습니다.
 * @note 풀 고갈 시 호출자가 직접 g_malloc() 등으로 버퍼를 할당해야 합니다.
 */
gint
pcv_uring_buf_alloc(PcvUringBufPool *pool)
{
    if (!pool) return -1;

    g_mutex_lock(&pool->mu);
    gint idx = -1;
    if (pool->free_top > 0) {
        /* 스택 pop: top 감소 후 해당 위치의 인덱스 반환 */
        pool->free_top--;
        idx = pool->free_list[pool->free_top];
        /* B11-M5: 할당 시 in_use 플래그 설정 → O(1) 이중 반환 검출 */
        pool->in_use[idx] = TRUE;
    }
    g_mutex_unlock(&pool->mu);

    if (idx < 0) {
        /* B11-M1: 로그 폭주 방지 — 1초당 최대 1회만 출력 */
        static gint64 last_warn_us = 0;
        gint64 now = g_get_monotonic_time();
        if (now - last_warn_us > G_USEC_PER_SEC) {
            last_warn_us = now;
            PCV_LOG_WARN(BUF_LOG_DOM, "Buffer pool exhausted (0/%d)",
                         pool->count);
        }
    }
    return idx;
}

/**
 * @brief 사용 완료된 버퍼 인덱스를 풀에 반환 (O(1) 스택 push)
 *
 * CQE 완료 콜백 후 pcv_uring.c의 _on_uring_ready()에서 자동으로 호출됩니다.
 * 수동으로 호출할 수도 있습니다 (이중 반환 방지는 호출자 책임).
 *
 * @param pool  버퍼 풀 (NULL이면 무시)
 * @param index 반환할 버퍼 인덱스 (범위 밖이면 무시)
 *
 * @note free_top == count이면 스택이 이미 가득 찬 상태 (이중 반환 의심)
 */
void
pcv_uring_buf_release(PcvUringBufPool *pool, gint index)
{
    if (!pool || index < 0 || index >= pool->count) return;

    g_mutex_lock(&pool->mu);
    /* B11-M5: O(1) 이중 반환 검증 — in_use 비트필드로 즉시 확인.
     * 이전 구현은 free_list를 O(n) 선형 스캔했으나, in_use[index]가
     * FALSE이면 이미 반환된(또는 한 번도 할당되지 않은) 버퍼이므로 거부한다. */
    if (!pool->in_use[index]) {
        g_mutex_unlock(&pool->mu);
        PCV_LOG_WARN(BUF_LOG_DOM,
                     "Double release detected (O(1)): buf idx=%d — ignoring", index);
        return;
    }
    /* 반환 처리: in_use 클리어 + 스택 push */
    pool->in_use[index] = FALSE;
    if (pool->free_top < pool->count) {
        /* 스택 push: 현재 top 위치에 인덱스 저장 후 top 증가 */
        pool->free_list[pool->free_top] = index;
        pool->free_top++;
    }
    g_mutex_unlock(&pool->mu);
}

/**
 * @brief 버퍼 인덱스에 해당하는 실제 메모리 포인터 반환
 *
 * base 주소에서 index * buf_size만큼 오프셋을 더해 포인터를 계산합니다.
 * 연속 메모리 블록이므로 단순 포인터 산술로 O(1) 접근이 가능합니다.
 *
 * @param pool  버퍼 풀
 * @param index pcv_uring_buf_alloc()으로 획득한 인덱스
 * @return 버퍼 메모리 포인터, 잘못된 인덱스면 NULL
 */
void *
pcv_uring_buf_get(PcvUringBufPool *pool, gint index)
{
    if (!pool || index < 0 || index >= pool->count) return NULL;
    return (char *)pool->base + ((gsize)index * pool->buf_size);
}

#endif /* PCV_USE_URING */
