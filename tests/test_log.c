
#include <glib.h>
#include <string.h>
#include "../src/utils/pcv_log.h"

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

    g_assert_cmpstr(pcv_log_req_id_get(), ==, "-");
}

static void test_generate_request_id(void) {
    gchar *id1 = pcv_generate_request_id();
    gchar *id2 = pcv_generate_request_id();
    g_assert_nonnull(id1);
    g_assert_nonnull(id2);
    g_assert_cmpstr(id1, !=, id2);
    g_free(id1);
    g_free(id2);
}

static void test_trace_context_new_free(void) {
    PcvTraceContext *ctx = pcv_trace_context_new();
    g_assert_nonnull(ctx);
    gchar *fmt = pcv_trace_context_format(ctx);
    g_assert_nonnull(fmt);

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

    pcv_log_set_global_level(PCV_LOG_LEVEL_DEBUG);
    PCV_LOG_DEBUG("test", "debug %d", 1);
    PCV_LOG_INFO("test", "info %s", "x");

}

static void test_audit_macro(void) {
    ensure_init();

    PCV_LOG_AUDIT("test", "vm.create", "test-vm", "vcpu=%d mem=%dMB", 2, 1024);
    PCV_LOG_AUDIT("test", "vm.delete", "test-vm", "completed");

    PCV_LOG_AUDIT("test", "op", "tgt", "no args");
}

static void test_level_filtering(void) {
    ensure_init();
    pcv_log_set_global_level(PCV_LOG_LEVEL_WARN);

    PCV_LOG_DEBUG("test_filter", "should be filtered");
    PCV_LOG_INFO("test_filter", "should be filtered");
    pcv_log_set_global_level(PCV_LOG_LEVEL_INFO);
}

static void test_module_level_load_no_config(void) {
    ensure_init();

    pcv_log_load_module_levels();
}

static void test_log_emit_with_req_id(void) {
    ensure_init();
    pcv_log_set_global_level(PCV_LOG_LEVEL_DEBUG);
    pcv_log_req_id_set("req-emit-test");
    PCV_LOG_INFO("emit_test", "with req_id");
    PCV_LOG_DEBUG("emit_test", "debug message");
    pcv_log_req_id_set(NULL);
}

static void test_log_emit_special_chars(void) {
    ensure_init();
    pcv_log_set_global_level(PCV_LOG_LEVEL_INFO);

    PCV_LOG_INFO("escape_test", "quote: \" backslash: \\ tab: \t newline: ");
    PCV_LOG_INFO("escape_test", "unicode: %s", "한글");
}

static void test_audit_with_long_target(void) {
    ensure_init();

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
