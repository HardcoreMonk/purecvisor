
#include <glib.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <string.h>

#define CHAIN_GENESIS \
    "0000000000000000000000000000000000000000000000000000000000000000"

static gchar *
_sha256_hex(const gchar *input)
{
    guchar digest[EVP_MAX_MD_SIZE];
    guint  digest_len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx) {
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
        EVP_DigestUpdate(ctx, input, strlen(input));
        EVP_DigestFinal_ex(ctx, digest, &digest_len);
        EVP_MD_CTX_free(ctx);
    }
    GString *hex = g_string_sized_new(digest_len * 2 + 1);
    for (guint i = 0; i < digest_len; i++)
        g_string_append_printf(hex, "%02x", digest[i]);
    return g_string_free(hex, FALSE);
}

static gchar *
_rec_hash(const gchar *prev, const gchar *ts, const gchar *user,
          const gchar *method, const gchar *target, const gchar *result,
          gint error_code)
{
    gchar *pre = g_strdup_printf("%s|%s|%s|%s|%s|%s|%d",
                                 prev ? prev : "", ts ? ts : "",
                                 user ? user : "", method ? method : "",
                                 target ? target : "", result ? result : "",
                                 error_code);
    gchar *h = _sha256_hex(pre);
    g_free(pre);
    return h;
}

static gboolean
_verify_chain(sqlite3 *db, gsize *first_break_rowid)
{
    if (first_break_rowid) *first_break_rowid = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id, ts, username, method, target, result, error_code, "
            "prev_hash, rec_hash FROM audit_log ORDER BY id ASC",
            -1, &st, NULL) != SQLITE_OK)
        return TRUE;

    gboolean ok = TRUE;
    gchar *expected_prev = g_strdup(CHAIN_GENESIS);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const gchar *rec_hash = (const gchar *)sqlite3_column_text(st, 8);
        if (!rec_hash || !*rec_hash) continue;

        gsize        rowid = (gsize)sqlite3_column_int64(st, 0);
        const gchar *ts    = (const gchar *)sqlite3_column_text(st, 1);
        const gchar *user  = (const gchar *)sqlite3_column_text(st, 2);
        const gchar *meth  = (const gchar *)sqlite3_column_text(st, 3);
        const gchar *targ  = (const gchar *)sqlite3_column_text(st, 4);
        const gchar *res   = (const gchar *)sqlite3_column_text(st, 5);
        gint         ecode = sqlite3_column_int(st, 6);
        const gchar *prev  = (const gchar *)sqlite3_column_text(st, 7);
        const gchar *prev_norm = prev ? prev : "";

        if (g_strcmp0(prev_norm, expected_prev) != 0) {
            if (first_break_rowid) *first_break_rowid = rowid;
            ok = FALSE; break;
        }
        gchar *recomputed = _rec_hash(prev_norm, ts, user, meth, targ, res, ecode);
        gboolean match = (g_strcmp0(recomputed, rec_hash) == 0);
        g_free(recomputed);
        if (!match) {
            if (first_break_rowid) *first_break_rowid = rowid;
            ok = FALSE; break;
        }
        g_free(expected_prev);
        expected_prev = g_strdup(rec_hash);
    }
    g_free(expected_prev);
    sqlite3_finalize(st);
    return ok;
}

static sqlite3 *
_open_audit_db(void)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(":memory:", &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db,
        "CREATE TABLE audit_log ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts TEXT, username TEXT, method TEXT, target TEXT, result TEXT,"
        "  error_code INTEGER, prev_hash TEXT, rec_hash TEXT)",
        NULL, NULL, NULL), ==, SQLITE_OK);
    return db;
}

static void
_append(sqlite3 *db, gchar **chain_head, const gchar *ts, const gchar *user,
        const gchar *method, const gchar *target, const gchar *result,
        gint error_code)
{
    const gchar *prev = *chain_head ? *chain_head : CHAIN_GENESIS;
    gchar *rec = _rec_hash(prev, ts, user, method, target, result, error_code);

    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "INSERT INTO audit_log(ts,username,method,target,result,error_code,prev_hash,rec_hash)"
        " VALUES(?,?,?,?,?,?,?,?)", -1, &st, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, ts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, method, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, target, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, result, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, error_code);
    sqlite3_bind_text(st, 7, prev, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, rec, -1, SQLITE_TRANSIENT);
    g_assert_cmpint(sqlite3_step(st), ==, SQLITE_DONE);
    sqlite3_finalize(st);

    g_free(*chain_head);
    *chain_head = rec;
}

static sqlite3 *
_build_three_row_chain(void)
{
    sqlite3 *db = _open_audit_db();
    gchar *head = NULL;
    _append(db, &head, "2026-07-16 00:00:01", "admin",  "auth.login",   "admin", "ok",   0);
    _append(db, &head, "2026-07-16 00:00:02", "alice",  "vm.create",    "vm1",   "ok",   0);
    _append(db, &head, "2026-07-16 00:00:03", "bob",    "vm.delete",    "vm1",   "fail", 403);
    g_free(head);
    return db;
}

static void
test_audit_chain_intact_passes(void)
{
    sqlite3 *db = _build_three_row_chain();
    gsize brk = 12345;
    g_assert_true(_verify_chain(db, &brk));
    g_assert_cmpuint(brk, ==, 0);
    sqlite3_close(db);
}

static void
test_audit_chain_field_tamper_detected(void)
{
    sqlite3 *db = _build_three_row_chain();

    g_assert_cmpint(sqlite3_exec(db,
        "UPDATE audit_log SET result='denied' WHERE id=2", NULL, NULL, NULL),
        ==, SQLITE_OK);

    gsize brk = 0;
    g_assert_false(_verify_chain(db, &brk));
    g_assert_cmpuint(brk, ==, 2);
    sqlite3_close(db);
}

static void
test_audit_chain_row_delete_detected(void)
{
    sqlite3 *db = _build_three_row_chain();

    g_assert_cmpint(sqlite3_exec(db,
        "DELETE FROM audit_log WHERE id=2", NULL, NULL, NULL), ==, SQLITE_OK);

    gsize brk = 0;
    g_assert_false(_verify_chain(db, &brk));
    g_assert_cmpuint(brk, ==, 3);
    sqlite3_close(db);
}

static void
test_audit_chain_rechash_tamper_detected(void)
{
    sqlite3 *db = _build_three_row_chain();
    g_assert_cmpint(sqlite3_exec(db,
        "UPDATE audit_log SET rec_hash='deadbeef' WHERE id=2", NULL, NULL, NULL),
        ==, SQLITE_OK);

    gsize brk = 0;
    g_assert_false(_verify_chain(db, &brk));
    g_assert_cmpuint(brk, ==, 2);
    sqlite3_close(db);
}

static void
test_audit_chain_null_prechain_skipped(void)
{
    sqlite3 *db = _open_audit_db();

    g_assert_cmpint(sqlite3_exec(db,
        "INSERT INTO audit_log(ts,username,method,target,result,error_code,prev_hash,rec_hash)"
        " VALUES('2026-07-15 00:00:00','old','x','y','ok',0,NULL,NULL)",
        NULL, NULL, NULL), ==, SQLITE_OK);

    gchar *head = NULL;
    _append(db, &head, "2026-07-16 00:00:01", "admin", "auth.login", "admin", "ok", 0);
    g_free(head);

    gsize brk = 123;
    g_assert_true(_verify_chain(db, &brk));
    g_assert_cmpuint(brk, ==, 0);
    sqlite3_close(db);
}

void
test_audit_chain_register(void)
{
    g_test_add_func("/audit_chain/intact_passes",         test_audit_chain_intact_passes);
    g_test_add_func("/audit_chain/field_tamper_detected", test_audit_chain_field_tamper_detected);
    g_test_add_func("/audit_chain/row_delete_detected",   test_audit_chain_row_delete_detected);
    g_test_add_func("/audit_chain/rechash_tamper_detected", test_audit_chain_rechash_tamper_detected);
    g_test_add_func("/audit_chain/null_prechain_skipped", test_audit_chain_null_prechain_skipped);
}
