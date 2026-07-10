/* tests/test_restart_breaker.c
 *
 * 대상 모듈: src/modules/ai/restart_breaker.c — AF-1 후속
 *            self-healing VM 재시작 서킷 브레이커 (VM(uuid) 단위)
 *
 * 이 테스트가 검증하는 것:
 *   VM 별 CLOSED/OPEN/HALF_OPEN 상태 전이 —
 *   연속 실패 → OPEN, cooldown 경과 → HALF_OPEN 프로브 1회,
 *   프로브 성공 → CLOSED, 프로브 실패 → 재-OPEN,
 *   성공/running-guard skip → 카운터 리셋, VM 간 격리, 설정/클램핑.
 *
 * 결정론:
 *   cooldown 을 0 으로 설정하면 OPEN 직후 즉시 HALF_OPEN 프로브가 허용되므로
 *   실시간 sleep 없이 전체 사이클(open→half-open→close/reopen)을 검증한다.
 *   cooldown>0 케이스는 "아직 차단 중" 경로만 검증한다.
 *
 * 실행: sudo ./test_runner -p /restart_breaker
 * 외부 의존: 없음 (libvirt 불필요, 순수 상태 머신)
 */

#include <glib.h>
#include "modules/ai/restart_breaker.h"
#include "modules/virt/circuit_breaker.h"  /* CbState enum */

/* 각 테스트 전후 init/shutdown 으로 상태 테이블 초기화.
 * 기본 설정: 임계값 3, cooldown 0 (half-open 즉시 허용). */
typedef struct { int dummy; } RbFixture;

static void rb_setup(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_init();
    rb_configure(3, 0);
}
static void rb_teardown(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_shutdown();
}

/* 헬퍼: uuid 를 CLOSED 에서 threshold 회 실패시켜 OPEN 으로 만든다. */
static void drive_to_open(const gchar *uuid, gint threshold) {
    for (gint i = 0; i < threshold; i++) {
        g_assert_true(rb_allow(uuid));    /* CLOSED 는 통과 */
        rb_record(uuid, FALSE);
    }
    g_assert_cmpint(rb_state(uuid), ==, CB_STATE_OPEN);
}

/* ── 초기 상태 ───────────────────────────────────────── */
static void test_initial_state(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 0);
    g_assert_true(rb_allow("vm1"));
}

/* ── CLOSED → OPEN (연속 실패 임계값 도달) ────────────── */
static void test_closed_to_open(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(3, 3600);   /* 긴 cooldown → OPEN 유지 */
    for (int i = 0; i < 3; i++) {
        g_assert_true(rb_allow("vm1"));
        g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);  /* 3회째 record 전까지 CLOSED */
        rb_record("vm1", FALSE);
    }
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_OPEN);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 3);
    /* OPEN + cooldown 미경과 → 차단 */
    g_assert_false(rb_allow("vm1"));
}

/* ── 성공 시 실패 카운터 리셋 (CLOSED 유지) ───────────── */
static void test_success_resets_counter(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(3, 3600);
    rb_record("vm1", FALSE);
    rb_record("vm1", FALSE);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 2);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);
    /* 성공 → 카운터 0 리셋 */
    rb_record("vm1", TRUE);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 0);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);
    g_assert_true(rb_allow("vm1"));
}

/* ── OPEN → HALF_OPEN 프로브 → 성공 → CLOSED ──────────── */
static void test_half_open_probe_close(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    drive_to_open("vm1", 3);            /* cooldown 0 → 즉시 half-open 가능 */
    /* cooldown(0) 경과 → 첫 allow 가 HALF_OPEN 프로브 허용 */
    g_assert_true(rb_allow("vm1"));
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_HALF_OPEN);
    /* 프로브 성공 → CLOSED 복귀, 카운터 0 */
    rb_record("vm1", TRUE);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 0);
    g_assert_true(rb_allow("vm1"));
}

/* ── OPEN → HALF_OPEN 프로브 → 실패 → 재-OPEN ─────────── */
static void test_half_open_probe_reopen(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    drive_to_open("vm1", 3);
    g_assert_true(rb_allow("vm1"));                       /* HALF_OPEN 프로브 */
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_HALF_OPEN);
    rb_record("vm1", FALSE);                             /* 프로브 실패 → 재-OPEN */
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_OPEN);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 4);
    /* cooldown 0 → 재차 프로브 허용 */
    g_assert_true(rb_allow("vm1"));
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_HALF_OPEN);
}

/* ── HALF_OPEN 중 두 번째 allow 는 차단 (단일 프로브) ──── */
static void test_probe_in_flight_blocks_second(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    drive_to_open("vm1", 3);
    g_assert_true(rb_allow("vm1"));       /* 프로브 1 허용 → probe_in_flight */
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_HALF_OPEN);
    /* 결과 record 전 두 번째 allow → 차단 */
    g_assert_false(rb_allow("vm1"));
}

/* ── cooldown 미경과 시 OPEN 차단 유지 (half-open 아님) ── */
static void test_cooldown_blocks(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(3, 3600);
    drive_to_open("vm1", 3);
    g_assert_false(rb_allow("vm1"));                     /* 아직 차단 */
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_OPEN); /* half-open 전이 없음 */
}

/* ── VM 간 격리: vm-a OPEN 이어도 vm-b 는 CLOSED ──────── */
static void test_per_vm_isolation(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(3, 3600);
    drive_to_open("vm-a", 3);
    g_assert_cmpint(rb_state("vm-a"), ==, CB_STATE_OPEN);
    g_assert_cmpint(rb_state("vm-b"), ==, CB_STATE_CLOSED);
    g_assert_true(rb_allow("vm-b"));
    g_assert_false(rb_allow("vm-a"));
}

/* ── 임계값 설정 반영 (threshold=1 → 1회 실패로 OPEN) ─── */
static void test_threshold_config(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(1, 3600);
    g_assert_cmpint(rb_get_threshold(), ==, 1);
    g_assert_true(rb_allow("vm1"));
    rb_record("vm1", FALSE);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_OPEN);
}

/* ── 설정 클램핑 (threshold<1 → 1, cooldown<0 → 0) ────── */
static void test_config_clamp(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(0, -5);
    g_assert_cmpint(rb_get_threshold(), ==, 1);
    g_assert_cmpint(rb_get_cooldown_sec(), ==, 0);
    rb_configure(999, 900);
    g_assert_cmpint(rb_get_threshold(), ==, 50);   /* 상한 50 */
    g_assert_cmpint(rb_get_cooldown_sec(), ==, 900);
}

/* ── NULL/빈 uuid 안전 (브레이커 미적용) ──────────────── */
static void test_null_uuid_safe(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_true(rb_allow(NULL));
    g_assert_true(rb_allow(""));
    rb_record(NULL, FALSE);    /* 크래시 없어야 함 */
    rb_record("", TRUE);
    g_assert_cmpint(rb_state(NULL), ==, CB_STATE_CLOSED);
    g_assert_cmpint(rb_failure_count(NULL), ==, 0);
}

/* ── running-guard skip(성공 취급) 이 OPEN 을 닫는지 ──── */
static void test_running_guard_skip_closes(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    drive_to_open("vm1", 3);
    g_assert_true(rb_allow("vm1"));                       /* HALF_OPEN 프로브 */
    /* 워커에서 running-guard skip → rb_record(uuid, TRUE) 와 동일 경로 */
    rb_record("vm1", TRUE);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 0);
}

/* ── 등록 함수 ───────────────────────────────────────── */
void test_restart_breaker_register(void) {
#define RB_TEST(name, fn) \
    g_test_add("/restart_breaker/" name, RbFixture, NULL, \
               rb_setup, fn, rb_teardown)

    RB_TEST("initial_state",             test_initial_state);
    RB_TEST("closed_to_open",            test_closed_to_open);
    RB_TEST("success_resets_counter",    test_success_resets_counter);
    RB_TEST("half_open_probe_close",     test_half_open_probe_close);
    RB_TEST("half_open_probe_reopen",    test_half_open_probe_reopen);
    RB_TEST("probe_in_flight_blocks",    test_probe_in_flight_blocks_second);
    RB_TEST("cooldown_blocks",           test_cooldown_blocks);
    RB_TEST("per_vm_isolation",          test_per_vm_isolation);
    RB_TEST("threshold_config",          test_threshold_config);
    RB_TEST("config_clamp",              test_config_clamp);
    RB_TEST("null_uuid_safe",            test_null_uuid_safe);
    RB_TEST("running_guard_skip_closes", test_running_guard_skip_closes);
#undef RB_TEST
}
