                                                                                      
                                                                                   
                                                        
                                                             
                                  
   
                        
                                      
  
                                                                               
                 
                                                                               
                                                                
                            
  
                                              
                                          
  
          
                                                      
                                                        
                                   
                                             
                                                                             
                                                            
                                 
                                          
                                      
  
                                                      
                                                                               
   
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <unistd.h>
#include <string.h>
#include "../src/modules/core/vm_state.h"
#include "../src/modules/virt/virt_conn_pool.h"

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
    PcvVmLock *a = pcv_vm_lock_acquire("vm-a", VM_OP_STARTING, &err);
    g_assert_nonnull(a);
    g_assert_null(err);
    pcv_vm_lock_release(a);
    teardown();
}

static void test_lock_conflict(void) {
    setup();
    gchar *err = NULL;
    PcvVmLock *a = pcv_vm_lock_acquire("vm-b", VM_OP_STARTING, &err);
    g_assert_nonnull(a);
                                         
    g_assert_null(pcv_vm_lock_acquire("vm-b", VM_OP_STOPPING, &err));
    g_assert_nonnull(err);
    g_free(err);
    pcv_vm_lock_release(a);
    teardown();
}

static void test_lock_after_unlock(void) {
    setup();
    gchar *err = NULL;
    PcvVmLock *a = pcv_vm_lock_acquire("vm-c", VM_OP_CREATING, &err);
    g_assert_nonnull(a);
    pcv_vm_lock_release(a);
                     
    PcvVmLock *b = pcv_vm_lock_acquire("vm-c", VM_OP_DELETING, &err);
    g_assert_nonnull(b);
    pcv_vm_lock_release(b);
    teardown();
}

static void test_unlock_idempotent(void) {
    setup();
                               
    pcv_vm_lock_release(NULL);
    teardown();
}

static void test_multiple_vms_independent(void) {
    setup();
    gchar *err = NULL;
    PcvVmLock *l1 = pcv_vm_lock_acquire("vm-1", VM_OP_STARTING, &err);
    PcvVmLock *l2 = pcv_vm_lock_acquire("vm-2", VM_OP_STARTING, &err);
    PcvVmLock *l3 = pcv_vm_lock_acquire("vm-3", VM_OP_SNAPSHOT, &err);
    g_assert_nonnull(l1);
    g_assert_nonnull(l2);
    g_assert_nonnull(l3);
    g_assert_null(pcv_vm_lock_acquire("vm-1", VM_OP_TUNING, &err));
    g_free(err);
    pcv_vm_lock_release(l1);
    pcv_vm_lock_release(l2);
    pcv_vm_lock_release(l3);
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
    PcvVmLock *a = pcv_vm_lock_acquire("orphan-vm", VM_OP_STARTING, &err);
    g_assert_nonnull(a);
    pcv_vm_lock_release(a);

    teardown();
}

                      
                                                 
static volatile gint g_concurrent_acquired = 0;

static gpointer concurrent_lock_worker(gpointer data) {
    const gchar *vm_id = (const gchar *)data;
    gchar *err = NULL;
    PcvVmLock *lock = pcv_vm_lock_acquire(vm_id, VM_OP_STARTING, &err);
    if (lock) {
        g_atomic_int_inc(&g_concurrent_acquired);
        g_usleep(10000);                
        pcv_vm_lock_release(lock);
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
    PcvVmLock *a = pcv_vm_lock_acquire("vm-msg", VM_OP_STARTING, &err);
    g_assert_nonnull(a);
    g_assert_null(pcv_vm_lock_acquire("vm-msg", VM_OP_STOPPING, &err));
    g_assert_nonnull(err);
                       
    g_assert_nonnull(g_strstr_len(err, -1, "vm-msg"));
    g_free(err);
    pcv_vm_lock_release(a);
    teardown();
}

static void test_all_op_types(void) {
    setup();
    gchar *err = NULL;
    int ops[] = {VM_OP_STARTING, VM_OP_STOPPING, VM_OP_DELETING,
                 VM_OP_CREATING, VM_OP_TUNING, VM_OP_SNAPSHOT, VM_OP_MIGRATING};
    for (size_t i = 0; i < G_N_ELEMENTS(ops); i++) {
        gchar *vm = g_strdup_printf("vm-op-%zu", i);
        PcvVmLock *l = pcv_vm_lock_acquire(vm, ops[i], &err);
        g_assert_nonnull(l);
        pcv_vm_lock_release(l);
        g_free(vm);
    }
    teardown();
}

static void _exec_on_db(const gchar *sql) {
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(g_dbpath, &db), ==, SQLITE_OK);
    sqlite3_busy_timeout(db, 5000);
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) g_test_message("sqlite exec failed: %s", errmsg ? errmsg : "?");
    sqlite3_free(errmsg);
    g_assert_cmpint(rc, ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_changes(db), ==, 1);
    sqlite3_close(db);
}

                             
                                                   
                                                              
static void test_lock_renew_owned(void) {
    setup();
    gchar *err = NULL;
    PcvVmLock *a = pcv_vm_lock_acquire("vm-renew", VM_OP_SNAPSHOT, &err);
    g_assert_nonnull(a);
                          
    g_assert_true(pcv_vm_lock_renew(a));
                                                
    g_assert_null(pcv_vm_lock_acquire("vm-renew", VM_OP_DELETING, &err));
    g_free(err);
    pcv_vm_lock_release(a);
    teardown();
}

static void test_lock_renew_absent(void) {
    setup();
                                        
    g_assert_false(pcv_vm_lock_renew(NULL));
    teardown();
}

static void test_lock_renew_after_unlock(void) {
    setup();
    gchar *err = NULL;
    PcvVmLock *a = pcv_vm_lock_acquire("vm-ru", VM_OP_SNAPSHOT, &err);
    g_assert_nonnull(a);
                                 
    _exec_on_db("UPDATE vm_locks SET locked_at = 1 WHERE vm_id = 'vm-ru';");
    PcvVmLock *b = pcv_vm_lock_acquire("vm-ru", VM_OP_TUNING, &err);
    g_assert_nonnull(b);
    pcv_vm_lock_release(b);
    g_assert_false(pcv_vm_lock_renew(a));
    pcv_vm_lock_release(a);
    teardown();
}

static void test_death_release_truth_table(void) {
    static const struct {
        PcvVmRuntimeState state;
        gboolean release_when_idle;
    } rows[] = {
        { PCV_VM_UNKNOWN,   TRUE  },
        { PCV_VM_RUNNING,   FALSE },
        { PCV_VM_STOPPED,   TRUE  },
        { PCV_VM_PAUSED,    FALSE },
        { PCV_VM_MIGRATING, FALSE },
        { PCV_VM_ERROR,     TRUE  },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(rows); i++) {
        g_assert_cmpint(pcv_vm_death_should_release_cores(rows[i].state, FALSE),
                        ==, rows[i].release_when_idle);

        g_assert_false(pcv_vm_death_should_release_cores(rows[i].state, TRUE));
    }
}

static void test_start_in_progress(void) {
    setup();
    gchar *err = NULL;
    const gchar *vm = "vm-sip";

    g_assert_false(pcv_vm_state_start_in_progress(vm));
    g_assert_false(pcv_vm_state_start_in_progress(NULL));
    g_assert_false(pcv_vm_state_start_in_progress(""));

    PcvVmLock *l = pcv_vm_lock_acquire(vm, VM_OP_STARTING, &err);
    g_assert_nonnull(l);
    g_assert_true(pcv_vm_state_start_in_progress(vm));
    g_assert_null(pcv_vm_lock_acquire(vm, VM_OP_STOPPING, &err));
    g_free(err); err = NULL;
    g_assert_true(pcv_vm_state_start_in_progress(vm));
    g_assert_false(pcv_vm_state_start_in_progress("vm-other"));

    pcv_vm_lock_release(l);
    g_assert_false(pcv_vm_state_start_in_progress(vm));

    l = pcv_vm_lock_acquire(vm, VM_OP_STOPPING, &err);
    g_assert_nonnull(l);
    g_assert_false(pcv_vm_state_start_in_progress(vm));
    pcv_vm_lock_release(l);

    l = pcv_vm_lock_acquire(vm, VM_OP_STARTING, &err);
    g_assert_nonnull(l);
    _exec_on_db("UPDATE vm_locks SET locked_at = CAST(strftime('%s','now') AS INTEGER) - 120 "
                "WHERE vm_id = 'vm-sip';");
    g_assert_false(pcv_vm_state_start_in_progress(vm));
    PcvVmLock *thief = pcv_vm_lock_acquire(vm, VM_OP_STOPPING, &err);
    g_assert_nonnull(thief);
    pcv_vm_lock_release(thief);
    pcv_vm_lock_release(l);

    l = pcv_vm_lock_acquire(vm, VM_OP_STARTING, &err);
    g_assert_nonnull(l);
    _exec_on_db("UPDATE vm_locks SET pid = 0 WHERE vm_id = 'vm-sip';");
    g_assert_false(pcv_vm_state_start_in_progress(vm));
    pcv_vm_lock_release(l);

    g_assert_null(err);
    teardown();

    g_assert_false(pcv_vm_state_start_in_progress(vm));
}

static void test_runtime_nowait_pool_busy(void) {
    g_setenv("PCV_LIBVIRT_URI", "test:///default", TRUE);
    virt_conn_pool_init(1);

    virConnectPtr held = virt_conn_pool_acquire();
    if (!held) {
        virt_conn_pool_shutdown();
        g_unsetenv("PCV_LIBVIRT_URI");
        g_test_skip("test:///default 연결 불가");
        return;
    }

    gint64 t0 = g_get_monotonic_time();
    PcvVmRuntimeState busy = pcv_vm_state_get_runtime_nowait("test");
    gint64 waited_us = g_get_monotonic_time() - t0;
    g_assert_cmpint(busy, ==, PCV_VM_UNKNOWN);
    g_assert_cmpint(waited_us, <, 2 * G_USEC_PER_SEC);
    g_assert_true(pcv_vm_death_should_release_cores(busy, FALSE));

    virt_conn_pool_release(held);

    g_assert_cmpint(pcv_vm_state_get_runtime_nowait("test"), ==, PCV_VM_RUNNING);
    g_assert_cmpint(pcv_vm_state_get_runtime("test"), ==, PCV_VM_RUNNING);
    g_assert_cmpint(pcv_vm_state_get_runtime_nowait("pcv-no-such-vm"), ==,
                    pcv_vm_state_get_runtime("pcv-no-such-vm"));
    g_assert_cmpint(pcv_vm_state_get_runtime_nowait(NULL), ==, PCV_VM_UNKNOWN);

    virt_conn_pool_shutdown();
    g_unsetenv("PCV_LIBVIRT_URI");
}

static void db_exec(const gchar *sql)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(g_dbpath, &db), ==, SQLITE_OK);
    sqlite3_busy_timeout(db, 5000);
    g_assert_cmpint(sqlite3_exec(db, sql, NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(db);
}

static gint db_count(const gchar *key)
{
    sqlite3 *db = NULL; sqlite3_stmt *st = NULL; gint n = -1;
    g_assert_cmpint(sqlite3_open(g_dbpath, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM vm_locks WHERE vm_id = ?;", -1, &st, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st); sqlite3_close(db);
    return n;
}

#define TEST_UUID "11111111-2222-3333-4444-555555555555"
static gboolean g_fake_resolves = TRUE;
static gboolean g_fake_unavailable = FALSE;
static gint g_fake_calls = 0;

static PcvVmKeyResolve fake_resolver(const gchar *vm_id, gchar **name_out)
{
    g_fake_calls++;
    if (g_fake_unavailable) return PCV_VM_KEY_UNAVAILABLE;
    GString *hex = g_string_new(NULL);
    for (const gchar *c = vm_id; *c; c++)
        if (g_ascii_isxdigit(*c)) g_string_append_c(hex, g_ascii_tolower(*c));
    gboolean same = g_fake_resolves && g_strcmp0(hex->str, "11111111222233334444555555555555") == 0;
    g_string_free(hex, TRUE);
    if (!same) return PCV_VM_KEY_NOT_FOUND;
    *name_out = g_strdup("vm-uuid-target");
    return PCV_VM_KEY_RESOLVED;
}

static void test_handle_acquire_release_basic(void) {
    setup();
    gchar *err = NULL;
    PcvVmLock *lk = pcv_vm_lock_acquire("vm-h1", VM_OP_TUNING, &err);
    g_assert_nonnull(lk); g_assert_null(err);
    g_assert_cmpstr(pcv_vm_lock_key(lk), ==, "vm-h1");
    g_assert_cmpint(db_count("vm-h1"), ==, 1);
    pcv_vm_lock_release(lk);
    g_assert_cmpint(db_count("vm-h1"), ==, 0);
    teardown();
}

static void test_handle_conflict_message_uses_key(void) {
    setup();
    gchar *err = NULL;
    PcvVmLock *lk = pcv_vm_lock_acquire("vm-h2", VM_OP_STARTING, &err);
    g_assert_nonnull(lk);
    g_assert_null(pcv_vm_lock_acquire("vm-h2", VM_OP_STOPPING, &err));
    g_assert_nonnull(err);
    g_assert_true(g_str_has_prefix(err, "VM 'vm-h2' is already locked (op: STARTING, pid: "));
    g_free(err);
    pcv_vm_lock_release(lk);
    teardown();
}

static void test_handle_release_null_is_noop(void) {
    pcv_vm_lock_release(NULL);
    g_assert_false(pcv_vm_lock_renew(NULL));
    g_assert_null(pcv_vm_lock_key(NULL));
}

static void test_handle_release_after_takeover_keeps_new_lock(void) {
    setup();
    gchar *err = NULL;
    PcvVmLock *a = pcv_vm_lock_acquire("vm-take", VM_OP_SNAPSHOT, &err);
    g_assert_nonnull(a);
    db_exec("UPDATE vm_locks SET locked_at = 1 WHERE vm_id = 'vm-take';");
    PcvVmLock *b = pcv_vm_lock_acquire("vm-take", VM_OP_DELETING, &err);
    g_assert_nonnull(b);
    pcv_vm_lock_release(a);
    g_assert_cmpint(db_count("vm-take"), ==, 1);
    g_assert_null(pcv_vm_lock_acquire("vm-take", VM_OP_TUNING, &err));
    g_clear_pointer(&err, g_free);
    pcv_vm_lock_release(b);
    g_assert_cmpint(db_count("vm-take"), ==, 0);
    teardown();
}

static void test_handle_renew_only_own(void) {
    setup();
    gchar *err = NULL;
    PcvVmLock *a = pcv_vm_lock_acquire("vm-rn", VM_OP_SNAPSHOT, &err);
    g_assert_true(pcv_vm_lock_renew(a));
    db_exec("UPDATE vm_locks SET locked_at = 1 WHERE vm_id = 'vm-rn';");
    PcvVmLock *b = pcv_vm_lock_acquire("vm-rn", VM_OP_TUNING, &err);
    g_assert_nonnull(b);
    g_assert_false(pcv_vm_lock_renew(a));
    g_assert_true(pcv_vm_lock_renew(b));
    pcv_vm_lock_release(a); pcv_vm_lock_release(b);
    teardown();
}

static void test_handle_uuid_and_name_exclusive(void) {
    setup();
    pcv_vm_state_set_key_resolver(fake_resolver); g_fake_resolves = TRUE;
    gchar *err = NULL;
    PcvVmLock *by_uuid = pcv_vm_lock_acquire(TEST_UUID, VM_OP_STARTING, &err);
    g_assert_nonnull(by_uuid);
    g_assert_cmpstr(pcv_vm_lock_key(by_uuid), ==, "vm-uuid-target");
    g_assert_null(pcv_vm_lock_acquire("vm-uuid-target", VM_OP_SNAPSHOT, &err));
    g_assert_true(g_str_has_prefix(err, "VM 'vm-uuid-target' is already locked"));
    g_clear_pointer(&err, g_free);
    g_assert_true(pcv_vm_state_start_in_progress("vm-uuid-target"));
    g_assert_true(pcv_vm_state_start_in_progress(TEST_UUID));
    pcv_vm_lock_release(by_uuid);
    pcv_vm_state_set_key_resolver(NULL);
    teardown();
}

static void test_handle_resolver_failure_falls_back(void) {
    setup();
    pcv_vm_state_set_key_resolver(fake_resolver); g_fake_resolves = FALSE;
    gchar *err = NULL;
    PcvVmLock *lk = pcv_vm_lock_acquire(TEST_UUID, VM_OP_TUNING, &err);
    g_assert_nonnull(lk);
    g_assert_cmpstr(pcv_vm_lock_key(lk), ==, TEST_UUID);
    pcv_vm_lock_release(lk);
    g_assert_cmpint(db_count(TEST_UUID), ==, 0);
    pcv_vm_state_set_key_resolver(NULL); g_fake_resolves = TRUE;
    teardown();
}

static void test_handle_release_after_resolver_stops_resolving(void) {
    setup();
    pcv_vm_state_set_key_resolver(fake_resolver); g_fake_resolves = TRUE;
    gchar *err = NULL;
    PcvVmLock *lk = pcv_vm_lock_acquire(TEST_UUID, VM_OP_DELETING, &err);
    g_assert_nonnull(lk);
    g_fake_resolves = FALSE;
    pcv_vm_lock_release(lk);
    g_assert_cmpint(db_count("vm-uuid-target"), ==, 0);
    pcv_vm_state_set_key_resolver(NULL); g_fake_resolves = TRUE;
    teardown();
}

static void test_handle_migrates_owner_column_and_takes_legacy_row(void) {
    g_tmpdir = g_dir_make_tmp("pcv-vmstate-mig-XXXXXX", NULL);
    g_dbpath = g_build_filename(g_tmpdir, "vm_state.db", NULL);
    db_exec("CREATE TABLE vm_locks(vm_id TEXT PRIMARY KEY, op_type INTEGER NOT NULL, pid INTEGER NOT NULL, "
            "locked_at INTEGER NOT NULL);");
    gchar *row = g_strdup_printf("INSERT INTO vm_locks VALUES('vm-legacy', %d, %d, 1);", VM_OP_SNAPSHOT, (gint)getpid());
    db_exec(row); g_free(row);
    g_setenv("PCV_VM_STATE_DB_PATH", g_dbpath, TRUE);
    init_pending_state_machine();
    db_exec("SELECT owner FROM vm_locks LIMIT 0;");
    gchar *err = NULL;
    PcvVmLock *lk = pcv_vm_lock_acquire("vm-legacy", VM_OP_TUNING, &err);
    g_assert_nonnull(lk);
    pcv_vm_lock_release(lk);
    g_assert_cmpint(db_count("vm-legacy"), ==, 0);
    teardown();
}

static void test_handle_no_db_is_unbacked(void) {
    setup();
    shutdown_pending_state_machine();
    gchar *err = NULL;
    PcvVmLock *lk = pcv_vm_lock_acquire("vm-nodb", VM_OP_TUNING, &err);
    g_assert_nonnull(lk);
    g_assert_false(pcv_vm_lock_renew(lk));
    pcv_vm_lock_release(lk);
    init_pending_state_machine();
    teardown();
}

static void test_handle_uuid_noncanonical_forms_normalized(void) {
    setup();
    pcv_vm_state_set_key_resolver(fake_resolver); g_fake_resolves = TRUE; g_fake_unavailable = FALSE;
    const gchar *forms[] = { "11111111222233334444555555555555",
                             "11111111-2222-3333-4444-555555555555",
                             "  11111111-2222-3333-4444-555555555555 " };
    for (guint i = 0; i < G_N_ELEMENTS(forms); i++) {
        gchar *err = NULL;
        PcvVmLock *lk = pcv_vm_lock_acquire(forms[i], VM_OP_STARTING, &err);
        g_assert_nonnull(lk);
        g_assert_cmpstr(pcv_vm_lock_key(lk), ==, "vm-uuid-target");
        g_assert_null(pcv_vm_lock_acquire("vm-uuid-target", VM_OP_SNAPSHOT, &err));
        g_clear_pointer(&err, g_free);
        pcv_vm_lock_release(lk);
    }
    pcv_vm_state_set_key_resolver(NULL);
    teardown();
}

static void test_handle_uuid_like_negative(void) {
    setup();
    pcv_vm_state_set_key_resolver(fake_resolver); g_fake_resolves = TRUE; g_fake_unavailable = FALSE;
    g_fake_calls = 0;
    const gchar *neg[] = { "vm-name",
                           "1111111122223333444455555555555",
                           "111111112222333344445555555555555",
                           "1111111122223333444455555555555g",
                           "1-1111112222333344445555555555555" };
    for (guint i = 0; i < G_N_ELEMENTS(neg); i++) {
        gchar *err = NULL;
        PcvVmLock *lk = pcv_vm_lock_acquire(neg[i], VM_OP_TUNING, &err);
        g_assert_nonnull(lk);
        g_assert_cmpstr(pcv_vm_lock_key(lk), ==, neg[i]);
        pcv_vm_lock_release(lk);
    }
    g_assert_cmpint(g_fake_calls, ==, 0);
    pcv_vm_state_set_key_resolver(NULL);
    teardown();
}

static void test_handle_resolver_unavailable_fails_closed(void) {
    setup();
    pcv_vm_state_set_key_resolver(fake_resolver); g_fake_resolves = TRUE; g_fake_unavailable = TRUE;
    gchar *err = NULL;
    g_assert_null(pcv_vm_lock_acquire(TEST_UUID, VM_OP_TUNING, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err, "could not be resolved"));
    g_free(err);
    g_assert_cmpint(db_count(TEST_UUID), ==, 0);
    g_assert_cmpint(db_count("vm-uuid-target"), ==, 0);
    g_assert_false(pcv_vm_state_start_in_progress(TEST_UUID));
    g_fake_unavailable = FALSE;
    pcv_vm_state_set_key_resolver(NULL);
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
    g_test_add_func("/vm_state/death_release_truth_table", test_death_release_truth_table);
    g_test_add_func("/vm_state/start_in_progress", test_start_in_progress);
    g_test_add_func("/vm_state/runtime_nowait_pool_busy", test_runtime_nowait_pool_busy);
    g_test_add_func("/vm_state/handle/acquire_release_basic", test_handle_acquire_release_basic);
    g_test_add_func("/vm_state/handle/conflict_message_uses_key", test_handle_conflict_message_uses_key);
    g_test_add_func("/vm_state/handle/release_null_is_noop", test_handle_release_null_is_noop);
    g_test_add_func("/vm_state/handle/release_after_takeover_keeps_new_lock",
                    test_handle_release_after_takeover_keeps_new_lock);
    g_test_add_func("/vm_state/handle/renew_only_own", test_handle_renew_only_own);
    g_test_add_func("/vm_state/handle/uuid_and_name_exclusive", test_handle_uuid_and_name_exclusive);
    g_test_add_func("/vm_state/handle/resolver_failure_falls_back", test_handle_resolver_failure_falls_back);
    g_test_add_func("/vm_state/handle/release_after_resolver_stops_resolving",
                    test_handle_release_after_resolver_stops_resolving);
    g_test_add_func("/vm_state/handle/migrates_owner_column_and_takes_legacy_row",
                    test_handle_migrates_owner_column_and_takes_legacy_row);
    g_test_add_func("/vm_state/handle/no_db_is_unbacked", test_handle_no_db_is_unbacked);
    g_test_add_func("/vm_state/handle/uuid_noncanonical_forms_normalized", test_handle_uuid_noncanonical_forms_normalized);
    g_test_add_func("/vm_state/handle/uuid_like_negative", test_handle_uuid_like_negative);
    g_test_add_func("/vm_state/handle/resolver_unavailable_fails_closed", test_handle_resolver_unavailable_fails_closed);
}
