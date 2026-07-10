/**
 * @file virt_conn_pool.c
 * @brief libvirt 연결 풀링 — 서킷 브레이커 통합 장애 허용
 *
 * == 아키텍처에서의 위치 ==
 *   vm_manager.c / handler_*.c → virt_conn_pool_acquire() → libvirt API 호출
 *                               → virt_conn_pool_release() → 풀에 반환
 *
 * == 풀 동작 원리 ==
 *   1. acquire(): 유휴 큐에서 커넥션 꺼냄 → 건강 검사 → 반환
 *      - 유휴 없으면 새로 생성 (max_size 이하)
 *      - 풀 고갈 시 10초 타임아웃으로 대기
 *   2. release(): 사용 끝난 커넥션을 유휴 큐에 반환
 *   3. 건강 검사: virConnectIsAlive()로 확인, 죽었으면 자동 재연결
 *
 * == 서킷 브레이커 연동 ==
 *   - pool_new_conn(): OPEN 상태면 즉시 NULL 반환 (연결 시도 안 함)
 *   - 연결 성공 시 cb_record_success(), 실패 시 cb_record_failure()
 *   - 반복 실패 → 서킷 OPEN → 백오프 후 HALF_OPEN → 프로브 → 복구/재차단
 *
 * == 텔레메트리 제외 ==
 *   telemetry.c는 1초 간격 폴링 전용 장기 커넥션을 독립적으로 관리합니다.
 *   이 풀을 사용하면 1초마다 acquire/release가 발생하여 풀 고갈 위험이 있습니다.
 *
 * == 스레드 안전성 ==
 *   GMutex + GCond로 구현. acquire/release는 어느 스레드에서든 안전합니다.
 */
/* src/modules/virt/virt_conn_pool.c
 *
 * Sprint B-2: libvirt 커넥션 풀 구현
 *
 * [설계 원칙]
 * - GQueue  : 유휴(idle) 커넥션 FIFO 큐
 * - GMutex  : 큐 접근 직렬화
 * - GCond   : 유휴 커넥션이 없을 때 acquire()를 블로킹, release() 시 신호
 * - max_size: 동시에 존재할 수 있는 커넥션 상한 (acquire 시 초과 불가)
 *
 * [커넥션 건강 검사]
 * acquire() 시 virConnectIsAlive()로 상태를 확인하고,
 * 끊어진 커넥션은 즉시 재연결합니다.
 *
 * [telemetry.c 제외]
 * 텔레메트리 데몬은 전용 장기 커넥션을 독립적으로 관리하므로
 * 이 풀을 사용하지 않습니다.
 */

#include "virt_conn_pool.h"
#include "circuit_breaker.h"
#include "../../utils/pcv_log.h"
#include <glib.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>   /* virGetLastError() 명시적 선언 */

#define POOL_URI "qemu:///system"

/* ── 모듈 내부 상태 ───────────────────────────────────── */

typedef struct {
    GQueue  *idle;       /* 유휴 virConnectPtr 큐           */
    GMutex   mutex;
    GCond    cond;       /* idle이 빌 때 acquire 블로킹용   */
    guint    max_size;   /* 풀 상한                         */
    guint    total;      /* 현재 생성된 커넥션 총 수        */
    gboolean shutdown;   /* 종료 플래그                     */
    gdouble  wait_total_us; /* 누적 대기 시간 (마이크로초)  */
    guint64  wait_count;    /* 대기 발생 횟수               */
} ConnPool;

static ConnPool g_pool = { 0 };

/* ── 내부 헬퍼 ────────────────────────────────────────── */

/* 새 커넥션을 열고 반환. 실패 시 NULL + cb_record_failure(). */
static virConnectPtr pool_new_conn(void) {
    /* 서킷 브레이커 OPEN이면 연결 시도 자체를 차단하는 이유:
     * 장애 서버에 반복 연결하면 (1) 타임아웃으로 RPC 응답이 수십 초 지연되고
     * (2) 장애 서버의 복구를 방해한다. 빠른 실패(fast-fail)가 핵심. */
    if (cb_is_open()) {
        PCV_LOG_WARN("conn_pool",
                     "Circuit OPEN — libvirt connection blocked (state=%s, failures=%d)",
                     cb_get_state_str(), cb_get_failure_count());
        return NULL;
    }

    /* 테스트 격리: PCV_LIBVIRT_URI 환경변수 우선 (test:///default 등) */
    const gchar *uri_env = g_getenv("PCV_LIBVIRT_URI");
    virConnectPtr conn = virConnectOpen((uri_env && *uri_env) ? uri_env : POOL_URI);
    if (!conn) {
        PCV_LOG_WARN("conn_pool", "virConnectOpen failed: %s",
                     virGetLastError() ? virGetLastError()->message : "unknown");
        cb_record_failure();
        return NULL;
    }

    cb_record_success();
    return conn;
}

/* [P2-4 수정] pool_ensure_alive() 삭제 이유:
 * 이전 구조: idle에서 pop → mutex unlock → alive 검사 → 사용
 * 문제: unlock 후 alive 검사 통과 → 사용 직전에 커넥션이 stale 됨 (check-then-use 레이스).
 * 수정: alive 검사를 acquire() 내부 mutex 보호 구간으로 이동하여 원자성 보장. */

/**
 * _pool_idle_reaper — 유휴 연결 상태 점검 (30초 주기)
 *
 * libvirt는 ~60초 유휴 시 연결을 타임아웃한다.
 * 사전에 virConnectIsAlive()로 확인하여 stale 연결을 미리 교체한다.
 */
static gboolean
_pool_idle_reaper(gpointer user_data)
{
    (void)user_data;
    g_mutex_lock(&g_pool.mutex);

    if (g_pool.shutdown) {
        g_mutex_unlock(&g_pool.mutex);
        return G_SOURCE_REMOVE;
    }

    guint len = g_queue_get_length(g_pool.idle);
    for (guint i = 0; i < len; i++) {
        virConnectPtr conn = g_queue_pop_head(g_pool.idle);
        if (!conn) continue;

        /* 뮤텍스 밖에서 virConnectIsAlive 호출하면 블로킹 가능하므로
         * 여기서는 간단한 alive 검사만 수행 (풀 전용 30초 주기이므로 허용) */
        if (virConnectIsAlive(conn) == 1) {
            g_queue_push_tail(g_pool.idle, conn);
        } else {
            PCV_LOG_INFO("conn_pool", "Reaping stale idle connection (slot %u/%u)", i, len);
            g_mutex_unlock(&g_pool.mutex);
            virConnectClose(conn);

            /* Proactive reconnect */
            virConnectPtr fresh = pool_new_conn();
            g_mutex_lock(&g_pool.mutex);
            if (fresh) {
                g_queue_push_tail(g_pool.idle, fresh);
            } else {
                PCV_LOG_WARN("conn_pool", "Reconnect failed during idle reap");
                g_pool.total--;
            }
        }
    }

    g_mutex_unlock(&g_pool.mutex);
    return G_SOURCE_CONTINUE;
}

/* ── 공개 API 구현 ────────────────────────────────────── */

/**
 * virt_conn_pool_init — 연결 풀 초기화 + 서킷 브레이커 + 1개 사전 연결
 *
 * [호출 시점] main.c 데몬 초기화 시 1회 호출
 * [동작] 1→서킷 브레이커 초기화 → 2→GMutex/GCond/GQueue 생성
 *        → 3→최소 1개 연결을 사전에 열어 초기 요청 지연 방지
 * [스레드] 메인 스레드 (데몬 시작 시)
 * [주의] max_size는 daemon.conf [libvirt] pool_max_conn (기본 8, 범위 1-64)에서 설정.
 *        virt_conn_pool_shutdown()과 짝을 이루어야 합니다.
 *
 * @param max_size 풀 최대 연결 수 (0이면 기본 8)
 */
void virt_conn_pool_init(guint max_size) {
    cb_init();  /* Circuit Breaker 초기화 */
    g_mutex_init(&g_pool.mutex);
    g_cond_init(&g_pool.cond);
    g_pool.idle     = g_queue_new();
    g_pool.max_size = (max_size > 0) ? max_size : 8;
    g_pool.total    = 0;
    g_pool.shutdown = FALSE;

    /* 최소 1개를 미리 열어 초기 응답 지연 방지 */
    virConnectPtr conn = pool_new_conn();
    if (conn) {
        g_queue_push_tail(g_pool.idle, conn);
        g_pool.total = 1;
    }

    /* 30초 주기 유휴 연결 점검 타이머 등록 */
    g_timeout_add_seconds(30, _pool_idle_reaper, NULL);

    PCV_LOG_INFO("conn_pool", "Initialized (max=%u, pre-opened=%u)",
              g_pool.max_size, g_pool.total);
}

/**
 * virt_conn_pool_acquire — 풀에서 libvirt 연결을 빌려옴
 *
 * [호출 시점] vm_manager.c, handler_*.c 등에서 libvirt API 호출 전
 * [동작] Case 1: 유휴 커넥션 있음 → 꺼냄 → 건강검사(virConnectIsAlive+GetVersion) → 반환
 *        Case 2: 유휴 없지만 상한 미달 → 새 커넥션 생성 (pool_new_conn → CB 연동)
 *        Case 3: 풀 고갈(모두 사용 중) → GCond 10초 타임아웃 대기 → release 신호로 깨어남
 * [스레드] 어느 스레드에서든 안전 (GMutex + GCond 보호)
 * [주의] 반환된 커넥션은 반드시 virt_conn_pool_release()로 반납해야 합니다!
 *        반납하지 않으면 풀이 고갈되어 다른 모든 RPC가 10초 타임아웃됩니다.
 *        건강검사는 뮤텍스 밖에서 수행합니다 (virConnectIsAlive가 블로킹 가능).
 *        서킷 브레이커가 OPEN이면 pool_new_conn()이 NULL을 반환합니다.
 *
 * @return virConnectPtr (성공) 또는 NULL (풀 고갈/서킷 OPEN/shutdown 중)
 */
virConnectPtr virt_conn_pool_acquire(void) {
    g_mutex_lock(&g_pool.mutex);

    while (!g_pool.shutdown) {

        /* Case 1: 유휴 커넥션 있음 → 꺼내서 건강 검사 후 반환
         * 건강 검사를 뮤텍스 안에서 수행하여 check-then-use 레이스 방지:
         * 이전에는 뮤텍스 밖에서 alive 검사를 했기 때문에 검사 통과 후
         * 실제 사용 전에 커넥션이 stale 될 수 있었다. */
        if (!g_queue_is_empty(g_pool.idle)) {
            virConnectPtr conn = g_queue_pop_head(g_pool.idle);

            /* 위험: virConnectIsAlive()를 뮤텍스 안에서 호출하면 블로킹 가능성이 있다.
             * 하지만 뮤텍스 밖에서 호출하면 check-then-use 레이스가 발생한다 (P2-4).
             * 트레이드오프: 블로킹 위험 < 레이스 위험. reaper가 30초마다 stale을 사전 제거하여 완화. */
            if (conn && virConnectIsAlive(conn) == 1) {
                unsigned long ver = 0;
                if (virConnectGetVersion(conn, &ver) >= 0) {
                    cb_record_success();
                    g_mutex_unlock(&g_pool.mutex);
                    return conn;
                }
            }
            /* 커넥션 dead 또는 unresponsive → 닫고 새로 생성 (뮤텍스 안에서) */
            if (conn) {
                PCV_LOG_WARN("conn_pool",
                             "Stale connection detected during acquire — reconnecting...");
                virConnectClose(conn);
            }
            virConnectPtr fresh = pool_new_conn();
            if (fresh) {
                g_mutex_unlock(&g_pool.mutex);
                return fresh;
            }
            /* 재연결 실패 → total 감소 후 NULL 반환 */
            g_pool.total--;
            g_mutex_unlock(&g_pool.mutex);
            return NULL;
        }

        /* Case 2: 유휴는 없지만 상한 미달 → 새 커넥션 생성.
         * total++을 mutex 안에서 먼저 하고, unlock 후 실제 연결하는 이유:
         * 예약(total++) 없이 unlock하면 다른 스레드도 동시에 Case 2에 진입하여
         * max_size를 초과할 수 있다. 연결 실패 시 total--로 예약을 취소한다. */
        if (g_pool.total < g_pool.max_size) {
            g_pool.total++;
            g_mutex_unlock(&g_pool.mutex);

            virConnectPtr conn = pool_new_conn();
            if (!conn) {
                g_mutex_lock(&g_pool.mutex);
                g_pool.total--;
                g_mutex_unlock(&g_pool.mutex);
                return NULL;
            }
            return conn;
        }

        /* Case 3: 풀 고갈 — 10초 타임아웃 대기.
         * 흔한 실수: acquire 후 release를 빠뜨리면 풀이 영구 고갈되어
         * 모든 후속 RPC가 여기서 10초 타임아웃된다. 반드시 release()와 짝을 맞출 것. */
        gint64 wait_start = g_get_monotonic_time();
        gint64 deadline = wait_start + 10 * G_TIME_SPAN_SECOND;
        if (!g_cond_wait_until(&g_pool.cond, &g_pool.mutex, deadline)) {
            /* 타임아웃 — 대기 시간 기록 */
            gint64 waited = g_get_monotonic_time() - wait_start;
            g_pool.wait_total_us += (gdouble)waited;
            g_pool.wait_count++;
            PCV_LOG_WARN("conn_pool", "acquire() timed out waiting for a connection");
            g_mutex_unlock(&g_pool.mutex);
            return NULL;
        }
        /* 대기 후 깨어남 — 대기 시간 기록 */
        {
            gint64 waited = g_get_monotonic_time() - wait_start;
            g_pool.wait_total_us += (gdouble)waited;
            g_pool.wait_count++;
        }
    }

    /* shutdown 중 */
    g_mutex_unlock(&g_pool.mutex);
    return NULL;
}

/**
 * virt_conn_pool_release — 사용 완료된 커넥션을 풀에 반납
 *
 * [호출 시점] libvirt API 호출 완료 후 (acquire와 반드시 짝을 이루어야 함)
 * [동작] 풀에 반납 → GCond 시그널로 대기 중인 acquire() 깨움
 *        shutdown 중이면 즉시 virConnectClose()로 닫음
 * [스레드] 어느 스레드에서든 안전 (GMutex 보호)
 * [주의] NULL 안전 — conn이 NULL이면 즉시 반환
 *
 * @param conn 반납할 libvirt 연결 (NULL이면 무시)
 */
void virt_conn_pool_release(virConnectPtr conn) {
    if (!conn) return;

    g_mutex_lock(&g_pool.mutex);

    if (g_pool.shutdown) {
        /* 종료 중이면 바로 닫기 */
        g_pool.total--;
        g_mutex_unlock(&g_pool.mutex);
        virConnectClose(conn);
        return;
    }

    /* 풀에 반환 후 signal로 대기 중인 acquire() 1개를 깨운다.
     * broadcast가 아닌 signal인 이유: 커넥션 1개 반환에 1개 스레드만 깨우면 충분하다. */
    g_queue_push_tail(g_pool.idle, conn);
    g_cond_signal(&g_pool.cond);

    g_mutex_unlock(&g_pool.mutex);
}

void virt_conn_pool_shutdown(void) {
    g_mutex_lock(&g_pool.mutex);
    g_pool.shutdown = TRUE;
    /* shutdown 시에는 broadcast: 모든 대기 스레드가 while 루프를 탈출해야 한다.
     * signal(1개만 깨움)을 쓰면 나머지 스레드가 영원히 대기하여 데몬 종료가 hang된다. */
    g_cond_broadcast(&g_pool.cond);
    g_mutex_unlock(&g_pool.mutex);

    /* 남은 유휴 커넥션 전부 닫기 */
    virConnectPtr conn;
    while ((conn = g_queue_pop_head(g_pool.idle)) != NULL) {
        virConnectClose(conn);
        g_pool.total--;
    }

    g_queue_free(g_pool.idle);
    g_pool.idle = NULL;
    g_mutex_clear(&g_pool.mutex);
    g_cond_clear(&g_pool.cond);

    cb_shutdown();  /* Circuit Breaker 자원 해제 */
    PCV_LOG_INFO("conn_pool", "Shutdown complete.");
}

void virt_conn_pool_stats(guint *out_idle, guint *out_total, guint *out_max) {
    g_mutex_lock(&g_pool.mutex);
    if (out_idle)  *out_idle  = g_queue_get_length(g_pool.idle);
    if (out_total) *out_total = g_pool.total;
    if (out_max)   *out_max   = g_pool.max_size;
    g_mutex_unlock(&g_pool.mutex);
}

gdouble virt_conn_pool_wait_avg_seconds(void) {
    g_mutex_lock(&g_pool.mutex);
    gdouble avg = (g_pool.wait_count > 0)
                  ? (g_pool.wait_total_us / (gdouble)g_pool.wait_count) / 1e6
                  : 0.0;
    g_mutex_unlock(&g_pool.mutex);
    return avg;
}
