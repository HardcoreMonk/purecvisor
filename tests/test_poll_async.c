   
                          
                                                                       
  
                           
                                                                                         
                                                                                  
                                                                                  
                                                                             
                                                              
                                                                      
                                                                             
                                                  
                                      
  
                       
                                                                  
                                                             
                    
   
#include <glib.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "api/drain.h"
#include "modules/audit/pcv_audit.h"
#include "modules/dispatcher/poll_async.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/rpc_utils.h"
#include "utils/pcv_proc_state.h"

                                
guint pcv_test_uds_response_count(void);
void pcv_test_uds_responses_reset(void);
const gchar *pcv_test_last_uds_response(void);
void pcv_test_audit_reset(void);
gint pcv_test_audit_call_count(void);
const gchar *pcv_test_audit_last_method(void);
const gchar *pcv_test_audit_last_result(void);
gint pcv_test_audit_last_error_code(void);
const gchar *pcv_test_audit_username_at(gint row);

                              
#define FAKE_BASE_US ((gint64)1000 * G_TIME_SPAN_SECOND)
                   
#define WAIT_DEADLINE_US ((gint64)10 * G_TIME_SPAN_SECOND)

typedef enum { SPAWN_OK, SPAWN_UNREAPED, SPAWN_EXIT1 } SpawnMode;

                                                             
                                            
static struct {
    GMutex   mu;
    gint64   now_us;
    SpawnMode mode;
    GPid     unreaped_pid;                                      
    guint    spawn_calls;
    guint    last_timeout_sec;
    gint     last_profile;
    gsize    last_max_output;
    gboolean last_abort_is_drain;
    gboolean last_new_group;
    gboolean last_has_unreaped_out;
    gchar   *last_argv0;
    guint    compute_calls;
    GQuark   guard_err_domain;                          
    gint     guard_err_code;
    gchar   *guard_err_msg;
    GCond    cond;                                            
    gboolean release;                                             
} g_fx;

typedef struct {
    gint unused;
} Fx;

                                                         
typedef struct {
    GObject *server;
    GObject *connection;
} FakePeer;

static gint64
_fake_now(void)
{
    g_mutex_lock(&g_fx.mu);
    gint64 v = g_fx.now_us;
    g_mutex_unlock(&g_fx.mu);
    return v;
}

static void
_fake_advance_ms(gint64 ms)
{
    g_mutex_lock(&g_fx.mu);
    g_fx.now_us += ms * 1000;
    g_mutex_unlock(&g_fx.mu);
}

static void
_fake_set_mode(SpawnMode mode, GPid pid)
{
    g_mutex_lock(&g_fx.mu);
    g_fx.mode = mode;
    g_fx.unreaped_pid = pid;
    g_mutex_unlock(&g_fx.mu);
}

static guint
_spawn_calls(void)
{
    g_mutex_lock(&g_fx.mu);
    guint n = g_fx.spawn_calls;
    g_mutex_unlock(&g_fx.mu);
    return n;
}

static guint
_compute_calls(void)
{
    g_mutex_lock(&g_fx.mu);
    guint n = g_fx.compute_calls;
    g_mutex_unlock(&g_fx.mu);
    return n;
}

                                                                                    
                                                                              
                                                                                  
static gboolean
_fake_spawn(const gchar * const *argv, const PcvSpawnBound *b, gchar **out, gchar **err,
            gint *status, GError **error)
{
    g_mutex_lock(&g_fx.mu);
    g_fx.spawn_calls++;
    g_fx.last_timeout_sec = b->timeout_sec;
    g_fx.last_profile = (gint)b->profile;
    g_fx.last_max_output = b->max_output_bytes;
    g_fx.last_abort_is_drain = (b->should_abort == pcv_drain_is_terminating);
    g_fx.last_new_group = b->new_process_group;
    g_fx.last_has_unreaped_out = (b->unreaped_pid_out != NULL);
    g_free(g_fx.last_argv0);
    g_fx.last_argv0 = g_strdup(argv[0]);
    SpawnMode mode = g_fx.mode;
    GPid pid = g_fx.unreaped_pid;
    g_mutex_unlock(&g_fx.mu);

    if (b->unreaped_pid_out)
        *b->unreaped_pid_out = 0;
    if (mode == SPAWN_UNREAPED) {
        if (b->unreaped_pid_out)
            *b->unreaped_pid_out = pid;
        *status = -1;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "%s: timed out after %us", argv[0],
                    b->timeout_sec);
        return FALSE;
    }
    if (mode == SPAWN_EXIT1) {
        if (out)
            *out = g_strdup("partial\n");
        if (err)
            *err = g_strdup("boom\n");
        *status = 1;
        return TRUE;
    }
    if (out)
        *out = g_strdup("ok\n");
    if (err)
        *err = g_strdup("");
    *status = 0;
    return TRUE;
}

                                                                   
static JsonNode *
_compute_count(JsonObject *params, PcvPollGuard *guard, GError **error)
{
    (void)guard; (void)error;
    g_mutex_lock(&g_fx.mu);
    guint n = ++g_fx.compute_calls;
    g_mutex_unlock(&g_fx.mu);
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "calls", n);
    if (params && json_object_has_member(params, "a"))
        json_object_set_int_member(o, "a", json_object_get_int_member(params, "a"));
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, o);
    return node;
}

                                                                                           
                                                                           
                           
static JsonNode *
_compute_spawn(JsonObject *params, PcvPollGuard *guard, GError **error)
{
    g_mutex_lock(&g_fx.mu);
    g_fx.compute_calls++;
    g_mutex_unlock(&g_fx.mu);
    _fake_advance_ms(params ? json_object_get_int_member_with_default(params, "advance_ms", 0) : 0);
    const gchar *argv[] = {
        params ? json_object_get_string_member_with_default(params, "argv0", "zpool") : "zpool",
        "list", NULL };
    gboolean want_err = params && json_object_get_boolean_member_with_default(params, "want_err", FALSE);
    gchar *out = NULL, *err = NULL;
    GError *e = NULL;
    if (!pcv_poll_guard_spawn(guard, argv, &out, want_err ? &err : NULL, &e)) {
        g_assert_null(out);
        g_assert_null(err);
        g_mutex_lock(&g_fx.mu);
        g_fx.guard_err_domain = e->domain;
        g_fx.guard_err_code = e->code;
        g_free(g_fx.guard_err_msg);
        g_fx.guard_err_msg = g_strdup(e->message);
        g_mutex_unlock(&g_fx.mu);
        g_propagate_error(error, e);
        return NULL;
    }
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "out", out);
    g_free(out);
    g_free(err);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, o);
    return node;
}

                                                                                               
                                                   
static JsonNode *
_compute_hold(JsonObject *params, PcvPollGuard *guard, GError **error)
{
    (void)params; (void)guard; (void)error;
    g_mutex_lock(&g_fx.mu);
    guint n = ++g_fx.compute_calls;
    while (!g_fx.release)
        g_cond_wait(&g_fx.cond, &g_fx.mu);
    g_mutex_unlock(&g_fx.mu);
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "calls", n);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, o);
    return node;
}

                                                          
static void
_hold_set(gboolean release)
{
    g_mutex_lock(&g_fx.mu);
    g_fx.release = release;
    g_cond_broadcast(&g_fx.cond);
    g_mutex_unlock(&g_fx.mu);
}

                                                                                 
static guint g_cap_warns;

static void
_count_cap_warn(const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer user_data)
{
    (void)domain; (void)level; (void)user_data;
    if (strstr(message, "refusing new requests until it completes"))
        g_cap_warns++;
}

                                                                          
static JsonNode *
_compute_fail_busy(JsonObject *params, PcvPollGuard *guard, GError **error)
{
    (void)params; (void)guard;
    g_set_error_literal(error, g_quark_from_static_string("test-poll"), PURE_RPC_ERR_BUSY, "pool is busy");
    return NULL;
}

                                                                                
static void
fx_setup(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    g_assert_true(g_main_context_acquire(NULL));
    g_mutex_lock(&g_fx.mu);
    g_fx.now_us = FAKE_BASE_US;
    g_fx.mode = SPAWN_OK;
    g_fx.unreaped_pid = 0;
    g_fx.spawn_calls = 0;
    g_fx.last_timeout_sec = 0;
    g_fx.last_profile = -1;
    g_fx.last_max_output = 0;
    g_fx.last_abort_is_drain = FALSE;
    g_fx.last_new_group = FALSE;
    g_fx.last_has_unreaped_out = FALSE;
    g_clear_pointer(&g_fx.last_argv0, g_free);
    g_fx.compute_calls = 0;
    g_fx.guard_err_domain = 0;
    g_fx.guard_err_code = 0;
    g_clear_pointer(&g_fx.guard_err_msg, g_free);
    g_fx.release = TRUE;
    g_mutex_unlock(&g_fx.mu);
    pcv_test_uds_responses_reset();
    pcv_test_audit_reset();
    pcv_poll_async_set_spawn_fn_for_test(_fake_spawn);
    pcv_poll_async_set_now_fn_for_test(_fake_now);
}

                                                                                 
static void
fx_teardown(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    pcv_poll_async_set_spawn_fn_for_test(NULL);
    pcv_poll_async_set_now_fn_for_test(NULL);
    g_main_context_release(NULL);
}

static void
_peer_new(FakePeer *p)
{
    p->server = g_object_new(G_TYPE_OBJECT, NULL);
    p->connection = g_object_new(G_TYPE_OBJECT, NULL);
    g_object_add_weak_pointer(p->server, (gpointer *)&p->server);
    g_object_add_weak_pointer(p->connection, (gpointer *)&p->connection);
}

                                                   
static void
_peer_release_and_assert_gone(FakePeer *p)
{
    g_object_unref(p->server);
    g_object_unref(p->connection);
    g_assert_null(p->server);
    g_assert_null(p->connection);
}

                                                       
static JsonObject *
_params(const gchar *json)
{
    JsonNode *n = json_from_string(json, NULL);
    g_assert_nonnull(n);
    JsonObject *o = json_object_ref(json_node_get_object(n));
    json_node_unref(n);
    return o;
}

                                                                        
                                                  
static void
_submit_as(const gchar *actor, const gchar *method, const gchar *params_json, const gchar *rpc_id,
           FakePeer *p, PcvPollComputeFn fn)
{
    JsonObject *params = params_json ? _params(params_json) : NULL;
    if (actor) {
        pcv_rpc_completion_begin(method);
        pcv_rpc_completion_set_actor(actor);
    }
    pcv_poll_async_submit(method, params, rpc_id, (UdsServer *)p->server,
                          (GSocketConnection *)p->connection, fn);
    if (actor)
        (void)pcv_rpc_completion_finish();
    if (params)
        json_object_unref(params);
}

                                                                                     
static void
_wait_responses(guint n)
{
    gint64 deadline = g_get_monotonic_time() + WAIT_DEADLINE_US;
    while (pcv_test_uds_response_count() < n && g_get_monotonic_time() < deadline)
        if (!g_main_context_iteration(NULL, FALSE))
            g_usleep(1000);
    gint64 settle = g_get_monotonic_time() + 50 * 1000;
    while (g_get_monotonic_time() < settle)
        if (!g_main_context_iteration(NULL, FALSE))
            g_usleep(1000);
    g_assert_cmpuint(pcv_test_uds_response_count(), ==, n);
}

                                                                                  
static JsonObject *
_last_response_member(JsonParser **parser_out, const gchar *rpc_id, const gchar *member)
{
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, pcv_test_last_uds_response(), -1, NULL));
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    g_assert_cmpstr(json_object_get_string_member(root, "id"), ==, rpc_id);
    g_assert_true(json_object_has_member(root, member));
    *parser_out = parser;
    return json_object_get_object_member(root, member);
}

                                                                                    
static void
test_coalesces_same_key(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *m = "test.poll.coalesce";
    FakePeer p[3];
    for (int i = 0; i < 3; i++)
        _peer_new(&p[i]);
    _submit_as(NULL, m, "{\"a\":1,\"b\":\"x\"}", "1", &p[0], _compute_count);
    _submit_as(NULL, m, "{\"b\":\"x\",\"a\":1}", "2", &p[1], _compute_count);
    _submit_as(NULL, m, "{\"a\":1,\"b\":\"x\"}", "3", &p[2], _compute_count);
    _wait_responses(3);
    g_assert_cmpuint(_compute_calls(), ==, 1);
    JsonParser *parser = NULL;
    JsonObject *res = _last_response_member(&parser, "3", "result");
    g_assert_cmpint(json_object_get_int_member(res, "calls"), ==, 1);
    g_assert_cmpint(json_object_get_int_member(res, "a"), ==, 1);                           
    g_object_unref(parser);
    for (int i = 0; i < 3; i++)
        _peer_release_and_assert_gone(&p[i]);

    FakePeer late;
    _peer_new(&late);
    _submit_as(NULL, m, "{\"a\":1,\"b\":\"x\"}", "4", &late, _compute_count);
    _wait_responses(4);
    g_assert_cmpuint(_compute_calls(), ==, 2);
    _peer_release_and_assert_gone(&late);
}

                                                            
static void
test_different_params_not_coalesced(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    FakePeer p[3];
    for (int i = 0; i < 3; i++)
        _peer_new(&p[i]);
    _submit_as(NULL, "test.poll.diff", "{\"a\":1}", "1", &p[0], _compute_count);
    _submit_as(NULL, "test.poll.diff", "{\"a\":2}", "2", &p[1], _compute_count);
    _submit_as(NULL, "test.poll.diff2", "{\"a\":1}", "3", &p[2], _compute_count);
    _wait_responses(3);
    g_assert_cmpuint(_compute_calls(), ==, 3);
    for (int i = 0; i < 3; i++)
        _peer_release_and_assert_gone(&p[i]);
}

                                                                          
                                                             
static void
test_command_timeout_10s(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    FakePeer p;
    _peer_new(&p);
    _submit_as(NULL, "test.poll.cmd", "{\"argv0\":\"zpool\"}", "1", &p, _compute_spawn);
    _wait_responses(1);
    g_assert_cmpuint(_spawn_calls(), ==, 1);
    g_assert_cmpuint(g_fx.last_timeout_sec, ==, 10);
    g_assert_cmpint(g_fx.last_profile, ==, PCV_CHILD_CAP_STORAGE);
    g_assert_cmpuint(g_fx.last_max_output, ==, (gsize)4 * 1024 * 1024);
    g_assert_true(g_fx.last_abort_is_drain);
    g_assert_true(g_fx.last_new_group);
    g_assert_true(g_fx.last_has_unreaped_out);
    g_assert_cmpstr(g_fx.last_argv0, ==, "zpool");
    JsonParser *parser = NULL;
    JsonObject *res = _last_response_member(&parser, "1", "result");
    g_assert_cmpstr(json_object_get_string_member(res, "out"), ==, "ok\n");
    g_object_unref(parser);

    _submit_as(NULL, "test.poll.cmd", "{\"argv0\":\"/usr/sbin/zfs\"}", "2", &p, _compute_spawn);
    _wait_responses(2);
    g_assert_cmpint(g_fx.last_profile, ==, PCV_CHILD_CAP_STORAGE);
    g_assert_cmpuint(g_fx.last_timeout_sec, ==, 10);

    _submit_as(NULL, "test.poll.cmd", "{\"argv0\":\"ip\"}", "3", &p, _compute_spawn);
    _wait_responses(3);
    g_assert_cmpint(g_fx.last_profile, ==, PCV_CHILD_CAP_BASE);
    g_assert_cmpuint(g_fx.last_timeout_sec, ==, 10);
    g_assert_cmpuint(_spawn_calls(), ==, 3);
    _peer_release_and_assert_gone(&p);
}

                                                                   
static void
test_command_timeout_shrinks_to_budget(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    FakePeer p;
    _peer_new(&p);
    _submit_as(NULL, "test.poll.shrink", "{\"argv0\":\"zpool\",\"advance_ms\":14000}", "1", &p,
               _compute_spawn);
    _wait_responses(1);
    g_assert_cmpuint(_spawn_calls(), ==, 1);
    g_assert_cmpuint(g_fx.last_timeout_sec, ==, 6);

    _submit_as(NULL, "test.poll.shrink", "{\"argv0\":\"zpool\",\"advance_ms\":10500}", "2", &p,
               _compute_spawn);
    _wait_responses(2);
    g_assert_cmpuint(g_fx.last_timeout_sec, ==, 9);
    _peer_release_and_assert_gone(&p);
}

                                                                              
                                      
static void
test_total_budget_20s(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    FakePeer p;
    _peer_new(&p);
    _submit_as(NULL, "test.poll.budget", "{\"argv0\":\"zpool\",\"advance_ms\":19500}", "1", &p,
               _compute_spawn);
    _wait_responses(1);
    g_assert_cmpuint(_spawn_calls(), ==, 0);
    g_assert_true(g_fx.guard_err_domain == G_IO_ERROR);
    g_assert_cmpint(g_fx.guard_err_code, ==, G_IO_ERROR_TIMED_OUT);
    JsonParser *parser = NULL;
    JsonObject *e = _last_response_member(&parser, "1", "error");
    g_assert_cmpint(json_object_get_int_member(e, "code"), ==, PURE_RPC_ERR_INTERNAL_ERROR);
    g_assert_cmpstr(json_object_get_string_member(e, "message"), ==, g_fx.guard_err_msg);
    g_object_unref(parser);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "test.poll.budget");
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, PURE_RPC_ERR_INTERNAL_ERROR);

    _submit_as(NULL, "test.poll.budget", "{\"argv0\":\"zpool\",\"advance_ms\":19000}", "2", &p,
               _compute_spawn);
    _wait_responses(2);
    g_assert_cmpuint(_spawn_calls(), ==, 1);
    g_assert_cmpuint(g_fx.last_timeout_sec, ==, 1);
    _peer_release_and_assert_gone(&p);
}

                                                               
                                                                                              
static void
test_stuck_child_blocks_new_spawn(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *m = "test.poll.stuck";
    pid_t child = fork();
    g_assert_cmpint(child, >=, 0);
    if (child == 0) {
                                                                    
        (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
        alarm(60);
        for (;;)
            pause();
    }
    g_assert_true(pcv_proc_pid_alive(child));
    FakePeer p;
    _peer_new(&p);

    _fake_set_mode(SPAWN_UNREAPED, child);
    _submit_as(NULL, m, "{\"argv0\":\"zpool\"}", "1", &p, _compute_spawn);
    _wait_responses(1);
    g_assert_cmpuint(_spawn_calls(), ==, 1);
    g_assert_cmpint(g_fx.guard_err_code, ==, G_IO_ERROR_TIMED_OUT);

    _fake_set_mode(SPAWN_OK, 0);
    _submit_as(NULL, m, "{\"argv0\":\"zfs\"}", "2", &p, _compute_spawn);
    _wait_responses(2);
    g_assert_cmpuint(_spawn_calls(), ==, 1);                
    g_assert_true(g_fx.guard_err_domain == G_IO_ERROR);
    g_assert_cmpint(g_fx.guard_err_code, ==, G_IO_ERROR_BUSY);
    g_assert_cmpstr(g_fx.guard_err_msg, ==, "previous 'zpool' for test.poll.stuck is still running");
    JsonParser *parser = NULL;
    JsonObject *e = _last_response_member(&parser, "2", "error");
    g_assert_cmpint(json_object_get_int_member(e, "code"), ==, PURE_RPC_ERR_INTERNAL_ERROR);
    g_assert_cmpstr(json_object_get_string_member(e, "message"), ==,
                    "previous 'zpool' for test.poll.stuck is still running");
    g_object_unref(parser);

    _submit_as(NULL, "test.poll.free", "{\"argv0\":\"zpool\"}", "3", &p, _compute_spawn);
    _wait_responses(3);
    g_assert_cmpuint(_spawn_calls(), ==, 2);                        

    g_assert_cmpint(kill(child, SIGKILL), ==, 0);
    int st = 0;
    g_assert_cmpint(waitpid(child, &st, 0), ==, child);
    g_assert_false(pcv_proc_pid_alive(child));

    _submit_as(NULL, m, "{\"argv0\":\"zfs\"}", "4", &p, _compute_spawn);
    _wait_responses(4);
    g_assert_cmpuint(_spawn_calls(), ==, 3);                          
    JsonObject *res = _last_response_member(&parser, "4", "result");
    g_assert_cmpstr(json_object_get_string_member(res, "out"), ==, "ok\n");
    g_object_unref(parser);
    _peer_release_and_assert_gone(&p);
}

                                                                 
                                                                               
static void
test_audit_per_waiter(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *m = "test.poll.audit";
    FakePeer a, b, c;
    _peer_new(&a);
    _peer_new(&b);
    _peer_new(&c);
    _submit_as("alice", m, "{}", "1", &a, _compute_count);
    _submit_as("bob", m, "{}", "2", &b, _compute_count);
    _submit_as(NULL, m, "{}", "3", &c, _compute_count);
    _wait_responses(3);
    g_assert_cmpuint(_compute_calls(), ==, 1);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 3);
    g_assert_cmpstr(pcv_test_audit_username_at(0), ==, "alice");
    g_assert_cmpstr(pcv_test_audit_username_at(1), ==, "bob");
    g_assert_cmpstr(pcv_test_audit_username_at(2), ==, PCV_AUDIT_ACTOR_LOCAL);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, m);
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "ok");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, 0);

    pcv_test_audit_reset();
    _submit_as("alice", m, "{}", "4", &a, _compute_fail_busy);
    _submit_as("bob", m, "{}", "5", &b, _compute_fail_busy);
    _wait_responses(5);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 2);
    g_assert_cmpstr(pcv_test_audit_username_at(0), ==, "alice");
    g_assert_cmpstr(pcv_test_audit_username_at(1), ==, "bob");
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, PURE_RPC_ERR_BUSY);
    JsonParser *parser = NULL;
    JsonObject *e = _last_response_member(&parser, "5", "error");
    g_assert_cmpint(json_object_get_int_member(e, "code"), ==, PURE_RPC_ERR_BUSY);
    g_assert_cmpstr(json_object_get_string_member(e, "message"), ==, "pool is busy");
    g_object_unref(parser);
    _peer_release_and_assert_gone(&a);
    _peer_release_and_assert_gone(&b);
    _peer_release_and_assert_gone(&c);
}

                                                                                             
                                                                                       
                                                                                                          
static void
test_nonzero_exit_reports_stderr(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    FakePeer p;
    _peer_new(&p);
    _fake_set_mode(SPAWN_EXIT1, 0);
    _submit_as(NULL, "test.poll.exit", "{\"argv0\":\"zpool\",\"want_err\":true}", "1", &p, _compute_spawn);
    _wait_responses(1);
    g_assert_cmpuint(_spawn_calls(), ==, 1);
    g_assert_true(g_fx.guard_err_domain == G_IO_ERROR);
    g_assert_cmpint(g_fx.guard_err_code, ==, G_IO_ERROR_FAILED);
    g_assert_cmpstr(g_fx.guard_err_msg, ==, "zpool: boom");
    JsonParser *parser = NULL;
    JsonObject *e = _last_response_member(&parser, "1", "error");
    g_assert_cmpint(json_object_get_int_member(e, "code"), ==, PURE_RPC_ERR_INTERNAL_ERROR);
    g_assert_cmpstr(json_object_get_string_member(e, "message"), ==, "zpool: boom");
    g_object_unref(parser);

    _submit_as(NULL, "test.poll.exit", "{\"argv0\":\"ip\"}", "2", &p, _compute_spawn);
    _wait_responses(2);
    g_assert_cmpuint(_spawn_calls(), ==, 2);
    g_assert_cmpint(g_fx.guard_err_code, ==, G_IO_ERROR_FAILED);
    g_assert_cmpstr(g_fx.guard_err_msg, ==, "ip: process exited with non-zero status");
    _peer_release_and_assert_gone(&p);
}

                                                                     
                                                                                                   
                                                                             
                                                              
static void
test_waiter_cap_refuses_with_busy(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *m = "test.poll.cap";
    g_cap_warns = 0;
    guint handler = g_log_set_handler("poll_async", G_LOG_LEVEL_WARNING, _count_cap_warn, NULL);
    FakePeer p;
    _peer_new(&p);
    guint sent = 0;
    for (guint round = 1; round <= 2; round++) {
        _hold_set(FALSE);
        pcv_test_audit_reset();
        for (gint i = 0; i < 16; i++)
            _submit_as("waiter", m, NULL, "w", &p, _compute_hold);
        g_assert_cmpuint(pcv_test_uds_response_count(), ==, sent);                    
        g_assert_cmpint(pcv_test_audit_call_count(), ==, 0);

        _submit_as("late", m, NULL, "17", &p, _compute_hold);                          
        g_assert_cmpuint(pcv_test_uds_response_count(), ==, sent + 1);
        JsonParser *parser = NULL;
        JsonObject *e = _last_response_member(&parser, "17", "error");
        g_assert_cmpint(json_object_get_int_member(e, "code"), ==, PURE_RPC_ERR_BUSY);
        g_assert_cmpstr(json_object_get_string_member(e, "message"), ==,
                        "test.poll.cap is still running from an earlier request");
        g_object_unref(parser);
        g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
        g_assert_cmpstr(pcv_test_audit_username_at(0), ==, "late");
        g_assert_cmpstr(pcv_test_audit_last_method(), ==, m);
        g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
        g_assert_cmpint(pcv_test_audit_last_error_code(), ==, PURE_RPC_ERR_BUSY);
        _submit_as(NULL, m, NULL, "18", &p, _compute_hold);                                         
        g_assert_cmpuint(pcv_test_uds_response_count(), ==, sent + 2);
        g_assert_cmpstr(pcv_test_audit_username_at(1), ==, PCV_AUDIT_ACTOR_LOCAL);
        g_assert_cmpuint(g_cap_warns, ==, round);

        _hold_set(TRUE);
        sent += 2 + 16;
        _wait_responses(sent);                                                              
        g_assert_cmpuint(_compute_calls(), ==, round);                                         
        g_assert_cmpint(pcv_test_audit_call_count(), ==, 2 + 16);
        g_assert_cmpstr(pcv_test_audit_last_result(), ==, "ok");
        JsonObject *res = _last_response_member(&parser, "w", "result");
        g_assert_cmpint(json_object_get_int_member(res, "calls"), ==, round);
        g_object_unref(parser);
    }
    g_log_remove_handler("poll_async", handler);
    _peer_release_and_assert_gone(&p);
}

                                                                     
static void
test_guard_public_budget(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *argv[] = { "zfs", "list", NULL };
    PcvPollGuard *g = pcv_poll_guard_new("t.guard");
    gchar *out = NULL;
    g_assert_true(pcv_poll_guard_spawn(g, argv, &out, NULL, NULL));
    g_clear_pointer(&out, g_free);
    g_assert_cmpuint(g_fx.last_timeout_sec, ==, 10);
    _fake_advance_ms(15000);
    g_assert_true(pcv_poll_guard_spawn(g, argv, &out, NULL, NULL));
    g_clear_pointer(&out, g_free);
    g_assert_cmpuint(g_fx.last_timeout_sec, ==, 5);
    _fake_advance_ms(4500);
    guint calls = _spawn_calls();
    GError *e = NULL;
    g_assert_false(pcv_poll_guard_spawn(g, argv, &out, NULL, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error(&e);
    g_assert_cmpuint(_spawn_calls(), ==, calls);
    pcv_poll_guard_free(g);
    pcv_poll_guard_free(NULL);
}

                                                                                   
                                                          
static void
test_guard_override_keeps_output_on_exit(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *argv[] = { "zfs", "list", NULL };
    PcvPollGuard *g = pcv_poll_guard_new("t.guard2");
    gchar *out = NULL, *err = NULL;
    GError *e = NULL;
    _fake_set_mode(SPAWN_EXIT1, 0);
    g_assert_false(pcv_poll_guard_spawn_override(g, argv, &out, &err, &e));
    g_assert_error(e, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(e->message, ==, "zfs: boom");
    g_assert_cmpstr(out, ==, "partial\n");
    g_assert_cmpstr(err, ==, "boom");
    g_clear_error(&e);
    g_clear_pointer(&out, g_free);
    g_clear_pointer(&err, g_free);
    g_assert_false(pcv_poll_guard_spawn(g, argv, &out, &err, &e));
    g_assert_null(out);
    g_assert_null(err);
    g_clear_error(&e);
    pcv_poll_guard_free(g);
}

                                                                    
static void
test_guard_override_keeps_auto_profile(Fx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    PcvPollGuard *g = pcv_poll_guard_new("t.guard3");
    static const struct { const gchar *cmd; } cases[] = { {"zfs"}, {"tar"}, {"lxc-info"}, {"pkill"}, {"dnsmasq"}, {"/bin/sh"}, {"ip"} };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        const gchar *argv[] = { cases[i].cmd, "x", NULL };
        gchar *out = NULL;
        g_assert_true(pcv_poll_guard_spawn_override(g, argv, &out, NULL, NULL));
        g_free(out);
        g_assert_cmpint(g_fx.last_profile, ==, (gint)pcv_spawn_capability_profile_for_argv(argv));
    }
    pcv_poll_guard_free(g);
}

void
test_poll_async_register(void)
{
    g_test_add("/poll_async/guard_public_budget", Fx, NULL, fx_setup, test_guard_public_budget, fx_teardown);
    g_test_add("/poll_async/guard_override_keeps_output_on_exit", Fx, NULL,
               fx_setup, test_guard_override_keeps_output_on_exit, fx_teardown);
    g_test_add("/poll_async/guard_override_keeps_auto_profile", Fx, NULL,
               fx_setup, test_guard_override_keeps_auto_profile, fx_teardown);
    g_test_add("/poll_async/coalesces_same_key", Fx, NULL,
               fx_setup, test_coalesces_same_key, fx_teardown);
    g_test_add("/poll_async/different_params_not_coalesced", Fx, NULL,
               fx_setup, test_different_params_not_coalesced, fx_teardown);
    g_test_add("/poll_async/command_timeout_10s", Fx, NULL,
               fx_setup, test_command_timeout_10s, fx_teardown);
    g_test_add("/poll_async/command_timeout_shrinks_to_budget", Fx, NULL,
               fx_setup, test_command_timeout_shrinks_to_budget, fx_teardown);
    g_test_add("/poll_async/total_budget_20s", Fx, NULL,
               fx_setup, test_total_budget_20s, fx_teardown);
    g_test_add("/poll_async/stuck_child_blocks_new_spawn", Fx, NULL,
               fx_setup, test_stuck_child_blocks_new_spawn, fx_teardown);
    g_test_add("/poll_async/audit_per_waiter", Fx, NULL,
               fx_setup, test_audit_per_waiter, fx_teardown);
    g_test_add("/poll_async/nonzero_exit_reports_stderr", Fx, NULL,
               fx_setup, test_nonzero_exit_reports_stderr, fx_teardown);
    g_test_add("/poll_async/waiter_cap_refuses_with_busy", Fx, NULL,
               fx_setup, test_waiter_cap_refuses_with_busy, fx_teardown);
}
