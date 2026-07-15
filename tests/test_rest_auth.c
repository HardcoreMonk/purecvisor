#include <gio/gio.h>
#include <glib.h>

#include "../src/api/rest_auth.h"

/*
 * Bootstrap fallback is intentionally narrow: it must recover only a true
 * first-install state (user absent from RBAC DB). Once _ensure_admin_user
 * seeds the admin account and an operator rotates the password, the stale
 * daemon.conf credentials must be denied (SEC-2 regression).
 */
static void
test_fallback_user_absent_recovers(void)
{
    /* 진짜 첫설치 복구 */
    g_assert_true(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                          "purecvisor",
                                                          "admin",
                                                          "purecvisor",
                                                          FALSE));
}

static void
test_fallback_user_present_denied_sec2(void)
{
    /* 회전 후 옛 비번 거부 — SEC-2 핵심 */
    g_assert_false(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                           "purecvisor",
                                                           "admin",
                                                           "purecvisor",
                                                           TRUE));
}

static void
test_fallback_nonmatching_creds_denied(void)
{
    g_assert_false(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                           "wrong",
                                                           "admin",
                                                           "purecvisor",
                                                           FALSE));
}

static void
test_fallback_null_denied(void)
{
    g_assert_false(pcv_rest_auth_should_fallback_bootstrap(NULL,
                                                           "purecvisor",
                                                           "admin",
                                                           "purecvisor",
                                                           FALSE));
}

void
test_rest_auth_register(void)
{
    g_test_add_func("/rest_auth/bootstrap_fallback/user_absent_recovers",
                    test_fallback_user_absent_recovers);
    g_test_add_func("/rest_auth/bootstrap_fallback/user_present_denied_sec2",
                    test_fallback_user_present_denied_sec2);
    g_test_add_func("/rest_auth/bootstrap_fallback/nonmatching_creds_denied",
                    test_fallback_nonmatching_creds_denied);
    g_test_add_func("/rest_auth/bootstrap_fallback/null_denied",
                    test_fallback_null_denied);
}
