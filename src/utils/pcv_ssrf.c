
#include "pcv_ssrf.h"

#include <gio/gio.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static gboolean
_v4_blocked(guint32 host_order_addr)
{
    if ((host_order_addr & 0xFFFF0000u) == 0xA9FE0000u) return TRUE;
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
            if (IN6_IS_ADDR_LINKLOCAL(a6)) {
                inet_ntop(AF_INET6, a6, blocked_ip, sizeof(blocked_ip));
                blocked = TRUE;
                break;
            }

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
