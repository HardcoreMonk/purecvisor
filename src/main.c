#include <glib.h>
#include <glib-unix.h>
#include <locale.h>
#include <stdlib.h>
#include <stdio.h>

#include "api/uds_server.h"
#include "api/dispatcher.h"
#include "utils/logger.h"
#include <libvirt-gobject/libvirt-gobject.h>

#include "api/dispatcher.h"
#include "api/uds_server.h"

#define SOCKET_PATH "/tmp/purecvisor.sock"

/* Global Main Loop for Signal Handling */
static GMainLoop *loop = NULL;

/* ------------------------------------------------------------------------
 * Signal Handler (Ctrl+C, SIGTERM)
 * ------------------------------------------------------------------------ */
static gboolean
on_signal_received(gpointer user_data)
{
    g_info("Received signal, shutting down...");
    if (loop) g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[])
{
    GError *error = NULL;
    PureCVisorDispatcher *dispatcher = NULL;
    UdsServer *server = NULL;

    setlocale(LC_ALL, "");
    purecvisor_logger_init();
    g_info("PureCVisor Engine (Phase 4) Starting...");

    /* [CRITICAL FIX] gvir_init_object returns void in newer versions */
    gvir_init_object(&argc, &argv);

    /* Create Dispatcher */
    dispatcher = purecvisor_dispatcher_new();
    if (!dispatcher) {
        g_critical("Failed to create Dispatcher Service.");
        return EXIT_FAILURE;
    }

    /* Create UDS Server */
    server = uds_server_new(SOCKET_PATH);
    if (!server) {
        g_critical("Failed to create UDS Server.");
        g_object_unref(dispatcher);
        return EXIT_FAILURE;
    }

    /* Dependency Injection */
    uds_server_set_dispatcher(server, dispatcher);

    /* Start Server */
    if (!uds_server_start(server, &error)) {
        g_critical("Failed to start UDS Server: %s", error->message);
        g_error_free(error);
        g_object_unref(server);
        g_object_unref(dispatcher);
        return EXIT_FAILURE;
    }
    g_info("Listening on UNIX Socket: %s", SOCKET_PATH);

    /* Main Loop */
    loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_signal_received, NULL);
    g_unix_signal_add(SIGTERM, on_signal_received, NULL);

    g_info("Entering Main Loop...");
    g_main_loop_run(loop);

    /* Cleanup */
    uds_server_stop(server);
    if (server) g_object_unref(server);
    if (dispatcher) g_object_unref(dispatcher);
    if (loop) g_main_loop_unref(loop);

    return EXIT_SUCCESS;
}
