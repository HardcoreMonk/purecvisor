/* tests/test_self_healing_anomaly.c
 *
 * AIO-1 검증 — anomaly 트래킹 전용 뮤텍스(g_anomaly_mu)의 다스레드 경쟁 하
 * liveness/무데드락 확인 (concurrency hammer).
 *
 * [대상] src/modules/ai/self_healing.c
 *   - g_recent_anomalies[] / g_last_multi_agent_us 를 g_anomaly_mu 로 가드.
 *   - writer: pcv_healing_on_anomaly() → _track_distinct_anomaly() (RMW)
 *   - reader: pcv_healing_should_trigger_agent_now() (g_last_multi_agent_us 읽기)
 *   AIO-1(커밋: "AIO-1 anomaly 트래킹 전용 뮤텍스")이 eBPF/이벤트/메인 3스레드
 *   경쟁을 leaf 뮤텍스로 직렬화한다.
 *
 * [범위·한계 — 정직한 기록]
 *   진짜 data-race 검증은 ThreadSanitizer 가 이상적이나, GLib GMutex 는 libglib
 *   내부 futex 경로로 동기화하여 미계장 libglib 에 대해 TSan 이 불투명하다 →
 *   올바르게 g_mutex_lock 으로 보호된 코드에도 TSan 이 false data race 를 보고한다
 *   (로컬 실증: GMutex 로 보호한 카운터의 positive control 이 false race 를 냄).
 *   따라서 이 테스트는 race 탐지가 아니라 (a) 3스레드 동시 진입 시 무데드락/무크래시
 *   liveness, (b) init 정합성을 고정한다. race-free 자체는 코드리뷰로 검증:
 *   leaf 뮤텍스 · 두 접근자 모두 대칭 잠금 · _track 은 G.mu 언락(:1046) 후 호출(:1050)
 *   → G.mu↔g_anomaly_mu 중첩 없음 → 순서역전/데드락 없음.
 *   `make sanitize`(ASan/UBSan) 경유 시 동시 경로의 메모리 오류/UB 도 커버된다.
 *
 * [무거운 경로 회피] writer 는 정책 trigger_metric 어디에도 부분문자열이 아닌
 *   2개 distinct 메트릭만 사용 → triggered_count=0 且 distinct<3 → window_trigger=
 *   FALSE → pcv_agent_compare_async(네트워크/스레드) 미발화. 유닛 테스트 안전.
 *
 * 실행: ./test_runner -p /selfhealing/anomaly_track_race
 * 외부 의존: 없음 (config 미초기화 → 기본값; DB/네트워크 무접촉).
 */
#include <glib.h>
#include "modules/ai/self_healing.h"

/* 헤더에 프로토타입이 없는 공개(non-static) 함수 — 링크 심볼 직접 참조. */
extern gboolean pcv_healing_should_trigger_agent_now(void);

#define HAMMER_THREADS 3
#define HAMMER_ITERS   30000

/* 정책 trigger_metric(purecvisor_ / node_ / vm- 계열) 어디에도 substring 이
 * 아님 -> strstr 매칭 0 -> 무거운 경로 미발화. 2개 distinct(<3 임계) 유지. */
static const char *const MET_A = "aio1_hammer_alpha";
static const char *const MET_B = "aio1_hammer_beta";

static gpointer
_writer(gpointer d)
{
    const char *metric = d;
    for (int i = 0; i < HAMMER_ITERS; i++)
        pcv_healing_on_anomaly(metric, 1.0, 5.0, 2.0, NULL);
    return NULL;
}

static gpointer
_reader(gpointer d)
{
    (void)d;
    for (int i = 0; i < HAMMER_ITERS; i++)
        (void)pcv_healing_should_trigger_agent_now();
    return NULL;
}

/* 3스레드(writer×2 distinct + reader)가 g_anomaly_mu 를 동시 두드려도
 * 무데드락으로 완주해야 한다. 완주 자체가 liveness 증거. */
static void
test_anomaly_track_race(void)
{
    pcv_healing_init();  /* G.initialized=TRUE, 내장 정책 10개(합성 메트릭과 미매칭) */

    GThread *th[HAMMER_THREADS];
    th[0] = g_thread_new("aio1-w0", _writer, (gpointer)MET_A);
    th[1] = g_thread_new("aio1-w1", _writer, (gpointer)MET_B);
    th[2] = g_thread_new("aio1-rd", _reader, NULL);
    for (int i = 0; i < HAMMER_THREADS; i++)
        g_thread_join(th[i]);

    /* init 정합성 확인(결정적): config 미로딩 → mode 기본 dry_run → TRUE.
     * 시간 의존 불변식(should_trigger)은 부팅 후 경과에 따라 흔들리므로 assert 안 함;
     * 무데드락 완주가 이 테스트의 핵심 판정이다. */
    g_assert_true(pcv_healing_get_mode());

    /* 해머 후에도 모듈이 응답 가능한지(재진입 무크래시) */
    (void)pcv_healing_should_trigger_agent_now();
    pcv_healing_on_anomaly(MET_A, 1.0, 5.0, 2.0, NULL);
}

void
test_self_healing_anomaly_register(void)
{
    g_test_add_func("/selfhealing/anomaly_track_race", test_anomaly_track_race);
}
