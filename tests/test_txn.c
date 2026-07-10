/**
 * @file test_txn.c
 * @brief pcv_txn 트랜잭션/롤백 유닛 테스트
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  pcv_txn.h (src/utils/)의 트랜잭션 래퍼를 검증한다. 5개 테스트 케이스.
 *
 *  PcvTxn은 "여러 단계 작업 중 하나라도 실패하면 이전 단계를 되돌리는" 패턴이다.
 *  예: VM 생성 = (1) zvol 생성 → (2) XML 정의 → (3) 네트워크 설정
 *      (3)에서 실패하면 (2)와 (1)을 역순으로 롤백한다.
 *
 *  핵심 규칙:
 *  - pcv_txn_commit() 호출 시: 롤백 콜백은 실행되지 않고 user_data만 free
 *  - pcv_txn_free() 호출 시 (미커밋): 등록된 롤백을 LIFO(후입선출) 순서로 실행
 *  - pcv_txn_rollback() 명시적 호출: 즉시 LIFO 롤백, 이후 free에서 중복 실행 안 함
 *  - NULL 안전: free/rollback/commit에 NULL 전달 가능
 *
 *  g_rollback_order[] 배열로 LIFO 순서를 직접 검증한다.
 *  (cb_rollback_a=1, cb_rollback_b=2, cb_rollback_c=3 → 역순: 3,2,1)
 * ============================================================================
 */
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
    pcv_txn_free(t); /* 미커밋 → 자동 롤백 LIFO */
    g_assert_cmpint(g_rollback_count, ==, 3);
    /* LIFO: c → b → a */
    g_assert_cmpint(g_rollback_order[0], ==, 3);
    g_assert_cmpint(g_rollback_order[1], ==, 2);
    g_assert_cmpint(g_rollback_order[2], ==, 1);
    /* free_func 미지정 항목이므로 추가 해제 콜백은 실행되지 않음 */
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
    /* rollback 후 free는 추가 롤백 안 함 */
    g_assert_cmpint(g_rollback_count, ==, 2);
    g_assert_cmpint(g_free_count, ==, 2);
}

static void test_null_safety(void) {
    /* NULL 입력 안전성 */
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
