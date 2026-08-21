                                                                                                
                                                                                           
                                                                                
                                                                  
                                                 
   
                             
                                                                   
  
                                                                      
                                                               
   

#include "modules/audit/pcv_audit.h"
#include "utils/pcv_log.h"

#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <sys/file.h>
#include <unistd.h>

                                                               
                                              
void
_pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...)
{
    (void)level;
    (void)domain;
    (void)fmt;
}

void
_pcv_log_audit(const gchar *domain, const gchar *operation,
               const gchar *target, const gchar *fmt, ...)
{
    (void)domain;
    (void)operation;
    (void)target;
    (void)fmt;
}

static gchar *
_new_db_path(gchar **tmp_dir)
{
    GError *error = NULL;
    *tmp_dir = g_dir_make_tmp("pcv-audit-startup-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(*tmp_dir);
    return g_build_filename(*tmp_dir, "audit.db", NULL);
}

static void
_cleanup_path(const gchar *db_path, const gchar *tmp_dir)
{
    gchar *lock = g_strconcat(db_path, ".lock", NULL);
    gchar *wal = g_strconcat(db_path, "-wal", NULL);
    gchar *shm = g_strconcat(db_path, "-shm", NULL);
    g_remove(shm);
    g_remove(wal);
    g_remove(lock);
    g_remove(db_path);
    g_assert_cmpint(g_rmdir(tmp_dir), ==, 0);
    g_free(shm);
    g_free(wal);
    g_free(lock);
}

static void
test_audit_startup_rejects_held_sidecar_lock(void)
{
    gchar *tmp_dir = NULL;
    gchar *db_path = _new_db_path(&tmp_dir);
    gchar *lock_path = g_strconcat(db_path, ".lock", NULL);
    gint fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(flock(fd, LOCK_EX | LOCK_NB), ==, 0);

    GError *error = NULL;
    g_assert_false(pcv_audit_init(db_path, &error));
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_AGAIN);
    g_clear_error(&error);

    flock(fd, LOCK_UN);
    close(fd);
    g_free(lock_path);
    _cleanup_path(db_path, tmp_dir);
    g_free(db_path);
    g_free(tmp_dir);
}

static void
_create_tampered_active_db(const gchar *db_path)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(db_path, &db), ==, SQLITE_OK);
    PcvAuditChainHealth health;
    GError *error = NULL;
    g_assert_true(pcv_audit_chain_prepare(db, &health, &error));
    g_assert_no_error(error);
    PcvAuditChainRecord record = {
        .ts = "2026-08-09 00:00:00", .node = "test-node",
        .username = "admin", .method = "test", .target = "audit",
        .result = "ok", .error_code = 0, .duration_ms = 1,
        .src_ip = "local", .event_ts = "2026-08-09 00:00:00",
    };
    g_assert_true(pcv_audit_chain_append(db, &record, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(sqlite3_exec(db,
        "UPDATE audit_log SET target='tampered' WHERE id=1",
        NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(db);
}

static void
test_audit_startup_rejects_active_epoch_tamper(void)
{
    gchar *tmp_dir = NULL;
    gchar *db_path = _new_db_path(&tmp_dir);
    _create_tampered_active_db(db_path);

    GError *error = NULL;
    g_assert_false(pcv_audit_init(db_path, &error));
    g_assert_error(error, PCV_AUDIT_CHAIN_ERROR,
                   PCV_AUDIT_CHAIN_ERROR_INTEGRITY);
    g_clear_error(&error);

    _cleanup_path(db_path, tmp_dir);
    g_free(db_path);
    g_free(tmp_dir);
}

static void
test_audit_startup_file_only_health_is_not_ok(void)
{
    gchar *tmp_dir = NULL;
    gchar *db_path = _new_db_path(&tmp_dir);
                                                               
    g_assert_cmpint(g_mkdir(db_path, 0700), ==, 0);

    GError *error = NULL;
    g_assert_true(pcv_audit_init(db_path, &error));
    g_assert_no_error(error);
    PcvAuditChainHealth health = pcv_audit_get_chain_health();
    g_assert_false(health.current_ok);
    g_assert_cmpstr(health.reason, ==, "db_unavailable");
    pcv_audit_shutdown();

    g_assert_cmpint(g_rmdir(db_path), ==, 0);
    _cleanup_path(db_path, tmp_dir);
    g_free(db_path);
    g_free(tmp_dir);
}

static void
test_audit_startup_valid_chain_and_shutdown(void)
{
    gchar *tmp_dir = NULL;
    gchar *db_path = _new_db_path(&tmp_dir);
    GError *error = NULL;
    g_assert_true(pcv_audit_init(db_path, &error));
    g_assert_no_error(error);
    PcvAuditChainHealth health = pcv_audit_get_chain_health();
    g_assert_true(health.current_ok);
    pcv_audit_log("admin", "startup.test", "audit", "ok", 0, 1, "local");
    pcv_audit_shutdown();

    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(db_path, &db), ==, SQLITE_OK);
    g_assert_true(pcv_audit_chain_verify(db, &health, &error));
    g_assert_no_error(error);
    g_assert_true(health.current_ok);
    sqlite3_close(db);

    _cleanup_path(db_path, tmp_dir);
    g_free(db_path);
    g_free(tmp_dir);
}

static void
test_audit_append_failure_marks_health_critical(void)
{
    gchar *tmp_dir = NULL;
    gchar *db_path = _new_db_path(&tmp_dir);
    GError *error = NULL;
    g_assert_true(pcv_audit_init(db_path, &error));
    g_assert_no_error(error);

    sqlite3 *injector = NULL;
    g_assert_cmpint(sqlite3_open(db_path, &injector), ==, SQLITE_OK);
    sqlite3_busy_timeout(injector, 5000);
    g_assert_cmpint(sqlite3_exec(injector,
        "CREATE TRIGGER reject_audit_insert "
        "BEFORE INSERT ON audit_log BEGIN "
        "SELECT RAISE(ABORT, 'injected append failure'); END",
        NULL, NULL, NULL), ==, SQLITE_OK);

    pcv_audit_log("admin", "append.failure", "audit", "ok", 0, 1, "local");
    PcvAuditChainHealth health = {0};
    for (guint attempt = 0; attempt < 200; attempt++) {
        health = pcv_audit_get_chain_health();
        if (g_strcmp0(health.reason, "append_failed") == 0)
            break;
        g_usleep(10 * 1000);
    }
    g_assert_false(health.current_ok);
    g_assert_cmpstr(health.reason, ==, "append_failed");

    g_assert_cmpint(sqlite3_exec(injector,
        "DROP TRIGGER reject_audit_insert", NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(injector);
    pcv_audit_shutdown();

    _cleanup_path(db_path, tmp_dir);
    g_free(db_path);
    g_free(tmp_dir);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/audit_startup/held_lock",
                    test_audit_startup_rejects_held_sidecar_lock);
    g_test_add_func("/audit_startup/active_tamper",
                    test_audit_startup_rejects_active_epoch_tamper);
    g_test_add_func("/audit_startup/file_only_health",
                    test_audit_startup_file_only_health_is_not_ok);
    g_test_add_func("/audit_startup/append_failure_health",
                    test_audit_append_failure_marks_health_critical);
    g_test_add_func("/audit_startup/valid_shutdown",
                    test_audit_startup_valid_chain_and_shutdown);
    return g_test_run();
}
