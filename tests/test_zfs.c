
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

    if (!ok) g_clear_error(&err);
}

static void test_create_volume_null_safe(void) {
    ensure_spawn();
    GError *err = NULL;
    gboolean ok = purecvisor_zfs_create_volume(NULL, NULL, NULL, &err);
    g_assert_false(ok);
    if (err) g_error_free(err);
}

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

    if (health) json_object_unref(health);
}

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

static void test_check_snapshot_quota_no_dataset(void) {
    ensure_spawn();

    gboolean ok = purecvisor_zfs_check_snapshot_quota(BAD_POOL "/" BAD_VM, 100);

    (void)ok;
}

static void test_state_metric_val_mapping(void) {
    g_assert_cmpfloat(pcv_zfs_pool_state_metric_val("ONLINE"),    ==, 0.0);
    g_assert_cmpfloat(pcv_zfs_pool_state_metric_val("DEGRADED"),  ==, 1.0);
    g_assert_cmpfloat(pcv_zfs_pool_state_metric_val("FAULTED"),   ==, 2.0);
    g_assert_cmpfloat(pcv_zfs_pool_state_metric_val("UNAVAIL"),   ==, 3.0);

    g_assert_cmpfloat(pcv_zfs_pool_state_metric_val("SUSPENDED"), ==, 4.0);
    g_assert_cmpfloat(pcv_zfs_pool_state_metric_val("SUSPENDED"), >=, 2.0);

    g_assert_cmpfloat(pcv_zfs_pool_state_metric_val("UNKNOWN"),   ==, 0.0);
    g_assert_cmpfloat(pcv_zfs_pool_state_metric_val(NULL),        ==, 0.0);
}

static void test_recover_guard_window_limit(void) {
    ZfsRecoverGuard g = {0};
    const gint64 W = 3600LL * G_USEC_PER_SEC;
    const gint   MAX = 3;
    gint64 t0 = 1000000;

    g_assert_true(pcv_zfs_recover_guard_allow(&g, t0 + 0,          W, MAX));
    g_assert_true(pcv_zfs_recover_guard_allow(&g, t0 + 60000000,   W, MAX));
    g_assert_true(pcv_zfs_recover_guard_allow(&g, t0 + 120000000,  W, MAX));

    g_assert_false(pcv_zfs_recover_guard_allow(&g, t0 + 180000000, W, MAX));
    g_assert_false(pcv_zfs_recover_guard_allow(&g, t0 + 200000000, W, MAX));

    g_assert_true(pcv_zfs_recover_guard_allow(&g, t0 + W + 1,      W, MAX));
    g_assert_true(pcv_zfs_recover_guard_allow(&g, t0 + W + 2,      W, MAX));

    g_assert_false(pcv_zfs_recover_guard_allow(NULL, t0, W, MAX));
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
    g_test_add_func("/zfs/state_metric_val_mapping",    test_state_metric_val_mapping);
    g_test_add_func("/zfs/recover_guard_window_limit",  test_recover_guard_window_limit);
}
