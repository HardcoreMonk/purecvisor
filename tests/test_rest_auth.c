#include <gio/gio.h>
#include <glib.h>

#include "../src/api/rest_auth.h"





static void
test_bootstrap_fallback_no_error(void)
{
    g_assert_true(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                          "purecvisor",
                                                          "admin",
                                                          "purecvisor",
                                                          NULL));
}

static void
test_bootstrap_fallback_invalid_credentials(void)
{
    GError *err = g_error_new(G_IO_ERROR,
                              G_IO_ERROR_PERMISSION_DENIED,
                              "Invalid credentials");

    g_assert_true(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                          "purecvisor",
                                                          "admin",
                                                          "purecvisor",
                                                          err));

    g_clear_error(&err);
}

static void
test_bootstrap_fallback_locked_denied(void)
{
    GError *err = g_error_new(G_IO_ERROR,
                              G_IO_ERROR_PERMISSION_DENIED,
                              "Account locked -- retry after 30 seconds");

    g_assert_false(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                           "purecvisor",
                                                           "admin",
                                                           "purecvisor",
                                                           err));

    g_clear_error(&err);
}

static void
test_bootstrap_fallback_db_failure_denied(void)
{
    GError *err = g_error_new(G_IO_ERROR,
                              G_IO_ERROR_FAILED,
                              "failed to prepare RBAC statement");

    g_assert_false(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                           "purecvisor",
                                                           "admin",
                                                           "purecvisor",
                                                           err));

    g_clear_error(&err);
}

static void
test_bootstrap_fallback_nonmatching_creds_denied(void)
{
    g_assert_false(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                           "wrong",
                                                           "admin",
                                                           "purecvisor",
                                                           NULL));
}

void
test_rest_auth_register(void)
{
    g_test_add_func("/rest_auth/bootstrap_fallback/no_error",
                    test_bootstrap_fallback_no_error);
    g_test_add_func("/rest_auth/bootstrap_fallback/invalid_credentials",
                    test_bootstrap_fallback_invalid_credentials);
    g_test_add_func("/rest_auth/bootstrap_fallback/locked_denied",
                    test_bootstrap_fallback_locked_denied);
    g_test_add_func("/rest_auth/bootstrap_fallback/db_failure_denied",
                    test_bootstrap_fallback_db_failure_denied);
    g_test_add_func("/rest_auth/bootstrap_fallback/nonmatching_creds_denied",
                    test_bootstrap_fallback_nonmatching_creds_denied);
}
