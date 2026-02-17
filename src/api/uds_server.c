#include "purecvisor/server.h"
#include <glib/gstdio.h>

struct UdsServer {
    GSocketService *service;
    char *socket_path;
};

/* 비동기 읽기 완료 콜백 */
static void on_client_read_line(GObject *source, GAsyncResult *res, gpointer user_data) {
    GDataInputStream *dis = G_DATA_INPUT_STREAM(source);
    GSocketConnection *conn = G_SOCKET_CONNECTION(user_data);
    g_autoptr(GError) error = NULL;
    gsize len;

    char *line = g_data_input_stream_read_line_finish(dis, res, &len, &error);
    if (line) {
        g_message("[API/Server] Recv: %s", line);
        
        // Echo Response
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        g_output_stream_write_all(out, "{\"status\":\"ok\"}\n", 16, NULL, NULL, NULL);

        g_free(line);
        // 다음 라인 대기 (Loop)
        g_data_input_stream_read_line_async(dis, G_PRIORITY_DEFAULT, NULL, on_client_read_line, conn);
    } else {
        // 연결 종료
        g_object_unref(conn);
    }
}

/* 새 연결 수락 콜백 */
static gboolean on_incoming(GSocketService *service, GSocketConnection *conn, GObject *source, gpointer user_data) {
    (void)service; (void)source; (void)user_data;
    
    g_info("[API/Server] New Connection Accepted");

    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GDataInputStream *dis = g_data_input_stream_new(in);
    
    g_object_ref(conn); // Keep alive
    g_data_input_stream_read_line_async(dis, G_PRIORITY_DEFAULT, NULL, on_client_read_line, conn);
    g_object_unref(dis);
    
    return FALSE;
}

UdsServer* uds_server_new(const char *socket_path) {
    UdsServer *s = g_new0(UdsServer, 1);
    s->socket_path = g_strdup(socket_path);
    s->service = g_socket_service_new();
    return s;
}

bool uds_server_start(UdsServer *self, GError **error) {
    g_unlink(self->socket_path); // Phase 0: Cleanup old socket
    
    GSocketAddress *addr = g_unix_socket_address_new(self->socket_path);
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(self->service), addr, 
                                       G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, 
                                       NULL, NULL, error)) {
        g_object_unref(addr);
        return false;
    }
    g_object_unref(addr);
    
    g_signal_connect(self->service, "incoming", G_CALLBACK(on_incoming), self);
    g_socket_service_start(self->service);
    
    g_info("[API/Server] Listening on %s", self->socket_path);
    return true;
}

void uds_server_stop(UdsServer *self) {
    if (!self) return;
    if (self->service) { g_socket_service_stop(self->service); g_object_unref(self->service); }
    if (self->socket_path) { g_unlink(self->socket_path); g_free(self->socket_path); }
    g_free(self);
}