/**
 * @file pcv_tls.h
 * @brief mTLS 인증 — TLS 컨텍스트 + PKI 관리 공개 헤더
 *
 * 엔터프라이즈 보안 단계에서 도입된 TLS 모듈의 공개 인터페이스입니다.
 * HTTPS 서버 바인딩, 노드 간 mTLS 통신, /health 상태 조회에 사용됩니다.
 *
 * [API 분류]
 *   초기화:   pcv_tls_ctx_new() / pcv_tls_ctx_free() / pcv_tls_init_from_config()
 *   상태:     pcv_tls_is_enabled() / pcv_tls_status()
 *   경로:     pcv_tls_get_cert_path() / pcv_tls_get_key_path()
 *   PKI:      pcv_tls_pki_init()
 *
 * [사용 예시]
 *   // main.c에서 초기화
 *   pcv_tls_init_from_config();
 *
 *   // rest_server.c에서 HTTPS 설정
 *   if (pcv_tls_is_enabled()) {
 *       soup_server_set_tls_certificate(server,
 *           pcv_tls_get_cert_path(), pcv_tls_get_key_path());
 *   }
 *
 *   // /health 응답에서 TLS 상태 포함
 *   JsonObject *tls = pcv_tls_status();
 *   json_object_set_object_member(health, "tls", tls);
 *
 * [주의사항]
 *   - TLS 비활성 시 get_cert_path()/get_key_path()는 NULL 반환
 *   - pcv_tls_init_from_config() 호출 전에 is_enabled()은 항상 FALSE
 *   - PcvTlsCtx는 불투명 타입(opaque type) — 내부 구조 직접 접근 금지
 */

#ifndef PURECVISOR_TLS_H
#define PURECVISOR_TLS_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * PcvTlsCtx - TLS 컨텍스트 (불투명 타입)
 *
 * 인증서, 개인키, CA 파일 경로를 보관하는 구조체입니다.
 * 내부 구조는 pcv_tls.c에서만 정의되며, 외부에서는 포인터로만 사용합니다.
 * pcv_tls_ctx_new()로 생성, pcv_tls_ctx_free()로 해제합니다.
 */
typedef struct _PcvTlsCtx PcvTlsCtx;

/* ── 초기화 / 해제 ───────────────────────────────────── */

/**
 * pcv_tls_ctx_new:
 * TLS 컨텍스트를 생성합니다. 인증서/키/CA 파일 존재를 확인합니다.
 * 실패 시 NULL + GError 반환. 성공 시 pcv_tls_ctx_free()로 해제.
 */
PcvTlsCtx *pcv_tls_ctx_new(const gchar *cert, const gchar *key,
                             const gchar *ca, GError **error);

/** pcv_tls_ctx_free: TLS 컨텍스트 해제. NULL 안전. */
void        pcv_tls_ctx_free(PcvTlsCtx *ctx);

/** pcv_tls_is_enabled: TLS 활성 여부 반환. REST 서버에서 HTTPS 분기에 사용. */
gboolean    pcv_tls_is_enabled(void);

/* ── 상태 조회 ───────────────────────────────────────── */

/**
 * pcv_tls_status:
 * TLS 상태를 JSON 객체로 반환합니다. /health 응답에 포함됩니다.
 * 반환: {"enabled": bool, "cert": "...", "ca": "..."} (호출자 unref)
 */
JsonObject *pcv_tls_status(void);

/* ── 인증서 경로 접근 (HTTPS 서버 바인딩용) ──────────── */

/** 서버 인증서 파일 경로 (TLS 비활성 시 NULL) */
const gchar *pcv_tls_get_cert_path(void);

/** 서버 개인키 파일 경로 (TLS 비활성 시 NULL) */
const gchar *pcv_tls_get_key_path(void);

/** CA 인증서 파일 경로 (mTLS 클라이언트 인증서 검증용, TLS 비활성 시 NULL) */
const gchar *pcv_tls_get_ca_path(void);

/* ── PKI 디렉터리 관리 ───────────────────────────────── */

/**
 * pcv_tls_pki_init:
 * PKI 디렉터리 생성 (인증서/키 파일을 저장할 경로).
 * 인증서 자동 생성은 미구현 — 수동 배치 필요.
 */
gboolean    pcv_tls_pki_init(const gchar *pki_dir, GError **error);

/* ── 인증서 만료 감시 ────────────────────────────────── */

/**
 * pcv_tls_get_cert_expiry_days:
 * TLS 인증서 만료까지 남은 일수를 반환합니다.
 * 인증서 미설정 또는 파싱 실패 시 -1 반환.
 */
gint64      pcv_tls_get_cert_expiry_days(void);

/**
 * pcv_tls_check_expiry_warning:
 * 인증서 만료까지 30일 미만이면 WARNING, 7일 미만이면 CRITICAL 로그를 출력합니다.
 * ebpf_telemetry.c에서 주기적으로 호출.
 */
void        pcv_tls_check_expiry_warning(void);

/* ── 설정 로드 ───────────────────────────────────────── */

/**
 * pcv_tls_init_from_config:
 * daemon.conf [tls] 섹션에서 TLS를 자동 초기화합니다.
 * main.c에서 pcv_config_init() 직후 1회 호출.
 * 실패 시 HTTP 평문 모드로 graceful degradation.
 */
void        pcv_tls_init_from_config(void);

G_END_DECLS
#endif
