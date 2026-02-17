#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include "purecvisor/storage.h"
#include "purecvisor/server.h"

/* 전역 컨텍스트 (Phase 0) */
typedef struct {
    GMainLoop *loop;
    StorageDriver *storage;
    UdsServer *api;
} AppContext;

/* Signal Handler (Phase 0: Graceful Shutdown) */
static gboolean on_signal(gpointer user_data) {
    AppContext *app = (AppContext *)user_data;
    g_message("Caught signal, shutting down...");
    g_main_loop_quit(app->loop);
    return FALSE; // Remove source
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    AppContext app = {0};
    GError *err = NULL;

    // 1. Setup Phase 0 (Logging & MainLoop)
    g_log_set_writer_func(g_log_writer_standard_streams, NULL, NULL);
    app.loop = g_main_loop_new(NULL, FALSE);

    // 2. Setup Phase 0 (Signal Handling)
    g_unix_signal_add(SIGINT, on_signal, &app);
    g_unix_signal_add(SIGTERM, on_signal, &app);

    // 3. Init Phase 1 (Storage)
    g_message("Initializing Storage Subsystem...");
    app.storage = storage_driver_new_zfs("tank_pool");
    
    // Test: 가상 볼륨 생성 (부팅 시 초기화 로직 시뮬레이션)
    storage_create_vol(app.storage, "boot_volume", 2048);

    // 4. Init Phase 1 (API Server)
    g_message("Starting API Server...");
    app.api = uds_server_new("/tmp/purecvisor.sock");
    
    if (!uds_server_start(app.api, &err)) {
        g_error("Failed to start server: %s", err->message);
        return 1;
    }

    // 5. Run Loop (Blocking)
    g_message(">> PureCVisor Engine Phase 1 Ready <<");
    g_main_loop_run(app.loop);

    // 6. Cleanup (Phase 0)
    g_message("Cleaning up resources...");
    uds_server_stop(app.api);
    storage_destroy(app.storage);
    g_main_loop_unref(app.loop);

    g_message("Bye.");
    return 0;
}