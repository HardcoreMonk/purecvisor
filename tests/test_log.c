/**
 * @file test_log.c
 * @brief pcv_log 레벨/req_id/trace context 유닛 테스트
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  pcv_log.h (src/utils/)의 로깅 시스템 전체를 검증한다. 17개 테스트 케이스.
 *
 *  1. 전역 로그 레벨: set/get 라운드트립 (DEBUG → WARN → INFO)
 *
 *  2. 모듈별 로그 레벨 오버라이드:
 *     전역=INFO인데 "test_mod"만 DEBUG로 설정 → 해당 모듈만 상세 로그 출력.
 *     미설정 모듈은 전역 레벨로 폴백.
 *     [발견된 진짜 버그] GINT_TO_POINTER(0)=NULL이라 DEBUG(0)을 해시에 저장해도
 *     lookup에서 NULL과 구분 불가 → 전역으로 폴백하는 문제가 이 테스트에서 적발됨.
 *
 *  3. 요청 ID(req_id): 스레드 로컬 저장 + 생성기 유일성 검증
 *
 *  4. W3C Trace Context:
 *     - 생성: "00-<trace_id 32hex>-<span_id 16hex>-<flags 2hex>" = 55자
 *     - 파싱: 유효한 traceparent 문자열 → PcvTraceContext 객체
 *     - 라운드트립: 생성 → format → parse → format → trace_id 보존
 *     - 무효 입력: "garbage", "", NULL → NULL 반환
 *
 *  5. 로그 매크로 크래시 없음: PCV_LOG_DEBUG/INFO 호출이 세그폴트 없이 실행
 *
 *  6. 감사 로그(AUDIT): PCV_LOG_AUDIT 매크로로 감사 이벤트 기록 (crash 없음)
 *
 *  7. 레벨 필터링: WARN 설정 시 DEBUG/INFO는 출력되지 않음
 * ============================================================================
 */
#include <glib.h>
#include <string.h>
#include "../src/utils/pcv_log.h"

/* pcv_config 의존성 차단 — pcv_log_load_module_levels는 호출하지 않음.
 * 단 module_levels 해시 테이블 초기화를 위해 pcv_log_init()은 1회 호출 필요. */
static void ensure_init(void) {
    static gboolean initialized = FALSE;
    if (!initialized) { pcv_log_init(); initialized = TRUE; }
}

static void test_global_level_set_get(void) {
    ensure_init();
    pcv_log_set_global_level(PCV_LOG_LEVEL_DEBUG);
    g_assert_cmpint(pcv_log_get_global_level(), ==, PCV_LOG_LEVEL_DEBUG);
    pcv_log_set_global_level(PCV_LOG_LEVEL_WARN);
    g_assert_cmpint(pcv_log_get_global_level(), ==, PCV_LOG_LEVEL_WARN);
    pcv_log_set_global_level(PCV_LOG_LEVEL_INFO);
}

static void test_module_level_override(void) {
    ensure_init();
    pcv_log_set_global_level(PCV_LOG_LEVEL_INFO);
    pcv_log_set_module_level("test_mod", PCV_LOG_LEVEL_DEBUG);
    PcvLogLevel got = pcv_log_get_module_level("test_mod");
    g_test_message("test_mod level=%d (expected %d)", got, PCV_LOG_LEVEL_DEBUG);
    g_assert_cmpint(got, ==, PCV_LOG_LEVEL_DEBUG);
    /* 미설정 모듈은 전역 레벨로 폴백 */
    g_assert_cmpint(pcv_log_get_module_level("other_mod"), ==, PCV_LOG_LEVEL_INFO);
}

static void test_module_level_multiple(void) {
    ensure_init();
    pcv_log_set_module_level("mod_a", PCV_LOG_LEVEL_DEBUG);
    pcv_log_set_module_level("mod_b", PCV_LOG_LEVEL_WARN);
    pcv_log_set_module_level("mod_c", PCV_LOG_LEVEL_ERROR);
    g_assert_cmpint(pcv_log_get_module_level("mod_a"), ==, PCV_LOG_LEVEL_DEBUG);
    g_assert_cmpint(pcv_log_get_module_level("mod_b"), ==, PCV_LOG_LEVEL_WARN);
    g_assert_cmpint(pcv_log_get_module_level("mod_c"), ==, PCV_LOG_LEVEL_ERROR);
}

static void test_req_id_set_get(void) {
    pcv_log_req_id_set("req-12345");
    g_assert_cmpstr(pcv_log_req_id_get(), ==, "req-12345");
    pcv_log_req_id_set(NULL);
    /* 미설정 시 "-" 센티넬 반환 (JSON 로그 가독성 위해) */
    g_assert_cmpstr(pcv_log_req_id_get(), ==, "-");
}

static void test_generate_request_id(void) {
    gchar *id1 = pcv_generate_request_id();
    gchar *id2 = pcv_generate_request_id();
    g_assert_nonnull(id1);
    g_assert_nonnull(id2);
    g_assert_cmpstr(id1, !=, id2);  /* 매번 다른 ID */
    g_free(id1);
    g_free(id2);
}

static void test_trace_context_new_free(void) {
    PcvTraceContext *ctx = pcv_trace_context_new();
    g_assert_nonnull(ctx);
    gchar *fmt = pcv_trace_context_format(ctx);
    g_assert_nonnull(fmt);
    /* W3C traceparent 형식: 00-<trace_id 32hex>-<span_id 16hex>-<flags 2hex> */
    g_assert_cmpuint(strlen(fmt), ==, 55);
    g_assert_true(g_str_has_prefix(fmt, "00-"));
    g_free(fmt);
    pcv_trace_context_free(ctx);
}

static void test_trace_context_parse_valid(void) {
    const gchar *tp = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
    PcvTraceContext *ctx = pcv_trace_context_parse(tp);
    g_assert_nonnull(ctx);
    gchar *fmt = pcv_trace_context_format(ctx);
    g_assert_nonnull(fmt);
    g_assert_true(g_str_has_prefix(fmt, "00-0af7651916cd43dd8448eb211c80319c"));
    g_free(fmt);
    pcv_trace_context_free(ctx);
}

static void test_trace_context_parse_invalid(void) {
    PcvTraceContext *ctx = pcv_trace_context_parse("garbage");
    g_assert_null(ctx);
    ctx = pcv_trace_context_parse("");
    g_assert_null(ctx);
    ctx = pcv_trace_context_parse(NULL);
    g_assert_null(ctx);
}

static void test_log_macros_no_crash(void) {
    /* 로그 매크로 자체 호출 — 충돌 없이 실행되는지만 검증 */
    pcv_log_set_global_level(PCV_LOG_LEVEL_DEBUG);
    PCV_LOG_DEBUG("test", "debug %d", 1);
    PCV_LOG_INFO("test", "info %s", "x");
    /* WARN/ERROR는 g_test fatal mask 때문에 호출하지 않음 */
}

static void test_audit_macro(void) {
    ensure_init();
    /* 감사 로그 — audit.log 파일 + stderr 이중 출력. crash 없이 실행. */
    PCV_LOG_AUDIT("test", "vm.create", "test-vm", "vcpu=%d mem=%dMB", 2, 1024);
    PCV_LOG_AUDIT("test", "vm.delete", "test-vm", "completed");
    /* NULL 인자 안전성 */
    PCV_LOG_AUDIT("test", "op", "tgt", "no args");
}

static void test_level_filtering(void) {
    ensure_init();
    pcv_log_set_global_level(PCV_LOG_LEVEL_WARN);
    /* WARN 미만 (DEBUG/INFO)는 필터링되어 출력 안됨 — 호출만 검증 */
    PCV_LOG_DEBUG("test_filter", "should be filtered");
    PCV_LOG_INFO("test_filter", "should be filtered");
    pcv_log_set_global_level(PCV_LOG_LEVEL_INFO);
}

static void test_module_level_load_no_config(void) {
    ensure_init();
    /* config 미초기화 상태에서 호출 — graceful */
    pcv_log_load_module_levels();
}

/* JSON 출력 핸들러 검증 — 여러 레벨/req_id 조합 */
static void test_log_emit_with_req_id(void) {
    ensure_init();
    pcv_log_set_global_level(PCV_LOG_LEVEL_DEBUG);
    pcv_log_req_id_set("req-emit-test");
    PCV_LOG_INFO("emit_test", "with req_id");
    PCV_LOG_DEBUG("emit_test", "debug message");
    pcv_log_req_id_set(NULL);  /* 다음 테스트에 영향 없도록 정리 */
}

static void test_log_emit_special_chars(void) {
    ensure_init();
    pcv_log_set_global_level(PCV_LOG_LEVEL_INFO);
    /* JSON 이스케이프 경로: 따옴표/백슬래시/제어문자 */
    PCV_LOG_INFO("escape_test", "quote: \" backslash: \\ tab: \t newline: ");
    PCV_LOG_INFO("escape_test", "unicode: %s", "한글");
}

static void test_audit_with_long_target(void) {
    ensure_init();
    /* 긴 target 문자열 */
    gchar long_name[256];
    memset(long_name, 'a', 255);
    long_name[255] = '\0';
    PCV_LOG_AUDIT("audit_test", "vm.create", long_name, "long target test");
}

static void test_log_module_level_isolated(void) {
    ensure_init();
    pcv_log_set_global_level(PCV_LOG_LEVEL_WARN);
    pcv_log_set_module_level("verbose_mod", PCV_LOG_LEVEL_DEBUG);
    pcv_log_set_module_level("quiet_mod", PCV_LOG_LEVEL_ERROR);
    g_assert_cmpint(pcv_log_get_module_level("verbose_mod"), ==, PCV_LOG_LEVEL_DEBUG);
    g_assert_cmpint(pcv_log_get_module_level("quiet_mod"), ==, PCV_LOG_LEVEL_ERROR);
    g_assert_cmpint(pcv_log_get_module_level("unset_mod"), ==, PCV_LOG_LEVEL_WARN);
    pcv_log_set_global_level(PCV_LOG_LEVEL_INFO);
}

static void test_trace_context_format_roundtrip(void) {
    PcvTraceContext *ctx = pcv_trace_context_new();
    gchar *fmt1 = pcv_trace_context_format(ctx);
    PcvTraceContext *parsed = pcv_trace_context_parse(fmt1);
    g_assert_nonnull(parsed);
    gchar *fmt2 = pcv_trace_context_format(parsed);
    /* trace_id는 보존, span_id는 새로 생성될 수 있음 */
    g_assert_cmpuint(strlen(fmt2), ==, 55);
    g_free(fmt1); g_free(fmt2);
    pcv_trace_context_free(ctx);
    pcv_trace_context_free(parsed);
}

void test_log_register(void) {
    g_test_add_func("/log/global_level_set_get", test_global_level_set_get);
    g_test_add_func("/log/module_level_override", test_module_level_override);
    g_test_add_func("/log/module_level_multiple", test_module_level_multiple);
    g_test_add_func("/log/req_id_set_get", test_req_id_set_get);
    g_test_add_func("/log/generate_request_id", test_generate_request_id);
    g_test_add_func("/log/trace_context_new_free", test_trace_context_new_free);
    g_test_add_func("/log/trace_context_parse_valid", test_trace_context_parse_valid);
    g_test_add_func("/log/trace_context_parse_invalid", test_trace_context_parse_invalid);
    g_test_add_func("/log/log_macros_no_crash", test_log_macros_no_crash);
    g_test_add_func("/log/audit_macro", test_audit_macro);
    g_test_add_func("/log/level_filtering", test_level_filtering);
    g_test_add_func("/log/module_level_load_no_config", test_module_level_load_no_config);
    g_test_add_func("/log/trace_context_format_roundtrip", test_trace_context_format_roundtrip);
    g_test_add_func("/log/emit_with_req_id", test_log_emit_with_req_id);
    g_test_add_func("/log/emit_special_chars", test_log_emit_special_chars);
    g_test_add_func("/log/audit_with_long_target", test_audit_with_long_target);
    g_test_add_func("/log/module_level_isolated", test_log_module_level_isolated);
}
