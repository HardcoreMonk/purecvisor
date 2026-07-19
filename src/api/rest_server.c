
#include "rest_server.h"
#include "rest_middleware.h"
#include "rest_auth.h"
#include "utils/pcv_crypto.h"
#include "../utils/pcv_jwt.h"
#include "../utils/pcv_log.h"
#include "../modules/daemons/prometheus_exporter.h"
#include "ws_server.h"
#include "../utils/pcv_config.h"
#include "../utils/pcv_tls.h"
#include "../modules/dispatcher/rpc_utils.h"
#if PCV_CLUSTER_ENABLED
#include "../modules/cluster/cluster_manager.h"
#endif

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <zlib.h>
#include "../modules/network/ovn_manager.h"
#include "../modules/network/dpdk_manager.h"
#include "../utils/pcv_spawn.h"
#include "../modules/auth/pcv_rbac.h"
#include "../modules/audit/pcv_audit.h"
#include "../modules/dispatcher/rpc_utils.h"
#include "../modules/virt/circuit_breaker.h"
#include "purecvisor/version.h"
#include <unistd.h>

#define REST_LOG_DOM   "rest_server"

static gboolean g_hsts_enabled = FALSE;

#define REST_API_PREFIX "/api/v1"
constexpr int REST_MAX_BODY = 1 * 1024 * 1024;
constexpr int REST_RPC_TIMEOUT_SEC = 8;
#define PCV_OVN_DEMO_HEALTH_PATH "/var/lib/purecvisor/demo/ovn-ovs-health.json"
#define PCV_OVN_DEMO_HEALTH_STALE_SEC 300

struct _PcvRestServer {
    GObject      parent_instance;
    SoupServer  *soup;
    guint16      port;
    guint16      https_port;
    gboolean     tls_active;

    GThread     *thread;
    GMainLoop   *rest_loop;
    GMainContext *rest_ctx;
};

G_DEFINE_TYPE(PcvRestServer, pcv_rest_server, G_TYPE_OBJECT)

static gchar *
_rpc_over_uds_timeout(const gchar *rpc_json, gint timeout_sec)
{
    const gchar *sock_path = pcv_config_get_socket_path();

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return g_strdup_printf(
            "{\"error\":{\"code\":\"DAEMON_UNAVAILABLE\","
             "\"message\":\"socket() failed: %s\"}}", strerror(errno));
    }

    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, sock_path, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        PCV_LOG_WARN("rest", "connect(%s) failed: %s", sock_path, strerror(errno));
        gchar *msg = g_strdup(
            "{\"error\":{\"code\":\"DAEMON_UNAVAILABLE\","
             "\"message\":\"Daemon temporarily unavailable\"}}");
        close(fd);
        return msg;
    }

    {
        gsize json_len = strlen(rpc_json);
        struct iovec iov[2] = {
            { .iov_base = (void *)rpc_json, .iov_len = json_len },
            { .iov_base = (void *)"\n",     .iov_len = 1        }
        };
        gssize total_len = (gssize)(json_len + 1);
        gssize sent = writev(fd, iov, 2);
        if (sent != total_len) {
            close(fd);
            return g_strdup("{\"error\":{\"code\":\"WRITE_FAILED\","
                             "\"message\":\"UDS write failed\"}}");
        }
    }

    shutdown(fd, SHUT_WR);

    GByteArray *dyn_buf = g_byte_array_new();
    gchar tmp[8192];
    for (;;) {
        gssize n = read(fd, tmp, sizeof(tmp));
        if (n < 0) {

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                g_byte_array_free(dyn_buf, TRUE);
                close(fd);
                return g_strdup_printf("{\"jsonrpc\":\"2.0\",\"error\":{"
                    "\"code\":%d,"
                    "\"message\":\"RPC timeout — daemon did not respond within %ds\"}}",
                    PURE_RPC_ERR_TIMEOUT, timeout_sec);
            }
            break;
        }
        if (n == 0) break;
        g_byte_array_append(dyn_buf, (guint8 *)tmp, (guint)n);
    }
    close(fd);
    gssize total = (gssize)dyn_buf->len;

    if (total <= 0) {
        g_byte_array_free(dyn_buf, TRUE);
        return g_strdup_printf("{\"jsonrpc\":\"2.0\",\"error\":{"
            "\"code\":%d,"
            "\"message\":\"No response from daemon\"}}", PURE_RPC_ERR_ZFS_OPERATION);
    }

    g_byte_array_append(dyn_buf, (guint8 *)"\0", 1);
    if (total > 0 && dyn_buf->data[total - 1] == '\n')
        dyn_buf->data[total - 1] = '\0';

    gchar *line = g_strdup((gchar *)dyn_buf->data);
    g_byte_array_free(dyn_buf, TRUE);

    if (!line) {
        return g_strdup("{\"error\":{\"code\":\"READ_FAILED\","
                         "\"message\":\"No response from daemon\"}}");
    }

    return line;
}

static gchar *
_rpc_over_uds(const gchar *rpc_json)
{
    return _rpc_over_uds_timeout(rpc_json, REST_RPC_TIMEOUT_SEC);
}

#define PCV_CSP_POLICY \
    "default-src 'self'; script-src 'self' 'unsafe-inline'; " \
    "style-src 'self' 'unsafe-inline'; img-src 'self' data:; " \
    "font-src 'self'; " \
    "connect-src 'self' ws: wss:"

static void
_send_json(SoupServerMessage *msg, guint status, const gchar *body)
{
    soup_server_message_set_status(msg, status, NULL);
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(hdrs, "Content-Type",
                                  "application/json; charset=utf-8");
    soup_message_headers_replace(hdrs, "X-Content-Type-Options", "nosniff");

    {
        const gchar *req_method = soup_server_message_get_method(msg);
        gboolean is_get = (g_strcmp0(req_method, "GET") == 0);

        if (is_get && status == 200 && body) {
            gsize body_len = strlen(body);
            gchar *etag = pcv_compute_etag(body, body_len);
            soup_message_headers_replace(hdrs, "ETag", etag);
            soup_message_headers_replace(hdrs, "Cache-Control",
                                          "private, max-age=5, must-revalidate");

            SoupMessageHeaders *req_hdrs =
                soup_server_message_get_request_headers(msg);
            const gchar *if_none_match =
                soup_message_headers_get_one(req_hdrs, "If-None-Match");
            if (if_none_match && g_strcmp0(if_none_match, etag) == 0) {
                soup_server_message_set_status(msg, 304, NULL);
                g_free(etag);
                return;
            }
            g_free(etag);
        } else {
            soup_message_headers_replace(hdrs, "Cache-Control", "no-store");
        }
    }

    soup_message_headers_replace(hdrs, "Connection", "close");

    if (g_hsts_enabled) {
        soup_message_headers_replace(hdrs, "Strict-Transport-Security",
                                      "max-age=31536000; includeSubDomains");
    }
    soup_message_headers_replace(hdrs, "Content-Security-Policy", PCV_CSP_POLICY);
    soup_message_headers_replace(hdrs, "X-Frame-Options", "SAMEORIGIN");
    soup_message_headers_replace(hdrs, "X-XSS-Protection", "1; mode=block");
    soup_message_headers_replace(hdrs, "Referrer-Policy",
                                  "strict-origin-when-cross-origin");
    soup_message_headers_replace(hdrs, "Permissions-Policy",
                                  "camera=(), microphone=(), geolocation=()");

    soup_message_headers_replace(hdrs, "Vary", "Accept-Encoding");

    gsize body_len = strlen(body);

    SoupMessageHeaders *req_hdrs = soup_server_message_get_request_headers(msg);
    const gchar *accept_enc = soup_message_headers_get_one(req_hdrs,
                                                            "Accept-Encoding");
    if (accept_enc && strstr(accept_enc, "gzip") != nullptr && body_len > 256) {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));

        int zinit_ret = deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                                     15 + 16, 8, Z_DEFAULT_STRATEGY);
        if (zinit_ret != Z_OK) {

            g_warning("rest_server: deflateInit2 failed (ret=%d), sending uncompressed",
                      zinit_ret);
        }
        if (zinit_ret == Z_OK) {
            gsize max_compressed = deflateBound(&zs, (uLong)body_len);
            guchar *compressed = g_malloc(max_compressed);
            zs.next_in  = (Bytef *)body;
            zs.avail_in = (uInt)body_len;
            zs.next_out  = compressed;
            zs.avail_out = (uInt)max_compressed;

            int zret = deflate(&zs, Z_FINISH);
            if (zret == Z_STREAM_END) {
                gsize compressed_len = zs.total_out;
                deflateEnd(&zs);

                if (compressed_len < body_len) {
                    soup_message_headers_replace(hdrs, "Content-Encoding",
                                                  "gzip");
                    soup_server_message_set_response(msg, "application/json",
                                                      SOUP_MEMORY_COPY,
                                                      (const gchar *)compressed,
                                                      compressed_len);
                    g_free(compressed);
                    return;
                }
            } else {
                deflateEnd(&zs);
            }
            g_free(compressed);
        }
    }

    soup_server_message_set_response(msg, "application/json",
                                      SOUP_MEMORY_COPY,
                                      body, body_len);
}

static void
_error(SoupServerMessage *msg, guint status,
       const gchar *code, const gchar *detail)
{
    gchar *body = g_strdup_printf(
        "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
        code, detail);
    _send_json(msg, status, body);
    g_free(body);
}

static gchar *
_build_rpc(const gchar *method, JsonObject *params)
{
    JsonBuilder *jb = json_builder_new();
    json_builder_begin_object(jb);

    json_builder_set_member_name(jb, "jsonrpc");
    json_builder_add_string_value(jb, "2.0");
    json_builder_set_member_name(jb, "method");
    json_builder_add_string_value(jb, method);
    json_builder_set_member_name(jb, "params");

    if (params) {

        JsonNode *params_node = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(params_node, params);
        json_builder_add_value(jb, params_node);
    } else {

        json_builder_begin_object(jb);
        json_builder_end_object(jb);
    }

    json_builder_set_member_name(jb, "id");
    json_builder_add_int_value(jb, 1);
    json_builder_end_object(jb);

    JsonNode *root = json_builder_get_root(jb);
    gchar *rpc = json_to_string(root, FALSE);

    json_node_free(root);
    g_object_unref(jb);
    return rpc;
}

static gchar *
_build_rpc_name(const gchar *method, const gchar *name)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", name);
    gchar *rpc = _build_rpc(method, p);
    json_object_unref(p);
    return rpc;
}

static gchar *
_rpc_attach_auth_context(const gchar *rpc_json,
                         const gchar *subject,
                         PcvRole      role)
{

    if (!rpc_json)
        return NULL;

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    /* PCV_PARSE_TRUSTED: 서버 생성 envelope 재파싱(_build_rpc/json_to_string 출력; passthrough도 _parse_body에서 깊이/크기 가드된 노드 재직렬화 — 외부 원문 아님) */
    if (!json_parser_load_from_data(parser, rpc_json, -1, &error)) {
        if (error) g_error_free(error);
        g_object_unref(parser);
        return g_strdup(rpc_json);
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return g_strdup(rpc_json);
    }

    JsonObject *root_obj = json_node_get_object(root);
    JsonObject *params = NULL;
    JsonNode *params_node = json_object_get_member(root_obj, "params");
    if (params_node && JSON_NODE_HOLDS_OBJECT(params_node)) {
        params = json_node_get_object(params_node);
    } else {
        params = json_object_new();
        json_object_set_object_member(root_obj, "params", params);
    }

    json_object_remove_member(params, "_pcv_caller_sub");
    json_object_remove_member(params, "_pcv_caller_role");
    if (subject && *subject)
        json_object_set_string_member(params, "_pcv_caller_sub", subject);
    json_object_set_int_member(params, "_pcv_caller_role", (gint)role);

    gchar *rpc_with_context = json_to_string(root, FALSE);
    g_object_unref(parser);
    return rpc_with_context ? rpc_with_context : g_strdup(rpc_json);
}

static void
_add_pagination_headers(SoupServerMessage *msg, gint total, gint offset,
                        gint limit, const gchar *base_path)
{
    SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);

    gchar *total_str = g_strdup_printf("%d", total);
    soup_message_headers_replace(hdrs, "X-Total-Count", total_str);
    g_free(total_str);

    GString *links = g_string_new("");
    if (offset + limit < total) {
        g_string_append_printf(links, "<%s?offset=%d&limit=%d>; rel=\"next\"",
                               base_path, offset + limit, limit);
    }
    if (offset > 0) {
        if (links->len > 0) g_string_append(links, ", ");
        gint prev_offset = offset - limit;
        if (prev_offset < 0) prev_offset = 0;
        g_string_append_printf(links, "<%s?offset=%d&limit=%d>; rel=\"prev\"",
                               base_path, prev_offset, limit);
    }
    if (links->len > 0)
        soup_message_headers_replace(hdrs, "Link", links->str);
    g_string_free(links, TRUE);
}

static void
_send_rpc_result(SoupServerMessage *msg, const gchar *rpc_resp)
{
    if (!rpc_resp) {
        _error(msg, 500, "NO_RESPONSE", "No response from daemon");
        return;
    }

    JsonParser *p   = json_parser_new();
    GError     *err = nullptr;

    /* PCV_PARSE_TRUSTED: 데몬 JSON-RPC 응답 파싱(내부 신뢰 출력, 외부 입력 아님) */
    if (!json_parser_load_from_data(p, rpc_resp, -1, &err)) {
        _error(msg, 500, "PARSE_ERROR",
               err ? err->message : "Invalid JSON from daemon");
        if (err) g_error_free(err);
        g_object_unref(p);
        return;
    }

    JsonObject    *obj = json_node_get_object(json_parser_get_root(p));
    gchar         *body_str;
    guint          status;

    if (json_object_has_member(obj, "result")) {

        JsonNode *result = json_object_get_member(obj, "result");
        gchar *r = json_to_string(result, FALSE);
        body_str = g_strdup_printf("{\"data\":%s}", r);
        g_free(r);
        status = 200;

        {
            GUri *uri = soup_server_message_get_uri(msg);
            const gchar *req_path = uri ? g_uri_get_path(uri) : NULL;
            const gchar *req_query = uri ? g_uri_get_query(uri) : NULL;
            if (req_path && JSON_NODE_HOLDS_ARRAY(result)) {
                gint total = (gint)json_array_get_length(json_node_get_array(result));
                gint offset = 0, limit = total;
                if (req_query) {

                    gchar **pairs = g_strsplit(req_query, "&", -1);
                    for (gchar **p = pairs; *p; p++) {
                        if (g_str_has_prefix(*p, "offset="))
                            offset = (gint)g_ascii_strtoll(*p + 7, NULL, 10);
                        else if (g_str_has_prefix(*p, "limit="))
                            limit = (gint)g_ascii_strtoll(*p + 6, NULL, 10);
                    }
                    g_strfreev(pairs);
                }
                if (limit > 0)
                    _add_pagination_headers(msg, total, offset, limit, req_path);
            }
        }
    } else if (json_object_has_member(obj, "error")) {

        JsonObject *e    = json_object_get_object_member(obj, "error");
        gint64      code = json_object_get_int_member_with_default(e, "code", PURE_RPC_ERR_ZFS_OPERATION);
        JsonNode   *enode = json_object_get_member(obj, "error");
        gchar *r = json_to_string(enode, FALSE);
        body_str = g_strdup_printf("{\"error\":%s}", r);
        g_free(r);

        if      (code == PURE_RPC_ERR_INVALID_PARAMS)   status = 400;
        else if (code == PURE_RPC_ERR_METHOD_NOT_FOUND) status = 404;
        else if (code == PURE_RPC_ERR_VM_NOT_FOUND)     status = 404;
        else if (code == PURE_RPC_ERR_TIMEOUT)          status = 504;
        else if (code == PURE_RPC_ERR_FORBIDDEN)        status = 403;
        else                                            status = 500;
    } else {
        body_str = g_strdup(rpc_resp);
        status   = 200;
    }

    _send_json(msg, status, body_str);
    g_free(body_str);
    g_object_unref(p);
}

static gchar *
_authenticate(SoupServerMessage *msg, gint *out_key_role)
{
    if (out_key_role) *out_key_role = -1;

    SoupMessageHeaders *req =
        soup_server_message_get_request_headers(msg);

    const gchar *api_key = soup_message_headers_get_one(req, "X-API-Key");
    if (api_key && *api_key) {
        GError *aerr = nullptr;
        /* PCV_SAFETY_CONTROL: apikey-role-enforce — 키의 실효 role은 저장 role 컬럼에서만
         *   파생(client_name 라이브 role 무시). verify가 저장 role을 out-param으로 돌려주고,
         *   호출자(_authenticate)가 out_key_role로 상위 디스패치에 전달한다 (SEC-3 privesc 차단). */
        PcvRole stored_role = PCV_ROLE_VIEWER;
        gchar *user = pcv_rbac_verify_api_key(api_key, &stored_role, &aerr);
        if (user) {
            if (out_key_role) *out_key_role = (gint)stored_role;
            return user;
        }

        GSocketAddress *ra = soup_server_message_get_remote_address(msg);
        gchar *rip = (ra && G_IS_INET_SOCKET_ADDRESS(ra))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)))
            : g_strdup("unknown");
        PCV_LOG_WARN(REST_LOG_DOM, "AUDIT: auth failed (invalid API key) from %s", rip);
        pcv_audit_log("-", "auth.failed",
                      aerr ? aerr->message : "invalid API key", "fail", 401, 0, rip);
        g_free(rip);
        _error(msg, 401, "UNAUTHORIZED",
               aerr ? aerr->message : "Invalid API key");
        if (aerr) g_error_free(aerr);
        return NULL;
    }

    const gchar *auth = soup_message_headers_get_one(req, "Authorization");

    if (!auth || !*auth) {
        GSocketAddress *ra = soup_server_message_get_remote_address(msg);
        gchar *rip = (ra && G_IS_INET_SOCKET_ADDRESS(ra))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)))
            : g_strdup("unknown");
        PCV_LOG_WARN(REST_LOG_DOM, "AUDIT: auth failed (no header) from %s", rip);
        pcv_audit_log("-", "auth.failed", "no Authorization header", "fail", 401, 0, rip);
        g_free(rip);
        _error(msg, 401, "UNAUTHORIZED", "Authorization header required");
        return NULL;
    }

    GError *err     = nullptr;
    gchar  *subject = pcv_jwt_verify(auth, &err);
    if (!subject) {
        GSocketAddress *ra = soup_server_message_get_remote_address(msg);
        gchar *rip = (ra && G_IS_INET_SOCKET_ADDRESS(ra))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)))
            : g_strdup("unknown");
        PCV_LOG_WARN(REST_LOG_DOM, "AUDIT: auth failed (invalid token) from %s", rip);
        pcv_audit_log("-", "auth.failed",
                      err ? err->message : "invalid token", "fail", 401, 0, rip);
        g_free(rip);
        _error(msg, 401, "UNAUTHORIZED",
               err ? err->message : "Invalid token");
        if (err) g_error_free(err);
    }
    return subject;
}

static JsonObject *
_parse_body(SoupServerMessage *msg)
{

    SoupMessageBody *body = soup_server_message_get_request_body(msg);
    if (!body || body->length == 0)
        return json_object_new();

    const gchar *data = body->data;
    gsize        len  = (gsize)body->length;
    if (len > REST_MAX_BODY) return NULL;

    JsonParser *p = nullptr;
    GError     *e = nullptr;
    if (!pcv_rpc_parse_guarded(data, (gssize)len, &p, &e)) {
        if (e) g_error_free(e);
        return NULL;
    }

    JsonNode   *root = json_parser_get_root(p);
    JsonObject *obj  = JSON_NODE_HOLDS_OBJECT(root)
                       ? json_object_ref(json_node_get_object(root))
                       : json_object_new();
    g_object_unref(p);
    return obj;
}

static gchar **
_split_path(const gchar *path)
{
    const gchar *p = path;
    if (g_str_has_prefix(p, REST_API_PREFIX))
        p += strlen(REST_API_PREFIX);
    if (*p == '/') p++;
    return g_strsplit(p, "/", -1);
}

static void
_on_msg_finished(SoupServerMessage *msg, gpointer user_data __attribute__((unused)))
{
    GSocket *gsock = soup_server_message_get_socket(msg);
    if (gsock) {
        int fd = g_socket_get_fd(gsock);
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
    }
}

static GMainContext *self_rest_ctx = nullptr;

typedef struct {
    SoupServerMessage *msg;
    gchar             *rpc;
    gchar             *rpc_method;
    gchar             *vm_delete_name;
    gboolean           is_vm_delete;
    GMainContext      *rest_ctx;
} _RestAsyncCtx;

static void
_rest_async_ctx_free(gpointer p)
{
    _RestAsyncCtx *ctx = p;
    if (!ctx) return;
    g_free(ctx->rpc);
    g_free(ctx->rpc_method);
    g_free(ctx->vm_delete_name);
    if (ctx->msg) g_object_unref(ctx->msg);
    g_free(ctx);
}

typedef struct {
    SoupServerMessage *msg;
    gchar *resp;
} _RestUnpauseData;

static gboolean
_rest_unpause_cb(gpointer data)
{
    _RestUnpauseData *ud = data;
    _send_rpc_result(ud->msg, ud->resp);
    soup_server_message_unpause(ud->msg);
    g_object_unref(ud->msg);
    g_free(ud->resp);
    g_free(ud);
    return G_SOURCE_REMOVE;
}

static void
_rest_async_worker(GTask *task, gpointer src __attribute__((unused)),
                   gpointer task_data, GCancellable *cancel __attribute__((unused)))
{
    _RestAsyncCtx *actx = task_data;

    gint timeout = pcv_get_rpc_timeout(actx->rpc_method);
    gchar *resp = _rpc_over_uds_timeout(actx->rpc, timeout);

    _RestUnpauseData *ud = g_new0(_RestUnpauseData, 1);
    ud->msg = actx->msg;
    ud->resp = resp;

    GSource *src2 = g_idle_source_new();
    g_source_set_callback(src2, _rest_unpause_cb, ud, NULL);
    g_source_attach(src2, actx->rest_ctx);
    g_source_unref(src2);

    g_free(actx->rpc);           actx->rpc = NULL;
    g_free(actx->rpc_method);    actx->rpc_method = NULL;
    g_free(actx->vm_delete_name);actx->vm_delete_name = NULL;
    actx->msg = NULL;
}

static gchar *
_json_string_member_dup(JsonObject *obj, const gchar *member)
{
    if (!obj || !json_object_has_member(obj, member))
        return NULL;

    JsonNode *node = json_object_get_member(obj, member);
    if (!node || !JSON_NODE_HOLDS_VALUE(node))
        return NULL;

    if (json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;

    return g_strdup(json_node_get_string(node));
}

static gboolean
_iso8601_is_stale(const gchar *checked_at, gint64 now_us)
{
    if (!checked_at || !*checked_at)
        return TRUE;

    GDateTime *dt = g_date_time_new_from_iso8601(checked_at, NULL);
    if (!dt)
        return TRUE;

    gint64 checked_us = g_date_time_to_unix(dt) * G_USEC_PER_SEC;
    gint64 age_us = now_us - checked_us;
    g_date_time_unref(dt);

    return age_us < 0 ||
           age_us > ((gint64)PCV_OVN_DEMO_HEALTH_STALE_SEC * G_USEC_PER_SEC);
}

static void
_send_ovn_demo_health(SoupServerMessage *msg)
{
    gchar *content = NULL;
    gsize len = 0;
    GError *err = NULL;

    if (!g_file_get_contents(PCV_OVN_DEMO_HEALTH_PATH, &content, &len, &err)) {
        gchar *safe = err ? g_strescape(err->message, NULL)
                          : g_strdup("state file unavailable");
        gchar *body = g_strdup_printf(
            "{\"data\":{\"demo\":\"ovn-ovs\",\"status\":\"stale\",\"stale\":true,"
            "\"reason\":\"state_file_unavailable\",\"detail\":\"%s\","
            "\"path\":\"%s\",\"stale_after_sec\":%d}}",
            safe, PCV_OVN_DEMO_HEALTH_PATH, PCV_OVN_DEMO_HEALTH_STALE_SEC);
        _send_json(msg, 200, body);
        g_free(body);
        g_free(safe);
        g_clear_error(&err);
        return;
    }

    JsonParser *parser = json_parser_new();
    /* PCV_PARSE_TRUSTED: 호스트 로컬 상태파일(PCV_OVN_DEMO_HEALTH_PATH) 파싱 — 네트워크 경계 외부 입력 아님(설계 §11 스코프 밖) */
    if (!json_parser_load_from_data(parser, content, (gssize)len, &err)) {
        gchar *safe = err ? g_strescape(err->message, NULL)
                          : g_strdup("invalid json");
        gchar *body = g_strdup_printf(
            "{\"data\":{\"demo\":\"ovn-ovs\",\"status\":\"stale\",\"stale\":true,"
            "\"reason\":\"invalid_state_json\",\"detail\":\"%s\","
            "\"path\":\"%s\",\"stale_after_sec\":%d}}",
            safe, PCV_OVN_DEMO_HEALTH_PATH, PCV_OVN_DEMO_HEALTH_STALE_SEC);
        _send_json(msg, 200, body);
        g_free(body);
        g_free(safe);
        g_clear_error(&err);
        g_object_unref(parser);
        g_free(content);
        return;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = root && JSON_NODE_HOLDS_OBJECT(root)
                    ? json_node_get_object(root)
                    : NULL;
    if (!obj) {
        gchar *body = g_strdup_printf(
            "{\"data\":{\"demo\":\"ovn-ovs\",\"status\":\"stale\",\"stale\":true,"
            "\"reason\":\"state_root_not_object\",\"path\":\"%s\","
            "\"stale_after_sec\":%d}}",
            PCV_OVN_DEMO_HEALTH_PATH, PCV_OVN_DEMO_HEALTH_STALE_SEC);
        _send_json(msg, 200, body);
        g_free(body);
        g_object_unref(parser);
        g_free(content);
        return;
    }

    gchar *checked_at = _json_string_member_dup(obj, "checked_at");
    gboolean stale = _iso8601_is_stale(checked_at, g_get_real_time());
    json_object_set_boolean_member(obj, "stale", stale);
    json_object_set_int_member(obj, "stale_after_sec",
                               PCV_OVN_DEMO_HEALTH_STALE_SEC);
    if (stale) {
        json_object_set_string_member(obj, "status", "stale");
        if (!json_object_has_member(obj, "reason"))
            json_object_set_string_member(obj, "reason", "state_file_stale");
    }

    JsonNode *data_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(data_node, obj);
    JsonObject *envelope = json_object_new();
    json_object_set_member(envelope, "data", data_node);

    JsonNode *envelope_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(envelope_node, envelope);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, envelope_node);
    gchar *body = json_generator_to_data(gen, NULL);

    _send_json(msg, 200, body);

    g_free(body);
    g_object_unref(gen);
    json_node_free(envelope_node);
    g_free(checked_at);
    g_object_unref(parser);
    g_free(content);
}

static gboolean
_cors_origin_allowed(const gchar *origin, const gchar *host_hdr)
{
    if (!origin || origin[0] == '\0')
        return FALSE;
    const gchar *sep = strstr(origin, "://");
    if (!sep)
        return FALSE;
    const gchar *oh = sep + 3;
    gsize oh_len = 0;
    while (oh[oh_len] && oh[oh_len] != '/' && oh[oh_len] != ':')
        oh_len++;
    if (oh_len == 0)
        return FALSE;

    gchar *origin_host = g_strndup(oh, oh_len);
    gboolean allow = (g_strcmp0(origin_host, "localhost") == 0 ||
                      g_strcmp0(origin_host, "127.0.0.1") == 0 ||
                      g_strcmp0(origin_host, "::1") == 0);
    if (!allow && host_hdr && host_hdr[0]) {
        gsize hh_len = 0;
        while (host_hdr[hh_len] && host_hdr[hh_len] != ':')
            hh_len++;
        gchar *host_host = g_strndup(host_hdr, hh_len);
        allow = (hh_len > 0 && g_strcmp0(origin_host, host_host) == 0);
        g_free(host_host);
    }
    g_free(origin_host);
    return allow;
}

static gboolean
_body_has_secret(const gchar *body, gsize len)
{
    if (!body || len == 0)
        return FALSE;
    static const gchar *const secret_keys[] = {
        "password", "passwd", "secret", "api_key", "apikey", "token",
        "chap_password", "jwt_secret", "private_key", "credential", nullptr
    };
    gsize scan_len = MIN(len, (gsize)4096);
    gchar *lower = g_ascii_strdown(body, (gssize)scan_len);
    gboolean found = FALSE;
    for (gsize i = 0; secret_keys[i]; i++) {
        if (strstr(lower, secret_keys[i])) { found = TRUE; break; }
    }
    g_free(lower);
    return found;
}

static void
_on_request(SoupServer        *server   __attribute__((unused)),
            SoupServerMessage *msg,
            const gchar       *path,
            GHashTable        *query,
            gpointer           user_data __attribute__((unused)))
{
    const gchar *method = soup_server_message_get_method(msg);
    gchar *req_id = nullptr;
    PcvTraceContext *trace_ctx = nullptr;

    soup_message_headers_replace(
        soup_server_message_get_response_headers(msg), "Connection", "close");

    g_signal_connect(msg, "finished", G_CALLBACK(_on_msg_finished), NULL);

    {
        static GMutex  rate_mu;
        static GHashTable *rate_counts = nullptr;
        static gint64  window_start = 0;
        g_mutex_lock(&rate_mu);
        if (!rate_counts)
            rate_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
        if (now - window_start > 60) {
            g_hash_table_remove_all(rate_counts);
            window_start = now;
        }

        GSocketAddress *rate_remote = soup_server_message_get_remote_address(msg);
        gchar *rate_ip = nullptr;
        if (rate_remote && G_IS_INET_SOCKET_ADDRESS(rate_remote)) {
            GInetAddress *inet = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(rate_remote));
            rate_ip = g_inet_address_to_string(inet);
        }
        if (!rate_ip) rate_ip = g_strdup("unknown");
        gchar *rate_key = pcv_build_rate_limit_key(rate_ip, path, method);
        gint count = GPOINTER_TO_INT(g_hash_table_lookup(rate_counts, rate_key)) + 1;
        g_hash_table_insert(rate_counts, g_strdup(rate_key), GINT_TO_POINTER(count));

        if (g_hash_table_size(rate_counts) > 1024) {
            GHashTableIter evict_iter;
            gpointer evict_key, evict_val;
            gint removed = 0;
            gint target = (gint)g_hash_table_size(rate_counts) / 2;
            g_hash_table_iter_init(&evict_iter, rate_counts);
            while (g_hash_table_iter_next(&evict_iter, &evict_key, &evict_val) && removed < target) {
                g_hash_table_iter_remove(&evict_iter);
                removed++;
            }
            PCV_LOG_WARN(REST_LOG_DOM,
                "Rate limit table overflow: evicted %d/%d bucket entries (LRU approximate)",
                removed, removed + (gint)g_hash_table_size(rate_counts));

            count = GPOINTER_TO_INT(g_hash_table_lookup(rate_counts, rate_key)) + 1;
            g_hash_table_insert(rate_counts, g_strdup(rate_key), GINT_TO_POINTER(count));
        }
        g_mutex_unlock(&rate_mu);

        gint endpoint_limit = pcv_get_endpoint_rate_limit(path, method);
        if (count > endpoint_limit) {
            PCV_LOG_WARN(REST_LOG_DOM,
                "Rate limit exceeded for %s on %s (%d req/min, limit=%d, key=%s)",
                rate_ip, path, count, endpoint_limit, rate_key);
            g_free(rate_key);
            g_free(rate_ip);
            _error(msg, 429, "RATE_LIMIT", "Too many requests");
            return;
        }
        g_free(rate_key);
        g_free(rate_ip);
    }

    {
        static GMutex      _user_rl_mu;
        static GHashTable *_user_rl_map   = nullptr;
        static gint64      _user_rl_start = 0;
#define USER_RATE_LIMIT  1200

        SoupMessageHeaders *rl_reqh = soup_server_message_get_request_headers(msg);
        const gchar *rl_auth = soup_message_headers_get_one(rl_reqh, "Authorization");
        gchar *rl_user = nullptr;
        if (rl_auth && g_str_has_prefix(rl_auth, "Bearer ")) {
            GError *rl_err = nullptr;
            rl_user = pcv_jwt_verify(rl_auth, &rl_err);
            if (rl_err) g_error_free(rl_err);
        }

        if (rl_user && *rl_user) {
            g_mutex_lock(&_user_rl_mu);
            if (!_user_rl_map)
                _user_rl_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, NULL);
            gint64 now_u = g_get_monotonic_time() / G_USEC_PER_SEC;
            if (now_u - _user_rl_start > 60) {
                g_hash_table_remove_all(_user_rl_map);
                _user_rl_start = now_u;
            }
            gint ucount = GPOINTER_TO_INT(
                g_hash_table_lookup(_user_rl_map, rl_user)) + 1;
            g_hash_table_insert(_user_rl_map, g_strdup(rl_user),
                                GINT_TO_POINTER(ucount));

            if (g_hash_table_size(_user_rl_map) > 512) {
                g_hash_table_remove_all(_user_rl_map);
                ucount = 1;
            }
            g_mutex_unlock(&_user_rl_mu);
            if (ucount > USER_RATE_LIMIT) {
                PCV_LOG_WARN(REST_LOG_DOM,
                    "Per-user rate limit exceeded for '%s' (%d req/min)",
                    rl_user, ucount);
                g_free(rl_user);
                _error(msg, 429, "RATE_LIMIT",
                    "Per-user rate limit exceeded");
                return;
            }
        }
        g_free(rl_user);
#undef USER_RATE_LIMIT
    }

    if (g_strcmp0(method, "POST") == 0 || g_strcmp0(method, "PUT") == 0 ||
        g_strcmp0(method, "DELETE") == 0) {
        GSocketAddress *remote = soup_server_message_get_remote_address(msg);
        gchar *remote_str = (remote && G_IS_INET_SOCKET_ADDRESS(remote))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(remote)))
            : g_strdup("unknown");

        SoupMessageBody *audit_body = soup_server_message_get_request_body(msg);
        const gchar *body_str = audit_body ? audit_body->data : NULL;
        gsize body_len = audit_body ? (gsize)audit_body->length : 0;
        gboolean is_auth = (g_strstr_len(path, -1, "/auth/token") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/refresh") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/logout") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/register") != nullptr) ||
                            (g_strstr_len(path, -1, "/auth/password") != nullptr) ||

                            _body_has_secret(body_str, body_len);
        if (body_str && body_len > 0 && !is_auth) {
            gchar *truncated = g_strndup(body_str, MIN(body_len, 200));
            PCV_LOG_INFO(REST_LOG_DOM, "AUDIT: %s %s from %s body=%.200s",
                         method, path, remote_str, truncated);
            g_free(truncated);
        } else {
            PCV_LOG_INFO(REST_LOG_DOM, "AUDIT: %s %s from %s%s",
                         method, path, remote_str,
                         is_auth ? " body=(credentials masked)" : "");
        }
        g_free(remote_str);
    }

    req_id = pcv_generate_request_id();
    {
        SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(rh, "X-Request-ID", req_id);
    }
    pcv_log_req_id_set(req_id);

    {
        SoupMessageHeaders *reqh_tp = soup_server_message_get_request_headers(msg);
        const gchar *incoming_tp = soup_message_headers_get_one(reqh_tp, "traceparent");
        trace_ctx = incoming_tp ? pcv_trace_context_parse(incoming_tp)
                                : NULL;
        if (!trace_ctx)
            trace_ctx = pcv_trace_context_new();
        gchar *tp_out = pcv_trace_context_format(trace_ctx);
        SoupMessageHeaders *rh_tp = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(rh_tp, "traceparent", tp_out);
        g_free(tp_out);
    }

    PCV_LOG_INFO(REST_LOG_DOM, "[%s] [trace=%s span=%s] %s %s",
                 req_id, trace_ctx->trace_id, trace_ctx->span_id, method, path);

    {
        SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
        SoupMessageHeaders *reqh = soup_server_message_get_request_headers(msg);
        const gchar *origin = soup_message_headers_get_one(reqh, "Origin");
        const gchar *host_hdr = soup_message_headers_get_one(reqh, "Host");

        if (origin && origin[0] != '\0' && _cors_origin_allowed(origin, host_hdr)) {
            soup_message_headers_replace(rh, "Access-Control-Allow-Origin", origin);
            soup_message_headers_replace(rh, "Access-Control-Allow-Methods",
                                          "GET, POST, PUT, DELETE, OPTIONS");
            soup_message_headers_replace(rh, "Access-Control-Allow-Headers",
                                          "Authorization, Content-Type, X-API-Key");
            soup_message_headers_replace(rh, "Access-Control-Allow-Credentials", "true");

            soup_message_headers_replace(rh, "Vary", "Origin");
        }
    }
    if (g_strcmp0(method, "OPTIONS") == 0) {
        soup_server_message_set_status(msg, 204, NULL);
        goto cleanup;
    }

    if (g_strcmp0(path, "/") == 0) {
        soup_server_message_set_redirect(msg, 302, "/ui/");
        return;
    }

    if (g_str_has_prefix(path, "/ui")) {
        const gchar *file_path = path + 3;
        if (!file_path[0] || g_strcmp0(file_path, "/") == 0)
            file_path = "/index.html";

        if (strstr(file_path, "..")) {
            _error(msg, 403, "FORBIDDEN", "Path traversal blocked");
            return;
        }

        static const char *ui_base_dir = "/usr/local/share/purecvisor/ui";

        static char ui_base_resolved[PATH_MAX] = {0};
        static size_t ui_base_len = 0;
        if (ui_base_resolved[0] == '\0') {
            if (!realpath(ui_base_dir, ui_base_resolved)) {
                _error(msg, 500, "SERVER_ERROR", "UI base dir missing");
                return;
            }
            ui_base_len = strlen(ui_base_resolved);
        }

        gchar *full = g_strdup_printf("%s%s", ui_base_dir, file_path);

        char resolved[PATH_MAX];
        if (!realpath(full, resolved)) {
            int e = errno;
            _error(msg, e == ENOENT ? 404 : 403,
                   e == ENOENT ? "NOT_FOUND" : "FORBIDDEN",
                   e == ENOENT ? "UI file not found" : "Path traversal blocked");
            g_free(full);
            return;
        }

        if (strncmp(resolved, ui_base_resolved, ui_base_len) != 0 ||
            (resolved[ui_base_len] != '/' && resolved[ui_base_len] != '\0')) {
            _error(msg, 403, "FORBIDDEN", "Path traversal blocked");
            g_free(full);
            return;
        }

        gchar *content = nullptr;
        gsize len = 0;

        if (g_file_get_contents(resolved, &content, &len, NULL)) {
            const gchar *ct = "text/html; charset=utf-8";
            if (g_str_has_suffix(file_path, ".js"))   ct = "application/javascript";
            if (g_str_has_suffix(file_path, ".css"))  ct = "text/css";
            if (g_str_has_suffix(file_path, ".json")) ct = "application/json";
            if (g_str_has_suffix(file_path, ".md"))   ct = "text/markdown; charset=utf-8";
            if (g_str_has_suffix(file_path, ".png"))  ct = "image/png";
            if (g_str_has_suffix(file_path, ".ico"))  ct = "image/x-icon";
            if (g_str_has_suffix(file_path, ".svg"))  ct = "image/svg+xml";
            if (g_str_has_suffix(file_path, ".webp")) ct = "image/webp";
            if (g_str_has_suffix(file_path, ".woff")) ct = "font/woff";
            if (g_str_has_suffix(file_path, ".woff2")) ct = "font/woff2";

            soup_server_message_set_status(msg, 200, NULL);
            SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
            soup_message_headers_replace(rh, "Cache-Control", "no-cache, must-revalidate");

            soup_message_headers_replace(rh, "Content-Security-Policy", PCV_CSP_POLICY);
            soup_message_headers_replace(rh, "X-Frame-Options", "SAMEORIGIN");
            soup_message_headers_replace(rh, "Referrer-Policy",
                                          "strict-origin-when-cross-origin");
            soup_message_headers_replace(rh, "X-Content-Type-Options", "nosniff");
            soup_server_message_set_response(msg, ct, SOUP_MEMORY_TAKE, content, len);
        } else {
            _error(msg, 404, "NOT_FOUND", "UI file not found");
        }
        g_free(full);
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/health/recent-errors") == 0) {
        const gchar *vm_q = NULL;
        gint lim = 5;
        if (query) {
            vm_q = g_hash_table_lookup(query, "vm");
            const gchar *lim_q = g_hash_table_lookup(query, "limit");
            if (lim_q) {
                gint v = atoi(lim_q);
                if (v > 0 && v <= 50) lim = v;
            }
        }
        JsonArray *failures = pcv_audit_recent_failures(vm_q, lim);
        JsonObject *root = json_object_new();
        JsonNode *fnode = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(fnode, failures);
        json_object_set_member(root, "data", fnode);
        JsonNode *rn = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(rn, root);
        JsonGenerator *gen = json_generator_new();
        json_generator_set_root(gen, rn);
        gchar *body = json_generator_to_data(gen, NULL);
        _send_json(msg, 200, body);
        g_free(body);
        g_object_unref(gen);
        json_node_free(rn);
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/health") == 0) {

        static gint64 cache_ts = 0;
        static gchar *cache_body = nullptr;
        static gint   cache_code = 200;
        static GMutex health_mu;
        static gboolean mu_init = FALSE;
        if (!mu_init) { g_mutex_init(&health_mu); mu_init = TRUE; }

        gint64 now = g_get_monotonic_time();
        g_mutex_lock(&health_mu);
        if (cache_body && (now - cache_ts) < 5 * G_USEC_PER_SEC) {
            _send_json(msg, cache_code, cache_body);
            g_mutex_unlock(&health_mu);
            return;
        }
        g_mutex_unlock(&health_mu);

        JsonObject *root = json_object_new();
        JsonObject *checks = json_object_new();
        gboolean critical = FALSE;
        gboolean degraded = FALSE;

        {
            JsonObject *c = json_object_new();
            gint64 t0 = g_get_monotonic_time();
            virConnectPtr conn = virConnectOpenReadOnly("qemu:///system");
            gint64 dt = (g_get_monotonic_time() - t0) / 1000;
            if (conn) {
                json_object_set_boolean_member(c, "ok", TRUE);
                json_object_set_int_member(c, "latency_ms", dt);
                const gchar *sla = (dt < 200) ? "ok" : (dt < 500) ? "warn" : "critical";
                json_object_set_string_member(c, "sla", sla);
                virConnectClose(conn);
            } else {
                json_object_set_boolean_member(c, "ok", FALSE);
                json_object_set_string_member(c, "error", "connection failed");
                critical = TRUE;
            }
            json_object_set_object_member(checks, "libvirt", c);
        }

        {
            JsonObject *c = json_object_new();
            json_object_set_boolean_member(c, "ok", TRUE);
            json_object_set_string_member(c, "note", "single_edge build: distributed metadata store not used");
            json_object_set_object_member(checks, "etcd", c);
        }

        {
            JsonObject *c = json_object_new();
            const gchar *db_path = pcv_config_get_string("daemon", "db_path",
                                       "/var/lib/purecvisor/vm_state.db");
            gchar *dir = g_path_get_dirname(db_path);
            struct statvfs sv;
            if (statvfs(dir, &sv) == 0) {
                gdouble avail_gb = (gdouble)sv.f_bavail * sv.f_frsize / (1024.0*1024*1024);
                json_object_set_boolean_member(c, "ok", avail_gb > 0.1);
                json_object_set_double_member(c, "avail_gb",
                    (double)((gint)(avail_gb * 100)) / 100.0);
                if (avail_gb <= 0.1) critical = TRUE;
            } else {
                json_object_set_boolean_member(c, "ok", FALSE);
                json_object_set_string_member(c, "error", "statvfs failed");
                critical = TRUE;
            }
            g_free(dir);
            json_object_set_object_member(checks, "disk", c);
        }

        {
            JsonObject *c = json_object_new();
            const gchar *audit_path = "/var/lib/purecvisor/pcv_audit.db";
            struct stat st;
            if (stat(audit_path, &st) == 0) {
                json_object_set_boolean_member(c, "ok", TRUE);
                json_object_set_double_member(c, "size_mb",
                    (double)((gint)(st.st_size / (1024.0*1024) * 10)) / 10.0);
            } else {
                json_object_set_boolean_member(c, "ok", TRUE);
                json_object_set_string_member(c, "note", "no audit db yet");
            }
            json_object_set_object_member(checks, "audit_db", c);
        }

        {
            JsonObject *c = json_object_new();
            json_object_set_boolean_member(c, "enabled", pcv_tls_is_enabled());
            json_object_set_object_member(checks, "tls", c);
        }

        {
            JsonObject *c = json_object_new();
            const gchar *vm_db = pcv_config_get_string("daemon", "db_path",
                                      "/var/lib/purecvisor/vm_state.db");
            struct stat st_vm;
            if (stat(vm_db, &st_vm) == 0) {
                json_object_set_boolean_member(c, "ok", TRUE);
                json_object_set_double_member(c, "size_mb",
                    (double)((gint)(st_vm.st_size / (1024.0*1024) * 10)) / 10.0);
            } else {
                json_object_set_boolean_member(c, "ok", TRUE);
                json_object_set_string_member(c, "note", "no vm_state db yet");
            }
            json_object_set_object_member(checks, "vm_state_db", c);
        }

        {
            JsonObject *c = json_object_new();
            json_object_set_string_member(c, "mode", "single_edge");
            json_object_set_object_member(checks, "cluster", c);
        }

        {
            JsonObject *cap = json_object_new();
            json_object_set_boolean_member(cap, "ovn", pcv_ovn_is_available());
            json_object_set_boolean_member(cap, "dpdk", pcv_dpdk_is_available());
            json_object_set_boolean_member(cap, "cluster", FALSE);
            json_object_set_object_member(root, "capabilities", cap);
        }

        const gchar *status_str = critical ? "critical" : degraded ? "degraded" : "ok";
        gint http_code = critical ? 503 : 200;

        json_object_set_string_member(root, "status", status_str);
        json_object_set_string_member(root, "service", "purecvisorsd");
        json_object_set_string_member(root, "version", PCV_PRODUCT_VERSION);
        json_object_set_string_member(root, "node_name", g_get_host_name());
        json_object_set_int_member(root, "uptime_sec",
            (gint64)(g_get_monotonic_time() / G_USEC_PER_SEC));
        json_object_set_object_member(root, "checks", checks);

        JsonNode *jn = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(jn, root);
        gchar *body = json_to_string(jn, FALSE);
        json_node_free(jn);

        g_mutex_lock(&health_mu);
        g_free(cache_body);
        cache_body = g_strdup(body);
        cache_code = http_code;
        cache_ts = g_get_monotonic_time();
        g_mutex_unlock(&health_mu);

        _send_json(msg, http_code, body);
        g_free(body);
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/version") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        gchar *rpc = _build_rpc("daemon.version", NULL);
        gchar *resp = _rpc_over_uds(rpc);
        g_free(rpc);
        if (resp) {
            JsonParser *jp = json_parser_new();
            /* PCV_PARSE_TRUSTED: 데몬 JSON-RPC 응답 파싱(내부 신뢰 출력, 외부 입력 아님) */
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *root = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(root, "result")) {
                    JsonNode *rn = json_object_dup_member(root, "result");
                    gchar *body = json_to_string(rn, FALSE);
                    json_node_unref(rn);
                    _send_json(msg, 200, body);
                    g_free(body);
                } else {
                    _send_json(msg, 200, resp);
                }
            } else {
                _send_json(msg, 200, resp);
            }
            g_object_unref(jp);
            g_free(resp);
        } else {
            _error(msg, 500, "INTERNAL_ERROR", "Version RPC failed");
        }
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/update-check") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        gchar *rpc = _build_rpc("daemon.update_check", NULL);
        gchar *resp = _rpc_over_uds(rpc);
        g_free(rpc);
        if (resp) {
            JsonParser *jp = json_parser_new();
            /* PCV_PARSE_TRUSTED: 데몬 JSON-RPC 응답 파싱(내부 신뢰 출력, 외부 입력 아님) */
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *root = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(root, "result")) {
                    JsonNode *rn = json_object_dup_member(root, "result");
                    gchar *body = json_to_string(rn, FALSE);
                    json_node_unref(rn);
                    _send_json(msg, 200, body);
                    g_free(body);
                } else {
                    _send_json(msg, 200, resp);
                }
            } else {
                _send_json(msg, 200, resp);
            }
            g_object_unref(jp);
            g_free(resp);
        } else {
            _error(msg, 500, "INTERNAL_ERROR", "Update-check RPC failed");
        }
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/internal/vms") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        virConnectPtr vconn = virConnectOpen("qemu:///system");
        JsonArray *arr = json_array_new();
        if (vconn) {
            virDomainPtr *domains = nullptr;
            int n = virConnectListAllDomains(vconn, &domains,
                VIR_CONNECT_LIST_DOMAINS_ACTIVE | VIR_CONNECT_LIST_DOMAINS_INACTIVE);
            for (int i = 0; i < n; i++) {
                JsonObject *vm = json_object_new();
                const gchar *name = virDomainGetName(domains[i]);
                json_object_set_string_member(vm, "name", name ? name : "?");
                int state = 0;
                virDomainGetState(domains[i], &state, NULL, 0);
                json_object_set_string_member(vm, "state",
                    state == 1 ? "running" : state == 5 ? "shutoff" : "other");
                virDomainInfo info;
                if (virDomainGetInfo(domains[i], &info) == 0) {
                    json_object_set_int_member(vm, "vcpu", info.nrVirtCpu);
                    json_object_set_int_member(vm, "memory_mb", (gint64)info.memory / 1024);
                }
                json_array_add_object_element(arr, vm);
                virDomainFree(domains[i]);
            }
            if (domains) free(domains);
            virConnectClose(vconn);
        }
        JsonNode *node = json_node_new(JSON_NODE_ARRAY);
        json_node_take_array(node, arr);
        gchar *data = json_to_string(node, FALSE);
        _send_json(msg, 200, data);
        g_free(data);
        json_node_free(node);
        return;
    }

#if PCV_CLUSTER_ENABLED

    if (g_strcmp0(path, REST_API_PREFIX "/internal/cluster/vms") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        gchar *rpc = g_strdup("{\"jsonrpc\":\"2.0\",\"method\":\"cluster.vm.list\",\"params\":{},\"id\":\"rest\"}");
        gchar *uds_resp = _rpc_over_uds(rpc);
        g_free(rpc);
        _send_json(msg, 200, uds_resp ? uds_resp : "{\"result\":[]}");
        g_free(uds_resp);
        return;
    }
#endif

    if (g_strcmp0(path, REST_API_PREFIX "/internal/telemetry") == 0
        && g_strcmp0(method, "GET") == 0)
    {
        JsonObject *obj = json_object_new();

        FILE *cpuf = fopen("/proc/stat", "r");
        if (cpuf) {
            char buf[256];
            if (fgets(buf, sizeof(buf), cpuf)) {
                guint64 u,n,s,id,io,ir,si,st;
                if (sscanf(buf,"cpu %lu %lu %lu %lu %lu %lu %lu %lu",&u,&n,&s,&id,&io,&ir,&si,&st)==8) {
                    guint64 tot=u+n+s+id+io+ir+si+st, idle=id+io;
                    json_object_set_double_member(obj,"cpu_percent",tot>0?100.0*(1.0-(double)idle/(double)tot):0);
                }
            }
            fclose(cpuf);
        }

        FILE *memf = fopen("/proc/meminfo", "r");
        if (memf) {
            char line[128]; guint64 val;
            while (fgets(line, sizeof(line), memf)) {
                if (sscanf(line,"MemTotal: %lu kB",&val)==1) json_object_set_int_member(obj,"mem_total_kb",(gint64)val);
                else if (sscanf(line,"MemAvailable: %lu kB",&val)==1) json_object_set_int_member(obj,"mem_avail_kb",(gint64)val);
            }
            fclose(memf);
        }

        virConnectPtr tc = virConnectOpen("qemu:///system");
        if (tc) {
            json_object_set_int_member(obj,"vm_count",virConnectNumOfDomains(tc));
            virConnectClose(tc);
        }
        JsonNode *tn = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(tn, obj);
        gchar *td = json_to_string(tn, FALSE);
        _send_json(msg, 200, td);
        g_free(td); json_node_free(tn);
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/metrics") == 0
        && g_strcmp0(method, "GET") == 0)
    {

        const gchar *m_auth = pcv_config_get_string("metrics", "auth", "none");
        if (g_strcmp0(m_auth, "required") == 0) {
            gchar *subj = _authenticate(msg, NULL);
            if (!subj) return;
            g_free(subj);
        }

        gchar *rpc = _build_rpc("monitor.fleet", NULL);
        gchar *resp = _rpc_over_uds(rpc);
        g_free(rpc);

        GString *prom = g_string_new("");
        if (resp) {
            JsonParser *jp = json_parser_new();
            /* PCV_PARSE_TRUSTED: 데몬 JSON-RPC 응답 파싱(내부 신뢰 출력, 외부 입력 아님) */
            if (json_parser_load_from_data(jp, resp, -1, NULL)) {
                JsonObject *root = json_node_get_object(json_parser_get_root(jp));
                if (json_object_has_member(root, "result")) {
                    JsonObject *res = json_object_get_object_member(root, "result");

                    if (json_object_has_member(res, "host")) {
                        JsonObject *h = json_object_get_object_member(res, "host");
                        {
                            gdouble cpu_pct = 0;
                            if (json_object_has_member(h,"cpu_percent")) {
                                cpu_pct = json_object_get_double_member(h,"cpu_percent");
                            } else if (json_object_has_member(h,"cpu_total_ticks") &&
                                       json_object_has_member(h,"cpu_idle_ticks")) {

                                static gdouble prev_total = 0, prev_idle = 0;
                                gdouble total = json_object_get_double_member(h,"cpu_total_ticks");
                                gdouble idle = json_object_get_double_member(h,"cpu_idle_ticks");
                                gdouble dt = total - prev_total;
                                gdouble di = idle - prev_idle;
                                if (dt > 0 && prev_total > 0)
                                    cpu_pct = 100.0 * (1.0 - di / dt);
                                prev_total = total;
                                prev_idle = idle;
                            }
                            g_string_append_printf(prom,
                                "# HELP purecvisor_host_cpu_percent Host CPU utilization\n"
                                "# TYPE purecvisor_host_cpu_percent gauge\n"
                                "purecvisor_host_cpu_percent %.2f\n", cpu_pct);
                        }
                        g_string_append_printf(prom,
                            "# HELP purecvisor_host_memory_percent Host memory utilization\n"
                            "# TYPE purecvisor_host_memory_percent gauge\n"
                            "purecvisor_host_memory_percent %.2f\n",
                            json_object_has_member(h,"mem_percent")
                                ? json_object_get_double_member(h,"mem_percent") : 0);
                        g_string_append_printf(prom,
                            "# HELP purecvisor_host_disk_percent Host disk utilization\n"
                            "# TYPE purecvisor_host_disk_percent gauge\n"
                            "purecvisor_host_disk_percent %.2f\n",
                            json_object_has_member(h,"disk_percent")
                                ? json_object_get_double_member(h,"disk_percent") : 0);
                        if (json_object_has_member(h,"mem_total_gb"))
                            g_string_append_printf(prom,
                                "# HELP purecvisor_host_memory_total_bytes Host total memory\n"
                                "# TYPE purecvisor_host_memory_total_bytes gauge\n"
                                "purecvisor_host_memory_total_bytes %.0f\n",
                                json_object_get_double_member(h,"mem_total_gb") * 1073741824.0);
                        if (json_object_has_member(h,"cpu_temp_c"))
                            g_string_append_printf(prom,
                                "# HELP purecvisor_host_cpu_temp_celsius CPU temperature\n"
                                "# TYPE purecvisor_host_cpu_temp_celsius gauge\n"
                                "purecvisor_host_cpu_temp_celsius %.1f\n",
                                json_object_get_double_member(h,"cpu_temp_c"));
                        if (json_object_has_member(h,"load_1"))
                            g_string_append_printf(prom,
                                "# HELP purecvisor_host_load1 1-minute load average\n"
                                "# TYPE purecvisor_host_load1 gauge\n"
                                "purecvisor_host_load1 %.2f\n",
                                json_object_get_double_member(h,"load_1"));
                    }

                    if (json_object_has_member(res, "fleet")) {
                        JsonArray *fleet = json_object_get_array_member(res, "fleet");
                        guint n = json_array_get_length(fleet);

                        g_string_append(prom,
                            "# HELP purecvisor_vm_vcpu VM vCPU count\n"
                            "# TYPE purecvisor_vm_vcpu gauge\n");
                        for (guint i = 0; i < n; i++) {
                            JsonObject *vm = json_array_get_object_element(fleet, i);
                            const gchar *vn = json_object_get_string_member(vm, "name");
                            g_string_append_printf(prom,
                                "purecvisor_vm_vcpu{vm=\"%s\"} %ld\n",
                                vn, (long)json_object_get_int_member(vm, "vcpu"));
                        }

                        g_string_append(prom,
                            "# HELP purecvisor_vm_memory_max_mb VM max memory MB\n"
                            "# TYPE purecvisor_vm_memory_max_mb gauge\n");
                        for (guint i = 0; i < n; i++) {
                            JsonObject *vm = json_array_get_object_element(fleet, i);
                            const gchar *vn = json_object_get_string_member(vm, "name");
                            g_string_append_printf(prom,
                                "purecvisor_vm_memory_max_mb{vm=\"%s\"} %.0f\n",
                                vn, json_object_get_double_member(vm, "mem_max_mb"));
                        }

                        g_string_append(prom,
                            "# HELP purecvisor_vm_memory_used_mb VM used memory MB\n"
                            "# TYPE purecvisor_vm_memory_used_mb gauge\n");
                        for (guint i = 0; i < n; i++) {
                            JsonObject *vm = json_array_get_object_element(fleet, i);
                            const gchar *vn = json_object_get_string_member(vm, "name");
                            g_string_append_printf(prom,
                                "purecvisor_vm_memory_used_mb{vm=\"%s\"} %.0f\n",
                                vn, json_object_get_double_member(vm, "mem_used_mb"));
                        }

                        g_string_append(prom,
                            "# HELP purecvisor_vm_running VM running state\n"
                            "# TYPE purecvisor_vm_running gauge\n");
                        for (guint i = 0; i < n; i++) {
                            JsonObject *vm = json_array_get_object_element(fleet, i);
                            const gchar *vn = json_object_get_string_member(vm, "name");
                            const gchar *st = json_object_get_string_member(vm, "state");
                            g_string_append_printf(prom,
                                "purecvisor_vm_running{vm=\"%s\"} %d\n",
                                vn, g_strcmp0(st, "RUNNING") == 0 ? 1 : 0);
                        }
                    }
                }
            }
            g_object_unref(jp);
            g_free(resp);
        }

        {
            gchar *reg_metrics = pcv_prom_render();
            if (reg_metrics) {
                g_string_append(prom, "\n# HELP purecvisor_registry PureCVisor Registry Metrics\n");
                g_string_append(prom, reg_metrics);
                g_free(reg_metrics);
            }
        }

        soup_server_message_set_status(msg, 200, NULL);
        SoupMessageHeaders *hdrs = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(hdrs, "Content-Type",
            "text/plain; version=0.0.4; charset=utf-8");
        soup_server_message_set_response(msg,
            "text/plain; version=0.0.4; charset=utf-8",
            SOUP_MEMORY_COPY, prom->str, prom->len);
        g_string_free(prom, TRUE);
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/auth/token") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        JsonObject  *body     = _parse_body(msg);
        const gchar *username_in = body
            ? json_object_get_string_member_with_default(body, "username", "")
            : "";
        const gchar *password_in = body
            ? json_object_get_string_member_with_default(body, "password", "")
            : "";
        gchar *username = g_strdup(username_in ? username_in : "");
        gchar *password = g_strdup(password_in ? password_in : "");

        const gchar *cfg_user = pcv_config_get_admin_user();
        const gchar *cfg_pass = pcv_config_get_admin_password();
        gboolean bootstrap_configured = cfg_user && *cfg_user && cfg_pass && *cfg_pass;

        gchar *token = nullptr;
        GError *err  = nullptr;

        gchar *refresh = nullptr;

        GSocketAddress *ra_pre = soup_server_message_get_remote_address(msg);
        gchar *client_ip = (ra_pre && G_IS_INET_SOCKET_ADDRESS(ra_pre))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(
                  G_INET_SOCKET_ADDRESS(ra_pre)))
            : g_strdup("unknown");
        {
            gint ip_lockout = pcv_rbac_get_ip_remaining_lockout(client_ip);
            if (ip_lockout > 0) {
                g_free(client_ip);
                if (body) json_object_unref(body);
                g_free(username);
                g_free(password);
                SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
                gchar retry_buf[32];
                g_snprintf(retry_buf, sizeof(retry_buf), "%d", ip_lockout);
                soup_message_headers_replace(rh, "Retry-After", retry_buf);
                _error(msg, 429, "TOO_MANY_REQUESTS",
                       "Too many failed login attempts from this IP");
                return;
            }
        }

        if (bootstrap_configured &&
            g_strcmp0(username, cfg_user) == 0 &&
            pcv_secret_str_eq(password, cfg_pass))
        {

            token = pcv_rbac_authenticate_v2(username, password, &refresh, &err);
            if (!token) {
                PcvUserExistence ex = pcv_rbac_user_exists(username);
                gboolean user_in_db = (ex != PCV_USER_ABSENT);
                if (pcv_rest_auth_should_fallback_bootstrap(username, password,
                                                            cfg_user, cfg_pass, user_in_db)) {

                    g_clear_error(&err);
                    token = pcv_jwt_sign(username, 900, &err);
                    pcv_audit_log(username, "auth.bootstrap.fallback",
                                  "bootstrap recovery", "ok", 0, 0, client_ip);
                }
            }
        } else {

            token = pcv_rbac_authenticate_v2(username, password, &refresh, &err);
        }

        if (!token) {
            g_free(refresh);
            pcv_rbac_ip_record_auth_failure(client_ip);
            pcv_audit_log(username, "auth.failed", "login attempt",
                          "fail", 401, 0, client_ip);
            g_free(client_ip);

            gint lockout_sec = pcv_rbac_get_remaining_lockout(username);
            if (lockout_sec > 0) {
                SoupMessageHeaders *resp_hdrs = soup_server_message_get_response_headers(msg);
                gchar retry_buf[32];
                g_snprintf(retry_buf, sizeof(retry_buf), "%d", lockout_sec);
                soup_message_headers_replace(resp_hdrs, "Retry-After", retry_buf);
                _error(msg, 429, "TOO_MANY_REQUESTS",
                       err ? err->message : "Account locked due to too many failed attempts");
            } else {

                if (err) {
                    PCV_LOG_WARN(REST_LOG_DOM, "auth.failed: user=%s reason=%s",
                                 username, err->message);
                }
                _error(msg, 401, "UNAUTHORIZED", "Invalid credentials");
            }
            if (err) g_error_free(err);
            PCV_LOG_WARN(REST_LOG_DOM,
                         "Auth failed for user '%s'", username);
            if (body) json_object_unref(body);
            g_free(username);
            g_free(password);
            return;
        }

        pcv_rbac_ip_record_auth_success(client_ip);
        g_free(client_ip);

        gchar *resp = nullptr;
        if (refresh) {
            resp = g_strdup_printf(
                "{\"access_token\":\"%s\","
                "\"refresh_token\":\"%s\","
                "\"token_type\":\"Bearer\","
                "\"expires_in\":900,"
                "\"refresh_expires_in\":604800}", token, refresh);
        } else {
            resp = g_strdup_printf(
                "{\"access_token\":\"%s\","
                "\"token_type\":\"Bearer\","
                "\"expires_in\":900}", token);
        }
        _send_json(msg, 200, resp);
        g_free(resp);
        g_free(token);
        g_free(refresh);
        PCV_LOG_INFO(REST_LOG_DOM, "Token issued for '%s'", username);
        if (body) json_object_unref(body);
        g_free(username);
        g_free(password);
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/auth/refresh") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        JsonObject  *body     = _parse_body(msg);
        const gchar *ref_tok  = body
            ? json_object_get_string_member_with_default(body, "refresh_token", "")
            : "";

        if (!ref_tok || !*ref_tok) {
            if (body) json_object_unref(body);
            _error(msg, 400, "BAD_REQUEST", "refresh_token is required");
            return;
        }

        GError *err = nullptr;
        gchar *new_refresh = nullptr;
        gchar *new_access = pcv_rbac_refresh_token(ref_tok, &new_refresh, &err);

        if (!new_access) {
            if (body) json_object_unref(body);
            g_free(new_refresh);
            _error(msg, 401, "UNAUTHORIZED",
                   err ? err->message : "Invalid refresh token");
            if (err) g_error_free(err);
            return;
        }

        gchar *resp = nullptr;
        if (new_refresh) {
            resp = g_strdup_printf(
                "{\"access_token\":\"%s\","
                "\"refresh_token\":\"%s\","
                "\"token_type\":\"Bearer\","
                "\"expires_in\":900,"
                "\"refresh_expires_in\":604800}",
                new_access, new_refresh);
        } else {
            resp = g_strdup_printf(
                "{\"access_token\":\"%s\","
                "\"token_type\":\"Bearer\","
                "\"expires_in\":900}",
                new_access);
        }
        _send_json(msg, 200, resp);
        g_free(resp);
        g_free(new_access);
        g_free(new_refresh);
        if (body) json_object_unref(body);
        PCV_LOG_INFO(REST_LOG_DOM, "Token refreshed via /auth/refresh");
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/auth/logout") == 0
        && g_strcmp0(method, "POST") == 0)
    {

        SoupMessageHeaders *hdrs = soup_server_message_get_request_headers(msg);
        const gchar *auth_hdr = soup_message_headers_get_one(hdrs, "Authorization");
        gchar *sub = nullptr;
        if (auth_hdr) {
            GError *verr = nullptr;
            sub = pcv_jwt_verify(auth_hdr, &verr);
            if (verr) g_error_free(verr);
        }

        if (sub) {
            GError *err = nullptr;
            pcv_rbac_revoke_session(sub, &err);
            if (err) g_error_free(err);

            if (auth_hdr) {
                const gchar *tk = auth_hdr;
                if (g_str_has_prefix(tk, "Bearer ")) tk += 7;
                if (g_str_has_prefix(tk, "bearer ")) tk += 7;

                gchar **parts = g_strsplit(tk, ".", 3);
                if (parts && parts[0] && parts[1]) {
                    gsize plen = 0;

                    gchar *p1 = g_strdup(parts[1]);
                    gsize p1len = strlen(p1);

                    for (gchar *c = p1; *c; c++) {
                        if (*c == '-') *c = '+';
                        else if (*c == '_') *c = '/';
                    }

                    gsize pad = (4 - (p1len % 4)) % 4;
                    gchar *padded = g_strconcat(p1, pad >= 1 ? "=" : "",
                                                pad >= 2 ? "=" : "",
                                                pad >= 3 ? "=" : "", NULL);
                    guchar *raw = g_base64_decode(padded, &plen);
                    g_free(padded); g_free(p1);
                    if (raw && plen > 0) {
                        gchar *json_str = g_strndup((const gchar *)raw, plen);
                        JsonParser *parser = json_parser_new();
                        /* PCV_PARSE_TRUSTED: pcv_jwt_verify() 서명검증 통과 후에만 도달 — 페이로드는 데몬 서명발급분(공격자 위조 불가), 서명이 곧 가드 */
                        if (json_parser_load_from_data(parser, json_str, -1, NULL)) {
                            JsonObject *o = json_node_get_object(json_parser_get_root(parser));
                            const gchar *jti = json_object_get_string_member_with_default(o, "jti", NULL);
                            gint64 exp = json_object_get_int_member_with_default(o, "exp", 0);
                            if (jti && exp > 0) {
                                pcv_jwt_blacklist_add(jti, exp);
                            }
                        }
                        g_object_unref(parser);
                        g_free(json_str);
                    }
                    g_free(raw);
                }
                g_strfreev(parts);
            }

            _send_json(msg, 200, "{\"status\":\"ok\",\"message\":\"All sessions revoked\"}");
            PCV_LOG_INFO(REST_LOG_DOM, "Logout: sessions revoked + access token blacklisted for '%s'", sub);
            g_free(sub);
        } else {
            _error(msg, 401, "UNAUTHORIZED", "Valid token required for logout");
        }
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/auth/password") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        SoupMessageHeaders *hdrs = soup_server_message_get_request_headers(msg);
        const gchar *auth_hdr = soup_message_headers_get_one(hdrs, "Authorization");
        if (!auth_hdr || !g_str_has_prefix(auth_hdr, "Bearer ")) {
            _error(msg, 401, "UNAUTHORIZED", "Bearer token required");
            return;
        }
        GError *verr = nullptr;
        gchar *sub = pcv_jwt_verify(auth_hdr, &verr);
        if (verr) g_error_free(verr);
        if (!sub) {
            _error(msg, 401, "UNAUTHORIZED", "Invalid or expired token");
            return;
        }

        const gchar *boot_admin = pcv_config_get_admin_user();
        if (boot_admin && g_strcmp0(sub, boot_admin) == 0) {
            g_free(sub);
            _error(msg, 403, "FORBIDDEN",
                   "Bootstrap admin password must be changed in daemon.conf [daemon] admin_password");
            return;
        }

        JsonObject *body = _parse_body(msg);
        const gchar *old_pw = body
            ? json_object_get_string_member_with_default(body, "old_password", "")
            : "";
        const gchar *new_pw = body
            ? json_object_get_string_member_with_default(body, "new_password", "")
            : "";

        if (!old_pw || !*old_pw || !new_pw || !*new_pw) {
            if (body) json_object_unref(body);
            g_free(sub);
            _error(msg, 400, "BAD_REQUEST",
                   "old_password and new_password are required");
            return;
        }

        GError *err = nullptr;
        gboolean ok = pcv_rbac_change_password(sub, old_pw, new_pw, &err);

        GSocketAddress *ra = soup_server_message_get_remote_address(msg);
        gchar *rip = (ra && G_IS_INET_SOCKET_ADDRESS(ra))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)))
            : g_strdup("unknown");

        if (!ok) {
            int code = err && err->code == G_IO_ERROR_PERMISSION_DENIED ? 401 :
                       err && err->code == G_IO_ERROR_INVALID_ARGUMENT ? 400 : 500;
            const char *ec = code == 401 ? "UNAUTHORIZED" :
                             code == 400 ? "BAD_REQUEST" : "SERVER_ERROR";
            pcv_audit_log(sub, "auth.password.change", "self change",
                          "fail", code, 0, rip);
            g_free(rip);
            if (body) json_object_unref(body);
            _error(msg, code, ec,
                   err ? err->message : "Failed to change password");
            if (err) g_error_free(err);
            g_free(sub);
            return;
        }

        pcv_audit_log(sub, "auth.password.change", "self change",
                      "ok", 0, 0, rip);
        g_free(rip);
        PCV_LOG_INFO(REST_LOG_DOM, "Password changed for '%s' (sessions revoked)", sub);
        g_free(sub);
        if (body) json_object_unref(body);

        _send_json(msg, 200,
                   "{\"status\":\"ok\",\"message\":\"Password changed. All sessions revoked.\"}");
        return;
    }

    if (g_strcmp0(path, REST_API_PREFIX "/auth/register") == 0
        && g_strcmp0(method, "POST") == 0)
    {
        const gchar *enabled = pcv_config_get_string("auth", "allow_self_register", "false");
        if (g_ascii_strcasecmp(enabled, "true") != 0 &&
            g_ascii_strcasecmp(enabled, "1")    != 0 &&
            g_ascii_strcasecmp(enabled, "yes")  != 0) {
            _error(msg, 403, "FORBIDDEN", "Self-registration is disabled");
            return;
        }

        JsonObject *body = _parse_body(msg);
        const gchar *username = body
            ? json_object_get_string_member_with_default(body, "username", "")
            : "";
        const gchar *password = body
            ? json_object_get_string_member_with_default(body, "password", "")
            : "";

        size_t ulen = username ? strlen(username) : 0;
        gboolean uvalid = (ulen >= 3 && ulen <= 32);
        for (size_t i = 0; uvalid && i < ulen; i++) {
            char c = username[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                uvalid = FALSE;
        }
        if (!uvalid) {
            if (body) json_object_unref(body);
            _error(msg, 400, "BAD_REQUEST",
                   "username must be 3-32 chars, lowercase alphanumeric or underscore");
            return;
        }

        size_t plen = password ? strlen(password) : 0;
        if (plen < 8) {
            if (body) json_object_unref(body);
            _error(msg, 400, "BAD_REQUEST", "password must be at least 8 characters");
            return;
        }
        if (g_strcmp0(username, password) == 0) {
            if (body) json_object_unref(body);
            _error(msg, 400, "BAD_REQUEST", "password must differ from username");
            return;
        }

        const gchar *adm = pcv_config_get_admin_user();
        if (adm && g_strcmp0(username, adm) == 0) {
            if (body) json_object_unref(body);
            _error(msg, 409, "CONFLICT", "username is reserved");
            return;
        }

        GError *err = nullptr;
        gboolean ok = pcv_rbac_user_create(username, password,
                                           PCV_ROLE_VIEWER, NULL, &err);

        GSocketAddress *ra = soup_server_message_get_remote_address(msg);
        gchar *rip = (ra && G_IS_INET_SOCKET_ADDRESS(ra))
            ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)))
            : g_strdup("unknown");

        if (!ok) {
            pcv_audit_log(username, "auth.register", "self register",
                          "fail", 409, 0, rip);
            g_free(rip);
            if (body) json_object_unref(body);
            _error(msg, 409, "CONFLICT",
                   err ? err->message : "Failed to create user (already exists?)");
            if (err) g_error_free(err);
            return;
        }

        pcv_audit_log(username, "auth.register", "self register",
                      "ok", 0, 0, rip);
        g_free(rip);

        gchar *resp = g_strdup_printf(
            "{\"status\":\"ok\",\"username\":\"%s\",\"role\":\"viewer\"}", username);
        _send_json(msg, 201, resp);
        g_free(resp);
        if (body) json_object_unref(body);
        PCV_LOG_INFO(REST_LOG_DOM, "Self-registered user '%s'", username);
        return;
    }

    gint key_role = -1;
    gchar *subject = _authenticate(msg, &key_role);
    if (!subject) return;

    JsonObject *body    = _parse_body(msg);
    gchar     **segs    = _split_path(path);

    const gchar *resource = segs[0] ? segs[0] : "";
    const gchar *name     = segs[1] ? segs[1] : "";
    const gchar *action   = segs[2] ? segs[2] : "";
    const gchar *sub      = segs[3] ? segs[3] : "";

    gchar *rpc = nullptr;
    gboolean  is_vm_delete   = FALSE;
    gchar    *vm_delete_name = nullptr;

    if (g_strcmp0(resource, "demo") == 0 &&
        g_strcmp0(name, "ovn-ovs") == 0 &&
        g_strcmp0(action, "health") == 0 &&
        g_strcmp0(method, "GET") == 0) {
        _send_ovn_demo_health(msg);
        goto cleanup;
    }

    gboolean needs_libvirt =
        (g_strcmp0(resource, "vms") == 0 ||
         g_strcmp0(resource, "containers") == 0 ||
#if PCV_CLUSTER_ENABLED
         g_str_has_prefix(path, REST_API_PREFIX "/cluster/vms") ||
#endif
         FALSE);
    if (needs_libvirt && cb_is_open()) {
        SoupMessageHeaders *rh = soup_server_message_get_response_headers(msg);
        soup_message_headers_replace(rh, "Retry-After", "30");
        _error(msg, 503, "SERVICE_UNAVAILABLE",
               "libvirt connection is down (circuit breaker open)");
        goto cleanup;
    }

    if (g_strcmp0(resource, "rpc") == 0 && *name == '\0' &&
        g_strcmp0(method, "POST") == 0) {
        if (!body) {
            _error(msg, 400, "BAD_REQUEST", "JSON body required");
            goto cleanup;
        }
        JsonNode *bn = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(bn, body);
        rpc = json_to_string(bn, FALSE);
        json_node_free(bn);
    }
    else

    if (g_strcmp0(resource, "vms") == 0) {

        if (*name == '\0') {

            if (g_strcmp0(method, "GET") == 0) {
                JsonObject *pg = nullptr;
                if (query) {
                    const gchar *q_off = g_hash_table_lookup(query, "offset");
                    const gchar *q_lim = g_hash_table_lookup(query, "limit");
                    if (q_off || q_lim) {
                        pg = json_object_new();
                        if (q_off) json_object_set_int_member(pg, "offset", g_ascii_strtoll(q_off, NULL, 10));
                        if (q_lim) json_object_set_int_member(pg, "limit", g_ascii_strtoll(q_lim, NULL, 10));
                    }
                }
                rpc = _build_rpc("vm.list", pg);
                if (pg) json_object_unref(pg);
            }

            else if (g_strcmp0(method, "POST") == 0) {
                const gchar *req[] = {"name"};
                if (!pcv_validate_required(msg, body, req, 1)) goto cleanup;
                rpc = _build_rpc("vm.create", body);
            }

        } else if (*action == '\0') {

            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc_name("vm.metrics", name);

            else if (g_strcmp0(method, "DELETE") == 0) {
                rpc = _build_rpc_name("vm.delete", name);
                is_vm_delete = TRUE;
                vm_delete_name = g_strdup(name);
            }

        } else if (g_strcmp0(action, "start") == 0) {
            rpc = _build_rpc_name("vm.start", name);

        } else if (g_strcmp0(action, "stop") == 0) {
            rpc = _build_rpc_name("vm.stop", name);

        } else if (g_strcmp0(action, "suspend") == 0 || g_strcmp0(action, "pause") == 0) {
            rpc = _build_rpc_name("vm.pause", name);

        } else if (g_strcmp0(action, "resume") == 0) {
            rpc = _build_rpc_name("vm.resume", name);

        } else if (g_strcmp0(action, "metrics") == 0) {
            rpc = _build_rpc_name("vm.metrics", name);

        } else if (g_strcmp0(action, "snapshot") == 0) {

            if (*sub == '\0' && g_strcmp0(method, "GET") == 0) {
                rpc = _build_rpc_name("vm.snapshot.list", name);

            } else if (g_strcmp0(sub, "create") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                const gchar *sname = nullptr;
                if (body) {
                    if (json_object_has_member(body, "snapshot_name"))
                        sname = json_object_get_string_member(body, "snapshot_name");
                    else if (json_object_has_member(body, "snap_name"))
                        sname = json_object_get_string_member(body, "snap_name");
                }
                if (sname)
                    json_object_set_string_member(p, "snapshot_name", sname);
                rpc = _build_rpc("vm.snapshot.create", p);
                json_object_unref(p);

            } else if (g_strcmp0(sub, "rollback") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                const gchar *rname = nullptr;
                if (body) {
                    if (json_object_has_member(body, "snapshot_name"))
                        rname = json_object_get_string_member(body, "snapshot_name");
                    else if (json_object_has_member(body, "snap_name"))
                        rname = json_object_get_string_member(body, "snap_name");
                }
                if (rname)
                    json_object_set_string_member(p, "snapshot_name", rname);
                rpc = _build_rpc("vm.snapshot.rollback", p);
                json_object_unref(p);

            } else if (g_strcmp0(sub, "delete_all") == 0 && g_strcmp0(method, "POST") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                if (body && json_object_has_member(body, "prefix"))
                    json_object_set_string_member(p, "prefix",
                        json_object_get_string_member(body, "prefix"));
                if (body && json_object_has_member(body, "keep_recent"))
                    json_object_set_int_member(p, "keep_recent",
                        json_object_get_int_member(body, "keep_recent"));
                rpc = _build_rpc("vm.snapshot.delete_all", p);
                json_object_unref(p);

            } else if (g_strcmp0(sub, "schedule") == 0) {
                if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "vm_name", name);
                    rpc = _build_rpc("vm.snapshot.schedule.set", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "vm_name", name);
                    rpc = _build_rpc("vm.snapshot.schedule.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "DELETE") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "vm_name", name);
                    rpc = _build_rpc("vm.snapshot.schedule.delete", p);
                    json_object_unref(p);
                }

            } else if (*sub != '\0' && g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                json_object_set_string_member(p, "snapshot_name", sub);
                rpc = _build_rpc("vm.snapshot.delete", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "nics") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "vm_id", name);
                    rpc = _build_rpc("device.nic.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "POST") == 0) {

                    JsonObject *p = body
                        ? (json_object_ref(body), body)
                        : json_object_new();
                    json_object_set_string_member(p, "vm_id", name);
                    rpc = _build_rpc("device.nic.attach", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(method, "DELETE") == 0) {

                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                json_object_set_string_member(p, "mac",   sub);
                rpc = _build_rpc("device.nic.detach", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "usb") == 0) {
            if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rpc = _build_rpc("vm.usb.list", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "POST") == 0) {
                JsonObject *p = body
                    ? (json_object_ref(body), body)
                    : json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rpc = _build_rpc("vm.usb.attach", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = body
                    ? (json_object_ref(body), body)
                    : json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rpc = _build_rpc("vm.usb.detach", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "vcpu") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            gint vcpu_count = 0;
            if (pcv_rpc_params_get_int_alias(p, "vcpu_count", "vcpu", &vcpu_count) ||
                pcv_rpc_params_get_int_alias(p, "count", NULL, &vcpu_count)) {
                json_object_set_int_member(p, "vcpu_count", vcpu_count);
            }
            json_object_set_string_member(p, "vm_id", name);
            rpc = _build_rpc("vm.set_vcpu", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "memory") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rpc = _build_rpc("vm.set_memory", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "delete-status") == 0 && g_strcmp0(method, "GET") == 0) {
            rpc = _build_rpc_name("vm.delete.status", name);

        } else if (g_strcmp0(action, "rename") == 0 && g_strcmp0(method, "PUT") == 0) {
            if (!body || !json_object_has_member(body, "new_name")) {
                _error(msg, 400, "BAD_REQUEST", "new_name required");
                goto cleanup;
            }
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            json_object_set_string_member(p, "new_name",
                json_object_get_string_member(body, "new_name"));
            rpc = _build_rpc("vm.rename", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "iso") == 0) {
            if (g_strcmp0(method, "POST") == 0) {
                JsonObject *p = body ? (json_object_ref(body), body)
                                     : json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rpc = _build_rpc("vm.mount_iso", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "vm_id", name);
                rpc = _build_rpc("vm.eject", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "clone") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.clone", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "disk") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rpc = _build_rpc("vm.resize_disk", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "cpu-pin") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rpc = _build_rpc("vm.pin_vcpu", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "bandwidth") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "vm_id", name);
            rpc = _build_rpc("vm.set_bandwidth", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "export") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc_name("vm.export.ova", name);

        } else if (g_strcmp0(action, "import") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("vm.import.ova", body);

        } else if (g_strcmp0(action, "import-ec2") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.import.ec2", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "export-ec2") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.export.ec2", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "import-status") == 0 && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.import.status", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "export-status") == 0 && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.export.status", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "memory-stats") == 0 && g_strcmp0(method, "GET") == 0) {
            rpc = _build_rpc_name("vm.memory.stats", name);

        } else if (g_strcmp0(action, "snapshot-schedule") == 0 && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("snapshot.schedule.status", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "guest-ping") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc_name("vm.guest.ping", name);

        } else if (g_strcmp0(action, "guest-agent") == 0 && g_strcmp0(method, "GET") == 0) {
            rpc = _build_rpc_name("vm.guest.agent.status", name);

        } else if (g_strcmp0(action, "guest-agent-channel") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc_name("vm.guest.agent.ensure_channel", name);

        } else if (g_strcmp0(action, "guest-shutdown") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc_name("vm.guest.shutdown", name);

        } else if (g_strcmp0(action, "disk-usage") == 0 && g_strcmp0(method, "GET") == 0) {
            rpc = _build_rpc_name("vm.guest.fsinfo", name);

        } else if (g_strcmp0(action, "disk-resize") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.disk.live_resize", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "guest-exec") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("vm.guest.exec", p);
            json_object_unref(p);
        }
    }

    else if (g_strcmp0(resource, "cloud") == 0) {

        if (g_strcmp0(name, "jobs") == 0 && g_strcmp0(method, "GET") == 0) {
            rpc = _build_rpc("cloud.jobs.list", NULL);
        }

        else if (g_strcmp0(name, "cancel") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("cloud.job.cancel", body);
        }
    }

    else if (g_strcmp0(resource, "containers") == 0) {

        if (*name == '\0') {
            if (g_strcmp0(method, "GET") == 0) {
                JsonObject *pg = nullptr;
                if (query) {
                    const gchar *q_off = g_hash_table_lookup(query, "offset");
                    const gchar *q_lim = g_hash_table_lookup(query, "limit");
                    if (q_off || q_lim) {
                        pg = json_object_new();
                        if (q_off) json_object_set_int_member(pg, "offset", g_ascii_strtoll(q_off, NULL, 10));
                        if (q_lim) json_object_set_int_member(pg, "limit", g_ascii_strtoll(q_lim, NULL, 10));
                    }
                }
                rpc = _build_rpc("container.list", pg);
                if (pg) json_object_unref(pg);
            } else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("container.create", body);

        } else if (*action == '\0' && g_strcmp0(method, "DELETE") == 0) {
            rpc = _build_rpc_name("container.destroy", name);

        } else if (g_strcmp0(action, "start") == 0) {
            rpc = _build_rpc_name("container.start", name);
        } else if (g_strcmp0(action, "stop") == 0) {
            rpc = _build_rpc_name("container.stop", name);
        } else if (g_strcmp0(action, "metrics") == 0) {
            rpc = _build_rpc_name("container.metrics", name);
        } else if (g_strcmp0(action, "exec") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            if (body && json_object_has_member(body, "command"))
                json_object_set_string_member(p, "cmd",
                    json_object_get_string_member(body, "command"));
            rpc = _build_rpc("container.exec", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "snapshots") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    rpc = _build_rpc_name("container.snapshot.list", name);
                } else if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rpc = _build_rpc("container.snapshot.create", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(sub, "rollback") == 0 && g_strcmp0(method, "POST") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.snapshot.rollback", p);
                json_object_unref(p);
            } else if (*sub != '\0' && g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                json_object_set_string_member(p, "snap_name", sub);
                rpc = _build_rpc("container.snapshot.delete", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "limits") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("container.set_limits", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "nics") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rpc = _build_rpc("container.nic.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? (json_object_ref(body), body)
                                         : json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rpc = _build_rpc("container.nic.attach", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                json_object_set_string_member(p, "nic_name", sub);
                rpc = _build_rpc("container.nic.detach", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "bandwidth") == 0 && g_strcmp0(method, "PUT") == 0) {
            JsonObject *p = body ? (json_object_ref(body), body)
                                 : json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("container.set_bandwidth", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "health") == 0) {
            if (g_strcmp0(method, "PUT") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.health.set", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.health.get", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.health.delete", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "volumes") == 0) {
            if (*sub == '\0') {
                if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rpc = _build_rpc("container.volume.list", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "POST") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "name", name);
                    rpc = _build_rpc("container.volume.attach", p);
                    json_object_unref(p);
                }
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                json_object_set_string_member(p, "volume", sub);
                rpc = _build_rpc("container.volume.detach", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "env") == 0) {
            if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.env.list", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "PUT") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.env.set", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("container.env.delete", p);
                json_object_unref(p);
            }

        } else if (g_strcmp0(action, "clone") == 0 && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = body ? json_object_ref(body) : json_object_new();
            json_object_set_string_member(p, "source", name);
            rpc = _build_rpc("container.clone", p);
            json_object_unref(p);

        } else if (g_strcmp0(action, "memory-stats") == 0 && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "name", name);
            rpc = _build_rpc("container.memory.stats", p);
            json_object_unref(p);
        }
    }

    else if (g_strcmp0(resource, "monitor") == 0) {
        if (g_strcmp0(name, "metrics") == 0)
            rpc = _build_rpc("monitor.metrics", NULL);
        else if (g_strcmp0(name, "fleet") == 0)
            rpc = _build_rpc("monitor.fleet", NULL);
    }

    else if (g_strcmp0(resource, "alerts") == 0) {
        if (g_strcmp0(name, "config") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("alert.config.get", NULL);
            else if (g_strcmp0(method, "PUT") == 0)
                rpc = _build_rpc("alert.config.set", body);
        } else if (g_strcmp0(name, "actions") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("alert.action.list", NULL);
        } else if (g_strcmp0(name, "silence") == 0) {

            if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("alert.silence", body);
        } else if (g_strcmp0(name, "silences") == 0) {

            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("alert.silence.list", NULL);
        } else if (g_strcmp0(name, "dlq") == 0) {

            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("alert.dlq.list", NULL);
        } else {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("alert.history", NULL);
        }
    }

    else if (g_strcmp0(resource, "prometheus") == 0) {
        if (g_strcmp0(name, "sd") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("prometheus.sd", NULL);
    }

    else if (g_strcmp0(resource, "health") == 0) {
        if (g_strcmp0(name, "deep") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("health.deep", NULL);
    }

    else if (g_strcmp0(resource, "pool") == 0) {
        if (g_strcmp0(name, "conninfo") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("pool.conninfo", NULL);
    }

    else if (g_strcmp0(resource, "db") == 0) {
        if (g_strcmp0(name, "migration") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("db.migration.status", NULL);
    }

    else if (g_strcmp0(resource, "audit") == 0) {
        if (g_strcmp0(name, "search") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("audit.search", body);
    }

    else if (g_strcmp0(resource, "jobs") == 0) {
        if (*name == '\0' && g_strcmp0(method, "GET") == 0) {
            JsonObject *pg = nullptr;
            if (query) {
                const gchar *q_off = g_hash_table_lookup(query, "offset");
                const gchar *q_lim = g_hash_table_lookup(query, "limit");
                if (q_off || q_lim) {
                    pg = json_object_new();
                    if (q_off) json_object_set_int_member(pg, "offset", g_ascii_strtoll(q_off, NULL, 10));
                    if (q_lim) json_object_set_int_member(pg, "limit", g_ascii_strtoll(q_lim, NULL, 10));
                }
            }
            rpc = _build_rpc("jobs.list", pg);
            if (pg) json_object_unref(pg);
        } else if (*name != '\0' && g_strcmp0(action, "cancel") == 0
                   && g_strcmp0(method, "POST") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "job_id", name);
            rpc = _build_rpc("jobs.cancel", p);
            json_object_unref(p);
        } else if (g_strcmp0(name, "persistent") == 0 && *action == '\0'
                   && g_strcmp0(method, "GET") == 0) {

            rpc = _build_rpc("jobs.persist.list", NULL);
        } else if (*name != '\0' && *action == '\0'
                   && g_strcmp0(method, "GET") == 0) {
            JsonObject *p = json_object_new();
            json_object_set_string_member(p, "job_id", name);
            rpc = _build_rpc("jobs.get", p);
            json_object_unref(p);
        }
    }

    else if (g_strcmp0(resource, "webhook") == 0) {
        if (g_strcmp0(name, "dlq") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("webhook.dlq.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("webhook.dlq.retry", NULL);
        }
    }

    else if (g_strcmp0(resource, "processes") == 0) {
        if (g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("monitor.processes", NULL);
    }

    else if (g_strcmp0(resource, "agent") == 0) {
        if (g_strcmp0(name, "config") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("agent.config.get", NULL);
            else if (g_strcmp0(method, "PUT") == 0)
                rpc = _build_rpc("agent.config.set", body);
        } else if (g_strcmp0(name, "history") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("agent.history", NULL);
        } else {

            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("agent.config.get", NULL);
        }
    }

    else if (g_strcmp0(resource, "networks") == 0) {
        if (*name == '\0') {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("network.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("network.create", body);
        } else {

            if (g_strcmp0(action, "mode") == 0 && g_strcmp0(method, "POST") == 0) {

                JsonObject *p = body ? json_object_ref(body) : json_object_new();
                json_object_set_string_member(p, "name", name);
                rpc = _build_rpc("network.mode_set", p);
                json_object_unref(p);

            } else if (g_strcmp0(action, "qos") == 0) {
                if (g_strcmp0(method, "PUT") == 0) {
                    JsonObject *p = body ? json_object_ref(body) : json_object_new();
                    json_object_set_string_member(p, "interface", name);
                    rpc = _build_rpc("network.qos.set", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "GET") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "interface", name);
                    rpc = _build_rpc("network.qos.get", p);
                    json_object_unref(p);
                } else if (g_strcmp0(method, "DELETE") == 0) {
                    JsonObject *p = json_object_new();
                    json_object_set_string_member(p, "interface", name);
                    rpc = _build_rpc("network.qos.remove", p);
                    json_object_unref(p);
                }

            } else if (g_strcmp0(method, "GET") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "bridge_name", name);
                rpc = _build_rpc("network.info", p);
                json_object_unref(p);
            } else if (g_strcmp0(method, "DELETE") == 0) {
                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "bridge_name", name);
                rpc = _build_rpc("network.delete", p);
                json_object_unref(p);
            }
        }
    }

    else if (g_strcmp0(resource, "storage") == 0) {

        if (g_strcmp0(name, "pools") == 0) {
            if (*action == '\0') {
                if (g_strcmp0(method, "GET") == 0)
                    rpc = _build_rpc("storage.pool.list", NULL);

                else if (g_strcmp0(method, "POST") == 0)
                    rpc = _build_rpc("storage.pool.create", body);

                else if (g_strcmp0(method, "DELETE") == 0)
                    rpc = _build_rpc("storage.pool.destroy", body);
            }

            else if (g_strcmp0(action, "scrub") == 0 && g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("storage.pool.scrub", body);
        }
        else if (g_strcmp0(name, "zvols") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("storage.zvol.list", body);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("storage.zvol.create", body);
            else if (g_strcmp0(method, "DELETE") == 0)
                rpc = _build_rpc("storage.zvol.delete", body);
        }
    }

    else if (g_strcmp0(resource, "dpdk") == 0) {
        if (g_strcmp0(name, "status") == 0)
            rpc = _build_rpc("dpdk.status", NULL);
        else if (g_strcmp0(name, "bind") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("dpdk.bind", body);
        else if (g_strcmp0(name, "unbind") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("dpdk.unbind", body);
        else if (g_strcmp0(name, "list") == 0)
            rpc = _build_rpc("dpdk.list", NULL);
        else if (g_strcmp0(name, "hugepage") == 0)
            rpc = _build_rpc("dpdk.hugepage.info", NULL);
        else if (g_strcmp0(name, "bridge") == 0) {
            if (g_strcmp0(action, "create") == 0 && g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("dpdk.bridge.create", body);
            else if (g_strcmp0(action, "delete") == 0 && g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("dpdk.bridge.delete", body);
        }
    }

    else if (g_strcmp0(resource, "sriov") == 0) {
        if (g_strcmp0(name, "status") == 0)
            rpc = _build_rpc("sriov.status", NULL);
        else if (g_strcmp0(name, "enable") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("sriov.enable", body);
        else if (g_strcmp0(name, "disable") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("sriov.disable", body);
        else if (g_strcmp0(name, "list") == 0)
            rpc = _build_rpc("sriov.list", body);
        else if (g_strcmp0(name, "set") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("sriov.set", body);
        else if (g_strcmp0(name, "attach") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("sriov.attach", body);
        else if (g_strcmp0(name, "detach") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("sriov.detach", body);
    }

    else if (g_strcmp0(resource, "backup") == 0) {
        if (g_strcmp0(name, "policies") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("backup.policy.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("backup.policy.set", body);
            else if (g_strcmp0(method, "DELETE") == 0)
                rpc = _build_rpc("backup.policy.delete", body);
        } else if (g_strcmp0(name, "history") == 0) {
            rpc = _build_rpc("backup.history", body);

        } else if (g_strcmp0(name, "incremental") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("backup.incremental", body);
        } else if (g_strcmp0(name, "verify") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("backup.verify", body);
        } else if (g_strcmp0(name, "replicate") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("backup.replicate", body);
        } else if (g_strcmp0(name, "export-s3") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("backup.export_s3", body);
        }
    }

    else if (g_strcmp0(resource, "ovn") == 0) {
        if (g_strcmp0(name, "status") == 0)
            rpc = _build_rpc("ovn.status", NULL);
        else if (g_strcmp0(name, "switches") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("ovn.switch.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("ovn.switch.create", body);
        } else if (g_strcmp0(name, "routers") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("ovn.router.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("ovn.router.create", body);
        } else if (g_strcmp0(name, "acl") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("ovn.acl.list", body);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("ovn.acl.add", body);
        } else if (g_strcmp0(name, "nat") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("ovn.nat.list", body);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("ovn.nat.add", body);
        }
    }

    else if (g_strcmp0(resource, "iso") == 0 && g_strcmp0(method, "GET") == 0) {
        rpc = _build_rpc("iso.list", NULL);
    }

    else if (g_strcmp0(resource, "iscsi") == 0) {
        if (g_strcmp0(name, "targets") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("iscsi.target.list", NULL);
        else if (g_strcmp0(name, "targets") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("iscsi.target.create", body);
        else if (g_strcmp0(name, "targets") == 0 && g_strcmp0(method, "DELETE") == 0)
            rpc = _build_rpc("iscsi.target.delete", body);
        else if (g_strcmp0(name, "connect") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("iscsi.connect", body);
        else if (g_strcmp0(name, "disconnect") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("iscsi.disconnect", body);
    }

    else if (g_strcmp0(resource, "overlay") == 0 && g_strcmp0(method, "GET") == 0) {
        rpc = _build_rpc("overlay.list", NULL);
    }

    else if (g_strcmp0(resource, "auth") == 0) {
        if (g_strcmp0(name, "whoami") == 0 && g_strcmp0(method, "GET") == 0) {

            PcvRole role = (key_role >= 0)
                ? (PcvRole)key_role
                : pcv_rbac_get_role(subject);
            const gchar *role_str = pcv_rbac_role_to_str(role);
            const gchar *tenant = pcv_rbac_get_tenant(subject);
            gchar *resp = g_strdup_printf(
                "{\"data\":{\"username\":\"%s\",\"role\":\"%s\",\"tenant\":\"%s\"}}",
                subject, role_str, tenant ? tenant : "---");
            _send_json(msg, 200, resp);
            g_free(resp);
            goto cleanup;
        } else if (g_strcmp0(name, "users") == 0) {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("auth.user.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("auth.user.create", body);
            else if (g_strcmp0(method, "DELETE") == 0)
                rpc = _build_rpc("auth.user.delete", body);
        } else if (g_strcmp0(name, "role") == 0 && g_strcmp0(method, "POST") == 0) {
            rpc = _build_rpc("auth.role.set", body);
        } else if (g_strcmp0(name, "sessions") == 0
                   && g_strcmp0(action, "revoke") == 0
                   && g_strcmp0(method, "POST") == 0) {

            rpc = _build_rpc("auth.session.revoke", body);
        } else if (g_strcmp0(name, "user-sessions") == 0
                   && g_strcmp0(action, "revoke") == 0
                   && g_strcmp0(method, "POST") == 0) {

            rpc = _build_rpc("auth.user.sessions.revoke", body);
        } else if (g_strcmp0(name, "apikeys") == 0) {

            if (g_strcmp0(action, "") == 0 && g_strcmp0(method, "GET") == 0) {

                rpc = _build_rpc("auth.apikey.list", NULL);
            } else if (g_strcmp0(action, "") == 0 && g_strcmp0(method, "POST") == 0) {

                rpc = _build_rpc("auth.apikey.create", body);
            } else if (g_strcmp0(sub, "revoke") == 0 && g_strcmp0(method, "POST") == 0) {

                JsonObject *p = json_object_new();
                json_object_set_string_member(p, "client_name", action);
                rpc = _build_rpc("auth.apikey.revoke", p);
                json_object_unref(p);
            }
        }
    }

    else if (g_strcmp0(resource, "config") == 0) {
        if (g_strcmp0(name, "backup") == 0) {
            if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("config.backup", body);
            else if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("config.backup", NULL);
        }
        else if (g_strcmp0(name, "history") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("config.history", NULL);

        else if (g_strcmp0(name, "reload") == 0 && g_strcmp0(method, "POST") == 0)
            rpc = _build_rpc("config.reload", NULL);

        else if (g_strcmp0(name, "daemon") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("daemon.config.get", body);

        else if (g_strcmp0(name, "daemon") == 0 && g_strcmp0(method, "PUT") == 0)
            rpc = _build_rpc("daemon.config.set", body);
    }

    else if (g_strcmp0(resource, "templates") == 0) {
        if (g_strcmp0(name, "history") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("template.history", NULL);
        else if (*name == '\0') {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc("template.list", NULL);
            else if (g_strcmp0(method, "POST") == 0)
                rpc = _build_rpc("template.create", body);
        } else if (*name != '\0') {
            if (g_strcmp0(method, "GET") == 0)
                rpc = _build_rpc_name("template.get", name);
            else if (g_strcmp0(method, "DELETE") == 0)
                rpc = _build_rpc_name("template.delete", name);
        }
    }

    else if (g_strcmp0(resource, "gpu") == 0) {
        if (g_strcmp0(name, "list") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("gpu.list", body);
        else if (g_strcmp0(name, "metrics") == 0 && g_strcmp0(method, "GET") == 0)
            rpc = _build_rpc("gpu.metrics", NULL);

    }

    else if (g_strcmp0(resource, "vnc") == 0 && *name != '\0') {
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, "vm_id", name);
        rpc = _build_rpc("get_vnc_info", p);
        json_object_unref(p);
    }

    if (!rpc) {
        _error(msg, 404, "NOT_FOUND",
               "Endpoint not found or method not allowed");
        goto cleanup;
    }

    PCV_LOG_INFO(REST_LOG_DOM, "→ RPC %.100s", rpc);
    {
        /* PCV_SAFETY_CONTROL: apikey-role-enforce — API 키 실효 role은 저장 role 컬럼에서만
         *   파생(client_name 라이브 role 무시), privesc 차단 (SEC-3).
         * apikey caller(key_role>=0)면 키의 저장 role을, 그 외(JWT)면 subject의 라이브 role을
         * 디스패치 role로 주입한다. 이 caller_role이 _pcv_caller_role로 실려 디스패처의
         * 서버측 RBAC 게이트(pcv_dispatcher_check_rbac)에서 집행된다 — client_name이 admin명이고
         * 저장 role이 VIEWER인 키는 VIEWER로 판정되어 admin 메서드가 403으로 거부된다. */
        PcvRole caller_role = (key_role >= 0)
            ? (PcvRole)key_role
            : pcv_rbac_get_role(subject);
        gchar *rpc_with_context = _rpc_attach_auth_context(rpc, subject, caller_role);
        g_free(rpc);
        rpc = rpc_with_context;
    }

    {

        const gchar *mp = strstr(rpc, "\"method\":\"");
        if (!mp) {
            static const char resp_missing_method[] =
                "{\"error\":{\"code\":\"BAD_REQUEST\",\"message\":\"Missing method field\"}}";
            g_message("[RBAC] reject: missing method field in RPC envelope");
            g_free(rpc); rpc = NULL;
            soup_server_message_set_status(msg, 400, "Bad Request");
            soup_server_message_set_response(msg, "application/json",
                SOUP_MEMORY_STATIC,
                resp_missing_method,
                sizeof(resp_missing_method) - 1);
            goto cleanup;
        }
        mp += 10;
        const gchar *me = strchr(mp, '"');
        if (!me || me == mp) {
            static const char resp_malformed_method[] =
                "{\"error\":{\"code\":\"BAD_REQUEST\",\"message\":\"Malformed method field\"}}";
            g_message("[RBAC] reject: malformed method field in RPC envelope");
            g_free(rpc); rpc = NULL;
            soup_server_message_set_status(msg, 400, "Bad Request");
            soup_server_message_set_response(msg, "application/json",
                SOUP_MEMORY_STATIC,
                resp_malformed_method,
                sizeof(resp_malformed_method) - 1);
            goto cleanup;
        }
        {
            gchar *rpc_method = g_strndup(mp, (gsize)(me - mp));
            if (!pcv_rbac_check_permission(subject, rpc_method)) {
                static const char resp_forbidden[] =
                    "{\"error\":{\"code\":\"FORBIDDEN\",\"message\":\"Insufficient permissions\"}}";
                g_message("[RBAC] denied: user=%s method=%s", subject, rpc_method);
                g_free(rpc_method);
                g_free(rpc);
                rpc = NULL;
                soup_server_message_set_status(msg, 403, "Forbidden");
                soup_server_message_set_response(msg, "application/json",
                    SOUP_MEMORY_STATIC,
                    resp_forbidden,
                    sizeof(resp_forbidden) - 1);
                goto cleanup;
            }
            g_free(rpc_method);
        }
    }

    {

        _RestAsyncCtx *actx = g_new0(_RestAsyncCtx, 1);
        actx->msg = g_object_ref(msg);
        actx->rpc = rpc;

        {
            const gchar *mp = rpc ? strstr(rpc, "\"method\":\"") : NULL;
            if (mp) {
                mp += 10;
                const gchar *me = strchr(mp, '"');
                if (me) actx->rpc_method = g_strndup(mp, (gsize)(me - mp));
            }
        }
        actx->vm_delete_name = vm_delete_name;
        actx->is_vm_delete = is_vm_delete;
        actx->rest_ctx = self_rest_ctx;

        soup_server_message_pause(msg);

        GTask *task = g_task_new(NULL, NULL, NULL, NULL);
        g_task_set_task_data(task, actx, _rest_async_ctx_free);
        g_task_run_in_thread(task, _rest_async_worker);
        g_object_unref(task);
    }

    goto cleanup_no_free;

cleanup:
    g_strfreev(segs);
    if (body) json_object_unref(body);
    g_free(subject);
    g_free(rpc);
    g_free(vm_delete_name);
    g_free(req_id);
    pcv_trace_context_free(trace_ctx);
    pcv_log_req_id_set(NULL);
    return;

cleanup_no_free:
    g_strfreev(segs);
    if (body) json_object_unref(body);
    g_free(subject);

    g_free(req_id);
    pcv_trace_context_free(trace_ctx);
    pcv_log_req_id_set(NULL);
}

static void
pcv_rest_server_finalize(GObject *object)
{
    PcvRestServer *self = PCV_REST_SERVER(object);
    if (self->rest_loop) {
        g_main_loop_quit(self->rest_loop);
        if (self->thread) {
            g_thread_join(self->thread);
            self->thread = nullptr;
        }
        g_main_loop_unref(self->rest_loop);
        self->rest_loop = nullptr;
    }
    if (self->rest_ctx) {
        g_main_context_unref(self->rest_ctx);
        self->rest_ctx = nullptr;
    }
    if (self->soup) {
        soup_server_disconnect(self->soup);
        g_object_unref(self->soup);
    }
    G_OBJECT_CLASS(pcv_rest_server_parent_class)->finalize(object);
}

static void pcv_rest_server_class_init(PcvRestServerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = pcv_rest_server_finalize;
}

static void pcv_rest_server_init(PcvRestServer *self)
{
    self->soup       = nullptr;
    self->port       = 8080;
    self->https_port = 443;
    self->tls_active = FALSE;
    self->thread     = nullptr;
    self->rest_loop  = nullptr;
    self->rest_ctx   = nullptr;
}

static gpointer
_rest_thread_func(gpointer data)
{
    PcvRestServer *self = (PcvRestServer *)data;

    g_main_context_push_thread_default(self->rest_ctx);
    g_main_loop_run(self->rest_loop);
    g_main_context_pop_thread_default(self->rest_ctx);
    return NULL;
}

PcvRestServer *
pcv_rest_server_new(PureCVisorDispatcher *dispatcher __attribute__((unused)),
                    guint16 port)
{
    PcvRestServer *self = g_object_new(PCV_TYPE_REST_SERVER, NULL);
    self->port = (port > 0) ? port : (guint16)pcv_config_get_rest_port();
    return self;
}

gboolean
pcv_rest_server_start(PcvRestServer *self, GError **error)
{

    self->rest_ctx  = g_main_context_new();
    self->rest_loop = g_main_loop_new(self->rest_ctx, FALSE);
    self_rest_ctx   = self->rest_ctx;

    g_main_context_push_thread_default(self->rest_ctx);

    self->soup = soup_server_new("server-header", "PureCVisord/1.0", NULL);

    soup_server_add_handler(self->soup, REST_API_PREFIX,
                             _on_request, self, NULL);
    soup_server_add_handler(self->soup, "/ui",
                             _on_request, self, NULL);
    soup_server_add_handler(self->soup, "/",
                             _on_request, self, NULL);

    pcv_ws_server_init(self->soup);

    gboolean mtls_fail_secure = FALSE;
    if (pcv_tls_is_enabled()) {
        const gchar *cert_path = pcv_tls_get_cert_path();
        const gchar *key_path  = pcv_tls_get_key_path();
        if (cert_path && key_path) {
            GError *tls_err = nullptr;
            GTlsCertificate *tls_cert =
                g_tls_certificate_new_from_files(cert_path, key_path, &tls_err);
            if (tls_cert) {
                soup_server_set_tls_certificate(self->soup, tls_cert);
                g_object_unref(tls_cert);
                PCV_LOG_INFO(REST_LOG_DOM, "TLS certificate loaded: %s", cert_path);

                const gchar *client_auth =
                    pcv_config_get_string("tls", "client_auth", "none");
                gboolean want_request = (g_strcmp0(client_auth, "request") == 0);
                gboolean want_require = (g_strcmp0(client_auth, "require") == 0);
                if (want_request || want_require) {
                    const gchar *ca_path = pcv_tls_get_ca_path();
                    if (!ca_path || !*ca_path) {
                        PCV_LOG_ERROR(REST_LOG_DOM,
                            "SECURITY: [tls] client_auth=%s 이지만 ca_path 미설정 — %s",
                            client_auth,
                            want_require
                              ? "mTLS 강제 불가로 HTTPS 미개시(fail-secure)"
                              : "클라이언트 인증서 검증 없이 진행(request)");
                        if (want_require)
                            mtls_fail_secure = TRUE;
                    } else {
                        GError *db_err = nullptr;
                        GTlsDatabase *db = g_tls_file_database_new(ca_path, &db_err);
                        if (db) {
                            soup_server_set_tls_database(self->soup, db);
                            soup_server_set_tls_auth_mode(self->soup,
                                want_require ? G_TLS_AUTHENTICATION_REQUIRED
                                             : G_TLS_AUTHENTICATION_REQUESTED);
                            g_object_unref(db);
                            PCV_LOG_INFO(REST_LOG_DOM,
                                "mTLS 클라이언트 인증서 검증 활성 "
                                "(client_auth=%s, ca=%s)", client_auth, ca_path);
                        } else {
                            PCV_LOG_ERROR(REST_LOG_DOM,
                                "SECURITY: CA 검증 DB 생성 실패 (%s): %s — %s",
                                ca_path, db_err ? db_err->message : "unknown",
                                want_require
                                  ? "mTLS 강제 불가로 HTTPS 미개시(fail-secure)"
                                  : "클라이언트 인증서 검증 없이 진행(request)");
                            if (db_err) g_error_free(db_err);
                            if (want_require)
                                mtls_fail_secure = TRUE;
                        }
                    }
                }
            } else {
                PCV_LOG_WARN(REST_LOG_DOM, "TLS cert load failed: %s — HTTPS disabled",
                             tls_err ? tls_err->message : "unknown");
                if (tls_err) g_error_free(tls_err);
            }
        }
    }

    const gchar *bind_mode = pcv_config_get_string("server", "bind_plaintext", "loopback");
    GError *lerr = nullptr;
    gboolean ok;
    if (g_strcmp0(bind_mode, "all") == 0) {
        ok = soup_server_listen_all(self->soup, self->port,
                                    SOUP_SERVER_LISTEN_IPV4_ONLY,
                                    &lerr);
    } else {
        GInetAddress   *lo   = g_inet_address_new_loopback(AF_INET);
        GSocketAddress *addr = g_inet_socket_address_new(lo, self->port);

        ok = soup_server_listen(self->soup, addr, 0, &lerr);
        g_object_unref(addr);
        g_object_unref(lo);
    }

    {
        GSList *listeners = soup_server_get_listeners(self->soup);
        for (GSList *l = listeners; l; l = l->next) {
            int fd = g_socket_get_fd(G_SOCKET(l->data));
            if (fd >= 0) {
                listen(fd, 1024);
                int optval = 1;
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
            }
        }
        g_slist_free(listeners);
    }
    if (!ok) {
        g_main_context_pop_thread_default(self->rest_ctx);
        g_propagate_error(error, lerr);
        g_main_loop_unref(self->rest_loop);
        g_main_context_unref(self->rest_ctx);
        self->rest_loop = nullptr;
        self->rest_ctx  = nullptr;
        return FALSE;
    }

    if (pcv_tls_is_enabled() && soup_server_get_tls_certificate(self->soup)
        && !mtls_fail_secure) {
        GError *tls_lerr = nullptr;
        gboolean tls_ok = soup_server_listen_all(self->soup, self->https_port,
                                                   SOUP_SERVER_LISTEN_IPV4_ONLY |
                                                   SOUP_SERVER_LISTEN_HTTPS,
                                                   &tls_lerr);
        if (tls_ok) {
            self->tls_active = TRUE;

            {
                const gchar *hsts_cfg = pcv_config_get_string("tls", "hsts", "false");
                if (g_strcmp0(hsts_cfg, "true") == 0 || g_strcmp0(hsts_cfg, "1") == 0)
                    g_hsts_enabled = TRUE;
            }
            PCV_LOG_INFO(REST_LOG_DOM, "HTTPS listening on https://0.0.0.0:%u (HSTS enabled)",
                         self->https_port);

            PCV_LOG_INFO(REST_LOG_DOM, "HTTP/2 support enabled (TLS via ALPN negotiation)");
        } else {
            PCV_LOG_WARN(REST_LOG_DOM, "HTTPS listen failed on port %u: %s",
                         self->https_port,
                         tls_lerr ? tls_lerr->message : "unknown");
            if (tls_lerr) g_error_free(tls_lerr);
        }
    }

    g_main_context_pop_thread_default(self->rest_ctx);

    self->thread = g_thread_new("purecvisor-rest", _rest_thread_func, self);

    const gchar *plain_host = (g_strcmp0(bind_mode, "all") == 0) ? "0.0.0.0" : "127.0.0.1";
    if (self->tls_active) {
        PCV_LOG_INFO(REST_LOG_DOM,
                     "REST API listening on http://%s:%u + https://0.0.0.0:%u%s",
                     plain_host, self->port, self->https_port, REST_API_PREFIX);
    } else {
        PCV_LOG_INFO(REST_LOG_DOM,
                     "REST API listening on http://%s:%u%s (thread: %p)",
                     plain_host, self->port, REST_API_PREFIX, (void *)self->thread);
    }
    return TRUE;
}

void
pcv_rest_server_stop(PcvRestServer *self)
{
    if (self->soup) {
        soup_server_disconnect(self->soup);
    }
    if (self->rest_loop && g_main_loop_is_running(self->rest_loop)) {
        g_main_loop_quit(self->rest_loop);
    }
    if (self->thread) {
        g_thread_join(self->thread);
        self->thread = nullptr;
    }
    PCV_LOG_INFO(REST_LOG_DOM, "REST API server stopped");
}
