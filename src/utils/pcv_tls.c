/**
 * @file pcv_tls.c
 * @brief mTLS 인증 — TLS 컨텍스트 관리 + PKI 초기화
 *
 * 엔터프라이즈 보안 단계에서 도입된 TLS 인증 모듈입니다.
 * daemon.conf [tls] enabled=true 시 활성화되며,
 * 미설정 또는 인증서 부재 시 graceful degradation (HTTP 평문 유지).
 *
 * [아키텍처 위치]
 *   main.c → pcv_tls_init_from_config() (pcv_config_init 이후)
 *          → REST 서버(libsoup3)가 TLS 컨텍스트를 사용하여 HTTPS 제공
 *          → 노드 간 통신(etcd, 복제, 마이그레이션) mTLS 인증
 *
 *   초기화 후 흐름:
 *     rest_server.c → pcv_tls_is_enabled() 확인
 *                   → TRUE면 pcv_tls_get_cert_path()/get_key_path()로
 *                     SoupServer에 HTTPS 바인딩 추가
 *                   → FALSE면 HTTP(80)만 동작
 *
 * [설정 파일]
 *   /etc/purecvisor/daemon.conf:
 *     [tls]
 *     enabled=true                             # TLS 활성화 (기본: false)
 *     cert=/etc/purecvisor/pki/node.crt        # 서버 인증서 (PEM)
 *     key=/etc/purecvisor/pki/node.key         # 서버 개인키 (PEM)
 *     ca=/etc/purecvisor/pki/ca.crt            # CA 인증서 (PEM, 클라이언트 검증용)
 *
 * [Graceful Degradation 전략]
 *   TLS 초기화 실패 시 (파일 부재, 파싱 에러 등) 경고 로그만 출력하고
 *   HTTP 평문 모드로 계속 동작합니다. 이는:
 *   - 개발/테스트 환경에서 인증서 없이도 데몬이 시작되도록 하기 위함
 *   - 인증서 만료/갱신 시 데몬 재시작 없이 운영 가능
 *   - 데몬이 TLS 설정 오류로 시작 불가가 되는 것을 방지
 *
 * [다른 모듈과의 관계]
 *   - pcv_config.c    : [tls] 섹션 설정값 조회
 *   - rest_server.c   : HTTPS 바인딩 시 cert/key 경로 조회
 *   - health_probe.c  : /health 심층 프로브에서 TLS 상태 포함
 *   - Web UI          : 로그인 페이지에서 TLS 상태 표시, HTTPS 전환 링크
 *
 * [의존성]
 *   GLib (g_file_test, g_new0), json-glib (상태 조회 JSON 반환),
 *   pcv_config (설정 파일 조회), pcv_log (로깅)
 *
 * [주의사항]
 *   - G 전역 상태는 프로세스당 1개 (싱글턴)
 *   - pcv_tls_ctx_new()는 인증서 파일 존재만 확인 (내용 유효성은 libsoup이 검증)
 *   - pcv_tls_status()는 JSON-RPC 상태 조회용 (/health 엔드포인트에서 사용)
 *   - 인증서 핫 리로드(hot reload)는 미구현: 변경 시 데몬 재시작 필요
 */
#include "pcv_tls.h"
#include "pcv_log.h"
#include "pcv_config.h"
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <string.h>

/** TLS_LOG_DOM - 이 모듈의 로그 도메인. journalctl에서 "dom":"pcv_tls"로 필터링 */
#define TLS_LOG_DOM "pcv_tls"

/* ── TLS 컨텍스트 구조체: 인증서/키/CA 파일 경로 보관 ──────── */

/**
 * struct _PcvTlsCtx - TLS 설정 컨텍스트
 *
 * 인증서, 개인키, CA 인증서의 파일 경로를 보관합니다.
 * REST 서버(libsoup3)가 HTTPS를 설정할 때 이 경로를 사용합니다.
 *
 * [PEM 파일 형식]
 *   -----BEGIN CERTIFICATE-----
 *   MIIDdzCCAl+gAwIBAgIE...
 *   -----END CERTIFICATE-----
 *
 * [인증서 역할]
 *   cert_path: 서버 인증서 — 클라이언트에게 제시하여 신원 증명
 *   key_path:  서버 개인키 — TLS 핸드셰이크에서 서명에 사용 (절대 공개 금지)
 *   ca_path:   CA 인증서 — 클라이언트 인증서 검증에 사용 (mTLS)
 *
 * [mTLS(Mutual TLS)]
 *   서버와 클라이언트가 서로의 인증서를 검증합니다.
 *   PureCVisor 클러스터에서 노드 간 통신에 사용됩니다.
 *   ca_path가 있으면 클라이언트도 인증서를 제시해야 합니다.
 */
struct _PcvTlsCtx {
    gchar *cert_path;     /* 서버 인증서 파일 경로 (PEM) */
    gchar *key_path;      /* 서버 개인키 파일 경로 (PEM) */
    gchar *ca_path;       /* CA 인증서 파일 경로 (PEM, mTLS 클라이언트 검증용) */
    gboolean enabled;     /* TLS 활성 상태 플래그 */
};

/* ── 모듈 전역 싱글턴 상태 ──────────────────────────────────── */

/**
 * G - TLS 모듈 전역 상태 (프로세스당 1개)
 *
 * ctx가 NULL이면 TLS가 비활성 상태입니다.
 * initialized는 pcv_tls_init_from_config() 호출 완료 여부를 나타냅니다.
 * 이 구분이 필요한 이유:
 *   - 초기화 전: TLS 상태 미결정 (아직 설정을 읽지 않음)
 *   - 초기화 후 ctx=NULL: TLS 비활성 (의도적으로 끈 상태 또는 인증서 부재)
 */
static struct {
    PcvTlsCtx *ctx;         /* TLS 컨텍스트 (NULL이면 TLS 비활성) */
    gboolean   initialized; /* pcv_tls_init_from_config() 호출 완료 여부 */
} G = {0};

/**
 * pcv_tls_ctx_new - TLS 컨텍스트 생성
 * @cert:  서버 인증서 파일 경로 (PEM 형식)
 * @key:   서버 개인키 파일 경로 (PEM 형식)
 * @ca:    CA 인증서 파일 경로 (PEM 형식, mTLS 클라이언트 검증용)
 * @error: 실패 시 GError 설정
 *
 * @return: 새 PcvTlsCtx 포인터 또는 NULL (실패 시)
 *
 * [검증 범위]
 *   파일 존재 여부만 확인합니다. 인증서 형식, 만료일, 키 쌍 일치 등의
 *   유효성은 실제 TLS 핸드셰이크 시 libsoup/GnuTLS가 검증합니다.
 *   이는 초기화 시점에 인증서 내용을 파싱하지 않아 빠른 시작을 보장합니다.
 *
 * [메모리 관리]
 *   반환된 PcvTlsCtx는 호출자가 pcv_tls_ctx_free()로 해제합니다.
 *   실제로는 G.ctx에 저장되어 프로세스 수명 동안 유지됩니다.
 *
 * [에러 코드]
 *   quark "tls", code 1: cert/key/ca 경로 누락 (NULL)
 *   quark "tls", code 2: 인증서 파일이 존재하지 않음
 */
PcvTlsCtx *pcv_tls_ctx_new(const gchar *cert, const gchar *key,
                             const gchar *ca, GError **error)
{
    /* 3개 경로 모두 필수 (mTLS 구성) */
    if (!cert || !key || !ca) {
        g_set_error(error, g_quark_from_static_string("tls"), 1,
                    "cert, key, ca paths required");
        return NULL;
    }

    /* 파일 존재 확인 — 3개 모두 존재해야 TLS 활성화 */
    if (!g_file_test(cert, G_FILE_TEST_EXISTS) ||
        !g_file_test(key, G_FILE_TEST_EXISTS) ||
        !g_file_test(ca, G_FILE_TEST_EXISTS)) {
        g_set_error(error, g_quark_from_static_string("tls"), 2,
                    "Certificate files not found");
        return NULL;
    }

    /* g_new0: 메모리 할당 + 0 초기화 (enabled = FALSE 초기값) */
    PcvTlsCtx *ctx = g_new0(PcvTlsCtx, 1);
    ctx->cert_path = g_strdup(cert);
    ctx->key_path = g_strdup(key);
    ctx->ca_path = g_strdup(ca);
    ctx->enabled = TRUE;

    /* Certificate expiration check */
    GError *cert_err = NULL;
    GTlsCertificate *tls_cert = g_tls_certificate_new_from_file(cert, &cert_err);
    if (tls_cert) {
        GDateTime *not_after = g_tls_certificate_get_not_valid_after(tls_cert);
        if (not_after) {
            GDateTime *now = g_date_time_new_now_utc();
            if (g_date_time_compare(now, not_after) > 0) {
                PCV_LOG_WARN(TLS_LOG_DOM, "TLS certificate has EXPIRED — connections may fail");
            }
            g_date_time_unref(now);
            g_date_time_unref(not_after);
        }

        /* Log certificate subject for operational visibility */
        GDateTime *not_before = g_tls_certificate_get_not_valid_before(tls_cert);
        if (not_before && not_after == NULL)
            not_after = g_tls_certificate_get_not_valid_after(tls_cert);
        gchar *nb_str = not_before ? g_date_time_format_iso8601(not_before) : g_strdup("unknown");
        GDateTime *na_recheck = g_tls_certificate_get_not_valid_after(tls_cert);
        gchar *na_str = na_recheck ? g_date_time_format_iso8601(na_recheck) : g_strdup("unknown");
        PCV_LOG_INFO(TLS_LOG_DOM, "TLS certificate loaded: %s (valid: %s ~ %s)", cert, nb_str, na_str);
        g_free(nb_str);
        g_free(na_str);
        if (not_before) g_date_time_unref(not_before);
        if (na_recheck) g_date_time_unref(na_recheck);

        g_object_unref(tls_cert);
    } else {
        PCV_LOG_WARN(TLS_LOG_DOM, "Could not parse certificate for validation: %s",
                     cert_err ? cert_err->message : "unknown");
        if (cert_err) g_error_free(cert_err);
    }

    PCV_LOG_INFO(TLS_LOG_DOM, "TLS context created (cert=%s)", cert);
    return ctx;
}

/**
 * pcv_tls_ctx_free - TLS 컨텍스트 해제 (NULL 안전)
 * @ctx: 해제할 TLS 컨텍스트 (NULL이면 아무것도 하지 않음)
 *
 * 내부 문자열과 구조체를 해제합니다.
 * NULL 안전: ctx가 NULL이어도 크래시하지 않습니다.
 */
void pcv_tls_ctx_free(PcvTlsCtx *ctx)
{
    if (!ctx) return;
    g_free(ctx->cert_path);
    g_free(ctx->key_path);
    g_free(ctx->ca_path);
    g_free(ctx);
}

/**
 * pcv_tls_is_enabled - TLS 활성 상태 조회
 *
 * @return: TRUE이면 TLS 활성 (HTTPS 사용 가능)
 *
 * REST 서버에서 HTTPS 바인딩 여부를 결정할 때 사용합니다.
 * 3가지 조건이 모두 충족되어야 TRUE:
 *   1. pcv_tls_init_from_config() 호출 완료 (initialized)
 *   2. TLS 컨텍스트 생성 성공 (ctx != NULL)
 *   3. 컨텍스트 내부 enabled 플래그 TRUE
 */
gboolean pcv_tls_is_enabled(void)
{
    return G.initialized && G.ctx && G.ctx->enabled;
}

/**
 * pcv_tls_get_cert_path - 서버 인증서 경로 반환
 *
 * @return: 인증서 파일 경로 문자열 또는 NULL (TLS 비활성 시)
 *
 * rest_server.c에서 SoupServer HTTPS 바인딩에 사용합니다.
 * 반환 포인터는 G.ctx 내부 문자열이므로 free하면 안 됩니다.
 */
const gchar *pcv_tls_get_cert_path(void)
{
    return (G.ctx) ? G.ctx->cert_path : NULL;
}

/**
 * pcv_tls_get_key_path - 서버 개인키 경로 반환
 *
 * @return: 개인키 파일 경로 문자열 또는 NULL (TLS 비활성 시)
 *
 * rest_server.c에서 SoupServer HTTPS 바인딩에 사용합니다.
 * 개인키는 보안상 가장 민감한 파일이므로 경로만 반환합니다.
 * 파일 권한은 0600 (소유자만 읽기)이어야 합니다.
 */
const gchar *pcv_tls_get_key_path(void)
{
    return (G.ctx) ? G.ctx->key_path : NULL;
}

/**
 * pcv_tls_status - TLS 상태를 JSON 객체로 반환
 *
 * @return: JsonObject* (호출자가 json_object_unref()로 해제)
 *
 * /health 심층 프로브, tls.status RPC, Web UI 로그인 페이지에서
 * TLS 상태를 표시하는 데 사용됩니다.
 *
 * [반환 JSON 형식]
 *   TLS 활성: {"enabled": true, "cert": "/etc/purecvisor/pki/node.crt", "ca": "/etc/purecvisor/pki/ca.crt"}
 *   TLS 비활성: {"enabled": false}
 */
JsonObject *pcv_tls_status(void)
{
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "enabled", pcv_tls_is_enabled());
    if (G.ctx) {
        json_object_set_string_member(obj, "cert", G.ctx->cert_path);
        json_object_set_string_member(obj, "ca", G.ctx->ca_path);
        /* key 경로는 보안상 상태 응답에 포함하지 않음 */
    }
    return obj;
}

/**
 * pcv_tls_pki_init - PKI 디렉터리 생성
 * @pki_dir: PKI 파일이 저장될 디렉터리 경로 (예: /etc/purecvisor/pki)
 * @error:   실패 시 GError 설정
 *
 * @return: TRUE이면 성공 (디렉터리 생성 또는 이미 존재)
 *
 * [현재 상태]
 *   인증서 자동 생성(CA 키 생성 + 서버 인증서 서명)은 아직 미구현입니다.
 *   현재는 디렉터리 구조만 준비하고, 인증서는 운영자가 수동 배치합니다.
 *
 * [수동 인증서 생성 예시]
 *   # CA 키 + 인증서 생성
 *   openssl genrsa -out ca.key 4096
 *   openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 -out ca.crt
 *
 *   # 노드 키 + CSR + 인증서 서명
 *   openssl genrsa -out node.key 2048
 *   openssl req -new -key node.key -out node.csr
 *   openssl x509 -req -in node.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out node.crt
 *
 * [디렉터리 권한]
 *   0700: root만 읽기/쓰기/실행 (개인키 보호)
 */
gboolean pcv_tls_pki_init(const gchar *pki_dir, GError **error)
{
    if (!pki_dir) {
        g_set_error(error, g_quark_from_static_string("tls"), 1, "pki_dir required");
        return FALSE;
    }

    /* PKI 디렉터리 재귀 생성 (이미 존재하면 0 반환 — 멱등성) */
    if (g_mkdir_with_parents(pki_dir, 0700) < 0) {
        g_set_error(error, g_quark_from_static_string("tls"), 2,
                    "Failed to create PKI dir: %s", pki_dir);
        return FALSE;
    }
    PCV_LOG_INFO(TLS_LOG_DOM, "PKI directory ready: %s", pki_dir);
    return TRUE;
}

/* ── 인증서 만료 감시 ────────────────────────────────────────────── */

/**
 * pcv_tls_get_cert_expiry_days - TLS 인증서 만료까지 남은 일수 반환
 *
 * GLib GTlsCertificate API를 사용하여 인증서 만료 시각을 읽고
 * 현재 시각과의 차이를 일(day) 단위로 반환합니다.
 *
 * @return: 만료까지 남은 일수 (음수이면 이미 만료). 인증서 미설정 시 -1.
 */
gint64
pcv_tls_get_cert_expiry_days(void)
{
    if (!G.ctx || !G.ctx->cert_path)
        return -1;

    GError *err = NULL;
    GTlsCertificate *cert = g_tls_certificate_new_from_file(G.ctx->cert_path, &err);
    if (!cert) {
        if (err) g_error_free(err);
        return -1;
    }

    GDateTime *not_after = g_tls_certificate_get_not_valid_after(cert);
    if (!not_after) {
        g_object_unref(cert);
        return -1;
    }

    GDateTime *now = g_date_time_new_now_utc();
    GTimeSpan diff = g_date_time_difference(not_after, now);
    gint64 days = diff / G_TIME_SPAN_DAY;

    g_date_time_unref(now);
    g_date_time_unref(not_after);
    g_object_unref(cert);

    return days;
}

/**
 * pcv_tls_check_expiry_warning - 인증서 만료 임박 시 경고 로그 출력
 *
 * 만료까지 남은 일수에 따라 로그 레벨을 다르게 출력합니다:
 *   < 7일:  CRITICAL (즉시 갱신 필요)
 *   < 30일: WARNING  (갱신 계획 수립 필요)
 *   >= 30일: 무시 (정상)
 *
 * ebpf_telemetry.c의 수집 루프에서 주기적으로 호출됩니다.
 */
void
pcv_tls_check_expiry_warning(void)
{
    gint64 days = pcv_tls_get_cert_expiry_days();
    if (days < 0)
        return;  /* TLS 미설정 또는 파싱 실패 */

    if (days < 7) {
        PCV_LOG_ERROR(TLS_LOG_DOM,
                      "TLS certificate expires in %" G_GINT64_FORMAT " days! Renew immediately!",
                      days);
    } else if (days < 30) {
        PCV_LOG_WARN(TLS_LOG_DOM,
                     "TLS certificate expires in %" G_GINT64_FORMAT " days — plan renewal",
                     days);
    }
}

/**
 * pcv_tls_init_from_config - daemon.conf [tls] 섹션에서 TLS 자동 초기화
 *
 * main.c에서 pcv_config_init() 직후 1회 호출합니다.
 *
 * [동작 흐름]
 *   1. [tls] enabled 키 확인 → "true"가 아니면 TLS 비활성 (기본값)
 *   2. cert/key/ca 경로 읽기 (각각 기본값: /etc/purecvisor/pki/ 하위)
 *   3. pcv_tls_ctx_new()로 컨텍스트 생성 시도
 *      - 성공: G.ctx에 저장, HTTPS 활성
 *      - 실패: 경고 로그 + HTTP 평문 모드 계속 (graceful degradation)
 *   4. G.initialized = TRUE 설정
 *
 * [Graceful Degradation 시나리오]
 *   - [tls] enabled=true지만 인증서 파일 없음 → WARN 로그 + HTTP 유지
 *   - [tls] 섹션 자체가 없음 → INFO 로그 + HTTP 유지
 *   - 인증서 파일 손상 → pcv_tls_ctx_new()에서 파일 존재만 확인하므로
 *     컨텍스트 생성은 성공하지만, 이후 libsoup HTTPS 바인딩에서 실패
 *
 * [왜 데몬을 중단하지 않는가?]
 *   개발/테스트 환경에서 인증서 없이도 데몬이 정상 시작되어야 합니다.
 *   TLS는 선택적 기능이며, HTTP 모드에서도 모든 핵심 기능이 동작합니다.
 */
void pcv_tls_init_from_config(void)
{
    /* [tls] enabled 설정 확인 — "true" 문자열만 활성화 */
    const gchar *enabled = pcv_config_get_string("tls", "enabled", "false");
    if (g_strcmp0(enabled, "true") != 0) {
        PCV_LOG_INFO(TLS_LOG_DOM, "TLS disabled (daemon.conf [tls] enabled != true)");
        G.initialized = TRUE;
        return;   /* TLS 비활성 — ctx는 NULL 유지 */
    }

    /* 인증서/키/CA 경로 조회 (기본값: /etc/purecvisor/pki/ 하위) */
    const gchar *cert = pcv_config_get_string("tls", "cert", "/etc/purecvisor/pki/node.crt");
    const gchar *key = pcv_config_get_string("tls", "key", "/etc/purecvisor/pki/node.key");
    const gchar *ca = pcv_config_get_string("tls", "ca", "/etc/purecvisor/pki/ca.crt");

    /* TLS 컨텍스트 생성 시도 */
    GError *err = NULL;
    G.ctx = pcv_tls_ctx_new(cert, key, ca, &err);
    if (!G.ctx) {
        /* Graceful degradation: 경고만 출력하고 HTTP 모드로 계속 */
        PCV_LOG_WARN(TLS_LOG_DOM, "TLS init failed: %s — running without TLS",
                     err ? err->message : "unknown");
        if (err) g_error_free(err);
    }
    G.initialized = TRUE;
}
