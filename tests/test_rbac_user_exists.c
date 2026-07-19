
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>

#include "../src/modules/auth/pcv_rbac.h"

static gchar *g_tmpdir = NULL;
static gchar *g_dbpath = NULL;

static void
setup(void)
{
    g_tmpdir = g_dir_make_tmp("pcv-rbac-user-exists-XXXXXX", NULL);
    g_dbpath = g_build_filename(g_tmpdir, "rbac.db", NULL);
    pcv_rbac_init(g_dbpath);
}

static void
teardown(void)
{
    pcv_rbac_shutdown();
    if (g_dbpath) {
        gchar *wal = g_strconcat(g_dbpath, "-wal", NULL);
        gchar *shm = g_strconcat(g_dbpath, "-shm", NULL);
        g_unlink(g_dbpath); g_unlink(wal); g_unlink(shm);
        g_free(wal); g_free(shm); g_free(g_dbpath); g_dbpath = NULL;
    }
    if (g_tmpdir) { g_rmdir(g_tmpdir); g_free(g_tmpdir); g_tmpdir = NULL; }
}

static void
test_user_exists_absent_for_missing_user(void)
{
    setup();
    g_assert_cmpint(pcv_rbac_user_exists("nobody_here"), ==, PCV_USER_ABSENT);
    teardown();
}

static void
test_user_exists_present_after_create(void)
{
    setup();
    GError *err = NULL;
    gboolean ok = pcv_rbac_user_create("alice", "pw", PCV_ROLE_ADMIN, NULL, &err);
    g_assert_true(ok);
    g_assert_no_error(err);
    g_assert_cmpint(pcv_rbac_user_exists("alice"), ==, PCV_USER_PRESENT);
    teardown();
}

static void
test_user_exists_absent_for_empty_username(void)
{
    setup();
    g_assert_cmpint(pcv_rbac_user_exists(""), ==, PCV_USER_ABSENT);
    teardown();
}

static void
test_user_exists_unknown_on_db_error(void)
{
    setup();

    sqlite3 *raw_db = NULL;
    g_assert_cmpint(sqlite3_open(g_dbpath, &raw_db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(raw_db, "DROP TABLE users;", NULL, NULL, NULL),
                    ==, SQLITE_OK);
    sqlite3_close(raw_db);

    g_assert_cmpint(pcv_rbac_user_exists("anyone"), ==, PCV_USER_UNKNOWN);

    teardown();
}

void
test_rbac_user_exists_register(void)
{
    g_test_add_func("/rbac/user_exists/absent_for_missing_user",
                    test_user_exists_absent_for_missing_user);
    g_test_add_func("/rbac/user_exists/present_after_create",
                    test_user_exists_present_after_create);
    g_test_add_func("/rbac/user_exists/absent_for_empty_username",
                    test_user_exists_absent_for_empty_username);
    g_test_add_func("/rbac/user_exists/unknown_on_db_error",
                    test_user_exists_unknown_on_db_error);
}
