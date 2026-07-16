/**
 * @file pcv_rbac.c
 * @brief RBAC 멀티테넌트 인증 모듈 — SQLite 사용자/역할 DB + SHA256 해싱 + JWT 발급
 *
 * [파일 역할]
 *   PureCVisor의 인증/인가(AuthN/AuthZ) 핵심 모듈.
 *   SQLite DB에 사용자/역할/테넌트 정보를 저장하고, 로그인 시 SHA256으로
 *   비밀번호를 검증한 뒤 JWT 토큰을 발급합니다. RPC 요청마다 사용자의 역할이
 *   해당 메서드의 최소 요구 역할 이상인지 확인하여 접근을 제어합니다.
 *
 * [아키텍처 위치]
 *   main.c (데몬 시작)
 *     -> pcv_rbac_init(db_path) -> _ensure_table() -> _ensure_admin_user()
 *   rest_server.c (POST /auth/token)
 *     -> pcv_rbac_authenticate(user, pass) -> SHA256 검증 -> pcv_jwt_sign()
 *   dispatcher.c (모든 RPC 처리 전)
 *     -> pcv_rbac_check_permission(user, method) -> 역할 >= 최소 역할 확인
 *   handler_auth.c (auth.* RPC)
 *     -> pcv_rbac_user_create/delete/list/set_role()
 *
 * [DB 스키마]
 *   경로: /var/lib/purecvisor/rbac.db (WAL 모드)
 *   테이블: users
 *     - username      TEXT PK   : 로그인 ID
 *     - password_hash TEXT      : SHA256(salt + password) 결과 hex 문자열
 *     - salt          TEXT      : 16바이트 랜덤 -> 32자리 hex
 *     - role          INTEGER   : 0=VIEWER, 1=OPERATOR, 2=ADMIN
 *     - tenant        TEXT      : 테넌트 격리 키 (NULL=전체 접근)
 *
 * [권한 모델 — 누적 계층형 (상위 역할은 하위 권한 포함)]
 *   VIEWER(0)  : 읽기 전용 (*.list, *.get, *.metrics, monitor.*, telemetry.*)
 *   OPERATOR(1): VIEWER + 운영. 단, VM 단일 대상 action은 dispatcher.c에서
 *                libvirt owner metadata를 추가 확인하여 "자신이 만든 VM"으로 제한
 *   ADMIN(2)   : 전체 (auth.*, *.delete, failover.test, 설정 변경 등)
 *   권한 판정: user_role >= _method_min_role(method) -> 허용
 *
 * [비전공자 설명]
 * 역할(role)은 건물 출입 등급과 비슷합니다. VIEWER는 보기만 가능하고,
 * OPERATOR는 기계를 조작할 수 있으며, ADMIN은 전체 관리가 가능합니다.
 * 다만 OPERATOR끼리는 서로의 VM을 만질 수 없도록 VM 소유자 이름표를
 * 한 번 더 확인합니다. 이 두 단계 검사가 RBAC + owner-scope입니다.
 *
 * [핵심 패턴]
 *   - 비밀번호 해싱: SHA256(salt + password)
 *     salt는 사용자 생성 시 16바이트 랜덤 생성, hex 인코딩하여 DB 저장
 *     검증 시 DB의 salt를 읽어 동일 해싱 후 hash 비교
 *   - JWT 발급: pcv_jwt_sign(username)으로 HS256 토큰 생성
 *     REST API는 Authorization: Bearer <token> 헤더로 인증
 *   - 초기 관리자: pcv_rbac_init() 시 admin 사용자가 없으면
 *     daemon.conf의 [daemon] admin_user/admin_password가 명시된 경우 자동 생성
 *
 * [스레드 안전]
 *   GMutex(g_rbac_mutex)로 모든 SQLite 접근을 직렬화합니다.
 *   REST 서버 스레드와 메인 스레드 양쪽에서 호출되므로 Mutex 필수.
 *
 * [주의사항]
 *   - pcv_rbac_init()은 pcv_config_init() 및 pcv_jwt_init() 이후에 호출해야 함
 *   - SQLite는 단일 파일 DB이므로 프로세스 간 동시 접근 시 WAL 모드 필요
 *   - pcv_rbac_get_tenant() 반환값은 TLS 버퍼이므로 g_free() 금지
 *   - 빌드 의존성: libsqlite3-dev, libcrypto (openssl EVP)
 */

#include "pcv_rbac.h"
#include "../../utils/pcv_jwt.h"
#include "../../utils/pcv_log.h"
#include "../../utils/pcv_config.h"
#include "../audit/pcv_audit.h"

#include <glib.h>
#include <gio/gio.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>

#define RBAC_LOG_DOM  "pcv_rbac"
#define SALT_LEN      16   /* 16 bytes → hex 변환 시 32 chars */

/* ── 세션/토큰 만료 상수 ──────────────────────────────────── */
#define ACCESS_TOKEN_EXPIRY   900     /* 15분 (보안 강화: 기존 3600 → 900) */
#define REFRESH_TOKEN_EXPIRY  604800  /* 7일 */
#define REFRESH_TOKEN_BYTES   32      /* 32 bytes → 64 hex chars */

/* ══════════════════════════════════════════════════════════════
 * [1] 모듈 전역 상태
 * ══════════════════════════════════════════════════════════════ */

static sqlite3 *g_rbac_db    = nullptr;   /* RBAC SQLite DB 핸들 (프로세스 수명) */
static GMutex   g_rbac_mutex;          /* 모든 DB 접근을 직렬화하는 뮤텍스 */

/* F8: api_keys canonical schema#2 보장/마이그레이션 (정의는 하단 [10] 섹션) */
static void _ensure_apikey_table(void);
static void _migrate_api_keys_schema(void);
/* apikey.create 계약 확장: schema#2 에 description/expires_at 컬럼 멱등 추가 */
static void _migrate_apikey_columns(void);
/* SEC-3: 기존 키의 저장 role 을 현 실효값으로 1회 고정 (freeze-effective) */
static void _migrate_freeze_apikey_effective_roles(void);

/* SEC-3 freeze-effective 마이그레이션 1회 실행 마커 (rbac.db PRAGMA user_version).
 * 이 DB 에 아직 다른 user_version 마이그레이션이 없어 1 부터 시작한다. */
#define PCV_RBAC_USER_VERSION_APIKEY_FREEZE 1

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

/* ══════════════════════════════════════════════════════════════
 * [1-B] 브루트포스 방어 — 인메모리 로그인 시도 추적
 *
 * 연속 실패 5회 → 계정 잠금 (지수 백오프: 30s, 60s, 300s, 3600s)
 * 성공 시 카운터 초기화.
 * GHashTable(username → LoginAttemptInfo)로 추적.
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    gint    attempts;       /* 연속 실패 횟수 */
    gint    lockout_count;  /* 잠금 발동 횟수 (백오프 단계) */
    gint64  locked_until;   /* 잠금 해제 시각 (monotonic µs), 0이면 잠기지 않음 */
} LoginAttemptInfo;

static GHashTable *g_login_attempts = nullptr;  /* username → LoginAttemptInfo* */
static GMutex      g_attempts_mu;

/* B6-M1: IP-based brute force defense — per-IP failed-login counter.
 * username 기반과 독립적으로 동작: 다수 username을 시도하는 credential stuffing 방어.
 * 임계: IP당 20회 실패 → 5분 잠금 (단일 단계, 지수 백오프 없음). */
static GHashTable *g_ip_attempts = nullptr;     /* client_ip → LoginAttemptInfo* */

#define BRUTE_MAX_ATTEMPTS  5
static const gint  BRUTE_BACKOFF_SEC[] = { 30, 60, 300, 3600 };
#define BRUTE_BACKOFF_STAGES  4

#define BRUTE_IP_MAX_ATTEMPTS  20
#define BRUTE_IP_LOCKOUT_SEC   300

static void
_brute_ensure_init(void)
{
    if (!g_login_attempts) {
        g_mutex_init(&g_attempts_mu);
        g_login_attempts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, g_free);
        g_ip_attempts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
    }
}

static LoginAttemptInfo *
_brute_get_info(const gchar *username)
{
    _brute_ensure_init();
    LoginAttemptInfo *info = g_hash_table_lookup(g_login_attempts, username);
    if (!info) {
        info = g_new0(LoginAttemptInfo, 1);
        g_hash_table_insert(g_login_attempts, g_strdup(username), info);
    }
    return info;
}

/**
 * _brute_check_locked — 잠금 여부 확인 (g_attempts_mu 보유 상태에서 호출)
 */
static gboolean
_brute_check_locked(const gchar *username)
{
    LoginAttemptInfo *info = _brute_get_info(username);
    if (info->locked_until <= 0) return FALSE;
    gint64 now = g_get_monotonic_time();
    if (now >= info->locked_until) {
        /* 잠금 해제 — 시도 카운터만 리셋 (lockout_count는 유지하여 백오프 증가) */
        info->locked_until = 0;
        info->attempts = 0;
        return FALSE;
    }
    return TRUE;
}

/**
 * _brute_record_failure — 실패 기록 + 필요 시 잠금 (g_attempts_mu 보유 상태에서 호출)
 */
static void
_brute_record_failure(const gchar *username)
{
    LoginAttemptInfo *info = _brute_get_info(username);
    info->attempts++;

    if (info->attempts >= BRUTE_MAX_ATTEMPTS) {
        gint stage = info->lockout_count;
        if (stage >= BRUTE_BACKOFF_STAGES)
            stage = BRUTE_BACKOFF_STAGES - 1;
        gint backoff_sec = BRUTE_BACKOFF_SEC[stage];

        info->locked_until = g_get_monotonic_time() + (gint64)backoff_sec * G_USEC_PER_SEC;
        info->lockout_count++;

        PCV_LOG_WARN(RBAC_LOG_DOM,
            "Account '%s' locked for %d seconds (attempt %d, lockout #%d)",
            username, backoff_sec, info->attempts, info->lockout_count);
    }
}

/**
 * _brute_record_success — 성공 시 카운터 초기화 (g_attempts_mu 보유 상태에서 호출)
 */
static void
_brute_record_success(const gchar *username)
{
    LoginAttemptInfo *info = _brute_get_info(username);
    info->attempts = 0;
    info->locked_until = 0;
    info->lockout_count = 0;
}

/* ── B6-M1: IP-based brute force helpers (g_attempts_mu 보유 상태 호출) ── */

static LoginAttemptInfo *
_brute_ip_get_info(const gchar *ip)
{
    LoginAttemptInfo *info = g_hash_table_lookup(g_ip_attempts, ip);
    if (!info) {
        info = g_new0(LoginAttemptInfo, 1);
        g_hash_table_insert(g_ip_attempts, g_strdup(ip), info);
    }
    return info;
}

static gboolean
_brute_ip_check_locked(const gchar *ip)
{
    if (!ip || !*ip) return FALSE;
    LoginAttemptInfo *info = _brute_ip_get_info(ip);
    if (info->locked_until <= 0) return FALSE;
    gint64 now = g_get_monotonic_time();
    if (now >= info->locked_until) {
        info->locked_until = 0;
        info->attempts = 0;
        return FALSE;
    }
    return TRUE;
}

static void
_brute_ip_record_failure(const gchar *ip)
{
    if (!ip || !*ip) return;
    LoginAttemptInfo *info = _brute_ip_get_info(ip);
    info->attempts++;
    if (info->attempts >= BRUTE_IP_MAX_ATTEMPTS) {
        info->locked_until = g_get_monotonic_time()
                             + (gint64)BRUTE_IP_LOCKOUT_SEC * G_USEC_PER_SEC;
        PCV_LOG_WARN(RBAC_LOG_DOM,
            "IP '%s' locked for %d seconds (%d failed attempts)",
            ip, BRUTE_IP_LOCKOUT_SEC, info->attempts);
    }
}

static void
_brute_ip_record_success(const gchar *ip)
{
    if (!ip || !*ip) return;
    LoginAttemptInfo *info = _brute_ip_get_info(ip);
    info->attempts = 0;
    info->locked_until = 0;
}

/* ══════════════════════════════════════════════════════════════
 * [2] 내부 헬퍼 — salt 생성 / 비밀번호 해싱
 * ══════════════════════════════════════════════════════════════ */

/**
 * _generate_salt:
 *
 * 16바이트 암호학적 랜덤 데이터를 생성하고, 32자 hex 문자열로 변환합니다.
 * /dev/urandom에서 읽기를 시도하고, 실패 시 GLib 난수로 폴백합니다.
 *
 * 왜 salt를 쓰는가:
 *   동일한 비밀번호라도 salt가 다르면 해시값이 달라져서,
 *   레인보우 테이블 공격을 방지할 수 있습니다.
 *
 * Returns: (transfer full): hex 문자열 (g_free 필요)
 */
static gchar *
_generate_salt(void)
{
    guchar raw[SALT_LEN];
    _fill_random_bytes(raw, sizeof(raw));

    GString *hex = g_string_sized_new(SALT_LEN * 2 + 1);
    for (gsize i = 0; i < sizeof(raw); i++)
        g_string_append_printf(hex, "%02x", raw[i]);

    return g_string_free(hex, FALSE);
}

/**
 * _hash_password_legacy:
 * @salt:     hex 문자열 (32 chars)
 * @password: 사용자 입력 평문 비밀번호
 *
 * [레거시] salt + password를 연결하여 단일 SHA256 해시를 계산합니다.
 * 기존 사용자 비밀번호 검증용으로만 사용. 신규 사용자는 PBKDF2를 사용합니다.
 *
 * 흐름: "salt" + "password" → SHA256 → "a3b1c2..." (64 hex chars)
 *
 * Returns: (transfer full): 64자 hex 해시 문자열 (g_free 필요)
 */
static gchar *
_hash_password_legacy(const gchar *salt, const gchar *password)
{
    gchar *input = g_strdup_printf("%s%s", salt, password);
    gsize  input_len = strlen(input);

    guchar digest[EVP_MAX_MD_SIZE];
    guint  digest_len = 0;

    /* OpenSSL EVP 3단계: Init → Update(데이터 투입) → Final(해시 추출) */
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx) {
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
        EVP_DigestUpdate(ctx, input, input_len);
        EVP_DigestFinal_ex(ctx, digest, &digest_len);
        EVP_MD_CTX_free(ctx);
    }
    g_free(input);

    GString *hex = g_string_sized_new(digest_len * 2 + 1);
    for (guint i = 0; i < digest_len; i++)
        g_string_append_printf(hex, "%02x", digest[i]);

    return g_string_free(hex, FALSE);
}

/**
 * _hash_password_pbkdf2:
 * @salt:     hex 문자열 (32 chars)
 * @password: 사용자 입력 평문 비밀번호
 *
 * PBKDF2-SHA256 (100,000 iterations) 해싱. 브루트포스/레인보우 테이블 공격에
 * 강한 키 스트레칭을 적용합니다.
 *
 * 결과: "pbkdf2:" 접두사 + 64자 hex 문자열 (총 71자)
 *
 * Returns: (transfer full): "pbkdf2:" 접두사 포함 해시 문자열 (g_free 필요)
 */
static gchar *
_hash_password_pbkdf2(const gchar *salt, const gchar *password)
{
    guchar dk[32]; /* SHA256 = 32 bytes */
    /* B6-M1 (Phase 6): PBKDF2 iteration을 daemon.conf에서 설정 가능하게.
     * 기본 100,000은 OWASP 2023 권장(600k) 대비 낮음. 하드웨어 성능이 향상된
     * 환경에서는 200k~600k로 상향할 수 있도록 config 훅 추가. 하한 100k로
     * 고정해 downgrade 공격 방지. */
    gint iter = pcv_config_get_int("auth", "pbkdf2_iterations", 100000);
    if (iter < 100000) iter = 100000;
    PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
                       (const guchar *)salt, (int)strlen(salt),
                       iter, EVP_sha256(), 32, dk);

    GString *hex = g_string_sized_new(7 + 64 + 1);
    g_string_append(hex, "pbkdf2:");
    for (int i = 0; i < 32; i++)
        g_string_append_printf(hex, "%02x", dk[i]);

    return g_string_free(hex, FALSE);
}

/**
 * _hash_password:
 * @salt:     hex 문자열 (32 chars)
 * @password: 사용자 입력 평문 비밀번호
 *
 * 기본 해싱 함수. PBKDF2-SHA256을 사용합니다.
 * 결과에 "pbkdf2:" 접두사가 포함되어 레거시 SHA256과 구분됩니다.
 *
 * [마이그레이션 경로]
 *   - 신규 사용자: PBKDF2 해시로 저장 (접두사 "pbkdf2:")
 *   - 기존 사용자: 로그인 시 레거시 SHA256으로 검증 성공 후
 *     DB의 해시를 PBKDF2로 자동 업데이트 (로그인 성공 시 마이그레이션)
 *
 * Returns: (transfer full): "pbkdf2:" 접두사 포함 해시 문자열 (g_free 필요)
 */
static gchar *
_hash_password(const gchar *salt, const gchar *password)
{
    return _hash_password_pbkdf2(salt, password);
}

/**
 * _ensure_table:
 *
 * users 테이블이 없으면 CREATE TABLE로 생성합니다.
 * "IF NOT EXISTS"이므로 이미 존재해도 안전합니다.
 *
 * 테이블 스키마:
 *   username      TEXT PK  — 로그인 ID (고유)
 *   password_hash TEXT     — SHA256(salt + password) hex 문자열
 *   salt          TEXT     — 16바이트 랜덤 hex
 *   role          INTEGER  — 0=VIEWER, 1=OPERATOR, 2=ADMIN
 *   tenant        TEXT     — 테넌트 격리 키 (NULL=전체 접근)
 *
 * Returns: 성공 시 TRUE
 */
static gboolean
_ensure_table(void)
{
    const gchar *sql_users =
        "CREATE TABLE IF NOT EXISTS users ("
        "  username      TEXT PRIMARY KEY NOT NULL,"
        "  password_hash TEXT NOT NULL,"
        "  salt          TEXT NOT NULL,"
        "  role          INTEGER NOT NULL DEFAULT 0,"
        "  tenant        TEXT"
        ");";

    gchar *errmsg = nullptr;
    int rc = sqlite3_exec(g_rbac_db, sql_users, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to create users table: %s",
                      errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return FALSE;
    }

    /* ── sessions 테이블: refresh token 기반 세션 관리 ──────── */
    const gchar *sql_sessions =
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id                 INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username           TEXT NOT NULL,"
        "  refresh_token_hash TEXT NOT NULL UNIQUE,"
        "  created_at         INTEGER NOT NULL,"
        "  expires_at         INTEGER NOT NULL,"
        "  revoked            INTEGER NOT NULL DEFAULT 0"
        ");";

    errmsg = nullptr;
    rc = sqlite3_exec(g_rbac_db, sql_sessions, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to create sessions table: %s",
                      errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return FALSE;
    }

    /* sessions 검색 성능을 위한 인덱스 */
    sqlite3_exec(g_rbac_db,
        "CREATE INDEX IF NOT EXISTS idx_sessions_hash "
        "ON sessions(refresh_token_hash);",
        NULL, NULL, NULL);
    sqlite3_exec(g_rbac_db,
        "CREATE INDEX IF NOT EXISTS idx_sessions_user "
        "ON sessions(username, revoked);",
        NULL, NULL, NULL);

    /* api_keys 테이블은 canonical schema#2로 _migrate_api_keys_schema()가
     * 별도 보장/마이그레이션한다 (F8: 이중 스키마 단일화). 여기서 생성하지 않음 —
     * schema#1(username/expires_at) CREATE가 먼저 실행되어 schema#2 컬럼을
     * 물리적으로 가리던 근본 원인 제거. */

    return TRUE;
}

/**
 * _ensure_admin_user:
 *
 * 최초 실행 시 관리자 계정이 없으면 자동 생성합니다.
 * daemon.conf의 admin_user/admin_password 설정을 사용합니다.
 * 공개 배포 안전을 위해 미설정 상태에서는 내장 기본 비밀번호로 생성하지 않습니다.
 *
 * 왜 필요한가:
 *   RBAC DB가 비어 있으면 누구도 JWT 토큰을 발급받을 수 없으므로,
 *   최소 1명의 ADMIN 사용자가 반드시 존재해야 합니다.
 */
static void
_ensure_admin_user(void)
{
    const gchar *admin_user = pcv_config_get_admin_user();
    const gchar *admin_pass = pcv_config_get_admin_password();

    if (!admin_user || !*admin_user) admin_user = "admin";
    if (!admin_pass || !*admin_pass) {
        PCV_LOG_WARN(RBAC_LOG_DOM,
                     "Bootstrap admin user '%s' not auto-created because admin_password is unset",
                     admin_user);
        return;
    }

    /* Check if admin exists */
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
                                "SELECT username FROM users WHERE username = ?;",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) return;

    sqlite3_bind_text(stmt, 1, admin_user, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_ROW) {
        /* Admin already exists */
        PCV_LOG_DEBUG(RBAC_LOG_DOM,
                      "Admin user '%s' already exists in RBAC DB", admin_user);
        return;
    }

    /* Create admin user */
    GError *err = nullptr;
    gboolean ok = pcv_rbac_user_create(admin_user, admin_pass,
                                       PCV_ROLE_ADMIN, NULL, &err);
    if (ok) {
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "Auto-created admin user '%s' (ADMIN role)", admin_user);
    } else {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to auto-create admin user: %s",
                      err ? err->message : "unknown");
        if (err) g_error_free(err);
    }
}

/* ══════════════════════════════════════════════════════════════
 * [3] 권한 매핑 — RPC 메서드 → 최소 필요 역할
 * ══════════════════════════════════════════════════════════════ */

/**
 * _method_min_role:
 * @method: RPC 메서드 이름 (예: "vm.delete", "vm.list")
 *
 * 주어진 RPC 메서드를 실행하기 위해 필요한 최소 역할을 반환합니다.
 * 역할 비교는 숫자 크기로 이루어집니다 (user_role >= min_role → 허용).
 *
 * 매핑 전략 (3단계):
 *   1) 접미사/접두사 패턴으로 VIEWER 판별 (*.list, *.metrics, monitor.* 등)
 *   2) 위험한 메서드는 명시적으로 ADMIN 지정 (*.delete, auth.*, failover 등)
 *   3) 나머지는 OPERATOR (start/stop/create, snapshot, migrate 등)
 *
 * Returns: 최소 필요 PcvRole
 */
static PcvRole
_method_min_role(const gchar *method)
{
    /* 메서드가 NULL이면 최대 권한 요구 — 안전 기본값 (deny by default) */
    if (!method || !*method) return PCV_ROLE_ADMIN;

    /* ── VIEWER methods ──────────────────────────────────── */
    /* Suffix-based: *.list, *.get, *.metrics, *.info */
    if (g_str_has_suffix(method, ".list") ||
        g_str_has_suffix(method, ".get")  ||
        g_str_has_suffix(method, ".metrics") ||
        g_str_has_suffix(method, ".info"))
    {
        return PCV_ROLE_VIEWER;
    }

    /* Prefix-based: monitor.*, telemetry.* */
    if (g_str_has_prefix(method, "monitor.") ||
        g_str_has_prefix(method, "telemetry."))
    {
        return PCV_ROLE_VIEWER;
    }

    /* Specific viewer methods */
    if (g_strcmp0(method, "iso.list") == 0        ||
        g_strcmp0(method, "vm.limit") == 0        ||
        g_strcmp0(method, "ovn.status") == 0      ||
        g_strcmp0(method, "vm.import.status") == 0  ||
        g_strcmp0(method, "vm.export.status") == 0  ||
        g_strcmp0(method, "cloud.jobs.list") == 0)
    {
        return PCV_ROLE_VIEWER;
    }
#if PCV_CLUSTER_ENABLED
    if (g_strcmp0(method, "cluster.status") == 0 ||
        g_strcmp0(method, "cluster.role") == 0   ||
        g_strcmp0(method, "storage.replicate.status") == 0)
    {
        return PCV_ROLE_VIEWER;
    }
#endif

    /* ── ADMIN-only methods ──────────────────────────────── */
    if (g_str_has_prefix(method, "auth."))
        return PCV_ROLE_ADMIN;

    if (g_strcmp0(method, "container.destroy") == 0  ||
        g_strcmp0(method, "storage.zvol.delete") == 0 ||
        g_strcmp0(method, "network.delete") == 0     ||
        g_strcmp0(method, "overlay.delete") == 0     ||
        g_strcmp0(method, "iscsi.target.delete") == 0 ||
        g_strcmp0(method, "ovn.switch.delete") == 0  ||
        g_strcmp0(method, "ovn.port.remove") == 0    ||
        g_strcmp0(method, "backup.replicate") == 0   ||
        g_strcmp0(method, "vm.import.ec2") == 0      ||
        g_strcmp0(method, "vm.export.ec2") == 0      ||
        g_strcmp0(method, "cloud.job.cancel") == 0)
    {
        return PCV_ROLE_ADMIN;
    }
    /* VM 단일 대상 operator action은 base role만 OPERATOR로 열고,
     * 실제 허용 여부는 dispatcher가 libvirt owner metadata와 인증 주체를
     * 비교해 결정한다. */
#if PCV_CLUSTER_ENABLED
    if (g_strcmp0(method, "cluster.failover.test") == 0 ||
        g_strcmp0(method, "cluster.peer.set") == 0 ||
        g_strcmp0(method, "storage.replicate.trigger") == 0)
    {
        return PCV_ROLE_ADMIN;
    }
#endif

    /* ── OPERATOR: known operational methods ───────────────── */
    /* vm.start, vm.stop, vm.create, vm.snapshot.*, vm.set_vcpu, vm.set_memory,
     * vm.eject, vm.vnc/get_vnc_info, container.start/stop/create/exec,
     * container.snapshot.*, network.create, network.mode_set,
     * storage.zvol.create, device.*, overlay.create, overlay.add_peer, etc. */
    if (g_str_has_prefix(method, "vm.") ||
        g_strcmp0(method, "get_vnc_info") == 0 ||
        g_str_has_prefix(method, "container.") ||
        g_str_has_prefix(method, "network.") ||
        g_str_has_prefix(method, "storage.") ||
        g_str_has_prefix(method, "device.") ||
        g_str_has_prefix(method, "overlay.") ||
        g_str_has_prefix(method, "iscsi.") ||
        g_str_has_prefix(method, "ovn.") ||
        g_str_has_prefix(method, "dpdk.") ||
        g_str_has_prefix(method, "sriov.") ||
        g_str_has_prefix(method, "backup.") ||
        g_str_has_prefix(method, "template.") ||
        g_str_has_prefix(method, "agent.") ||
        g_str_has_prefix(method, "alert."))
    {
        return PCV_ROLE_OPERATOR;
    }
#if PCV_CLUSTER_ENABLED
    if (g_str_has_prefix(method, "cluster."))
    {
        return PCV_ROLE_OPERATOR;
    }
#endif

    /* fail-secure: unlisted methods require ADMIN */
    return PCV_ROLE_ADMIN;
}

/* ══════════════════════════════════════════════════════════════
 * [3-B] Per-user 스토리지 쿼터 (BE-A9)
 * ══════════════════════════════════════════════════════════════ */

/**
 * _ensure_quota_columns:
 *
 * users 테이블에 quota_vm_count, quota_storage_gb 컬럼을 추가합니다.
 * SQLite ALTER TABLE은 이미 존재하는 컬럼 추가 시 실패하므로 무시합니다.
 */
static void
_ensure_quota_columns(void)
{
    if (!g_rbac_db) return;
    /* SQLite ALTER TABLE은 컬럼이 이미 있으면 실패하므로 무시 */
    sqlite3_exec(g_rbac_db,
        "ALTER TABLE users ADD COLUMN quota_vm_count INTEGER DEFAULT 0",
        NULL, NULL, NULL);
    sqlite3_exec(g_rbac_db,
        "ALTER TABLE users ADD COLUMN quota_storage_gb INTEGER DEFAULT 0",
        NULL, NULL, NULL);
}

/**
 * pcv_rbac_check_quota:
 * @username:          사용자 이름
 * @current_vm_count:  현재 사용자가 보유한 VM 수
 *
 * 사용자의 VM 쿼터를 확인합니다.
 * quota_vm_count가 0이면 무제한으로 간주합니다.
 *
 * Returns: 쿼터 이내이면 TRUE, 초과 시 FALSE
 */
gboolean
pcv_rbac_check_quota(const gchar *username, gint current_vm_count)
{
    if (!g_rbac_db || !username) return TRUE;

    g_mutex_lock(&g_rbac_mutex);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_rbac_db,
        "SELECT quota_vm_count FROM users WHERE username=?",
        -1, &stmt, NULL) != SQLITE_OK || !stmt) {
        g_mutex_unlock(&g_rbac_mutex);
        return TRUE;  /* prepare 실패 시 제한 없음으로 처리 */
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    gint quota = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        quota = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (quota <= 0) return TRUE;  /* 0 = unlimited */
    return current_vm_count < quota;
}

/**
 * pcv_rbac_set_quota:
 * @username:    사용자 이름
 * @vm_count:    VM 생성 한도 (0 = 무제한)
 * @storage_gb:  스토리지 한도 GB (0 = 무제한)
 *
 * 사용자의 리소스 쿼터를 설정합니다.
 *
 * Returns: 성공 시 TRUE
 */
gboolean
pcv_rbac_set_quota(const gchar *username, gint vm_count, gint storage_gb)
{
    if (!g_rbac_db || !username) return FALSE;

    g_mutex_lock(&g_rbac_mutex);
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_rbac_db,
        "UPDATE users SET quota_vm_count=?, quota_storage_gb=? WHERE username=?",
        -1, &stmt, NULL) != SQLITE_OK || !stmt) {
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }
    sqlite3_bind_int(stmt, 1, vm_count);
    sqlite3_bind_int(stmt, 2, storage_gb);
    sqlite3_bind_text(stmt, 3, username, -1, SQLITE_STATIC);
    gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (ok) {
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "Quota set for '%s': vm_count=%d, storage_gb=%d",
                     username, vm_count, storage_gb);
    }
    return ok;
}

/* ══════════════════════════════════════════════════════════════
 * [4] 공개 API — 초기화 / 종료
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_rbac_init:
 * @db_path: SQLite DB 경로 (NULL이면 /var/lib/purecvisor/rbac.db 사용)
 *
 * RBAC 모듈을 초기화합니다. 호출 순서:
 *   1) GMutex 초기화
 *   2) SQLite DB 열기 + WAL 모드 설정
 *   3) users 테이블 보장 (_ensure_table)
 *   4) 관리자 계정 보장 (_ensure_admin_user)
 *
 * WAL(Write-Ahead Logging) 모드:
 *   읽기와 쓰기가 동시에 가능해져서, 텔레메트리 등 읽기 빈번한
 *   환경에서 DB 잠금 경합을 줄여줍니다.
 *
 * main.c에서 pcv_config_init(), pcv_jwt_init() 이후에 호출해야 합니다.
 */

/**
 * _migrate_api_keys_schema:
 *
 * api_keys 테이블을 canonical schema#2(key_hash/client_name/role/created_at/
 * last_used_at/revoked)로 멱등 보장합니다. 과거 배포에 남아 있을 수 있는
 * schema#1(username/description/expires_at)을 감지하면 재작성합니다.
 * 데몬 리스너(REST/RPC) 개시 전 init 경로에서 1회만 호출되므로 동시 접근이 없습니다.
 *
 * 절차:
 *   1) PRAGMA table_info(api_keys)로 테이블/`client_name` 컬럼 존재 여부 탐지.
 *   2) 테이블 부재 또는 이미 schema#2 → _ensure_apikey_table()로 보장 후 종료.
 *   3) schema#1 감지 시:
 *        - 행 0개  → DROP TABLE 후 schema#2 CREATE (데이터 없음 확실, 최단·안전).
 *        - 행 >0개 → api_keys_new(schema#2) 생성 → INSERT SELECT(username→client_name,
 *                    role 기본 1=OPERATOR, revoked COALESCE) → DROP old → RENAME.
 *   ALTER ADD COLUMN 단독 금지: schema#1 expires_at INTEGER NOT NULL(기본값 없음)이
 *   남으면 schema#2 INSERT가 NOT NULL 위반 → 테이블 재작성 필수.
 */
static void
_migrate_api_keys_schema(void)
{
    if (!g_rbac_db) return;

    /* 1) 테이블 존재 + client_name 컬럼 존재 여부 탐지 */
    gboolean table_exists    = FALSE;
    gboolean has_client_name = FALSE;
    sqlite3_stmt *pragma = nullptr;
    if (sqlite3_prepare_v2(g_rbac_db,
            "PRAGMA table_info(api_keys);", -1, &pragma, NULL) == SQLITE_OK) {
        while (sqlite3_step(pragma) == SQLITE_ROW) {
            table_exists = TRUE;
            const char *col = (const char *)sqlite3_column_text(pragma, 1);
            if (col && g_strcmp0(col, "client_name") == 0)
                has_client_name = TRUE;
        }
        sqlite3_finalize(pragma);
    }

    /* 2) 테이블 부재 또는 이미 canonical → 보장 후 종료 */
    if (!table_exists || has_client_name) {
        _ensure_apikey_table();
        return;
    }

    /* 3) schema#1 감지 — 행 수 확인 */
    gint64 row_count = 0;
    sqlite3_stmt *cnt = nullptr;
    if (sqlite3_prepare_v2(g_rbac_db,
            "SELECT COUNT(*) FROM api_keys;", -1, &cnt, NULL) == SQLITE_OK) {
        if (sqlite3_step(cnt) == SQLITE_ROW)
            row_count = sqlite3_column_int64(cnt, 0);
        sqlite3_finalize(cnt);
    }

    gchar *errmsg = nullptr;
    if (row_count == 0) {
        /* 데이터 없음 확실 → DROP 후 재생성 (최단·안전) */
        int rc = sqlite3_exec(g_rbac_db, "DROP TABLE api_keys;",
                              NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            PCV_LOG_ERROR(RBAC_LOG_DOM,
                          "api_keys schema#2 migration: DROP failed: %s",
                          errmsg ? errmsg : "unknown");
            sqlite3_free(errmsg);
            return;
        }
        _ensure_apikey_table();
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "api_keys migrated to canonical schema#2 "
                     "(empty legacy schema#1 table dropped)");
        return;
    }

    /* 방어적 경로: 기존 행 보존 재작성 */
    const char *migrate_sql =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE api_keys_new ("
        "  key_hash     TEXT PRIMARY KEY,"
        "  client_name  TEXT NOT NULL,"
        "  role         INTEGER NOT NULL DEFAULT 1,"
        "  created_at   TEXT NOT NULL DEFAULT (datetime('now')),"
        "  last_used_at TEXT,"
        "  revoked      INTEGER NOT NULL DEFAULT 0"
        ");"
        "INSERT INTO api_keys_new (key_hash, client_name, role, revoked) "
        "  SELECT key_hash, username, 1, COALESCE(revoked, 0) FROM api_keys;"
        "DROP TABLE api_keys;"
        "ALTER TABLE api_keys_new RENAME TO api_keys;"
        "COMMIT;";
    int rc = sqlite3_exec(g_rbac_db, migrate_sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "api_keys schema#2 migration (table rewrite) failed: %s",
                      errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        sqlite3_exec(g_rbac_db, "ROLLBACK;", NULL, NULL, NULL);
        return;
    }
    PCV_LOG_INFO(RBAC_LOG_DOM,
                 "api_keys migrated to canonical schema#2 "
                 "(%lld legacy row(s) preserved, role defaulted to OPERATOR)",
                 (long long)row_count);
}

/* _migrate_freeze_apikey_effective_roles:
 *
 * [SEC-3 freeze-effective] 기존 API 키의 '저장 role'(api_keys.role)을 '현재 실효
 * (라이브 사용자) role'으로 1회 고정한다. 과거 auth 경로는 키의 저장 role을 무시하고
 * client_name의 라이브 role을 실효 role로 파생했다(privesc). 이제 저장 role을
 * 집행하도록 바뀌므로, 업그레이드 시점에 기존 키의 저장 role을 '지금까지의 실효값'으로
 * 못박아 권한 변동 0을 보장한다:
 *   - client_name이 실존 사용자  → 그 사용자의 role (admin명 키는 admin 유지 = grandfather)
 *   - client_name이 사용자 아님   → 0(VIEWER) = 실제 실효값(get_role 미존재 시 VIEWER)
 *
 * [반드시 1회만 — 매 부팅 재실행 금지]
 * 이 UPDATE를 매 부팅 무조건 재실행하면, 관리자가 이후 apikey.create/(향후 update)로
 * 명시적으로 설정한 키 role을 다시 client_name의 라이브 사용자 role로 되돌려 admin이
 * 설정한 값을 매 재시작마다 클로버한다(=BUG). 이를 막기 위해 rbac.db의
 * PRAGMA user_version을 1회성 마커로 사용한다: user_version이 목표 버전 미만일 때만
 * 실행하고, 성공 시 user_version을 올려 재실행을 봉쇄한다. → DB당 정확히 1회.
 *
 * 호출 순서: users(_ensure_table) + api_keys schema#2(_migrate_api_keys_schema/
 *   _migrate_apikey_columns)가 모두 존재한 뒤 호출해야 COALESCE 서브쿼리가 유효하다.
 * 리스너 개시 전 init 경로에서 호출되므로 동시 접근이 없다. */
static void
_migrate_freeze_apikey_effective_roles(void)
{
    if (!g_rbac_db) return;

    /* 1회성 게이트 — user_version 이 목표 미만일 때만 실행.
     * [SEC-3 follow-up] prepare/step rc 미확인이면 DB 오류(락/손상 등)를 uv=0으로
     * 오인해 "아직 미적용"으로 잘못 판단 → 이미 적용된 뒤 admin이 명시적으로 설정한
     * api_keys.role을 매 부팅 재계산값으로 클로버하는 권한 변동 위험(SEC-3가 막으려던
     * 바로 그 결함)이 재발한다. rc를 확인해 조회 실패 시 이번 부팅은 마이그레이션을
     * skip(=미적용으로 안전 처리)하고 다음 기동에 재시도한다. */
    gint64 uv = 0;
    gboolean uv_known = FALSE;
    sqlite3_stmt *q = nullptr;
    int uv_prep_rc = sqlite3_prepare_v2(g_rbac_db, "PRAGMA user_version;", -1, &q, NULL);
    if (uv_prep_rc == SQLITE_OK) {
        int uv_step_rc = sqlite3_step(q);
        if (uv_step_rc == SQLITE_ROW) {
            uv = sqlite3_column_int64(q, 0);
            uv_known = TRUE;
        } else {
            PCV_LOG_ERROR(RBAC_LOG_DOM,
                          "SEC-3 PRAGMA user_version step failed (rc=%d) — "
                          "apikey freeze-effective migration skipped this boot, "
                          "will retry next boot", uv_step_rc);
        }
        sqlite3_finalize(q);
    } else {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "SEC-3 PRAGMA user_version prepare failed (rc=%d): %s — "
                      "apikey freeze-effective migration skipped this boot, "
                      "will retry next boot",
                      uv_prep_rc, sqlite3_errmsg(g_rbac_db));
    }
    if (!uv_known)
        return;   /* fail-secure: 판정 불가 → 강행하지 않고 skip (재실행 안전 유지) */
    if (uv >= PCV_RBAC_USER_VERSION_APIKEY_FREEZE)
        return;   /* 이미 1회 실행됨 → 관리자 설정 role 클로버 방지 (재실행 금지) */

    char *errmsg = nullptr;
    int rc = sqlite3_exec(g_rbac_db,
        "UPDATE api_keys SET role = "
        "COALESCE((SELECT role FROM users WHERE users.username = api_keys.client_name), 0);",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "SEC-3 apikey freeze-effective migration failed: %s",
                      errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        /* user_version 미증가 → 다음 부팅 재시도. 아직 미실행이므로 클로버 위험 없음. */
        return;
    }

    /* 성공 → user_version 증가로 재실행 봉쇄. PRAGMA 는 바인딩 불가(리터럴만) →
     * 매크로 값을 G_STRINGIFY 로 컴파일타임 문자열화.
     * [SEC-3 follow-up] 이 쓰기의 rc를 확인하지 않으면, UPDATE는 이미 적용됐는데
     * user_version 마킹만 실패한 경우를 "성공"으로 잘못 로그하게 된다 — 마커가
     * 안 남았으니 다음 부팅에 uv<target으로 재실행되는 것 자체는 안전하지만(재실행 시
     * UPDATE가 다시 성공하면 그때 비로소 마킹됨), 실패를 성공으로 오인 로그하는 것은
     * 운영 판단을 오도한다. rc 확인 + 실패 시 명시 로그(성공 로그는 남기지 않음)로
     * "다음 기동 재시도"가 우연이 아니라 의도된 안전 처리임을 드러낸다. */
    int uv_write_rc = sqlite3_exec(g_rbac_db,
        "PRAGMA user_version = " G_STRINGIFY(PCV_RBAC_USER_VERSION_APIKEY_FREEZE) ";",
        NULL, NULL, NULL);
    if (uv_write_rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "SEC-3 PRAGMA user_version write failed (rc=%d): %s — "
                      "migration UPDATE already applied but marker not persisted, "
                      "will retry next boot",
                      uv_write_rc, sqlite3_errmsg(g_rbac_db));
        return;
    }
    PCV_LOG_INFO(RBAC_LOG_DOM,
                 "SEC-3 apikey freeze-effective migration applied "
                 "(existing keys' stored role pinned to current effective role; user_version=%d)",
                 PCV_RBAC_USER_VERSION_APIKEY_FREEZE);
}

void
pcv_rbac_init(const gchar *db_path)
{
    g_mutex_init(&g_rbac_mutex);

    const gchar *path = db_path ? db_path : "/var/lib/purecvisor/rbac.db";

    int rc = sqlite3_open(path, &g_rbac_db);
    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Cannot open RBAC DB '%s': %s",
                      path, sqlite3_errmsg(g_rbac_db));
        return;
    }

    /* Enable WAL mode for concurrent readers */
    sqlite3_exec(g_rbac_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    if (!_ensure_table()) return;

    /* F8: api_keys 이중 스키마 단일화 — canonical schema#2 보장 + 멱등 마이그레이션.
     * 리스너 개시 전이라 동시 접근 없음. */
    _migrate_api_keys_schema();

    /* apikey.create 계약 확장: 기존 schema#2 배포에 description/expires_at 컬럼 보강.
     * 신규 설치는 _ensure_apikey_table() 이 이미 포함하므로 ALTER 는 no-op(무시). */
    _migrate_apikey_columns();

    /* SEC-3: 기존 키의 저장 role 을 현 실효값으로 1회 고정 (권한 변동 0).
     * users + api_keys schema#2 존재 후 호출 (위 두 마이그레이션 뒤). PRAGMA user_version 으로 1회성. */
    _migrate_freeze_apikey_effective_roles();

    _ensure_admin_user();

    /* BE-A9: 사용자별 스토리지 쿼터 컬럼 보장 */
    _ensure_quota_columns();

    PCV_LOG_INFO(RBAC_LOG_DOM, "RBAC module initialized (DB: %s)", path);
}

/**
 * pcv_rbac_shutdown:
 *
 * SQLite 연결을 닫고 뮤텍스를 정리합니다.
 * 그레이스풀 드레인 완료 후 main.c에서 호출됩니다.
 */
void
pcv_rbac_shutdown(void)
{
    g_mutex_lock(&g_rbac_mutex);
    if (g_rbac_db) {
        sqlite3_close(g_rbac_db);
        g_rbac_db = nullptr;
    }
    g_mutex_unlock(&g_rbac_mutex);
    g_mutex_clear(&g_rbac_mutex);

    PCV_LOG_INFO(RBAC_LOG_DOM, "RBAC module shut down");
}

/* ══════════════════════════════════════════════════════════════
 * [5] 공개 API — 사용자 CRUD
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_rbac_user_create:
 * @username: 사용자 이름 (고유, NULL/빈문자열 불가)
 * @password: 평문 비밀번호 (SHA256 해싱 후 저장, 평문은 메모리에서 즉시 해제)
 * @role:     PCV_ROLE_VIEWER / PCV_ROLE_OPERATOR / PCV_ROLE_ADMIN
 * @tenant:   테넌트 격리 키 (NULL이면 전체 접근)
 * @error:    GError 반환 (중복 사용자, DB 오류 시)
 *
 * 새 사용자를 RBAC DB에 등록합니다.
 * 16바이트 랜덤 salt를 생성하고, SHA256(salt + password)로 해싱하여 저장합니다.
 *
 * 감사 로그: PCV_LOG_AUDIT로 사용자 생성 이벤트를 기록합니다.
 *
 * Returns: 성공 시 TRUE, 실패 시 FALSE + error 설정
 */

gboolean
pcv_rbac_user_create(const gchar *username,
                     const gchar *password,
                     PcvRole      role,
                     const gchar *tenant,
                     GError     **error)
{
    g_return_val_if_fail(username && *username, FALSE);
    g_return_val_if_fail(password && *password, FALSE);

    gchar *salt = _generate_salt();
    gchar *hash = _hash_password(salt, password);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "INSERT INTO users (username, password_hash, salt, role, tenant) "
        "VALUES (?, ?, ?, ?, ?);",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        g_free(salt);
        g_free(hash);
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash,     -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, salt,     -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 4, (int)role);
    if (tenant)
        sqlite3_bind_text(stmt, 5, tenant, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    g_free(salt);
    g_free(hash);

    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "User '%s' already exists or DB error: %s",
                    username, sqlite3_errmsg(g_rbac_db));
        pcv_audit_log(NULL, "auth.user.create", username, "failed", -1, 0, NULL);
        return FALSE;
    }

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.user.create", username,
                  "User created (role=%s, tenant=%s)",
                  pcv_rbac_role_to_str(role), tenant ? tenant : "global");
    pcv_audit_log(NULL, "auth.user.create", username, "ok", 0, 0, NULL);
    return TRUE;
}

/**
 * pcv_rbac_user_exists:
 * @username: 조회할 사용자 이름
 *
 * RBAC DB에 해당 사용자가 존재하는지 조회합니다.
 * DB 미초기화(g_rbac_db NULL)나 prepare 실패 등 판정 불능 상황은
 * PCV_USER_UNKNOWN으로 구분해, 호출자가 "존재하지 않음"과 "확인 불가"를
 * 혼동하지 않게 합니다.
 *
 * Returns: PCV_USER_PRESENT/ABSENT, 판정 불능 시 PCV_USER_UNKNOWN
 */
PcvUserExistence
pcv_rbac_user_exists(const gchar *username)
{
    if (!username || !*username) return PCV_USER_ABSENT;

    g_mutex_lock(&g_rbac_mutex);
    if (!g_rbac_db) {
        g_mutex_unlock(&g_rbac_mutex);
        return PCV_USER_UNKNOWN;
    }

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
                                "SELECT 1 FROM users WHERE username = ? LIMIT 1;",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return PCV_USER_UNKNOWN;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (rc == SQLITE_ROW)  return PCV_USER_PRESENT;
    if (rc == SQLITE_DONE) return PCV_USER_ABSENT;
    return PCV_USER_UNKNOWN;   /* SQLITE_ERROR/BUSY 등 = 판정 불능 */
}

/**
 * pcv_rbac_user_delete:
 * @username: 삭제할 사용자 이름
 * @error:    GError 반환
 *
 * RBAC DB에서 사용자를 삭제합니다.
 * sqlite3_changes()로 실제 삭제 여부를 확인하여, 존재하지 않는 사용자는 에러 반환합니다.
 *
 * Returns: 성공 시 TRUE
 */
gboolean
pcv_rbac_user_delete(const gchar *username, GError **error)
{
    g_return_val_if_fail(username && *username, FALSE);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "DELETE FROM users WHERE username = ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_rbac_db);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error: %s", sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }

    if (changes == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "User '%s' not found", username);
        return FALSE;
    }

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.user.delete", username,
                  "User deleted");
    pcv_audit_log(NULL, "auth.user.delete", username, "ok", 0, 0, NULL);
    return TRUE;
}

/**
 * pcv_rbac_user_list:
 *
 * 모든 사용자 목록을 조회합니다 (비밀번호 해시/salt는 포함하지 않음).
 * username 알파벳순으로 정렬하여 반환합니다.
 *
 * Returns: (transfer full): GPtrArray of PcvUser*
 *   호출자가 g_ptr_array_unref()로 해제해야 합니다.
 *   (GPtrArray의 free_func이 pcv_user_free이므로 원소도 자동 해제)
 */
GPtrArray *
pcv_rbac_user_list(void)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func((GDestroyNotify)pcv_user_free);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT username, role, tenant FROM users ORDER BY username;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return arr;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PcvUser *u = g_new0(PcvUser, 1);
        u->username = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
        u->role     = (PcvRole)sqlite3_column_int(stmt, 1);
        const gchar *t = (const gchar *)sqlite3_column_text(stmt, 2);
        u->tenant   = t ? g_strdup(t) : NULL;
        g_ptr_array_add(arr, u);
    }

    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    return arr;
}

/**
 * pcv_rbac_user_set_role:
 * @username: 대상 사용자
 * @role:     새 역할 (PcvRole 열거형)
 * @error:    GError 반환
 *
 * 기존 사용자의 역할을 변경합니다.
 * 존재하지 않는 사용자에 대해서는 에러를 반환합니다.
 *
 * Returns: 성공 시 TRUE
 */
gboolean
pcv_rbac_user_set_role(const gchar *username,
                       PcvRole      role,
                       GError     **error)
{
    g_return_val_if_fail(username && *username, FALSE);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE users SET role = ? WHERE username = ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }

    sqlite3_bind_int (stmt, 1, (int)role);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_rbac_db);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error: %s", sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }

    if (changes == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "User '%s' not found", username);
        return FALSE;
    }

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.role.set", username,
                  "Role changed to %s", pcv_rbac_role_to_str(role));
    {
        gchar *detail = g_strdup_printf("role changed to %s", pcv_rbac_role_to_str(role));
        pcv_audit_log(NULL, "auth.role.change", username, detail, 0, 0, NULL);
        g_free(detail);
    }
    return TRUE;
}

/**
 * pcv_rbac_change_password:
 * 본인 비밀번호 변경. old_password 검증 후 새 salt+hash로 교체합니다.
 */
gboolean
pcv_rbac_change_password(const gchar *username,
                         const gchar *old_password,
                         const gchar *new_password,
                         GError     **error)
{
    g_return_val_if_fail(username && *username, FALSE);
    g_return_val_if_fail(old_password && *old_password, FALSE);
    g_return_val_if_fail(new_password && *new_password, FALSE);

    if (strlen(new_password) < 8) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "New password must be at least 8 characters");
        return FALSE;
    }
    if (g_strcmp0(old_password, new_password) == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "New password must differ from current password");
        return FALSE;
    }

    GError *verr = nullptr;
    gchar *t = pcv_rbac_authenticate(username, old_password, &verr);
    if (!t) {
        if (verr) g_error_free(verr);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Current password is incorrect");
        pcv_audit_log(username, "auth.password.change", username,
                      "fail", 401, 0, NULL);
        return FALSE;
    }
    g_free(t);

    gchar *salt = _generate_salt();
    gchar *hash = _hash_password(salt, new_password);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE users SET password_hash = ?, salt = ? WHERE username = ?;",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        g_free(salt); g_free(hash);
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, hash,     -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, salt,     -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_rbac_db);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    g_free(salt);
    g_free(hash);

    if (rc != SQLITE_DONE || changes == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to update password for '%s'", username);
        pcv_audit_log(username, "auth.password.change", username,
                      "fail", 500, 0, NULL);
        return FALSE;
    }

    pcv_rbac_revoke_session(username, NULL);

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.password.change", username,
                  "Password changed (sessions revoked)");
    pcv_audit_log(username, "auth.password.change", username, "ok", 0, 0, NULL);
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════
 * [6] 공개 API — 인증 (로그인 → JWT 발급)
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_rbac_authenticate:
 * @username: 사용자 이름
 * @password: 평문 비밀번호
 * @error:    GError 반환 (인증 실패 시)
 *
 * DB에서 해당 사용자의 salt를 꺼내 비밀번호 해시를 계산하고,
 * 저장된 해시와 비교합니다. PBKDF2 ("pbkdf2:" 접두사) 및
 * 레거시 SHA256 형식을 모두 지원합니다.
 *
 * 인증 성공 시 pcv_jwt_sign()으로 JWT 토큰을 발급합니다.
 * 이 토큰은 REST API 요청 시 Authorization: Bearer <token> 헤더에 사용됩니다.
 *
 * 보안: constant-time CRYPTO_memcmp로 64바이트 고정 비교,
 *       실패 시 "Invalid credentials"만 반환하여 사용자 존재 여부 미노출.
 *
 * Returns: (transfer full): JWT 토큰 문자열 (g_free 필요), 실패 시 NULL
 */

gchar *
pcv_rbac_authenticate(const gchar *username,
                      const gchar *password,
                      GError     **error)
{
    g_return_val_if_fail(username && *username, NULL);
    g_return_val_if_fail(password && *password, NULL);

    /* Brute-force check */
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    if (_brute_check_locked(username)) {
        LoginAttemptInfo *info = g_hash_table_lookup(g_login_attempts, username);
        gint remain = info ? (gint)((info->locked_until - g_get_monotonic_time()) / G_USEC_PER_SEC) : 0;
        g_mutex_unlock(&g_attempts_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Account locked — retry after %d seconds", remain > 0 ? remain : 1);
        return NULL;
    }
    g_mutex_unlock(&g_attempts_mu);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT password_hash, salt FROM users WHERE username = ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);

    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid credentials");
        pcv_audit_log(username, "auth.login.failed", username, "user not found", 404, 0, NULL);
        return NULL;
    }

    const gchar *stored_hash = (const gchar *)sqlite3_column_text(stmt, 0);
    const gchar *stored_salt = (const gchar *)sqlite3_column_text(stmt, 1);

    gchar *computed_hash = _hash_password(stored_salt, password);

    /* PBKDF2 / 레거시 SHA256 자동 감지 비교
     * stored_hash가 "pbkdf2:" 접두사를 가지면 PBKDF2 검증,
     * 그렇지 않으면 레거시 SHA256 검증 (하위 호환).
     * constant-time 비교: 고정 64바이트 CRYPTO_memcmp 사용. */
    gboolean match = FALSE;
    if (g_str_has_prefix(stored_hash, "pbkdf2:")) {
        /* PBKDF2 해시 검증 */
        gchar *pbkdf2_hash = _hash_password_pbkdf2(stored_salt, password);
        const gchar *stored_hex = stored_hash + 7;  /* "pbkdf2:" 접두사 이후 */
        const gchar *computed_hex = pbkdf2_hash + 7;
        match = (strlen(stored_hex) >= 64) &&
                (CRYPTO_memcmp(stored_hex, computed_hex, 64) == 0);
        g_free(pbkdf2_hash);
    } else {
        /* 레거시 SHA256 해시 검증 (기존 사용자 하위 호환) */
        gchar *legacy_hash = _hash_password_legacy(stored_salt, password);
        match = (strlen(stored_hash) >= 64) &&
                (CRYPTO_memcmp(stored_hash, legacy_hash, 64) == 0);
        g_free(legacy_hash);

        /* B6-M2: 레거시 SHA256 사용을 INFO 로그로 가시화 — 마이그레이션 진행 추적 */
        if (match) {
            PCV_LOG_INFO(RBAC_LOG_DOM,
                "Legacy SHA256 hash accepted for '%s' — will auto-migrate",
                username);
        }

        /* 레거시 SHA256 해시 → PBKDF2 자동 마이그레이션 */
        if (match) {
            gchar *new_hash = _hash_password_pbkdf2(stored_salt, password);
            if (new_hash) {
                sqlite3_stmt *upd = nullptr;
                if (sqlite3_prepare_v2(g_rbac_db,
                        "UPDATE users SET password_hash=? WHERE username=?",
                        -1, &upd, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(upd, 1, new_hash, -1, SQLITE_STATIC);
                    sqlite3_bind_text(upd, 2, username, -1, SQLITE_STATIC);
                    if (sqlite3_step(upd) == SQLITE_DONE) {
                        PCV_LOG_INFO(RBAC_LOG_DOM,
                                     "Migrated password hash to PBKDF2 for user '%s'", username);
                    }
                    sqlite3_finalize(upd);
                }
                g_free(new_hash);
            }
        }
    }
    g_free(computed_hash);

    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (!match) {
        /* Brute-force: record failure */
        g_mutex_lock(&g_attempts_mu);
        _brute_record_failure(username);
        g_mutex_unlock(&g_attempts_mu);

        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid credentials");
        PCV_LOG_WARN(RBAC_LOG_DOM,
                     "Authentication failed for user '%s'", username);
        pcv_audit_log(username, "auth.login.failed", username, "invalid credentials", 401, 0, NULL);
        return NULL;
    }

    /* Brute-force: record success */
    g_mutex_lock(&g_attempts_mu);
    _brute_record_success(username);
    g_mutex_unlock(&g_attempts_mu);

    /* 인증 성공 → JWT 발급 (subject=username, expiry=0은 기본 만료 사용) */
    gchar *token = pcv_jwt_sign(username, 0, error);
    if (token) {
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "User '%s' authenticated successfully", username);
        pcv_audit_log(username, "auth.login", username, "ok", 0, 0, NULL);
    }
    return token;
}

/* ══════════════════════════════════════════════════════════════
 * [6-B] 내부 헬퍼 — refresh token 생성 / 해싱
 * ══════════════════════════════════════════════════════════════ */

/**
 * _generate_refresh_token:
 *
 * 32바이트 암호학적 랜덤 데이터를 생성하고, 64자 hex 문자열로 반환합니다.
 * /dev/urandom 사용, 실패 시 GLib 난수 폴백.
 *
 * Returns: (transfer full): 64자 hex 문자열 (g_free 필요)
 */
static gchar *
_generate_refresh_token(void)
{
    guchar raw[REFRESH_TOKEN_BYTES];
    _fill_random_bytes(raw, sizeof(raw));

    GString *hex = g_string_sized_new(REFRESH_TOKEN_BYTES * 2 + 1);
    for (gsize i = 0; i < sizeof(raw); i++)
        g_string_append_printf(hex, "%02x", raw[i]);

    return g_string_free(hex, FALSE);
}

/**
 * _hash_refresh_token:
 * @token: refresh token 평문 (64 hex chars)
 *
 * GChecksum SHA256으로 해싱합니다. DB에는 해시만 저장하여
 * DB 유출 시 refresh token 원문 노출을 방지합니다.
 *
 * Returns: (transfer full): 64자 hex SHA256 해시 (g_free 필요)
 */
static gchar *
_hash_refresh_token(const gchar *token)
{
    GChecksum *cksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(cksum, (const guchar *)token, (gssize)strlen(token));
    gchar *hex = g_strdup(g_checksum_get_string(cksum));
    g_checksum_free(cksum);
    return hex;
}

/**
 * _store_session:
 * @username:      세션 소유자
 * @token_hash:    refresh token의 SHA256 해시 (64 hex chars)
 *
 * sessions 테이블에 새 세션을 INSERT합니다.
 * 호출자가 g_rbac_mutex를 잡고 호출해야 합니다.
 *
 * Returns: 성공 시 TRUE
 */
static gboolean
_store_session(const gchar *username, const gchar *token_hash)
{
    sqlite3_stmt *stmt = nullptr;
    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;

    int rc = sqlite3_prepare_v2(g_rbac_db,
        "INSERT INTO sessions (username, refresh_token_hash, created_at, expires_at, revoked) "
        "VALUES (?, ?, ?, ?, 0);",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to prepare session INSERT: %s",
                      sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }

    sqlite3_bind_text (stmt, 1, username,   -1, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 2, token_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_bind_int64(stmt, 4, now + REFRESH_TOKEN_EXPIRY);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        PCV_LOG_ERROR(RBAC_LOG_DOM,
                      "Failed to store session: %s",
                      sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }

    return TRUE;
}

/* ══════════════════════════════════════════════════════════════
 * [6-C] 공개 API — pcv_rbac_authenticate_v2 (access + refresh token)
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_rbac_authenticate_v2:
 *
 * pcv_rbac_authenticate()의 확장 버전.
 * 인증 성공 시 access_token(JWT, 15분) + refresh_token(랜덤 64hex, 7일) 발급.
 * refresh_token은 SHA256 해시를 sessions 테이블에 저장합니다.
 *
 * out_refresh_token이 NULL이면 refresh token을 생성하지 않습니다
 * (기존 pcv_rbac_authenticate와 동일 동작).
 */
gchar *
pcv_rbac_authenticate_v2(const gchar *username,
                         const gchar *password,
                         gchar      **out_refresh_token,
                         GError     **error)
{
    g_return_val_if_fail(username && *username, NULL);
    g_return_val_if_fail(password && *password, NULL);

    /* Brute-force check */
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    if (_brute_check_locked(username)) {
        LoginAttemptInfo *info = g_hash_table_lookup(g_login_attempts, username);
        gint remain = info ? (gint)((info->locked_until - g_get_monotonic_time()) / G_USEC_PER_SEC) : 0;
        g_mutex_unlock(&g_attempts_mu);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Account locked — retry after %d seconds", remain > 0 ? remain : 1);
        return NULL;
    }
    g_mutex_unlock(&g_attempts_mu);

    /* ── 비밀번호 검증 (pcv_rbac_authenticate와 동일 로직) ─── */
    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT password_hash, salt FROM users WHERE username = ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);

    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid credentials");
        pcv_audit_log(username, "auth.login.failed", username, "user not found", 404, 0, NULL);
        return NULL;
    }

    const gchar *stored_hash = (const gchar *)sqlite3_column_text(stmt, 0);
    const gchar *stored_salt = (const gchar *)sqlite3_column_text(stmt, 1);

    /* PBKDF2 / 레거시 SHA256 자동 감지 비교 */
    gboolean match = FALSE;
    if (g_str_has_prefix(stored_hash, "pbkdf2:")) {
        gchar *pbkdf2_hash = _hash_password_pbkdf2(stored_salt, password);
        const gchar *stored_hex = stored_hash + 7;
        const gchar *computed_hex = pbkdf2_hash + 7;
        match = (strlen(stored_hex) >= 64) &&
                (CRYPTO_memcmp(stored_hex, computed_hex, 64) == 0);
        g_free(pbkdf2_hash);
    } else {
        gchar *legacy_hash = _hash_password_legacy(stored_salt, password);
        match = (strlen(stored_hash) >= 64) &&
                (CRYPTO_memcmp(stored_hash, legacy_hash, 64) == 0);
        g_free(legacy_hash);

        /* 레거시 SHA256 해시 → PBKDF2 자동 마이그레이션 */
        if (match) {
            gchar *new_hash = _hash_password_pbkdf2(stored_salt, password);
            if (new_hash) {
                sqlite3_stmt *upd = nullptr;
                if (sqlite3_prepare_v2(g_rbac_db,
                        "UPDATE users SET password_hash=? WHERE username=?",
                        -1, &upd, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(upd, 1, new_hash, -1, SQLITE_STATIC);
                    sqlite3_bind_text(upd, 2, username, -1, SQLITE_STATIC);
                    if (sqlite3_step(upd) == SQLITE_DONE) {
                        PCV_LOG_INFO(RBAC_LOG_DOM,
                                     "Migrated password hash to PBKDF2 for user '%s'", username);
                    }
                    sqlite3_finalize(upd);
                }
                g_free(new_hash);
            }
        }
    }

    sqlite3_finalize(stmt);

    if (!match) {
        g_mutex_unlock(&g_rbac_mutex);

        /* Brute-force: record failure */
        g_mutex_lock(&g_attempts_mu);
        _brute_record_failure(username);
        g_mutex_unlock(&g_attempts_mu);

        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid credentials");
        PCV_LOG_WARN(RBAC_LOG_DOM,
                     "Authentication failed for user '%s'", username);
        pcv_audit_log(username, "auth.login.failed", username, "invalid credentials", 401, 0, NULL);
        return NULL;
    }

    /* Brute-force: record success (still holding g_rbac_mutex, acquire g_attempts_mu separately) */
    g_mutex_lock(&g_attempts_mu);
    _brute_record_success(username);
    g_mutex_unlock(&g_attempts_mu);

    /* ── refresh token 생성 + DB 저장 (mutex 보유 상태) ─────── */
    gchar *refresh = nullptr;
    if (out_refresh_token) {
        refresh = _generate_refresh_token();
        gchar *hash = _hash_refresh_token(refresh);

        if (!_store_session(username, hash)) {
            g_free(hash);
            g_free(refresh);
            g_mutex_unlock(&g_rbac_mutex);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to create session");
            return NULL;
        }
        g_free(hash);
    }

    g_mutex_unlock(&g_rbac_mutex);

    /* ── JWT access token 발급 (15분) ──────────────────────── */
    gchar *token = pcv_jwt_sign(username, ACCESS_TOKEN_EXPIRY, error);
    if (token) {
        if (out_refresh_token)
            *out_refresh_token = refresh;
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "User '%s' authenticated (v2, refresh=%s)",
                     username, out_refresh_token ? "yes" : "no");
        pcv_audit_log(username, "auth.login", username, "ok", 0, 0, NULL);
    } else {
        g_free(refresh);
    }

    return token;
}

/* ══════════════════════════════════════════════════════════════
 * [6-D] 공개 API — refresh token 갱신 / 세션 취소 / 만료 정리
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_rbac_refresh_token:
 *
 * refresh token을 검증하고 새 access_token + 새 refresh token을 발급합니다.
 * 기존 refresh token은 revoke 처리하여 재사용을 방지합니다 (토큰 회전).
 */
gchar *
pcv_rbac_refresh_token(const gchar *refresh_token,
                       gchar      **out_new_refresh,
                       GError     **error)
{
    g_return_val_if_fail(refresh_token && *refresh_token, NULL);

    gchar *token_hash = _hash_refresh_token(refresh_token);
    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;

    g_mutex_lock(&g_rbac_mutex);

    /* ── 세션 조회: 유효한(미취소 + 미만료) refresh token 검색 ── */
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT id, username FROM sessions "
        "WHERE refresh_token_hash = ? AND revoked = 0 AND expires_at > ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        g_free(token_hash);
        return NULL;
    }

    sqlite3_bind_text (stmt, 1, token_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now);
    rc = sqlite3_step(stmt);

    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        g_mutex_unlock(&g_rbac_mutex);
        g_free(token_hash);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid or expired refresh token");
        PCV_LOG_WARN(RBAC_LOG_DOM, "Refresh token validation failed");
        pcv_audit_log(NULL, "auth.token.refresh", "unknown", "invalid token", 401, 0, NULL);
        return NULL;
    }

    gint64 session_id = sqlite3_column_int64(stmt, 0);
    gchar *username = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
    sqlite3_finalize(stmt);

    /* ── 기존 refresh token 무효화 (토큰 회전) ─────────────── */
    stmt = nullptr;
    rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE sessions SET revoked = 1 WHERE id = ?;",
        -1, &stmt, NULL);

    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, session_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* ── 새 refresh token 생성 + 저장 ─────────────────────── */
    gchar *new_refresh = nullptr;
    if (out_new_refresh) {
        new_refresh = _generate_refresh_token();
        gchar *new_hash = _hash_refresh_token(new_refresh);

        if (!_store_session(username, new_hash)) {
            g_free(new_hash);
            g_free(new_refresh);
            g_free(username);
            g_mutex_unlock(&g_rbac_mutex);
            g_free(token_hash);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to rotate refresh token");
            return NULL;
        }
        g_free(new_hash);
    }

    g_mutex_unlock(&g_rbac_mutex);
    g_free(token_hash);

    /* ── 새 access token 발급 ─────────────────────────────── */
    gchar *access_token = pcv_jwt_sign(username, ACCESS_TOKEN_EXPIRY, error);
    if (access_token) {
        if (out_new_refresh)
            *out_new_refresh = new_refresh;
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "Token refreshed for user '%s'", username);
        pcv_audit_log(username, "auth.token.refresh", username, "ok", 0, 0, NULL);
    } else {
        g_free(new_refresh);
    }

    g_free(username);
    return access_token;
}

/**
 * pcv_rbac_revoke_session:
 *
 * 해당 사용자의 모든 활성 세션을 무효화합니다.
 */
gboolean
pcv_rbac_revoke_session(const gchar *username, GError **error)
{
    g_return_val_if_fail(username && *username, FALSE);

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE sessions SET revoked = 1 WHERE username = ? AND revoked = 0;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed: %s", sqlite3_errmsg(g_rbac_db));
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_rbac_db);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB error: %s", sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }

    PCV_LOG_AUDIT(RBAC_LOG_DOM, "auth.session.revoke", username,
                  "Revoked %d active session(s)", changes);
    pcv_audit_log(username, "auth.session.revoke", username, "ok", 0, 0, NULL);
    return TRUE;
}

/**
 * pcv_rbac_cleanup_expired_sessions:
 *
 * 만료되거나 무효화된 세션을 sessions 테이블에서 삭제합니다.
 * 주기적 호출 권장 (예: 1시간마다, audit DB retention과 유사).
 */
gint
pcv_rbac_cleanup_expired_sessions(void)
{
    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "DELETE FROM sessions WHERE expires_at < ? OR revoked = 1;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, now);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_rbac_db);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (changes > 0) {
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "Cleaned up %d expired/revoked session(s)", changes);
    }

    return changes;
}

/* ══════════════════════════════════════════════════════════════
 * [6-E] 공개 API — 활성 세션 목록 조회 / 개별 세션 해지
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_rbac_list_sessions:
 * @username: 세션을 조회할 사용자
 *
 * 해당 사용자의 활성(만료되지 않고 무효화되지 않은) 세션 목록을 반환합니다.
 *
 * Returns: (transfer full): JsonArray of {id, created_at, expires_at}.
 *   호출자가 json_array_unref()로 해제해야 합니다.
 */
JsonArray *
pcv_rbac_list_sessions(const gchar *username)
{
    JsonArray *arr = json_array_new();
    if (!g_rbac_db || !username) return arr;

    gint64 now = (gint64)g_get_real_time() / G_USEC_PER_SEC;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT id, created_at, expires_at FROM sessions "
        "WHERE username=? AND revoked=0 AND expires_at > ?";
    if (sqlite3_prepare_v2(g_rbac_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, now);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            JsonObject *s = json_object_new();
            json_object_set_int_member(s, "id", sqlite3_column_int64(stmt, 0));
            json_object_set_int_member(s, "created_at", sqlite3_column_int64(stmt, 1));
            json_object_set_int_member(s, "expires_at", sqlite3_column_int64(stmt, 2));
            json_array_add_object_element(arr, s);
        }
        sqlite3_finalize(stmt);
    }

    g_mutex_unlock(&g_rbac_mutex);
    return arr;
}

/**
 * pcv_rbac_revoke_session_by_id:
 * @username:   세션 소유자 (권한 확인용)
 * @session_id: 해지할 세션 ID (sessions.id)
 *
 * 특정 세션을 개별적으로 무효화합니다.
 * username이 일치하는 세션만 해지하여 타 사용자 세션 탈취를 방지합니다.
 *
 * Returns: 성공 시 TRUE, 대상 없거나 권한 불일치 시 FALSE
 */
gboolean
pcv_rbac_revoke_session_by_id(const gchar *username, gint64 session_id)
{
    if (!g_rbac_db || !username) return FALSE;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE sessions SET revoked=1 WHERE id=? AND username=? AND revoked=0",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return FALSE;
    }

    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(g_rbac_db);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    gboolean ok = (rc == SQLITE_DONE && changes > 0);
    if (ok) {
        PCV_LOG_INFO(RBAC_LOG_DOM,
                     "Revoked session %ld for user '%s'", (long)session_id, username);
        pcv_audit_log(username, "auth.session.revoke", username, "ok", 0, 0, NULL);
    }
    return ok;
}

/* ══════════════════════════════════════════════════════════════
 * [7] 공개 API — 권한 확인
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_rbac_check_permission:
 * @username: 인증된 사용자 이름
 * @method:   실행하려는 RPC 메서드 (예: "vm.delete")
 *
 * 사용자의 역할(role)이 해당 메서드의 최소 요구 역할 이상인지 확인합니다.
 * 역할 비교: user_role >= min_role (숫자가 클수록 높은 권한)
 *
 * 예) ADMIN(2) >= OPERATOR(1) → 허용
 *     VIEWER(0) >= ADMIN(2) → 거부
 *
 * dispatcher.c에서 RPC 처리 전에 호출됩니다.
 *
 * Returns: 허용 시 TRUE, 거부 시 FALSE
 */

gboolean
pcv_rbac_check_permission(const gchar *username,
                          const gchar *method)
{
    if (!username || !method) return FALSE;

    PcvRole user_role = pcv_rbac_get_role(username);
    PcvRole min_role  = _method_min_role(method);

    return (user_role >= min_role);
}

/**
 * pcv_rbac_get_role:
 * @username: 조회할 사용자 이름
 *
 * DB에서 사용자의 역할을 조회합니다.
 * 사용자가 없거나 DB 오류 시 PCV_ROLE_VIEWER(최소 권한)를 반환합니다.
 *
 * Returns: PcvRole 열거형 값
 */
PcvRole
pcv_rbac_get_role(const gchar *username)
{
    /* 안전 기본값: 알 수 없는 사용자는 최소 권한 부여 */
    if (!username || !*username) return PCV_ROLE_VIEWER;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT role FROM users WHERE username = ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return PCV_ROLE_VIEWER;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);

    PcvRole role = PCV_ROLE_VIEWER;
    if (rc == SQLITE_ROW)
        role = (PcvRole)sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    return role;
}

/**
 * pcv_rbac_get_tenant:
 * @username: 조회할 사용자 이름
 *
 * 사용자의 테넌트 값을 조회합니다.
 * 멀티테넌트 환경에서 사용자가 접근 가능한 VM/네트워크를 필터링할 때 사용됩니다.
 *
 * 스레드 안전 주의:
 *   __thread (TLS) 정적 버퍼에 값을 복사하여 반환합니다.
 *   따라서 반환된 포인터는 같은 스레드에서 다음 호출 전까지만 유효합니다.
 *   g_free() 하면 안 됩니다 (transfer none).
 *
 * Returns: (transfer none): 테넌트 문자열, 전체 접근 시 NULL
 */
const gchar *
pcv_rbac_get_tenant(const gchar *username)
{
    /* TLS(Thread-Local Storage) 버퍼 — 스레드마다 독립 공간을 가짐 */
    static __thread gchar t_tenant[256];

    if (!username || !*username) return NULL;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT tenant FROM users WHERE username = ?;",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);

    const gchar *result = nullptr;
    if (rc == SQLITE_ROW) {
        const gchar *val = (const gchar *)sqlite3_column_text(stmt, 0);
        if (val) {
            g_strlcpy(t_tenant, val, sizeof(t_tenant));
            result = t_tenant;
        }
    }

    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    return result;
}

/* ══════════════════════════════════════════════════════════════
 * [8] 유틸리티 — 역할 문자열 변환 / PcvUser 해제
 * ══════════════════════════════════════════════════════════════ */

/**
 * pcv_rbac_role_to_str:
 * @role: PcvRole 열거형
 *
 * 역할 열거형을 사람이 읽을 수 있는 문자열로 변환합니다.
 * JSON 응답, 로그 출력에 사용됩니다.
 *
 * Returns: (transfer none): 정적 문자열 ("viewer"/"operator"/"admin"/"unknown")
 */

const gchar *
pcv_rbac_role_to_str(PcvRole role)
{
    switch (role) {
    case PCV_ROLE_VIEWER:   return "viewer";
    case PCV_ROLE_OPERATOR: return "operator";
    case PCV_ROLE_ADMIN:    return "admin";
    default:                return "unknown";
    }
}

/**
 * pcv_rbac_str_to_role:
 * @str: 역할 문자열 ("admin", "operator", "viewer" — 대소문자 무시)
 *
 * 문자열을 PcvRole 열거형으로 변환합니다.
 * 알 수 없는 값이면 PCV_ROLE_VIEWER(최소 권한)를 반환합니다.
 *
 * Returns: PcvRole 열거형 값
 */
PcvRole
pcv_rbac_str_to_role(const gchar *str)
{
    if (!str) return PCV_ROLE_VIEWER;
    if (g_ascii_strcasecmp(str, "admin") == 0)    return PCV_ROLE_ADMIN;
    if (g_ascii_strcasecmp(str, "operator") == 0) return PCV_ROLE_OPERATOR;
    if (g_ascii_strcasecmp(str, "viewer") == 0)   return PCV_ROLE_VIEWER;
    return PCV_ROLE_VIEWER;
}

/* ══════════════════════════════════════════════════════════════
 * [8] API Key 인증 — CI/자동화용 프로그래밍 방식 접근
 * ══════════════════════════════════════════════════════════════ */

/**
 * _sha256_hex:
 * 입력 데이터의 SHA256 해시를 64자 hex 문자열로 반환합니다.
 *
 * Returns: (transfer full): 64자 hex 문자열 (g_free 필요)
 */
static gchar *
_sha256_hex(const gchar *data, gsize len)
{
    guchar digest[EVP_MAX_MD_SIZE];
    guint  digest_len = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx) {
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
        EVP_DigestUpdate(ctx, data, len);
        EVP_DigestFinal_ex(ctx, digest, &digest_len);
        EVP_MD_CTX_free(ctx);
    }

    GString *hex = g_string_sized_new(digest_len * 2 + 1);
    for (guint i = 0; i < digest_len; i++)
        g_string_append_printf(hex, "%02x", digest[i]);

    return g_string_free(hex, FALSE);
}

/* F8: pcv_rbac_verify_api_key — REST X-API-Key 인증 (rest_server.c 소비).
 * canonical schema#2(api_keys) 조회. 키의 SHA256 hex는 apikey_create가 쓰는
 * g_compute_checksum_for_string(SHA256)과 _sha256_hex(OpenSSL EVP)가 동일 값이므로
 * apikey_create로 발급된 키가 그대로 검증된다. revoked=0 확인에 더해, 계약 확장으로
 * 도입된 expires_at(epoch 초; 0=무기한)을 현재 시각과 대조해 만료 키를 거부(집행)한다.
 * 유효하면 client_name을 신원으로 반환하고, @out_role에 키의 '저장 role'을 기록한다.
 *
 * PCV_SAFETY_CONTROL: apikey-role-enforce — 키의 실효 role은 저장 role 컬럼에서만
 *   파생(client_name 라이브 role 무시). 저장 role을 out-param으로 반환해 호출자가
 *   pcv_rbac_get_role(client_name) 대신 이 값으로 권한을 판정하게 강제한다 (SEC-3 privesc 차단). */
gchar *
pcv_rbac_verify_api_key(const gchar *api_key, PcvRole *out_role, GError **error)
{
    if (out_role) *out_role = PCV_ROLE_VIEWER;   /* 안전 기본값(실패/미매칭 시 최소 권한) */

    if (!api_key || strlen(api_key) != 68 ||
        strncmp(api_key, "pcv_", 4) != 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Invalid API key format");
        return NULL;
    }

    gchar *key_hash = _sha256_hex(api_key, strlen(api_key));
    gint64 now_epoch = g_get_real_time() / G_USEC_PER_SEC;

    g_mutex_lock(&g_rbac_mutex);

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "SELECT client_name, role FROM api_keys "
        "WHERE key_hash = ? AND revoked = 0 "
        "AND (expires_at = 0 OR expires_at > ?);",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_mutex_unlock(&g_rbac_mutex);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "DB prepare failed");
        g_free(key_hash);
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, key_hash, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, now_epoch);

    gchar *client_name = nullptr;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        client_name = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
        if (out_role) *out_role = (PcvRole)sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);
    g_free(key_hash);

    if (!client_name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "API key invalid or revoked");
    }
    return client_name;
}

/* F8: schema#1 기반 dead API Key 함수 제거 —
 *   pcv_rbac_revoke_api_key / pcv_rbac_list_api_keys / pcv_rbac_get_expiring_api_keys.
 * 세 함수 모두 호출자 0(라이브 소비자는 apikey_revoke/apikey_list, schema#2)이었고
 * schema#1 컬럼(username/description/expires_at)을 참조해 schema#2 단일화 후
 * 잔존 시 재파손 위험이 있어 삭제. */

/**
 * pcv_user_free:
 * @u: 해제할 PcvUser 구조체 (NULL 안전)
 *
 * PcvUser의 모든 동적 할당 멤버(username, tenant)와 구조체 자체를 해제합니다.
 * GPtrArray의 free_func으로 등록되어 g_ptr_array_unref() 시 자동 호출됩니다.
 */
void
pcv_user_free(PcvUser *u)
{
    if (!u) return;
    g_free(u->username);
    g_free(u->tenant);
    g_free(u);
}

/* ══════════════════════════════════════════════════════════════
 * [9] 브루트포스 방어 — 공개 API
 * ══════════════════════════════════════════════════════════════ */

gboolean
pcv_rbac_is_locked(const gchar *username)
{
    if (!username || !*username) return FALSE;
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    gboolean locked = _brute_check_locked(username);
    g_mutex_unlock(&g_attempts_mu);
    return locked;
}

gint
pcv_rbac_get_remaining_lockout(const gchar *username)
{
    if (!username || !*username) return 0;
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    LoginAttemptInfo *info = g_hash_table_lookup(g_login_attempts, username);
    gint remaining = 0;
    if (info && info->locked_until > 0) {
        gint64 now = g_get_monotonic_time();
        if (now < info->locked_until)
            remaining = (gint)((info->locked_until - now) / G_USEC_PER_SEC);
    }
    g_mutex_unlock(&g_attempts_mu);
    return remaining;
}

/* B6-M1: IP-based brute force — public API */

gint
pcv_rbac_get_ip_remaining_lockout(const gchar *ip)
{
    if (!ip || !*ip) return 0;
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    /* _brute_ip_check_locked는 잠금 만료 시 자동 리셋도 수행 */
    gboolean locked = _brute_ip_check_locked(ip);
    gint remaining = 0;
    if (locked) {
        LoginAttemptInfo *info = g_hash_table_lookup(g_ip_attempts, ip);
        if (info && info->locked_until > 0) {
            gint64 now = g_get_monotonic_time();
            if (now < info->locked_until)
                remaining = (gint)((info->locked_until - now) / G_USEC_PER_SEC);
        }
    }
    g_mutex_unlock(&g_attempts_mu);
    return remaining;
}

void
pcv_rbac_ip_record_auth_failure(const gchar *ip)
{
    if (!ip || !*ip) return;
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    _brute_ip_record_failure(ip);
    g_mutex_unlock(&g_attempts_mu);
}

void
pcv_rbac_ip_record_auth_success(const gchar *ip)
{
    if (!ip || !*ip) return;
    _brute_ensure_init();
    g_mutex_lock(&g_attempts_mu);
    _brute_ip_record_success(ip);
    g_mutex_unlock(&g_attempts_mu);
}

/* ══════════════════════════════════════════════════════════════
 * [10] API Key 관리 — 머신 인증용 장기 토큰
 *
 * SQLite api_keys 테이블: key_hash(SHA256), client_name, role,
 * created_at, last_used_at, revoked (0/1)
 * ══════════════════════════════════════════════════════════════ */

static void _ensure_apikey_table(void) {
    /* 신규 설치는 full canonical schema(description/expires_at 포함)를 바로 생성한다.
     * 기존 schema#2 배포(컬럼 부재)는 _migrate_apikey_columns() 가 ALTER 로 보강. */
    const char *sql =
        "CREATE TABLE IF NOT EXISTS api_keys ("
        "  key_hash     TEXT PRIMARY KEY,"
        "  client_name  TEXT NOT NULL,"
        "  role         INTEGER NOT NULL DEFAULT 1,"
        "  description  TEXT NOT NULL DEFAULT '',"
        "  created_at   TEXT NOT NULL DEFAULT (datetime('now')),"
        "  last_used_at TEXT,"
        "  expires_at   INTEGER NOT NULL DEFAULT 0,"  /* epoch 초, 0 = 무기한 */
        "  revoked      INTEGER NOT NULL DEFAULT 0"
        ")";
    sqlite3_exec(g_rbac_db, sql, NULL, NULL, NULL);
}

/* _migrate_apikey_columns:
 *
 * canonical schema#2 api_keys 테이블에 description/expires_at 컬럼을 멱등 추가한다.
 * SQLite ALTER TABLE ADD COLUMN 은 이미 존재하는 컬럼 추가 시 에러이므로 무시한다
 * (users quota 컬럼 보강과 동일 패턴). 데몬 리스너 개시 전 init 경로에서 1회 호출. */
static void _migrate_apikey_columns(void)
{
    if (!g_rbac_db) return;
    const char *alters[] = {
        "ALTER TABLE api_keys ADD COLUMN description TEXT NOT NULL DEFAULT ''",
        "ALTER TABLE api_keys ADD COLUMN expires_at  INTEGER NOT NULL DEFAULT 0",
    };
    for (guint i = 0; i < G_N_ELEMENTS(alters); i++) {
        char *errmsg = nullptr;
        /* 컬럼이 이미 있으면 실패 → 무시 (멱등) */
        sqlite3_exec(g_rbac_db, alters[i], NULL, NULL, &errmsg);
        sqlite3_free(errmsg);
    }
}

/**
 * pcv_rbac_apikey_create — 새 API Key 생성
 * @client_name: 클라이언트 식별 이름 (예: "grafana-scraper")
 * @role: 권한 역할 (PCV_ROLE_VIEWER/OPERATOR/ADMIN)
 * @description: 키 용도 메모 (NULL/"" 허용, 저장)
 * @expires_at: 만료 시각 (epoch 초). 0 = 무기한. 인증 경로에서 만료 거부로 집행.
 * @out_key: (out) 생성된 평문 키 (호출자가 g_free)
 * @return TRUE 성공
 */
gboolean
pcv_rbac_apikey_create(const gchar *client_name, PcvRole role,
                       const gchar *description, gint64 expires_at,
                       gchar **out_key, GError **error)
{
    /* 32바이트 랜덤 키 생성 → hex 인코딩 (64자) */
    guint8 raw[32];
    _fill_random_bytes(raw, sizeof(raw));

    GString *key_str = g_string_new("pcv_");
    for (int i = 0; i < 32; i++) g_string_append_printf(key_str, "%02x", raw[i]);

    /* SHA256 해시 저장 (평문 키는 DB에 저장하지 않음) */
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key_str->str, -1);

    /* SEC-6: _ensure_apikey_table()을 g_rbac_mutex 획득 이후로 이동 —
     * "모든 SQLite 접근은 g_rbac_mutex로 직렬화" 불변식 정합. _ensure는
     * 내부 락이 없으므로 여기서 호출해도 데드락 없음. */
    g_mutex_lock(&g_rbac_mutex);
    _ensure_apikey_table();
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "INSERT INTO api_keys (key_hash, client_name, role, description, expires_at) "
        "VALUES (?, ?, ?, ?, ?)", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, client_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, (int)role);
        sqlite3_bind_text(stmt, 4, description ? description : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, expires_at > 0 ? expires_at : 0);
        rc = sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);

    if (rc != SQLITE_DONE) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create API key");
        g_free(hash);
        g_string_free(key_str, TRUE);
        return FALSE;
    }

    *out_key = g_string_free(key_str, FALSE);
    g_free(hash);
    return TRUE;
}

/**
 * pcv_rbac_apikey_validate — API Key 검증 + role 반환
 * @return PcvRole (유효), -1 (무효/폐기)
 */
gint
pcv_rbac_apikey_validate(const gchar *api_key)
{
    if (!api_key || !g_str_has_prefix(api_key, "pcv_")) return -1;

    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, api_key, -1);
    gint role = -1;
    gint64 now_epoch = g_get_real_time() / G_USEC_PER_SEC;

    /* SEC-6: _ensure_apikey_table() 호출을 g_rbac_mutex 획득 이후로 이동 */
    g_mutex_lock(&g_rbac_mutex);
    _ensure_apikey_table();
    sqlite3_stmt *stmt = nullptr;
    /* revoked=0 + 만료(expires_at) 집행 — verify_api_key 와 동일 계약 */
    if (sqlite3_prepare_v2(g_rbac_db,
        "SELECT role FROM api_keys WHERE key_hash = ? AND revoked = 0 "
        "AND (expires_at = 0 OR expires_at > ?)", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, now_epoch);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            role = sqlite3_column_int(stmt, 0);
    }
    if (stmt) sqlite3_finalize(stmt);

    /* last_used_at 갱신 — prepared stmt로 SQL injection 방지 */
    if (role >= 0) {
        sqlite3_stmt *upd = nullptr;
        if (sqlite3_prepare_v2(g_rbac_db,
            "UPDATE api_keys SET last_used_at=datetime('now') WHERE key_hash=?",
            -1, &upd, NULL) == SQLITE_OK) {
            sqlite3_bind_text(upd, 1, hash, -1, SQLITE_TRANSIENT);
            sqlite3_step(upd);
            sqlite3_finalize(upd);
        }
    }
    g_mutex_unlock(&g_rbac_mutex);
    g_free(hash);
    return role;
}

/**
 * pcv_rbac_apikey_list — 전체 API Key 목록 (평문 키 제외)
 */
JsonArray *
pcv_rbac_apikey_list(void)
{
    JsonArray *arr = json_array_new();

    /* SEC-6: _ensure_apikey_table() 호출을 g_rbac_mutex 획득 이후로 이동 */
    g_mutex_lock(&g_rbac_mutex);
    _ensure_apikey_table();
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_rbac_db,
        "SELECT client_name, role, description, created_at, last_used_at, "
        "expires_at, revoked FROM api_keys ORDER BY created_at DESC",
        -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "client_name", (const char *)sqlite3_column_text(stmt, 0));
            json_object_set_int_member(obj, "role", sqlite3_column_int(stmt, 1));
            const char *desc = (const char *)sqlite3_column_text(stmt, 2);
            json_object_set_string_member(obj, "description", desc ? desc : "");
            json_object_set_string_member(obj, "created_at", (const char *)sqlite3_column_text(stmt, 3));
            const char *lu = (const char *)sqlite3_column_text(stmt, 4);
            if (lu) json_object_set_string_member(obj, "last_used_at", lu);
            /* expires_at: epoch 초, 0 = 무기한 */
            json_object_set_int_member(obj, "expires_at", sqlite3_column_int64(stmt, 5));
            json_object_set_boolean_member(obj, "revoked", sqlite3_column_int(stmt, 6) != 0);
            json_array_add_object_element(arr, obj);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    g_mutex_unlock(&g_rbac_mutex);
    return arr;
}

/**
 * pcv_rbac_apikey_revoke — API Key 폐기
 */
gboolean
pcv_rbac_apikey_revoke(const gchar *client_name, GError **error)
{
    /* SEC-6: _ensure_apikey_table() 호출을 g_rbac_mutex 획득 이후로 이동 */
    g_mutex_lock(&g_rbac_mutex);
    _ensure_apikey_table();
    /* security: prepared stmt prevents SQL injection via client_name */
    sqlite3_stmt *upd = nullptr;
    int rc = sqlite3_prepare_v2(g_rbac_db,
        "UPDATE api_keys SET revoked=1 WHERE client_name=?",
        -1, &upd, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(upd, 1, client_name, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(upd);
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
        sqlite3_finalize(upd);
    }
    g_mutex_unlock(&g_rbac_mutex);
    if (rc != SQLITE_OK) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Revoke failed: %s", sqlite3_errmsg(g_rbac_db));
        return FALSE;
    }
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════
 * [12] RBAC 권한 캐싱 — SQLite 라운드트립 절감
 *
 * 캐시: GHashTable("username:method" → min_role 충족 여부)
 * TTL: 60초, auth.user.set_role/delete 시 무효화
 * ══════════════════════════════════════════════════════════════ */

static GHashTable *g_perm_cache    = nullptr;  /* "user:method" → GINT_TO_POINTER(allowed) */
static GHashTable *g_perm_cache_ts = nullptr;  /* "user:method" → gint64* (timestamp) */
static GMutex      g_perm_cache_mu;
#define PERM_CACHE_TTL_SEC 60

void pcv_rbac_perm_cache_init(void) {
    g_mutex_init(&g_perm_cache_mu);
    g_perm_cache    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_perm_cache_ts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

void pcv_rbac_perm_cache_invalidate(const gchar *username) {
    if (!g_perm_cache) return;
    g_mutex_lock(&g_perm_cache_mu);
    /* 해당 사용자의 모든 캐시 엔트리 제거 */
    GHashTableIter iter;
    gpointer key, value;
    GList *to_remove = nullptr;
    g_hash_table_iter_init(&iter, g_perm_cache);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (g_str_has_prefix((const gchar *)key, username))
            to_remove = g_list_prepend(to_remove, g_strdup((const gchar *)key));
    }
    for (GList *l = to_remove; l; l = l->next) {
        g_hash_table_remove(g_perm_cache, l->data);
        g_hash_table_remove(g_perm_cache_ts, l->data);
    }
    g_list_free_full(to_remove, g_free);
    g_mutex_unlock(&g_perm_cache_mu);
}

gint pcv_rbac_perm_cache_check(const gchar *username, const gchar *method) {
    if (!g_perm_cache || !username || !method) return -1;  /* -1 = cache miss */
    /* perf: stack key avoids heap alloc on every permission check hot path */
    gchar key[192];
    g_snprintf(key, sizeof(key), "%s:%s", username, method);
    gint result = -1;

    g_mutex_lock(&g_perm_cache_mu);
    gpointer val = g_hash_table_lookup(g_perm_cache, key);
    gint64 *ts = g_hash_table_lookup(g_perm_cache_ts, key);
    if (val && ts) {
        gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
        if (now - *ts < PERM_CACHE_TTL_SEC)
            result = GPOINTER_TO_INT(val);  /* 1=allowed, 0=denied */
        else {
            g_hash_table_remove(g_perm_cache, key);
            g_hash_table_remove(g_perm_cache_ts, key);
        }
    }
    g_mutex_unlock(&g_perm_cache_mu);
    /* perf: no g_free needed — key is stack-allocated */
    return result;
}

void pcv_rbac_perm_cache_set(const gchar *username, const gchar *method, gboolean allowed) {
    if (!g_perm_cache || !username || !method) return;
    /* perf: stack key for lookup; g_strdup(key) only for hash table ownership */
    gchar key[192];
    g_snprintf(key, sizeof(key), "%s:%s", username, method);
    gint64 *ts = g_new(gint64, 1);
    *ts = g_get_monotonic_time() / G_USEC_PER_SEC;

    g_mutex_lock(&g_perm_cache_mu);
    g_hash_table_replace(g_perm_cache, g_strdup(key), GINT_TO_POINTER(allowed ? 1 : 0));
    g_hash_table_replace(g_perm_cache_ts, g_strdup(key), ts);
    g_mutex_unlock(&g_perm_cache_mu);
}

/* ══════════════════════════════════════════════════════════════
 * [13] Per-user Rate Limiting — 토큰 버킷
 * ══════════════════════════════════════════════════════════════ */

static GHashTable *g_user_rate = nullptr;  /* username → {count, window_start} */
static GMutex      g_user_rate_mu;
#define USER_RATE_LIMIT  100   /* 분당 최대 요청 */
#define USER_RATE_WINDOW  60   /* 윈도우 (초) */

typedef struct { gint count; gint64 window_start; } UserRateInfo;

gboolean
pcv_rbac_check_user_rate(const gchar *username)
{
    if (!username) return TRUE;
    if (!g_user_rate) {
        g_mutex_init(&g_user_rate_mu);
        g_user_rate = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    }
    g_mutex_lock(&g_user_rate_mu);
    UserRateInfo *info = g_hash_table_lookup(g_user_rate, username);
    gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
    if (!info) {
        info = g_new0(UserRateInfo, 1);
        info->window_start = now;
        g_hash_table_insert(g_user_rate, g_strdup(username), info);
    }
    if (now - info->window_start >= USER_RATE_WINDOW) {
        info->count = 0;
        info->window_start = now;
    }
    info->count++;
    gboolean allowed = (info->count <= USER_RATE_LIMIT);
    g_mutex_unlock(&g_user_rate_mu);
    return allowed;
}
