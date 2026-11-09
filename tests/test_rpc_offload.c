   
                           
                                                             
  
                           
                                                                                                          
                                                                                          
                                                                                               
                                                                                                        
                                                              
                                       
  
                       
                                                                                      
                                   
   
#include <glib.h>
#include <string.h>
#include <unistd.h>

#include "api/drain.h"
#include "modules/audit/pcv_audit.h"
#include "modules/dispatcher/poll_async.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/rpc_offload.h"
#include "modules/dispatcher/rpc_utils.h"

                                
guint pcv_test_uds_response_count(void);
void pcv_test_uds_responses_reset(void);
const gchar *pcv_test_last_uds_response(void);
void pcv_test_audit_reset(void);
gint pcv_test_audit_call_count(void);
const gchar *pcv_test_audit_last_method(void);
const gchar *pcv_test_audit_last_result(void);
gint pcv_test_audit_last_error_code(void);
const gchar *pcv_test_audit_username_at(gint row);

#define WAIT_DEADLINE_US ((gint64)10 * G_TIME_SPAN_SECOND)
#define FAKE_BASE_US     ((gint64)1000 * G_TIME_SPAN_SECOND)

                                                                               
static struct {
    GMutex   mu;
    GCond    cond;
    gboolean release;                                     
    guint    handler_calls;
    guint    running;
    guint    max_running;
    guint    finish_calls;
    guint    owner_calls;
    PcvRpcCompletionResult last_completion;
    GThread *last_handler_thread;
    gchar   *seen_actor[8];
    guint    seen_actor_n;
    gchar   *seen_param;
    gint64   now_us;
    gboolean finish_async;                                               
} g_fx;

typedef struct { gint unused; } Fx;

                                                               
static void
_wait_until(gboolean (*cond)(gpointer), gpointer data)
{
    gint64 deadline = g_get_monotonic_time() + WAIT_DEADLINE_US;
    while (!cond(data)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
}

static gboolean
_finish_at_least(gpointer data)
{
    g_mutex_lock(&g_fx.mu);
    gboolean ok = g_fx.finish_calls >= GPOINTER_TO_UINT(data);
    g_mutex_unlock(&g_fx.mu);
    return ok;
}

static gboolean
_running_at_least(gpointer data)
{
    g_mutex_lock(&g_fx.mu);
    gboolean ok = g_fx.running >= GPOINTER_TO_UINT(data);
    g_mutex_unlock(&g_fx.mu);
    return ok;
}

static gboolean
_handlers_at_least(gpointer data)
{
    g_mutex_lock(&g_fx.mu);
    gboolean ok = g_fx.handler_calls >= GPOINTER_TO_UINT(data);
    g_mutex_unlock(&g_fx.mu);
    return ok;
}

static void
_hold_set(gboolean release)
{
    g_mutex_lock(&g_fx.mu);
    g_fx.release = release;
    g_cond_broadcast(&g_fx.cond);
    g_mutex_unlock(&g_fx.mu);
}

                                                        
static void
_note_enter(void)
{
    g_mutex_lock(&g_fx.mu);
    g_fx.handler_calls++;
    g_fx.running++;
    if (g_fx.running > g_fx.max_running)
        g_fx.max_running = g_fx.running;
    g_fx.last_handler_thread = g_thread_self();
    g_mutex_unlock(&g_fx.mu);
}

static void
_note_leave(void)
{
    g_mutex_lock(&g_fx.mu);
    g_fx.running--;
    g_mutex_unlock(&g_fx.mu);
}

static void
_send_ok(const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    gchar *resp = pure_rpc_build_success_response(rpc_id, json_node_init_int(json_node_alloc(), 1));
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

                               
static void
_h_ok(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    _note_enter();
    _send_ok(rpc_id, server, connection);
    _note_leave();
}

static void
_h_twice(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    _note_enter();
    _send_ok(rpc_id, server, connection);
    _send_ok(rpc_id, server, connection);
    _note_leave();
}

static void
_h_silent(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    (void)params; (void)rpc_id; (void)server; (void)connection;
    _note_enter();
    _note_leave();
}

                                                                    
static void
_h_hold(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    _note_enter();
    g_mutex_lock(&g_fx.mu);
    if (g_fx.seen_actor_n < G_N_ELEMENTS(g_fx.seen_actor))
        g_fx.seen_actor[g_fx.seen_actor_n++] = g_strdup(pcv_rpc_completion_actor());
    while (!g_fx.release)
        g_cond_wait(&g_fx.cond, &g_fx.mu);
    g_mutex_unlock(&g_fx.mu);
    if (params && json_object_has_member(params, "p")) {
        g_mutex_lock(&g_fx.mu);
        g_free(g_fx.seen_param);
        g_fx.seen_param = g_strdup(json_object_get_string_member(params, "p"));
        g_mutex_unlock(&g_fx.mu);
    }
    _send_ok(rpc_id, server, connection);
    _note_leave();
}

                                      
typedef struct { gchar *rpc_id; GObject *server; GObject *connection; } LateCtx;

static gboolean
_late_send(gpointer data)
{
    LateCtx *c = data;
    _send_ok(c->rpc_id, (UdsServer *)c->server, (GSocketConnection *)c->connection);
    g_free(c->rpc_id);
    g_object_unref(c->server);
    g_object_unref(c->connection);
    g_free(c);
    return G_SOURCE_REMOVE;
}

static void
_h_late(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    _note_enter();
    LateCtx *c = g_new0(LateCtx, 1);
    c->rpc_id = g_strdup(rpc_id);
    c->server = g_object_ref((GObject *)server);
    c->connection = g_object_ref((GObject *)connection);
    g_idle_add(_late_send, c);
    _note_leave();
}

                                                    
static gboolean
_owner_deny(const gchar *method, JsonObject *params, const gchar *rpc_id, UdsServer *server,
            GSocketConnection *connection, const gchar *actor)
{
    (void)params;
    g_mutex_lock(&g_fx.mu);
    g_fx.owner_calls++;
    g_fx.last_handler_thread = g_thread_self();
    g_mutex_unlock(&g_fx.mu);
    gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_FORBIDDEN, "Permission denied");
    pure_uds_server_send_response(server, connection, err);
    g_free(err);
    pcv_audit_log(actor, method, "vm-x", "denied", PURE_RPC_ERR_FORBIDDEN, 0, "rbac");
    pcv_rpc_completion_note_audit(method);                                                   
    return FALSE;
}

static gboolean
_owner_allow(const gchar *method, JsonObject *params, const gchar *rpc_id, UdsServer *server,
             GSocketConnection *connection, const gchar *actor)
{
    (void)method; (void)params; (void)rpc_id; (void)server; (void)connection; (void)actor;
    g_mutex_lock(&g_fx.mu);
    g_fx.owner_calls++;
    g_mutex_unlock(&g_fx.mu);
    return TRUE;
}

                                                                        
static void
_finish(const gchar *method, const gchar *rpc_id, const gchar *actor, const gchar *target,
        gint64 start_us, PcvRpcCompletionResult completion)
{
    (void)rpc_id; (void)start_us;
    g_assert_true(g_main_context_is_owner(NULL));
    gboolean is_async = g_fx.finish_async;
    gboolean success = pcv_rpc_completion_dispatch_succeeded(completion, is_async);
    gint code = success ? 0 : (completion.response_observed ? completion.error_code : PURE_RPC_ERR_INTERNAL_ERROR);
    if (pcv_rpc_completion_should_audit(completion, is_async))
        pcv_audit_log(actor, method, target, success ? "ok" : "fail", code, 0, "local");
    g_mutex_lock(&g_fx.mu);
    g_fx.finish_calls++;
    g_fx.last_completion = completion;
    g_mutex_unlock(&g_fx.mu);
}

static gint64
_fake_now(void)
{
    g_mutex_lock(&g_fx.mu);
    gint64 v = g_fx.now_us;
    g_mutex_unlock(&g_fx.mu);
    return v;
}

static gboolean _always_terminating(void) { return TRUE; }

static void
fx_setup(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    g_assert_true(g_main_context_acquire(NULL));
    g_mutex_lock(&g_fx.mu);
    g_fx.release = TRUE;
    g_fx.handler_calls = g_fx.running = g_fx.max_running = g_fx.finish_calls = g_fx.owner_calls = 0;
    memset(&g_fx.last_completion, 0, sizeof g_fx.last_completion);
    g_fx.last_handler_thread = NULL;
    for (guint i = 0; i < g_fx.seen_actor_n; i++)
        g_clear_pointer(&g_fx.seen_actor[i], g_free);
    g_fx.seen_actor_n = 0;
    g_clear_pointer(&g_fx.seen_param, g_free);
    g_fx.now_us = FAKE_BASE_US;
    g_fx.finish_async = FALSE;
    g_mutex_unlock(&g_fx.mu);
    pcv_test_uds_responses_reset();
    pcv_test_audit_reset();
    pcv_rpc_offload_add_for_test("t.libvirt", PCV_OFFLOAD_LANE_LIBVIRT_READ);
    pcv_rpc_offload_add_for_test("t.host", PCV_OFFLOAD_LANE_HOST_READ);
    pcv_rpc_offload_add_for_test("t.net", PCV_OFFLOAD_LANE_NET_MUTATE);
    pcv_rpc_offload_add_for_test("t.storage", PCV_OFFLOAD_LANE_STORAGE_MUTATE);
}

static void
fx_teardown(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _hold_set(TRUE);
    pcv_rpc_offload_shutdown();
    while (g_main_context_iteration(NULL, FALSE)) {}
    pcv_rpc_offload_set_now_fn_for_test(NULL);
    pcv_rpc_offload_set_terminating_fn_for_test(NULL);
    pcv_rpc_offload_add_for_test(NULL, -1);
    g_main_context_release(NULL);
}

                                                                        
static GObject *_new_obj(void) { return g_object_new(G_TYPE_OBJECT, NULL); }

                                                   
static gboolean
_submit_full(const gchar *method, const gchar *rpc_id, const gchar *actor, PcvRpcOffloadHandler h,
             PcvRpcOffloadOwnerCheck owner, JsonParser *parser, JsonObject *params)
{
    GObject *server = _new_obj(), *conn = _new_obj();
    const PcvRpcOffloadRequest req = {
        .method = method, .rpc_id = rpc_id, .req_id = "req-t", .actor = actor, .target = "tgt",
        .start_us = g_get_monotonic_time(), .handler = h, .owner_check = owner, .finish = _finish,
        .parser = parser, .params = params, .server = (UdsServer *)server, .connection = (GSocketConnection *)conn };
    gboolean ok = pcv_rpc_offload_submit(&req);
    g_object_unref(server);
    g_object_unref(conn);
    return ok;
}

static gboolean
_submit(const gchar *method, const gchar *rpc_id, PcvRpcOffloadHandler h)
{
    return _submit_full(method, rpc_id, "actor-a", h, NULL, NULL, NULL);
}

                                          
static gint
_last_error_code(gchar **message)
{
    JsonNode *n = json_from_string(pcv_test_last_uds_response(), NULL);
    g_assert_nonnull(n);
    JsonObject *e = json_object_get_object_member(json_node_get_object(n), "error");
    g_assert_nonnull(e);
    gint code = (gint)json_object_get_int_member(e, "code");
    if (message)
        *message = g_strdup(json_object_get_string_member(e, "message"));
    json_node_unref(n);
    return code;
}

                                                                      
static void
test_response_once(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    g_assert_true(_submit("t.libvirt", "1", _h_ok));
    gint64 deadline = g_get_monotonic_time() + WAIT_DEADLINE_US;
    while (!_handlers_at_least(GUINT_TO_POINTER(1))) {                           
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    g_usleep(20000);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 0);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 1);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"result\":1"));
    g_assert_true(g_fx.last_handler_thread != g_thread_self());
    g_assert_true(g_fx.last_completion.response_observed);
    g_assert_true(g_fx.last_completion.success);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "ok");
}

static gint g_second_warns;

static void
_count_second_warn(const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer user_data)
{
    (void)domain; (void)level; (void)user_data;
    if (strstr(message, "second response ignored"))
        g_atomic_int_inc(&g_second_warns);
}

                               
static void
test_second_response_ignored(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    g_atomic_int_set(&g_second_warns, 0);
    guint handler = g_log_set_handler("rpc_offload", G_LOG_LEVEL_WARNING, _count_second_warn, NULL);
    g_assert_true(_submit("t.libvirt", "2", _h_twice));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_log_remove_handler("rpc_offload", handler);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 1);
    g_assert_cmpint(g_atomic_int_get(&g_second_warns), ==, 1);
}

                                                                 
static void
test_no_response_is_internal_error(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    g_assert_true(_submit("t.libvirt", "3", _h_silent));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 0);
    g_assert_false(g_fx.last_completion.response_observed);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, PURE_RPC_ERR_INTERNAL_ERROR);
}

static gboolean
_responses_at_least(gpointer data)
{
    return pcv_test_uds_response_count() >= GPOINTER_TO_UINT(data);
}

                                                                 
static void
test_late_callback_passes_through(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    g_assert_true(_submit("t.libvirt", "4", _h_late));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    _wait_until(_responses_at_least, GUINT_TO_POINTER(1));
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 1);
}

                                                         
static void
test_queue_full_busy(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _hold_set(FALSE);
    g_assert_true(_submit("t.host", "a", _h_hold));
    g_assert_true(_submit("t.host", "b", _h_hold));
    _wait_until(_running_at_least, GUINT_TO_POINTER(2));
    for (guint i = 0; i < 32; i++)
        g_assert_true(_submit("t.host", "q", _h_hold));
    g_assert_false(_submit("t.host", "over", _h_hold));
    g_assert_true(_submit("t.libvirt", "x", _h_ok));                     
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(35));
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 35);
}

                             
static void
test_lane_parallel_cap(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _hold_set(FALSE);
    for (guint i = 0; i < 6; i++)
        g_assert_true(_submit("t.libvirt", "p", _h_hold));
    _wait_until(_running_at_least, GUINT_TO_POINTER(2));
    g_usleep(50000);
    g_assert_cmpuint(g_fx.running, ==, 2);
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(6));
    g_assert_cmpuint(g_fx.max_running, ==, 2);

    g_fx.max_running = 0;
    _hold_set(FALSE);
    for (guint i = 0; i < 3; i++)
        g_assert_true(_submit("t.net", "n", _h_hold));
    _wait_until(_running_at_least, GUINT_TO_POINTER(1));
    g_usleep(50000);
    g_assert_cmpuint(g_fx.running, ==, 1);
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(9));
    g_assert_cmpuint(g_fx.max_running, ==, 1);
}

                                             
static void
test_actor_scope_isolated(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _hold_set(FALSE);
    g_assert_true(_submit_full("t.libvirt", "a", "actor-a", _h_hold, NULL, NULL, NULL));
    g_assert_true(_submit_full("t.libvirt", "b", "actor-b", _h_hold, NULL, NULL, NULL));
    _wait_until(_running_at_least, GUINT_TO_POINTER(2));
    gboolean a = g_strcmp0(g_fx.seen_actor[0], "actor-a") == 0 || g_strcmp0(g_fx.seen_actor[1], "actor-a") == 0;
    gboolean b = g_strcmp0(g_fx.seen_actor[0], "actor-b") == 0 || g_strcmp0(g_fx.seen_actor[1], "actor-b") == 0;
    g_assert_true(a && b);
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 2);
    gboolean ua = g_strcmp0(pcv_test_audit_username_at(0), "actor-a") == 0 || g_strcmp0(pcv_test_audit_username_at(1), "actor-a") == 0;
    gboolean ub = g_strcmp0(pcv_test_audit_username_at(0), "actor-b") == 0 || g_strcmp0(pcv_test_audit_username_at(1), "actor-b") == 0;
    g_assert_true(ua && ub);
}

                                                  
static void
test_terminating_rejects_queued(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    pcv_rpc_offload_set_terminating_fn_for_test(_always_terminating);
    g_assert_true(_submit("t.host", "9", _h_ok));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpuint(g_fx.handler_calls, ==, 0);
    gchar *msg = NULL;
    g_assert_cmpint(_last_error_code(&msg), ==, PURE_RPC_ERR_ZFS_OPERATION);
    g_assert_cmpstr(msg, ==, "server is shutting down");
    g_free(msg);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, PURE_RPC_ERR_ZFS_OPERATION);
}

                                                               
static void
test_owner_scope_denied_in_worker(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    g_assert_true(_submit_full("t.libvirt", "o", "op1", _h_ok, _owner_deny, NULL, NULL));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpuint(g_fx.owner_calls, ==, 1);
    g_assert_true(g_fx.last_handler_thread != g_thread_self());
    g_assert_cmpuint(g_fx.handler_calls, ==, 0);
    g_assert_cmpint(_last_error_code(NULL), ==, PURE_RPC_ERR_FORBIDDEN);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "denied");
    g_assert_cmpstr(pcv_test_audit_username_at(0), ==, "op1");
    pcv_test_uds_responses_reset();
    g_assert_true(_submit_full("t.libvirt", "o2", "op1", _h_ok, _owner_allow, NULL, NULL));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    g_assert_cmpuint(g_fx.handler_calls, ==, 1);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 1);
}

                                                            
static void
test_parser_lives_until_done(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, "{\"p\":\"from-parser\"}", -1, NULL));
    JsonObject *params = json_node_get_object(json_parser_get_root(parser));
    _hold_set(FALSE);
    g_assert_true(_submit_full("t.libvirt", "p", "actor-a", _h_hold, NULL, parser, params));
    g_object_unref(parser);
    _wait_until(_running_at_least, GUINT_TO_POINTER(1));
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpstr(g_fx.seen_param, ==, "from-parser");
}

                                                                                        
static void
test_prom_paired(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    g_assert_true(_submit("t.libvirt", "1", _h_ok));
    g_assert_true(_submit("t.libvirt", "2", _h_silent));
    pcv_rpc_offload_set_terminating_fn_for_test(NULL);
    g_assert_false(_submit("t.unregistered", "3", _h_ok));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(20000);
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_assert_cmpuint(g_fx.finish_calls, ==, 2);
}

                                                                  
static void
test_stale_queued_mutation_rejected(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    pcv_rpc_offload_set_now_fn_for_test(_fake_now);
    _hold_set(FALSE);
    g_assert_true(_submit("t.net", "a", _h_hold));
    _wait_until(_running_at_least, GUINT_TO_POINTER(1));
    g_assert_true(_submit("t.net", "b", _h_ok));
    g_mutex_lock(&g_fx.mu);
    g_fx.now_us += (gint64)8100 * 1000;
    g_mutex_unlock(&g_fx.mu);
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    g_assert_cmpuint(g_fx.handler_calls, ==, 1);               
    gchar *msg = NULL;
    g_assert_cmpint(_last_error_code(&msg), ==, PURE_RPC_ERR_BUSY);
    g_assert_cmpstr(msg, ==, "t.net: queued too long");
    g_free(msg);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 2);
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, PURE_RPC_ERR_BUSY);
}

                           
static void
test_mutation_within_wait_runs(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    pcv_rpc_offload_set_now_fn_for_test(_fake_now);
    _hold_set(FALSE);
    g_assert_true(_submit("t.storage", "a", _h_hold));
    _wait_until(_running_at_least, GUINT_TO_POINTER(1));
    g_assert_true(_submit("t.storage", "b", _h_ok));
    g_mutex_lock(&g_fx.mu);
    g_fx.now_us += (gint64)7900 * 1000;
    g_mutex_unlock(&g_fx.mu);
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    g_assert_cmpuint(g_fx.handler_calls, ==, 2);
}

                        
static void
test_read_lane_no_stale_reject(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    pcv_rpc_offload_set_now_fn_for_test(_fake_now);
    _hold_set(FALSE);
    g_assert_true(_submit("t.libvirt", "a", _h_hold));
    g_assert_true(_submit("t.libvirt", "b", _h_hold));
    _wait_until(_running_at_least, GUINT_TO_POINTER(2));
    g_assert_true(_submit("t.libvirt", "c", _h_ok));
    g_mutex_lock(&g_fx.mu);
    g_fx.now_us += (gint64)60 * G_USEC_PER_SEC;
    g_mutex_unlock(&g_fx.mu);
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(3));
    g_assert_cmpuint(g_fx.handler_calls, ==, 3);
}

                                     
static void
test_mutate_queue_cap_8(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _hold_set(FALSE);
    g_assert_true(_submit("t.storage", "a", _h_hold));
    _wait_until(_running_at_least, GUINT_TO_POINTER(1));
    for (guint i = 0; i < 8; i++)
        g_assert_true(_submit("t.storage", "q", _h_hold));
    g_assert_false(_submit("t.storage", "over", _h_hold));
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(9));
}

static gpointer
_shutdown_thread(gpointer data)
{
    pcv_rpc_offload_shutdown();
    g_atomic_int_set((gint *)data, 1);
    return NULL;
}

                                                                       
static void
test_shutdown_joins_idle(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    gint done = 0;
    _hold_set(FALSE);
    g_assert_true(_submit("t.libvirt", "s", _h_hold));
    _wait_until(_running_at_least, GUINT_TO_POINTER(1));
    GThread *t = g_thread_new("offload-shutdown", _shutdown_thread, &done);
    g_usleep(100000);
    g_assert_cmpint(g_atomic_int_get(&done), ==, 0);
    _hold_set(TRUE);
    g_thread_join(t);
    g_assert_cmpint(g_atomic_int_get(&done), ==, 1);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_true(_submit("t.libvirt", "s2", _h_ok));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
}

                                    

                                         
typedef enum { SP_OK, SP_TIMEOUT, SP_TIMEOUT_UNREAPED, SP_NONZERO, SP_EXEC_FAIL, SP_ABORTED } SpMode;
static struct { GMutex mu; guint calls; guint last_timeout; gint64 now_us; gint after_real; SpMode mode, mode_next; gboolean use_next; } g_sp;

static gboolean
_sp_fake(const gchar * const *argv, const PcvSpawnBound *b, gchar **out, gchar **err, gint *status, GError **error)
{
    g_mutex_lock(&g_sp.mu);
    g_sp.calls++;
    g_sp.last_timeout = b->timeout_sec;
    SpMode mode = g_sp.mode;
    if (g_sp.use_next) {                                  
        g_sp.mode = g_sp.mode_next;
        g_sp.use_next = FALSE;
    }
    g_mutex_unlock(&g_sp.mu);
    switch (mode) {                                           
    case SP_TIMEOUT_UNREAPED:
        *b->unreaped_pid_out = getpid();                                           
        [[fallthrough]];
    case SP_TIMEOUT:
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "%s: timed out after %us", argv[0], b->timeout_sec);
        return FALSE;
    case SP_EXEC_FAIL:
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s: not found", argv[0]);
        return FALSE;
    case SP_ABORTED:
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "%s: aborted", argv[0]);
        return FALSE;
    case SP_NONZERO:
        if (out) *out = g_strdup("");
        if (err) *err = g_strdup("no datasets");
        *status = 1;
        return TRUE;
    case SP_OK:
        break;
    }
    if (out) *out = g_strdup("ok\n");
    if (err) *err = g_strdup("");
    *status = 0;
    return TRUE;
}

static gint64
_sp_now(void)
{
    g_mutex_lock(&g_sp.mu);
    gint64 v = g_sp.now_us;
    g_mutex_unlock(&g_sp.mu);
    return v;
}

                                                                                      
static void
_h_spawn(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    _note_enter();
    gint64 adv = params ? json_object_get_int_member_with_default(params, "advance_ms", 0) : 0;
    g_mutex_lock(&g_sp.mu);
    g_sp.now_us += adv * 1000;
    g_mutex_unlock(&g_sp.mu);
    const gchar *argv[] = { "/bin/true", NULL };
    gchar *out = NULL;
    GError *e = NULL;
    gboolean ok = pcv_spawn_sync(argv, &out, NULL, &e);
    g_free(out);
    gchar *resp = ok ? pure_rpc_build_success_response(rpc_id, json_node_init_int(json_node_alloc(), 1))
                     : pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_INTERNAL_ERROR, e ? e->message : "spawn failed");
    g_clear_error(&e);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    _note_leave();
}

                                                                                           
static void
_h_swallow(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    _note_enter();
    const gchar *argv[] = { "/bin/true", NULL };
    gchar *out = NULL;
    (void)pcv_spawn_sync(argv, &out, NULL, NULL);
    g_free(out);
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, json_array_new());
    gchar *resp = pure_rpc_build_success_response(rpc_id, n);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    _note_leave();
}

                                                    
static void
_h_swallow_twice(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    const gchar *argv[] = { "/bin/true", NULL };
    gchar *out = NULL;
    (void)pcv_spawn_sync(argv, &out, NULL, NULL);
    g_free(out);
    _h_swallow(params, rpc_id, server, connection);
}

                                                                                    
static void
_after_job_spawn(void)
{
    const gchar *argv[] = { "/bin/true", NULL };
    g_mutex_lock(&g_sp.mu);
    guint before = g_sp.calls;
    g_mutex_unlock(&g_sp.mu);
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, NULL);
    g_mutex_lock(&g_sp.mu);
    if (ok && g_sp.calls == before)
        g_sp.after_real++;
    g_mutex_unlock(&g_sp.mu);
}

static void
_sp_setup(void)
{
    g_mutex_lock(&g_sp.mu);
    g_sp.calls = 0;
    g_sp.last_timeout = 0;
    g_sp.now_us = FAKE_BASE_US;
    g_sp.after_real = 0;
    g_sp.mode = SP_OK;
    g_sp.use_next = FALSE;
    g_mutex_unlock(&g_sp.mu);
    pcv_poll_async_set_spawn_fn_for_test(_sp_fake);
    pcv_poll_async_set_now_fn_for_test(_sp_now);
    pcv_spawn_launcher_init();
}

static void
_sp_teardown(void)
{
    pcv_rpc_offload_set_after_job_fn_for_test(NULL);
    pcv_rpc_offload_shutdown();
    pcv_poll_async_set_spawn_fn_for_test(NULL);
    pcv_poll_async_set_now_fn_for_test(NULL);
    pcv_spawn_launcher_shutdown();
}

                                                     
static void
test_host_lane_installs_guard(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _sp_setup();
    g_assert_true(_submit("t.host", "h", _h_spawn));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpuint(g_sp.calls, ==, 1);
    g_assert_cmpuint(g_sp.last_timeout, ==, 10);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"result\":1"));
    _sp_teardown();
}

                                        
static void
test_libvirt_and_mutate_lanes_no_guard(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _sp_setup();
    g_assert_true(_submit("t.libvirt", "l", _h_spawn));
    g_assert_true(_submit("t.net", "n", _h_spawn));
    g_assert_true(_submit("t.storage", "s", _h_spawn));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(3));
    g_assert_cmpuint(g_sp.calls, ==, 0);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 3);
    _sp_teardown();
}

                              
static void
test_guard_cleared_after_job(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _sp_setup();
    pcv_rpc_offload_set_after_job_fn_for_test(_after_job_spawn);
    g_assert_true(_submit("t.host", "c", _h_spawn));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpuint(g_sp.calls, ==, 1);
    g_assert_cmpint(g_sp.after_real, ==, 1);
    _sp_teardown();
}

                                   
static void
test_budget_per_request(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _sp_setup();
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, "{\"advance_ms\":25000}", -1, NULL));
    g_assert_true(_submit_full("t.host", "a", "actor-a", _h_spawn, NULL, parser,
                               json_node_get_object(json_parser_get_root(parser))));
    g_object_unref(parser);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpuint(g_sp.calls, ==, 0);                        
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "t.host: host command timed out"));
    g_assert_true(_submit("t.host", "b", _h_spawn));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    g_assert_cmpuint(g_sp.calls, ==, 1);
    g_assert_cmpuint(g_sp.last_timeout, ==, 10);
    _sp_teardown();
}

                                       
static void
_sp_mode(SpMode mode)
{
    g_mutex_lock(&g_sp.mu);
    g_sp.mode = mode;
    g_mutex_unlock(&g_sp.mu);
}

                                                                                          
static void
_assert_stalled_error(const gchar *method, guint responses, gint audits)
{
    gchar *msg = NULL;
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, responses);
    g_assert_cmpint(_last_error_code(&msg), ==, PURE_RPC_ERR_ZFS_OPERATION);
    gchar *want = g_strdup_printf("%s: host command timed out", method);
    g_assert_cmpstr(msg, ==, want);
    g_free(want);
    g_free(msg);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, audits);
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, PURE_RPC_ERR_ZFS_OPERATION);
    g_assert_true(g_fx.last_completion.response_observed);
    g_assert_false(g_fx.last_completion.success);
    g_assert_cmpint(g_atomic_int_get(&g_second_warns), ==, 0);
}

                                                       
static void
test_host_timeout_becomes_error(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _sp_setup();
    _sp_mode(SP_TIMEOUT);
    g_atomic_int_set(&g_second_warns, 0);
    guint handler = g_log_set_handler("rpc_offload", G_LOG_LEVEL_WARNING, _count_second_warn, NULL);
    g_assert_true(_submit("t.host", "to", _h_swallow));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_log_remove_handler("rpc_offload", handler);
    g_assert_cmpuint(g_sp.calls, ==, 1);
    _assert_stalled_error("t.host", 1, 1);
    _sp_teardown();
}

                                                                                
static void
test_host_busy_becomes_error(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _sp_setup();
    pcv_rpc_offload_add_for_test("t.host.stuck", PCV_OFFLOAD_LANE_HOST_READ);
    g_atomic_int_set(&g_second_warns, 0);
    guint handler = g_log_set_handler("rpc_offload", G_LOG_LEVEL_WARNING, _count_second_warn, NULL);
    _sp_mode(SP_TIMEOUT_UNREAPED);
    g_assert_true(_submit("t.host.stuck", "s1", _h_swallow));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpuint(g_sp.calls, ==, 1);
    _assert_stalled_error("t.host.stuck", 1, 1);
    _sp_mode(SP_OK);
    g_assert_true(_submit("t.host.stuck", "s2", _h_swallow));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    g_log_remove_handler("rpc_offload", handler);
    g_assert_cmpuint(g_sp.calls, ==, 1);                          
    _assert_stalled_error("t.host.stuck", 2, 2);
    _sp_teardown();
}

                                                                             
static void
_assert_handler_response_kept(SpMode mode)
{
    _sp_setup();
    _sp_mode(mode);
    g_assert_true(_submit("t.host", "keep", _h_swallow));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 1);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"result\":[]"));
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "ok");
    _sp_teardown();
}

                                                             
static void
test_host_nonzero_keeps_response(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _assert_handler_response_kept(SP_NONZERO);
}

                                                     
static void
test_host_exec_failure_keeps_response(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _assert_handler_response_kept(SP_EXEC_FAIL);
}

                                                                                               
static void
_assert_shutting_down_error(void)
{
    gchar *msg = NULL;
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 1);
    g_assert_cmpint(_last_error_code(&msg), ==, PURE_RPC_ERR_ZFS_OPERATION);
    g_assert_cmpstr(msg, ==, "server is shutting down");
    g_free(msg);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, PURE_RPC_ERR_ZFS_OPERATION);
    g_assert_true(g_fx.last_completion.response_observed);
    g_assert_false(g_fx.last_completion.success);
    g_assert_cmpint(g_atomic_int_get(&g_second_warns), ==, 0);
}

                                                                                                        
static void
test_host_abort_becomes_shutting_down(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _sp_setup();
    _sp_mode(SP_ABORTED);
    g_atomic_int_set(&g_second_warns, 0);
    guint handler = g_log_set_handler("rpc_offload", G_LOG_LEVEL_WARNING, _count_second_warn, NULL);
    g_assert_true(_submit("t.host", "ab", _h_swallow));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_log_remove_handler("rpc_offload", handler);
    g_assert_cmpuint(g_sp.calls, ==, 1);
    _assert_shutting_down_error();
    _sp_mode(SP_OK);
    g_assert_true(_submit("t.host", "ab2", _h_swallow));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"result\":[]"));
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "ok");
    _sp_teardown();
}

                                                        
static void
test_host_abort_wins_over_timeout(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const SpMode orders[2][2] = { { SP_TIMEOUT, SP_ABORTED }, { SP_ABORTED, SP_TIMEOUT } };
    for (guint i = 0; i < G_N_ELEMENTS(orders); i++) {
        _sp_setup();
        pcv_test_uds_responses_reset();
        pcv_test_audit_reset();
        g_mutex_lock(&g_fx.mu);
        g_fx.finish_calls = 0;
        g_mutex_unlock(&g_fx.mu);
        g_atomic_int_set(&g_second_warns, 0);
        g_mutex_lock(&g_sp.mu);
        g_sp.mode = orders[i][0];
        g_sp.mode_next = orders[i][1];
        g_sp.use_next = TRUE;
        g_mutex_unlock(&g_sp.mu);
        g_assert_true(_submit("t.host", "both", _h_swallow_twice));
        _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
        g_assert_cmpuint(g_sp.calls, ==, 2);
        _assert_shutting_down_error();
        _sp_teardown();
    }
}

                                                              
static void
test_no_guard_lane_keeps_response(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _sp_setup();
    _sp_mode(SP_TIMEOUT);
    g_assert_true(_submit("t.libvirt", "nl", _h_swallow));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpuint(g_sp.calls, ==, 0);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"result\":[]"));
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "ok");
    _sp_teardown();
}

                                                
static void
test_stall_mark_per_request(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    _sp_setup();
    _sp_mode(SP_TIMEOUT);
    g_assert_true(_submit("t.host", "m1", _h_swallow));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    _sp_mode(SP_OK);
    g_assert_true(_submit("t.host", "m2", _h_swallow));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "\"result\":[]"));
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "ok");
    _sp_teardown();
}

                                                                                        
static void
test_net_lane_serial(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    static const gchar * const net_routes[] = {
        "security_group.create", "security_group.delete", "security_group.detach", "security_group.rule.add",
        "security_group.rule.remove", "vm.security_group.set", "dpdk.bind", "dpdk.unbind",
    };
    for (guint i = 0; i < G_N_ELEMENTS(net_routes); i++)
        g_assert_cmpint(pcv_rpc_offload_lane_of(net_routes[i]), ==, PCV_OFFLOAD_LANE_NET_MUTATE);
    g_assert_cmpint(pcv_rpc_offload_lane_of("push.subscribe"), ==, PCV_OFFLOAD_LANE_HOST_READ);
    g_assert_cmpint(pcv_rpc_offload_lane_of("storage.pool.create"), ==, PCV_OFFLOAD_LANE_STORAGE_MUTATE);
    g_assert_cmpint(pcv_rpc_offload_lane_of("vm.snapshot.create"), ==, PCV_OFFLOAD_LANE_STORAGE_MUTATE);
    g_assert_cmpint(pcv_rpc_offload_lane_of("vm.snapshot.delete"), ==, PCV_OFFLOAD_LANE_STORAGE_MUTATE);
    g_assert_cmpint(pcv_rpc_offload_lane_of("vm.snapshot.delete_all"), ==, PCV_OFFLOAD_LANE_STORAGE_MUTATE);
    g_assert_cmpint(pcv_rpc_offload_lane_of("vm.snapshot.list"), ==, PCV_OFFLOAD_LANE_HOST_READ);
    g_assert_cmpint(pcv_rpc_offload_lane_of("vm.snapshot.rollback"), ==, -1);
    _hold_set(FALSE);
    g_assert_true(_submit("security_group.rule.add", "a", _h_hold));
    g_assert_true(_submit("dpdk.bind", "b", _h_hold));
    _wait_until(_running_at_least, GUINT_TO_POINTER(1));
    g_usleep(100000);
    g_mutex_lock(&g_fx.mu);
    g_assert_cmpuint(g_fx.handler_calls, ==, 1);
    g_mutex_unlock(&g_fx.mu);
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    g_assert_cmpuint(g_fx.max_running, ==, 1);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 2);
}

                                                                                                  
static void
test_ctr_mutate_lane(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    g_assert_cmpint(pcv_rpc_offload_lane_of("container.volume.attach"), ==, PCV_OFFLOAD_LANE_CTR_MUTATE);
    g_assert_cmpint(pcv_rpc_offload_lane_of("container.volume.detach"), ==, PCV_OFFLOAD_LANE_CTR_MUTATE);
    g_assert_cmpint(pcv_rpc_offload_lane_of("container.volume.list"), ==, PCV_OFFLOAD_LANE_HOST_READ);
    g_assert_cmpint(pcv_rpc_offload_lane_of("vm.snapshot.rollback"), ==, -1);
    _hold_set(FALSE);
    g_assert_true(_submit("container.volume.attach", "a", _h_hold));
    g_assert_true(_submit("container.volume.detach", "b", _h_hold));
    _wait_until(_running_at_least, GUINT_TO_POINTER(1));
    g_usleep(100000);
    g_mutex_lock(&g_fx.mu);
    g_assert_cmpuint(g_fx.handler_calls, ==, 1);
    g_mutex_unlock(&g_fx.mu);
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    g_assert_cmpuint(g_fx.max_running, ==, 1);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, 2);
}

                                                                                            
static void
_h_self_audit(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    (void)params;
    _note_enter();
    pcv_audit_log("actor", "t.storage", "vm-x", "fail", PURE_RPC_ERR_ZFS_OPERATION, 0, "local");
    pcv_rpc_completion_note_audit("t.storage");
    gchar *err = pure_rpc_build_error_response(rpc_id, PURE_RPC_ERR_ZFS_OPERATION, "VM not found");
    pure_uds_server_send_response(server, connection, err);
    g_free(err);
    _note_leave();
}

                                                                                            
                                                        
static void
test_registered_async_method_audits_once(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    g_fx.finish_async = TRUE;
    g_assert_true(_submit("t.storage", "self", _h_self_audit));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(1));
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);

    pcv_test_audit_reset();
    g_assert_true(_submit("t.storage", "late", _h_late));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(2));
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 0);                
    while (g_main_context_iteration(NULL, FALSE)) {}

    pcv_test_audit_reset();
    pcv_rpc_offload_set_now_fn_for_test(_fake_now);
    g_assert_true(_submit("t.storage", "hold", _h_ok));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(3));
    pcv_test_audit_reset();
    _hold_set(FALSE);
    g_assert_true(_submit("t.storage", "first", _h_hold));
    _wait_until(_running_at_least, GUINT_TO_POINTER(1));
    g_assert_true(_submit("t.storage", "stale", _h_ok));
    g_mutex_lock(&g_fx.mu);
    g_fx.now_us += ((gint64)PCV_OFFLOAD_MUTATE_MAX_WAIT_SEC + 1) * G_USEC_PER_SEC;
    g_mutex_unlock(&g_fx.mu);
    _hold_set(TRUE);
    _wait_until(_finish_at_least, GUINT_TO_POINTER(5));
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, PURE_RPC_ERR_BUSY);

    pcv_test_audit_reset();
    pcv_rpc_offload_set_terminating_fn_for_test(_always_terminating);
    g_assert_true(_submit("t.storage", "term", _h_ok));
    _wait_until(_finish_at_least, GUINT_TO_POINTER(6));
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, PURE_RPC_ERR_ZFS_OPERATION);
}

void
test_rpc_offload_register(void)
{
#define ADD(path, fn) g_test_add("/rpc_offload/" path, Fx, NULL, fx_setup, fn, fx_teardown)
    ADD("response_once", test_response_once);
    ADD("second_response_ignored", test_second_response_ignored);
    ADD("no_response_is_internal_error", test_no_response_is_internal_error);
    ADD("late_callback_passes_through", test_late_callback_passes_through);
    ADD("queue_full_busy", test_queue_full_busy);
    ADD("lane_parallel_cap", test_lane_parallel_cap);
    ADD("actor_scope_isolated", test_actor_scope_isolated);
    ADD("terminating_rejects_queued", test_terminating_rejects_queued);
    ADD("owner_scope_denied_in_worker", test_owner_scope_denied_in_worker);
    ADD("parser_lives_until_done", test_parser_lives_until_done);
    ADD("prom_paired", test_prom_paired);
    ADD("stale_queued_mutation_rejected", test_stale_queued_mutation_rejected);
    ADD("mutation_within_wait_runs", test_mutation_within_wait_runs);
    ADD("read_lane_no_stale_reject", test_read_lane_no_stale_reject);
    ADD("mutate_queue_cap_8", test_mutate_queue_cap_8);
    ADD("shutdown_joins_idle", test_shutdown_joins_idle);
    ADD("host_lane_installs_guard", test_host_lane_installs_guard);
    ADD("libvirt_and_mutate_lanes_no_guard", test_libvirt_and_mutate_lanes_no_guard);
    ADD("guard_cleared_after_job", test_guard_cleared_after_job);
    ADD("budget_per_request", test_budget_per_request);
    ADD("host_timeout_becomes_error", test_host_timeout_becomes_error);
    ADD("host_busy_becomes_error", test_host_busy_becomes_error);
    ADD("host_nonzero_keeps_response", test_host_nonzero_keeps_response);
    ADD("host_exec_failure_keeps_response", test_host_exec_failure_keeps_response);
    ADD("host_abort_becomes_shutting_down", test_host_abort_becomes_shutting_down);
    ADD("host_abort_wins_over_timeout", test_host_abort_wins_over_timeout);
    ADD("no_guard_lane_keeps_response", test_no_guard_lane_keeps_response);
    ADD("stall_mark_per_request", test_stall_mark_per_request);
    ADD("net_lane_serial", test_net_lane_serial);
    ADD("ctr_mutate_lane", test_ctr_mutate_lane);
    ADD("registered_async_method_audits_once", test_registered_async_method_audits_once);
#undef ADD
}
