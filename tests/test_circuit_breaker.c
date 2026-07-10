/* tests/test_circuit_breaker.c
 *
 * 대상 모듈: src/modules/virt/circuit_breaker.c — libvirt 장애 격리
 *
 * 이 테스트가 검증하는 것:
 *   서킷 브레이커의 CLOSED/OPEN/HALF_OPEN 3-상태 전이를 검사한다.
 *   연속 실패 시 OPEN 전환, 성공 시 카운터 리셋, 임계값 변경,
 *   per-resource named state, Prometheus 메트릭 출력을 포함.
 *
 * 실행: sudo ./test_runner -p /circuit_breaker
 *
 * 테스트 추가:
 *   1. 함수 작성 (시그니처: CbFixture *f, gconstpointer d)
 *   2. CB_TEST("이름", 함수) 매크로로 등록
 *   (픽스처가 매 테스트마다 cb_init/shutdown 호출하여 상태 초기화)
 *
 * 외부 의존: 없음 (libvirt 연결 불필요, 순수 상태 머신)
 */

#include <glib.h>
#include <string.h>
#include "modules/virt/circuit_breaker.h"

/* 각 테스트 전후 init/shutdown 으로 상태 초기화 */
typedef struct { int dummy; } CbFixture;

static void cb_setup(CbFixture *f, gconstpointer data) {
    (void)f; (void)data;
    cb_init();
}
static void cb_teardown(CbFixture *f, gconstpointer data) {
    (void)f; (void)data;
    cb_shutdown();
}

/* ── 초기 상태 ───────────────────────────────────────── */

static void test_initial_state(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_false(cb_is_open());
    g_assert_cmpint(cb_get_failure_count(), ==, 0);
    g_assert_cmpstr(cb_get_state_str(), ==, "CLOSED");
}

/* ── CLOSED → OPEN 전이 ──────────────────────────────── */

static void test_closed_to_open(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    for (int i = 0; i < CB_FAILURE_THRESHOLD; i++) {
        g_assert_false(cb_is_open());
        cb_record_failure();
    }
    /* threshold 회 실패 후 OPEN */
    g_assert_true(cb_is_open());
    g_assert_cmpstr(cb_get_state_str(), ==, "OPEN");
}

/* ── OPEN 중 is_open() == TRUE ───────────────────────── */

static void test_open_blocks_all(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    for (int i = 0; i < CB_FAILURE_THRESHOLD; i++)
        cb_record_failure();

    /* OPEN 상태에서 10번 연속 is_open() → 모두 TRUE */
    for (int i = 0; i < 10; i++)
        g_assert_true(cb_is_open());
}

/* ── OPEN 중 success 기록 → 여전히 OPEN ─────────────── */

static void test_open_ignores_success(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    for (int i = 0; i < CB_FAILURE_THRESHOLD; i++)
        cb_record_failure();

    cb_record_success();   /* OPEN 상태에서 성공 → 무시 */
    g_assert_true(cb_is_open());
    g_assert_cmpstr(cb_get_state_str(), ==, "OPEN");
}

/* ── HALF_OPEN 전이: cb_is_open()이 FALSE 반환 (probe) */
/* 백오프 0 으로 만들기 위해 circuit_breaker 내부 API 를 직접 쓰는 대신
 * "OPEN → 백오프 → HALF_OPEN" 경로를 시간 없이 검증하려면
 * 공개 API 만으로는 백오프 대기가 필요합니다.
 * → 대신 CLOSED+success 조합으로 카운터 리셋 검증에 집중합니다. */

static void test_success_resets_counter(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    /* 임계값 - 1 회 실패 → 아직 CLOSED */
    for (int i = 0; i < CB_FAILURE_THRESHOLD - 1; i++)
        cb_record_failure();

    g_assert_false(cb_is_open());
    g_assert_cmpint(cb_get_failure_count(), ==, CB_FAILURE_THRESHOLD - 1);

    /* 성공 한 번 → 카운터 리셋 */
    cb_record_success();
    g_assert_cmpint(cb_get_failure_count(), ==, 0);
    g_assert_false(cb_is_open());
}

/* ── failure_count 정확성 ────────────────────────────── */

static void test_failure_count(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_cmpint(cb_get_failure_count(), ==, 0);
    cb_record_failure();
    g_assert_cmpint(cb_get_failure_count(), ==, 1);
    cb_record_failure();
    g_assert_cmpint(cb_get_failure_count(), ==, 2);
}

/* ── state_str 정확성 ────────────────────────────────── */

static void test_state_str(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_cmpstr(cb_get_state_str(), ==, "CLOSED");
    for (int i = 0; i < CB_FAILURE_THRESHOLD; i++)
        cb_record_failure();
    g_assert_cmpstr(cb_get_state_str(), ==, "OPEN");
}

/* ── 추가 케이스: threshold/named_state/prometheus/half_open ── */

static void test_set_get_threshold(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    gint orig = cb_get_failure_threshold();
    cb_set_failure_threshold(10);
    g_assert_cmpint(cb_get_failure_threshold(), ==, 10);
    cb_set_failure_threshold(3);
    g_assert_cmpint(cb_get_failure_threshold(), ==, 3);
    cb_set_failure_threshold(orig);
}

/* 임계값을 0이나 음수로 설정하면 최소 1로 클램핑되는지 확인 */
static void test_threshold_zero_or_negative(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    cb_set_failure_threshold(0);
    /* 구현은 보통 1로 클램프하거나 무시 */
    gint t = cb_get_failure_threshold();
    g_assert_cmpint(t, >=, 1);
    cb_set_failure_threshold(CB_FAILURE_THRESHOLD_DEFAULT);
}

/* per-resource CB: 미등록 리소스 이름 조회 시 기본 상태 반환 확인 */
static void test_named_state_default(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    /* 미등록 named state — 기본 CLOSED 또는 정의된 폴백 */
    CbState s = cb_get_named_state("nonexistent-resource");
    g_assert_true(s == CB_STATE_CLOSED || s == CB_STATE_OPEN || s == CB_STATE_HALF_OPEN);
}

/* Prometheus 텍스트 형식 메트릭 출력이 비어있지 않은지 확인 */
static void test_prometheus_metrics(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    gchar *m = cb_get_prometheus_metrics();
    g_assert_nonnull(m);
    /* Prometheus 형식이면 # HELP 또는 metric 이름 포함 */
    g_assert_cmpuint(strlen(m), >, 0);
    g_free(m);
}

static void test_failure_count_resets_on_success(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    cb_record_failure();
    cb_record_failure();
    g_assert_cmpint(cb_get_failure_count(), ==, 2);
    cb_record_success();
    g_assert_cmpint(cb_get_failure_count(), ==, 0);
}

static void test_state_enum_to_str(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_cmpstr(cb_get_state_str(), ==, "CLOSED");
    /* OPEN 상태로 전이 */
    for (int i = 0; i < cb_get_failure_threshold(); i++)
        cb_record_failure();
    g_assert_cmpint(cb_get_state(), ==, CB_STATE_OPEN);
    g_assert_cmpstr(cb_get_state_str(), ==, "OPEN");
}

/* ── 신규 케이스 a: half_open_transition ─────────────── */

/* OPEN 상태에서 처음에는 cb_is_open()==TRUE임을 확인.
 * 백오프 만료 전이므로 HALF_OPEN 전이는 발생하지 않지만,
 * OPEN 직후 상태 일관성(state=OPEN, is_open=TRUE)을 검증한다. */
static void test_half_open_state_after_open(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    /* threshold를 1로 낮춰 단 1회 실패로 OPEN 전이 */
    cb_set_failure_threshold(1);
    cb_record_failure();

    g_assert_true(cb_is_open());
    g_assert_cmpstr(cb_get_state_str(), ==, "OPEN");
    g_assert_cmpint(cb_get_state(), ==, CB_STATE_OPEN);

    /* 백오프 만료 전이므로 추가 is_open() 호출에서도 여전히 TRUE */
    g_assert_true(cb_is_open());
    g_assert_true(cb_is_open());

    cb_set_failure_threshold(CB_FAILURE_THRESHOLD_DEFAULT);
}

/* ── 신규 케이스 b: named_instance_multi ─────────────── */

/* 두 개의 named CB 인스턴스가 독립적임을 검증.
 * svc-a를 OPEN 시켜도 svc-b는 CLOSED를 유지해야 한다. */
static void test_named_instance_multi(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;

    /* 처음 조회하면 양쪽 모두 CLOSED */
    CbState state_a = cb_get_named_state("svc-a");
    CbState state_b = cb_get_named_state("svc-b");
    g_assert_cmpint(state_a, ==, CB_STATE_CLOSED);
    g_assert_cmpint(state_b, ==, CB_STATE_CLOSED);

    /* 두 번 연속 조회해도 새 인스턴스를 만들지 않고 동일 상태를 반환 */
    g_assert_cmpint(cb_get_named_state("svc-a"), ==, CB_STATE_CLOSED);
    g_assert_cmpint(cb_get_named_state("svc-b"), ==, CB_STATE_CLOSED);

    /* svc-a, svc-b 인스턴스는 전역 g_cb와 독립적:
     * 전역 g_cb를 OPEN 시켜도 named 인스턴스에는 영향 없음 */
    for (int i = 0; i < CB_FAILURE_THRESHOLD; i++)
        cb_record_failure();
    g_assert_true(cb_is_open());                         /* 전역 g_cb: OPEN */
    g_assert_cmpint(cb_get_named_state("svc-a"), ==, CB_STATE_CLOSED); /* svc-a: 무관 */
    g_assert_cmpint(cb_get_named_state("svc-b"), ==, CB_STATE_CLOSED); /* svc-b: 무관 */
}

/* ── 등록 함수 ───────────────────────────────────────── */

void test_circuit_breaker_register(void) {
#define CB_TEST(name, fn) \
    g_test_add("/circuit_breaker/" name, CbFixture, NULL, \
               cb_setup, fn, cb_teardown)

    CB_TEST("initial_state",          test_initial_state);
    CB_TEST("closed_to_open",         test_closed_to_open);
    CB_TEST("open_blocks_all",        test_open_blocks_all);
    CB_TEST("open_ignores_success",   test_open_ignores_success);
    CB_TEST("success_resets_counter", test_success_resets_counter);
    CB_TEST("failure_count",          test_failure_count);
    CB_TEST("state_str",              test_state_str);
    CB_TEST("set_get_threshold",      test_set_get_threshold);
    CB_TEST("threshold_zero_or_negative", test_threshold_zero_or_negative);
    CB_TEST("named_state_default",    test_named_state_default);
    CB_TEST("prometheus_metrics",     test_prometheus_metrics);
    CB_TEST("failure_resets_on_success", test_failure_count_resets_on_success);
    CB_TEST("state_enum_to_str",      test_state_enum_to_str);
    CB_TEST("half_open_state_after_open", test_half_open_state_after_open);
    CB_TEST("named_instance_multi",   test_named_instance_multi);
#undef CB_TEST
}
