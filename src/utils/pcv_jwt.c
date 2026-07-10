/**
 * @file pcv_jwt.c
 * @brief JWT (JSON Web Token) HS256 서명/검증 — REST API 인증 핵심 모듈
 *
 * Sprint E에서 도입된 JWT 인증 모듈입니다.
 * REST API(rest_server.c)의 /auth/token 엔드포인트에서 토큰을 발행하고,
 * 모든 JWT 필수 엔드포인트에서 Bearer 토큰을 검증합니다.
 *
 * [아키텍처 위치]
 *   클라이언트 → POST /api/v1/auth/token {username, password}
 *             → rest_server.c가 RBAC 인증 후 pcv_jwt_sign("admin", 3600) 호출
 *             → JWT 토큰 반환
 *   클라이언트 → GET /api/v1/vms (Authorization: Bearer <token>)
 *             → rest_server.c가 pcv_jwt_verify(token) 호출
 *             → subject(사용자명) 추출 후 RBAC 권한 확인
 *
 * [JWT 구조 (RFC 7519)]
 *   <header>.<payload>.<signature>
 *   header  : {"alg":"HS256","typ":"JWT"} (고정 문자열, 변경 불가)
 *   payload : {"sub":"admin","iat":1711234567,"exp":1711238167}
 *   signature: HMAC-SHA256(secret, header_b64 + "." + payload_b64)
 *   각 파트는 base64url 인코딩 (RFC 4648 Section 5, padding 제거)
 *
 * [핵심 흐름]
 *   pcv_jwt_sign():
 *     1. header JSON → base64url 인코딩
 *     2. payload JSON (sub, iat, exp) → base64url 인코딩
 *     3. signing_input = header_b64 + "." + payload_b64
 *     4. HMAC-SHA256(secret, signing_input) → signature
 *     5. token = signing_input + "." + sig_b64
 *
 *   pcv_jwt_verify():
 *     1. "Bearer " 접두사 제거
 *     2. 토큰을 "."으로 3파트 분리
 *     3. 서명 재계산 + CRYPTO_memcmp() 상수 시간 비교 (timing attack 방지)
 *     4. payload 디코딩 → exp 만료 확인 → sub 추출
 *
 * [보안 고려사항]
 *   - CRYPTO_memcmp(): 상수 시간 비교로 timing attack 방지
 *     일반 memcmp()는 첫 바이트 불일치에서 즉시 반환하므로,
 *     공격자가 응답 시간 차이로 올바른 서명을 유추할 수 있습니다.
 *   - secret 미설정 시 /dev/urandom 32바이트 랜덤 키 자동 생성
 *     (재시작 시 기존 토큰 무효 — 운영 시 daemon.conf에 jwt_secret 설정 권장)
 *   - g_jwt_mutex: sign/verify 동시 호출 시 secret 접근 보호
 *     (REST 서버가 멀티스레드로 요청을 처리하므로 동기화 필요)
 *
 * [다른 모듈과의 관계]
 *   - pcv_config.c    : jwt_secret 설정값 제공
 *   - rest_server.c   : /auth/token에서 sign() 호출, 모든 JWT 엔드포인트에서 verify()
 *   - pcv_rbac.c      : verify()가 반환한 subject로 역할(VIEWER/OPERATOR/ADMIN) 확인
 *
 * [의존성]
 *   GLib (g_base64_encode/decode), json-glib, OpenSSL libcrypto (HMAC, CRYPTO_memcmp)
 *
 * [주의사항]
 *   - _b64url_encode/decode: 표준 base64를 base64url로 변환 (+→-, /→_, = 제거)
 *   - expires_in=0이면 JWT_DEFAULT_EXPIRY(3600초=1시간) 적용
 *   - verify 반환값: subject 문자열 (호출자 g_free 필수), 실패 시 NULL + GError
 *   - HS256만 지원 (RS256/ES256 미지원). 단일 클러스터 환경에서 대칭키로 충분
 */

#include "pcv_jwt.h"
#include "pcv_log.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <string.h>
#include <time.h>

/* ── 모듈 상태 ──────────────────────────────────────────────── */

/**
 * g_jwt_secret - HMAC-SHA256 서명 키 (바이너리 데이터)
 *
 * pcv_jwt_init()에서 설정됩니다.
 * 문자열 키(daemon.conf에서 설정) 또는 32바이트 랜덤 바이너리 키.
 * sign/verify 모두 이 키를 사용하므로 뮤텍스로 보호합니다.
 */
static gchar  *g_jwt_secret  = NULL;

/* B6-W3 (Phase 3 fix): in-memory jti blacklist (logout 후 토큰 무효화).
 * GHashTable<jti_hex, expiry_unix> — sweeper가 만료된 entry 자동 제거.
 * 재시작 시 비워지지만 access token TTL이 짧아 (15분 default) 영향 제한적.
 * 영구 blacklist 필요 시 SQLite 마이그레이션 가능. */
static GHashTable *g_jti_blacklist = NULL;
static GMutex      g_jti_blacklist_mu;

/** g_jwt_secret_len - 시크릿 키의 바이트 길이 */
static gsize   g_jwt_secret_len = 0;

/**
 * g_jwt_mutex - 시크릿 키 접근 동기화 뮤텍스
 *
 * REST 서버가 GLib 스레드 풀에서 요청을 병렬 처리하므로,
 * pcv_jwt_sign()과 pcv_jwt_verify()가 동시에 g_jwt_secret에
 * 접근할 수 있습니다. 읽기 전용이지만 pcv_jwt_shutdown()에서
 * free되므로 뮤텍스로 보호합니다.
 */
static GMutex  g_jwt_mutex;

/** JWT_LOG_DOM - 이 모듈의 로그 도메인. journalctl에서 필터링에 사용 */
#define JWT_LOG_DOM "pcv_jwt"

/** JWT_DEFAULT_EXPIRY - expires_in=0일 때 적용되는 기본 만료 시간 (1시간) */
#define JWT_DEFAULT_EXPIRY 3600   /* 1시간 */

static void
_fill_random_bytes(guchar *buf, gsize len)
{
    gboolean filled = FALSE;

    g_return_if_fail(buf != NULL);

    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        filled = fread(buf, 1, len, f) == len;
        fclose(f);
    }

    if (!filled) {
        for (gsize i = 0; i < len; i++)
            buf[i] = (guchar)g_random_int_range(0, 256);
    }
}

/* ── base64url 인코딩 (RFC 4648 Section 5, padding 제거) ──── */

/**
 * _b64url_encode - 표준 base64를 base64url로 변환
 * @data: 인코딩할 바이너리 데이터
 * @len:  데이터 길이 (바이트)
 *
 * @return: base64url 인코딩된 문자열 (호출자 g_free 필수)
 *
 * [base64 vs base64url]
 *   표준 base64: ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=
 *   base64url:   ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_
 *
 *   변환 규칙:
 *     '+' → '-'  (URL에서 +는 공백으로 해석됨)
 *     '/' → '_'  (URL 경로 구분자와 충돌)
 *     '=' 제거   (JWT에서 padding 불필요, 디코딩 시 복원 가능)
 *
 *   이 변환이 필요한 이유: JWT 토큰은 HTTP Authorization 헤더,
 *   URL 쿼리 파라미터 등에 포함되므로 URL-safe 문자셋이 필수입니다.
 */
static gchar *
_b64url_encode(const guchar *data, gsize len)
{
    /* GLib의 표준 base64 인코딩 */
    gchar *b64 = g_base64_encode(data, len);

    /* 표준 base64 → base64url 문자 치환 */
    for (gchar *p = b64; *p; p++) {
        if (*p == '+') *p = '-';
        else if (*p == '/') *p = '_';
    }

    /* padding('=') 제거 — JWT 사양에 따라 padding 미사용 */
    gsize blen = strlen(b64);
    while (blen > 0 && b64[blen - 1] == '=')
        b64[--blen] = '\0';
    return b64;
}

/**
 * _b64url_decode - base64url을 바이너리 데이터로 디코딩
 * @str:     base64url 인코딩된 문자열
 * @out_len: (출력) 디코딩된 데이터 길이
 *
 * @return: 디코딩된 바이너리 데이터 (호출자 g_free 필수)
 *
 * [디코딩 과정]
 *   1. base64url → 표준 base64 문자 복원 ('-'→'+', '_'→'/')
 *   2. padding 복원 (4의 배수가 되도록 '=' 추가)
 *   3. GLib g_base64_decode()로 디코딩
 *
 * [padding 복원 규칙]
 *   base64는 항상 4의 배수 길이여야 합니다.
 *   모자란 만큼 '='를 추가합니다:
 *     길이 % 4 == 0 → padding 0개
 *     길이 % 4 == 2 → padding 2개 (==)
 *     길이 % 4 == 3 → padding 1개 (=)
 *     길이 % 4 == 1 → 잘못된 입력 (실제로는 base64에서 발생하지 않음)
 */
static guchar *
_b64url_decode(const gchar *str, gsize *out_len)
{
    /* padding 복원을 위해 GString에 복사 */
    gsize slen = strlen(str);
    gsize pad  = (4 - (slen % 4)) % 4;   /* 4의 배수에 모자란 만큼 */
    GString *padded = g_string_new(str);

    /* base64url → 표준 base64 문자 복원 */
    for (gchar *p = padded->str; *p; p++) {
        if (*p == '-') *p = '+';
        else if (*p == '_') *p = '/';
    }

    /* padding 복원 */
    for (gsize i = 0; i < pad; i++)
        g_string_append_c(padded, '=');

    guchar *decoded = g_base64_decode(padded->str, out_len);
    g_string_free(padded, TRUE);  /* TRUE: 내부 문자열도 해제 */
    return decoded;
}

/* ── HMAC-SHA256 계산 ──────────────────────────────────────── */

/**
 * _hmac_sha256 - HMAC-SHA256 메시지 인증 코드 계산
 * @key:         HMAC 키 (g_jwt_secret)
 * @key_len:     키 길이 (바이트)
 * @msg:         서명할 메시지 (signing_input)
 * @msg_len:     메시지 길이
 * @out_sig:     (출력) 서명 결과 버퍼 (최소 EVP_MAX_MD_SIZE 바이트)
 * @out_sig_len: (출력) 실제 서명 길이 (SHA256 = 32바이트)
 *
 * @return: TRUE이면 성공, FALSE이면 OpenSSL HMAC 실패
 *
 * [HMAC-SHA256이란?]
 *   Hash-based Message Authentication Code.
 *   비밀 키와 메시지를 결합하여 인증 코드를 생성합니다.
 *   동일한 키 + 메시지 = 동일한 코드 (결정적)
 *   키를 모르면 코드 생성 불가 (위조 방지)
 *
 * [OpenSSL HMAC() 함수]
 *   result = HMAC(알고리즘, 키, 키길이, 메시지, 메시지길이, 출력버퍼, &출력길이)
 *   성공 시 out_sig 포인터 반환, 실패 시 NULL 반환
 */
static gboolean
_hmac_sha256(const gchar  *key,      gsize key_len,
             const gchar  *msg,      gsize msg_len,
             guchar       *out_sig,  guint *out_sig_len)
{
    unsigned int len = 0;
    guchar *result = HMAC(EVP_sha256(),
                          key, (int)key_len,
                          (const unsigned char *)msg, msg_len,
                          out_sig, &len);
    if (!result) return FALSE;
    *out_sig_len = len;
    return TRUE;
}

/* ── 공개 API ───────────────────────────────────────────────── */

/**
 * pcv_jwt_init - JWT 모듈 초기화
 * @secret: HMAC 서명 키 (NULL 또는 빈 문자열이면 랜덤 키 자동 생성)
 *
 * [호출 시점]
 *   main.c에서 pcv_config_init() 이후, REST 서버 시작 전에 1회 호출.
 *   pcv_config_get_jwt_secret()으로 설정값을 전달받습니다.
 *
 *   호출 예시:
 *     pcv_jwt_init(pcv_config_get_jwt_secret());
 *
 * [랜덤 키 생성]
 *   secret 미설정 시 /dev/urandom에서 32바이트(256비트) 키를 생성합니다.
 *   256비트는 HS256(HMAC-SHA256)의 권장 최소 키 길이입니다.
 *
 * [운영 주의]
 *   랜덤 키 사용 시 데몬 재시작 시 기존 JWT 토큰이 모두 무효화됩니다.
 *   로드밸런서 뒤의 멀티 인스턴스 환경에서는 모든 노드가 동일한
 *   jwt_secret을 사용해야 합니다 (daemon.conf에서 설정).
 *
 * [뮤텍스 초기화]
 *   g_mutex_init()은 정적 초기화(G_MUTEX_INIT)와 달리
 *   런타임에 초기화합니다. 프로세스당 1회만 호출해야 합니다.
 */
void
pcv_jwt_init(const gchar *secret)
{
    g_mutex_init(&g_jwt_mutex);
    g_mutex_init(&g_jti_blacklist_mu);  /* B6-W3 */

    if (secret && *secret) {
        /* B6-C2 (Phase 1 fix): HS256 안전 최소 길이 32 바이트 강제.
         * RFC 8725 §3.2 / NIST SP 800-117 — HMAC-SHA256 키는 출력 크기와
         * 동일한 최소 길이를 권장. 짧은 secret은 충돌 저항성 약화 + 무차별 공격 가능. */
        gsize secret_len = strlen(secret);
        if (secret_len < 32) {
            PCV_LOG_WARN(JWT_LOG_DOM,
                         "JWT secret too short (%zu bytes < 32) — falling back to random key. "
                         "Set [auth] jwt_secret to >= 32 bytes in daemon.conf for stable tokens.",
                         secret_len);
            secret = NULL;  /* fall through to /dev/urandom branch */
        } else {
            g_jwt_secret     = g_strdup(secret);
            g_jwt_secret_len = secret_len;
        }
    }
    if (!secret || !*secret) {
        /*
         * /dev/urandom에서 32바이트 랜덤 키 생성
         *
         * [/dev/urandom vs /dev/random]
         *   /dev/urandom: 비블로킹, 충분한 엔트로피 (현대 Linux에서 안전)
         *   /dev/random:  블로킹 가능 (부팅 직후 엔트로피 부족 시)
         *   HMAC 키 용도로는 /dev/urandom이 적합합니다.
        */
        guchar rnd[32];
        _fill_random_bytes(rnd, sizeof(rnd));
        /* g_memdup2: GLib 2.68+에서 g_memdup의 안전한 대체 (size overflow 방지) */
        g_jwt_secret     = (gchar *)g_memdup2(rnd, sizeof(rnd));
        g_jwt_secret_len = sizeof(rnd);
        PCV_LOG_WARN(JWT_LOG_DOM,
                     "JWT secret not configured — using random key "
                     "(tokens invalidated on restart)");
    }
}

/* B6-W3: jti blacklist API */
void
pcv_jwt_blacklist_add(const gchar *jti, gint64 expiry_unix)
{
    if (!jti || !*jti) return;
    g_mutex_lock(&g_jti_blacklist_mu);
    if (!g_jti_blacklist) {
        g_jti_blacklist = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
    }
    /* GINT_TO_POINTER는 32비트 unix timestamp 가정 (2038년까지 유효) */
    g_hash_table_replace(g_jti_blacklist, g_strdup(jti),
                         GINT_TO_POINTER((gint)expiry_unix));
    g_mutex_unlock(&g_jti_blacklist_mu);
    PCV_LOG_INFO(JWT_LOG_DOM, "Token revoked: jti=%s", jti);
}

gboolean
pcv_jwt_blacklist_check(const gchar *jti)
{
    if (!jti || !*jti) return FALSE;
    gboolean revoked = FALSE;
    g_mutex_lock(&g_jti_blacklist_mu);
    if (g_jti_blacklist) {
        gpointer val = g_hash_table_lookup(g_jti_blacklist, jti);
        if (val) {
            gint expiry = GPOINTER_TO_INT(val);
            if ((gint)time(NULL) < expiry) {
                revoked = TRUE;
            } else {
                /* 만료된 entry 즉시 제거 (sweeper 부담 경감) */
                g_hash_table_remove(g_jti_blacklist, jti);
            }
        }
    }
    g_mutex_unlock(&g_jti_blacklist_mu);
    return revoked;
}

void
pcv_jwt_blacklist_sweep(void)
{
    g_mutex_lock(&g_jti_blacklist_mu);
    if (g_jti_blacklist) {
        GHashTableIter it;
        gpointer key, val;
        gint now = (gint)time(NULL);
        GPtrArray *to_remove = g_ptr_array_new();
        g_hash_table_iter_init(&it, g_jti_blacklist);
        while (g_hash_table_iter_next(&it, &key, &val)) {
            if (GPOINTER_TO_INT(val) <= now) {
                g_ptr_array_add(to_remove, key);
            }
        }
        for (guint i = 0; i < to_remove->len; i++) {
            g_hash_table_remove(g_jti_blacklist, g_ptr_array_index(to_remove, i));
        }
        g_ptr_array_free(to_remove, TRUE);
    }
    g_mutex_unlock(&g_jti_blacklist_mu);
}

/**
 * pcv_jwt_shutdown - JWT 모듈 종료 및 시크릿 키 메모리 해제
 *
 * g_jwt_secret을 안전하게 해제하고 뮤텍스를 정리합니다.
 * main.c 종료 시 호출합니다.
 *
 * [주의]
 *   뮤텍스를 잠근 상태에서 secret을 해제하여
 *   다른 스레드의 동시 접근을 방지합니다.
 *   g_mutex_clear()는 뮤텍스 리소스를 정리합니다.
 */
void
pcv_jwt_shutdown(void)
{
    g_mutex_lock(&g_jwt_mutex);
    g_free(g_jwt_secret);
    g_jwt_secret     = NULL;
    g_jwt_secret_len = 0;
    g_mutex_unlock(&g_jwt_mutex);
    g_mutex_clear(&g_jwt_mutex);
    /* B6-W3: blacklist 해제 (테스트 setup/teardown 반복 시 필수) */
    g_mutex_lock(&g_jti_blacklist_mu);
    if (g_jti_blacklist) {
        g_hash_table_destroy(g_jti_blacklist);
        g_jti_blacklist = NULL;
    }
    g_mutex_unlock(&g_jti_blacklist_mu);
    g_mutex_clear(&g_jti_blacklist_mu);
}

/**
 * pcv_jwt_update_secret - SIGHUP 시크릿 키 핫 리로드
 * @new_secret: 새 HMAC 서명 키 (문자열, NULL/빈 문자열이면 무시)
 *
 * 보안 사고 대응용: daemon.conf의 jwt_secret을 변경한 뒤
 * SIGHUP을 보내면 런타임에 서명 키가 교체됩니다.
 * 기존 토큰은 자동으로 무효화됩니다 (서명 불일치).
 */
void
pcv_jwt_update_secret(const gchar *new_secret)
{
    if (!new_secret || !*new_secret) return;

    g_mutex_lock(&g_jwt_mutex);
    g_free(g_jwt_secret);
    g_jwt_secret     = g_strdup(new_secret);
    g_jwt_secret_len = strlen(new_secret);
    g_mutex_unlock(&g_jwt_mutex);
}

/**
 * pcv_jwt_sign - JWT 토큰 서명 발행
 * @subject:    토큰 subject (사용자명, 예: "admin", "operator1")
 * @expires_in: 만료 시간(초), 0이면 기본값 3600초(1시간) 적용
 * @error:      HMAC 실패 시 GError 설정 (호출자 g_error_free 필수)
 *
 * @return: "header.payload.signature" 형식 JWT 문자열 (호출자 g_free 필수)
 *          실패 시 NULL + error 설정
 *
 * [동작 흐름]
 *   1. 고정 header JSON → base64url 인코딩
 *      {"alg":"HS256","typ":"JWT"} → eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9
 *
 *   2. payload(sub, iat, exp) → base64url 인코딩
 *      {"sub":"admin","iat":1711234567,"exp":1711238167} → eyJzdWIi...
 *
 *   3. signing_input = header_b64 + "." + payload_b64
 *      eyJhbGci...eyJzdWIi...
 *
 *   4. HMAC-SHA256(g_jwt_secret, signing_input) → 32바이트 바이너리 서명
 *
 *   5. token = signing_input + "." + sig_b64
 *      eyJhbGci...eyJzdWIi....HMAC_서명_base64url
 *
 * [스레드 안전]
 *   g_jwt_mutex로 시크릿 접근을 동기화합니다.
 *   여러 REST 요청이 동시에 토큰을 발행해도 안전합니다.
 *
 * [JWT 클레임(Claims)]
 *   sub (subject):   토큰 소유자 식별자 (사용자명)
 *   iat (issued at): 토큰 발행 시각 (Unix timestamp)
 *   exp (expiry):    토큰 만료 시각 (Unix timestamp)
 *
 * [메모리 관리]
 *   함수 내부에서 생성된 중간 문자열(header_b64, payload_b64 등)은
 *   모두 g_free()로 해제합니다. 최종 token만 호출자에게 반환됩니다.
 */
gchar *
pcv_jwt_sign(const gchar *subject,
             guint        expires_in,
             GError     **error)
{
    /* subject 필수 검증 (빈 문자열도 거부) */
    g_return_val_if_fail(subject && *subject, NULL);

    /* expires_in=0이면 기본 만료 시간 적용 */
    if (expires_in == 0)
        expires_in = JWT_DEFAULT_EXPIRY;

    /* ── 1. Header ──────────────────────────────────────────── */
    /*
     * JWT header는 항상 동일한 고정 문자열입니다.
     * HS256(HMAC-SHA256)만 지원하므로 동적 생성이 불필요합니다.
     */
    static const gchar header_json[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    gchar *header_b64 = _b64url_encode((const guchar *)header_json,
                                        strlen(header_json));

    /* ── 2. Payload ─────────────────────────────────────────── */
    /*
     * json-glib의 JsonBuilder로 payload JSON을 동적 생성합니다.
     * time(NULL)은 현재 Unix timestamp (1970-01-01부터 경과 초)를 반환합니다.
     */
    gint64 now = (gint64)time(NULL);
    JsonBuilder *jb = json_builder_new();
    json_builder_begin_object(jb);
    json_builder_set_member_name(jb, "sub");
    json_builder_add_string_value(jb, subject);         /* 사용자명 */
    json_builder_set_member_name(jb, "iat");
    json_builder_add_int_value(jb, now);                /* 발행 시각 */
    json_builder_set_member_name(jb, "exp");
    json_builder_add_int_value(jb, now + (gint64)expires_in);  /* 만료 시각 */
    /* B6-W3 (Phase 3 fix): jti 클레임 — logout/revoke 추적용 unique ID
     * 16바이트 random hex (= 32자) — 충분한 entropy로 충돌 회피 */
    {
        guchar jti_bytes[16];
        _fill_random_bytes(jti_bytes, sizeof(jti_bytes));
        gchar jti_hex[33];
        for (int i = 0; i < 16; i++) snprintf(jti_hex + i*2, 3, "%02x", jti_bytes[i]);
        jti_hex[32] = '\0';
        json_builder_set_member_name(jb, "jti");
        json_builder_add_string_value(jb, jti_hex);
    }

    /* BE-A11: IP 바인딩 (선택적) — pcv_jwt_sign_with_ip() 경유 시 설정 */
    /* IP 클레임은 pcv_jwt_sign_with_ip() 래퍼에서 별도 추가 */

    json_builder_end_object(jb);

    /* JsonBuilder → JSON 문자열 직렬화 */
    JsonNode *jwt_root = json_builder_get_root(jb);
    gchar *payload_json = json_to_string(jwt_root, FALSE);
    json_node_free(jwt_root);
    g_object_unref(jb);

    gchar *payload_b64 = _b64url_encode((const guchar *)payload_json,
                                         strlen(payload_json));
    g_free(payload_json);  /* 직렬화 후 원본 JSON 해제 */

    /* ── 3. Signing Input ────────────────────────────────────── */
    /*
     * signing_input = header_b64 + "." + payload_b64
     * 이 문자열에 HMAC-SHA256 서명을 적용합니다.
     * "."는 header와 payload의 구분자입니다.
     */
    gchar *signing_input = g_strdup_printf("%s.%s", header_b64, payload_b64);

    /* ── 4. HMAC-SHA256 서명 ─────────────────────────────────── */
    guchar sig[EVP_MAX_MD_SIZE];  /* 서명 결과 버퍼 (SHA256 = 32바이트) */
    guint  sig_len = 0;

    /* 뮤텍스로 시크릿 키 접근 동기화 */
    g_mutex_lock(&g_jwt_mutex);
    gboolean ok = _hmac_sha256(g_jwt_secret, g_jwt_secret_len,
                                signing_input, strlen(signing_input),
                                sig, &sig_len);
    g_mutex_unlock(&g_jwt_mutex);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "HMAC-SHA256 computation failed");
        g_free(header_b64); g_free(payload_b64); g_free(signing_input);
        return NULL;
    }

    /* ── 5. 최종 토큰 조립 ───────────────────────────────────── */
    gchar *sig_b64 = _b64url_encode(sig, sig_len);
    gchar *token   = g_strdup_printf("%s.%s", signing_input, sig_b64);

    /* 중간 문자열 전부 해제 — token만 반환 */
    g_free(header_b64);
    g_free(payload_b64);
    g_free(signing_input);
    g_free(sig_b64);

    return token;
}

/**
 * pcv_jwt_sign_with_ip - IP 바인딩 JWT 토큰 발행 (BE-A11)
 * @subject:    토큰 subject (사용자명)
 * @expires_in: 만료 시간(초), 0이면 기본값 적용
 * @client_ip:  바인딩할 클라이언트 IP (NULL 또는 빈 문자열이면 IP 바인딩 생략)
 * @error:      오류 반환
 *
 * pcv_jwt_sign()과 동일하나, payload에 "ip" 클레임을 추가합니다.
 * 검증 시 클라이언트 IP가 일치하지 않으면 토큰이 거부됩니���.
 *
 * Returns: JWT 문자열 (호출자 g_free), 실패 시 NULL
 */
gchar *
pcv_jwt_sign_with_ip(const gchar *subject,
                     guint        expires_in,
                     const gchar *client_ip,
                     GError     **error)
{
    g_return_val_if_fail(subject && *subject, NULL);

    if (expires_in == 0)
        expires_in = JWT_DEFAULT_EXPIRY;

    static const gchar header_json[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    gchar *header_b64 = _b64url_encode((const guchar *)header_json,
                                        strlen(header_json));

    gint64 now = (gint64)time(NULL);
    JsonBuilder *jb = json_builder_new();
    json_builder_begin_object(jb);
    json_builder_set_member_name(jb, "sub");
    json_builder_add_string_value(jb, subject);
    json_builder_set_member_name(jb, "iat");
    json_builder_add_int_value(jb, now);
    json_builder_set_member_name(jb, "exp");
    json_builder_add_int_value(jb, now + (gint64)expires_in);

    /* IP 바인딩 클레임 (선택적) */
    if (client_ip && *client_ip) {
        json_builder_set_member_name(jb, "ip");
        json_builder_add_string_value(jb, client_ip);
    }

    json_builder_end_object(jb);

    JsonNode *jwt_root2 = json_builder_get_root(jb);
    gchar *payload_json = json_to_string(jwt_root2, FALSE);
    json_node_free(jwt_root2);
    g_object_unref(jb);

    gchar *payload_b64 = _b64url_encode((const guchar *)payload_json,
                                         strlen(payload_json));
    g_free(payload_json);

    gchar *signing_input = g_strdup_printf("%s.%s", header_b64, payload_b64);

    guchar sig[EVP_MAX_MD_SIZE];
    guint  sig_len = 0;

    g_mutex_lock(&g_jwt_mutex);
    gboolean ok = _hmac_sha256(g_jwt_secret, g_jwt_secret_len,
                                signing_input, strlen(signing_input),
                                sig, &sig_len);
    g_mutex_unlock(&g_jwt_mutex);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "HMAC-SHA256 computation failed");
        g_free(header_b64); g_free(payload_b64); g_free(signing_input);
        return NULL;
    }

    gchar *sig_b64 = _b64url_encode(sig, sig_len);
    gchar *token   = g_strdup_printf("%s.%s", signing_input, sig_b64);

    g_free(header_b64);
    g_free(payload_b64);
    g_free(signing_input);
    g_free(sig_b64);

    return token;
}

/**
 * pcv_jwt_verify - JWT 토큰 검증 및 subject 추출
 * @token_or_bearer: JWT 토큰 문자열 ("Bearer <token>" 또는 토큰 단독)
 * @error:           검증 실패 시 GError 설정 (서명 불일치/만료/파싱 실패)
 *
 * @return: subject 문자열 (호출자 g_free 필수), 실패 시 NULL
 *
 * [검증 단계]
 *   1. "Bearer " 접두사 제거 (대소문자 모두 처리)
 *      HTTP Authorization 헤더에서 직접 전달받을 수 있으므로
 *      접두사를 자동으로 제거합니다.
 *
 *   2. 토큰을 "."으로 3파트 분리 (header.payload.signature)
 *      3파트가 아니면 형식 오류로 즉시 거부합니다.
 *
 *   3. 서명 재계산 후 CRYPTO_memcmp() 상수 시간 비교 (timing attack 방지)
 *      공격자가 서명을 한 바이트씩 맞춰가는 것을 방지합니다.
 *      일반 memcmp()는 첫 불일치에서 반환하므로 응답 시간으로 유추 가능.
 *
 *   4. payload 디코딩 → exp(만료 시간) 확인 → sub(사용자명) 추출
 *      만료된 토큰은 거부합니다 (현재 시각 > exp).
 *
 * [REST 서버 통합]
 *   rest_server.c에서 모든 JWT 필수 엔드포인트의 Authorization 헤더를
 *   이 함수로 검증합니다. 반환된 subject로 pcv_rbac_check_permission()을
 *   호출하여 역할별 권한 확인을 수행합니다.
 *
 * [에러 종류]
 *   G_IO_ERROR_INVALID_ARGUMENT : JWT 형식 오류 (3파트가 아님)
 *   G_IO_ERROR_FAILED           : HMAC 계산 실패 (내부 오류)
 *   G_IO_ERROR_PERMISSION_DENIED: 서명 불일치 (위조된 토큰)
 *   G_IO_ERROR_TIMED_OUT        : 토큰 만료
 *   G_IO_ERROR_INVALID_DATA     : sub 클레임 누락
 */
gchar *
pcv_jwt_verify(const gchar *token_or_bearer,
               GError     **error)
{
    g_return_val_if_fail(token_or_bearer, NULL);

    /*
     * "Bearer " 접두사 처리
     *
     * HTTP Authorization 헤더 형식: "Bearer eyJhbGci..."
     * 대소문자 모두 처리 ("Bearer " / "bearer ")
     * 접두사가 없으면 토큰 자체로 처리
     */
    const gchar *token = token_or_bearer;
    if (g_str_has_prefix(token, "Bearer "))
        token += 7;   /* "Bearer " = 7자 */
    if (g_str_has_prefix(token, "bearer "))
        token += 7;

    /* ── 1. 세 파트 분리 ─────────────────────────────────────── */
    /*
     * JWT 구조: header.payload.signature
     * g_strsplit(".", 3): 최대 3개 토큰으로 분리
     * parts[0] = header_b64, parts[1] = payload_b64, parts[2] = signature_b64
     */
    gchar **parts = g_strsplit(token, ".", 3);
    if (!parts || !parts[0] || !parts[1] || !parts[2]) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid JWT format");
        g_strfreev(parts);
        return NULL;
    }

    /* ── 2. 서명 재계산 후 비교 ──────────────────────────────── */
    /*
     * signing_input = header_b64 + "." + payload_b64
     * 이 문자열로 HMAC-SHA256을 재계산하여 토큰의 서명과 비교합니다.
     * 서명이 일치하면 토큰이 우리 서버에서 발행된 것이 확인됩니다.
     */
    gchar *signing_input = g_strdup_printf("%s.%s", parts[0], parts[1]);

    guchar expected_sig[EVP_MAX_MD_SIZE];  /* 재계산된 기대 서명 */
    guint  expected_len = 0;

    g_mutex_lock(&g_jwt_mutex);
    gboolean ok = _hmac_sha256(g_jwt_secret, g_jwt_secret_len,
                                signing_input, strlen(signing_input),
                                expected_sig, &expected_len);
    g_mutex_unlock(&g_jwt_mutex);

    g_free(signing_input);

    if (!ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "HMAC computation failed");
        g_strfreev(parts);
        return NULL;
    }

    /* 수신된 서명을 base64url 디코딩 */
    gsize   recv_len = 0;
    guchar *recv_sig = _b64url_decode(parts[2], &recv_len);

    /* base64url 디코딩 실패 방어 (malformed 토큰) */
    if (!recv_sig) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "JWT signature decode failed");
        g_strfreev(parts);
        return NULL;
    }

    /*
     * constant-time 비교 (timing attack 방지)
     *
     * CRYPTO_memcmp()는 OpenSSL이 제공하는 상수 시간 비교 함수입니다.
     * 일반 memcmp()는 첫 바이트 불일치에서 즉시 반환하므로,
     * 공격자가 응답 시간 차이를 측정하여 올바른 서명을 한 바이트씩
     * 유추할 수 있습니다 (timing side-channel attack).
     *
     * CRYPTO_memcmp()는 항상 전체 길이를 비교하므로
     * 일치/불일치에 관계없이 동일한 시간이 소요됩니다.
     */
    gboolean sig_ok = (recv_len == expected_len) &&
                      (CRYPTO_memcmp(recv_sig, expected_sig, expected_len) == 0);
    g_free(recv_sig);

    if (!sig_ok) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "JWT signature mismatch");
        g_strfreev(parts);
        return NULL;
    }

    /* ── 3. Payload 디코딩 + 만료 확인 ──────────────────────── */
    /*
     * 서명 검증 성공 후 payload를 디코딩하여 클레임을 추출합니다.
     * 순서가 중요: 먼저 서명 검증 → 그 다음 payload 파싱
     * (서명 미검증 payload를 신뢰하면 보안 위험)
     */
    gsize payload_len = 0;
    guchar *payload_raw = _b64url_decode(parts[1], &payload_len);
    g_strfreev(parts);  /* 이후 parts 불필요 */

    /* base64url 디코딩 실패 방어 (malformed payload) */
    if (!payload_raw) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "JWT payload decode failed");
        return NULL;
    }

    /* 바이너리 → C 문자열 변환 (NUL 종단 보장) */
    gchar *payload_str = g_strndup((const gchar *)payload_raw, payload_len);
    g_free(payload_raw);

    /* json-glib으로 payload JSON 파싱 */
    JsonParser *parser = json_parser_new();
    GError *parse_err  = NULL;
    if (!json_parser_load_from_data(parser, payload_str, -1, &parse_err)) {
        g_free(payload_str);
        g_object_unref(parser);
        g_propagate_error(error, parse_err);  /* 파싱 에러를 호출자에게 전파 */
        return NULL;
    }
    g_free(payload_str);

    /* 클레임 추출 */
    JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
    gint64 exp = json_object_get_int_member_with_default(obj, "exp", 0);
    const gchar *sub = json_object_get_string_member_with_default(obj, "sub", NULL);
    /* B6-W3: jti 클레임 추출 — parser unref 이전에 g_strdup */
    const gchar *jti = json_object_get_string_member_with_default(obj, "jti", NULL);
    gchar *jti_copy = jti ? g_strdup(jti) : NULL;

    /* sub 문자열 복사 (parser 해제 전에) */
    gchar *subject = sub ? g_strdup(sub) : NULL;
    g_object_unref(parser);  /* parser 해제 → obj, sub, jti 포인터 무효화 */

    /* 만료 시간 확인: 현재 시각이 exp를 초과하면 만료 */
    if (exp == 0 || (gint64)time(NULL) > exp) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                    "JWT token expired");
        g_free(subject); g_free(jti_copy);
        return NULL;
    }

    /* sub 클레임 필수 확인 */
    if (!subject) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "JWT missing 'sub' claim");
        g_free(jti_copy);
        return NULL;
    }

    /* B6-W3: jti blacklist 체크 (logout된 토큰 거부) */
    if (jti_copy && pcv_jwt_blacklist_check(jti_copy)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "JWT token revoked (logout)");
        g_free(subject); g_free(jti_copy);
        return NULL;
    }
    g_free(jti_copy);

    return subject;  /* 호출자가 g_free()로 해제해야 함 */
}

/**
 * pcv_jwt_verify_with_ip - JWT 토큰 검증 + IP 바인딩 확인 (BE-A11)
 * @token_or_bearer: JWT 토큰 문자열
 * @client_ip:       현재 클라이언트 IP (NULL이면 IP 검증 건너뜀)
 * @error:           검증 실패 시 GError 설정
 *
 * pcv_jwt_verify()와 동일하나, 토큰에 "ip" 클레임이 있으면
 * client_ip와 비교하여 불일치 시 거부합니다.
 *
 * Returns: subject 문자열 (호출자 g_free), 실패 시 NULL
 */
gchar *
pcv_jwt_verify_with_ip(const gchar *token_or_bearer,
                       const gchar *client_ip,
                       GError     **error)
{
    /* 먼저 기본 검증 수행 */
    gchar *subject = pcv_jwt_verify(token_or_bearer, error);
    if (!subject) return NULL;

    /* IP 바인딩 검증: 토큰 payload에서 "ip" 클레임 확인 */
    if (client_ip && *client_ip) {
        /* payload 재파싱 (verify에서 이미 검증 완료이므로 안전) */
        const gchar *tok = token_or_bearer;
        if (g_str_has_prefix(tok, "Bearer ") || g_str_has_prefix(tok, "bearer "))
            tok += 7;

        gchar **parts = g_strsplit(tok, ".", 4);
        if (parts && parts[0] && parts[1]) {
            gsize dec_len = 0;
            guchar *dec = g_base64_decode(parts[1], &dec_len);
            if (dec) {
                JsonParser *jp = json_parser_new();
                if (json_parser_load_from_data(jp, (const gchar *)dec, (gssize)dec_len, NULL)) {
                    JsonObject *payload = json_node_get_object(json_parser_get_root(jp));
                    if (payload && json_object_has_member(payload, "ip")) {
                        const gchar *bound_ip = json_object_get_string_member(payload, "ip");
                        if (bound_ip && g_strcmp0(bound_ip, client_ip) != 0) {
                            PCV_LOG_WARN(JWT_LOG_DOM,
                                         "JWT IP mismatch: bound=%s actual=%s user=%s",
                                         bound_ip, client_ip, subject);
                            g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                        "JWT IP binding mismatch");
                            g_free(subject);
                            subject = NULL;
                        }
                    }
                }
                g_object_unref(jp);
                g_free(dec);
            }
        }
        g_strfreev(parts);
    }

    return subject;
}
