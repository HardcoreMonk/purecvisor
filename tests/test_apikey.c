/* tests/test_apikey.c
 *
 * apikey.create 계약 확장 — 만료(expires_at) 집행 + schema 컬럼 마이그레이션의
 * 계약 수준 회귀 테스트.
 *
 * [왜 self-contained SQLite 인가]
 *   test_runner 는 COMMON_CORE_SRCS 만 링크하고 src/modules/auth/pcv_rbac.c 는
 *   링크하지 않는다 (그래서 tests/test_stubs.c 가 pcv_rbac_apikey_revoke 를 스텁한다).
 *   따라서 실 함수 pcv_rbac_verify_api_key()/pcv_rbac_apikey_create() 를 직접
 *   호출할 수 없다. 대신 production 이 사용하는 것과 '동일한' canonical schema 와
 *   만료 판정 술어(WHERE revoked=0 AND (expires_at=0 OR expires_at > ?))를 이
 *   테스트에서 재현하여 계약(스키마 + 집행 규칙)을 고정한다.
 *
 *   진리원: src/modules/auth/pcv_rbac.c
 *     - _ensure_apikey_table()      (canonical schema#2 + description/expires_at)
 *     - _migrate_apikey_columns()   (기존 schema#2 → 컬럼 멱등 ALTER)
 *     - pcv_rbac_verify_api_key()   (revoked=0 + expires_at 집행)
 *     - pcv_rbac_apikey_validate()  (동일 만료 술어)
 *   위 함수들이 바뀌면 이 상수/술어도 함께 갱신해야 한다.
 */
#include <glib.h>
#include <sqlite3.h>

/* production _ensure_apikey_table() 과 동일한 canonical schema#2(+계약 확장 컬럼) */
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

/* production F8 이후, 계약 확장 이전의 schema#2 (description/expires_at 없음) */
#define APIKEY_LEGACY_SCHEMA2 \
    "CREATE TABLE api_keys (" \
    "  key_hash     TEXT PRIMARY KEY," \
    "  client_name  TEXT NOT NULL," \
    "  role         INTEGER NOT NULL DEFAULT 1," \
    "  created_at   TEXT NOT NULL DEFAULT (datetime('now'))," \
    "  last_used_at TEXT," \
    "  revoked      INTEGER NOT NULL DEFAULT 0" \
    ")"

/* production pcv_rbac_verify_api_key() 의 만료 집행 술어와 동일 */
#define APIKEY_VERIFY_SQL \
    "SELECT client_name FROM api_keys " \
    "WHERE key_hash = ? AND revoked = 0 " \
    "AND (expires_at = 0 OR expires_at > ?)"

/* SEC-3: 저장 role 을 함께 조회하는 신(수정 후) 파생 — production verify_api_key 와 동일
 * (SELECT client_name, role ...). 이 저장 role 이 키의 실효 role 이다. */
#define APIKEY_VERIFY_ROLE_SQL \
    "SELECT client_name, role FROM api_keys " \
    "WHERE key_hash = ? AND revoked = 0 " \
    "AND (expires_at = 0 OR expires_at > ?)"

/* 반사실용 users 테이블 (production _ensure_table() 의 부분집합: 파생에 쓰는 두 컬럼) */
#define USERS_MIN_SCHEMA \
    "CREATE TABLE users (" \
    "  username TEXT PRIMARY KEY NOT NULL," \
    "  role     INTEGER NOT NULL DEFAULT 0" \
    ")"

/* _migrate_apikey_columns() 이 실행하는 멱등 ALTER 두 건 */
#define APIKEY_ALTER_DESC "ALTER TABLE api_keys ADD COLUMN description TEXT NOT NULL DEFAULT ''"
#define APIKEY_ALTER_EXP  "ALTER TABLE api_keys ADD COLUMN expires_at  INTEGER NOT NULL DEFAULT 0"

/* ── 헬퍼 ─────────────────────────────────────────────────── */

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

/* production verify 술어 재현 — 유효하면 client_name(g_strdup), 아니면 NULL */
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

/* ── 만료 집행 ────────────────────────────────────────────── */

/* expires_at = 0 → 무기한, 항상 수락 */
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

/* 미래 만료 → 수락 */
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

/* 과거 만료 → 거부 (핵심: 만료 키 거부 집행) */
static void
test_apikey_expiry_past_rejected(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_past", "expired-bot", now - 3600, 0);

    gchar *who = _verify(db, "hash_past", now);
    g_assert_null(who);   /* 만료 → 인증 실패 */
    sqlite3_close(db);
}

/* 경계: expires_at == now → '초과(>)'가 아니므로 거부 */
static void
test_apikey_expiry_boundary_now_rejected(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_boundary", "edge-bot", now, 0);

    gchar *who = _verify(db, "hash_boundary", now);
    g_assert_null(who);   /* expires_at > now 실패 → 거부 */
    sqlite3_close(db);
}

/* revoked=1 은 만료 여부와 무관하게 거부 */
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

/* ── 컬럼 마이그레이션 멱등성 ─────────────────────────────── */

/* 기존 schema#2(컬럼 부재) → ALTER 로 description/expires_at 보강, 재실행 무해 */
static void
test_apikey_migrate_columns_idempotent(void)
{
    sqlite3 *db = _open_with(APIKEY_LEGACY_SCHEMA2);

    g_assert_false(_has_column(db, "description"));
    g_assert_false(_has_column(db, "expires_at"));

    /* 1차 ALTER — 컬럼 추가 성공 */
    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_DESC, NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_EXP,  NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_true(_has_column(db, "description"));
    g_assert_true(_has_column(db, "expires_at"));

    /* 2차 ALTER — 이미 존재 → 에러이나 production 은 무시(멱등). 여기선 non-OK 확인 */
    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_DESC, NULL, NULL, NULL), !=, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, APIKEY_ALTER_EXP,  NULL, NULL, NULL), !=, SQLITE_OK);

    /* 보강된 컬럼으로 삽입 + 만료 술어 동작 확인 */
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    _insert_key(db, "hash_migrated", "post-migrate", now + 3600, 0);
    gchar *who = _verify(db, "hash_migrated", now);
    g_assert_cmpstr(who, ==, "post-migrate");
    g_free(who);

    sqlite3_close(db);
}

/* ── SEC-3: API 키 실효 role 파생 계약 (저장 role 집행 vs 라이브 role 반사실) ── */

/* 키의 client_name 이 admin 사용자명이지만 저장 role 이 VIEWER(0)일 때,
 *   신(수정 후) 파생 = 저장 role → VIEWER → admin gate(min=2)에 (0>=2)==FALSE DENIED,
 *   구(수정 전, 반사실) 파생 = client_name 라이브 사용자 role → ADMIN → (2>=2)==TRUE ALLOWED.
 * 두 파생 role 이 서로 달라야 SEC-3 privesc(저-role admin명 키의 admin 승격)가 인코딩된다.
 * 진리원: pcv_rbac_verify_api_key(신 SELECT) / pcv_rbac_check_permission(role>=min_role). */
static void
test_apikey_stored_role_enforced(void)
{
    sqlite3 *db = _open_with(APIKEY_CANONICAL_SCHEMA);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    /* users: admin 'root' role=2(ADMIN) */
    g_assert_cmpint(sqlite3_exec(db, USERS_MIN_SCHEMA, NULL, NULL, NULL), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db,
        "INSERT INTO users (username, role) VALUES ('root', 2)",
        NULL, NULL, NULL), ==, SQLITE_OK);

    /* api_keys: client_name='root'(admin 사용자명), 저장 role=0(VIEWER) — SEC-3 공격 형상 */
    g_assert_cmpint(sqlite3_exec(db,
        "INSERT INTO api_keys (key_hash, client_name, role, description, expires_at, revoked) "
        "VALUES ('hash_root', 'root', 0, '', 0, 0)", NULL, NULL, NULL), ==, SQLITE_OK);

    /* 신 파생: 키의 저장 role 조회 (client_name + role) */
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

    /* 구 파생(반사실): client_name 의 라이브 사용자 role 조회 */
    sqlite3_stmt *lu = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(db,
        "SELECT role FROM users WHERE username = ?", -1, &lu, NULL), ==, SQLITE_OK);
    sqlite3_bind_text(lu, 1, "root", -1, SQLITE_STATIC);
    g_assert_cmpint(sqlite3_step(lu), ==, SQLITE_ROW);
    gint live_role = sqlite3_column_int(lu, 0);
    sqlite3_finalize(lu);

    /* 계약: 저장 role(신)=VIEWER(0), 라이브 role(구)=ADMIN(2), 상이 */
    g_assert_cmpint(stored_role, ==, 0);   /* PCV_ROLE_VIEWER */
    g_assert_cmpint(live_role,   ==, 2);   /* PCV_ROLE_ADMIN */
    g_assert_cmpint(stored_role, !=, live_role);

    /* admin gate(min_role=ADMIN=2) 판정: role >= min_role */
    const gint admin_min = 2;
    g_assert_false(stored_role >= admin_min);  /* 신: (0>=2)==FALSE → DENIED (privesc 차단) */
    g_assert_true (live_role  >= admin_min);   /* 구: (2>=2)==TRUE  → ALLOWED (SEC-3 결함) */

    sqlite3_close(db);
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
}
