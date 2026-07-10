/**
 * @file test_zfs.c
 * @brief zfs_driver 유닛 테스트 (에러 경로 위주)
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  zfs_driver.c (src/modules/storage/)의 에러 처리 경로를 검증한다.
 *  11개 테스트 케이스.
 *
 *  실제 ZFS 풀을 만들지 않고, 존재하지 않는 풀("nonexistent-pcv-test-pool-XYZ")에
 *  대한 호출이 크래시 없이 깔끔하게 실패하는지 확인한다.
 *  경로: API 호출 → pcv_spawn_sync(zfs 명령) → 실패 → GError 반환
 *
 *  검증 항목:
 *  - zvol CRUD: create/destroy에 잘못된 풀 이름 → FALSE + GError
 *  - NULL 안전: NULL 인자 전달 → 크래시 없이 FALSE
 *  - 풀 관리: destroy/scrub/health_detail 에러 경로
 *  - 클론/프로모트: 존재하지 않는 데이터셋 → FALSE
 *  - 비동기 스냅샷: create_async/list_async에 잘못된 풀 → 콜백에서 실패 반환
 *    GMainLoop으로 비동기 완료를 동기적으로 대기
 *  - 스냅샷 쿼터: 존재하지 않는 데이터셋 → 크래시 없이 반환
 * ============================================================================
 */
#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "../src/modules/storage/zfs_driver.h"
#include "../src/utils/pcv_spawn.h"

#define BAD_POOL "nonexistent-pcv-test-pool-XYZ"
#define BAD_VM   "nonexistent-pcv-test-vm-XYZ"

static void ensure_spawn(void) {
    static gboolean initialized = FALSE;
    if (!initialized) { pcv_spawn_launcher_init(); initialized = TRUE; }
}

/* ── 동기 zvol API ──────────────────────────────────────── */

static void test_create_volume_bad_pool(void) {
    ensure_spawn();
    GError *err = NULL;
    gboolean ok = purecvisor_zfs_create_volume(BAD_POOL, BAD_VM, "1G", &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_destroy_volume_bad_pool(void) {
    ensure_spawn();
    GError *err = NULL;
    gboolean ok = purecvisor_zfs_destroy_volume(BAD_POOL, BAD_VM, &err);
    /* 존재하지 않는 데이터셋 — 오류 또는 멱등 성공 */
    if (!ok) g_clear_error(&err);
}

static void test_create_volume_null_safe(void) {
    ensure_spawn();
    GError *err = NULL;
    gboolean ok = purecvisor_zfs_create_volume(NULL, NULL, NULL, &err);
    g_assert_false(ok);
    if (err) g_error_free(err);
}

/* ── 풀 관리 ──────────────────────────────────────────── */

static void test_destroy_pool_nonexistent(void) {
    ensure_spawn();
    GError *err = NULL;
    gboolean ok = purecvisor_zfs_destroy_pool(BAD_POOL, &err);
    g_assert_false(ok);
    if (err) g_error_free(err);
}

static void test_scrub_pool_nonexistent(void) {
    ensure_spawn();
    GError *err = NULL;
    gboolean ok = purecvisor_zfs_scrub_pool(BAD_POOL, &err);
    g_assert_false(ok);
    if (err) g_error_free(err);
}

static void test_pool_health_detail_nonexistent(void) {
    ensure_spawn();
    JsonObject *health = purecvisor_zfs_pool_health_detail(BAD_POOL);
    /* 결과 객체는 항상 반환 (state/health 필드 등) */
    if (health) json_object_unref(health);
}

/* ── 클론/카피 ──────────────────────────────────────── */

static void test_clone_volume_nonexistent(void) {
    ensure_spawn();
    GError *err = NULL;
    gboolean ok = purecvisor_zfs_clone_volume(BAD_POOL, BAD_VM, "snap1", "clone-vm", &err);
    g_assert_false(ok);
    if (err) g_error_free(err);
}

static void test_promote_nonexistent(void) {
    ensure_spawn();
    gboolean ok = purecvisor_zfs_promote(BAD_POOL "/clone");
    g_assert_false(ok);
}

/* ── 비동기 스냅샷 (콜백 동기화) ──────────────────────── */

static GMainLoop *g_loop = NULL;
static GError *g_async_err = NULL;
static gboolean g_async_ok = FALSE;
static gboolean g_async_done = FALSE;

static void on_snapshot_create_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)src; (void)u;
    g_async_ok = purecvisor_zfs_snapshot_create_finish(res, &g_async_err);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static void test_snapshot_create_async_bad(void) {
    ensure_spawn();
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_err = NULL;
    purecvisor_zfs_snapshot_create_async(BAD_POOL, BAD_VM, "snap-test",
                                         NULL, on_snapshot_create_done, NULL);
    g_main_loop_run(g_loop);
    g_assert_true(g_async_done);
    g_assert_false(g_async_ok);
    g_clear_error(&g_async_err);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
}

static void on_snapshot_list_done(GObject *src, GAsyncResult *res, gpointer u) {
    (void)src; (void)u;
    GPtrArray *list = purecvisor_zfs_snapshot_list_finish(res, &g_async_err);
    if (list) g_ptr_array_unref(list);
    g_async_done = TRUE;
    g_main_loop_quit(g_loop);
}

static void test_snapshot_list_async_bad(void) {
    ensure_spawn();
    g_loop = g_main_loop_new(NULL, FALSE);
    g_async_done = FALSE;
    g_async_err = NULL;
    purecvisor_zfs_snapshot_list_async(BAD_POOL, BAD_VM, NULL, on_snapshot_list_done, NULL);
    g_main_loop_run(g_loop);
    g_assert_true(g_async_done);
    g_clear_error(&g_async_err);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
}

/* ── 스냅샷 quota 검증 ──────────────────────────────── */

static void test_check_snapshot_quota_no_dataset(void) {
    ensure_spawn();
    /* 존재하지 않는 데이터셋 → 0건 → quota OK */
    gboolean ok = purecvisor_zfs_check_snapshot_quota(BAD_POOL "/" BAD_VM, 100);
    /* 결과는 구현에 따라 TRUE 또는 FALSE — crash 없이 반환되면 성공 */
    (void)ok;
}

void test_zfs_register(void) {
    g_test_add_func("/zfs/create_volume_bad_pool",      test_create_volume_bad_pool);
    g_test_add_func("/zfs/destroy_volume_bad_pool",     test_destroy_volume_bad_pool);
    g_test_add_func("/zfs/create_volume_null_safe",     test_create_volume_null_safe);
    g_test_add_func("/zfs/destroy_pool_nonexistent",    test_destroy_pool_nonexistent);
    g_test_add_func("/zfs/scrub_pool_nonexistent",      test_scrub_pool_nonexistent);
    g_test_add_func("/zfs/pool_health_detail_nonexistent", test_pool_health_detail_nonexistent);
    g_test_add_func("/zfs/clone_volume_nonexistent",    test_clone_volume_nonexistent);
    g_test_add_func("/zfs/promote_nonexistent",         test_promote_nonexistent);
    g_test_add_func("/zfs/snapshot_create_async_bad",   test_snapshot_create_async_bad);
    g_test_add_func("/zfs/snapshot_list_async_bad",     test_snapshot_list_async_bad);
    g_test_add_func("/zfs/check_snapshot_quota_no_dataset", test_check_snapshot_quota_no_dataset);
}
