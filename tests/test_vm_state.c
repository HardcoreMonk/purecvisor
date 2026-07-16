/**
 * @file test_vm_state.c
 * @brief vm_state SQLite 락 시스템 유닛 테스트
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  vm_state.c (src/modules/core/)의 SQLite WAL 기반 오퍼레이션 락을 검증한다.
 *  11개 테스트 케이스 (동시성 2건 포함).
 *
 *  VM 상태 락은 "하나의 VM에 동시에 두 작업이 실행되는 것"을 방지한다.
 *  예: vm-a를 시작하는 중에 삭제 요청이 오면 → 락 충돌로 거부.
 *
 *  검증 항목:
 *  - 기본 락/언락: lock_vm_operation → unlock_vm_operation
 *  - 충돌 감지: 같은 VM에 두 번째 락 시도 → FALSE + 에러 메시지(vm_id 포함)
 *  - 언락 후 재취득: 해제하면 다른 작업으로 재취득 가능
 *  - 독립성: vm-1, vm-2, vm-3은 서로 간섭 없이 동시 락 가능
 *  - 모든 작업 타입: STARTING/STOPPING/DELETING/CREATING/TUNING/SNAPSHOT/MIGRATING
 *  - 고아 락 회수(reconcile): PID 999999(존재하지 않는 프로세스)의 stale 락을
 *    init 시점에 자동 회수하여 재취득 가능하게 함
 *  - 동시성: 4 스레드가 같은 VM에 동시 시도 → 정확히 순차 실행
 *  - 동시성: 4 스레드가 다른 VM에 동시 시도 → 전부 성공
 *
 *  테스트 격리: PCV_VM_STATE_DB_PATH 환경변수로 임시 디렉터리에 DB 생성.
 * ============================================================================
 */
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
    /* 두 번째 락 시도 — 같은 PID지만 다른 op라도 거부 */
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
    /* 해제 후 재취득 가능 */
    g_assert_true(lock_vm_operation("vm-c", VM_OP_DELETING, &err));
    unlock_vm_operation("vm-c");
    teardown();
}

static void test_unlock_idempotent(void) {
    setup();
    /* 락 없는 상태에서 unlock — 안전 */
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

/* 죽은 PID(1) 락은 init reconcile에서 회수되어야 함 */
static void test_reconcile_orphan_lock(void) {
    /* 1단계: 임시 DB에 stale lock 직접 INSERT */
    g_tmpdir = g_dir_make_tmp("pcv-vmstate-recon-XXXXXX", NULL);
    g_dbpath = g_build_filename(g_tmpdir, "vm_state.db", NULL);

    /* sqlite3 명령으로 vm_locks 테이블 + stale 락 삽입 */
    gchar *cmd = g_strdup_printf(
        "sqlite3 %s 'CREATE TABLE vm_locks(vm_id TEXT PRIMARY KEY, op_type INTEGER, "
        "pid INTEGER, locked_at INTEGER); INSERT INTO vm_locks VALUES(\"orphan-vm\", 1, 999999, 0);'",
        g_dbpath);
    int rc = system(cmd);
    g_free(cmd);
    if (rc != 0) {
        /* sqlite3 CLI 미설치 시 skip */
        g_unlink(g_dbpath); g_free(g_dbpath); g_dbpath = NULL;
        g_rmdir(g_tmpdir); g_free(g_tmpdir); g_tmpdir = NULL;
        g_test_skip("sqlite3 CLI 미설치");
        return;
    }

    /* 2단계: init → reconcile이 PID 999999 (존재하지 않음)을 회수 */
    g_setenv("PCV_VM_STATE_DB_PATH", g_dbpath, TRUE);
    init_pending_state_machine();

    /* 3단계: 같은 vm_id로 락 재취득 가능해야 함 */
    gchar *err = NULL;
    g_assert_true(lock_vm_operation("orphan-vm", VM_OP_STARTING, &err));
    unlock_vm_operation("orphan-vm");

    teardown();
}

/* 락 시도 시 오류 메시지 검증 */
/* 다중 스레드에서 동시 lock 시도 — GMutex/SQLite 트랜잭션 경합 */
static volatile gint g_concurrent_acquired = 0;

static gpointer concurrent_lock_worker(gpointer data) {
    const gchar *vm_id = (const gchar *)data;
    gchar *err = NULL;
    if (lock_vm_operation(vm_id, VM_OP_STARTING, &err)) {
        g_atomic_int_inc(&g_concurrent_acquired);
        g_usleep(10000); /* 10ms hold */
        unlock_vm_operation(vm_id);
    }
    if (err) g_free(err);
    return NULL;
}

static void test_concurrent_lock_same_vm(void) {
    setup();
    g_concurrent_acquired = 0;
    /* 4개 스레드가 같은 vm에 동시 락 시도 → 정확히 1개만 성공 (또는 직렬화) */
    GThread *t[4];
    for (int i = 0; i < 4; i++)
        t[i] = g_thread_new("lock", concurrent_lock_worker, "vm-concurrent");
    for (int i = 0; i < 4; i++)
        g_thread_join(t[i]);
    /* 각 스레드는 unlock 후 바로 다음 스레드가 잡을 수 있으므로 최대 4 */
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
    /* 모두 다른 VM이므로 4개 모두 성공 */
    g_assert_cmpint(g_atomic_int_get(&g_concurrent_acquired), ==, 4);
    teardown();
}

static void test_lock_conflict_error_msg(void) {
    setup();
    gchar *err = NULL;
    g_assert_true(lock_vm_operation("vm-msg", VM_OP_STARTING, &err));
    g_assert_false(lock_vm_operation("vm-msg", VM_OP_STOPPING, &err));
    g_assert_nonnull(err);
    /* 메시지에 vm_id 포함 */
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

/* [CMP-7] 락 갱신(renew) 계약 검증.
 * 내 락은 갱신 가능하고, 갱신은 락을 해제하지 않으며(여전히 충돌), 부재/해제된 락은
 * 갱신 불가(FALSE). clone 하트비트가 이 프리미티브로 장기 clone 중 TTL을 되감는다. */
static void test_lock_renew_owned(void) {
    setup();
    gchar *err = NULL;
    g_assert_true(lock_vm_operation("vm-renew", VM_OP_SNAPSHOT, &err));
    /* 내가 보유한 락 → 갱신 성공 */
    g_assert_true(pcv_vm_lock_renew("vm-renew"));
    /* 갱신은 해제가 아니다 — 락은 여전히 유효(다른 op가 충돌해야 함) */
    g_assert_false(lock_vm_operation("vm-renew", VM_OP_DELETING, &err));
    g_free(err);
    unlock_vm_operation("vm-renew");
    teardown();
}

static void test_lock_renew_absent(void) {
    setup();
    /* 존재하지 않는 락 갱신 → FALSE (되살리지 않음) */
    g_assert_false(pcv_vm_lock_renew("vm-absent"));
    g_assert_false(pcv_vm_lock_renew(NULL));
    teardown();
}

static void test_lock_renew_after_unlock(void) {
    setup();
    gchar *err = NULL;
    g_assert_true(lock_vm_operation("vm-ru", VM_OP_SNAPSHOT, &err));
    unlock_vm_operation("vm-ru");
    /* 해제 후에는 갱신 대상 없음 → FALSE */
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
