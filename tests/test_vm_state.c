
#include <glib.h>
#include <glib/gstdio.h>
#include "../src/modules/core/vm_state.h"

static gchar *g_tmpdir = NULL;
static gchar *g_dbpath = NULL;

static void setup(void) {
    g_tmpdir = g_dir_make_tmp("pcv-vmstate-XXXXXX", NULL);
    g_dbpath = g_build_filename(g_tmpdir, "vm_state.db", NULL);
    g_setenv("PCV_VM_STATE_DB_PATH", g_dbpath, TRUE);
    init_pending_state_machine();
}
static void teardown(void) {
    shutdown_pending_state_machine();
    g_unsetenv("PCV_VM_STATE_DB_PATH");
    if (g_dbpath) {
        gchar *wal = g_strconcat(g_dbpath, "-wal", NULL);
        gchar *shm = g_strconcat(g_dbpath, "-shm", NULL);
        g_unlink(g_dbpath); g_unlink(wal); g_unlink(shm);
        g_free(wal); g_free(shm); g_free(g_dbpath); g_dbpath = NULL;
    }
    if (g_tmpdir) { g_rmdir(g_tmpdir); g_free(g_tmpdir); g_tmpdir = NULL; }
}

static void test_init_shutdown(void) {
    setup();
    teardown();
}

static void test_lock_unlock_basic(void) {
    setup();
    gchar *err = NULL;
    g_assert_true(lock_vm_operation("vm-a", VM_OP_STARTING, &err));
    g_assert_null(err);
    unlock_vm_operation("vm-a");
    teardown();
}

static void test_lock_conflict(void) {
    setup();
    gchar *err = NULL;
    g_assert_true(lock_vm_operation("vm-b", VM_OP_STARTING, &err));

    g_assert_false(lock_vm_operation("vm-b", VM_OP_STOPPING, &err));
    g_assert_nonnull(err);
    g_free(err);
    unlock_vm_operation("vm-b");
    teardown();
}

static void test_lock_after_unlock(void) {
    setup();
    gchar *err = NULL;
    g_assert_true(lock_vm_operation("vm-c", VM_OP_CREATING, &err));
    unlock_vm_operation("vm-c");

    g_assert_true(lock_vm_operation("vm-c", VM_OP_DELETING, &err));
    unlock_vm_operation("vm-c");
    teardown();
}

static void test_unlock_idempotent(void) {
    setup();

    unlock_vm_operation("vm-nonexist");
    unlock_vm_operation(NULL);
    teardown();
}

static void test_multiple_vms_independent(void) {
    setup();
    gchar *err = NULL;
    g_assert_true(lock_vm_operation("vm-1", VM_OP_STARTING, &err));
    g_assert_true(lock_vm_operation("vm-2", VM_OP_STARTING, &err));
    g_assert_true(lock_vm_operation("vm-3", VM_OP_SNAPSHOT, &err));
    g_assert_false(lock_vm_operation("vm-1", VM_OP_TUNING, &err));
    g_free(err);
    unlock_vm_operation("vm-1");
    unlock_vm_operation("vm-2");
    unlock_vm_operation("vm-3");
    teardown();
}

static void test_reconcile_orphan_lock(void) {

    g_tmpdir = g_dir_make_tmp("pcv-vmstate-recon-XXXXXX", NULL);
    g_dbpath = g_build_filename(g_tmpdir, "vm_state.db", NULL);

    gchar *cmd = g_strdup_printf(
        "sqlite3 %s 'CREATE TABLE vm_locks(vm_id TEXT PRIMARY KEY, op_type INTEGER, "
        "pid INTEGER, locked_at INTEGER); INSERT INTO vm_locks VALUES(\"orphan-vm\", 1, 999999, 0);'",
        g_dbpath);
    int rc = system(cmd);
    g_free(cmd);
    if (rc != 0) {

        g_unlink(g_dbpath); g_free(g_dbpath); g_dbpath = NULL;
        g_rmdir(g_tmpdir); g_free(g_tmpdir); g_tmpdir = NULL;
        g_test_skip("sqlite3 CLI 미설치");
        return;
    }

    g_setenv("PCV_VM_STATE_DB_PATH", g_dbpath, TRUE);
    init_pending_state_machine();

    gchar *err = NULL;
    g_assert_true(lock_vm_operation("orphan-vm", VM_OP_STARTING, &err));
    unlock_vm_operation("orphan-vm");

    teardown();
}

static volatile gint g_concurrent_acquired = 0;

static gpointer concurrent_lock_worker(gpointer data) {
    const gchar *vm_id = (const gchar *)data;
    gchar *err = NULL;
    if (lock_vm_operation(vm_id, VM_OP_STARTING, &err)) {
        g_atomic_int_inc(&g_concurrent_acquired);
        g_usleep(10000);
        unlock_vm_operation(vm_id);
    }
    if (err) g_free(err);
    return NULL;
}

static void test_concurrent_lock_same_vm(void) {
    setup();
    g_concurrent_acquired = 0;

    GThread *t[4];
    for (int i = 0; i < 4; i++)
        t[i] = g_thread_new("lock", concurrent_lock_worker, "vm-concurrent");
    for (int i = 0; i < 4; i++)
        g_thread_join(t[i]);

    g_assert_cmpint(g_atomic_int_get(&g_concurrent_acquired), >=, 1);
    g_assert_cmpint(g_atomic_int_get(&g_concurrent_acquired), <=, 4);
    teardown();
}

static void test_concurrent_lock_different_vms(void) {
    setup();
    g_concurrent_acquired = 0;
    GThread *t[4];
    const gchar *vms[] = {"vm-a", "vm-b", "vm-c", "vm-d"};
    for (int i = 0; i < 4; i++)
        t[i] = g_thread_new("lock", concurrent_lock_worker, (gpointer)vms[i]);
    for (int i = 0; i < 4; i++)
        g_thread_join(t[i]);

    g_assert_cmpint(g_atomic_int_get(&g_concurrent_acquired), ==, 4);
    teardown();
}

static void test_lock_conflict_error_msg(void) {
    setup();
    gchar *err = NULL;
    g_assert_true(lock_vm_operation("vm-msg", VM_OP_STARTING, &err));
    g_assert_false(lock_vm_operation("vm-msg", VM_OP_STOPPING, &err));
    g_assert_nonnull(err);

    g_assert_nonnull(g_strstr_len(err, -1, "vm-msg"));
    g_free(err);
    unlock_vm_operation("vm-msg");
    teardown();
}

static void test_all_op_types(void) {
    setup();
    gchar *err = NULL;
    int ops[] = {VM_OP_STARTING, VM_OP_STOPPING, VM_OP_DELETING,
                 VM_OP_CREATING, VM_OP_TUNING, VM_OP_SNAPSHOT, VM_OP_MIGRATING};
    for (size_t i = 0; i < G_N_ELEMENTS(ops); i++) {
        gchar *vm = g_strdup_printf("vm-op-%zu", i);
        g_assert_true(lock_vm_operation(vm, ops[i], &err));
        unlock_vm_operation(vm);
        g_free(vm);
    }
    teardown();
}

static void test_lock_renew_owned(void) {
    setup();
    gchar *err = NULL;
    g_assert_true(lock_vm_operation("vm-renew", VM_OP_SNAPSHOT, &err));

    g_assert_true(pcv_vm_lock_renew("vm-renew"));

    g_assert_false(lock_vm_operation("vm-renew", VM_OP_DELETING, &err));
    g_free(err);
    unlock_vm_operation("vm-renew");
    teardown();
}

static void test_lock_renew_absent(void) {
    setup();

    g_assert_false(pcv_vm_lock_renew("vm-absent"));
    g_assert_false(pcv_vm_lock_renew(NULL));
    teardown();
}

static void test_lock_renew_after_unlock(void) {
    setup();
    gchar *err = NULL;
    g_assert_true(lock_vm_operation("vm-ru", VM_OP_SNAPSHOT, &err));
    unlock_vm_operation("vm-ru");

    g_assert_false(pcv_vm_lock_renew("vm-ru"));
    teardown();
}

void test_vm_state_register(void) {
    g_test_add_func("/vm_state/init_shutdown", test_init_shutdown);
    g_test_add_func("/vm_state/lock_unlock_basic", test_lock_unlock_basic);
    g_test_add_func("/vm_state/lock_conflict", test_lock_conflict);
    g_test_add_func("/vm_state/lock_after_unlock", test_lock_after_unlock);
    g_test_add_func("/vm_state/unlock_idempotent", test_unlock_idempotent);
    g_test_add_func("/vm_state/multiple_vms_independent", test_multiple_vms_independent);
    g_test_add_func("/vm_state/all_op_types", test_all_op_types);
    g_test_add_func("/vm_state/reconcile_orphan_lock", test_reconcile_orphan_lock);
    g_test_add_func("/vm_state/lock_conflict_error_msg", test_lock_conflict_error_msg);
    g_test_add_func("/vm_state/concurrent_lock_same_vm", test_concurrent_lock_same_vm);
    g_test_add_func("/vm_state/concurrent_lock_different_vms", test_concurrent_lock_different_vms);
    g_test_add_func("/vm_state/lock_renew_owned", test_lock_renew_owned);
    g_test_add_func("/vm_state/lock_renew_absent", test_lock_renew_absent);
    g_test_add_func("/vm_state/lock_renew_after_unlock", test_lock_renew_after_unlock);
}
