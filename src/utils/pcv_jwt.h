/**
 * @file pcv_jwt.h
 * @brief JWT (JSON Web Token) HS256 서명/검증 — 공개 헤더
 *
 * Sprint E에서 도입된 경량 JWT 유틸리티의 공개 인터페이스입니다.
 * REST API(rest_server.c)에서 인증 토큰 발행/검증에 사용합니다.
 *
 * [알고리즘] HS256 (HMAC-SHA256) — RFC 7519
 * [토큰 형식] base64url(header).base64url(payload).base64url(signature)
 *
 * [외부 의존]
 *   GLib      : g_base64_encode/decode, GString, GMutex
 *   json-glib : JsonBuilder, JsonParser (payload JSON 생성/파싱)
 *   libcrypto : HMAC(), EVP_sha256(), CRYPTO_memcmp() (pkg-config: libcrypto)
 *
 * [초기화 순서]
 *   main.c에서: pcv_config_init() → pcv_jwt_init(pcv_config_get_jwt_secret()) → ...
 *   종료 시:    pcv_jwt_shutdown()
 *
 * [사용 예시]
 *   // 토큰 발행 (POST /auth/token 처리 시)
 *   GError *err = NULL;
 *   gchar *token = pcv_jwt_sign("admin", 3600, &err);  // 1시간 유효
 *   if (!token) { ... 에러 처리 ... }
 *
 *   // 토큰 검증 (Authorization 헤더 검증 시)
 *   gchar *sub = pcv_jwt_verify("Bearer eyJhbGci...", &err);
 *   if (sub) {
 *       // sub == "admin" → RBAC 권한 확인
 *       g_free(sub);
 *   }
 *   g_free(token);
 *
 * [주의사항]
 *   - secret이 NULL/빈 문자열이면 /dev/urandom 랜덤 키 생성 (재시작 시 기존 토큰 무효)
 *   - verify는 "Bearer " 접두사를 자동 처리 (대소문자 모두)
 *   - 반환된 subject 문자열은 호출자가 g_free()로 해제
 *   - HS256 전용: RS256/ES256 같은 비대칭 알고리즘은 미지원
 *   - 토큰 블랙리스트(로그아웃) 미구현: 만료 시간에만 의존
 */

#ifndef PCV_JWT_H
#define PCV_JWT_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * pcv_jwt_init:
 * @secret: HMAC 서명 키 (최소 32바이트 권장, NULL이면 랜덤 생성)
 *
 * JWT 모듈을 초기화합니다. main() 에서 1회 호출.
 * secret이 NULL 또는 빈 문자열이면 /dev/urandom에서 32바이트 랜덤 키를 생성합니다.
 * 랜덤 키 사용 시 데몬 재시작 시 기존 토큰이 모두 무효화됩니다.
 */
void pcv_jwt_init(const gchar *secret);

/**
 * pcv_jwt_shutdown:
 * JWT 모듈 종료. 시크릿 키를 해제하고 뮤텍스를 정리합니다.
 * main.c 종료 시 호출.
 */
void pcv_jwt_shutdown(void);

/**
 * pcv_jwt_update_secret:
 * @new_secret: 새 HMAC 서명 키 (NULL/빈 문자열이면 무시)
 *
 * SIGHUP 핫 리로드용 — 런타임에 서명 키를 교체합니다.
 * 기존 발급된 토큰은 자동으로 무효화됩니다.
 */
void pcv_jwt_update_secret(const gchar *new_secret);

/**
 * pcv_jwt_sign:
 * @subject:    토큰 subject (보통 사용자 이름, 예: "admin")
 * @expires_in: 유효 시간 (초). 0이면 기본값 3600초(1시간) 적용.
 * @error:      오류 반환 (HMAC 실패 시)
 *
 * Returns: 새 JWT 문자열 "header.payload.signature" (g_free로 해제).
 *          오류 시 NULL + error 설정.
 *
 * [클레임]
 *   sub: subject (사용자명)
 *   iat: issued at (발행 시각, Unix timestamp)
 *   exp: expiry (만료 시각, iat + expires_in)
 */
gchar *pcv_jwt_sign(const gchar *subject,
                    guint        expires_in,
                    GError     **error);

/**
 * pcv_jwt_verify:
 * @token: "Bearer <token>" 또는 순수 토큰 문자열
 * @error: 오류 반환 (서명 불일치, 만료, 파싱 실패)
 *
 * 서명과 만료 시간을 검증합니다.
 * Returns: subject 문자열 (g_free로 해제). 검증 실패 시 NULL + error.
 *
 * [에러 코드]
 *   G_IO_ERROR_INVALID_ARGUMENT  : JWT 형식 오류
 *   G_IO_ERROR_PERMISSION_DENIED : 서명 불일치
 *   G_IO_ERROR_TIMED_OUT         : 토큰 만료
 *   G_IO_ERROR_INVALID_DATA      : sub 클레임 누락
 */
[[nodiscard]] gchar *pcv_jwt_verify(const gchar *token,
                      GError     **error);

/**
 * pcv_jwt_sign_with_ip:
 * @subject:    사용자명
 * @expires_in: 만료 시간 (초), 0이면 기본값
 * @client_ip:  바인딩할 IP (NULL이면 IP 바인딩 생략)
 * @error:      오류 반환
 *
 * IP 바인딩 JWT 토큰을 발행합니다 (BE-A11).
 */
gchar *pcv_jwt_sign_with_ip(const gchar *subject,
                            guint        expires_in,
                            const gchar *client_ip,
                            GError     **error);

/**
 * pcv_jwt_verify_with_ip:
 * @token:      JWT 토큰 (Bearer 접두사 허용)
 * @client_ip:  현재 클라이언트 IP (NULL이면 IP 검증 건너뜀)
 * @error:      오류 반환
 *
 * JWT 검증 + IP 바인딩 확인 (BE-A11). 토큰의 "ip" 클레임과 비교.
 */
gchar *pcv_jwt_verify_with_ip(const gchar *token,
                              const gchar *client_ip,
                              GError     **error);

/**
 * pcv_jwt_blacklist_add:
 * @jti:         취소할 토큰의 jti 클레임 (16바이트 hex 문자열)
 * @expiry_unix: 토큰의 exp 클레임 (이 시각 이후엔 자동 제거)
 *
 * B6-W3 (Phase 3): logout/세션 무효화 시 호출. in-memory blacklist에 추가하여
 * pcv_jwt_verify가 거부하도록 한다. 데몬 재시작 시 blacklist는 비워지지만
 * access token TTL이 짧으므로 영향 제한적.
 */
void pcv_jwt_blacklist_add(const gchar *jti, gint64 expiry_unix);

/**
 * pcv_jwt_blacklist_check:
 * @jti: 검사할 jti
 *
 * Returns: TRUE if revoked (and not yet expired), FALSE otherwise.
 * pcv_jwt_verify 내부에서 호출됨.
 */
gboolean pcv_jwt_blacklist_check(const gchar *jti);

/**
 * pcv_jwt_blacklist_sweep:
 * 만료된 blacklist entry를 일괄 제거. 1시간 주기 g_timeout 권장.
 */
void pcv_jwt_blacklist_sweep(void);

G_END_DECLS

#endif /* PCV_JWT_H */
