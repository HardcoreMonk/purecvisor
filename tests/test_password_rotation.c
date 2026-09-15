










#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include "../src/modules/auth/pcv_rbac.h"
#include "../src/utils/pcv_jwt.h"

static const gchar *old_password = "Fixture-Old-Password-73!";
static const gchar *new_password = "Fixture-New-Password-73!";
static gchar *tmpdir, *dbpath;

static void setup(void)
{
    tmpdir = g_dir_make_tmp("pcv-password-rotation-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    dbpath = g_build_filename(tmpdir, "rbac.db", NULL);
    pcv_jwt_init("password-rotation-fixture-secret-32-bytes");
    pcv_rbac_init(dbpath);
    GError *error = NULL;
    g_assert_true(pcv_rbac_user_create("rotation-admin", old_password,
                                      PCV_ROLE_ADMIN, NULL, &error));
    g_assert_no_error(error);
}

static void teardown(void)
{
    pcv_rbac_shutdown();
    pcv_jwt_shutdown();
    const gchar *suffixes[] = { "", "-wal", "-shm" };
    for (guint i = 0; i < G_N_ELEMENTS(suffixes); i++) {
        gchar *path = g_strconcat(dbpath, suffixes[i], NULL);
        g_unlink(path); g_free(path);
    }
    g_rmdir(tmpdir);
    g_clear_pointer(&dbpath, g_free);
    g_clear_pointer(&tmpdir, g_free);
}



static void exec_sql(const gchar *sql)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(dbpath, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, sql, NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(db);
}

static gchar *credential_snapshot(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    g_assert_cmpint(sqlite3_open(dbpath, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "SELECT password_hash || ':' || salt FROM users WHERE username='rotation-admin'",
        -1, &stmt, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
    gchar *snapshot = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt); sqlite3_close(db);
    return snapshot;
}

static gchar *issue_refresh(void)
{
    GError *error = NULL;
    gchar *refresh = NULL;
    gchar *access = pcv_rbac_authenticate_v2("rotation-admin", old_password, &refresh, &error);
    g_assert_no_error(error);
    g_assert_nonnull(access);
    g_assert_nonnull(refresh);
    g_free(access);
    return refresh;
}

static void assert_active_sessions(guint expected)
{
    JsonArray *sessions = pcv_rbac_list_sessions("rotation-admin");
    g_assert_cmpuint(json_array_get_length(sessions), ==, expected);
    json_array_unref(sessions);
}

static void test_rotation_success_and_restart(void)
{
    setup();
    gchar *refresh = issue_refresh();
    gchar *before = credential_snapshot();
    GError *error = NULL;
    g_assert_true(pcv_rbac_change_password("rotation-admin", old_password, new_password, &error));
    g_assert_no_error(error);
    gchar *after = credential_snapshot();
    g_assert_cmpstr(before, !=, after);
    assert_active_sessions(0);


    gchar *next_refresh = NULL;
    gchar *access = pcv_rbac_refresh_token(refresh, &next_refresh, &error);
    g_assert_null(access);
    g_assert_null(next_refresh);
    g_clear_error(&error);
    g_assert_false(pcv_rbac_password_check("rotation-admin", old_password, &error));
    g_clear_error(&error);
    g_assert_true(pcv_rbac_password_check("rotation-admin", new_password, &error));
    g_assert_no_error(error);


    pcv_rbac_shutdown(); pcv_rbac_init(dbpath);
    g_assert_true(pcv_rbac_password_check("rotation-admin", new_password, &error));
    g_assert_no_error(error);
    g_free(before); g_free(after); g_free(refresh);
    teardown();
}

static void test_rotation_rejects_bad_input(gconstpointer data)
{
    setup();
    gchar *refresh = issue_refresh();
    gchar *before = credential_snapshot();
    const gchar *old = GPOINTER_TO_INT(data) == 0 ? "Wrong-Old-Password!" : old_password;
    const gchar *next = GPOINTER_TO_INT(data) == 1 ? "short" :
                        GPOINTER_TO_INT(data) == 2 ? old_password : new_password;
    GError *error = NULL;
    g_assert_false(pcv_rbac_change_password("rotation-admin", old, next, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    gchar *after = credential_snapshot();
    g_assert_cmpstr(before, ==, after);
    assert_active_sessions(1);
    g_free(before); g_free(after); g_free(refresh);
    teardown();
}

static void test_rotation_sql_failure_is_atomic(gconstpointer data)
{
    setup();
    gchar *refresh = issue_refresh();
    gchar *before = credential_snapshot();
    exec_sql(GPOINTER_TO_INT(data) != 0
        ? "CREATE TRIGGER fixture_failure BEFORE UPDATE OF revoked ON sessions "
          "BEGIN SELECT RAISE(ABORT, 'fixture session failure'); END"
        : "CREATE TRIGGER fixture_failure BEFORE UPDATE OF password_hash ON users "
          "BEGIN SELECT RAISE(ABORT, 'fixture password failure'); END");
    GError *error = NULL;
    g_assert_false(pcv_rbac_change_password("rotation-admin", old_password, new_password, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);
    gchar *after = credential_snapshot();
    g_assert_cmpstr(before, ==, after);
    assert_active_sessions(1);


    exec_sql("DROP TRIGGER fixture_failure");
    g_assert_true(pcv_rbac_change_password("rotation-admin", old_password, new_password, &error));
    g_assert_no_error(error);
    assert_active_sessions(0);
    g_free(before); g_free(after); g_free(refresh);
    teardown();
}

void test_password_rotation_register(void)
{
    g_test_add_func("/password_rotation/success_restart", test_rotation_success_and_restart);
    g_test_add_data_func("/password_rotation/wrong_old", GINT_TO_POINTER(0), test_rotation_rejects_bad_input);
    g_test_add_data_func("/password_rotation/short_new", GINT_TO_POINTER(1), test_rotation_rejects_bad_input);
    g_test_add_data_func("/password_rotation/same_new", GINT_TO_POINTER(2), test_rotation_rejects_bad_input);
    g_test_add_data_func("/password_rotation/users_failure", GINT_TO_POINTER(0), test_rotation_sql_failure_is_atomic);
    g_test_add_data_func("/password_rotation/sessions_failure", GINT_TO_POINTER(1), test_rotation_sql_failure_is_atomic);
}
