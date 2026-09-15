







#include <glib.h>

#include "modules/daemons/pcv_telemetry_reconnect_guard.h"


static void
test_successful_bulk_results_keep_connection(void)
{
    g_assert_false(pcv_telemetry_reconnect_required(TRUE, 5, TRUE));
    g_assert_false(pcv_telemetry_reconnect_required(TRUE, 0, TRUE));
}


static void
test_rpc_error_requires_reconnect(void)
{
    g_assert_true(pcv_telemetry_reconnect_required(TRUE, -1, TRUE));
}


static void
test_missing_connection_or_result_requires_reconnect(void)
{
    g_assert_true(pcv_telemetry_reconnect_required(FALSE, 0, TRUE));
    g_assert_true(pcv_telemetry_reconnect_required(TRUE, 0, FALSE));
}


void
test_telemetry_reconnect_guard_register(void)
{
    g_test_add_func("/telemetry_reconnect_guard/bulk/success",
                    test_successful_bulk_results_keep_connection);
    g_test_add_func("/telemetry_reconnect_guard/bulk/error",
                    test_rpc_error_requires_reconnect);
    g_test_add_func("/telemetry_reconnect_guard/bulk/missing",
                    test_missing_connection_or_result_requires_reconnect);
}
