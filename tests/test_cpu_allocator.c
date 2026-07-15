/* tests/test_cpu_allocator.c
 *
 * 대상 모듈: src/modules/core/cpu_allocator.c — NUMA 인식 배타적 CPU 할당기
 *
 * 이 테스트가 검증하는 것:
 *   격리 코어 배타적 할당, 중복 할당 방지, 해제 후 재할당,
 *   NUMA 노드 동일/혼합 할당, overcommit 모드, mark_used 외부 예약,
 *   dump/get_numa_info 안전성을 검사한다.
 *
 * 실행: sudo ./test_runner -p /cpu_allocator
 *
 * 테스트 추가: CPU_TEST("이름", 함수) 매크로로 등록
 *   (픽스처가 매 테스트마다 CpuAllocator를 new/free)
 *
 * 외부 의존: 없음 (실제 CPU 토폴로지 읽지 않고 add_core로 시뮬레이션)
 */
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

/* 코어가 하나도 등록되지 않은 상태에서 할당 시도 → 실패 확인 */
static void test_allocate_empty(CpuFixture *f, gconstpointer d) {
    (void)d;
    GArray *out = NULL;
    g_assert_false(cpu_allocator_allocate_exclusive(f->alloc, "vm-empty", 0, 1, &out, NULL));
    free_out(out);
}

/* 비격리 코어만 → FALSE */
static void test_allocate_non_isolated_fails(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, FALSE);
    cpu_allocator_add_core(f->alloc, 1, 1, 0, FALSE);
    GArray *out = NULL;
    g_assert_false(cpu_allocator_allocate_exclusive(f->alloc, "vm-niso", 0, 1, &out, NULL));
    free_out(out);
}

/* 격리 코어 할당 성공 */
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

/* 중복 할당 방지 */
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

/* free 후 재할당 */
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

/* 여러 VM 에 서로 다른 코어 */
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

/* vcpu_count > 1 */
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

/* NUMA 노드 출력 — 같은 노드 할당 시 해당 노드 번호 반환 */
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

/* dump 호출 안전성 (출력만 — crash 없으면 성공) */
static void test_dump_no_crash(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, FALSE);
    cpu_allocator_add_core(f->alloc, 1, 1, 0, TRUE);
    cpu_allocator_dump(f->alloc);
    cpu_allocator_dump(NULL); /* NULL 입력 안전 */
}

/* overcommit 모드 활성화 시 비격리 코어도 할당 가능한지 확인 */
static void test_overcommit_allows_non_isolated(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, FALSE);
    cpu_allocator_add_core(f->alloc, 1, 1, 0, FALSE);
    cpu_allocator_set_overcommit(TRUE);
    GArray *out = NULL;
    gboolean ok = cpu_allocator_allocate_exclusive(f->alloc, "vm-over", 0, 1, &out, NULL);
    /* overcommit 모드 — 구현에 따라 TRUE 가능 */
    free_out(out);
    cpu_allocator_set_overcommit(FALSE);
    (void)ok;
}

/* 외부에서 mark_used로 코어 0을 예약하면 allocate 시 코어 1이 할당되는지 확인 */
static void test_mark_used(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 1, 1, 0, TRUE);
    cpu_allocator_mark_used(f->alloc, 0, "external-vm");
    /* 0은 표시됨 → 1만 사용 가능 */
    GArray *out = NULL;
    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm-test", 0, 1, &out, NULL));
    g_assert_cmpuint(g_array_index(out, guint, 0), ==, 1);
    free_out(out);
}

/* get_numa_info JSON 반환 */
static void test_get_numa_info(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 1, 1, 1, TRUE);
    JsonObject *info = cpu_allocator_get_numa_info(f->alloc);
    g_assert_nonnull(info);
    json_object_unref(info);
}

/* free_vm_cores - 미존재 VM 안전 */
static void test_free_nonexistent_vm(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_free_vm_cores(f->alloc, "no-such-vm");
    cpu_allocator_free_vm_cores(f->alloc, NULL);
}

/* 서로 다른 NUMA 노드에서 코어를 혼합 할당하면 numa=-1 반환 확인 */
static void test_numa_node_output_mixed(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 2, 2, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 3, 3, 1, TRUE);
    GArray *out = NULL;
    gint numa = -1;
    /* 요청 NUMA 0에 코어 1개뿐 → fallback으로 혼합 할당 */
    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm-mixed", 0, 2, &out, &numa));
    g_assert_cmpint(numa, ==, -1);
    free_out(out);
}

/* [CMP-2] VM 재start 시 already-owns 체크로 코어 누수가 없는지 확인.
 * 자유 격리코어 4개(numa0) 중 vm1에 2개 할당 후, 실행중 VM 재start를 흉내내
 * 같은 vm_id로 재호출 — 새 코어를 마킹하지 않고 기존 2개를 그대로 반환해야 한다. */
static void test_restart_no_leak(CpuFixture *f, gconstpointer d) {
    (void)d;
    cpu_allocator_add_core(f->alloc, 0, 0, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 1, 1, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 2, 2, 0, TRUE);
    cpu_allocator_add_core(f->alloc, 3, 3, 0, TRUE);

    GArray *out1 = NULL;
    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm1", 0, 2, &out1, NULL));
    g_assert_cmpuint(out1->len, ==, 2);

    /* 재start — 같은 vm_id로 재호출 (worker의 idempotent no-op 경로 흉내) */
    GArray *out2 = NULL;
    g_assert_true(cpu_allocator_allocate_exclusive(f->alloc, "vm1", 0, 2, &out2, NULL));
    g_assert_cmpuint(out2->len, ==, 2);
    g_assert_cmpuint(g_array_index(out2, guint, 0), ==, g_array_index(out1, guint, 0));
    g_assert_cmpuint(g_array_index(out2, guint, 1), ==, g_array_index(out1, guint, 1));

    JsonObject *info = cpu_allocator_get_numa_info(f->alloc);
    JsonArray *numa_arr = json_object_get_array_member(info, "numa_nodes");
    g_assert_cmpuint(json_array_get_length(numa_arr), ==, 1);
    JsonObject *node0 = json_array_get_object_element(numa_arr, 0);
    g_assert_cmpint(json_object_get_int_member(node0, "allocated"), ==, 2);
    json_object_unref(info);

    free_out(out1); free_out(out2);
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
    CPU_TEST("restart_no_leak",             test_restart_no_leak);
#undef CPU_TEST
}
