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

void test_alert_silence_register(void) {
    g_test_add_func("/alert_silence/case_insensitive_suppresses",
                    test_silence_case_insensitive_suppresses);
}
