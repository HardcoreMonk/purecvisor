/* src/api/uds_server.c */
#include "uds_server.h"
#include "dispatcher.h"
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <stdio.h>

struct _UdsServer {
    GSocketService *service;
    Dispatcher *dispatcher;
    guint16 handler_id;
};

// 클라이언트 연결 컨텍스트
typedef struct {
    UdsServer *server;
    GIOStream *connection;
    GDataInputStream *data_in;
} ClientContext;

static void client_context_free(ClientContext *ctx) {
    if (ctx->data_in) g_object_unref(ctx->data_in);
    if (ctx->connection) g_object_unref(ctx->connection);
    g_free(ctx);
}

// 비동기 읽기 콜백
static void on_line_read(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GDataInputStream *stream = G_DATA_INPUT_STREAM(source_object);
    ClientContext *ctx = (ClientContext *)user_data;
    GError *error = NULL;
    gsize length;

    gchar *line = g_data_input_stream_read_line_finish(stream, res, &length, &error);

    if (line) {
        // [핵심] Dispatcher에게 처리 위임
        dispatcher_process_line(ctx->server->dispatcher, ctx->connection, line);
        g_free(line);

        // 다음 줄 읽기 (Loop)
        g_data_input_stream_read_line_async(ctx->data_in, G_PRIORITY_DEFAULT, NULL, on_line_read, ctx);
    } else {
        // 연결 종료
        if (error) {
            g_warning("Client read error: %s", error->message);
            g_error_free(error);
        }
        client_context_free(ctx);
    }
}

static gboolean on_incoming_connection(GSocketService *service,
                                       GSocketConnection *connection,
                                       GObject *source_object,
                                       gpointer user_data) {
    UdsServer *self = (UdsServer *)user_data;
    (void)service;
    (void)source_object;

    // g_message("New client connected.");

    ClientContext *ctx = g_new0(ClientContext, 1);
    ctx->server = self;
    ctx->connection = G_IO_STREAM(g_object_ref(connection));
    
    // Line-based 처리를 위한 DataInputStream 래핑
    GInputStream *base_in = g_io_stream_get_input_stream(ctx->connection);
    ctx->data_in = g_data_input_stream_new(base_in);

    // 읽기 시작
    g_data_input_stream_read_line_async(ctx->data_in, G_PRIORITY_DEFAULT, NULL, on_line_read, ctx);

    return TRUE;
}

UdsServer* uds_server_new(const char *socket_path) {
    UdsServer *server = g_new0(UdsServer, 1);
    server->service = g_socket_service_new();
    server->dispatcher = dispatcher_new(); // Dispatcher 초기화

    GError *error = NULL;
    GSocketAddress *addr = g_unix_socket_address_new(socket_path);

    unlink(socket_path); // 기존 소켓 파일 제거

    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(server->service),
                                       addr,
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL, NULL, &error)) {
        g_error("Failed to bind socket: %s", error->message);
        g_object_unref(addr);
        g_free(server);
        return NULL;
    }
    g_object_unref(addr);

    server->handler_id = g_signal_connect(server->service, "incoming",
                                          G_CALLBACK(on_incoming_connection), server);
    
    g_socket_service_start(server->service);
    g_message("UDS Server listening on %s", socket_path);
    return server;
}

void uds_server_free(UdsServer *server) {
    if (!server) return;
    if (server->service) {
        g_socket_service_stop(server->service);
        g_object_unref(server->service);
    }
    if (server->dispatcher) {
        dispatcher_free(server->dispatcher);
    }
    g_free(server);
}