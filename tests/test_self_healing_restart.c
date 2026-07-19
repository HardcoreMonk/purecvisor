
#include <glib.h>
#include "modules/ai/self_healing_restart.h"

static int spy_calls;
static int spy_ret;

static int
spy_create(gpointer dom)
{
    (void)dom;
    spy_calls++;
    return spy_ret;
}

static void
spy_reset(int ret)
{
    spy_calls = 0;
    spy_ret = ret;
}

static void
test_running_guard_skip(void)
{
    spy_reset(0);
    gint rb_feedback = -99;
    const gchar *result = pcv_healing_restart_decide(1, spy_create, NULL, &rb_feedback);

    g_assert_cmpstr(result, ==, "skipped");
    g_assert_cmpint(spy_calls, ==, 0);
    g_assert_cmpint(rb_feedback, ==, +1);
}

static void
test_stopped_create_success(void)
{
    spy_reset(0);
    gint rb_feedback = -99;
    const gchar *result = pcv_healing_restart_decide(0, spy_create, NULL, &rb_feedback);

    g_assert_cmpstr(result, ==, "success");
    g_assert_cmpint(spy_calls, ==, 1);
    g_assert_cmpint(rb_feedback, ==, +1);
}

static void
test_stopped_create_failure(void)
{
    spy_reset(-1);
    gint rb_feedback = -99;
    const gchar *result = pcv_healing_restart_decide(0, spy_create, NULL, &rb_feedback);

    g_assert_cmpstr(result, ==, "failed");
    g_assert_cmpint(spy_calls, ==, 1);
    g_assert_cmpint(rb_feedback, ==, -1);
}

void
test_self_healing_restart_register(void)
{
    g_test_add_func("/self_healing_restart/running_guard_skip", test_running_guard_skip);
    g_test_add_func("/self_healing_restart/stopped_create_success", test_stopped_create_success);
    g_test_add_func("/self_healing_restart/stopped_create_failure", test_stopped_create_failure);
}
