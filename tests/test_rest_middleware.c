
#include <glib.h>
#include <string.h>
#include "../src/api/rest_middleware.h"

static void test_rate_limit_auth_strict(void) {

    gint l = pcv_get_endpoint_rate_limit("/api/v1/auth/token", "POST");
    g_assert_cmpint(l, >, 0);
    g_assert_cmpint(l, <=, 600);
}

static void test_rate_limit_metrics_loose(void) {

    gint l = pcv_get_endpoint_rate_limit("/api/v1/metrics", "GET");
    g_assert_cmpint(l, >=, 600);
}

static void test_rate_limit_default(void) {
    gint l = pcv_get_endpoint_rate_limit("/api/v1/vms", "GET");
    g_assert_cmpint(l, >, 0);
}

static void test_rate_limit_null_safe(void) {

    gint l = pcv_get_endpoint_rate_limit(NULL, NULL);
    g_assert_cmpint(l, >=, 0);
}

static void test_rate_limit_bucket_is_endpoint_scoped(void) {

    gchar *auth_key = pcv_build_rate_limit_key("127.0.0.1",
                                               "/api/v1/auth/token",
                                               "POST");
    gchar *vm_key = pcv_build_rate_limit_key("127.0.0.1",
                                             "/api/v1/vms",
                                             "GET");
    g_assert_nonnull(auth_key);
    g_assert_nonnull(vm_key);
    g_assert_cmpstr(auth_key, !=, vm_key);
    g_assert_true(g_str_has_prefix(auth_key, "127.0.0.1:auth"));
    g_free(auth_key);
    g_free(vm_key);
}

static void test_rpc_timeout_default(void) {
    gint t = pcv_get_rpc_timeout("vm.list");
    g_assert_cmpint(t, >, 0);
}

static void test_rpc_timeout_long_running(void) {

    gint normal = pcv_get_rpc_timeout("vm.list");
    gint heavy  = pcv_get_rpc_timeout("vm.create");

    g_assert_cmpint(heavy, >=, normal);
}

static void test_rpc_timeout_null_safe(void) {
    gint t = pcv_get_rpc_timeout(NULL);
    g_assert_cmpint(t, >=, 0);
}

static void test_etag_compute_basic(void) {
    const gchar *body = "{\"hello\":\"world\"}";
    gchar *etag = pcv_compute_etag(body, strlen(body));
    g_assert_nonnull(etag);
    g_assert_cmpuint(strlen(etag), >, 0);
    g_free(etag);
}

static void test_etag_deterministic(void) {
    const gchar *body = "{\"a\":1}";
    gchar *e1 = pcv_compute_etag(body, strlen(body));
    gchar *e2 = pcv_compute_etag(body, strlen(body));
    g_assert_cmpstr(e1, ==, e2);
    g_free(e1); g_free(e2);
}

static void test_etag_different_for_different_body(void) {
    gchar *e1 = pcv_compute_etag("{\"a\":1}", 7);
    gchar *e2 = pcv_compute_etag("{\"a\":2}", 7);
    g_assert_cmpstr(e1, !=, e2);
    g_free(e1); g_free(e2);
}

static void test_etag_empty_body(void) {
    gchar *e = pcv_compute_etag("", 0);

    if (e) { g_assert_cmpuint(strlen(e), >, 0); g_free(e); }
}

void test_rest_middleware_register(void) {
    g_test_add_func("/middleware/rate_limit_auth_strict", test_rate_limit_auth_strict);
    g_test_add_func("/middleware/rate_limit_metrics_loose", test_rate_limit_metrics_loose);
    g_test_add_func("/middleware/rate_limit_default", test_rate_limit_default);
    g_test_add_func("/middleware/rate_limit_null_safe", test_rate_limit_null_safe);
    g_test_add_func("/middleware/rate_limit_bucket_is_endpoint_scoped", test_rate_limit_bucket_is_endpoint_scoped);
    g_test_add_func("/middleware/rpc_timeout_default", test_rpc_timeout_default);
    g_test_add_func("/middleware/rpc_timeout_long_running", test_rpc_timeout_long_running);
    g_test_add_func("/middleware/rpc_timeout_null_safe", test_rpc_timeout_null_safe);
    g_test_add_func("/middleware/etag_compute_basic", test_etag_compute_basic);
    g_test_add_func("/middleware/etag_deterministic", test_etag_deterministic);
    g_test_add_func("/middleware/etag_different_for_different_body", test_etag_different_for_different_body);
    g_test_add_func("/middleware/etag_empty_body", test_etag_empty_body);
}
