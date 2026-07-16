/* src/modules/daemons/alert_silence.c
 *
 * [백엔드 4차] 알림 음소거 스토어 — alert_engine.c 에서 추출 (AIO-3).
 * 배경/매칭 의미는 alert_silence.h 헤더 주석 참조. 발화측 호출부와
 * PCV_SAFETY_CONTROL 마커는 alert_engine.c 에 잔존한다.
 */
#include "alert_silence.h"
#include "utils/pcv_log.h"

#include <glib.h>

#define ALERT_SILENCE_LOG_DOM "alert_engine"

typedef struct {
    gchar  *metric;       /* 대상 메트릭 이름 (cpu/mem/disk) */
    gint64  until;        /* 음소거 종료 시각 (monotonic µs) */
    gchar  *reason;       /* 음소거 사유 */
} AlertSilence;

static GPtrArray *g_silences = nullptr;
static GMutex     g_silence_mu;
/* AIO-7: g_silences/g_silence_mu 지연초기화를 g_once로 경쟁 제거.
 * 이전에는 `if (!g_silences)` 무락 검사라 두 스레드가 동시에 최초 진입 시
 * g_mutex_init 이중 호출 + g_ptr_array 이중 할당(누수/torn)이 가능했다. */
static gsize      g_silence_once = 0;

/* AIO-7 리더 배리어: add뿐 아니라 is_silenced/get_silences/reset 등 모든 리더도
 * 반드시 이 진입점을 거쳐야 한다. 이전에는 리더가 `if (!g_silences)` 무락 검사만
 * 하고 바로 g_silences/g_silence_mu를 참조했는데, 이는 g_once_init_enter/leave가
 * 제공하는 acquire/release 배리어 밖의 접근이라 — writer 스레드의 g_mutex_init/
 * g_ptr_array_new 완료를 리더가 관측하지 못한 채(컴파일러/CPU 재정렬) 그 값을
 * 읽어 들일 수 있는 데이터 레이스였다. 모든 경로가 g_once_init_enter/leave를
 * 통과하게 하여 배리어 안에서만 접근하도록 통일한다. */
static void
_ensure_silence_init(void)
{
    if (g_once_init_enter(&g_silence_once)) {
        g_mutex_init(&g_silence_mu);
        g_silences = g_ptr_array_new_with_free_func(g_free);
        g_once_init_leave(&g_silence_once, 1);
    }
}

void
pcv_alert_add_silence(const gchar *metric, gint duration_min, const gchar *reason)
{
    _ensure_silence_init();
    AlertSilence *s = g_new0(AlertSilence, 1);
    s->metric = g_strdup(metric);
    s->until  = g_get_monotonic_time() + (gint64)duration_min * 60 * G_USEC_PER_SEC;
    s->reason = g_strdup(reason ? reason : "");

    g_mutex_lock(&g_silence_mu);
    g_ptr_array_add(g_silences, s);
    g_mutex_unlock(&g_silence_mu);

    PCV_LOG_INFO(ALERT_SILENCE_LOG_DOM, "Alert silenced: metric=%s duration=%dmin reason=%s",
                 metric, duration_min, reason ? reason : "");
}

gboolean
pcv_alert_is_silenced(const gchar *metric)
{
    _ensure_silence_init();
    if (!metric) return FALSE;
    gint64 now = g_get_monotonic_time();
    gboolean silenced = FALSE;

    g_mutex_lock(&g_silence_mu);
    for (guint i = 0; i < g_silences->len; i++) {
        AlertSilence *s = g_ptr_array_index(g_silences, i);
        /* AIO-3: match-time casefold — 발화측 "CPU" 와 사용자 "cpu" 를 매칭.
         * s->metric && 가드는 g_strcmp0 이 제공하던 NULL-tolerance 복원. */
        if (s->metric && g_ascii_strcasecmp(s->metric, metric) == 0 && now < s->until) {
            silenced = TRUE;
            break;
        }
    }
    g_mutex_unlock(&g_silence_mu);
    return silenced;
}

JsonArray *
pcv_alert_get_silences(void)
{
    _ensure_silence_init();
    JsonArray *arr = json_array_new();
    gint64 now = g_get_monotonic_time();

    g_mutex_lock(&g_silence_mu);
    for (guint i = 0; i < g_silences->len; i++) {
        AlertSilence *s = g_ptr_array_index(g_silences, i);
        if (now < s->until) {
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "metric", s->metric);
            json_object_set_int_member(obj, "remaining_sec",
                (gint64)((s->until - now) / G_USEC_PER_SEC));
            json_object_set_string_member(obj, "reason", s->reason);
            json_array_add_object_element(arr, obj);
        }
    }
    g_mutex_unlock(&g_silence_mu);
    return arr;
}

/* PCV_DEAD_EXPORT_OK: 테스트 격리 전용 헬퍼 — 유일한 소비자는 test_runner
 * (tests/test_alert_silence.c). dead-export 게이트는 src 하위 .c 만 스캔하므로
 * 프로덕션 배선이 없는 이 심볼을 dead 로 보지만, 의도된 test-only export 다. */
void
pcv_alert_silence_reset(void)
{
    _ensure_silence_init();
    g_mutex_lock(&g_silence_mu);
    g_ptr_array_set_size(g_silences, 0);
    g_mutex_unlock(&g_silence_mu);
}
