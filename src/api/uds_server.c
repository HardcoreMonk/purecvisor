/* src/api/uds_server.c */
#include "uds_server.h"
#include "dispatcher.h"
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <stdio.h>

struct _UdsServer {
    GObject parent_instance;
    GSocketService *service;
    gchar *socket_path;
    Dispatcher *dispatcher;
    gulong connection_id;
};

/* * [중요] G_DEFINE_TYPE 매크로:
 * 두 번째 인자 'purecvisor_uds_server'는 다음 함수들을 자동 생성하거나 요구합니다:
 * 1. purecvisor_uds_server_get_type() (생성됨)
 * 2. purecvisor_uds_server_init() (우리가 구현해야 함)
 * 3. purecvisor_uds_server_class_init() (우리가 구현해야 함)
 */
G_DEFINE_TYPE(UdsServer, purecvisor_uds_server, G_TYPE_OBJECT)

static void uds_server_dispose(GObject *obj) {
    UdsServer *self = PURECVISOR_UDS_SERVER(obj);
    if (self->service) {
        g_socket_service_stop(self->service);
        g_object_unref(self->service);
        self->service = NULL;
    }
    if (self->dispatcher) {
        dispatcher_free(self->dispatcher);
        self->dispatcher = NULL;
    }
    g_free(self->socket_path);
    
    G_OBJECT_CLASS(purecvisor_uds_server_parent_class)->dispose(obj);
}

/* [규칙 준수] purecvisor_uds_server_class_init */
static void purecvisor_uds_server_class_init(UdsServerClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = uds_server_dispose;
}

/* [규칙 준수] purecvisor_uds_server_init */
static void purecvisor_uds_server_init(UdsServer *self) {
    self->service = NULL;
    self->dispatcher = dispatcher_new();
}

// UdsServer *uds_server_new(const gchar *socket_path) {
//     return g_object_new(PURECVISOR_TYPE_UDS_SERVER, NULL);
// }

/* uds_server_new 수정 버전 (위 코드 파일에 덮어쓰기) */
UdsServer *uds_server_new(const gchar *socket_path) {
    UdsServer *self = g_object_new(PURECVISOR_TYPE_UDS_SERVER, NULL);
    /* 구조체 멤버에 직접 할당 (private이 아니므로 가능) */
    self->socket_path = g_strdup(socket_path); 
    return self;
}

void uds_server_set_vm_manager(UdsServer *self, VmManager *mgr) {
    if (self->dispatcher) {
        dispatcher_set_vm_manager(self->dispatcher, mgr);
    }
}

/* --- Connection Handling --- */

static gboolean on_incoming_connection(GSocketService *service,
                                       GSocketConnection *connection,
                                       GObject *source_object,
                                       gpointer user_data) {
    UdsServer *self = PURECVISOR_UDS_SERVER(user_data);
    GIOStream *stream = G_IO_STREAM(connection);
    GInputStream *input = g_io_stream_get_input_stream(stream);
    
    /* [DEBUG] Connection Accepted */
    // g_print("[DEBUG] Connection accepted.\n");

    /* Create Data Input Stream */
    GDataInputStream *dis = g_data_input_stream_new(input);
    
    /* [FIX] 중요: DIS가 소멸될 때 기본 스트림(소켓)을 닫지 않도록 설정 */
    g_filter_input_stream_set_close_base_stream(G_FILTER_INPUT_STREAM(dis), FALSE);
    
    GError *error = NULL;
    (void)service; (void)source_object;

    /* Read Line */
    gsize length;
    gchar *line = g_data_input_stream_read_line(dis, &length, NULL, &error);
    
    if (line) {
        g_print("[RPC] Received: %s\n", line);
        
        /* Dispatcher에게 스트림 전달 (비동기 응답을 위해) */
        /* Dispatcher 내부에서 stream의 Output Stream을 Ref하므로 소켓은 유지됨 */
        dispatcher_process_line(self->dispatcher, stream, line);
        g_free(line);
    } else {
        if (error) {
            g_warning("Read error: %s", error->message);
            g_error_free(error);
        }
    }

    /* 이제 dis를 unref 해도 소켓은 닫히지 않음 */
    g_object_unref(dis);
    
    /* TRUE를 리턴하여 핸들링 완료를 알림 (권장) */
    return TRUE; 
}

/* --- Start/Stop --- */

gboolean uds_server_start(UdsServer *self, GError **error) {
    g_return_val_if_fail(PURECVISOR_IS_UDS_SERVER(self), FALSE);

    /* Socket path assignment moved here or ensured in new() */
    if (!self->socket_path) {
        /* If new() didn't set it (due to g_object_new limitation without properties), set default or fix new() */
        /* Let's fix new() properly below, but for now fallback */
        self->socket_path = g_strdup("/tmp/purecvisor.sock"); 
    }

    if (g_file_test(self->socket_path, G_FILE_TEST_EXISTS)) {
        unlink(self->socket_path);
    }

    self->service = g_socket_service_new();
    GSocketAddress *addr = g_unix_socket_address_new(self->socket_path);

    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(self->service),
                                       addr, /* No cast needed now */
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL,
                                       NULL,
                                       error)) {
        g_object_unref(addr);
        return FALSE;
    }
    g_object_unref(addr);

    self->connection_id = g_signal_connect(self->service, "incoming",
                                           G_CALLBACK(on_incoming_connection), self);
    
    g_socket_service_start(self->service);
    g_print("UDS Server listening on %s\n", self->socket_path);
    
    return TRUE;
}

void uds_server_stop(UdsServer *self) {
    if (self->service) {
        g_socket_service_stop(self->service);
    }
}