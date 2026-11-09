   
                        
                                                           
                                                                
                                                                 
                                                          
                                                 
   
#include "rest_shutdown.h"
#include <unistd.h>

typedef struct { SoupServer *soup; GMainLoop *loop; gint64 deadline; } PcvRestStop;
static void _pcv_rest_stop_free(gpointer data)
{
    PcvRestStop *stop = data;
    g_main_loop_unref(stop->loop);
    g_free(stop);
}
static gboolean _pcv_rest_stop_poll(gpointer data)
{
    PcvRestStop *stop = data;
    if (pcv_ws_server_is_quiescent()) {
        g_main_loop_quit(stop->loop);
        return G_SOURCE_REMOVE;
    }
    if (g_get_monotonic_time() >= stop->deadline) {
        g_printerr("REST shutdown timed out; exiting unsuccessfully without unsafe cleanup\n");
        _exit(EXIT_FAILURE);
    }
    return G_SOURCE_CONTINUE;
}
static gboolean _pcv_rest_stop_begin(gpointer data)
{
    PcvRestStop *stop = data;
                                                        
    pcv_ws_server_shutdown();
    soup_server_disconnect(stop->soup);
    GSource *poll = g_timeout_source_new(10);
    g_source_set_priority(poll, G_PRIORITY_HIGH);
    g_source_set_callback(poll, _pcv_rest_stop_poll, stop, _pcv_rest_stop_free);
    g_source_attach(poll, g_main_loop_get_context(stop->loop));
    g_source_unref(poll);
    return G_SOURCE_REMOVE;
}
void pcv_rest_shutdown_schedule(SoupServer *soup, GMainLoop *loop)
{
    PcvRestStop *stop = g_new0(PcvRestStop, 1);
    stop->soup = soup; stop->loop = g_main_loop_ref(loop);
    stop->deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    GSource *start = g_idle_source_new();
                                                               
                                                     
    g_source_set_priority(start, G_PRIORITY_HIGH);
    g_source_set_callback(start, _pcv_rest_stop_begin, stop, NULL);
    g_source_attach(start, g_main_loop_get_context(loop));
    g_source_unref(start);
}
