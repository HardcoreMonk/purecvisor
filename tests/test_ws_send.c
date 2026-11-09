   
                       
                                                                 
  
                           
                                                                                     
                                                                     
                                                                
                                                                          
                                                                 
                                                                                      
  
                                                        
                 
                                                             
                         
   
#include <glib.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "api/drain.h"
#include "api/ws_send.h"

                                                                      
typedef struct {
    GMainContext *ctx;
    SoupWebsocketConnection *conn;
    GIOStream *peer;
} WsPair;

                                                   
static void
_ws_pair_open(WsPair *pair)
{
    int fds[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), ==, 0);
    pair->ctx = g_main_context_new();
    g_main_context_push_thread_default(pair->ctx);                                          
    GSocket *mine = g_socket_new_from_fd(fds[0], NULL);
    GSocket *theirs = g_socket_new_from_fd(fds[1], NULL);
    g_assert_nonnull(mine);
    g_assert_nonnull(theirs);
    GSocketConnection *stream = g_socket_connection_factory_create_connection(mine);
    pair->peer = G_IO_STREAM(g_socket_connection_factory_create_connection(theirs));
    GUri *uri = g_uri_parse("ws://localhost/ws-send-test", G_URI_FLAGS_NONE, NULL);
    pair->conn = soup_websocket_connection_new(G_IO_STREAM(stream), uri,
                                               SOUP_WEBSOCKET_CONNECTION_SERVER, NULL, NULL, NULL);
    g_main_context_pop_thread_default(pair->ctx);
    g_uri_unref(uri);
    g_object_unref(stream);
    g_object_unref(mine);
    g_object_unref(theirs);
    g_assert_cmpint(soup_websocket_connection_get_state(pair->conn), ==, SOUP_WEBSOCKET_STATE_OPEN);
}

                                               
static void
_ws_pair_close(WsPair *pair)
{
    g_clear_object(&pair->conn);
    if (pair->peer) {
        g_io_stream_close(pair->peer, NULL, NULL);
        g_clear_object(&pair->peer);
    }
                                                                 
    for (guint i = 0; i < 100 && g_main_context_iteration(pair->ctx, FALSE); i++) { }
    g_main_context_unref(pair->ctx);
    pair->ctx = NULL;
}

                                                  
static gboolean
_ws_peer_readable(WsPair *pair)
{
    GSocket *socket = g_socket_connection_get_socket(G_SOCKET_CONNECTION(pair->peer));
    struct pollfd pfd = { .fd = g_socket_get_fd(socket), .events = POLLIN };
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        if (poll(&pfd, 1, 10) == 1 && (pfd.revents & POLLIN))
            return TRUE;
        g_main_context_iteration(pair->ctx, FALSE);
    }
    return FALSE;
}

static void
test_ws_send_on_open(void)
{
    WsPair pair = {0};
    _ws_pair_open(&pair);
    GLogLevelFlags saved = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    g_assert_true(pcv_ws_send_text_if_open(pair.conn, "{\"type\":\"ping\"}"));
    g_log_set_always_fatal(saved);
    g_assert_true(_ws_peer_readable(&pair));                          
    _ws_pair_close(&pair);
}

                                                   
                                                                                                   
                                                         
static void
test_ws_send_after_close(void)
{
    WsPair pair = {0};
    _ws_pair_open(&pair);
    soup_websocket_connection_close(pair.conn, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
    g_assert_cmpint(soup_websocket_connection_get_state(pair.conn), !=, SOUP_WEBSOCKET_STATE_OPEN);
    GLogLevelFlags saved = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    g_assert_false(pcv_ws_send_text_if_open(pair.conn, "{\"type\":\"job.complete\"}"));
    g_log_set_always_fatal(saved);
    _ws_pair_close(&pair);
}

static gint null_fn_calls;
static gboolean
_count_fn(gpointer data)
{
    (void)data;
    null_fn_calls++;
    return G_SOURCE_REMOVE;
}

static void
test_ws_send_null_args(void)
{
    WsPair pair = {0};
    _ws_pair_open(&pair);
    GMainContext *ctx = g_main_context_new();
    null_fn_calls = 0;
    GLogLevelFlags saved = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
    g_assert_false(pcv_ws_send_text_if_open(NULL, "{}"));
    g_assert_false(pcv_ws_send_text_if_open(pair.conn, NULL));
    g_assert_false(pcv_ws_dispatch_on_owner(NULL, _count_fn, NULL, NULL));                              
    g_assert_false(pcv_ws_dispatch_on_owner(ctx, NULL, NULL, NULL));
    g_log_set_always_fatal(saved);
    g_assert_false(g_main_context_iteration(ctx, FALSE));                      
    g_assert_cmpint(null_fn_calls, ==, 0);
    g_main_context_unref(ctx);
    _ws_pair_close(&pair);
}

                                                     
typedef struct {
    gint fn_calls;
    gint notify_calls;
    GThread *ran_on;
    gboolean dispatched;
    GMainContext *ctx;
} DispatchProbe;

static gboolean
_probe_fn(gpointer data)
{
    DispatchProbe *probe = data;
    probe->fn_calls++;
    probe->ran_on = g_thread_self();
    return G_SOURCE_REMOVE;
}

static void
_probe_notify(gpointer data)
{
    DispatchProbe *probe = data;
    probe->notify_calls++;
}

static void
test_ws_dispatch_on_owner_inline(void)
{
    pcv_drain_init();
    gint work_before = pcv_drain_get_work();
    GMainContext *ctx = g_main_context_new();
    g_assert_true(g_main_context_acquire(ctx));                    
    DispatchProbe probe = {0};
    g_assert_true(pcv_ws_dispatch_on_owner(ctx, _probe_fn, &probe, _probe_notify));
    g_assert_cmpint(probe.fn_calls, ==, 1);                           
    g_assert_cmpint(probe.notify_calls, ==, 1);
    g_assert_true(probe.ran_on == g_thread_self());
    g_assert_cmpint(pcv_drain_get_work(), ==, work_before);                            
    g_assert_false(g_main_context_iteration(ctx, FALSE));                         
    g_main_context_release(ctx);
    g_main_context_unref(ctx);
}

                                                    
static gpointer
_dispatch_thread(gpointer data)
{
    DispatchProbe *probe = data;
    probe->dispatched = pcv_ws_dispatch_on_owner(probe->ctx, _probe_fn, probe, _probe_notify);
    return NULL;
}

static void
test_ws_dispatch_from_other_thread_runs_on_owner(void)
{
    pcv_drain_init();
    gint work_before = pcv_drain_get_work();
    DispatchProbe probe = {0};
    probe.ctx = g_main_context_new();
    g_assert_true(g_main_context_acquire(probe.ctx));                    
    GThread *thread = g_thread_new("ws-send-other", _dispatch_thread, &probe);
    g_thread_join(thread);
    g_assert_true(probe.dispatched);
    g_assert_cmpint(probe.fn_calls, ==, 0);                                   
    g_assert_cmpint(probe.notify_calls, ==, 0);
    g_assert_cmpint(pcv_drain_get_work(), ==, work_before + 1);                                 
    g_assert_true(g_main_context_iteration(probe.ctx, FALSE));
    g_assert_cmpint(probe.fn_calls, ==, 1);
    g_assert_true(probe.ran_on == g_thread_self());                             
    g_assert_cmpint(probe.notify_calls, ==, 1);
    g_assert_cmpint(pcv_drain_get_work(), ==, work_before);
    g_main_context_release(probe.ctx);
    g_main_context_unref(probe.ctx);
}

void test_ws_send_register(void);
void
test_ws_send_register(void)
{
    g_test_add_func("/ws/send/send_on_open", test_ws_send_on_open);
    g_test_add_func("/ws/send/send_after_close", test_ws_send_after_close);
    g_test_add_func("/ws/send/null_args", test_ws_send_null_args);
    g_test_add_func("/ws/send/dispatch_on_owner_inline", test_ws_dispatch_on_owner_inline);
    g_test_add_func("/ws/send/dispatch_from_other_thread_runs_on_owner",
                    test_ws_dispatch_from_other_thread_runs_on_owner);
}
