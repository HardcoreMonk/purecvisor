













































#include "rest_middleware.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <glib.h>
#include <string.h>


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

















gint
pcv_get_endpoint_rate_limit(const gchar *path, const gchar *http_method)
{
    const gchar *bucket = _endpoint_rate_bucket(path, http_method);


    if (g_strcmp0(bucket, "auth") == 0)
        return 60;

    if (g_strcmp0(bucket, "metrics") == 0 ||
        g_strcmp0(bucket, "health") == 0)
        return 3600;

    if (g_strcmp0(bucket, "vms-post") == 0)
        return 120;

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


constexpr int REST_RPC_TIMEOUT_SEC_MW = 8;













gint
pcv_get_rpc_timeout(const gchar *rpc_method)
{
    if (!rpc_method) return REST_RPC_TIMEOUT_SEC_MW;

    if (g_str_has_prefix(rpc_method, "zfs.") ||
        g_strcmp0(rpc_method, "backup.restore") == 0
#if PCV_CLUSTER_ENABLED
        || g_strcmp0(rpc_method, "vm.migrate") == 0
#endif
    )
        return 60;

    if (g_str_has_prefix(rpc_method, "cloud.") ||
        g_strcmp0(rpc_method, "vm.create") == 0)
        return 30;
#if PCV_CLUSTER_ENABLED
    if (g_str_has_prefix(rpc_method, "cluster."))
        return 30;
#endif

    return REST_RPC_TIMEOUT_SEC_MW;
}




















gchar *
pcv_compute_etag(const gchar *body, gsize len)
{
    GChecksum *cs = g_checksum_new(G_CHECKSUM_MD5);
    g_checksum_update(cs, (const guchar *)body, len);
    const gchar *hex = g_checksum_get_string(cs);

    gchar *etag = g_strdup_printf("\"%.*s\"", 32, hex);
    g_checksum_free(cs);
    return etag;
}























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
