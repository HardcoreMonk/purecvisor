#include <gio/gio.h>
#include <glib.h>

#include "../src/api/rest_auth.h"
#include "../src/utils/pcv_crypto.h"

static void
test_fallback_user_absent_recovers(void)
{

    g_assert_true(pcv_rest_auth_should_fallback_bootstrap("admin",
                                                          "purecvisor",
                                                          "admin",
                                                          "purecvisor",
                                                          FALSE));
}

static void
test_fallback_user_present_denied_sec2(void)
{

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
