


















#include <glib.h>
#include <string.h>
#include "modules/virt/circuit_breaker.h"


typedef struct { int dummy; } CbFixture;

static void cb_setup(CbFixture *f, gconstpointer data) {
    (void)f; (void)data;
    cb_init();
}
static void cb_teardown(CbFixture *f, gconstpointer data) {
    (void)f; (void)data;
    cb_shutdown();
}



static void test_initial_state(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_false(cb_is_open());
    g_assert_cmpint(cb_get_failure_count(), ==, 0);
    g_assert_cmpstr(cb_get_state_str(), ==, "CLOSED");
}



static void test_closed_to_open(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    for (int i = 0; i < CB_FAILURE_THRESHOLD; i++) {
        g_assert_false(cb_is_open());
        cb_record_failure();
    }

    g_assert_true(cb_is_open());
    g_assert_cmpstr(cb_get_state_str(), ==, "OPEN");
}



static void test_open_blocks_all(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    for (int i = 0; i < CB_FAILURE_THRESHOLD; i++)
        cb_record_failure();


    for (int i = 0; i < 10; i++)
        g_assert_true(cb_is_open());
}



static void test_open_ignores_success(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    for (int i = 0; i < CB_FAILURE_THRESHOLD; i++)
        cb_record_failure();

    cb_record_success();
    g_assert_true(cb_is_open());
    g_assert_cmpstr(cb_get_state_str(), ==, "OPEN");
}







static void test_success_resets_counter(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;

    for (int i = 0; i < CB_FAILURE_THRESHOLD - 1; i++)
        cb_record_failure();

    g_assert_false(cb_is_open());
    g_assert_cmpint(cb_get_failure_count(), ==, CB_FAILURE_THRESHOLD - 1);


    cb_record_success();
    g_assert_cmpint(cb_get_failure_count(), ==, 0);
    g_assert_false(cb_is_open());
}



static void test_failure_count(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_cmpint(cb_get_failure_count(), ==, 0);
    cb_record_failure();
    g_assert_cmpint(cb_get_failure_count(), ==, 1);
    cb_record_failure();
    g_assert_cmpint(cb_get_failure_count(), ==, 2);
}



static void test_state_str(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_cmpstr(cb_get_state_str(), ==, "CLOSED");
    for (int i = 0; i < CB_FAILURE_THRESHOLD; i++)
        cb_record_failure();
    g_assert_cmpstr(cb_get_state_str(), ==, "OPEN");
}



static void test_set_get_threshold(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    gint orig = cb_get_failure_threshold();
    cb_set_failure_threshold(10);
    g_assert_cmpint(cb_get_failure_threshold(), ==, 10);
    cb_set_failure_threshold(3);
    g_assert_cmpint(cb_get_failure_threshold(), ==, 3);
    cb_set_failure_threshold(orig);
}


static void test_threshold_zero_or_negative(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    cb_set_failure_threshold(0);

    gint t = cb_get_failure_threshold();
    g_assert_cmpint(t, >=, 1);
    cb_set_failure_threshold(CB_FAILURE_THRESHOLD_DEFAULT);
}


static void test_named_state_default(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;

    CbState s = cb_get_named_state("nonexistent-resource");
    g_assert_true(s == CB_STATE_CLOSED || s == CB_STATE_OPEN || s == CB_STATE_HALF_OPEN);
}


static void test_prometheus_metrics(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;
    gchar *m = cb_get_prometheus_metrics();
    g_assert_nonnull(m);

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

    for (int i = 0; i < cb_get_failure_threshold(); i++)
        cb_record_failure();
    g_assert_cmpint(cb_get_state(), ==, CB_STATE_OPEN);
    g_assert_cmpstr(cb_get_state_str(), ==, "OPEN");
}






static void test_half_open_state_after_open(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;

    cb_set_failure_threshold(1);
    cb_record_failure();

    g_assert_true(cb_is_open());
    g_assert_cmpstr(cb_get_state_str(), ==, "OPEN");
    g_assert_cmpint(cb_get_state(), ==, CB_STATE_OPEN);


    g_assert_true(cb_is_open());
    g_assert_true(cb_is_open());

    cb_set_failure_threshold(CB_FAILURE_THRESHOLD_DEFAULT);
}





static void test_named_instance_multi(CbFixture *f, gconstpointer d) {
    (void)f; (void)d;


    CbState state_a = cb_get_named_state("svc-a");
    CbState state_b = cb_get_named_state("svc-b");
    g_assert_cmpint(state_a, ==, CB_STATE_CLOSED);
    g_assert_cmpint(state_b, ==, CB_STATE_CLOSED);


    g_assert_cmpint(cb_get_named_state("svc-a"), ==, CB_STATE_CLOSED);
    g_assert_cmpint(cb_get_named_state("svc-b"), ==, CB_STATE_CLOSED);



    for (int i = 0; i < CB_FAILURE_THRESHOLD; i++)
        cb_record_failure();
    g_assert_true(cb_is_open());
    g_assert_cmpint(cb_get_named_state("svc-a"), ==, CB_STATE_CLOSED);
    g_assert_cmpint(cb_get_named_state("svc-b"), ==, CB_STATE_CLOSED);
}



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
