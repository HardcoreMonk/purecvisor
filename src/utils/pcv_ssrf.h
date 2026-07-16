/**
 * @file pcv_ssrf.h
 * @brief 아웃바운드 URL 대상 SSRF 검증 헬퍼 — 공개 헤더 (Wave B Item 5-a, A10/V4)
 *
 * 데몬이 사용자/설정 제어 URL로 아웃바운드 요청(webhook, AI endpoint, S3 endpoint)을
 * 보내기 **전에** 대상 호스트를 실제 IP로 resolve하여 위험 대역을 차단합니다.
 *
 * [차단 정책 — resolve된 주소 중 하나라도 아래면 차단(FALSE)]
 *   - 루프백: 127.0.0.0/8, ::1
 *   - 링크로컬: 169.254.0.0/16, fe80::/10 (클라우드 메타데이터 169.254.169.254 포함)
 * [허용] RFC1918(10/172.16/192.168)·공인 주소 — 내부망 제품이라 사설망은 정상 대상.
 * [fail-closed] URL 파싱 실패·host 없음·resolve 실패 → 차단(FALSE).
 *
 * [인코딩 우회] host를 getaddrinfo로 실주소 판정하므로 십진/16진/DNS 별칭 표기가
 *   무력화된다(실제 resolve 결과로 판정). 다만 resolve-후-connect 사이의 DNS-rebind
 *   TOCTOU는 본 헬퍼로 막지 못하는 잔여 위험이다(호출 시점 resolve만 검증).
 *
 * [include 경로]
 *   src/utils/ 내부: #include "pcv_ssrf.h"
 *   다른 디렉터리:   #include "utils/pcv_ssrf.h" (또는 "../../utils/pcv_ssrf.h")
 */

#ifndef PURECVISOR_SSRF_H
#define PURECVISOR_SSRF_H

#include <glib.h>

/**
 * pcv_url_target_allowed — 아웃바운드 URL 대상이 허용 대역인지 검증한다.
 *
 * @param url    검사할 절대 URL(scheme 포함, 예: "https://host/path").
 * @param error  차단/실패 사유(선택). 설정 시 호출자가 g_error_free / g_clear_error.
 * @return TRUE = 전송 허용, FALSE = 차단(루프백/링크로컬 또는 파싱·resolve 실패).
 */
gboolean pcv_url_target_allowed(const gchar *url, GError **error);

#endif /* PURECVISOR_SSRF_H */
