/* src/main.c */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "api/uds_server.h"
#include "modules/virt/vm_manager.h" 

static GMainLoop *loop = NULL;

static void signal_handler(int signo) {
    if (loop && g_main_loop_is_running(loop)) {
        g_message("Caught signal %d, stopping...", signo);
        g_main_loop_quit(loop);
    }
}

static void on_hypervisor_connected(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source; // source is NULL (safe to ignore)
    (void)user_data;
    
    // vm_manager_connect_finish는 첫 인자를 쓰지 않으므로 NULL 전달해도 안전
    GError *error = NULL;
    if (vm_manager_connect_finish(NULL, res, &error)) {
        g_message("[Success] Connected to Hypervisor (QEMU/KVM).");
    } else {
        g_warning("[Failed] Could not connect to Hypervisor: %s", error->message);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    g_message("Starting PureCVisor Engine (Phase 2)...");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    loop = g_main_loop_new(NULL, FALSE);

    VmManager *vm_mgr = vm_manager_new();
    UdsServer *server = uds_server_new("/tmp/purecvisor.sock");
    if (!server) return 1;

    uds_server_set_vm_manager(server, vm_mgr);

    g_message("Connecting to qemu:///system ...");
    vm_manager_connect_async(vm_mgr, on_hypervisor_connected, NULL);

    g_message("Engine is running. Listening on /tmp/purecvisor.sock");
    g_main_loop_run(loop);

    g_message("Engine stopping...");
    uds_server_free(server);
    vm_manager_free(vm_mgr);
    g_main_loop_unref(loop);

    return 0;
}