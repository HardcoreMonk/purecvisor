
#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include <openssl/evp.h>

#include "../src/modules/auth/pcv_rbac.h"
#include "../src/utils/pcv_jwt.h"

static gchar *g_tmpdir = NULL;
static gchar *g_dbpath = NULL;

static void
setup(void)
{
    g_tmpdir = g_dir_make_tmp("pcv-pbkdf2-verify-XXXXXX", NULL);
    g_dbpath = g_build_filename(g_tmpdir, "rbac.db", NULL);

    pcv_jwt_init("test-secret-key-for-pbkdf2-verify-32bytes");
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

static gchar *
compute_pbkdf2_hex(const gchar *salt, const gchar *password, int iter)
{
    unsigned char dk[32];
    PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
                      (const unsigned char *)salt, (int)strlen(salt),
                      iter, EVP_sha256(), 32, dk);
    GString *hex = g_string_sized_new(65);
    for (int i = 0; i < 32; i++)
        g_string_append_printf(hex, "%02x", dk[i]);
    return g_string_free(hex, FALSE);
}

static void
seed_user(const gchar *username, const gchar *password_hash, const gchar *salt)
{
    sqlite3 *raw = NULL;
    g_assert_cmpint(sqlite3_open(g_dbpath, &raw), ==, SQLITE_OK);
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(raw,
        "INSERT INTO users(username, password_hash, salt, role, tenant) "
        "VALUES(?,?,?,2,NULL);", -1, &st, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, username,      -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, password_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, salt,          -1, SQLITE_STATIC);
    g_assert_cmpint(sqlite3_step(st), ==, SQLITE_DONE);
    sqlite3_finalize(st);
    sqlite3_close(raw);
}

static void
test_new_format_correct_password_verifies(void)
{
    setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("u_new_ok", "Str0ng!Passw0rd",
                                       PCV_ROLE_ADMIN, NULL, &err));
    g_assert_no_error(err);

    gchar *tok = pcv_rbac_authenticate("u_new_ok", "Str0ng!Passw0rd", &err);
    g_assert_nonnull(tok);
    g_assert_no_error(err);
    g_free(tok);
    teardown();
}

static void
test_new_format_wrong_password_rejected(void)
{
    setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("u_new_bad", "Str0ng!Passw0rd",
                                       PCV_ROLE_ADMIN, NULL, &err));
    g_assert_no_error(err);

    gchar *tok = pcv_rbac_authenticate("u_new_bad", "wrong-password", &err);
    g_assert_null(tok);
    g_clear_error(&err);
    teardown();
}

static void
test_legacy_format_correct_password_verifies(void)
{
    setup();
    const gchar *salt = "a1b2c3d4e5f60718";
    const gchar *pw   = "L3gacyPa$$word!";

    gchar *hex  = compute_pbkdf2_hex(salt, pw, 100000);
    gchar *hash = g_strconcat("pbkdf2:", hex, NULL);
    seed_user("u_legacy_ok", hash, salt);
    g_free(hex); g_free(hash);

    GError *err = NULL;
    gchar *tok = pcv_rbac_authenticate("u_legacy_ok", pw, &err);
    g_assert_nonnull(tok);
    g_assert_no_error(err);
    g_free(tok);
    teardown();
}

static void
test_legacy_format_wrong_password_rejected(void)
{
    setup();
    const gchar *salt = "0f1e2d3c4b5a6978";
    const gchar *pw   = "L3gacyPa$$word!";
    gchar *hex  = compute_pbkdf2_hex(salt, pw, 100000);
    gchar *hash = g_strconcat("pbkdf2:", hex, NULL);
    seed_user("u_legacy_bad", hash, salt);
    g_free(hex); g_free(hash);

    GError *err = NULL;
    gchar *tok = pcv_rbac_authenticate("u_legacy_bad", "not-the-password", &err);
    g_assert_null(tok);
    g_clear_error(&err);
    teardown();
}

static void
test_legacy_implicit_iterations_are_100000(void)
{
    setup();
    const gchar *salt = "9988776655443322";
    const gchar *pw   = "L3gacyPa$$word!";

    gchar *hex  = compute_pbkdf2_hex(salt, pw, 200000);
    gchar *hash = g_strconcat("pbkdf2:", hex, NULL);
    seed_user("u_legacy_iter", hash, salt);
    g_free(hex); g_free(hash);

    GError *err = NULL;
    gchar *tok = pcv_rbac_authenticate("u_legacy_iter", pw, &err);
    g_assert_null(tok);
    g_clear_error(&err);
    teardown();
}

void
test_pbkdf2_verify_register(void)
{
    g_test_add_func("/pbkdf2_verify/new_format/correct_password_verifies",
                    test_new_format_correct_password_verifies);
    g_test_add_func("/pbkdf2_verify/new_format/wrong_password_rejected",
                    test_new_format_wrong_password_rejected);
    g_test_add_func("/pbkdf2_verify/legacy_format/correct_password_verifies",
                    test_legacy_format_correct_password_verifies);
    g_test_add_func("/pbkdf2_verify/legacy_format/wrong_password_rejected",
                    test_legacy_format_wrong_password_rejected);
    g_test_add_func("/pbkdf2_verify/legacy_format/implicit_iterations_are_100000",
                    test_legacy_implicit_iterations_are_100000);
}
