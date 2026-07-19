
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "../src/modules/dispatcher/rpc_utils.h"

static void test_accepts_normal(void) {
    JsonParser *p = NULL;
    GError *e = NULL;
    const char *j = "{\"method\":\"vm.list\",\"id\":1}";

    g_assert_true(pcv_rpc_parse_guarded(j, -1, &p, &e));
    g_assert_nonnull(p);
    g_assert_null(e);

    g_object_unref(p);
}

static void test_rejects_deep(void) {
    GString *s = g_string_new("");
    for (int i = 0; i < 200; i++) {
        g_string_append_c(s, '[');
    }

    JsonParser *p = NULL;
    GError *e = NULL;

    g_assert_false(pcv_rpc_parse_guarded(s->str, (gssize)s->len, &p, &e));
    g_assert_null(p);

    if (e) g_error_free(e);
    g_string_free(s, TRUE);
}

static void test_rejects_oversized(void) {
    gsize n = 2u * 1024 * 1024;
    gchar *big = g_malloc(n + 1);
    memset(big, 'a', n);
    big[n] = 0;
    big[0] = '"';
    big[n - 1] = '"';

    JsonParser *p = NULL;
    GError *e = NULL;

    g_assert_false(pcv_rpc_parse_guarded(big, (gssize)n, &p, &e));
    g_assert_null(p);

    if (e) g_error_free(e);
    g_free(big);
}

static void test_bounded_len_not_nul_terminated(void) {

    char buf[64];
    memcpy(buf, "{\"a\":1}GARBAGE", 14);

    JsonParser *p = NULL;
    GError *e = NULL;

    g_assert_true(pcv_rpc_parse_guarded(buf, 7, &p, &e));
    g_assert_nonnull(p);
    g_assert_null(e);

    if (p) g_object_unref(p);
    if (e) g_error_free(e);
}

void test_rpc_parse_guarded_register(void) {
    g_test_add_func("/rpc_parse_guarded/accepts_normal", test_accepts_normal);
    g_test_add_func("/rpc_parse_guarded/rejects_deep", test_rejects_deep);
    g_test_add_func("/rpc_parse_guarded/rejects_oversized", test_rejects_oversized);
    g_test_add_func("/rpc_parse_guarded/bounded_len_not_nul_terminated", test_bounded_len_not_nul_terminated);
}
