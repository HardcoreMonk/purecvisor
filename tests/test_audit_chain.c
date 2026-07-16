/* tests/test_audit_chain.c
 *
 * 감사 로그 해시체인(A09/2.9) 위변조 탐지 효과 테스트.
 *
 * [왜 self-contained SQLite 인가]
 *   test_runner 는 tests/test_stubs.c 가 pcv_audit_log() 를 스텁 정의하므로
 *   src/modules/audit/pcv_audit.c 를 링크할 수 없다 (pcv_audit_log 중복 심볼).
 *   또한 production 기록 경로는 async 워커 스레드를 거친다. 따라서 실 함수
 *   pcv_audit_verify_chain() 을 직접 구동하는 대신, production 이 사용하는 것과
 *   '동일한' 체인 해시 공식과 검증 규칙을 이 테스트에서 재현하여, 체인이 실제로
 *   수정·삭제를 탐지함을 고정한다 (test_apikey.c 와 동일 선례).
 *
 *   진리원: src/modules/audit/pcv_audit.c
 *     - _sha256_hex()              (EVP SHA-256 → 64 hex)
 *     - _audit_rec_hash()          (prev|ts|user|method|target|result|code)
 *     - pcv_audit_verify_chain()   (① prev_hash 링크 연속성 ② rec_hash 재계산)
 *     - AUDIT_CHAIN_GENESIS        (64자 0)
 *   위 공식/규칙이 바뀌면 이 재현도 함께 갱신해야 한다.
 */
#include <glib.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <string.h>

#define CHAIN_GENESIS \
    "0000000000000000000000000000000000000000000000000000000000000000"

/* production _sha256_hex() 재현 (EVP) */
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

/* production _audit_rec_hash() 재현 (동일 프리이미지 포맷) */
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

/* production pcv_audit_verify_chain() 재현 — 반환 TRUE=무결, 아니면 FALSE + break rowid */
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
        if (!rec_hash || !*rec_hash) continue;   /* 체인 밖 (마이그레이션 전) */

        gsize        rowid = (gsize)sqlite3_column_int64(st, 0);
        const gchar *ts    = (const gchar *)sqlite3_column_text(st, 1);
        const gchar *user  = (const gchar *)sqlite3_column_text(st, 2);
        const gchar *meth  = (const gchar *)sqlite3_column_text(st, 3);
        const gchar *targ  = (const gchar *)sqlite3_column_text(st, 4);
        const gchar *res   = (const gchar *)sqlite3_column_text(st, 5);
        gint         ecode = sqlite3_column_int(st, 6);
        const gchar *prev  = (const gchar *)sqlite3_column_text(st, 7);
        const gchar *prev_norm = prev ? prev : "";

        if (g_strcmp0(prev_norm, expected_prev) != 0) {   /* ① 링크 연속성 */
            if (first_break_rowid) *first_break_rowid = rowid;
            ok = FALSE; break;
        }
        gchar *recomputed = _rec_hash(prev_norm, ts, user, meth, targ, res, ecode);
        gboolean match = (g_strcmp0(recomputed, rec_hash) == 0);   /* ② 무결성 */
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

/* ── 헬퍼: production 워커의 INSERT 경로 재현 (체인 헤드 전진 포함) ── */

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

/* chain_head 를 갱신하며 한 행 INSERT (production _audit_worker 성공 경로 재현) */
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
    *chain_head = rec;   /* 헤드 전진 */
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

/* ── 테스트 ─────────────────────────────────────────────────── */

/* 정상 체인은 무결 검증(PASS) */
static void
test_audit_chain_intact_passes(void)
{
    sqlite3 *db = _build_three_row_chain();
    gsize brk = 12345;
    g_assert_true(_verify_chain(db, &brk));
    g_assert_cmpuint(brk, ==, 0);
    sqlite3_close(db);
}

/* 핵심: 한 행의 필드 변조 → rec_hash 재계산 불일치로 그 rowid 에서 break */
static void
test_audit_chain_field_tamper_detected(void)
{
    sqlite3 *db = _build_three_row_chain();

    /* row 2 의 result 를 'ok' → 'denied' 로 몰래 변조 (rec_hash 는 갱신 안 함) */
    g_assert_cmpint(sqlite3_exec(db,
        "UPDATE audit_log SET result='denied' WHERE id=2", NULL, NULL, NULL),
        ==, SQLITE_OK);

    gsize brk = 0;
    g_assert_false(_verify_chain(db, &brk));   /* 위변조 탐지 */
    g_assert_cmpuint(brk, ==, 2);              /* 정확히 변조된 행에서 break */
    sqlite3_close(db);
}

/* 핵심: 중간 행 삭제 → 다음 행 prev_hash 링크 단절로 break */
static void
test_audit_chain_row_delete_detected(void)
{
    sqlite3 *db = _build_three_row_chain();

    /* row 2 를 통째로 삭제 — row 3 의 prev_hash 는 여전히 row2.rec_hash 를 가리켜
     * expected_prev(=row1.rec_hash)와 불일치 → 링크 연속성 위반 */
    g_assert_cmpint(sqlite3_exec(db,
        "DELETE FROM audit_log WHERE id=2", NULL, NULL, NULL), ==, SQLITE_OK);

    gsize brk = 0;
    g_assert_false(_verify_chain(db, &brk));
    g_assert_cmpuint(brk, ==, 3);   /* 링크 단절은 뒤따르는 row 3 에서 드러난다 */
    sqlite3_close(db);
}

/* rec_hash 자체를 직접 변조해도 재계산 불일치로 탐지 */
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

/* 마이그레이션 전(rec_hash NULL) 레코드는 체인 밖 — 무결 취급, 이후 체인은 검증 */
static void
test_audit_chain_null_prechain_skipped(void)
{
    sqlite3 *db = _open_audit_db();
    /* 레거시 행: rec_hash NULL */
    g_assert_cmpint(sqlite3_exec(db,
        "INSERT INTO audit_log(ts,username,method,target,result,error_code,prev_hash,rec_hash)"
        " VALUES('2026-07-15 00:00:00','old','x','y','ok',0,NULL,NULL)",
        NULL, NULL, NULL), ==, SQLITE_OK);
    /* 이후 체인 행 (id=2 부터, prev=genesis) */
    gchar *head = NULL;
    _append(db, &head, "2026-07-16 00:00:01", "admin", "auth.login", "admin", "ok", 0);
    g_free(head);

    gsize brk = 123;
    g_assert_true(_verify_chain(db, &brk));   /* NULL 행 skip, 체인 무결 */
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
