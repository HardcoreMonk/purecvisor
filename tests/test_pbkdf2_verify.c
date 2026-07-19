/* tests/test_pbkdf2_verify.c
 *
 * Q-4 회귀 테스트: PBKDF2 비밀번호 검증의 레거시/신형 포맷 하위호환 고정.
 *
 * Wave B 하드닝에서 PBKDF2 저장 포맷이 두 가지로 갈렸다:
 *   - 신형:   "pbkdf2:<iter>:<64hex>"   (반복수 임베딩, 예 pbkdf2:600000:…)
 *   - 레거시: "pbkdf2:<64hex>"          (반복수 미기록 → 암묵 100000)
 * pcv_rbac.c 의 _pbkdf2_parse() 가 레거시 포맷을 iter=100000 으로 해석해야
 * 반복수 상향(600000) 이후에도 기존 사용자가 로그인 가능하다(무락아웃). 이
 * 하위호환이 깨지면 전 사용자 로그인 불가 장애가 재발하므로 아래 테스트로 고정한다.
 *
 * pcv_rbac.c 는 test_runner 에 실제로 링크되어 있으므로(Makefile TEST_COMMON_SRCS),
 * 실제 컴파일된 pcv_rbac_authenticate() 의 검증 경로를 그대로 구동한다. 레거시
 * 해시는 OpenSSL PKCS5_PBKDF2_HMAC 로 _pbkdf2_hex() 와 동일하게 계산해 users
 * 테이블에 직접 INSERT(두 번째 sqlite3 연결)한 뒤, authenticate 로 검증한다.
 * (직접 seed 방식은 tests/test_rbac_user_exists.c 의 raw 연결 관례를 따른다.)
 *
 * 각 테스트는 독립된 임시 DB 로 pcv_jwt_init()/pcv_rbac_init()/shutdown 라이프사이클을
 * 거친다(WAL 모드라 -wal/-shm 파일도 함께 정리).
 */
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
    /* authenticate 성공 시 JWT 를 서명하므로 JWT 를 먼저 초기화한다. */
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

/* pcv_rbac.c 의 _pbkdf2_hex() 와 동일한 계산: PBKDF2-HMAC-SHA256, 32바이트 파생키,
 * 소문자 %02x hex(접두사 없음). salt 는 문자열 그대로 PBKDF2 salt 로 사용한다. */
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

/* users 테이블에 특정 password_hash/salt 로 사용자 한 명을 직접 INSERT.
 * (pcv_rbac_user_create 는 항상 신형 포맷을 쓰므로, 레거시 포맷을 강제로 심으려면
 *  raw sqlite3 연결로 직접 넣는다. WAL 이라 커밋 후 g_rbac_db 에서도 보인다.) */
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

/* ── 신형 포맷: pcv_rbac_user_create 가 만든 "pbkdf2:<iter>:<hex>" 검증 ── */

static void
test_new_format_correct_password_verifies(void)
{
    setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("u_new_ok", "Str0ng!Passw0rd",
                                       PCV_ROLE_ADMIN, NULL, &err));
    g_assert_no_error(err);

    gchar *tok = pcv_rbac_authenticate("u_new_ok", "Str0ng!Passw0rd", &err);
    g_assert_nonnull(tok);   /* 올바른 비번 → JWT 발급 */
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
    g_assert_null(tok);      /* 틀린 비번 → 거부 */
    g_clear_error(&err);
    teardown();
}

/* ── 레거시 포맷: "pbkdf2:<hex>" (iter 미기록 → 암묵 100000) 검증 ── */

static void
test_legacy_format_correct_password_verifies(void)
{
    setup();
    const gchar *salt = "a1b2c3d4e5f60718";
    const gchar *pw   = "L3gacyPa$$word!";
    /* 레거시는 반복수 100000 으로 계산해 "pbkdf2:<hex>" (iter 필드 없음) 로 저장. */
    gchar *hex  = compute_pbkdf2_hex(salt, pw, 100000);
    gchar *hash = g_strconcat("pbkdf2:", hex, NULL);
    seed_user("u_legacy_ok", hash, salt);
    g_free(hex); g_free(hash);

    GError *err = NULL;
    gchar *tok = pcv_rbac_authenticate("u_legacy_ok", pw, &err);
    g_assert_nonnull(tok);   /* 레거시 해시 + 올바른 비번 → 검증 성공(무락아웃) */
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
    g_assert_null(tok);      /* 레거시 해시 + 틀린 비번 → 거부 */
    g_clear_error(&err);
    teardown();
}

/* 레거시 암묵 반복수가 정확히 100000 임을 못박는다: 100000 이 아닌 반복수로 만든
 * 해시를 레거시 포맷("pbkdf2:<hex>")으로 저장하면, parse 는 iter=100000 으로
 * 재계산하므로 올바른 비번이라도 검증 실패해야 한다(암묵 반복수 회귀 방지). */
static void
test_legacy_implicit_iterations_are_100000(void)
{
    setup();
    const gchar *salt = "9988776655443322";
    const gchar *pw   = "L3gacyPa$$word!";
    /* 200000 반복으로 만든 파생키를 iter 없는 레거시 포맷으로 저장 → 100000 로
     * 검증되므로 불일치해야 한다. */
    gchar *hex  = compute_pbkdf2_hex(salt, pw, 200000);
    gchar *hash = g_strconcat("pbkdf2:", hex, NULL);
    seed_user("u_legacy_iter", hash, salt);
    g_free(hex); g_free(hash);

    GError *err = NULL;
    gchar *tok = pcv_rbac_authenticate("u_legacy_iter", pw, &err);
    g_assert_null(tok);      /* 반복수 불일치 → 검증 실패(암묵 iter=100000 고정 확인) */
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
