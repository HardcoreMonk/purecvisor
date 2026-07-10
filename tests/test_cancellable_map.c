/* tests/test_cancellable_map.c
 *
 * 대상 모듈: src/modules/virt/cancellable_map.c — VM별 GCancellable 관리
 *
 * 이 테스트가 검증하는 것:
 *   비동기 GTask 취소를 위한 VM→GCancellable 매핑 자료구조의 CRUD와
 *   취소 전파를 검사한다. 중복 등록 시 교체, 미존재 키 안전성,
 *   cancel_all 일괄 취소, shutdown 정리를 포함.
 *
 * 실행: sudo ./test_runner -p /cancellable_map
 *
 * 테스트 추가: CM_TEST("이름", 함수) 매크로로 등록
 *
 * 외부 의존: 없음 (GIO GCancellable만 사용, 네트워크/디스크 없음)
 */

#include <glib.h>
#include <gio/gio.h>
#include "modules/virt/cancellable_map.h"

typedef struct { int dummy; } CmapFixture;

static void cm_setup(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    cmap_init();
}
static void cm_teardown(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    cmap_shutdown();
}

/* ── 초기 상태 ───────────────────────────────────────── */

static void test_initial_empty(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_cmpuint(cmap_size(), ==, 0);
}

/* ── register → size 증가 ────────────────────────────── */

static void test_register_increases_size(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    GCancellable *c1 = g_cancellable_new();
    GCancellable *c2 = g_cancellable_new();

    cmap_register("vm-a", c1);
    g_assert_cmpuint(cmap_size(), ==, 1);
    cmap_register("vm-b", c2);
    g_assert_cmpuint(cmap_size(), ==, 2);

    g_object_unref(c1);
    g_object_unref(c2);
}

/* ── remove → size 감소 ──────────────────────────────── */

static void test_remove_decreases_size(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    GCancellable *c = g_cancellable_new();
    cmap_register("vm-x", c);
    g_assert_cmpuint(cmap_size(), ==, 1);
    cmap_remove("vm-x");
    g_assert_cmpuint(cmap_size(), ==, 0);
    g_object_unref(c);
}

/* ── cancel → GCancellable 취소 확인 ────────────────── */

static void test_cancel_sets_cancelled(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    GCancellable *c = g_cancellable_new();
    g_assert_false(g_cancellable_is_cancelled(c));

    cmap_register("vm-cancel", c);
    cmap_cancel("vm-cancel");

    g_assert_true(g_cancellable_is_cancelled(c));
    g_object_unref(c);
}

/* ── cancel_all ──────────────────────────────────────── */

static void test_cancel_all(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    GCancellable *c1 = g_cancellable_new();
    GCancellable *c2 = g_cancellable_new();
    GCancellable *c3 = g_cancellable_new();

    cmap_register("vm1", c1);
    cmap_register("vm2", c2);
    cmap_register("vm3", c3);

    cmap_cancel_all();

    g_assert_true(g_cancellable_is_cancelled(c1));
    g_assert_true(g_cancellable_is_cancelled(c2));
    g_assert_true(g_cancellable_is_cancelled(c3));

    /* cancel_all 은 맵을 비우지 않음 — size 유지 */
    g_assert_cmpuint(cmap_size(), ==, 3);

    g_object_unref(c1);
    g_object_unref(c2);
    g_object_unref(c3);
}

/* ── 중복 register → 기존 교체 ──────────────────────── */

static void test_duplicate_register_replaces(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    GCancellable *c1 = g_cancellable_new();
    GCancellable *c2 = g_cancellable_new();

    cmap_register("vm-dup", c1);
    cmap_register("vm-dup", c2);  /* 교체 */

    /* size 는 여전히 1 */
    g_assert_cmpuint(cmap_size(), ==, 1);

    /* c2 를 취소 → 등록된 cancellable 이 c2 임을 확인 */
    cmap_cancel("vm-dup");
    g_assert_true(g_cancellable_is_cancelled(c2));
    /* c1 은 취소 안 됨 */
    g_assert_false(g_cancellable_is_cancelled(c1));

    g_object_unref(c1);
    g_object_unref(c2);
}

/* ── 없는 키 cancel/remove → crash 없음 ─────────────── */

static void test_nonexistent_key_safe(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    /* crash 없이 조용히 무시해야 함 */
    cmap_cancel("no-such-vm");
    cmap_remove("no-such-vm");
    g_assert_cmpuint(cmap_size(), ==, 0);
}

/* ── shutdown 후 size == 0 ───────────────────────────── */

static void test_shutdown_clears(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    GCancellable *c = g_cancellable_new();
    cmap_register("vm-s", c);
    g_assert_cmpuint(cmap_size(), ==, 1);

    cmap_shutdown();
    /* shutdown 후 재초기화 */
    cmap_init();
    g_assert_cmpuint(cmap_size(), ==, 0);
    g_object_unref(c);
}

/* ── 등록 함수 ───────────────────────────────────────── */

void test_cancellable_map_register(void) {
#define CM_TEST(name, fn) \
    g_test_add("/cancellable_map/" name, CmapFixture, NULL, \
               cm_setup, fn, cm_teardown)

    CM_TEST("initial_empty",              test_initial_empty);
    CM_TEST("register_increases_size",    test_register_increases_size);
    CM_TEST("remove_decreases_size",      test_remove_decreases_size);
    CM_TEST("cancel_sets_cancelled",      test_cancel_sets_cancelled);
    CM_TEST("cancel_all",                 test_cancel_all);
    CM_TEST("duplicate_register_replaces",test_duplicate_register_replaces);
    CM_TEST("nonexistent_key_safe",       test_nonexistent_key_safe);
    CM_TEST("shutdown_clears",            test_shutdown_clears);
#undef CM_TEST
}
