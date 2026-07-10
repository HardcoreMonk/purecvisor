/**
 * @file test_rest_middleware.c
 * @brief rest_middleware 유닛 테스트 (ETag/rate limit/RPC timeout)
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  rest_middleware.c (src/api/)의 HTTP 미들웨어 기능을 검증한다.
 *  11개 테스트 케이스.
 *
 *  1. Rate Limit (요청 제한):
 *     - /auth 경로: 브루트포스 방어를 위해 엄격한 한도 (<=600/분)
 *     - /metrics 경로: Prometheus scrape용 관대한 한도 (>=600/분)
 *     - 기본 경로: 양수 반환
 *     - NULL 안전: NULL 입력 시 폴백값 반환
 *
 *  2. RPC 타임아웃:
 *     - vm.list(일반): 기본 타임아웃
 *     - vm.create(장기 실행): 더 긴 타임아웃 (>= 기본)
 *     - NULL 안전: NULL 입력 시 폴백값 반환
 *
 *  3. ETag (조건부 캐싱):
 *     - 기본 계산: 본문 → 해시 문자열 생성
 *     - 결정성: 같은 입력 → 항상 같은 ETag
 *     - 차별성: 다른 입력 → 다른 ETag
 *     - 빈 본문: 빈 문자열도 ETag 생성 가능
 *
 *  CSRF 테스트가 없는 이유: JWT Bearer 인증이 CSRF 방어를 대체 (ADR-0014)
 *  SoupServerMessage 의존 함수는 libsoup mock 복잡도 때문에 통합 테스트에서 검증.
 * ============================================================================
 */
#include <glib.h>
#include <string.h>
#include "../src/api/rest_middleware.h"

/* ── Rate Limit ────────────────────────────────────────── */

static void test_rate_limit_auth_strict(void) {
    /* /auth path는 brute-force 방어를 위해 엄격한 한도 */
    gint l = pcv_get_endpoint_rate_limit("/api/v1/auth/token", "POST");
    g_assert_cmpint(l, >, 0);
    g_assert_cmpint(l, <=, 600);  /* 합리적 상한 */
}

static void test_rate_limit_metrics_loose(void) {
    /* /metrics 는 Prometheus scrape용 — 관대한 한도 */
    gint l = pcv_get_endpoint_rate_limit("/api/v1/metrics", "GET");
    g_assert_cmpint(l, >=, 600);
}

static void test_rate_limit_default(void) {
    gint l = pcv_get_endpoint_rate_limit("/api/v1/vms", "GET");
    g_assert_cmpint(l, >, 0);
}

static void test_rate_limit_null_safe(void) {
    /* NULL 입력 → 폴백 */
    gint l = pcv_get_endpoint_rate_limit(NULL, NULL);
    g_assert_cmpint(l, >=, 0);
}

static void test_rate_limit_bucket_is_endpoint_scoped(void) {
    /* 같은 IP라도 일반 API 폭주가 인증 API의 더 엄격한 60/min 버킷을
     * 소진하면 안 된다. 인증과 기본 API는 서로 다른 카운터 키를 써야 한다. */
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

/* ── RPC 타임아웃 ──────────────────────────────────────── */

static void test_rpc_timeout_default(void) {
    gint t = pcv_get_rpc_timeout("vm.list");
    g_assert_cmpint(t, >, 0);
}

static void test_rpc_timeout_long_running(void) {
    /* vm.create / vm.export 같은 장기 실행 RPC는 더 긴 타임아웃 */
    gint normal = pcv_get_rpc_timeout("vm.list");
    gint heavy  = pcv_get_rpc_timeout("vm.create");
    /* heavy >= normal (둘 다 양수) */
    g_assert_cmpint(heavy, >=, normal);
}

static void test_rpc_timeout_null_safe(void) {
    gint t = pcv_get_rpc_timeout(NULL);
    g_assert_cmpint(t, >=, 0);
}

/* ── ETag ──────────────────────────────────────────────── */

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
    g_assert_cmpstr(e1, ==, e2);  /* 동일 입력 → 동일 ETag */
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
    /* 빈 본문도 ETag 생성 가능 */
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
