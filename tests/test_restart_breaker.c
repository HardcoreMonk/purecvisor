
#include <glib.h>
#include "modules/ai/restart_breaker.h"
#include "modules/virt/circuit_breaker.h"

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

static void drive_to_open(const gchar *uuid, gint threshold) {
    for (gint i = 0; i < threshold; i++) {
        g_assert_true(rb_allow(uuid));
        rb_record(uuid, FALSE);
    }
    g_assert_cmpint(rb_state(uuid), ==, CB_STATE_OPEN);
}

static void test_initial_state(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 0);
    g_assert_true(rb_allow("vm1"));
}

static void test_closed_to_open(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(3, 3600);
    for (int i = 0; i < 3; i++) {
        g_assert_true(rb_allow("vm1"));
        g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);
        rb_record("vm1", FALSE);
    }
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_OPEN);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 3);

    g_assert_false(rb_allow("vm1"));
}

static void test_success_resets_counter(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(3, 3600);
    rb_record("vm1", FALSE);
    rb_record("vm1", FALSE);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 2);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);

    rb_record("vm1", TRUE);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 0);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);
    g_assert_true(rb_allow("vm1"));
}

static void test_half_open_probe_close(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    drive_to_open("vm1", 3);

    g_assert_true(rb_allow("vm1"));
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_HALF_OPEN);

    rb_record("vm1", TRUE);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 0);
    g_assert_true(rb_allow("vm1"));
}

static void test_half_open_probe_reopen(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    drive_to_open("vm1", 3);
    g_assert_true(rb_allow("vm1"));
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_HALF_OPEN);
    rb_record("vm1", FALSE);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_OPEN);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 4);

    g_assert_true(rb_allow("vm1"));
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_HALF_OPEN);
}

static void test_probe_in_flight_blocks_second(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    drive_to_open("vm1", 3);
    g_assert_true(rb_allow("vm1"));
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_HALF_OPEN);

    g_assert_false(rb_allow("vm1"));
}

static void test_cooldown_blocks(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(3, 3600);
    drive_to_open("vm1", 3);
    g_assert_false(rb_allow("vm1"));
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_OPEN);
}

static void test_per_vm_isolation(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(3, 3600);
    drive_to_open("vm-a", 3);
    g_assert_cmpint(rb_state("vm-a"), ==, CB_STATE_OPEN);
    g_assert_cmpint(rb_state("vm-b"), ==, CB_STATE_CLOSED);
    g_assert_true(rb_allow("vm-b"));
    g_assert_false(rb_allow("vm-a"));
}

static void test_threshold_config(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(1, 3600);
    g_assert_cmpint(rb_get_threshold(), ==, 1);
    g_assert_true(rb_allow("vm1"));
    rb_record("vm1", FALSE);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_OPEN);
}

static void test_config_clamp(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(0, -5);
    g_assert_cmpint(rb_get_threshold(), ==, 1);
    g_assert_cmpint(rb_get_cooldown_sec(), ==, 0);
    rb_configure(999, 900);
    g_assert_cmpint(rb_get_threshold(), ==, 50);
    g_assert_cmpint(rb_get_cooldown_sec(), ==, 900);
}

static void test_null_uuid_safe(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_true(rb_allow(NULL));
    g_assert_true(rb_allow(""));
    rb_record(NULL, FALSE);
    rb_record("", TRUE);
    g_assert_cmpint(rb_state(NULL), ==, CB_STATE_CLOSED);
    g_assert_cmpint(rb_failure_count(NULL), ==, 0);
}

static void test_running_guard_skip_closes(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    drive_to_open("vm1", 3);
    g_assert_true(rb_allow("vm1"));

    rb_record("vm1", TRUE);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_CLOSED);
    g_assert_cmpint(rb_failure_count("vm1"), ==, 0);
}

static void test_probe_release_no_feedback(RbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    rb_configure(1, 0);
    rb_record("vm1", FALSE);
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_OPEN);

    g_assert_true(rb_allow("vm1"));
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_HALF_OPEN);
    g_assert_false(rb_allow("vm1"));

    rb_release_probe("vm1");

    g_assert_true(rb_allow("vm1"));
    g_assert_cmpint(rb_state("vm1"), ==, CB_STATE_HALF_OPEN);
}

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
    RB_TEST("probe_release_no_feedback", test_probe_release_no_feedback);
#undef RB_TEST
}
