
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

static void test_initial_empty(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    g_assert_cmpuint(cmap_size(), ==, 0);
}

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

static void test_remove_decreases_size(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    GCancellable *c = g_cancellable_new();
    cmap_register("vm-x", c);
    g_assert_cmpuint(cmap_size(), ==, 1);
    cmap_remove("vm-x");
    g_assert_cmpuint(cmap_size(), ==, 0);
    g_object_unref(c);
}

static void test_cancel_sets_cancelled(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    GCancellable *c = g_cancellable_new();
    g_assert_false(g_cancellable_is_cancelled(c));

    cmap_register("vm-cancel", c);
    cmap_cancel("vm-cancel");

    g_assert_true(g_cancellable_is_cancelled(c));
    g_object_unref(c);
}

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

    g_assert_cmpuint(cmap_size(), ==, 3);

    g_object_unref(c1);
    g_object_unref(c2);
    g_object_unref(c3);
}

static void test_duplicate_register_replaces(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    GCancellable *c1 = g_cancellable_new();
    GCancellable *c2 = g_cancellable_new();

    cmap_register("vm-dup", c1);
    cmap_register("vm-dup", c2);

    g_assert_cmpuint(cmap_size(), ==, 1);

    cmap_cancel("vm-dup");
    g_assert_true(g_cancellable_is_cancelled(c2));

    g_assert_false(g_cancellable_is_cancelled(c1));

    g_object_unref(c1);
    g_object_unref(c2);
}

static void test_nonexistent_key_safe(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;

    cmap_cancel("no-such-vm");
    cmap_remove("no-such-vm");
    g_assert_cmpuint(cmap_size(), ==, 0);
}

static void test_shutdown_clears(CmapFixture *f, gconstpointer d) {
    (void)f; (void)d;
    GCancellable *c = g_cancellable_new();
    cmap_register("vm-s", c);
    g_assert_cmpuint(cmap_size(), ==, 1);

    cmap_shutdown();

    cmap_init();
    g_assert_cmpuint(cmap_size(), ==, 0);
    g_object_unref(c);
}

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
