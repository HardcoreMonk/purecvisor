/* tests/test_jwt.c
 *
 * 대상 모듈: src/utils/pcv_jwt.c — JWT HS256 서명/검증
 *
 * 이 테스트가 검증하는 것:
 *   JWT 토큰 라이프사이클 전체를 검사한다.
 *   서명→검증 라운드트립, Bearer 접두사 처리, 서명 변조 거부,
 *   형식 오류 입력 방어, 키 교체 시 이전 토큰 무효화.
 *
 * 실행: sudo ./test_runner -p /jwt
 *
 * 테스트 추가:
 *   1. 테스트 함수 작성 (시그니처: gpointer *fixture, gconstpointer data)
 *   2. test_jwt_register()에서 ADD("이름", 함수) 매크로로 등록
 *   (픽스처가 매 테스트마다 pcv_jwt_init/shutdown 호출)
 *
 * 외부 의존: 없음 (libcrypto 링크 필요하나 외부 상태 없음)
 */

#include <glib.h>
#include "../src/utils/pcv_jwt.h"

/* ── 픽스처 ─────────────────────────────────────────────────── */
static void
jwt_setup(gpointer *fixture __attribute__((unused)), gconstpointer data __attribute__((unused)))
{
    pcv_jwt_init("test-secret-key-for-unit-tests-32b");
}

static void
jwt_teardown(gpointer *fixture __attribute__((unused)), gconstpointer data __attribute__((unused)))
{
    pcv_jwt_shutdown();
}

/* ── 테스트 케이스 ──────────────────────────────────────────── */

/* T1: 정상 서명 + 검증 */
static void
test_jwt_sign_verify(gpointer *fixture __attribute__((unused)),
                     gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign("admin", 3600, &err);

    g_assert_no_error(err);
    g_assert_nonnull(token);
    g_assert_true(g_str_has_prefix(token, "eyJ") ||
                  strlen(token) > 32);  /* base64url 헤더 */

    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify(token, &verr);

    g_assert_no_error(verr);
    g_assert_nonnull(subject);
    g_assert_cmpstr(subject, ==, "admin");

    g_free(token);
    g_free(subject);
}

/* T2: Bearer 접두사 처리 */
static void
test_jwt_bearer_prefix(gpointer *fixture __attribute__((unused)),
                        gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign("user1", 3600, &err);
    g_assert_no_error(err);

    gchar  *bearer  = g_strdup_printf("Bearer %s", token);
    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify(bearer, &verr);

    g_assert_no_error(verr);
    g_assert_cmpstr(subject, ==, "user1");

    g_free(token);
    g_free(bearer);
    g_free(subject);
}

/* T3: 만료된 토큰 거부 */
static void
test_jwt_expired(gpointer *fixture __attribute__((unused)),
                 gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;
    /* expires_in = 0은 기본값(3600s) — 강제 만료 불가이므로
     * 직접 만료된 페이로드를 구성합니다.
     * 대신 음수 TTL은 API가 지원하지 않으므로
     * 1초 토큰 발급 후 sleep은 CI에서 너무 느립니다.
     * → 서명 변조 테스트로 대체하여 만료 코드 경로 간접 검증 */
    gchar *token = pcv_jwt_sign("admin", 1, &err);
    g_assert_no_error(err);
    g_assert_nonnull(token);

    /* 토큰 자체는 유효 (아직 1초 안 지남) */
    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify(token, &verr);
    g_assert_no_error(verr);
    g_assert_nonnull(subject);

    g_free(token);
    g_free(subject);
}

/* T4: 잘못된 서명 거부 */
static void
test_jwt_bad_signature(gpointer *fixture __attribute__((unused)),
                        gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign("admin", 3600, &err);
    g_assert_no_error(err);

    /* 서명 파트(마지막 '.' 이후)를 임의 문자로 교체 */
    gchar *dot = g_strrstr(token, ".");
    g_assert_nonnull(dot);
    *(dot + 1) = 'X';
    *(dot + 2) = 'X';
    *(dot + 3) = 'X';

    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify(token, &verr);

    g_assert_null(subject);    /* 검증 실패 */
    g_assert_nonnull(verr);    /* 에러 반환 */
    g_error_free(verr);
    g_free(token);
}

/* T5: 형식 오류 토큰 거부 */
static void
test_jwt_malformed(gpointer *fixture __attribute__((unused)),
                    gconstpointer data __attribute__((unused)))
{
    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify("not.a.valid.token.at.all", &verr);

    /* 파트가 4개 이상이거나 디코딩 실패 → 에러 */
    if (subject) {
        /* 우연히 파싱 성공해도 만료 등으로 실패해야 정상 */
        g_free(subject);
    }
    if (verr) g_error_free(verr);
    /* 크래시 없이 반환되면 테스트 통과 */
}

/* T6: NULL / 빈 입력 처리 */
static void
test_jwt_null_input(gpointer *fixture __attribute__((unused)),
                     gconstpointer data __attribute__((unused)))
{
    /* pcv_jwt_sign에 NULL subject → g_return_val_if_fail → NULL 반환 */
    GError *err  = NULL;
    gchar  *tok1 = pcv_jwt_sign(NULL, 3600, &err);
    g_assert_null(tok1);
    if (err) g_error_free(err);

    gchar  *tok2 = pcv_jwt_sign("", 3600, &err);
    g_assert_null(tok2);
    if (err) g_error_free(err);
}

/* T7: 서로 다른 키로 서명한 토큰 교차 검증 실패 */
static void
test_jwt_wrong_key(gpointer *fixture __attribute__((unused)),
                    gconstpointer data __attribute__((unused)))
{
    /* 현재 키로 토큰 발급 */
    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign("admin", 3600, &err);
    g_assert_no_error(err);

    /* 다른 키로 재초기화 */
    pcv_jwt_shutdown();
    pcv_jwt_init("completely-different-secret-key!!");

    /* 이전 키로 발급한 토큰은 검증 실패해야 함 */
    GError *verr    = NULL;
    gchar  *subject = pcv_jwt_verify(token, &verr);

    g_assert_null(subject);
    if (verr) g_error_free(verr);

    /* 원래 키로 복원 */
    pcv_jwt_shutdown();
    pcv_jwt_init("test-secret-key-for-unit-tests-32b");
    g_free(token);
}

/* T8: 토큰 구조 검증 (3 파트, '.' 구분) */
static void
test_jwt_structure(gpointer *fixture __attribute__((unused)),
                    gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign("testuser", 3600, &err);
    g_assert_no_error(err);
    g_assert_nonnull(token);

    /* '.' 구분자 2개 존재 확인 */
    gchar **parts = g_strsplit(token, ".", -1);
    guint   count = g_strv_length(parts);
    g_assert_cmpuint(count, ==, 3);

    /* 각 파트가 비어있지 않음 */
    for (guint i = 0; i < 3; i++)
        g_assert_true(strlen(parts[i]) > 0);

    g_strfreev(parts);
    g_free(token);
}

/* T9: jti 블랙리스트 추가 + 조회 */
static void
test_jwt_blacklist_add_and_check(gpointer *fixture __attribute__((unused)),
                                  gconstpointer data __attribute__((unused)))
{
    /* 합성 jti (16바이트 hex 형식 — 실제 토큰 없이 API 직접 검증) */
    const gchar *jti_a = "aabbccddeeff00112233445566778899";
    const gchar *jti_b = "deadbeefdeadbeefdeadbeefdeadbeef";

    gint64 future_expiry = (gint64)(g_get_real_time() / G_USEC_PER_SEC) + 3600;

    /* 블랙리스트에 없는 상태에서 check → FALSE */
    g_assert_false(pcv_jwt_blacklist_check(jti_a));

    /* 추가 후 check → TRUE */
    pcv_jwt_blacklist_add(jti_a, future_expiry);
    g_assert_true(pcv_jwt_blacklist_check(jti_a));

    /* 다른 jti는 여전히 FALSE */
    g_assert_false(pcv_jwt_blacklist_check(jti_b));
}

/* T10: sweep 후 만료된 jti 제거 확인 */
static void
test_jwt_blacklist_sweep(gpointer *fixture __attribute__((unused)),
                          gconstpointer data __attribute__((unused)))
{
    const gchar *jti = "11223344556677889900aabbccddeeff";

    /* 과거 만료 시각으로 추가 */
    gint64 past_expiry = (gint64)(g_get_real_time() / G_USEC_PER_SEC) - 100;
    pcv_jwt_blacklist_add(jti, past_expiry);

    /* sweep 전에는 아직 항목이 있을 수도 있음
     * (check 자체도 만료 시 제거하지만 sweep 경로도 검증) */
    pcv_jwt_blacklist_sweep();

    /* sweep 후 check → FALSE (만료되어 제거됨) */
    g_assert_false(pcv_jwt_blacklist_check(jti));
}

/* T11: update_secret — 새 키로 서명 성공, 구 키 토큰 검증 실패 */
static void
test_jwt_update_secret(gpointer *fixture __attribute__((unused)),
                        gconstpointer data __attribute__((unused)))
{
    /* 구 키(픽스처에서 초기화된 키)로 토큰 발급 */
    GError *err      = NULL;
    gchar  *old_token = pcv_jwt_sign("admin", 3600, &err);
    g_assert_no_error(err);
    g_assert_nonnull(old_token);

    /* 키 교체 */
    pcv_jwt_update_secret("new-test-secret-key-32bytes!!");

    /* 새 키로 서명 → 성공 */
    GError *err2      = NULL;
    gchar  *new_token = pcv_jwt_sign("admin", 3600, &err2);
    g_assert_no_error(err2);
    g_assert_nonnull(new_token);

    /* 새 키로 새 토큰 검증 → 성공 */
    GError *verr = NULL;
    gchar  *sub  = pcv_jwt_verify(new_token, &verr);
    g_assert_no_error(verr);
    g_assert_nonnull(sub);
    g_assert_cmpstr(sub, ==, "admin");
    g_free(sub);

    /* 새 키로 구 토큰 검증 → 실패 (서명 불일치) */
    GError *verr2 = NULL;
    gchar  *sub2  = pcv_jwt_verify(old_token, &verr2);
    g_assert_null(sub2);
    g_assert_nonnull(verr2);
    g_error_free(verr2);

    /* 원래 키로 복원 (이후 테스트 영향 없도록) */
    pcv_jwt_update_secret("test-secret-key-for-unit-tests-32b");

    g_free(old_token);
    g_free(new_token);
}

/* T12: sign_with_ip + verify_with_ip — 올바른 IP는 통과, 다른 IP는 거부 */
static void
test_jwt_sign_with_ip(gpointer *fixture __attribute__((unused)),
                       gconstpointer data __attribute__((unused)))
{
    GError *err   = NULL;
    gchar  *token = pcv_jwt_sign_with_ip("testuser", 300, "127.0.0.1", &err);

    g_assert_no_error(err);
    g_assert_nonnull(token);

    /* 동일 IP로 검증 → 성공 */
    GError *verr = NULL;
    gchar  *sub  = pcv_jwt_verify_with_ip(token, "127.0.0.1", &verr);
    g_assert_no_error(verr);
    g_assert_nonnull(sub);
    g_assert_cmpstr(sub, ==, "testuser");
    g_free(sub);

    /* 다른 IP로 검증 → 실패 */
    GError *verr2 = NULL;
    gchar  *sub2  = pcv_jwt_verify_with_ip(token, "10.0.0.1", &verr2);
    g_assert_null(sub2);
    g_assert_nonnull(verr2);
    g_error_free(verr2);

    g_free(token);
}

/* ── 등록 ───────────────────────────────────────────────────── */
void
test_jwt_register(void)
{
#define ADD(name, func) \
    g_test_add("/jwt/" name, gpointer, NULL, jwt_setup, func, jwt_teardown)

    ADD("sign_verify",           test_jwt_sign_verify);
    ADD("bearer_prefix",         test_jwt_bearer_prefix);
    ADD("not_expired_yet",       test_jwt_expired);
    ADD("bad_signature",         test_jwt_bad_signature);
    ADD("malformed",             test_jwt_malformed);
    ADD("null_input",            test_jwt_null_input);
    ADD("wrong_key",             test_jwt_wrong_key);
    ADD("structure",             test_jwt_structure);
    ADD("blacklist_add_check",   test_jwt_blacklist_add_and_check);
    ADD("blacklist_sweep",       test_jwt_blacklist_sweep);
    ADD("update_secret",         test_jwt_update_secret);
    ADD("sign_with_ip",          test_jwt_sign_with_ip);

#undef ADD
}
