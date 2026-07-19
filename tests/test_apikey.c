
#include <glib.h>
#include <sqlite3.h>

#define APIKEY_CANONICAL_SCHEMA \
    "CREATE TABLE api_keys (" \
    "  key_hash     TEXT PRIMARY KEY," \
    "  client_name  TEXT NOT NULL," \
    "  role         INTEGER NOT NULL DEFAULT 1," \
    "  description  TEXT NOT NULL DEFAULT ''," \
    "  created_at   TEXT NOT NULL DEFAULT (datetime('now'))," \
    "  last_used_at TEXT," \
    "  expires_at   INTEGER NOT NULL DEFAULT 0," \
    "  revoked      INTEGER NOT NULL DEFAULT 0" \
    ")"

#define APIKEY_LEGACY_SCHEMA2 \
    "CREATE TABLE api_keys (" \
    "  key_hash     TEXT PRIMARY KEY," \
    "  client_name  TEXT NOT NULL," \
    "  role         INTEGER NOT NULL DEFAULT 1," \
    "  created_at   TEXT NOT NULL DEFAULT (datetime('now'))," \
    "  last_used_at TEXT," \
    "  revoked      INTEGER NOT NULL DEFAULT 0" \
    ")"

#define APIKEY_VERIFY_SQL \
    "SELECT client_name FROM api_keys " \
    "WHERE key_hash = ? AND revoked = 0 " \
    "AND (expires_at = 0 OR expires_at > ?)"

#define APIKEY_VERIFY_ROLE_SQL \
    "SELECT client_name, role FROM api_keys " \
    "WHERE key_hash = ? AND revoked = 0 " \
    "AND (expires_at = 0 OR expires_at > ?)"

#define USERS_MIN_SCHEMA \
    "CREATE TABLE users (" \
    "  username TEXT PRIMARY KEY NOT NULL," \
    "  role     INTEGER NOT NULL DEFAULT 0" \
    ")"

#define APIKEY_ALTER_DESC "ALTER TABLE api_keys ADD COLUMN description TEXT NOT NULL DEFAULT ''"
#define APIKEY_ALTER_EXP  "ALTER TABLE api_keys ADD COLUMN expires_at  INTEGER NOT NULL DEFAULT 0"

static sqlite3 *
_open_with(const char *schema_sql)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(":memory:", &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, schema_sql, NULL, NULL, NULL), ==, SQLITE_OK);
    return db;
}

static void
_insert_key(sqlite3 *db, const char *hash, const char *client,
            gint64 expires_at, int revoked)
{
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "INSERT INTO api_keys (key_hash, client_name, role, description, "
        "expires_at, revoked) VALUES (?,?,1,'',?,?)", -1, &st, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, client, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, expires_at);
    sqlite3_bind_int(st, 4, revoked);
    g_assert_cmpint(sqlite3_step(st), ==, SQLITE_DONE);
    sqlite3_finalize(st);
}

static gchar *
_verify(sqlite3 *db, const char *hash, gint64 now_epoch)
{
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, APIKEY_VERIFY_SQL, -1, &st, NULL),
                    ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now_epoch);
    gchar *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = g_strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

static gboolean
_has_column(sqlite3 *db, const char *col)
{
    sqlite3_stmt *st = NULL;
    gboolean found = FALSE;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(api_keys)", -1, &st, NULL)
        == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 1);
            if (name && g_strcmp0(name, col) == 0) { found = TRUE; break; }
        }
    }
    sqlite3_finalize(st);
    return found;
}

static void
test_apikey_expiry_never(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_never", "grafana", 0, 0);

    gchar *who = _verify(db, "hash_never", now);
    g_assert_nonnull(who);
    g_assert_cmpstr(who, ==, "grafana");
    g_free(who);
    sqlite3_close(db);
}

static void
test_apikey_expiry_future_accepted(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_future", "ci-bot", now + 3600, 0);

    gchar *who = _verify(db, "hash_future", now);
    g_assert_nonnull(who);
    g_assert_cmpstr(who, ==, "ci-bot");
    g_free(who);
    sqlite3_close(db);
}

static void
test_apikey_expiry_past_rejected(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_past", "expired-bot", now - 3600, 0);

    gchar *who = _verify(db, "hash_past", now);
    g_assert_null(who);
    sqlite3_close(db);
}

static void
test_apikey_expiry_boundary_now_rejected(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_boundary", "edge-bot", now, 0);

    gchar *who = _verify(db, "hash_boundary", now);
    g_assert_null(who);
    sqlite3_close(db);
}

static void
test_apikey_revoked_rejected(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_revoked", "old-bot", now + 100000, 1);

    gchar *who = _verify(db, "hash_revoked", now);
    g_assert_null(who);
    sqlite3_close(db);
}

static void
test_apikey_migrate_columns_idempotent(void)
{
    sqlite3 *db = _open_with(APIKEY_LEGACY_SCHEMA2);

    g_assert_false(_has_column(db, "description"));
    g_assert_false(_has_column(db, "expires_at"));

    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_DESC, NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_EXP,  NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_true(_has_column(db, "description"));
    g_assert_true(_has_column(db, "expires_at"));

    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_DESC, NULL, NULL, NULL), !=, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_EXP,  NULL, NULL, NULL), !=, SQLITE_OK);

    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_migrated", "post-migrate", now + 3600, 0);
    gchar *who = _verify(db, "hash_migrated", now);
    g_assert_cmpstr(who, ==, "post-migrate");
    g_free(who);

    sqlite3_close(db);
}

static void
test_apikey_stored_role_enforced(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    g_assert_cmpint(sqlite3_exec(db, USERS_MIN_SCHEMA, NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db,
        "INSERT INTO users (username, role) VALUES ('root', 2)",
        NULL, NULL, NULL), ==, SQLITE_OK);

    g_assert_cmpint(sqlite3_exec(db,
        "INSERT INTO api_keys (key_hash, client_name, role, description, expires_at, revoked) "
        "VALUES ('hash_root', 'root', 0, '', 0, 0)", NULL, NULL, NULL), ==, SQLITE_OK);

    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db, APIKEY_VERIFY_ROLE_SQL, -1, &st, NULL),
                    ==, SQLITE_OK);
    sqlite3_bind_text(st, 1, "hash_root", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, now);
    g_assert_cmpint(sqlite3_step(st), ==, SQLITE_ROW);
    const char *who = (const char *)sqlite3_column_text(st, 0);
    gint stored_role = sqlite3_column_int(st, 1);
    g_assert_cmpstr(who, ==, "root");
    sqlite3_finalize(st);

    sqlite3_stmt *lu = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "SELECT role FROM users WHERE username = ?", -1, &lu, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(lu, 1, "root", -1, SQLITE_STATIC);
    g_assert_cmpint(sqlite3_step(lu), ==, SQLITE_ROW);
    gint live_role = sqlite3_column_int(lu, 0);
    sqlite3_finalize(lu);

    g_assert_cmpint(stored_role, ==, 0);
    g_assert_cmpint(live_role,   ==, 2);
    g_assert_cmpint(stored_role, !=, live_role);

    const gint admin_min = 2;
    g_assert_false(stored_role >= admin_min);
    g_assert_true (live_role  >= admin_min);

    sqlite3_close(db);
}

/* ── 발급 role 바운딩 (비-admin caller 는 자신 이하 role 만) ─────────────
 *
 * PCV_SAFETY_CONTROL: apikey-role-enforce 의 create-time 바운딩 facet.
 * 진리원: src/api/dispatcher.c::_handle_apikey_create (라인 3946-3958)
 *   (1) role ∉ {VIEWER=0, ADMIN=2} → 거부 (range, 먼저 검사)
 *   (2) role > caller_role       → 거부 (비-admin 발급자는 자신 이하 role 만)
 * 등록된 effect_test(tests/integration/test_apikey_role_enforce.sh)는 저장 role
 * 집행(SEC-3 privesc)만 커버하고, 이 발급-상한 술어는 커버하지 않으므로 여기서
 * 계약을 고정한다. dispatcher.c 술어가 바뀌면 이 재현도 함께 갱신해야 한다. */
typedef enum { CREATE_OK, CREATE_RANGE, CREATE_EXCEEDS } ApikeyCreateVerdict;

static ApikeyCreateVerdict
_apikey_create_bound(gint role, gint caller_role)
{
    const gint VIEWER = 0, ADMIN = 2;
    if (role < VIEWER || role > ADMIN) return CREATE_RANGE;
    if (role > caller_role)            return CREATE_EXCEEDS;
    return CREATE_OK;
}

static void
test_apikey_rolecap_admin_grants_all(void)
{
    g_assert_cmpint(_apikey_create_bound(0, 2), ==, CREATE_OK);
    g_assert_cmpint(_apikey_create_bound(1, 2), ==, CREATE_OK);
    g_assert_cmpint(_apikey_create_bound(2, 2), ==, CREATE_OK);
}

static void
test_apikey_rolecap_nonadmin_bounded(void)
{

    g_assert_cmpint(_apikey_create_bound(2, 1), ==, CREATE_EXCEEDS);
    g_assert_cmpint(_apikey_create_bound(1, 1), ==, CREATE_OK);
    g_assert_cmpint(_apikey_create_bound(0, 1), ==, CREATE_OK);

    g_assert_cmpint(_apikey_create_bound(2, 0), ==, CREATE_EXCEEDS);
    g_assert_cmpint(_apikey_create_bound(1, 0), ==, CREATE_EXCEEDS);
    g_assert_cmpint(_apikey_create_bound(0, 0), ==, CREATE_OK);
}

static void
test_apikey_rolecap_range_first(void)
{
    g_assert_cmpint(_apikey_create_bound(3, 2),  ==, CREATE_RANGE);
    g_assert_cmpint(_apikey_create_bound(-1, 2), ==, CREATE_RANGE);
    g_assert_cmpint(_apikey_create_bound(99, 2), ==, CREATE_RANGE);
}

void
test_apikey_register(void)
{
    g_test_add_func("/apikey/expiry/never_expires",       test_apikey_expiry_never);
    g_test_add_func("/apikey/expiry/future_accepted",     test_apikey_expiry_future_accepted);
    g_test_add_func("/apikey/expiry/past_rejected",       test_apikey_expiry_past_rejected);
    g_test_add_func("/apikey/expiry/boundary_now_rejected", test_apikey_expiry_boundary_now_rejected);
    g_test_add_func("/apikey/revoked_rejected",           test_apikey_revoked_rejected);
    g_test_add_func("/apikey/migrate/columns_idempotent", test_apikey_migrate_columns_idempotent);
    g_test_add_func("/apikey/sec3/stored_role_enforced",  test_apikey_stored_role_enforced);
    g_test_add_func("/apikey/rolecap/admin_grants_all",   test_apikey_rolecap_admin_grants_all);
    g_test_add_func("/apikey/rolecap/nonadmin_bounded",   test_apikey_rolecap_nonadmin_bounded);
    g_test_add_func("/apikey/rolecap/range_first",        test_apikey_rolecap_range_first);
}
