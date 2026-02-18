#include "uds_server.h"
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

struct _UdsServer {
    GObject parent_instance;
    GSocketService *service;
    gchar *socket_path;
    PureCVisorDispatcher *dispatcher; 
    guint handler_id;
};

G_DEFINE_TYPE(UdsServer, uds_server, G_TYPE_OBJECT)

static void uds_server_dispose(GObject *obj) {
    UdsServer *self = UDS_SERVER(obj);
    if (self->dispatcher) g_object_unref(self->dispatcher);
    if (self->service) {
        if (self->handler_id) {
            g_signal_handler_disconnect(self->service, self->handler_id);
            self->handler_id = 0;
        }
        g_socket_service_stop(self->service);
        g_object_unref(self->service);
        self->service = NULL;
    }
    g_free(self->socket_path);
    G_OBJECT_CLASS(uds_server_parent_class)->dispose(obj);
}

static void uds_server_class_init(UdsServerClass *klass) {
    G_OBJECT_CLASS(klass)->dispose = uds_server_dispose;
}

static void uds_server_init(UdsServer *self) {}

/* Connection Handler */
static gboolean
on_incoming_connection(GSocketService *service,
                       GSocketConnection *connection,
                       GObject *source_object,
                       gpointer user_data)
{
    UdsServer *self = UDS_SERVER(user_data);
    GInputStream *input;
    GOutputStream *output;
    
    if (!self->dispatcher) {
        g_warning("No dispatcher set for incoming connection.");
        return TRUE; 
    }

    input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    output = g_io_stream_get_output_stream(G_IO_STREAM(connection));

    /* [CRITICAL FIX] 
     * Dispatcher가 비동기 작업을 수행하는 동안 Connection 객체가 소멸되지 않도록
     * Output Stream에 Connection의 참조를 묶어둡니다 (Keep-Alive).
     * Dispatcher가 작업을 마치고 output stream을 unref하면, connection도 같이 unref 됩니다.
     */
    g_object_set_data_full(G_OBJECT(output), 
                           "keep_alive_connection", 
                           g_object_ref(connection), 
                           g_object_unref);

    /* * JSON 읽기 (단순화를 위해 4KB 버퍼 사용)
     * 실제 프로덕션에서는 GDataInputStream으로 라인 단위 읽기를 권장하지만
     * 테스트 목적상 read_all 사용. socat이 EOF(종료)를 보내야 read_all이 리턴됩니다.
     */
    gchar buffer[4096] = {0};
    gsize bytes_read = 0;
    GError *error = NULL;

    // g_input_stream_read_all은 EOF를 만날 때까지 블로킹됩니다.
    // 클라이언트가 데이터를 보내고 close(write)를 해야 리턴됩니다.
    if (g_input_stream_read_all(input, buffer, sizeof(buffer) - 1, &bytes_read, NULL, &error)) {
        if (bytes_read > 0) {
            JsonParser *parser = json_parser_new();
            if (json_parser_load_from_data(parser, buffer, bytes_read, NULL)) {
                JsonNode *root = json_parser_get_root(parser);
                purecvisor_dispatcher_dispatch(self->dispatcher, root, output);
            } else {
                g_warning("Failed to parse JSON request");
            }
            g_object_unref(parser);
        }
    }
    
    if (error) {
        g_warning("Socket read error: %s", error->message);
        g_error_free(error);
    }
    
    return TRUE; // 리스너 유지
}

UdsServer *uds_server_new(const gchar *socket_path) {
    UdsServer *self = g_object_new(UDS_TYPE_SERVER, NULL);
    self->socket_path = g_strdup(socket_path);
    return self;
}

void uds_server_set_dispatcher(UdsServer *self, PureCVisorDispatcher *dispatcher) {
    if (self->dispatcher) g_object_unref(self->dispatcher);
    self->dispatcher = dispatcher ? g_object_ref(dispatcher) : NULL;
}

gboolean uds_server_start(UdsServer *self, GError **error) {
    if (g_file_test(self->socket_path, G_FILE_TEST_EXISTS)) {
        unlink(self->socket_path);
    }

    self->service = g_socket_service_new();
    GSocketAddress *addr = g_unix_socket_address_new(self->socket_path);
    
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(self->service),
                                       addr,
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL, NULL, error)) {
        g_object_unref(addr);
        return FALSE;
    }
    g_object_unref(addr);

    self->handler_id = g_signal_connect(self->service, "incoming",
                                        G_CALLBACK(on_incoming_connection), self);
    g_socket_service_start(self->service);
    return TRUE;
}

void uds_server_stop(UdsServer *self) {
    if (self->service) g_socket_service_stop(self->service);
}