/**
 * @file test_job_queue.c
 * @brief pcv_job_queue 유닛 테스트 (PCV_JOBS_DB_PATH 격리)
 *
 * ============================================================================
 *  이 파일이 테스트하는 것
 * ============================================================================
 *  pcv_job_queue.h (src/utils/)의 SQLite 기반 비동기 작업 큐를 검증한다.
 *  10개 테스트 케이스.
 *
 *  Job Queue는 장기 실행 작업(OVA 내보내기, 클라우드 마이그레이션 등)의
 *  진행 상태를 SQLite에 영속 저장한다. 데몬 재시작 후에도 작업 상태 조회 가능.
 *
 *  검증 항목:
 *  - CRUD: create → get(PENDING) → update(RUNNING/42%) → set_result(COMPLETED)
 *  - 취소: PENDING 상태만 취소 가능, COMPLETED는 거부
 *  - 목록 조회: 최근 N개 작업 반환
 *  - 존재하지 않는 ID: get=NULL, cancel=FALSE
 *  - 정리: max_age_hours 기준으로 완료된 작업 삭제
 *  - 멱등성: init/shutdown 여러 번 호출 안전
 *
 *  테스트 격리: PCV_JOBS_DB_PATH 환경변수로 임시 디렉터리에 DB 생성.
 *  teardown에서 DB 파일 + WAL/SHM 사이드카 파일까지 정리.
 * ============================================================================
 */
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
    /* WAL/SHM 사이드카도 정리 */
    if (g_tmpdir) {
        gchar *wal = g_strconcat(g_tmpdir, "/jobs.db-wal", NULL);
        gchar *shm = g_strconcat(g_tmpdir, "/jobs.db-shm", NULL);
        g_unlink(wal); g_unlink(shm); g_free(wal); g_free(shm);
        g_rmdir(g_tmpdir); g_free(g_tmpdir); g_tmpdir = NULL;
    }
}

/* ── 케이스 ───────────────────────────────────────────────── */

static void test_init_shutdown_idempotent(void) {
    setup_db();
    pcv_job_queue_init();  /* 재호출 안전 */
    pcv_job_queue_shutdown();
    pcv_job_queue_shutdown();  /* 재호출 안전 */
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
    /* 재취소: 이미 cancelled 상태 → FALSE */
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
    /* max_age_hours=-1 → cutoff가 미래 → 모든 완료 작업 삭제
       (0이면 같은 초에 updated_at < now가 false라 안 지워짐) */
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
