/* src/modules/daemons/alert_silence.h
 *
 * [백엔드 4차] 알림 음소거(Silence) 스토어 — 계획된 유지보수 시 노이즈 억제.
 *
 * [추출 배경 — AIO-3]
 *   원래 alert_engine.c 안에 있던 self-contained 음소거 스토어(g_silences +
 *   3함수)를 별도 TU 로 분리했다. 이유:
 *     - 효과 테스트가 실제 프로덕션 함수(pcv_alert_is_silenced)를 호출해야 하는데,
 *       alert_engine.c 전체를 test_runner 에 링크하면
 *       pcv_alert_record_security_event 가 test_stubs.c 스텁과 중복정의로 충돌한다.
 *     - restart_breaker.c 선례와 동일하게 순수 로직 단위를 분리해 실-코드
 *       효과 테스트 경로를 확보한다.
 *   발화측(_fire_alert)의 pcv_alert_is_silenced 호출부와 PCV_SAFETY_CONTROL
 *   마커는 alert_engine.c 에 그대로 남는다.
 *
 * [매칭 의미 — AIO-3 시정]
 *   매칭은 match-time casefold(g_ascii_strcasecmp)로 대소문자를 무시한다.
 *   발화측 메트릭 이름("CPU"/"Memory"/"Disk")은 프로그램 생성 ASCII 이고
 *   음소거측은 사용자 입력(임의 대소문자)이므로, 정확 매칭(g_strcmp0)은 침묵을
 *   무력화한다. 저장은 원문 그대로 유지(pcv_alert_get_silences 가 UI 에 원문 반환).
 */
#ifndef PURECVISOR_ALERT_SILENCE_H
#define PURECVISOR_ALERT_SILENCE_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

void       pcv_alert_add_silence(const gchar *metric, gint duration_min, const gchar *reason);

gboolean   pcv_alert_is_silenced(const gchar *metric);

JsonArray *pcv_alert_get_silences(void);

void       pcv_alert_silence_reset(void);

G_END_DECLS

#endif
