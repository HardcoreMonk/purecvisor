                                                                                        
                                                                                                   
                                                                                              
                                                             
                                 
   
                           
                                                  
  
                  
                                                        
                                                                 
  
                 
                                                   
                                                           
   

#include "modules/audit/pcv_audit_chain.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <unistd.h>

static void
_exec_ok(sqlite3 *db, const gchar *sql)
{
    gchar *message = NULL;
    gint rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (rc != SQLITE_OK)
        g_test_message("sqlite rc=%d: %s", rc,
                       message ? message : sqlite3_errmsg(db));
    sqlite3_free(message);
    g_assert_cmpint(rc, ==, SQLITE_OK);
}

static sqlite3 *
_open_prepared_db(void)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(":memory:", &db), ==, SQLITE_OK);
    PcvAuditChainHealth health;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_prepare(db, &health, &error));
    g_assert_no_error(error);
    g_assert_true(health.current_ok);
    return db;
}

static gint64
_append(sqlite3 *db, const gchar *ts, const gchar *username,
        const gchar *method, const gchar *target, const gchar *result,
        gint error_code)
{
    PcvAuditChainRecord record = {
        .ts = ts,
        .node = "test-node",
        .username = username,
        .method = method,
        .target = target,
        .result = result,
        .error_code = error_code,
        .duration_ms = 7,
        .src_ip = "127.0.0.1",
        .event_ts = ts,
    };
    gint64 rowid = 0;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_append(db, &record, &rowid, &error));
    g_assert_no_error(error);
    g_assert_cmpint(rowid, >, 0);
    return rowid;
}

static PcvAuditChainHealth
_verify_ok(sqlite3 *db)
{
    PcvAuditChainHealth health;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_verify(db, &health, &error));
    g_assert_no_error(error);
    g_assert_true(health.current_ok);
    return health;
}

static PcvAuditChainHealth
_verify_broken(sqlite3 *db, gint64 expected_rowid)
{
    PcvAuditChainHealth health;
    GError *error = NULL;
    g_assert_false(pcv_audit_chain_verify(db, &health, &error));
    g_assert_no_error(error);
    g_assert_false(health.current_ok);
    g_assert_cmpint(health.first_break_rowid, ==, expected_rowid);
    return health;
}

static sqlite3 *
_build_active_chain(void)
{
    sqlite3 *db = _open_prepared_db();
    _append(db, "2026-07-16 00:00:01", "admin", "auth.login", "admin", "ok", 0);
    _append(db, "2026-07-16 00:00:02", "alice", "vm.create", "vm1", "ok", 0);
    _append(db, "2026-07-16 00:00:03", "bob", "vm.delete", "vm1", "fail", 403);
    return db;
}

static gint64
_scalar_int64(sqlite3 *db, const gchar *sql)
{
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
    gint64 value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static gchar *
_legacy_dump(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "SELECT group_concat(id||':'||COALESCE(prev_hash,'')||':'||"
        "COALESCE(rec_hash,''),'|') FROM audit_log", -1, &stmt, NULL),
        ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
    gchar *dump = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return dump;
}

static sqlite3 *
_open_legacy_db(void)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(":memory:", &db), ==, SQLITE_OK);
    _exec_ok(db,
        "CREATE TABLE audit_log("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,ts TEXT NOT NULL,node TEXT NOT NULL,"
        "username TEXT,method TEXT NOT NULL,target TEXT,result TEXT NOT NULL,"
        "error_code INTEGER,duration_ms INTEGER,src_ip TEXT,"
        "prev_hash TEXT,rec_hash TEXT)");
    return db;
}

static gchar *
_legacy_insert(sqlite3 *db, const gchar *prev_hash, const gchar *ts,
               const gchar *username, const gchar *method)
{
    GError *error = NULL;
    gchar *rec_hash = pcv_audit_chain_record_hash(
        prev_hash, ts, username, method, "legacy-target", "ok", 0, &error);
    g_assert_no_error(error);
    g_assert_nonnull(rec_hash);

    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "INSERT INTO audit_log(ts,node,username,method,target,result,error_code,"
        "duration_ms,src_ip,prev_hash,rec_hash) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        -1, &stmt, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, "legacy-node", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, method, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, "legacy-target", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, "ok", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 7, 0);
    sqlite3_bind_int64(stmt, 8, 3);
    sqlite3_bind_text(stmt, 9, "127.0.0.1", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, prev_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, rec_hash, -1, SQLITE_TRANSIENT);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_DONE);
    sqlite3_finalize(stmt);
    return rec_hash;
}

static sqlite3 *
_build_legacy_chain(gboolean broken)
{
    sqlite3 *db = _open_legacy_db();
    gchar *one = _legacy_insert(db, PCV_AUDIT_CHAIN_GENESIS,
                                "2026-06-01 00:00:01", "legacy-a", "one");
    gchar *two = _legacy_insert(db, one,
                                "2026-06-01 00:00:02", "legacy-b", "two");
    gchar *three = _legacy_insert(db, broken ? one : two,
                                  "2026-06-01 00:00:03", "legacy-c", "three");
    g_free(three);
    g_free(two);
    g_free(one);
    return db;
}

static void
test_audit_chain_intact_passes(void)
{
    sqlite3 *db = _build_active_chain();
    PcvAuditChainHealth health = _verify_ok(db);
    g_assert_false(health.historical_break);
    g_assert_cmpint(health.first_break_rowid, ==, 0);
    sqlite3_close(db);
}

static void
test_audit_chain_field_tamper_detected(void)
{
    sqlite3 *db = _build_active_chain();
    _exec_ok(db, "UPDATE audit_log SET result='denied' WHERE id=2");
    _verify_broken(db, 2);
    sqlite3_close(db);
}

static void
test_audit_chain_row_delete_detected(void)
{
    sqlite3 *db = _build_active_chain();
    _exec_ok(db, "DELETE FROM audit_log WHERE id=2");
    _verify_broken(db, 3);
    sqlite3_close(db);
}

static void
test_audit_chain_rechash_tamper_detected(void)
{
    sqlite3 *db = _build_active_chain();
    _exec_ok(db, "UPDATE audit_log SET rec_hash='deadbeef' WHERE id=2");
    _verify_broken(db, 2);
    sqlite3_close(db);
}

static void
test_audit_chain_null_rechash_is_not_skipped(void)
{
    sqlite3 *db = _build_active_chain();
    _exec_ok(db, "UPDATE audit_log SET rec_hash=NULL WHERE id=2");
    _verify_broken(db, 2);
    sqlite3_close(db);
}

static void
test_audit_chain_epoch_null_move_is_detected(void)
{
    sqlite3 *db = _build_active_chain();
    _exec_ok(db, "UPDATE audit_log SET chain_epoch=NULL WHERE id=2");
    PcvAuditChainHealth health = _verify_broken(db, 2);
    g_assert_cmpstr(health.reason, ==, "epoch_orphan");
    sqlite3_close(db);
}

static void
test_audit_chain_unknown_epoch_move_is_detected(void)
{
    sqlite3 *db = _build_active_chain();
    _exec_ok(db, "UPDATE audit_log SET chain_epoch=999 WHERE id=2");
    PcvAuditChainHealth health = _verify_broken(db, 2);
    g_assert_cmpstr(health.reason, ==, "epoch_orphan");
    sqlite3_close(db);
}

typedef struct {
    sqlite3 *db;
    GMutex *mutex;
    GCond *cond;
    guint *ready;
    gboolean *go;
    const gchar *username;
    gboolean result;
    GError *error;
} WriterCtx;

static gpointer
_writer_thread(gpointer data)
{
    WriterCtx *ctx = data;
    g_mutex_lock(ctx->mutex);
    (*ctx->ready)++;
    g_cond_broadcast(ctx->cond);
    while (!*ctx->go)
        g_cond_wait(ctx->cond, ctx->mutex);
    g_mutex_unlock(ctx->mutex);

    PcvAuditChainRecord record = {
        .ts = "2026-08-09 00:00:02",
        .node = "test-node",
        .username = ctx->username,
        .method = "anomaly_detected",
        .target = ctx->username,
        .result = "ok",
        .error_code = 0,
        .duration_ms = 1,
        .src_ip = "127.0.0.1",
        .event_ts = "2026-08-09 00:00:02",
    };
    ctx->result = pcv_audit_chain_append(ctx->db, &record, NULL, &ctx->error);
    return NULL;
}

static void
test_audit_chain_two_writers_must_not_branch(void)
{
    gchar *path = NULL;
    gint fd = g_file_open_tmp("pcv-audit-chain-XXXXXX.db", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    sqlite3 *db_a = NULL;
    sqlite3 *db_b = NULL;
    g_assert_cmpint(sqlite3_open(path, &db_a), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_open(path, &db_b), ==, SQLITE_OK);
    sqlite3_busy_timeout(db_a, 5000);
    sqlite3_busy_timeout(db_b, 5000);
    PcvAuditChainHealth health;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_prepare(db_a, &health, &error));
    g_assert_no_error(error);
    _append(db_a, "2026-08-09 00:00:01", "system", "seed", "audit", "ok", 0);

    GMutex mutex;
    GCond cond;
    g_mutex_init(&mutex);
    g_cond_init(&cond);
    guint ready = 0;
    gboolean go = FALSE;
    WriterCtx a = {db_a, &mutex, &cond, &ready, &go, "writer-a", FALSE, NULL};
    WriterCtx b = {db_b, &mutex, &cond, &ready, &go, "writer-b", FALSE, NULL};
    GThread *thread_a = g_thread_new("audit-writer-a", _writer_thread, &a);
    GThread *thread_b = g_thread_new("audit-writer-b", _writer_thread, &b);
    g_mutex_lock(&mutex);
    while (ready < 2)
        g_cond_wait(&cond, &mutex);
    go = TRUE;
    g_cond_broadcast(&cond);
    g_mutex_unlock(&mutex);
    g_thread_join(thread_a);
    g_thread_join(thread_b);

    g_assert_true(a.result);
    g_assert_no_error(a.error);
    g_assert_true(b.result);
    g_assert_no_error(b.error);
    _verify_ok(db_a);
    g_assert_cmpint(_scalar_int64(db_a, "SELECT COUNT(*) FROM audit_log"), ==, 3);
    g_assert_cmpint(_scalar_int64(db_a,
        "SELECT COUNT(*) FROM ("
        "SELECT chain_epoch, prev_hash FROM audit_log "
        "WHERE chain_epoch IS NOT NULL "
        "GROUP BY chain_epoch, prev_hash HAVING COUNT(*) > 1)"), ==, 0);

    g_cond_clear(&cond);
    g_mutex_clear(&mutex);
    sqlite3_close(db_b);
    sqlite3_close(db_a);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_free(path);
}

static void
test_audit_chain_clean_db_creates_epoch_once(void)
{
    sqlite3 *db = _open_prepared_db();
    PcvAuditChainHealth health;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_prepare(db, &health, &error));
    g_assert_no_error(error);
    g_assert_cmpint(_scalar_int64(db, "SELECT COUNT(*) FROM audit_chain_epoch"), ==, 1);
    g_assert_cmpstr(health.reason, ==, "ok");
    sqlite3_close(db);
}

static void
test_audit_chain_verified_legacy_is_not_rewritten(void)
{
    sqlite3 *db = _build_legacy_chain(FALSE);
    gchar *before = _legacy_dump(db);
    PcvAuditChainHealth health;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_prepare(db, &health, &error));
    g_assert_no_error(error);
    gchar *after = _legacy_dump(db);
    g_assert_cmpstr(after, ==, before);
    g_assert_false(health.historical_break);
    g_assert_cmpint(_scalar_int64(db,
        "SELECT COUNT(*) FROM audit_log WHERE chain_epoch IS NOT NULL"), ==, 0);
    g_assert_cmpint(_scalar_int64(db,
        "SELECT COUNT(*) FROM audit_chain_epoch WHERE predecessor_status='verified'"), ==, 1);
    _append(db, "2026-08-09 01:00:00", "new", "append", "active", "ok", 0);
    _verify_ok(db);
    g_free(after);
    g_free(before);
    sqlite3_close(db);
}

static void
test_audit_chain_unhashed_legacy_boundary_is_preserved(void)
{
    sqlite3 *db = _open_legacy_db();
    _exec_ok(db,
        "INSERT INTO audit_log(ts,node,username,method,target,result,error_code,"
        "duration_ms,src_ip,prev_hash,rec_hash) VALUES("
        "'2026-06-01 00:00:00','legacy-node','old','prechain','target','ok',"
        "0,1,'local',NULL,NULL)");
    PcvAuditChainHealth health;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_prepare(db, &health, &error));
    g_assert_no_error(error);
    g_assert_cmpint(_scalar_int64(db,
        "SELECT predecessor_last_id FROM audit_chain_epoch"), ==, 1);
    _append(db, "2026-08-09 01:00:00", "new", "append", "active", "ok", 0);
    _verify_ok(db);
    sqlite3_close(db);
}

static void
test_audit_chain_broken_legacy_starts_new_epoch(void)
{
    sqlite3 *db = _build_legacy_chain(TRUE);
    gchar *before = _legacy_dump(db);
    PcvAuditChainHealth health;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_prepare(db, &health, &error));
    g_assert_no_error(error);
    gchar *after = _legacy_dump(db);
    g_assert_cmpstr(after, ==, before);
    g_assert_true(health.current_ok);
    g_assert_true(health.historical_break);
    g_assert_cmpint(health.first_break_rowid, ==, 3);
    g_assert_cmpint(_scalar_int64(db,
        "SELECT COUNT(*) FROM audit_chain_epoch WHERE predecessor_status='broken' "
        "AND predecessor_break_id=3"), ==, 1);
    _append(db, "2026-08-09 01:00:00", "new", "append", "active", "ok", 0);
    health = _verify_ok(db);
    g_assert_true(health.historical_break);
    g_free(after);
    g_free(before);
    sqlite3_close(db);
}

static void
test_audit_chain_active_break_blocks_prepare(void)
{
    sqlite3 *db = _build_active_chain();
    _exec_ok(db, "UPDATE audit_log SET target='tampered' WHERE id=2");
    PcvAuditChainHealth health;
    GError *error = NULL;
    g_assert_false(pcv_audit_chain_prepare(db, &health, &error));
    g_assert_error(error, PCV_AUDIT_CHAIN_ERROR, PCV_AUDIT_CHAIN_ERROR_INTEGRITY);
    g_assert_false(health.current_ok);
    g_assert_cmpint(health.first_break_rowid, ==, 2);
    g_clear_error(&error);
    sqlite3_close(db);
}

static void
test_audit_chain_retention_preserves_anchor(void)
{
    sqlite3 *db = _open_prepared_db();
    _append(db, "2026-01-01 00:00:00", "old-a", "audit", "one", "ok", 0);
    _append(db, "2026-01-02 00:00:00", "old-b", "audit", "two", "ok", 0);
    _append(db, "2026-01-03 00:00:00", "keep", "audit", "three", "ok", 0);
    gint deleted = 0;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_retention(
        db, "2026-01-03 00:00:00", &deleted, &error));
    g_assert_no_error(error);
    g_assert_cmpint(deleted, ==, 2);
    g_assert_cmpint(_scalar_int64(db, "SELECT MIN(id) FROM audit_log"), ==, 3);
    g_assert_cmpint(_scalar_int64(db,
        "SELECT COUNT(*) FROM audit_chain_checkpoint"), ==, 1);
    _verify_ok(db);
    sqlite3_close(db);
}

static void
test_audit_chain_retention_allows_future_append(void)
{
    sqlite3 *db = _open_prepared_db();
    _append(db, "2026-01-01 00:00:00", "old-a", "audit", "one", "ok", 0);
    _append(db, "2026-01-02 00:00:00", "old-b", "audit", "two", "ok", 0);
    gint deleted = 0;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_retention(
        db, "2026-02-01 00:00:00", &deleted, &error));
    g_assert_no_error(error);
    g_assert_cmpint(deleted, ==, 2);
    g_assert_cmpint(_scalar_int64(db, "SELECT COUNT(*) FROM audit_log"), ==, 0);
    _verify_ok(db);
    _append(db, "2026-02-02 00:00:00", "future", "audit", "new", "ok", 0);
    _verify_ok(db);
    sqlite3_close(db);
}

static void
test_audit_chain_retention_checkpoint_failure_rolls_back(void)
{
    sqlite3 *db = _open_prepared_db();
    _append(db, "2026-01-01 00:00:00", "old-a", "audit", "one", "ok", 0);
    _append(db, "2026-01-02 00:00:00", "keep", "audit", "two", "ok", 0);
    _exec_ok(db,
        "CREATE TRIGGER reject_checkpoint BEFORE INSERT ON audit_chain_checkpoint "
        "BEGIN SELECT RAISE(ABORT,'forced checkpoint failure'); END");
    gint deleted = -1;
    GError *error = NULL;
    g_assert_false(pcv_audit_chain_retention(
        db, "2026-01-02 00:00:00", &deleted, &error));
    g_assert_error(error, PCV_AUDIT_CHAIN_ERROR, PCV_AUDIT_CHAIN_ERROR_SQLITE);
    g_clear_error(&error);
    g_assert_cmpint(_scalar_int64(db, "SELECT COUNT(*) FROM audit_log"), ==, 2);
    g_assert_cmpint(_scalar_int64(db,
        "SELECT COUNT(*) FROM audit_chain_checkpoint"), ==, 0);
    _exec_ok(db, "DROP TRIGGER reject_checkpoint");
    _verify_ok(db);
    sqlite3_close(db);
}

static void
test_audit_chain_retention_delete_failure_rolls_back_checkpoint(void)
{
    sqlite3 *db = _open_prepared_db();
    _append(db, "2026-01-01 00:00:00", "old-a", "audit", "one", "ok", 0);
    _append(db, "2026-01-02 00:00:00", "keep", "audit", "two", "ok", 0);
    _exec_ok(db,
        "CREATE TRIGGER reject_delete BEFORE DELETE ON audit_log "
        "BEGIN SELECT RAISE(ABORT,'forced delete failure'); END");
    gint deleted = -1;
    GError *error = NULL;
    g_assert_false(pcv_audit_chain_retention(
        db, "2026-01-02 00:00:00", &deleted, &error));
    g_assert_error(error, PCV_AUDIT_CHAIN_ERROR, PCV_AUDIT_CHAIN_ERROR_SQLITE);
    g_clear_error(&error);
    g_assert_cmpint(_scalar_int64(db, "SELECT COUNT(*) FROM audit_log"), ==, 2);
    g_assert_cmpint(_scalar_int64(db,
        "SELECT COUNT(*) FROM audit_chain_checkpoint"), ==, 0);
    _exec_ok(db, "DROP TRIGGER reject_delete");
    _verify_ok(db);
    sqlite3_close(db);
}

static void
test_audit_chain_failed_insert_rolls_back(void)
{
    sqlite3 *db = _open_prepared_db();
    _append(db, "2026-01-01 00:00:00", "seed", "audit", "seed", "ok", 0);
    _exec_ok(db,
        "CREATE TRIGGER reject_audit BEFORE INSERT ON audit_log "
        "BEGIN SELECT RAISE(ABORT,'forced append failure'); END");
    PcvAuditChainRecord record = {
        .ts = "2026-01-02 00:00:00", .node = "test-node",
        .username = "failed", .method = "audit", .target = "failure",
        .result = "fail", .error_code = 1, .duration_ms = 1,
        .src_ip = "127.0.0.1", .event_ts = "2026-01-02 00:00:00",
    };
    GError *error = NULL;
    g_assert_false(pcv_audit_chain_append(db, &record, NULL, &error));
    g_assert_error(error, PCV_AUDIT_CHAIN_ERROR, PCV_AUDIT_CHAIN_ERROR_SQLITE);
    g_clear_error(&error);
    g_assert_cmpint(_scalar_int64(db, "SELECT COUNT(*) FROM audit_log"), ==, 1);
    _exec_ok(db, "DROP TRIGGER reject_audit");
    _append(db, "2026-01-03 00:00:00", "recovered", "audit", "ok", "ok", 0);
    _verify_ok(db);
    sqlite3_close(db);
}

static void
test_audit_chain_throughput_1000(void)
{
    gchar *path = NULL;
    gint fd = g_file_open_tmp("pcv-audit-bench-XXXXXX.db", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
    sqlite3_busy_timeout(db, 5000);
    _exec_ok(db, "PRAGMA journal_mode=WAL");
    PcvAuditChainHealth health;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_prepare(db, &health, &error));
    g_assert_no_error(error);

    PcvAuditChainRecord record = {
        .ts = "2026-08-09 00:00:00", .node = "bench-node",
        .username = "bench", .method = "audit.benchmark", .target = "chain",
        .result = "ok", .error_code = 0, .duration_ms = 0,
        .src_ip = "local", .event_ts = "2026-08-09 00:00:00",
    };
    g_test_timer_start();
    for (gint i = 0; i < 1000; i++) {
        g_assert_true(pcv_audit_chain_append(db, &record, NULL, &error));
        g_assert_no_error(error);
    }
    gdouble elapsed = g_test_timer_elapsed();
    g_test_message("atomic append 1000건: %.3f초, %.0f ops/s",
                   elapsed, 1000.0 / elapsed);
    g_assert_cmpint(_scalar_int64(db, "SELECT COUNT(*) FROM audit_log"), ==, 1000);
    _verify_ok(db);
    sqlite3_close(db);
    gchar *wal = g_strconcat(path, "-wal", NULL);
    gchar *shm = g_strconcat(path, "-shm", NULL);
    g_unlink(wal);
    g_unlink(shm);
    g_assert_cmpint(g_unlink(path), ==, 0);
    g_free(shm);
    g_free(wal);
    g_free(path);
}

void
test_audit_chain_register(void)
{
    g_test_add_func("/audit_chain/intact_passes", test_audit_chain_intact_passes);
    g_test_add_func("/audit_chain/field_tamper_detected", test_audit_chain_field_tamper_detected);
    g_test_add_func("/audit_chain/row_delete_detected", test_audit_chain_row_delete_detected);
    g_test_add_func("/audit_chain/rechash_tamper_detected", test_audit_chain_rechash_tamper_detected);
    g_test_add_func("/audit_chain/null_rechash_detected", test_audit_chain_null_rechash_is_not_skipped);
    g_test_add_func("/audit_chain/epoch_null_move_detected", test_audit_chain_epoch_null_move_is_detected);
    g_test_add_func("/audit_chain/unknown_epoch_move_detected", test_audit_chain_unknown_epoch_move_is_detected);
    g_test_add_func("/audit_chain/two_writers_must_not_branch", test_audit_chain_two_writers_must_not_branch);
    g_test_add_func("/audit_chain/clean_db_creates_epoch_once", test_audit_chain_clean_db_creates_epoch_once);
    g_test_add_func("/audit_chain/verified_legacy_is_not_rewritten", test_audit_chain_verified_legacy_is_not_rewritten);
    g_test_add_func("/audit_chain/unhashed_legacy_boundary", test_audit_chain_unhashed_legacy_boundary_is_preserved);
    g_test_add_func("/audit_chain/broken_legacy_starts_new_epoch", test_audit_chain_broken_legacy_starts_new_epoch);
    g_test_add_func("/audit_chain/active_break_blocks_prepare", test_audit_chain_active_break_blocks_prepare);
    g_test_add_func("/audit_chain/retention_preserves_anchor", test_audit_chain_retention_preserves_anchor);
    g_test_add_func("/audit_chain/retention_allows_future_append", test_audit_chain_retention_allows_future_append);
    g_test_add_func("/audit_chain/retention_checkpoint_failure_rollback", test_audit_chain_retention_checkpoint_failure_rolls_back);
    g_test_add_func("/audit_chain/retention_delete_failure_rollback", test_audit_chain_retention_delete_failure_rolls_back_checkpoint);
    g_test_add_func("/audit_chain/failed_insert_rolls_back", test_audit_chain_failed_insert_rolls_back);
    g_test_add_func("/audit_chain/throughput_1000", test_audit_chain_throughput_1000);
}
