                                                                                                                          
                                                                                                                           
                                                                           
                                                                                                          
                                                      
   
                                 
                                            
   
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "api/uds_server.h"
#include "modules/core/vm_state.h"
#include "modules/dispatcher/handler_snapshot.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/snapshot_fallback_lock.h"
#include "utils/pcv_spawn.h"

guint pcv_test_uds_response_count(void);
void pcv_test_uds_responses_reset(void);
const gchar *pcv_test_last_uds_response(void);
void pcv_test_audit_reset(void);
gint pcv_test_audit_call_count(void);

#define VM "fb-vm"
#define WAIT_DEADLINE_US ((gint64)5 * G_TIME_SPAN_SECOND)

static gchar *g_dir;
static gchar *g_db;
static GMutex g_clock_mu;
static gint64 g_clock_us;
static gint   g_renews;
static gboolean g_real_renew_ok;

                                                                            
static gboolean
_zfs_absent(gpointer data, const gchar * const *argv, gchar **out, gchar **err, GError **error)
{
    (void)data; (void)out; (void)err;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: dataset does not exist", argv[0]);
    return FALSE;
}

static gint64
_fake_now(void)
{
    g_mutex_lock(&g_clock_mu);
    gint64 v = g_clock_us;
    g_mutex_unlock(&g_clock_mu);
    return v;
}

static void
_clock_add_sec(gint64 sec)
{
    g_mutex_lock(&g_clock_mu);
    g_clock_us += sec * G_TIME_SPAN_SECOND;
    g_mutex_unlock(&g_clock_mu);
}

static gboolean
_count_renew(const PcvVmLock *lock)
{
    g_real_renew_ok = pcv_vm_lock_renew(lock);
    g_atomic_int_inc(&g_renews);
    return TRUE;
}

static void
_setup(void)
{
    g_dir = g_dir_make_tmp("pcv-snapfb-XXXXXX", NULL);
    g_db = g_build_filename(g_dir, "vm_state.db", NULL);
    g_setenv("PCV_VM_STATE_DB_PATH", g_db, TRUE);
    init_pending_state_machine();
    pcv_test_uds_responses_reset();
    pcv_test_audit_reset();
    pcv_spawn_set_thread_override(_zfs_absent, NULL);
}

static void
_teardown(void)
{
    pcv_spawn_set_thread_override(NULL, NULL);
    pcv_snapshot_fallback_lock_set_seam_for_test(NULL, NULL);
    shutdown_pending_state_machine();
    g_unsetenv("PCV_VM_STATE_DB_PATH");
    gchar *wal = g_strconcat(g_db, "-wal", NULL), *shm = g_strconcat(g_db, "-shm", NULL);
    g_unlink(g_db); g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm);
    g_rmdir(g_dir);
    g_clear_pointer(&g_db, g_free);
    g_clear_pointer(&g_dir, g_free);
}

                                                        
static PcvRpcCompletionResult
_call(void (*handler)(JsonObject *, const gchar *, UdsServer *, GSocketConnection *))
{
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", VM);
    json_object_set_string_member(params, "snapshot_name", "s1");
    GObject *server = g_object_new(G_TYPE_OBJECT, NULL), *conn = g_object_new(G_TYPE_OBJECT, NULL);
    pcv_rpc_completion_begin("vm.snapshot.create");
    handler(params, "id-1", (UdsServer *)server, (GSocketConnection *)conn);
    PcvRpcCompletionResult r = pcv_rpc_completion_finish();
    g_object_unref(server);
    g_object_unref(conn);
    json_object_unref(params);
    return r;
}

                                                                                      
static void
_assert_busy(void (*handler)(JsonObject *, const gchar *, UdsServer *, GSocketConnection *))
{
    _setup();
    gchar *err = NULL;
    PcvVmLock *held = pcv_vm_lock_acquire(VM, VM_OP_TUNING, &err);
    g_assert_nonnull(held);
    PcvRpcCompletionResult r = _call(handler);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 1);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "-32004"));
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "already locked"));
    g_assert_true(r.response_observed);
    g_assert_false(r.success);
    g_assert_cmpint(r.error_code, ==, PURE_RPC_ERR_BUSY);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 0);
    g_assert_true(pcv_rpc_completion_should_audit(r, TRUE));                                    
    pcv_vm_lock_release(held);
    _teardown();
}

static void test_create_busy(void) { _assert_busy(handle_vm_snapshot_create); }
static void test_delete_busy(void) { _assert_busy(handle_vm_snapshot_delete); }

                                                           
static void
_assert_released(void (*handler)(JsonObject *, const gchar *, UdsServer *, GSocketConnection *))
{
    _setup();
    PcvRpcCompletionResult r = _call(handler);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "Hypervisor connection failed"));
    g_assert_false(r.success);
    gchar *err = NULL;
    PcvVmLock *again = pcv_vm_lock_acquire(VM, VM_OP_SNAPSHOT, &err);
    g_assert_nonnull(again);
    pcv_vm_lock_release(again);
    _teardown();
}

static void test_create_released(void) { _assert_released(handle_vm_snapshot_create); }
static void test_delete_released(void) { _assert_released(handle_vm_snapshot_delete); }

static void
_wait_renews(gint want)
{
    gint64 deadline = g_get_monotonic_time() + WAIT_DEADLINE_US;
    while (g_atomic_int_get(&g_renews) < want) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(5000);
    }
}

                                                  
static void
test_lock_renewed(void)
{
    _setup();
    g_clock_us = 0;
    _clock_add_sec(1000);
    g_atomic_int_set(&g_renews, 0);
    pcv_snapshot_fallback_lock_set_seam_for_test(_fake_now, _count_renew);
    gchar *err = NULL;
    PcvSnapshotFallbackLock *h = pcv_snapshot_fallback_lock_begin(VM, &err);
    g_assert_nonnull(h);
    g_usleep(100000);                          
    _clock_add_sec(59);
    g_usleep(400000);
    g_assert_cmpint(g_atomic_int_get(&g_renews), ==, 0);
    _clock_add_sec(1);
    _wait_renews(1);
    g_assert_true(g_real_renew_ok);                   
    _clock_add_sec(60);
    _wait_renews(2);
    gchar *e2 = NULL;
    PcvVmLock *other = pcv_vm_lock_acquire(VM, VM_OP_SNAPSHOT, &e2);
    g_assert_null(other);
    g_free(e2);
    pcv_snapshot_fallback_lock_end(h);
    gint n = g_atomic_int_get(&g_renews);
    _clock_add_sec(120);
    g_usleep(400000);
    g_assert_cmpint(g_atomic_int_get(&g_renews), ==, n);
    PcvVmLock *again = pcv_vm_lock_acquire(VM, VM_OP_SNAPSHOT, &e2);
    g_assert_nonnull(again);
    pcv_vm_lock_release(again);
    _teardown();
}

                                                                                                   
static void
_spin_main(gint ms)
{
    gint64 end = g_get_monotonic_time() + (gint64)ms * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < end) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(2000);
    }
}

                                                                                       
static void
test_keeper_renews_on_main_context(void)
{
    _setup();
    g_atomic_int_set(&g_renews, 0);
    pcv_snapshot_lock_keeper_set_seam_for_test(50, _count_renew);
    gchar *err = NULL;
    PcvVmLock *lock = pcv_vm_lock_acquire(VM, VM_OP_SNAPSHOT, &err);
    g_assert_nonnull(lock);
    PcvSnapshotLockKeeper *k = pcv_snapshot_lock_keeper_start(lock);
    g_assert_nonnull(k);
    _spin_main(260);
    g_assert_cmpint(g_atomic_int_get(&g_renews), >=, 4);
    g_assert_true(g_real_renew_ok);
    pcv_snapshot_lock_keeper_stop_and_release(k);
    pcv_snapshot_lock_keeper_set_seam_for_test(0, NULL);
    _teardown();
}

                                                         
static void
test_keeper_stop_releases_without_renew(void)
{
    _setup();
    g_atomic_int_set(&g_renews, 0);
    pcv_snapshot_lock_keeper_set_seam_for_test(50, _count_renew);
    gchar *err = NULL;
    PcvVmLock *lock = pcv_vm_lock_acquire(VM, VM_OP_SNAPSHOT, &err);
    g_assert_nonnull(lock);
    PcvSnapshotLockKeeper *k = pcv_snapshot_lock_keeper_start(lock);
    g_assert_nonnull(k);
    _spin_main(120);
    pcv_snapshot_lock_keeper_stop_and_release(k);
    gint n = g_atomic_int_get(&g_renews);
    _spin_main(200);
    g_assert_cmpint(g_atomic_int_get(&g_renews), ==, n);
    PcvVmLock *again = pcv_vm_lock_acquire(VM, VM_OP_SNAPSHOT, &err);
    g_assert_nonnull(again);
    pcv_vm_lock_release(again);
    pcv_snapshot_lock_keeper_set_seam_for_test(0, NULL);
    _teardown();
}

                                               
static void
test_keeper_start_null_lock(void)
{
    g_assert_null(pcv_snapshot_lock_keeper_start(NULL));
    pcv_snapshot_lock_keeper_stop_and_release(NULL);
}

                                                                                                            
static gboolean
_zfs_err_code(gpointer data, const gchar * const *argv, gchar **out, gchar **err, GError **error)
{
    (void)out; (void)err;
    g_set_error(error, G_IO_ERROR, GPOINTER_TO_INT(data), "%s: probe", argv[0]);
    return FALSE;
}

                                                                                            
                                                                                           
                                                                     
static void
_list_case(gint code, gboolean expect_unknown)
{
    _setup();
    pcv_spawn_set_thread_override(_zfs_err_code, GINT_TO_POINTER(code));
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", VM);
    GObject *server = g_object_new(G_TYPE_OBJECT, NULL), *conn = g_object_new(G_TYPE_OBJECT, NULL);
    pcv_rpc_completion_begin("vm.snapshot.list");
    handle_vm_snapshot_list(params, "id-1", (UdsServer *)server, (GSocketConnection *)conn);
    PcvRpcCompletionResult r = pcv_rpc_completion_finish();
    g_object_unref(server); g_object_unref(conn); json_object_unref(params);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 1);
    const gchar *resp = pcv_test_last_uds_response();
    if (expect_unknown) {
        g_assert_nonnull(strstr(resp, "-32000"));
        g_assert_nonnull(strstr(resp, "vm.snapshot.list: host command timed out"));
        g_assert_cmpint(pcv_test_audit_call_count(), ==, 0);
        g_assert_true(r.response_observed);
        g_assert_false(r.success);
        g_assert_cmpint(r.error_code, ==, PURE_RPC_ERR_ZFS_OPERATION);
        g_assert_true(pcv_rpc_completion_should_audit(r, TRUE));
    } else {
        g_assert_nonnull(strstr(resp, "Hypervisor connection failed"));
    }
    _teardown();
}
static void test_list_timed_out(void) { _list_case(G_IO_ERROR_TIMED_OUT, TRUE); }
static void test_list_busy(void) { _list_case(G_IO_ERROR_BUSY, TRUE); }
static void test_list_cancelled(void) { _list_case(G_IO_ERROR_CANCELLED, TRUE); }
                                                                             
static void test_list_failed_falls_back(void) { _list_case(G_IO_ERROR_FAILED, FALSE); }

void
test_snapshot_fallback_register(void)
{
    g_test_add_func("/snapshot/fallback_create_busy_when_locked", test_create_busy);
    g_test_add_func("/snapshot/fallback_delete_busy_when_locked", test_delete_busy);
    g_test_add_func("/snapshot/fallback_create_lock_released", test_create_released);
    g_test_add_func("/snapshot/fallback_delete_lock_released", test_delete_released);
    g_test_add_func("/snapshot/fallback_lock_renewed", test_lock_renewed);
    g_test_add_func("/snapshot/keeper_renews_on_main_context", test_keeper_renews_on_main_context);
    g_test_add_func("/snapshot/keeper_stop_releases_without_renew", test_keeper_stop_releases_without_renew);
    g_test_add_func("/snapshot/keeper_start_null_lock", test_keeper_start_null_lock);
    g_test_add_func("/snapshot/list_unknown_timed_out", test_list_timed_out);
    g_test_add_func("/snapshot/list_unknown_busy", test_list_busy);
    g_test_add_func("/snapshot/list_unknown_cancelled", test_list_cancelled);
    g_test_add_func("/snapshot/list_absent_falls_back", test_list_failed_falls_back);
}
