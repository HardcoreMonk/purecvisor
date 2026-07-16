/* tests/test_alert_silence.c
 *
 * 대상 모듈: src/modules/daemons/alert_silence.c — AIO-3
 *            알림 음소거(Silence) 스토어 (alert_engine.c 에서 추출).
 *
 * 이 테스트가 검증하는 것 (효과 테스트 = 무동작→실동작):
 *   사용자가 소문자("cpu")로 음소거를 등록하면, 엔진이 프로그램 생성
 *   대문자("CPU")로 발화하는 알림이 실제로 억제되는지 —
 *   즉 pcv_alert_is_silenced 의 match-time casefold(g_ascii_strcasecmp).
 *
 * 반사실 (load-bearing):
 *   매칭을 g_strcmp0(대소문자 구분)으로 되돌리면 is_silenced("CPU")==FALSE →
 *   RED. 저장/조회를 다른 대소문자로 하는 것이 casefold 를 load-bearing 하게 만든다.
 *
 * 실행: ./test_runner -p /alert_silence
 * 외부 의존: 없음 (glib + json-glib, 순수 로직).
 */

#include <glib.h>
#include "modules/daemons/alert_silence.h"

/* AIO-3 반사실: 다른 대소문자로 저장/조회해야 casefold 가 load-bearing.
 * g_strcmp0(revert) 이면 is_silenced("CPU")==FALSE → RED. */
static void test_silence_case_insensitive_suppresses(void) {
    pcv_alert_silence_reset();
    pcv_alert_add_silence("cpu", 60, "maint");                 /* 사용자 소문자 입력 */
    g_assert_true (pcv_alert_is_silenced("CPU"));              /* 엔진 발화 "CPU" → 억제돼야 */
    g_assert_false(pcv_alert_is_silenced("NeverSilencedXYZ")); /* 무관 메트릭 negative */
}

/* AIO-7: writer(add)와 reader(is_silenced/get_silences)를 동시에 돌려 리더가
 * _ensure_silence_init() 배리어 없이 g_silences/g_silence_mu에 접근하던 이전
 * 코드였다면 노출됐을 크래시/torn-state가 재발하지 않는지 회귀 고정한다.
 *
 * 한계(정직하게 기록): g_silence_once는 프로세스당 1회만 최초진입하므로, 진짜
 * "최초 동시진입" 레이스 윈도우는 test_runner 프로세스 안에서 이미 앞선
 * 테스트(위 test_silence_case_insensitive_suppresses 등)가 초기화를 끝낸 뒤에는
 * 재현되지 않는다(g_once_init_enter가 이후 호출에서 즉시 already-init 경로로
 * 빠짐). 그럼에도 이 테스트는 모든 리더 진입점이 실제로 _ensure_silence_init()을
 * 거쳐 동시 부하에서 안전하게 동작함을 회귀 고정하며, 최초진입 배리어 자체의
 * 정확성은 g_once_init_enter/leave의 GLib 표준 acquire/release 보장에 의존한다
 * (코드리뷰로 확인된 표준 관용구 — 신규/실험적 기법 아님).
 */
static gpointer silence_writer_worker(gpointer u) {
    (void)u;
    for (int i = 0; i < 200; i++)
        pcv_alert_add_silence("stress", 60, "concurrent");
    return NULL;
}

static gpointer silence_reader_worker(gpointer u) {
    (void)u;
    for (int i = 0; i < 200; i++) {
        pcv_alert_is_silenced("stress");
        JsonArray *arr = pcv_alert_get_silences();
        json_array_unref(arr);
    }
    return NULL;
}

static void test_silence_concurrent_writer_reader_safe(void) {
    pcv_alert_silence_reset();
    GThread *w[4], *r[4];
    for (int i = 0; i < 4; i++) {
        w[i] = g_thread_new("silence-w", silence_writer_worker, NULL);
        r[i] = g_thread_new("silence-r", silence_reader_worker, NULL);
    }
    for (int i = 0; i < 4; i++) {
        g_thread_join(w[i]);
        g_thread_join(r[i]);
    }
    /* 모든 writer가 완료됐으므로 리더는 반드시 억제 상태를 관측해야 한다. */
    g_assert_true(pcv_alert_is_silenced("stress"));
    pcv_alert_silence_reset();
}

void test_alert_silence_register(void) {
    g_test_add_func("/alert_silence/case_insensitive_suppresses",
                    test_silence_case_insensitive_suppresses);
    g_test_add_func("/alert_silence/concurrent_writer_reader_safe",
                    test_silence_concurrent_writer_reader_safe);
}
