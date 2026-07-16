/**
 * @file pcv_ssrf.c
 * @brief 아웃바운드 URL 대상 SSRF 검증 헬퍼 구현 (Wave B Item 5-a, A10/V4)
 *
 * 설계 근거: docs/operations/2026-07-16-security-remediation-roadmap.md Item 5.
 * 정책·잔여 위험(DNS-rebind TOCTOU) 설명은 pcv_ssrf.h 참조.
 */

#include "pcv_ssrf.h"

#include <gio/gio.h>   /* G_IO_ERROR quark/코드 */
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/**
 * _v4_blocked — IPv4 주소(host byte order)가 차단 대역인지.
 *   169.254.0.0/16 링크로컬(클라우드 메타데이터 169.254.169.254 포함)만 차단.
 * 루프백(127/8)·RFC1918(10/172.16/192.168)·공인은 허용 — 로컬 AI(OLLAMA
 * 127.0.0.1:11434)·내부망 webhook/S3 등 정당한 내부 대상 지원. 헤드라인 위협인
 * 메타데이터/링크로컬 SSRF는 resolve된 실주소 기준으로 차단(인코딩 우회 무력화).
 * (잔여: admin-설정 endpoint의 루프백 대상 SSRF는 저위험으로 수용.)
 */
static gboolean
_v4_blocked(guint32 host_order_addr)
{
    if ((host_order_addr & 0xFFFF0000u) == 0xA9FE0000u) return TRUE;  /* 169.254/16 링크로컬 */
    return FALSE;
}

gboolean
pcv_url_target_allowed(const gchar *url, GError **error)
{
    if (!url || !*url) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "SSRF guard: empty URL");
        return FALSE;
    }

    /* 1. host 추출 — g_uri_parse는 절대 URI(scheme 필수)를 요구한다.
     *    파싱 실패/host 부재는 fail-closed로 차단한다. */
    GError *perr = NULL;
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, &perr);
    if (!uri) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "SSRF guard: URL 파싱 실패: %s",
                    perr ? perr->message : "unknown");
        if (perr) g_error_free(perr);
        return FALSE;
    }

    const gchar *host = g_uri_get_host(uri);
    if (!host || !*host) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "SSRF guard: URL에 host 없음");
        g_uri_unref(uri);
        return FALSE;
    }
    gchar *host_dup = g_strdup(host);
    g_uri_unref(uri);

    /* 2. 실주소 resolve(IPv4+IPv6). 인코딩 표기는 여기서 실주소로 정규화된다.
     *    resolve 실패 = fail-closed 차단. */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host_dup, NULL, &hints, &res);
    if (rc != 0 || !res) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND,
                    "SSRF guard: host '%s' resolve 실패: %s",
                    host_dup, gai_strerror(rc));
        if (res) freeaddrinfo(res);
        g_free(host_dup);
        return FALSE;
    }

    /* 3. resolve된 각 주소 검사 — 하나라도 차단 대역이면 전체 차단. */
    gboolean blocked = FALSE;
    char blocked_ip[INET6_ADDRSTRLEN] = {0};
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ai->ai_addr;
            guint32 a = ntohl(sin->sin_addr.s_addr);
            if (_v4_blocked(a)) {
                inet_ntop(AF_INET, &sin->sin_addr, blocked_ip, sizeof(blocked_ip));
                blocked = TRUE;
                break;
            }
        } else if (ai->ai_family == AF_INET6) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ai->ai_addr;
            const struct in6_addr *a6 = &s6->sin6_addr;
            if (IN6_IS_ADDR_LINKLOCAL(a6)) {   /* fe80::/10 링크로컬만 차단(루프백 ::1 허용) */
                inet_ntop(AF_INET6, a6, blocked_ip, sizeof(blocked_ip));
                blocked = TRUE;
                break;
            }
            /* ::ffff:a.b.c.d — IPv4-mapped 주소는 내장 IPv4에 v4 규칙 적용
             * (예: ::ffff:127.0.0.1, ::ffff:169.254.169.254 우회 차단) */
            if (IN6_IS_ADDR_V4MAPPED(a6)) {
                guint32 a = ((guint32)a6->s6_addr[12] << 24) |
                            ((guint32)a6->s6_addr[13] << 16) |
                            ((guint32)a6->s6_addr[14] << 8)  |
                            ((guint32)a6->s6_addr[15]);
                if (_v4_blocked(a)) {
                    inet_ntop(AF_INET6, a6, blocked_ip, sizeof(blocked_ip));
                    blocked = TRUE;
                    break;
                }
            }
        }
    }
    freeaddrinfo(res);

    if (blocked) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "SSRF guard: host '%s'가 차단 대역 %s로 resolve됨 "
                    "(루프백/링크로컬)", host_dup, blocked_ip);
        g_free(host_dup);
        return FALSE;
    }

    g_free(host_dup);
    return TRUE;
}
