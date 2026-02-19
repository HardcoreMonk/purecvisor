/* src/main.c */

#include <glib.h>
#include <glib-unix.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <stdio.h>

#include "api/uds_server.h"
#include "api/dispatcher.h"
#include "utils/logger.h"

#define SOCKET_PATH "/tmp/purecvisor.sock"

static GMainLoop *loop;

static gboolean on_signal_received(gpointer user_data) {
    (void)user_data;
    g_message("Signal received, stopping...");
    g_main_loop_quit(loop);
    return FALSE;
}

int main(int argc, char *argv[]) {
    GError *error = NULL;

    // 1. Logger & Type System Init
    purecvisor_logger_init();
    
    #if !GLIB_CHECK_VERSION(2, 36, 0)
    g_type_init();
    #endif

    gvir_init_object(&argc, &argv);

    g_message("Starting PureCVisor Engine (Phase 5)...");

    // 2. Libvirt Connection
    // 로컬 시스템 KVM 연결 (qemu:///system)
    GVirConnection *conn = gvir_connection_new("qemu:///system");
    if (!gvir_connection_open(conn, NULL, &error)) {
        g_critical("Failed to connect to libvirt: %s", error->message);
        g_error_free(error);
        return 1;
    }

    // 3. Components Init
    // Dispatcher 생성 (Connection 주입 X -> SetConnection 사용하거나 생성자 변경)
    // 현재 dispatcher_new는 인자가 없으므로, 생성 후 Connection 설정
    PureCVisorDispatcher *dispatcher = purecvisor_dispatcher_new();
    purecvisor_dispatcher_set_connection(dispatcher, conn);

    // UDS Server 생성
    UdsServer *server = uds_server_new(SOCKET_PATH);
    
    // Dispatcher 연결
    uds_server_set_dispatcher(server, dispatcher);

    // 4. Start Server
    if (!uds_server_start(server, &error)) {
        g_critical("Failed to start UDS server: %s", error->message);
        g_error_free(error);
        return 1;
    }

    // 5. Main Loop
    loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_signal_received, NULL);
    g_unix_signal_add(SIGTERM, on_signal_received, NULL);

    g_message("Daemon is running. Waiting for requests...");
    g_main_loop_run(loop);

    // 6. Cleanup
    g_object_unref(server);
    g_object_unref(dispatcher);
    g_object_unref(conn);
    g_main_loop_unref(loop);

    return 0;
}