                                                                                      
                                                                                                  
                             
                                                             
                         
   
                         
                                                    
  
                                                                               
                 
                                                                               
                                                           
                
  
                                                 
                                                   
  
          
                                                                               

                        
                                        
                                      
                                   
  
                                                  
                                            
                                                                               
   
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <sqlite3.h>
#include "../src/utils/pcv_job_queue.h"

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
        g_rmdir(g_tmpdir); g_free(g_tmpdir); g_tmpdir = NULL;
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
    JsonObject *job = pcv_job_get(id);
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
    JsonObject *job = pcv_job_get(id);
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
    JsonObject *job = pcv_job_get(id);
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
    JsonObject *job = pcv_job_get(id);
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
    JsonArray *list = pcv_job_list(10);
    g_assert_nonnull(list);
    g_assert_cmpuint(json_array_get_length(list), >=, 3);
    json_array_unref(list);
    for (int i = 0; i < 3; i++) g_free(ids[i]);
    teardown_db();
}

static void test_get_nonexistent(void) {
    setup_db();
    JsonObject *job = pcv_job_get("job-nonexistent");
    g_assert_null(job);
    g_assert_false(pcv_job_cancel("job-nonexistent"));
    teardown_db();
}

static void test_cleanup_old(void) {
    setup_db();
    gchar *id = pcv_job_create("backup", "vm-old", NULL);
    pcv_job_set_result(id, PCV_JOB_COMPLETED, NULL);
                                                  
                                                    
    pcv_job_queue_cleanup_old(-1);
    JsonObject *job = pcv_job_get(id);
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
}
