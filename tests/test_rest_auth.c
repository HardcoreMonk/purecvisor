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

/*
 * SEC-8: pcv_secret_str_eq 정확성 테스트. 타이밍 자체는 단위테스트로
 * 검증 불가하므로 NULL/길이차/동일/상이 케이스로 정확성만 확인한다.
 */
static void
test_secret_str_eq_identical_true(void)
{
    g_assert_true(pcv_secret_str_eq("purecvisor", "purecvisor"));
}

static void
test_secret_str_eq_different_false(void)
{
    g_assert_false(pcv_secret_str_eq("purecvisor", "wrongpass"));
}

static void
test_secret_str_eq_length_diff_false(void)
{
    g_assert_false(pcv_secret_str_eq("a", "aa"));
}

static void
test_secret_str_eq_null_denied(void)
{
    g_assert_false(pcv_secret_str_eq(NULL, "purecvisor"));
    g_assert_false(pcv_secret_str_eq("purecvisor", NULL));
    g_assert_false(pcv_secret_str_eq(NULL, NULL));
}

static void
test_secret_str_eq_empty_identical_true(void)
{
    g_assert_true(pcv_secret_str_eq("", ""));
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
    g_test_add_func("/rest_auth/secret_str_eq/identical_true",
                    test_secret_str_eq_identical_true);
    g_test_add_func("/rest_auth/secret_str_eq/different_false",
                    test_secret_str_eq_different_false);
    g_test_add_func("/rest_auth/secret_str_eq/length_diff_false",
                    test_secret_str_eq_length_diff_false);
    g_test_add_func("/rest_auth/secret_str_eq/null_denied",
                    test_secret_str_eq_null_denied);
    g_test_add_func("/rest_auth/secret_str_eq/empty_identical_true",
                    test_secret_str_eq_empty_identical_true);
}
