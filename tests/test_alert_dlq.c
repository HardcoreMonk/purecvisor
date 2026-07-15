/* tests/test_alert_dlq.c
 *
 * 대상 모듈: src/modules/daemons/alert_dlq.c — AIO-4
 *            Webhook DLQ 스토어 (alert_engine.c 에서 추출).
 *
 * 이 테스트가 검증하는 것 (락 밖 HTTP 재시도의 핵심 프리미티브):
 *   pcv_alert_dlq_retry 는 (1) 락 하 snapshot deep-copy → (2) 락 밖 HTTP →
 *   (3) 재락 후 성공 항목 **값매칭** 제거로 동작한다. 락을 놓은 사이 DLQ 배열이
 *   변동(dlq_store 로 append)할 수 있으므로, 제거는 스냅샷 시점 인덱스가 아니라
 *   **값**으로 이뤄져야 올바른 항목을 지운다.
 *
 * 검증 프리미티브 2개:
 *   - pcv_alert_dlq_snapshot()          : 락 하 deep copy (retry 1단계)
 *   - pcv_alert_dlq_remove_matching(V)  : 값매칭 제거 (retry 3단계)
 *
 * 반사실 (load-bearing):
 *   remove_matching 을 값매칭 대신 위치 인덱스 기반 제거로 되돌리면
 *   (예: g_ptr_array_remove_index(dlq, values_위치)), remove_matching({E2}) 가
 *   스토어 [E1,E2,E3,E4] 에서 E2 대신 인덱스 0(E1)을 지워 스토어=={E2,E3,E4} →
 *   store-contents 단언 RED. 즉 제거가 값 기준이어야 함이 load-bearing.
 *
 * 실행: ./test_runner -p /alert_dlq
 * 외부 의존: 없음 (glib + json-glib, 순수 로직 — HTTP seam 미사용).
 */

#include <glib.h>
#include "modules/daemons/alert_dlq.h"

/* store 가 "url|payload" 로 저장하므로 항목 원문은 "url|payload" 문자열이다. */
static gchar *entry_of(const gchar *url, const gchar *payload) {
    return g_strdup_printf("%s|%s", url, payload);
}

/* GPtrArray 의 i 번째 항목 == 기대 문자열 단언 헬퍼. */
static void assert_at(GPtrArray *a, guint i, const gchar *want) {
    g_assert_cmpuint(i, <, a->len);
    g_assert_cmpstr((const gchar *)g_ptr_array_index(a, i), ==, want);
}

/* AIO-4: 스냅샷 후 배열 변동(E4 append) 하에서도 값매칭으로 올바른 항목이
 * 제거되는가 + 스냅샷은 deep copy 라 원본 변동/해제에 무관한가. */
static void test_dlq_remove_by_value_survives_mutation(void) {
    pcv_alert_dlq_reset();

    gchar *e1 = entry_of("http://a", "1");
    gchar *e2 = entry_of("http://b", "2");
    gchar *e3 = entry_of("http://c", "3");
    gchar *e4 = entry_of("http://d", "4");

    /* 스토어 [E1,E2,E3] */
    pcv_alert_dlq_store("http://a", "1");
    pcv_alert_dlq_store("http://b", "2");
    pcv_alert_dlq_store("http://c", "3");

    /* retry 1단계: 락 하 deep-copy 스냅샷 (이 시점 [E1,E2,E3]) */
    GPtrArray *snap = pcv_alert_dlq_snapshot();
    g_assert_cmpuint(snap->len, ==, 3);

    /* 락 밖 HTTP 구간을 모사한 배열 변동: E4 append → 스토어 [E1,E2,E3,E4] */
    pcv_alert_dlq_store("http://d", "4");

    /* retry 3단계: 성공 원문 {E2} 값매칭 제거. 스토어 [E1,E2,E3,E4] 에서
     * E2 를 값으로 찾아 제거해야 한다 (위치 인덱스 아님). */
    GPtrArray *ok = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ok, g_strdup(e2));
    pcv_alert_dlq_remove_matching(ok);
    g_ptr_array_unref(ok);

    /* 단언 1 — 스토어 == [E1,E3,E4] (E2 가 값으로 제거됨).
     * 반사실(위치 인덱스 제거)이면 E1 이 지워져 [E2,E3,E4] → RED. */
    GPtrArray *now = pcv_alert_dlq_snapshot();
    g_assert_cmpuint(now->len, ==, 3);
    assert_at(now, 0, e1);
    assert_at(now, 1, e3);
    assert_at(now, 2, e4);
    g_ptr_array_unref(now);

    /* 단언 2 — 스냅샷은 deep copy: E4 append 에 len 불변(3),
     * 그리고 스토어가 자신의 E2 복사본을 해제한 뒤에도 snap[1] 은 여전히 "E2"
     * (별도 g_strdup 복사본 — 얕은 별칭이면 UAF/불일치). */
    g_assert_cmpuint(snap->len, ==, 3);
    assert_at(snap, 0, e1);
    assert_at(snap, 1, e2);
    assert_at(snap, 2, e3);
    g_ptr_array_unref(snap);

    g_free(e1); g_free(e2); g_free(e3); g_free(e4);
    pcv_alert_dlq_reset();
}

void test_alert_dlq_register(void) {
    g_test_add_func("/alert_dlq/remove_by_value_survives_mutation",
                    test_dlq_remove_by_value_survives_mutation);
}
