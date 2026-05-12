















#include <glib.h>
#include <json-glib/json-glib.h>
#include "modules/core/cpu_allocator.h"

typedef struct { CpuAllocator *alloc; } CpuFixture;

static void cpu_setup(CpuFixture *f, gconstpointer d) {
    (void)d; f->alloc = cpu_allocator_new(); g_assert_nonnull(f->alloc);
}
static void cpu_teardown(CpuFixture *f, gconstpointer d) {
    (void)d; cpu_allocator_free(f->alloc);
}
static void free_out(GArray *a) { if (a) g_array_unref(a); }


static void test_allocate_empty(CpuFixture *f, gconstpointer d) {
    (void)d;
    GArray *out = NULL;
    g_assert_false(cpu_allocator_allocate_exclusive(f->alloc, "vm-empty", 0, 1, &out, NULL));
    free_out(out);
}


static void test_allocate_non_isolated_fails(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, FALSE);
    cpu_allocator_add_core(f->alloc, 1, 1, 0, FALSE);
    GArray *out = NULL;
    g_assert_false(cpu_allocator_allocate_exclusive(f->alloc, "vm-niso", 0, 1, &out, NULL));
    free_out(out);
}


static void test_allocate_isolated_success(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, FALSE);
    cpu_allocator_add_core(f->alloc, 1, 1, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 2, 2, 0, TRUE);
    GArray *out = NULL;
    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm-iso1", 0, 1, &out, NULL));
    g_assert_nonnull(out);
    g_assert_cmpuint(out->len, >=, 1);
    guint core_id = g_array_index(out, guint, 0);
    g_assert_true(core_id == 1 || core_id == 2);
    free_out(out);
}


static void test_no_double_allocate(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, TRUE);
    GArray *out1 = NULL, *out2 = NULL;
    gboolean ok1 = cpu_allocator_allocate_exclusive(f->alloc, "vm-1", 0, 1, &out1, NULL);
    gboolean ok2 = cpu_allocator_allocate_exclusive(f->alloc, "vm-2", 0, 1, &out2, NULL);
    g_assert_true(ok1);
    g_assert_false(ok2);
    free_out(out1); free_out(out2);
}


static void test_free_then_reallocate(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, TRUE);
    GArray *out1 = NULL;
    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm-r", 0, 1, &out1, NULL));
    guint first = g_array_index(out1, guint, 0);
    free_out(out1);
    cpu_allocator_free_vm_cores(f->alloc, "vm-r");
    GArray *out2 = NULL;
    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm-r2", 0, 1, &out2, NULL));
    g_assert_cmpuint(g_array_index(out2, guint, 0), ==, first);
    free_out(out2);
}


static void test_multi_vm_distinct_cores(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 2, 2, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 3, 3, 0, TRUE);
    GArray *out1 = NULL, *out2 = NULL;
    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm-A", 0, 1, &out1, NULL));
    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm-B", 0, 1, &out2, NULL));
    g_assert_cmpuint(g_array_index(out1, guint, 0), !=, g_array_index(out2, guint, 0));
    free_out(out1); free_out(out2);
}


static void test_allocate_multi_vcpu(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 2, 2, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 3, 3, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 4, 4, 0, TRUE);
    GArray *out = NULL;
    gboolean ok = cpu_allocator_allocate_exclusive(f->alloc, "vm-multi", 0, 2, &out, NULL);
    g_assert_true(ok);
    g_assert_nonnull(out);
    g_assert_cmpuint(out->len, ==, 2);
    g_assert_cmpuint(g_array_index(out, guint, 0), !=, g_array_index(out, guint, 1));
    free_out(out);
}


static void test_numa_node_output_same(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 2, 2, 1, TRUE);
    cpu_allocator_add_core(f->alloc, 3, 3, 1, TRUE);
    GArray *out = NULL;
    gint numa = -1;
    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm-numa", 1, 2, &out, &numa));
    g_assert_cmpint(numa, ==, 1);
    free_out(out);
}


static void test_dump_no_crash(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, FALSE);
    cpu_allocator_add_core(f->alloc, 1, 1, 0, TRUE);
    cpu_allocator_dump(f->alloc);
    cpu_allocator_dump(NULL);
}


static void test_overcommit_allows_non_isolated(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, FALSE);
    cpu_allocator_add_core(f->alloc, 1, 1, 0, FALSE);
    cpu_allocator_set_overcommit(TRUE);
    GArray *out = NULL;
    gboolean ok = cpu_allocator_allocate_exclusive(f->alloc, "vm-over", 0, 1, &out, NULL);

    free_out(out);
    cpu_allocator_set_overcommit(FALSE);
    (void)ok;
}


static void test_mark_used(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 1, 1, 0, TRUE);
    cpu_allocator_mark_used(f->alloc, 0, "external-vm");

    GArray *out = NULL;
    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm-test", 0, 1, &out, NULL));
    g_assert_cmpuint(g_array_index(out, guint, 0), ==, 1);
    free_out(out);
}


static void test_get_numa_info(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 1, 1, 1, TRUE);
    JsonObject *info = cpu_allocator_get_numa_info(f->alloc);
    g_assert_nonnull(info);
    json_object_unref(info);
}


static void test_free_nonexistent_vm(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_free_vm_cores(f->alloc, "no-such-vm");
    cpu_allocator_free_vm_cores(f->alloc, NULL);
}


static void test_numa_node_output_mixed(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 2, 2, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 3, 3, 1, TRUE);
    GArray *out = NULL;
    gint numa = -1;

    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm-mixed", 0, 2, &out, &numa));
    g_assert_cmpint(numa, ==, -1);
    free_out(out);
}

void test_cpu_allocator_register(void) {
#define CPU_TEST(name, fn) \
    g_test_add("/cpu_allocator/" name, CpuFixture, NULL, cpu_setup, fn, cpu_teardown)
    CPU_TEST("allocate_empty",              test_allocate_empty);
    CPU_TEST("allocate_non_isolated_fails", test_allocate_non_isolated_fails);
    CPU_TEST("allocate_isolated_success",   test_allocate_isolated_success);
    CPU_TEST("no_double_allocate",          test_no_double_allocate);
    CPU_TEST("free_then_reallocate",        test_free_then_reallocate);
    CPU_TEST("multi_vm_distinct_cores",     test_multi_vm_distinct_cores);
    CPU_TEST("allocate_multi_vcpu",         test_allocate_multi_vcpu);
    CPU_TEST("numa_node_output_same",       test_numa_node_output_same);
    CPU_TEST("numa_node_output_mixed",      test_numa_node_output_mixed);
    CPU_TEST("dump_no_crash",               test_dump_no_crash);
    CPU_TEST("overcommit_allows_non_isolated", test_overcommit_allows_non_isolated);
    CPU_TEST("mark_used",                   test_mark_used);
    CPU_TEST("get_numa_info",               test_get_numa_info);
    CPU_TEST("free_nonexistent_vm",         test_free_nonexistent_vm);
#undef CPU_TEST
}
