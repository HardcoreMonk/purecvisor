#include <glib.h>
#include "modules/daemons/update_check.h"

static void test_compare_equal(void) {
    gboolean up = TRUE;
    g_assert_true(pcv_update_check_compare("1.4.1", "v1.4.1", &up));
    g_assert_false(up);
}
static void test_compare_newer(void) {
    gboolean up = FALSE;
    g_assert_true(pcv_update_check_compare("1.4.1", "v1.4.2", &up));
    g_assert_true(up);
}
static void test_compare_older(void) {
    gboolean up = TRUE;
    g_assert_true(pcv_update_check_compare("1.4.1", "v1.4.0", &up));
    g_assert_false(up);
}
static void test_compare_minor_major(void) {
    gboolean up = FALSE;
    g_assert_true(pcv_update_check_compare("1.4.9", "v1.5.0", &up));
    g_assert_true(up);
    up = FALSE;
    g_assert_true(pcv_update_check_compare("1.9.9", "v2.0.0", &up));
    g_assert_true(up);
}
static void test_compare_no_v_prefix(void) {
    gboolean up = FALSE;
    g_assert_true(pcv_update_check_compare("1.4.1", "1.4.2", &up));
    g_assert_true(up);
}
static void test_compare_prerelease_core(void) {
    gboolean up = FALSE;
    g_assert_true(pcv_update_check_compare("1.4.1", "v1.4.2-rc1", &up));
    g_assert_true(up);
}
static void test_compare_malformed_unknown(void) {
    gboolean up = FALSE;
    g_assert_false(pcv_update_check_compare("1.4.1", "garbage", &up));
    g_assert_false(pcv_update_check_compare("", "v1.4.2", &up));
    g_assert_false(pcv_update_check_compare("1.4", "v1.4.2", &up)); /* 2-튜플 불가 */
}
static void test_parse_ok(void) {
    const char *json = "{\"tag_name\":\"v1.4.2\",\"html_url\":\"https://github.com/HardcoreMonk/purecvisor/releases/tag/v1.4.2\"}";
    char *tag = nullptr, *url = nullptr;
    g_assert_true(pcv_update_check_parse_release(json, -1, &tag, &url));
    g_assert_cmpstr(tag, ==, "1.4.2");
    g_assert_cmpstr(url, ==, "https://github.com/HardcoreMonk/purecvisor/releases/tag/v1.4.2");
    g_free(tag); g_free(url);
}
static void test_parse_bad_url_dropped(void) {
    const char *json = "{\"tag_name\":\"v1.4.2\",\"html_url\":\"http://evil.example.com/x\"}";
    char *tag = nullptr, *url = nullptr;
    g_assert_true(pcv_update_check_parse_release(json, -1, &tag, &url));
    g_assert_cmpstr(tag, ==, "1.4.2");
    g_assert_null(url); /* github.com 아니면 drop */
    g_free(tag);
}
static void test_parse_bad_tag_unknown(void) {
    const char *json = "{\"tag_name\":\"nightly\",\"html_url\":\"https://github.com/x/y\"}";
    char *tag = nullptr, *url = nullptr;
    g_assert_false(pcv_update_check_parse_release(json, -1, &tag, &url));
}
static void test_parse_garbage_json(void) {
    char *tag = nullptr, *url = nullptr;
    g_assert_false(pcv_update_check_parse_release("not json", -1, &tag, &url));
}

void test_update_check_register(void) {
    g_test_add_func("/update_check/compare_equal",        test_compare_equal);
    g_test_add_func("/update_check/compare_newer",        test_compare_newer);
    g_test_add_func("/update_check/compare_older",        test_compare_older);
    g_test_add_func("/update_check/compare_minor_major",  test_compare_minor_major);
    g_test_add_func("/update_check/compare_no_v_prefix",  test_compare_no_v_prefix);
    g_test_add_func("/update_check/compare_prerelease",   test_compare_prerelease_core);
    g_test_add_func("/update_check/compare_malformed",    test_compare_malformed_unknown);
    g_test_add_func("/update_check/parse_ok",             test_parse_ok);
    g_test_add_func("/update_check/parse_bad_url",        test_parse_bad_url_dropped);
    g_test_add_func("/update_check/parse_bad_tag",        test_parse_bad_tag_unknown);
    g_test_add_func("/update_check/parse_garbage",        test_parse_garbage_json);
}
