/**
 * @file rest_middleware.h
 * @brief REST 미들웨어 함수 — Rate Limit, ETag, RPC 타임아웃, 입력 검증
 *
 * rest_server.c에서 분리된 자기 완결적 미들웨어 유틸리티입니다.
 * [ADR-0014] CSRF 토큰은 제거됨 — JWT Bearer 인증이 CSRF 방어를 대체합니다.
 */

#ifndef PCV_REST_MIDDLEWARE_H
#define PCV_REST_MIDDLEWARE_H

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

/**
 * Per-endpoint Rate Limit 티어 (BE-A4)
 * 엔드포인트별 요청 빈도 제한. 인증은 엄격하게, 모니터링은 관대하게.
 * @param path        요청 경로
 * @param http_method HTTP 메서드 ("GET", "POST" 등)
 * @return 분당 허용 요청 수
 */
gint pcv_get_endpoint_rate_limit(const gchar *path, const gchar *http_method);

/**
 * Rate Limit 카운터 키 생성
 * 같은 IP라도 인증, 모니터링, VM 생성, 기본 API의 카운터를 분리합니다.
 *
 * @param client_ip   클라이언트 IP 문자열
 * @param path        요청 경로
 * @param http_method HTTP 메서드
 * @return "ip:bucket" 문자열 (호출자가 g_free)
 */
gchar *pcv_build_rate_limit_key(const gchar *client_ip,
                                const gchar *path,
                                const gchar *http_method);

/**
 * Per-method RPC 타임아웃 (BE-A6)
 * 장기 실행 RPC 메서드에 대해 기본 타임아웃을 오버라이드합니다.
 * @param rpc_method RPC 메서드명
 * @return 타임아웃 초
 */
gint pcv_get_rpc_timeout(const gchar *rpc_method);

/**
 * ETag 생성 (BE-A1)
 * GET 응답 본문의 MD5 해시 앞 16자를 ETag으로 사용합니다.
 * @param body 응답 본문
 * @param len  본문 길이
 * @return ETag 문자열 (호출자가 g_free)
 */
gchar *pcv_compute_etag(const gchar *body, gsize len);

/**
 * REST JSON 스키마 사전 검증
 * POST body에서 필수 필드 존재 여부를 사전 확인합니다.
 * 부재 시 400 Bad Request 즉시 반환.
 *
 * @param msg    SoupServerMessage
 * @param body   파싱된 JSON body
 * @param fields 필수 필드명 배열
 * @param count  필드 수
 * @return TRUE=유효, FALSE=에러 응답 전송 완료
 */
gboolean pcv_validate_required(SoupServerMessage *msg, JsonObject *body,
                               const gchar *fields[], gint count);

/* _error 헬퍼 — rest_middleware.c 내부 및 rest_server.c에서 사용 */
void pcv_rest_error(SoupServerMessage *msg, guint status,
                    const gchar *code, const gchar *detail);

#endif /* PCV_REST_MIDDLEWARE_H */
