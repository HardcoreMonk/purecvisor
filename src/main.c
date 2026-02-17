/* src/main.c */
#include <glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <signal.h>
#include <stdio.h>

#include "api/uds_server.h"
#include "modules/virt/vm_manager.h"

static GMainLoop *loop;

/* Signal Handler */
static void on_signal(int signo) {
    if (loop && g_main_loop_is_running(loop)) {
        g_print("\nReceived signal %d, quitting...\n", signo);
        g_main_loop_quit(loop);
    }
}

/* Libvirt Connection Callback */
static void on_connection_open(GObject *source, GAsyncResult *res, gpointer user_data) {
    GVirConnection *conn = GVIR_CONNECTION(source);
    /* [FIX] 올바른 캐스팅 매크로 사용 */
    UdsServer *server = PURECVISOR_UDS_SERVER(user_data);    
    GError *error = NULL;

    if (!gvir_connection_open_finish(conn, res, &error)) {
        g_printerr("Failed to connect to Hypervisor: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(loop);
        return;
    }

    g_print("Connected to Hypervisor (QEMU/KVM).\n");

    /* [FIX] Phase 3: Create Manager WITH Connection */
    VmManager *vm_mgr = purecvisor_vm_manager_new(conn);

    /* Inject Manager into Server */
    uds_server_set_vm_manager(server, vm_mgr);

    /* Start Server */
    GError *srv_err = NULL;
    if (!uds_server_start(server, &srv_err)) {
        g_printerr("Failed to start UDS Server: %s\n", srv_err->message);
        g_error_free(srv_err);
        g_main_loop_quit(loop);
    }

    /* Cleanup (Manager is ref-ed by Server now) */
    g_object_unref(vm_mgr);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // g_type_init(); // Deprecated 
    loop = g_main_loop_new(NULL, FALSE);

    /* 1. Create Libvirt Connection First */
    GVirConnection *conn = gvir_connection_new("qemu:///system");

    /* 2. Create Server (Wait for connection before starting) */
    UdsServer *server = uds_server_new("/tmp/purecvisor.sock");

    /* 3. Open Connection Async */
    gvir_connection_open_async(conn, NULL, on_connection_open, server);

    g_print("PureCVisor Engine Started. Connecting to Hypervisor...\n");
    g_main_loop_run(loop);

    /* Cleanup */
    g_object_unref(conn);
    g_object_unref(server);
    g_main_loop_unref(loop);

    return 0;
}