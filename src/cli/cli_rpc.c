
#include "cli_rpc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

PcvCtx g_ctx = {
    .fmt         = FMT_TABLE,
    .interactive = false,
    .batch       = false,
    .no_color    = false,
    .verbose     = false,
    .socket_path = DAEMON_SOCK_PATH,
};

const char *cc(const char *code) {
    if (g_ctx.no_color || !isatty(STDOUT_FILENO)) return "";
    return code;
}

const char *ce(const char *code) {
    if (g_ctx.no_color || !isatty(STDERR_FILENO)) return "";
    return code;
}

gchar *purectl_send_request(const gchar *method,
                            JsonObject  *params_obj,
                            GError     **error) {

    GSocketClient    *client = g_socket_client_new();
    GSocketAddress   *addr   = g_unix_socket_address_new(g_ctx.socket_path);
    GSocketConnection *conn  = g_socket_client_connect(
            client, G_SOCKET_CONNECTABLE(addr), NULL, error);

    g_object_unref(client);
    g_object_unref(addr);
    if (!conn) return NULL;

    GSocket *sock = g_socket_connection_get_socket(conn);
    g_socket_set_timeout(sock, 10);

    JsonObject *root_obj = json_object_new();
    json_object_set_string_member(root_obj, "jsonrpc", "2.0");
    json_object_set_string_member(root_obj, "method",  method);
    json_object_set_object_member(root_obj, "params",
            params_obj ? params_obj : json_object_new());
    json_object_set_int_member(root_obj, "id", 1);

    JsonNode      *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, root_obj);
    gchar *payload = json_to_string(root_node, FALSE);
    json_node_free(root_node);

    if (g_ctx.verbose) {

        gchar *masked = g_strdup(payload);
        static const char *sensitive_keys[] = {
            "password", "secret", "token", "api_key", "apikey",
            "jwt_secret", "refresh_token", "access_token", NULL
        };
        for (int i = 0; sensitive_keys[i]; i++) {
            gchar *needle = g_strdup_printf("\"%s\":\"", sensitive_keys[i]);
            gchar *found = strstr(masked, needle);
            if (found) {
                gchar *val_start = found + strlen(needle);
                gchar *val_end = strchr(val_start, '"');
                if (val_end) {
                    gsize val_len = (gsize)(val_end - val_start);
                    memset(val_start, '*', val_len > 4 ? 4 : val_len);
                    if (val_len > 4) memmove(val_start + 4, val_end, strlen(val_end) + 1);
                }
            }
            g_free(needle);
        }
        g_printerr("%s[→ %s]%s\n", ce(CYBER_DIM), masked, ce(CYBER_RESET));
        g_free(masked);
    }

    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    gsize bytes_written;
    if (!g_output_stream_write_all(out, payload, strlen(payload),
                                   &bytes_written, NULL, error)) {
        g_free(payload);
        g_object_unref(conn);
        return NULL;
    }
    g_free(payload);

    if (!g_socket_shutdown(sock, FALSE, TRUE, error)) {
        g_object_unref(conn);
        return NULL;
    }

    GInputStream *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GByteArray   *buf = g_byte_array_new();
    gchar         tmp[8192];
    gssize        n;
    while ((n = g_input_stream_read(in, tmp, sizeof(tmp), NULL, error)) > 0)
        g_byte_array_append(buf, (guint8 *)tmp, (guint)n);

    g_object_unref(conn);

    if (buf->len == 0) {
        g_byte_array_free(buf, TRUE);
        return NULL;
    }
    g_byte_array_append(buf, (guint8 *)"\0", 1);
    gchar *result = g_strdup((gchar *)buf->data);
    g_byte_array_free(buf, TRUE);

    if (g_ctx.verbose)
        g_printerr("%s[← %s]%s\n", ce(CYBER_DIM), result, ce(CYBER_RESET));

    if (result && strstr(result, "-32601")) {
        g_printerr("\n%s[!] This command is not included in Single Edge.%s\n"
                   "%s    Current daemon does not support '%s' RPC method.%s\n"
                   "%s    Use the appropriate edition repository for this feature.%s\n\n",
            ce(CYBER_YELLOW), ce(CYBER_RESET),
            ce(CYBER_DIM), method, ce(CYBER_RESET),
            ce(CYBER_DIM), ce(CYBER_RESET));
        g_free(result);
        return NULL;
    }

    return result;
}
