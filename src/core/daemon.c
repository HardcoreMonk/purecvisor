



































































#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <glib-unix.h>
#include <libvirt-glib/libvirt-glib.h>
#include "purecvisor/core.h"













struct PvContext {
    GMainLoop *loop;
    virConnectPtr conn;
    int is_running;
};









static PvContext g_ctx = {0};













PvContext* pv_get_instance(void) {
    return &g_ctx;
}




















static gboolean on_signal(gpointer user_data) {
    PvContext *ctx = (PvContext *)user_data;
    pv_log(LOG_INFO, "Signal received, stopping engine...");
    g_main_loop_quit(ctx->loop);
    return G_SOURCE_REMOVE;
}























int pv_init(void) {
    pv_log(LOG_INFO, "Initializing PureCVisor-engine (C11)...");












    if (virEventRegisterDefaultImpl() < 0) {
        pv_log(LOG_ERR, "Failed to register libvirt event impl");
        goto err_return;
    }

















    g_ctx.conn = virConnectOpen("qemu:///system");
    if (!g_ctx.conn) {
        pv_log(LOG_ERR, "Failed to connect to qemu:///system");
        goto err_return;
    }
    pv_log(LOG_INFO, "Connected to Hypervisor");













    g_ctx.loop = g_main_loop_new(NULL, FALSE);
    if (!g_ctx.loop) {
        pv_log(LOG_ERR, "Failed to create GLib Main Loop");
        goto err_conn;
    }












    g_unix_signal_add(SIGINT, on_signal, &g_ctx);
    g_unix_signal_add(SIGTERM, on_signal, &g_ctx);

    g_ctx.is_running = 1;
    return 0;

err_conn:
    virConnectClose(g_ctx.conn);
    g_ctx.conn = NULL;
err_return:
    return -1;
}


















void pv_run(void) {
    if (g_ctx.is_running && g_ctx.loop) {
        pv_log(LOG_INFO, "Engine Loop Started.");
        g_main_loop_run(g_ctx.loop);
    }
}

















void pv_cleanup(void) {
    pv_log(LOG_INFO, "Cleaning up engine resources...");
    if (g_ctx.conn) {
        virConnectClose(g_ctx.conn);
        g_ctx.conn = NULL;
    }
    if (g_ctx.loop) {
        g_main_loop_unref(g_ctx.loop);
        g_ctx.loop = NULL;
    }
}


























void pv_log(int level, const char *fmt, ...) {
    va_list args;
    const char *level_str = "UNKNOWN";

    switch(level) {
        case LOG_INFO: level_str = "[INFO]"; break;
        case LOG_ERR:  level_str = "[ERR ]"; break;



    }

    fprintf(stderr, "[PureCVisor] %s ", level_str);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}
