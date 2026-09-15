










#include <glib.h>
#include <libvirt/libvirt.h>

#include "modules/daemons/pcv_virt_listener_guard.h"

static void
test_liveness_treats_dead_error_and_missing_as_lost(void)
{
    g_assert_false(pcv_virt_listener_connection_lost(TRUE, 1, 0));
    g_assert_true(pcv_virt_listener_connection_lost(TRUE, 1, -1));
    g_assert_true(pcv_virt_listener_connection_lost(TRUE, 0, 0));
    g_assert_true(pcv_virt_listener_connection_lost(TRUE, -1, 0));
    g_assert_true(pcv_virt_listener_connection_lost(FALSE, 1, 0));
}

static void
test_watchdog_wait_is_bounded_and_context_independent(void)
{
    gint64 started_at = g_get_monotonic_time();
    g_assert_true(pcv_virt_listener_guard_wait(10U));
    gint64 elapsed = g_get_monotonic_time() - started_at;
    g_assert_cmpint(elapsed, >=, 5 * G_TIME_SPAN_MILLISECOND);
    g_assert_cmpint(elapsed, <, G_TIME_SPAN_SECOND);
}

static void
test_invalid_interval_fails_without_registration(void)
{
    g_assert_false(pcv_virt_listener_guard_wait(0U));
}

void
test_virt_listener_guard_register(void)
{
    g_test_add_func("/virt_listener_guard/liveness/error_is_lost",
                    test_liveness_treats_dead_error_and_missing_as_lost);
    g_test_add_func("/virt_listener_guard/watchdog/independent_wait",
                    test_watchdog_wait_is_bounded_and_context_independent);
    g_test_add_func("/virt_listener_guard/watchdog/invalid_interval",
                    test_invalid_interval_fails_without_registration);
}
