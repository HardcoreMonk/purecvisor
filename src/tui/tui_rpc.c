






































#include "tui_rpc.h"

#include <stdio.h>
#include <string.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
















const gchar *safe_str(JsonObject *obj, const gchar *key, const gchar *def) {
    if (!obj || !key || !json_object_has_member(obj, key)) return def;
    JsonNode *n = json_object_get_member(obj, key);
    if (!n || JSON_NODE_TYPE(n) != JSON_NODE_VALUE) return def;
    const gchar *v = json_node_get_string(n);
    return v ? v : def;
}




double safe_double(JsonObject *obj, const gchar *key) {
    if (!obj || !json_object_has_member(obj, key)) return 0.0;
    return json_object_get_double_member(obj, key);
}




gint64 safe_int(JsonObject *obj, const gchar *key) {
    if (!obj || !json_object_has_member(obj, key)) return 0;
    return json_object_get_int_member(obj, key);
}








void format_bytes(unsigned long long b, char *out, int sz) {
    if      (b >= (1ULL<<30)) snprintf(out, sz, "%.1fG", (double)b/(1<<30));
    else if (b >= (1ULL<<20)) snprintf(out, sz, "%.1fM", (double)b/(1<<20));
    else if (b >= (1ULL<<10)) snprintf(out, sz, "%.1fK", (double)b/(1<<10));
    else                      snprintf(out, sz, "%lluB",  b);
}























gchar *tui_send_request(const gchar *method, JsonObject *params, GError **error) {
    GSocketClient  *client = g_socket_client_new();
    GSocketAddress *addr   = g_unix_socket_address_new(SOCKET_PATH);
    g_socket_client_set_timeout(client, SOCK_TIMEOUT_S);

    GSocketConnection *conn = g_socket_client_connect(
        client, G_SOCKET_CONNECTABLE(addr), NULL, error);
    if (!conn) {
        g_object_unref(client); g_object_unref(addr); return NULL;
    }

    JsonObject *req = json_object_new();
    json_object_set_string_member(req, "jsonrpc", "2.0");
    json_object_set_string_member(req, "method",  method);
    json_object_set_object_member(req, "params",  params ? params : json_object_new());
    json_object_set_string_member(req, "id",      "tui-req");
    JsonNode      *root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, req);
    gchar *data = json_to_string(root, FALSE);
    gsize  len  = strlen(data);

    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));

    gchar *resp = NULL;
    if (!g_output_stream_write_all(out, data, len, NULL, NULL, error) ||
        !g_output_stream_write_all(out, "\n",  1,  NULL, NULL, error)) {
        goto out_cleanup;
    }

    GInputStream     *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GDataInputStream *din = g_data_input_stream_new(in);
    resp = g_data_input_stream_read_line(din, NULL, NULL, error);
    g_object_unref(din);
out_cleanup:

    g_free(data);
    json_node_free(root);
    g_object_unref(conn);
    g_object_unref(addr); g_object_unref(client);
    return resp;
}
