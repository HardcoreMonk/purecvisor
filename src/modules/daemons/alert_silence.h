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

/* ── [백엔드 4차] 알림 음소거 ──────────────────────────────── */

/**
 * pcv_alert_add_silence:
 * @metric: 대상 메트릭 이름 (NULL 허용 — g_strdup(NULL)==NULL 로 저장)
 * @duration_min: 음소거 지속 시간(분)
 * @reason: 음소거 사유 (NULL 허용 — 빈 문자열로 저장)
 *
 * 메트릭을 duration_min 분 동안 음소거 목록에 등록한다.
 */
void       pcv_alert_add_silence(const gchar *metric, gint duration_min, const gchar *reason);

/**
 * pcv_alert_is_silenced:
 * @metric: 확인할 메트릭 이름
 *
 * 해당 메트릭이 현재(미만료) 음소거 중인지 반환한다. 매칭은 대소문자 무시
 * (match-time casefold). Returns: 음소거 중이면 TRUE.
 */
gboolean   pcv_alert_is_silenced(const gchar *metric);

/**
 * pcv_alert_get_silences:
 *
 * 활성(미만료) 음소거 목록을 JSON 배열로 반환한다. metric 은 저장 원문 그대로
 * 노출된다. Returns: 호출자 소유 JsonArray.
 */
JsonArray *pcv_alert_get_silences(void);

/**
 * pcv_alert_silence_reset:
 *
 * 등록된 모든 음소거를 제거한다 — 테스트 격리용.
 */
void       pcv_alert_silence_reset(void);

G_END_DECLS

#endif /* PURECVISOR_ALERT_SILENCE_H */
