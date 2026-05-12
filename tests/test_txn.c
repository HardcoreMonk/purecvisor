






















#include <glib.h>
#include "../src/utils/pcv_txn.h"

static int g_rollback_count = 0;
static int g_rollback_order[8];
static int g_rollback_idx = 0;
static int g_free_count = 0;

static void cb_rollback_a(gpointer u) { g_rollback_order[g_rollback_idx++] = 1; g_rollback_count++; (void)u; }
static void cb_rollback_b(gpointer u) { g_rollback_order[g_rollback_idx++] = 2; g_rollback_count++; (void)u; }
static void cb_rollback_c(gpointer u) { g_rollback_order[g_rollback_idx++] = 3; g_rollback_count++; (void)u; }
static void cb_free(gpointer u) { g_free_count++; g_free(u); }

static void reset(void) {
    g_rollback_count = 0;
    g_rollback_idx = 0;
    g_free_count = 0;
    for (int i = 0; i < 8; i++) g_rollback_order[i] = 0;
}

static void test_new_free_empty(void) {
    reset();
    PcvTxn *t = pcv_txn_new("empty");
    g_assert_nonnull(t);
    pcv_txn_free(t);
    g_assert_cmpint(g_rollback_count, ==, 0);
}

static void test_commit_skips_rollback(void) {
    reset();
    PcvTxn *t = pcv_txn_new("commit");
    pcv_txn_add_rollback(t, cb_rollback_a, g_strdup("a"), cb_free);
    pcv_txn_add_rollback(t, cb_rollback_b, g_strdup("b"), cb_free);
    pcv_txn_commit(t);
    pcv_txn_free(t);
    g_assert_cmpint(g_rollback_count, ==, 0);
    g_assert_cmpint(g_free_count, ==, 2);
}

static void test_free_triggers_rollback_lifo(void) {
    reset();
    PcvTxn *t = pcv_txn_new("auto-rollback");
    pcv_txn_add_rollback(t, cb_rollback_a, (gpointer)"a", NULL);
    pcv_txn_add_rollback(t, cb_rollback_b, (gpointer)"b", NULL);
    pcv_txn_add_rollback(t, cb_rollback_c, (gpointer)"c", NULL);
    pcv_txn_free(t);
    g_assert_cmpint(g_rollback_count, ==, 3);

    g_assert_cmpint(g_rollback_order[0], ==, 3);
    g_assert_cmpint(g_rollback_order[1], ==, 2);
    g_assert_cmpint(g_rollback_order[2], ==, 1);

    g_assert_cmpint(g_free_count, ==, 0);
}

static void test_explicit_rollback(void) {
    reset();
    PcvTxn *t = pcv_txn_new("explicit");
    pcv_txn_add_rollback(t, cb_rollback_a, g_strdup("a"), cb_free);
    pcv_txn_add_rollback(t, cb_rollback_b, g_strdup("b"), cb_free);
    pcv_txn_rollback(t);
    g_assert_cmpint(g_rollback_count, ==, 2);
    pcv_txn_free(t);

    g_assert_cmpint(g_rollback_count, ==, 2);
    g_assert_cmpint(g_free_count, ==, 2);
}

static void test_null_safety(void) {

    pcv_txn_free(NULL);
    pcv_txn_rollback(NULL);
    pcv_txn_commit(NULL);
}

void test_txn_register(void) {
    g_test_add_func("/txn/new_free_empty", test_new_free_empty);
    g_test_add_func("/txn/commit_skips_rollback", test_commit_skips_rollback);
    g_test_add_func("/txn/free_triggers_rollback_lifo", test_free_triggers_rollback_lifo);
    g_test_add_func("/txn/explicit_rollback", test_explicit_rollback);
    g_test_add_func("/txn/null_safety", test_null_safety);
}
