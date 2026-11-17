                                                                                      
                                                                                                  
                             
                                                             
                         
   
                         
                                                    
  
                                                                               
                 
                                                                               
                                                           
                
  
                                                 
                                                   
  
          
                                                                               

                        
                                        
                                      
                                   
  
                                                  
                                            
                                                                               
   
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <sqlite3.h>
#include <string.h>
#include "fixtures/job_queue_legacy.h"
#include "../src/utils/pcv_job_queue.h"
#include "../src/utils/pcv_job_recovery.h"
#include "../src/modules/dispatcher/rpc_completion.h"

static gchar *g_tmpdir = NULL;
static gchar *g_dbpath = NULL;

static void setup_db(void) {
    g_tmpdir = g_dir_make_tmp("pcv-jobq-XXXXXX", NULL);
    g_assert_nonnull(g_tmpdir);
    g_dbpath = g_build_filename(g_tmpdir, "jobs.db", NULL);
    g_setenv("PCV_JOBS_DB_PATH", g_dbpath, TRUE);
    pcv_job_queue_init();
}

static void teardown_db(void) {
    pcv_job_queue_shutdown();
    g_unsetenv("PCV_JOBS_DB_PATH");
    if (g_dbpath) { g_unlink(g_dbpath); g_free(g_dbpath); g_dbpath = NULL; }
                          
    if (g_tmpdir) {
        gchar *wal = g_strconcat(g_tmpdir, "/jobs.db-wal", NULL);
        gchar *shm = g_strconcat(g_tmpdir, "/jobs.db-shm", NULL);
        g_unlink(wal); g_unlink(shm); g_free(wal); g_free(shm);
        gchar *journal = g_build_filename(g_tmpdir, "jobs.db.journal", NULL);
        g_rmdir(journal); g_free(journal);
        g_assert_cmpint(g_rmdir(g_tmpdir), ==, 0); g_free(g_tmpdir); g_tmpdir = NULL;
    }
}

                                                              

static void test_init_shutdown_idempotent(void) {
    setup_db();
    pcv_job_queue_init();              
    pcv_job_queue_shutdown();
    pcv_job_queue_shutdown();              
    teardown_db();
}

static void test_create_returns_id(void) {
    setup_db();
    gchar *id = pcv_job_create("ova_export", "vm-test", NULL);
    g_assert_nonnull(id);
    g_assert_cmpuint(strlen(id), >, 4);
    g_assert_true(g_str_has_prefix(id, "job-"));
    g_free(id);
    teardown_db();
}

static void test_get_pending_after_create(void) {
    setup_db();
    gchar *id = pcv_job_create("ova_export", "vm-test", "{\"k\":\"v\"}");
    JsonObject *job = pcv_job_get(id, NULL);
    g_assert_nonnull(job);
    g_assert_cmpstr(json_object_get_string_member(job, "type"), ==, "ova_export");
    g_assert_cmpstr(json_object_get_string_member(job, "target"), ==, "vm-test");
    g_assert_cmpint((gint)json_object_get_int_member(job, "status_code"), ==, PCV_JOB_PENDING);
    json_object_unref(job);
    g_free(id);
    teardown_db();
}

static void test_update_status_progress(void) {
    setup_db();
    gchar *id = pcv_job_create("backup", "vm-x", NULL);
    pcv_job_update_status(id, PCV_JOB_RUNNING, 42, "in progress");
    JsonObject *job = pcv_job_get(id, NULL);
    g_assert_cmpint((gint)json_object_get_int_member(job, "status_code"), ==, PCV_JOB_RUNNING);
    g_assert_cmpint((gint)json_object_get_int_member(job, "progress"), ==, 42);
    g_assert_cmpstr(json_object_get_string_member(job, "detail"), ==, "in progress");
    json_object_unref(job);
    g_free(id);
    teardown_db();
}

static void test_set_result_completed(void) {
    setup_db();
    gchar *id = pcv_job_create("vm_create", "vm-c", NULL);
    pcv_job_set_result(id, PCV_JOB_COMPLETED, "{\"ok\":true}");
    JsonObject *job = pcv_job_get(id, NULL);
    g_assert_cmpint((gint)json_object_get_int_member(job, "status_code"), ==, PCV_JOB_COMPLETED);
    g_assert_cmpstr(json_object_get_string_member(job, "result"), ==, "{\"ok\":true}");
    json_object_unref(job);
    g_free(id);
    teardown_db();
}

static void test_cancel_pending(void) {
    setup_db();
    gchar *id = pcv_job_create("backup", "vm-y", NULL);
    g_assert_true(pcv_job_cancel(id));
    JsonObject *job = pcv_job_get(id, NULL);
    g_assert_cmpint((gint)json_object_get_int_member(job, "status_code"), ==, PCV_JOB_CANCELLED);
    json_object_unref(job);
                                      
    g_assert_false(pcv_job_cancel(id));
    g_free(id);
    teardown_db();
}

static void test_cancel_completed_fails(void) {
    setup_db();
    gchar *id = pcv_job_create("backup", "vm-z", NULL);
    pcv_job_set_result(id, PCV_JOB_COMPLETED, NULL);
    g_assert_false(pcv_job_cancel(id));
    g_free(id);
    teardown_db();
}

static void test_list_returns_recent(void) {
    setup_db();
    gchar *ids[3];
    for (int i = 0; i < 3; i++) {
        gchar *target = g_strdup_printf("vm-%d", i);
        ids[i] = pcv_job_create("test", target, NULL);
        g_free(target);
    }
    JsonArray *list = pcv_job_list(10, NULL);
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), >=, 3);
    json_array_unref(list);
    for (int i = 0; i < 3; i++) g_free(ids[i]);
    teardown_db();
}

static void test_get_nonexistent(void) {
    setup_db();
    JsonObject *job = pcv_job_get("job-nonexistent", NULL);
    g_assert_null(job);
    g_assert_false(pcv_job_cancel("job-nonexistent"));
    teardown_db();
}

static void test_cleanup_old(void) {
    setup_db();
    gchar *id = pcv_job_create("backup", "vm-old", NULL);
    pcv_job_set_result(id, PCV_JOB_COMPLETED, NULL);
                                                  
                                                    
    pcv_job_queue_cleanup_old(-1);
    JsonObject *job = pcv_job_get(id, NULL);
    g_assert_null(job);
    g_free(id);
    teardown_db();
}



static sqlite3 *external_db(void) {
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(g_dbpath, &db), ==, SQLITE_OK);
    return db;
}

static void external_sql(sqlite3 *db, const gchar *sql) {
    g_assert_cmpint(sqlite3_exec(db, sql, NULL, NULL, NULL), ==, SQLITE_OK);
}

static void assert_external_job(sqlite3 *db, const gchar *id, const gchar *target,
                                PcvJobStatus status, const gchar *result) {
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "SELECT target,status,result FROM jobs WHERE job_id=?", -1, &stmt, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
    g_assert_cmpstr((const gchar *)sqlite3_column_text(stmt, 0), ==, target);
    g_assert_cmpint(sqlite3_column_int(stmt, 1), ==, status);
    g_assert_cmpstr((const gchar *)sqlite3_column_text(stmt, 2), ==, result);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static void assert_external_count(sqlite3 *db, gint count) {
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM jobs", -1, &stmt, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
    g_assert_cmpint(sqlite3_column_int(stmt, 0), ==, count);
    sqlite3_finalize(stmt);
}

static void test_retention_tick_daily_gate(void) {
    setup_db();
    gchar *id = pcv_job_create("backup", "vm-old", NULL);
    pcv_job_set_result(id, PCV_JOB_COMPLETED, NULL);
    sqlite3 *db = external_db();
    external_sql(db, "UPDATE jobs SET updated_at = strftime('%s','now') - 100000");
    gint64 now = (gint64)time(NULL);
    gint64 last_run = now - 100;
    g_assert_false(pcv_job_queue_retention_tick(&last_run, now, 30));
    g_assert_cmpint(last_run, ==, now - 100);
    assert_external_count(db, 1);
    sqlite3_close(db); g_free(id); teardown_db();
}

static void test_retention_tick_disabled_guard(void) {
    setup_db();
    gchar *id = pcv_job_create("backup", "vm-old", NULL);
    pcv_job_set_result(id, PCV_JOB_COMPLETED, NULL);
    sqlite3 *db = external_db();
    external_sql(db, "UPDATE jobs SET updated_at = strftime('%s','now') - 100000");
    gint64 now = (gint64)time(NULL);
    gint64 last0 = 0;
    g_assert_false(pcv_job_queue_retention_tick(&last0, now, 0));
    assert_external_count(db, 1);
    gint64 lastn = 0;
    g_assert_false(pcv_job_queue_retention_tick(&lastn, now, -5));
    assert_external_count(db, 1);
    sqlite3_close(db); g_free(id); teardown_db();
}

static void test_retention_tick_deletes_terminal_keeps_running(void) {
    setup_db();
    gchar *done = pcv_job_create("backup", "vm-done", NULL);
    pcv_job_set_result(done, PCV_JOB_COMPLETED, NULL);
    gchar *pending = pcv_job_create("backup", "vm-pending", NULL);
    sqlite3 *db = external_db();
    external_sql(db, "UPDATE jobs SET updated_at = strftime('%s','now') - 100000");
    gint64 now = (gint64)time(NULL);
    gint64 last0 = 0;
    g_assert_true(pcv_job_queue_retention_tick(&last0, now, 1));
    g_assert_cmpint(last0, ==, now);
    assert_external_count(db, 1);
    JsonObject *job = pcv_job_get(pending, NULL);
    g_assert_nonnull(job);
    g_assert_cmpint((gint)json_object_get_int_member(job, "status_code"), ==, PCV_JOB_PENDING);
    json_object_unref(job);
    sqlite3_close(db); g_free(done); g_free(pending); teardown_db();
}

static void test_create_locked(void) {
    setup_db();
    sqlite3 *db = external_db();
    external_sql(db, "BEGIN IMMEDIATE");
    gchar *id = pcv_job_create("test", "blocked", NULL);
    assert_external_count(db, 0);
    g_assert_null(id);
    external_sql(db, "ROLLBACK");
    id = pcv_job_create("test", "unlocked", NULL);
    g_assert_nonnull(id);
    assert_external_count(db, 1);
    assert_external_job(db, id, "unlocked", PCV_JOB_PENDING, NULL);
    g_free(id); sqlite3_close(db); teardown_db();
}

static void test_collision_retry(void) {
    setup_db();
    sqlite3 *db = external_db();
    g_random_set_seed(12345);
    gchar *first = pcv_job_create("test", "original", NULL);
    g_random_set_seed(12345);
    gchar *second = pcv_job_create("test", "new-target", NULL);
    g_assert_nonnull(second);
    g_assert_cmpstr(first, !=, second);
    g_assert_true(pcv_job_set_result(second, PCV_JOB_FAILED, "{\"owner\":\"new-target\"}"));
    assert_external_count(db, 2);
    assert_external_job(db, first, "original", PCV_JOB_PENDING, NULL);
    assert_external_job(db, second, "new-target", PCV_JOB_FAILED, "{\"owner\":\"new-target\"}");
    g_free(first); g_free(second); sqlite3_close(db); teardown_db();
}

static void test_collision_limit(void) {
    setup_db();
    sqlite3 *db = external_db();
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "INSERT INTO jobs(job_id,type,target) VALUES(?,'existing','original')", -1, &stmt, NULL), ==, SQLITE_OK);
    g_random_set_seed(54321);
    for (gint i = 0; i < 32; i++) {
        gchar *id = g_strdup_printf("job-%08x", g_random_int());
        sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
        g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_DONE);
        sqlite3_reset(stmt); g_free(id);
    }
    sqlite3_finalize(stmt);
    g_random_set_seed(54321);
    g_assert_null(pcv_job_create("test", "must-not-replace", NULL));
    assert_external_count(db, 32);
    sqlite3_close(db); teardown_db();
}

static void test_write_locked(void) {
    setup_db();
    sqlite3 *db = external_db();
    gchar *id = pcv_job_create("test", "original", NULL);
    external_sql(db, "BEGIN IMMEDIATE");
    g_assert_false(pcv_job_update_status(id, PCV_JOB_RUNNING, 50, "locked"));
    g_assert_false(pcv_job_set_result(id, PCV_JOB_COMPLETED, "{\"ok\":true}"));
    g_assert_false(pcv_job_cancel(id));
    assert_external_job(db, id, "original", PCV_JOB_PENDING, NULL);
    external_sql(db, "ROLLBACK");
    g_assert_true(pcv_job_update_status(id, PCV_JOB_RUNNING, 50, "unlocked"));
    g_assert_true(pcv_job_set_result(id, PCV_JOB_COMPLETED, "{\"ok\":true}"));
    assert_external_job(db, id, "original", PCV_JOB_COMPLETED, "{\"ok\":true}");
    g_free(id); sqlite3_close(db); teardown_db();
}

static void test_write_missing(void) {
    setup_db();
    sqlite3 *db = external_db();
    g_assert_false(pcv_job_update_status("absent", PCV_JOB_RUNNING, 50, NULL));
    g_assert_false(pcv_job_set_result("absent", PCV_JOB_COMPLETED, NULL));
    g_assert_false(pcv_job_update_status(NULL, PCV_JOB_RUNNING, 50, NULL));
    g_assert_false(pcv_job_set_result(NULL, PCV_JOB_COMPLETED, NULL));
    g_assert_false(pcv_job_cancel("absent"));
    assert_external_count(db, 0);
    sqlite3_close(db); teardown_db();
}

static void test_disabled_admission(void) {
    setup_db(); pcv_job_queue_shutdown();
    gchar *missing = g_build_filename(g_tmpdir, "missing", "jobs.db", NULL);
    g_setenv("PCV_JOBS_DB_PATH", missing, TRUE);
    pcv_job_queue_init();
    g_assert_null(pcv_job_create("test", "disabled", NULL));
    g_assert_false(pcv_job_update_status("absent", PCV_JOB_RUNNING, 1, NULL));
    g_assert_false(pcv_job_set_result("absent", PCV_JOB_COMPLETED, NULL));
    g_assert_false(pcv_job_cancel("absent"));
    g_free(missing); teardown_db();
}

static void test_schema_failure(void) {
    setup_db(); pcv_job_queue_shutdown();
    sqlite3 *db = external_db();
    external_sql(db, "DROP TABLE jobs; CREATE TABLE jobs(wrong_column TEXT)");
    pcv_job_queue_init();
    g_assert_null(pcv_job_create("test", "bad-schema", NULL));
    sqlite3_close(db); teardown_db();
}

static void test_prepare_failure(void) {
    setup_db();
    sqlite3 *db = external_db();
    external_sql(db, "DROP TABLE jobs");
    g_assert_null(pcv_job_create("test", "bad-schema", NULL));
    g_assert_false(pcv_job_update_status("absent", PCV_JOB_RUNNING, 1, NULL));
    g_assert_false(pcv_job_set_result("absent", PCV_JOB_COMPLETED, NULL));
    g_assert_false(pcv_job_cancel("absent"));
    sqlite3_close(db); teardown_db();
}

static void test_insert_ignored(void) {
    setup_db();
    sqlite3 *db = external_db();
    external_sql(db, "CREATE TRIGGER reject_insert BEFORE INSERT ON jobs BEGIN SELECT RAISE(IGNORE); END");
    g_assert_null(pcv_job_create("test", "ignored", NULL));
    assert_external_count(db, 0);
    external_sql(db, "DROP TRIGGER reject_insert");
    gchar *id = pcv_job_create("test", "allowed", NULL);
    g_assert_nonnull(id); assert_external_count(db, 1);
    g_free(id); sqlite3_close(db); teardown_db();
}

static void test_update_ignored(void) {
    setup_db();
    sqlite3 *db = external_db();
    gchar *id = pcv_job_create("test", "original", NULL);
    external_sql(db, "CREATE TRIGGER reject_update BEFORE UPDATE ON jobs BEGIN SELECT RAISE(IGNORE); END");
    g_assert_false(pcv_job_update_status(id, PCV_JOB_RUNNING, 1, NULL));
    g_assert_false(pcv_job_set_result(id, PCV_JOB_COMPLETED, "{\"ok\":true}"));
    g_assert_false(pcv_job_cancel(id));
    assert_external_job(db, id, "original", PCV_JOB_PENDING, NULL);
    external_sql(db, "DROP TRIGGER reject_update");
    g_assert_true(pcv_job_set_result(id, PCV_JOB_COMPLETED, "{\"ok\":true}"));
    assert_external_job(db, id, "original", PCV_JOB_COMPLETED, "{\"ok\":true}");
    g_free(id); sqlite3_close(db); teardown_db();
}

static void test_durable_result(void) {
    setup_db();
    gchar *id = pcv_job_create("test", "durable", NULL);
    g_assert_true(pcv_job_set_result(id, PCV_JOB_COMPLETED, "{\"ok\":true}"));
    pcv_job_queue_shutdown();
    sqlite3 *db = external_db();
    assert_external_job(db, id, "durable", PCV_JOB_COMPLETED, "{\"ok\":true}");
    sqlite3_close(db); pcv_job_queue_init();
    db = external_db();
    assert_external_job(db, id, "durable", PCV_JOB_COMPLETED, "{\"ok\":true}");
    g_free(id); sqlite3_close(db); teardown_db();
}

static void insert_job_row(sqlite3 *db, gint64 rowid, const gchar *id, const gchar *method,
                           const gchar *target, PcvJobStatus status, gint64 created) {
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "INSERT INTO jobs(rowid,job_id,type,target,status,progress,created_at,updated_at,generation,method,outcome_known)"
        " VALUES(?,?,?,?,?,0,?,?,'g',?,?)", -1, &stmt, NULL), ==, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, rowid);
    sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, method, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, target, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, status);
    sqlite3_bind_int64(stmt, 6, created);
    sqlite3_bind_int64(stmt, 7, created);
    sqlite3_bind_text(stmt, 8, method, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, status == PCV_JOB_COMPLETED || status == PCV_JOB_FAILED);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static gchar *latest_id(const gchar *method, const gchar *target) {
    JsonObject *job = NULL;
    g_assert_true(pcv_job_find_latest(method, target, NULL, &job));
    gchar *id = job ? g_strdup(json_object_get_string_member(job, "job_id")) : NULL;
    if (job) json_object_unref(job);
    return id;
}

static void test_find_latest_absent(void) {
    setup_db();
    JsonObject *job = json_object_new();
    JsonObject *keep = job;
    g_assert_true(pcv_job_find_latest("vm.delete", "web", NULL, &job));
    g_assert_null(job);
    json_object_unref(keep);
    teardown_db();
}

static void test_find_latest_newest_and_method_filter(void) {
    setup_db();
    sqlite3 *db = external_db();
    insert_job_row(db, 1, "job-old", "vm.delete", "web", PCV_JOB_COMPLETED, 100);
    insert_job_row(db, 2, "job-new", "vm.delete", "web", PCV_JOB_FAILED, 200);
    insert_job_row(db, 3, "job-create", "vm.create", "web", PCV_JOB_COMPLETED, 300);
    insert_job_row(db, 4, "job-other", "vm.delete", "db", PCV_JOB_COMPLETED, 400);
    gchar *id = latest_id("vm.delete", "web");
    g_assert_cmpstr(id, ==, "job-new");
    g_free(id);
    JsonObject *job = NULL;
    g_assert_true(pcv_job_find_latest("vm.delete", "web", NULL, &job));
    g_assert_cmpstr(json_object_get_string_member(job, "status"), ==, "failed");
    g_assert_cmpstr(json_object_get_string_member(job, "method"), ==, "vm.delete");
    json_object_unref(job);
    sqlite3_close(db); teardown_db();
}

static void test_find_latest_same_second_uses_rowid(void) {
    setup_db();
    sqlite3 *db = external_db();
    insert_job_row(db, 20, "job-later", "vm.delete", "web", PCV_JOB_COMPLETED, 300);
    insert_job_row(db, 10, "job-earlier", "vm.delete", "web", PCV_JOB_FAILED, 300);
    gchar *id = latest_id("vm.delete", "web");
    g_assert_cmpstr(id, ==, "job-later");
    g_free(id);
    external_sql(db, "DROP INDEX idx_jobs_method_target");
    id = latest_id("vm.delete", "web");
    g_assert_cmpstr(id, ==, "job-later");
    g_free(id);
    sqlite3_close(db); teardown_db();
}

static void test_find_latest_returns_interrupted(void) {
    setup_db();
    sqlite3 *db = external_db();
    insert_job_row(db, 1, "job-int", "vm.delete", "web", PCV_JOB_INTERRUPTED, 100);
    JsonObject *job = NULL;
    g_assert_true(pcv_job_find_latest("vm.delete", "web", NULL, &job));
    g_assert_cmpstr(json_object_get_string_member(job, "status"), ==, "interrupted");
    g_assert_false(json_object_get_boolean_member(job, "outcome_known"));
    json_object_unref(job);
    sqlite3_close(db); teardown_db();
}

static void test_find_latest_disabled_ledger(void) {
    setup_db(); pcv_job_queue_shutdown();
    gchar *missing = g_build_filename(g_tmpdir, "missing", "jobs.db", NULL);
    g_setenv("PCV_JOBS_DB_PATH", missing, TRUE);
    pcv_job_queue_init();
    JsonObject *job = NULL;
    g_assert_false(pcv_job_find_latest("vm.delete", "web", NULL, &job));
    g_assert_null(job);
    g_assert_false(pcv_job_find_latest(NULL, "web", NULL, &job));
    g_free(missing); teardown_db();
}

static void test_find_latest_index_created(void) {
    setup_db();
    sqlite3 *db = external_db();
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "SELECT sql FROM sqlite_master WHERE type='index' AND name='idx_jobs_method_target'", -1, &stmt, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
    g_assert_nonnull(strstr((const gchar *)sqlite3_column_text(stmt, 0), "method, target, created_at"));
    sqlite3_finalize(stmt);
    g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
    g_assert_cmpint(sqlite3_column_int(stmt, 0), ==, 2);
    sqlite3_finalize(stmt);
    sqlite3_close(db); teardown_db();
}

void pcv_job_queue_fail_scope_alter_for_test(gboolean fail);
guint pcv_job_queue_scope_warns_for_test(void);
gboolean pcv_job_recovery_record_roundtrip_for_test(const PcvJobRef *ref, PcvJobStatus status, gchar **encoded_out);

static gboolean scope_fake_decide(gint role, const gchar *subject, const gchar *tenant, const gchar *actor,
                                  const gchar *actor_tenant) {
    if (role >= 2) return TRUE;
    if (!actor) return FALSE;
    return g_strcmp0(actor, subject) == 0 || (tenant && actor_tenant && g_strcmp0(tenant, actor_tenant) == 0);
}

static const PcvJobScope SCOPE_TA = { FALSE, 0, "zed", "ta", scope_fake_decide };
static const PcvJobScope SCOPE_ADMIN = { TRUE, 2, "root", NULL, scope_fake_decide };

static void insert_actor_row(sqlite3 *db, gint64 rowid, const gchar *id, const gchar *target, gint64 created,
                             const gchar *actor, const gchar *actor_tenant) {
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "INSERT INTO jobs(rowid,job_id,type,target,status,progress,created_at,updated_at,generation,method,outcome_known,actor,actor_tenant)"
        " VALUES(?,?,'vm.delete',?,2,100,?,?,'g','vm.delete',1,?,?)", -1, &stmt, NULL), ==, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, rowid);
    sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, target, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, created);
    sqlite3_bind_int64(stmt, 5, created);
    sqlite3_bind_text(stmt, 6, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, actor_tenant, -1, SQLITE_TRANSIENT);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_DONE);
    sqlite3_finalize(stmt);
}

static gboolean has_column(sqlite3 *db, const gchar *name) {
    sqlite3_stmt *stmt = NULL;
    gboolean found = FALSE;
    g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA table_info(jobs)", -1, &stmt, NULL), ==, SQLITE_OK);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        found |= g_strcmp0((const gchar *)sqlite3_column_text(stmt, 1), name) == 0;
    sqlite3_finalize(stmt);
    return found;
}

static gint user_version(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
    gint v = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

static void test_scope_columns_migrate(void) {
    setup_db();
    sqlite3 *db = external_db();
    g_assert_true(has_column(db, "actor") && has_column(db, "actor_tenant"));
    g_assert_cmpint(user_version(db), ==, 2);
    sqlite3_close(db);
    teardown_db();

    g_tmpdir = g_dir_make_tmp("pcv-jobq-XXXXXX", NULL);
    g_dbpath = g_build_filename(g_tmpdir, "jobs.db", NULL);
    g_assert_cmpint(sqlite3_open(g_dbpath, &db), ==, SQLITE_OK);
    external_sql(db, "CREATE TABLE jobs (job_id TEXT PRIMARY KEY,type TEXT NOT NULL,target TEXT,status INTEGER DEFAULT 0,"
                     "progress INTEGER DEFAULT 0,detail TEXT,params TEXT,result TEXT,created_at INTEGER,updated_at INTEGER,"
                     "generation TEXT NOT NULL DEFAULT '',method TEXT NOT NULL DEFAULT '',recovery_reason TEXT,"
                     "outcome_known INTEGER NOT NULL DEFAULT 0)");
    external_sql(db, "INSERT INTO jobs(job_id,type,target,status,created_at,updated_at,generation,method) "
                     "VALUES('job-0000beef','vm.delete','web',2,100,100,'g','vm.delete')");
    external_sql(db, "PRAGMA user_version=2");
    g_assert_false(has_column(db, "actor"));
    sqlite3_close(db);
    g_setenv("PCV_JOBS_DB_PATH", g_dbpath, TRUE);
    pcv_job_queue_init();
    db = external_db();
    g_assert_true(has_column(db, "actor") && has_column(db, "actor_tenant"));
    g_assert_cmpint(user_version(db), ==, 2);
    sqlite3_close(db);
    JsonObject *old = pcv_job_get("job-0000beef", NULL);
    g_assert_nonnull(old);
    json_object_unref(old);
    pcv_job_queue_shutdown();
    pcv_job_queue_init();
    db = external_db();
    g_assert_true(has_column(db, "actor") && has_column(db, "actor_tenant"));
    g_assert_cmpint(user_version(db), ==, 2);
    sqlite3_close(db);
    teardown_db();
}

static void test_old_sql_on_new_db(void) {
    static const gchar *const OLD[] = {
        "SELECT job_id,type,target,status,progress,detail,params,result,created_at,updated_at,generation,method,recovery_reason,outcome_known FROM jobs LIMIT 0",
        "INSERT INTO jobs(job_id,type,target,params,method,generation,status,progress,created_at,updated_at) VALUES('job-0000a001','vm.delete','web',NULL,'vm.delete','gen-1',0,0,100,100)",
        "INSERT INTO jobs (job_id, type, target, status, progress, params, created_at, updated_at) VALUES ('job-0000a002', 'ova_export', 'web', 0, 0, NULL, 100, 100)",
        "SELECT job_id,type,target,status,progress,detail,params,result,created_at,updated_at,generation,method,recovery_reason,outcome_known FROM jobs ORDER BY created_at DESC LIMIT 50",
        "SELECT job_id,type,target,status,progress,detail,params,result,created_at,updated_at,generation,method,recovery_reason,outcome_known FROM jobs WHERE job_id='job-0000a001'",
        "SELECT job_id,type,target,status,progress,detail,params,result,created_at,updated_at,generation,method,recovery_reason,outcome_known FROM jobs WHERE method='vm.delete' AND target='web' ORDER BY created_at DESC, rowid DESC LIMIT 1",
        "UPDATE jobs SET status=1, progress=5, detail='d', updated_at=101 WHERE job_id='job-0000a002' AND generation='' AND status<2",
        "UPDATE jobs SET status=2, progress=100, result='{}', outcome_known=1, updated_at=102 WHERE job_id='job-0000a002' AND generation=''",
        "UPDATE jobs SET status=4, outcome_known=0, recovery_reason='cancel_requested', detail='Cancellation requested; worker outcome unconfirmed', updated_at=103 WHERE job_id='job-0000a001' AND status < 2",
        "UPDATE jobs SET status=1,progress=7,detail='d',updated_at=104 WHERE job_id='job-0000a001' AND generation='gen-1' AND method='vm.delete' AND target IS 'web' AND status<2",
        "UPDATE jobs SET status=2,progress=100,result='{}',outcome_known=1,recovery_reason=NULL,updated_at=105 WHERE job_id='job-0000a001' AND generation='gen-1' AND status<2",
        "UPDATE jobs SET status=5,outcome_known=0,recovery_reason='restart_without_durable_result',detail='Execution interrupted; actual outcome unknown',updated_at=106 WHERE job_id='job-0000a001' AND status<2",
        "SELECT job_id FROM jobs WHERE status IN (2,3,4) AND outcome_known=1 AND updated_at<1000",
        "SELECT generation,method,target,status,result FROM jobs WHERE job_id='job-0000a001'",
    };
    setup_db();
    sqlite3 *db = external_db();
    for (guint i = 0; i < G_N_ELEMENTS(OLD); i++) {
        gchar *err = NULL;
        gint rc = sqlite3_exec(db, OLD[i], NULL, NULL, &err);
        if (rc != SQLITE_OK) g_error("old SQL %u failed: %s", i, err ? err : "?");
    }
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, "SELECT count(*) FROM jobs WHERE actor IS NULL AND actor_tenant IS NULL", -1, &stmt, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
    g_assert_cmpint(sqlite3_column_int(stmt, 0), ==, 2);
    sqlite3_finalize(stmt);
    g_assert_cmpint(user_version(db), ==, 2);
    sqlite3_close(db);
    teardown_db();
}

static void test_alter_fail_scope_off(void) {
    pcv_job_queue_fail_scope_alter_for_test(TRUE);
    guint warns = pcv_job_queue_scope_warns_for_test();
    setup_db();
    pcv_job_queue_fail_scope_alter_for_test(FALSE);
    g_assert_cmpuint(pcv_job_queue_scope_warns_for_test() - warns, ==, 1);
    gchar *id = pcv_job_create("test", "web", NULL);
    g_assert_nonnull(id);
    JsonArray *all = pcv_job_list(10, &SCOPE_ADMIN);
    g_assert_cmpuint(json_array_get_length(all), ==, 1);
    json_array_unref(all);
    JsonArray *none = pcv_job_list(10, &SCOPE_TA);
    g_assert_cmpuint(json_array_get_length(none), ==, 0);
    json_array_unref(none);
    g_assert_null(pcv_job_get(id, &SCOPE_TA));
    JsonObject *job = json_object_new(), *keep = job;
    g_assert_true(pcv_job_find_latest("test", "web", &SCOPE_TA, &job));
    g_assert_null(job);
    json_object_unref(keep);
    g_free(id);
    teardown_db();
}

static void test_list_where_before_limit(void) {
    setup_db();
    sqlite3 *db = external_db();
    for (gint i = 0; i < 3; i++) {
        gchar *id = g_strdup_printf("job-0000b%03d", i);
        insert_actor_row(db, 1 + i, id, "mine", 100 + i, "bob", "ta");
        g_free(id);
    }
    for (gint i = 0; i < 60; i++) {
        gchar *id = g_strdup_printf("job-0000c%03d", i);
        insert_actor_row(db, 10 + i, id, "other", 1000 + i, "carol", "tb");
        g_free(id);
    }
    JsonArray *list = pcv_job_list(50, &SCOPE_TA);
    g_assert_cmpuint(json_array_get_length(list), ==, 3);
    json_array_unref(list);
    list = pcv_job_list(50, &SCOPE_ADMIN);
    g_assert_cmpuint(json_array_get_length(list), ==, 50);
    json_array_unref(list);
    sqlite3_close(db);
    teardown_db();
}

static void test_get_invisible_is_null(void) {
    setup_db();
    sqlite3 *db = external_db();
    insert_actor_row(db, 1, "job-0000d001", "web", 100, "carol", "tb");
    insert_actor_row(db, 2, "job-0000d002", "web", 100, "zed", NULL);
    g_assert_null(pcv_job_get("job-0000d001", &SCOPE_TA));
    g_assert_null(pcv_job_get("job-0000dfff", &SCOPE_TA));
    JsonObject *own = pcv_job_get("job-0000d002", &SCOPE_TA);
    g_assert_nonnull(own);
    json_object_unref(own);
    JsonObject *adm = pcv_job_get("job-0000d001", &SCOPE_ADMIN);
    g_assert_nonnull(adm);
    json_object_unref(adm);
    sqlite3_close(db);
    teardown_db();
}

static void test_find_latest_visible(void) {
    setup_db();
    sqlite3 *db = external_db();
    insert_actor_row(db, 1, "job-0000e001", "web", 100, "bob", "ta");
    insert_actor_row(db, 2, "job-0000e002", "web", 200, "carol", "tb");
    insert_actor_row(db, 3, "job-0000e003", "db", 300, "carol", "tb");
    JsonObject *job = NULL;
    g_assert_true(pcv_job_find_latest("vm.delete", "web", &SCOPE_TA, &job));
    g_assert_cmpstr(json_object_get_string_member(job, "job_id"), ==, "job-0000e001");
    json_object_unref(job);
    job = NULL;
    g_assert_true(pcv_job_find_latest("vm.delete", "db", &SCOPE_TA, &job));
    g_assert_null(job);
    g_assert_true(pcv_job_find_latest("vm.delete", "web", &SCOPE_ADMIN, &job));
    g_assert_cmpstr(json_object_get_string_member(job, "job_id"), ==, "job-0000e002");
    json_object_unref(job);
    sqlite3_close(db);
    teardown_db();
}

static void test_legacy_rows_admin_only(void) {
    setup_db();
    sqlite3 *db = external_db();
    insert_actor_row(db, 1, "job-0000f001", "web", 100, NULL, NULL);
    PcvJobScope self = { FALSE, 1, "zed", NULL, scope_fake_decide };
    g_assert_null(pcv_job_get("job-0000f001", &self));
    JsonObject *adm = pcv_job_get("job-0000f001", &SCOPE_ADMIN);
    g_assert_nonnull(adm);
    json_object_unref(adm);
    static const gchar *const RESERVED[] = { "local-admin", "system" };
    for (guint i = 0; i < G_N_ELEMENTS(RESERVED); i++) {
        pcv_rpc_completion_begin("vm.delete");
        pcv_rpc_completion_set_actor(RESERVED[i]);
        pcv_rpc_completion_set_actor_tenant("ta");
        PcvJobRef *ref = pcv_job_admit("vm.delete", "web", NULL, "vm.delete");
        (void)pcv_rpc_completion_finish();
        g_assert_nonnull(ref);
        sqlite3_stmt *stmt = NULL;
        g_assert_cmpint(sqlite3_prepare_v2(db, "SELECT actor IS NULL, actor_tenant IS NULL FROM jobs WHERE job_id=?", -1, &stmt, NULL), ==, SQLITE_OK);
        sqlite3_bind_text(stmt, 1, pcv_job_id(ref), -1, SQLITE_TRANSIENT);
        g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
        g_assert_cmpint(sqlite3_column_int(stmt, 0), ==, 1);
        g_assert_cmpint(sqlite3_column_int(stmt, 1), ==, 1);
        sqlite3_finalize(stmt);
        pcv_job_unref(ref);
    }
    sqlite3_close(db);
    teardown_db();
}

static void test_journal_record_unchanged(void) {
    setup_db();
    pcv_rpc_completion_begin("vm.delete");
    pcv_rpc_completion_set_actor("zed");
    pcv_rpc_completion_set_actor_tenant("ta");
    PcvJobRef *ref = pcv_job_admit("vm.delete", "web", NULL, "vm.delete");
    (void)pcv_rpc_completion_finish();
    g_assert_nonnull(ref);
    gchar *encoded = NULL;
    g_assert_true(pcv_job_recovery_record_roundtrip_for_test(ref, PCV_JOB_COMPLETED, &encoded));
    JsonParser *outer = json_parser_new();
    g_assert_true(json_parser_load_from_data(outer, encoded, -1, NULL));
    const gchar *payload = json_object_get_string_member(json_node_get_object(json_parser_get_root(outer)), "payload");
    JsonParser *inner = json_parser_new();
    g_assert_true(json_parser_load_from_data(inner, payload, -1, NULL));
    JsonObject *rec = json_node_get_object(json_parser_get_root(inner));
    g_assert_cmpuint(json_object_get_size(rec), ==, 8);
    g_assert_cmpint(json_object_get_int_member(rec, "version"), ==, 1);
    g_assert_false(json_object_has_member(rec, "actor"));
    g_assert_false(json_object_has_member(rec, "actor_tenant"));
    g_object_unref(inner);
    g_object_unref(outer);
    g_free(encoded);
    pcv_job_unref(ref);
    teardown_db();
}

void test_job_queue_register(void) {
    g_test_add_func("/job_queue/init_shutdown_idempotent", test_init_shutdown_idempotent);
    g_test_add_func("/job_queue/create_returns_id", test_create_returns_id);
    g_test_add_func("/job_queue/get_pending_after_create", test_get_pending_after_create);
    g_test_add_func("/job_queue/update_status_progress", test_update_status_progress);
    g_test_add_func("/job_queue/set_result_completed", test_set_result_completed);
    g_test_add_func("/job_queue/cancel_pending", test_cancel_pending);
    g_test_add_func("/job_queue/cancel_completed_fails", test_cancel_completed_fails);
    g_test_add_func("/job_queue/list_returns_recent", test_list_returns_recent);
    g_test_add_func("/job_queue/get_nonexistent", test_get_nonexistent);
    g_test_add_func("/job_queue/cleanup_old", test_cleanup_old);
    g_test_add_func("/job_queue/retention_tick_daily_gate", test_retention_tick_daily_gate);
    g_test_add_func("/job_queue/retention_tick_disabled_guard", test_retention_tick_disabled_guard);
    g_test_add_func("/job_queue/retention_tick_deletes_terminal_keeps_running", test_retention_tick_deletes_terminal_keeps_running);
    g_test_add_func("/job_queue/create_locked", test_create_locked);
    g_test_add_func("/job_queue/collision_retry", test_collision_retry);
    g_test_add_func("/job_queue/collision_limit", test_collision_limit);
    g_test_add_func("/job_queue/write_locked", test_write_locked);
    g_test_add_func("/job_queue/write_missing", test_write_missing);
    g_test_add_func("/job_queue/disabled_admission", test_disabled_admission);
    g_test_add_func("/job_queue/schema_failure", test_schema_failure);
    g_test_add_func("/job_queue/prepare_failure", test_prepare_failure);
    g_test_add_func("/job_queue/insert_ignored", test_insert_ignored);
    g_test_add_func("/job_queue/update_ignored", test_update_ignored);
    g_test_add_func("/job_queue/durable_result", test_durable_result);
    g_test_add_func("/job_queue/find_latest_absent", test_find_latest_absent);
    g_test_add_func("/job_queue/find_latest_newest_and_method_filter", test_find_latest_newest_and_method_filter);
    g_test_add_func("/job_queue/find_latest_same_second_uses_rowid", test_find_latest_same_second_uses_rowid);
    g_test_add_func("/job_queue/find_latest_returns_interrupted", test_find_latest_returns_interrupted);
    g_test_add_func("/job_queue/find_latest_disabled_ledger", test_find_latest_disabled_ledger);
    g_test_add_func("/job_queue/find_latest_index_created", test_find_latest_index_created);
    g_test_add_func("/job_queue/scope_columns_migrate", test_scope_columns_migrate);
    g_test_add_func("/job_queue/old_sql_on_new_db", test_old_sql_on_new_db);
    g_test_add_func("/job_queue/alter_fail_scope_off", test_alter_fail_scope_off);
    g_test_add_func("/job_queue/list_where_before_limit", test_list_where_before_limit);
    g_test_add_func("/job_queue/get_invisible_is_null", test_get_invisible_is_null);
    g_test_add_func("/job_queue/find_latest_visible", test_find_latest_visible);
    g_test_add_func("/job_queue/legacy_rows_admin_only", test_legacy_rows_admin_only);
    g_test_add_func("/job_queue/journal_record_unchanged", test_journal_record_unchanged);
}
