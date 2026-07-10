/**
 * @file rest_middleware.c
 * @brief REST 미들웨어 — ETag, Rate Limit, 파라미터 검증, RPC 타임아웃
 *
 * ============================================================================
 *  아키텍처 위치
 * ============================================================================
 *  rest_server.c의 _on_request() 콜백에서 호출되는 횡단 관심사(cross-cutting concern)
 *  함수들을 모아놓은 미들웨어 계층입니다. rest_server.c → rest_middleware.c 방향으로만
 *  호출하며, 역방향 의존은 없습니다 (pcv_rest_error만 예외적으로 여기에 self-contained).
 *
 *    [HTTP 요청] → rest_server._on_request()
 *                    → pcv_get_endpoint_rate_limit()  // Rate Limit 티어 결정
 *                    → pcv_validate_required()         // 필수 필드 사전 검증
 *                    → (RPC 호출)
 *                    → pcv_compute_etag()              // 응답 ETag 생성
 *                    → pcv_get_rpc_timeout()           // RPC별 타임아웃 결정
 *
 * ============================================================================
 *  분리 배경
 * ============================================================================
 *  rest_server.c가 1,000+ LOC로 비대해져 유지보수가 어려워졌습니다.
 *  비즈니스 로직(라우팅)과 횡단 관심사(보안/캐싱/검증)를 분리하여
 *  rest_server.c는 라우팅에만 집중하고, 이 파일은 미들웨어만 담당합니다.
 *
 * ============================================================================
 *  주니어 개발자 필독: 각 미들웨어의 역할
 * ============================================================================
 *  - ETag (pcv_compute_etag): HTTP 캐싱 최적화. 응답 본문의 MD5 해시를 ETag 헤더로
 *    전달하면, 클라이언트가 If-None-Match 헤더로 동일 요청 시 304 Not Modified를
 *    반환하여 네트워크 대역폭을 절약합니다. MD5를 쓰는 이유: 보안용이 아니라
 *    콘텐츠 변경 감지용이므로, SHA-256보다 빠른 MD5로 충분합니다.
 *
 *  - Rate Limit (pcv_get_endpoint_rate_limit): 엔드포인트별 차등 제한.
 *    인증 API는 브루트포스 방지를 위해 엄격(60/min), 모니터링은 느슨(3600/min).
 *
 *  - RPC 타임아웃 (pcv_get_rpc_timeout): RPC 메서드별 차등 타임아웃.
 *    ZFS/마이그레이션 등 장기 작업은 60초, 일반 조회는 8초.
 *
 *  - 입력 검증 (pcv_validate_required): REST JSON body의 필수 필드 사전 검증.
 *    핸들러 진입 전에 누락 필드를 400 Bad Request로 거부합니다.
 *
 *  [ADR-0014] CSRF 토큰은 제거됨. JWT Bearer 인증이 CSRF 방어를 대체합니다.
 * ============================================================================
 */

#include "rest_middleware.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <glib.h>
#include <string.h>

/* ── Per-endpoint Rate Limit 티어 (BE-A4) ──────────────────────── */
static const gchar *
_endpoint_rate_bucket(const gchar *path, const gchar *http_method)
{
    if (!path) return "default";

    if (g_str_has_prefix(path, "/api/v1/auth/"))
        return "auth";
    if (g_str_has_prefix(path, "/api/v1/metrics"))
        return "metrics";
    if (g_str_has_prefix(path, "/api/v1/health"))
        return "health";
    if (g_str_has_prefix(path, "/api/v1/vms") &&
        g_strcmp0(http_method, "POST") == 0)
        return "vms-post";

    return "default";
}

/**
 * pcv_get_endpoint_rate_limit — 엔드포인트별 Rate Limit 티어를 반환 (req/min)
 *
 * [호출 시점] rest_server.c의 Rate Limit 체크 시
 * [동작] 경로(path)와 HTTP 메서드(method)를 조합하여 적절한 제한값을 반환
 *
 * 티어 설계 근거 (BE-A4):
 *   - /auth: 60/min — 브루트포스 공격 방어 (초당 1회)
 *   - /metrics, /health: 3600/min — Prometheus 등 자동화 에이전트가 빈번히 폴링
 *   - POST /vms (VM 생성): 120/min — 리소스 소모가 크므로 중간 제한
 *   - 기본: 600/min — 일반 API 호출
 *
 * @param path        요청 경로 (예: "/api/v1/auth/token")
 * @param http_method HTTP 메서드 (예: "POST", "GET")
 * @return 분당 허용 요청 수 (정수)
 */
gint
pcv_get_endpoint_rate_limit(const gchar *path, const gchar *http_method)
{
    const gchar *bucket = _endpoint_rate_bucket(path, http_method);

    /* 인증 엔드포인트: 브루트포스 방지 — 60 req/min */
    if (g_strcmp0(bucket, "auth") == 0)
        return 60;
    /* 모니터링/헬스체크: 자동화 에이전트가 빈번히 폴링 — 3600 req/min */
    if (g_strcmp0(bucket, "metrics") == 0 ||
        g_strcmp0(bucket, "health") == 0)
        return 3600;
    /* VM 생성(POST /vms): 중간 제한 — 120 req/min */
    if (g_strcmp0(bucket, "vms-post") == 0)
        return 120;
    /* 기본 — 600 req/min */
    return 600;
}

gchar *
pcv_build_rate_limit_key(const gchar *client_ip,
                         const gchar *path,
                         const gchar *http_method)
{
    const gchar *ip = (client_ip && *client_ip) ? client_ip : "unknown";
    return g_strdup_printf("%s:%s", ip,
                           _endpoint_rate_bucket(path, http_method));
}

/* ── Per-method RPC 타임아웃 (BE-A6) ──────────────────────────── */
constexpr int REST_RPC_TIMEOUT_SEC_MW = 8;

/**
 * pcv_get_rpc_timeout — RPC 메서드별 타임아웃(초)을 반환
 *
 * [호출 시점] rest_server.c에서 비동기 RPC 호출 시 soup_server_message_pause 타이머 설정
 * [동작] 메서드 이름 prefix/정확 매칭으로 3단계 타임아웃 분류:
 *   - 60초: ZFS 스냅샷, 백업 복원 (디스크 I/O 대기)
 *   - 30초: 클라우드 RPC, VM 생성 (네트워크 + 이미지 다운로드)
 *   - 8초(기본): 일반 조회/상태 변경 (libvirt API 호출)
 *
 * @param rpc_method JSON-RPC 메서드 이름 (예: "vm.create", "zfs.snapshot.create")
 * @return 타임아웃 초 단위
 */
gint
pcv_get_rpc_timeout(const gchar *rpc_method)
{
    if (!rpc_method) return REST_RPC_TIMEOUT_SEC_MW;
    /* 장기 실행 작업 — 60초 */
    if (g_str_has_prefix(rpc_method, "zfs.") ||
        g_strcmp0(rpc_method, "backup.restore") == 0
#if PCV_CLUSTER_ENABLED
        || g_strcmp0(rpc_method, "vm.migrate") == 0
#endif
    )
        return 60;
    /* 중간 — 30초 (클러스터/클라우드/VM 생성) */
    if (g_str_has_prefix(rpc_method, "cloud.") ||
        g_strcmp0(rpc_method, "vm.create") == 0)
        return 30;
#if PCV_CLUSTER_ENABLED
    if (g_str_has_prefix(rpc_method, "cluster."))
        return 30;
#endif
    /* 기본 */
    return REST_RPC_TIMEOUT_SEC_MW;
}

/* ── ETag 생성 (BE-A1) ──────────────────────────────────────── */
/**
 * pcv_compute_etag — 응답 본문의 ETag 해시를 생성
 *
 * [호출 시점] rest_server.c의 _send_json()에서 응답 전송 직전
 * [동작] 응답 body를 MD5 해싱하여 앞 16자(8바이트)만 잘라 ETag 헤더 값으로 반환
 *        (RFC 7232 형식: 큰따옴표로 감싼 문자열 "abcdef0123456789")
 *
 * [왜 MD5인가?]
 *   ETag는 콘텐츠 변경 감지용이지 보안 해시가 아닙니다.
 *   MD5는 SHA-256 대비 ~3배 빠르고, 전체 32 hex chars(16바이트 = 2^128)를 사용하여
 *   충분한 충돌 저항성을 제공합니다.
 *
 * [주의] 호출자가 반환된 gchar*를 g_free()해야 합니다.
 *
 * @param body 응답 본문 문자열
 * @param len  응답 본문 길이 (바이트)
 * @return ETag 문자열 (큰따옴표 포함, 호출자가 g_free 필요)
 */
gchar *
pcv_compute_etag(const gchar *body, gsize len)
{
    GChecksum *cs = g_checksum_new(G_CHECKSUM_MD5);
    g_checksum_update(cs, (const guchar *)body, len);
    const gchar *hex = g_checksum_get_string(cs);
    /* MD5 전체 32자 사용 (16바이트 = 2^64 충돌 저항성) */
    gchar *etag = g_strdup_printf("\"%.*s\"", 32, hex);
    g_checksum_free(cs);
    return etag;
}

/* ── REST 에러 응답 헬퍼 ──────────────────────────────────────── */

/* Forward declaration: _send_json is in rest_server.c, but pcv_rest_error
 * needs a minimal JSON error sender. We duplicate the minimal logic here
 * to avoid circular dependency. The full _send_json with gzip/ETag/HSTS
 * remains in rest_server.c. */
/**
 * pcv_rest_error — 표준 JSON 에러 응답을 전송하는 미들웨어 헬퍼
 *
 * [호출 시점] CSRF 검증 실패, Rate Limit 초과, 필수 필드 누락 등 미들웨어 단계 에러
 * [동작] {"error":{"code":"<code>","message":"<detail>"}} 형태의 JSON 응답을 HTTP status와 함께 전송
 *
 * [왜 rest_server.c의 _send_json을 재사용하지 않는가?]
 *   _send_json은 gzip 압축/ETag/HSTS 등 풀 파이프라인을 수행하는데,
 *   이 파일에서 호출하면 rest_server.c → rest_middleware.c → rest_server.c로
 *   순환 의존이 발생합니다. 따라서 에러 전송에 필요한 최소 로직만 중복 구현합니다.
 *
 * @param msg    libsoup 메시지 객체
 * @param status HTTP 상태 코드 (예: 400, 403, 429)
 * @param code   에러 코드 문자열 (예: "BAD_REQUEST", "CSRF_INVALID")
 * @param detail 사람이 읽을 수 있는 에러 메시지
 */
void pcv_rest_error(SoupServerMessage *msg, guint status,
                    const gchar *code, const gchar *detail)
{
    gchar *body = g_strdup_printf(
        "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
        code, detail);
    soup_server_message_set_status(msg, status, NULL);
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(hdrs, "Content-Type",
                                  "application/json; charset=utf-8");
    soup_message_headers_replace(hdrs, "X-Content-Type-Options", "nosniff");
    soup_message_headers_replace(hdrs, "Cache-Control", "no-store");
    gsize body_len = strlen(body);
    soup_server_message_set_response(msg, "application/json",
                                      SOUP_MEMORY_COPY,
                                      body, body_len);
    g_free(body);
}

/* ── REST JSON 스키마 사전 검증 ──────────────────────────────── */
/**
 * pcv_validate_required — REST 요청 body에서 필수 JSON 필드를 사전 검증
 *
 * [호출 시점] rest_server.c의 POST/PUT 핸들러에서 RPC 호출 전
 * [동작] fields 배열의 각 키가 body JsonObject에 존재하는지 확인.
 *        누락 시 400 Bad Request + "Missing required field: xxx" 에러 응답을 즉시 전송.
 * [주의] body가 NULL이면 "Request body required" 에러를 반환합니다.
 *        모든 필드가 존재하면 TRUE를 반환하고, 하나라도 없으면 FALSE를 반환합니다.
 *        FALSE 반환 시 호출자는 즉시 return해야 합니다 (응답 이미 전송됨).
 *
 * @param msg    libsoup 메시지 객체 (에러 응답 전송용)
 * @param body   요청 body에서 파싱한 JsonObject (NULL 가능)
 * @param fields 필수 필드 이름 배열 (예: {"name", "vcpu", "memory_mb"})
 * @param count  fields 배열의 길이
 * @return TRUE: 모든 필수 필드 존재, FALSE: 누락 있음 (에러 응답 이미 전송됨)
 */
gboolean
pcv_validate_required(SoupServerMessage *msg, JsonObject *body,
                      const gchar *fields[], gint count)
{
    if (!body) {
        pcv_rest_error(msg, 400, "BAD_REQUEST", "Request body required (JSON)");
        return FALSE;
    }
    for (gint i = 0; i < count; i++) {
        if (!json_object_has_member(body, fields[i])) {
            gchar *detail = g_strdup_printf("Missing required field: %s", fields[i]);
            pcv_rest_error(msg, 400, "BAD_REQUEST", detail);
            g_free(detail);
            return FALSE;
        }
    }
    return TRUE;
}
