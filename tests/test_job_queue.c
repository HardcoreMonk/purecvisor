
























#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
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
    gchar *id = pcv_job_create("cloud_import", "vm-c", NULL);
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
}
