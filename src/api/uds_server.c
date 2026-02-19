/* src/api/uds_server.c */

#include "uds_server.h"
#include "dispatcher.h" // Dispatcher 함수 호출용
#include <gio/gio.h>
#include <sys/stat.h>  // <--- [추가] chmod 함수 정의 포함
#include <glib.h>

struct _UdsServer {
    GObject parent_instance;
    GSocketService *service;
    gchar *socket_path;
    PureCVisorDispatcher *dispatcher;
    guint16 connection_count;
};

G_DEFINE_TYPE(UdsServer, uds_server, G_TYPE_OBJECT)

static void uds_server_finalize(GObject *object) {
    UdsServer *self = PURECVISOR_UDS_SERVER(object);
    if (self->service) {
        g_socket_service_stop(self->service);
        g_object_unref(self->service);
    }
    g_free(self->socket_path);
    if (self->dispatcher) g_object_unref(self->dispatcher);
    
    G_OBJECT_CLASS(uds_server_parent_class)->finalize(object);
}

static void uds_server_class_init(UdsServerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = uds_server_finalize;
}

static void uds_server_init(UdsServer *self) {
    self->service = NULL;
    self->socket_path = NULL;
    self->dispatcher = NULL;
    self->connection_count = 0;
}

/* 들어오는 연결 처리 (비동기) */
static gboolean on_incoming_connection(GSocketService *service,
                                       GSocketConnection *connection,
                                       GObject *source_object,
                                       gpointer user_data) {
    UdsServer *self = PURECVISOR_UDS_SERVER(user_data);
    GInputStream *input;
    gchar buffer[4096];
    gssize bytes_read;
    GError *error = NULL;

    (void)service;
    (void)source_object;

    // 소켓 연결 유지 (Dispatcher 비동기 처리를 위해 ref)
    g_object_ref(connection);

    input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    
    // 단순화: 4KB 버퍼로 한 번 읽음 (실제 프로덕션에선 Loop/Line-reader 필요)
    bytes_read = g_input_stream_read(input, buffer, sizeof(buffer) - 1, NULL, &error);

    if (bytes_read > 0) {
        buffer[bytes_read] = '\0'; // Null-terminate
        
        if (self->dispatcher) {
            // [Phase 5 Fix] JSON 파싱 없이 Raw String을 Dispatcher로 전달
            purecvisor_dispatcher_dispatch(self->dispatcher, self, connection, buffer);
        } else {
            g_warning("No dispatcher set for UdsServer");
        }
    } else if (bytes_read < 0) {
        g_warning("Read error: %s", error->message);
        g_error_free(error);
    }

    // Dispatcher가 비동기 작업 후 connection을 사용하므로 여기서 닫지 않음.
    // 대신 Dispatcher나 Callback에서 작업 완료 후 unref/close 해야 함.
    // 하지만 현재 구조상 Dispatcher_dispatch 호출 후 즉시 리턴하므로, 
    // Dispatcher가 Connection의 소유권을 가져가야 함 (Ref 유지).
    // 여기서는 on_incoming_connection이 TRUE를 반환하면 연결이 유지됨.
    
    // 임시: Dispatcher 내부에서 Connection Ref를 관리한다고 가정하고 여기선 Unref
    g_object_unref(connection); 

    return TRUE; // 계속 리스닝
}

UdsServer *uds_server_new(const gchar *socket_path) {
    UdsServer *self = g_object_new(PURECVISOR_TYPE_UDS_SERVER, NULL);
    self->socket_path = g_strdup(socket_path);
    return self;
}

void uds_server_set_dispatcher(UdsServer *self, PureCVisorDispatcher *dispatcher) {
    if (self->dispatcher) g_object_unref(self->dispatcher);
    self->dispatcher = g_object_ref(dispatcher);
}

gboolean uds_server_start(UdsServer *self, GError **error) {
    GSocketAddress *address;
    GError *err = NULL;

    // 기존 소켓 파일 삭제
    if (g_file_test(self->socket_path, G_FILE_TEST_EXISTS)) {
        unlink(self->socket_path);
    }

    self->service = g_socket_service_new();
    address = g_unix_socket_address_new(self->socket_path);

    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(self->service),
                                       address,
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL, // Object
                                       NULL, // Effective Address
                                       &err)) {
        g_propagate_error(error, err);
        g_object_unref(address);
        return FALSE;
    }

    g_object_unref(address);

    // 시그널 연결
    g_signal_connect(self->service, "incoming", G_CALLBACK(on_incoming_connection), self);

    g_socket_service_start(self->service);
    g_message("UDS Server listening on %s", self->socket_path);
    
    // 권한 설정 (누구나 접근 가능 - 개발용)
    chmod(self->socket_path, 0666);

    return TRUE;
}

void uds_server_stop(UdsServer *self) {
    if (self->service)
        g_socket_service_stop(self->service);
}

void pure_uds_server_send_response(UdsServer *self, GSocketConnection *connection, const gchar *response) {
    (void)self;
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GError *error = NULL;

    if (!g_output_stream_write_all(output, response, strlen(response), NULL, NULL, &error)) {
        g_warning("Failed to send response: %s", error->message);
        g_error_free(error);
    }
    
    // 응답 전송 후 연결 종료 (Short-lived connection model)
    // 실제 RPC에서는 Keep-Alive를 쓰기도 하지만, 여기서는 요청-응답-종료로 단순화
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
}