                                                                                                                          
                                                                                               
                                                                                          
                                  
                                                                                                  
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <sqlite3.h>
#include <string.h>

#include "../src/api/rest_login.h"
#include "../src/modules/auth/pcv_rbac.h"
#include "../src/modules/auth/pcv_rbac_internal.h"
#include "../src/utils/pcv_jwt.h"

                   
void pcv_rest_login_set_hooks_for_test(void (*run_fn)(const PcvRestLoginRequest *, PcvRestLoginResult *),
                                       gboolean (*terminating_fn)(void));
gint pcv_rest_login_inflight_for_test(void);
void pcv_rbac_set_pbkdf2_probe_for_test(void (*probe)(gint iter, gboolean db_locked));
void pcv_rbac_attempts_reset_for_test(guint cards_max, gint64 (*now_fn)(void));

#define WAIT_MAX_MS 8000

                       
typedef struct {
    GMainContext *ctx;
    GMainLoop    *loop;
    SoupServer   *server;
    GThread      *thread;
    guint         port;
    GThread      *rest_thread;
    gint          ready;
} Srv;

static void
srv_handler(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer data)
{
    (void)server; (void)query; (void)data;
    pcv_rest_login_start(msg, g_main_context_get_thread_default(), path + 1, "client-password", "192.0.2.77");
}

static gpointer
srv_main(gpointer data)
{
    Srv *s = data;
    g_main_context_push_thread_default(s->ctx);
    s->server = soup_server_new(NULL, NULL);
    soup_server_add_handler(s->server, NULL, srv_handler, s, NULL);
    GError *err = NULL;
    g_assert_true(soup_server_listen_local(s->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &err));
    g_assert_no_error(err);
    GSList *uris = soup_server_get_uris(s->server);
    g_assert_nonnull(uris);
    s->port = (guint)g_uri_get_port(uris->data);
    g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
    s->rest_thread = g_thread_self();
    g_atomic_int_set(&s->ready, 1);
    g_main_loop_run(s->loop);
    soup_server_disconnect(s->server);
    while (g_main_context_iteration(s->ctx, FALSE)) { }
    g_object_unref(s->server);
    g_main_context_pop_thread_default(s->ctx);
    return NULL;
}

static Srv *
srv_start(void)
{
    Srv *s = g_new0(Srv, 1);
    s->ctx = g_main_context_new();
    s->loop = g_main_loop_new(s->ctx, FALSE);
    s->thread = g_thread_new("login-rest", srv_main, s);
    for (int i = 0; i < WAIT_MAX_MS && !g_atomic_int_get(&s->ready); i++)
        g_usleep(1000);
    g_assert_cmpint(g_atomic_int_get(&s->ready), ==, 1);
    return s;
}

static gboolean
srv_quit(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

static void
srv_stop(Srv *s)
{
    g_main_context_invoke(s->ctx, srv_quit, s->loop);
    g_thread_join(s->thread);
    g_main_loop_unref(s->loop);
    g_main_context_unref(s->ctx);
    g_free(s);
}

                 
static GSocketConnection *
client_send(Srv *s, const gchar *name)
{
    g_autoptr(GSocketClient) c = g_socket_client_new();
    g_socket_client_set_timeout(c, 300);                                                             
    GError *err = NULL;
    GSocketConnection *conn = g_socket_client_connect_to_host(c, "127.0.0.1", (guint16)s->port, NULL, &err);
    g_assert_no_error(err);
    g_autofree gchar *req = g_strdup_printf("POST /%s HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", name);
    gsize wrote = 0;
    g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), req, strlen(req), &wrote, NULL, &err));
    g_assert_no_error(err);
    return conn;
}

                                                           
static gchar *
client_read(GSocketConnection *conn)
{
    GString *out = g_string_new(NULL);
    gchar buf[1024];
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    for (;;) {
        gssize n = g_input_stream_read(in, buf, sizeof(buf), NULL, NULL);
        if (n <= 0) break;
        g_string_append_len(out, buf, n);
    }
    g_object_unref(conn);
    return g_string_free(out, FALSE);
}

static gboolean
wait_int(gint (*get)(void), gint want)
{
    for (int i = 0; i < WAIT_MAX_MS; i++) {
        if (get() == want) return TRUE;
        g_usleep(1000);
    }
    return FALSE;
}

                           
static GMutex g_gate_mu;
static GCond  g_gate_cond;
static gboolean g_gate_open = FALSE;
static gint g_running = 0, g_max_running = 0, g_runs = 0;
static GThread *g_run_thread = NULL;

static void
fake_run(const PcvRestLoginRequest *in, PcvRestLoginResult *out)
{
    g_assert_cmpstr(in->client_ip, ==, "192.0.2.77");
    g_assert_cmpstr(in->password, ==, "client-password");
    g_mutex_lock(&g_gate_mu);
    g_runs++;
    g_running++;
    if (g_running > g_max_running) g_max_running = g_running;
    g_run_thread = g_thread_self();
    while (!g_gate_open)
        g_cond_wait(&g_gate_cond, &g_gate_mu);
    g_running--;
    g_mutex_unlock(&g_gate_mu);
    out->status = 401;
    out->err_code = "UNAUTHORIZED";
    out->err_msg = g_strdup("Invalid credentials");
}

static gint
get_running(void)
{
    g_mutex_lock(&g_gate_mu);
    gint v = g_running;
    g_mutex_unlock(&g_gate_mu);
    return v;
}

static void
fake_setup(gboolean open)
{
    g_mutex_lock(&g_gate_mu);
    g_gate_open = open;
    g_running = g_max_running = g_runs = 0;
    g_run_thread = NULL;
    g_mutex_unlock(&g_gate_mu);
    pcv_rest_login_set_hooks_for_test(fake_run, NULL);
}

static void
gate_release(void)
{
    g_mutex_lock(&g_gate_mu);
    g_gate_open = TRUE;
    g_cond_broadcast(&g_gate_cond);
    g_mutex_unlock(&g_gate_mu);
}

static void
fake_teardown(void)
{
    pcv_rest_login_set_hooks_for_test(NULL, NULL);
    pcv_rest_login_pool_shutdown();
}

                                                     
static void
test_runs_off_rest_thread(void)
{
    fake_setup(TRUE);
    Srv *s = srv_start();
    g_autofree gchar *r = client_read(client_send(s, "any-name"));
    g_assert_true(g_str_has_prefix(r, "HTTP/1.1 401"));
    g_assert_nonnull(strstr(r, "Invalid credentials"));
    g_assert_null(g_strstr_len(r, -1, "Retry-After"));
    g_assert_nonnull(g_run_thread);
    g_assert_true(g_run_thread != s->rest_thread);
    g_assert_true(g_run_thread != g_thread_self());
    srv_stop(s);
    fake_teardown();
}

                                                                        
static void
test_overflow_is_fast_429(void)
{
    fake_setup(FALSE);
    Srv *s = srv_start();
    GSocketConnection *held[PCV_REST_LOGIN_INFLIGHT_MAX];
    for (int i = 0; i < PCV_REST_LOGIN_INFLIGHT_MAX; i++) {
        g_autofree gchar *name = g_strdup_printf("queued-%d", i);
        held[i] = client_send(s, name);
    }
    g_assert_true(wait_int(pcv_rest_login_inflight_for_test, PCV_REST_LOGIN_INFLIGHT_MAX));
    g_assert_true(wait_int(get_running, PCV_REST_LOGIN_WORKERS));

    const gchar *names[] = { "over-a", "a-different-over-name" };
    g_autofree gchar *first = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autofree gchar *r = client_read(client_send(s, names[i]));
        g_assert_true(g_str_has_prefix(r, "HTTP/1.1 429"));
        g_assert_nonnull(strstr(r, "Retry-After: 2"));
        const gchar *body = strstr(r, "\r\n\r\n");
        g_assert_nonnull(body);
        g_assert_nonnull(strstr(body, "Login service is busy"));
        if (!first) first = g_strdup(body);
        else g_assert_cmpstr(body, ==, first);                       
    }
    g_assert_cmpint(g_runs, ==, PCV_REST_LOGIN_WORKERS);                          
    g_assert_cmpint(pcv_rest_login_inflight_for_test(), ==, PCV_REST_LOGIN_INFLIGHT_MAX);

    gate_release();
    for (int i = 0; i < PCV_REST_LOGIN_INFLIGHT_MAX; i++) {
        g_autofree gchar *r = client_read(held[i]);
        g_assert_true(g_str_has_prefix(r, "HTTP/1.1 401"));
    }
    g_assert_true(wait_int(pcv_rest_login_inflight_for_test, 0));
    g_assert_cmpint(g_runs, ==, PCV_REST_LOGIN_INFLIGHT_MAX);
    g_assert_cmpint(g_max_running, ==, PCV_REST_LOGIN_WORKERS);
    srv_stop(s);
    fake_teardown();
}

                                                              
static void
test_slots_return_after_overflow(void)
{
    fake_setup(FALSE);
    Srv *s = srv_start();
    GSocketConnection *held[PCV_REST_LOGIN_INFLIGHT_MAX];
    for (int i = 0; i < PCV_REST_LOGIN_INFLIGHT_MAX; i++)
        held[i] = client_send(s, "fill");
    g_assert_true(wait_int(pcv_rest_login_inflight_for_test, PCV_REST_LOGIN_INFLIGHT_MAX));
    g_autofree gchar *over = client_read(client_send(s, "over"));
    g_assert_true(g_str_has_prefix(over, "HTTP/1.1 429"));
    gate_release();
    for (int i = 0; i < PCV_REST_LOGIN_INFLIGHT_MAX; i++)
        g_free(client_read(held[i]));
    g_assert_true(wait_int(pcv_rest_login_inflight_for_test, 0));
    g_autofree gchar *again = client_read(client_send(s, "after"));
    g_assert_true(g_str_has_prefix(again, "HTTP/1.1 401"));
    srv_stop(s);
    fake_teardown();
}

static gboolean
always_terminating(void)
{
    return TRUE;
}

                              
static void
test_terminating_skips_run(void)
{
    fake_setup(TRUE);
    pcv_rest_login_set_hooks_for_test(fake_run, always_terminating);
    Srv *s = srv_start();
    g_autofree gchar *r = client_read(client_send(s, "name"));
    g_assert_true(g_str_has_prefix(r, "HTTP/1.1 503"));
    g_assert_cmpint(g_runs, ==, 0);
    g_assert_true(wait_int(pcv_rest_login_inflight_for_test, 0));
    srv_stop(s);
    fake_teardown();
}

                                              
static void
test_client_disconnect_returns_slot(void)
{
    fake_setup(FALSE);
    Srv *s = srv_start();
    GSocketConnection *gone[3];
    for (int i = 0; i < 3; i++)
        gone[i] = client_send(s, "gone");
    g_assert_true(wait_int(pcv_rest_login_inflight_for_test, 3));
    for (int i = 0; i < 3; i++) {
        g_io_stream_close(G_IO_STREAM(gone[i]), NULL, NULL);
        g_object_unref(gone[i]);
    }
    g_usleep(50 * 1000);                         
    gate_release();
    g_assert_true(wait_int(pcv_rest_login_inflight_for_test, 0));
    g_autofree gchar *r = client_read(client_send(s, "next"));
    g_assert_true(g_str_has_prefix(r, "HTTP/1.1 401"));
    srv_stop(s);
    fake_teardown();
}

                 
static gchar *g_dir = NULL, *g_db = NULL;
static GThread *g_hash_thread = NULL;
static gint g_hash_calls = 0;

static void
hash_probe(gint iter, gboolean db_locked)
{
    (void)iter;
    g_assert_false(db_locked);
    g_hash_thread = g_thread_self();
    g_atomic_int_inc(&g_hash_calls);
}

static void
db_setup(void)
{
    g_dir = g_dir_make_tmp("pcv-rest-login-XXXXXX", NULL);
    g_db = g_build_filename(g_dir, "rbac.db", NULL);
    pcv_jwt_init("rest-login-test-secret-32-bytes!!!!!");
    pcv_rbac_init(g_db);
    pcv_rbac_attempts_reset_for_test(0, NULL);
    g_hash_thread = NULL;
    g_hash_calls = 0;
    pcv_rbac_set_pbkdf2_probe_for_test(hash_probe);
}

static void
db_teardown(void)
{
    pcv_rbac_set_pbkdf2_probe_for_test(NULL);
    pcv_rbac_attempts_reset_for_test(0, NULL);
    pcv_rbac_shutdown();
    pcv_jwt_shutdown();
    gchar *wal = g_strconcat(g_db, "-wal", NULL), *shm = g_strconcat(g_db, "-shm", NULL);
    g_unlink(g_db); g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm);
    g_rmdir(g_dir);
    g_clear_pointer(&g_db, g_free);
    g_clear_pointer(&g_dir, g_free);
}

                                                              
static void
test_real_hash_off_rest_thread(void)
{
    db_setup();
    Srv *s = srv_start();
    g_autofree gchar *r = client_read(client_send(s, "no-such-name"));
    g_assert_true(g_str_has_prefix(r, "HTTP/1.1 401"));
    g_assert_nonnull(strstr(r, "Invalid credentials"));
    g_assert_cmpint(g_atomic_int_get(&g_hash_calls), ==, 1);
    g_assert_nonnull(g_hash_thread);
    g_assert_true(g_hash_thread != s->rest_thread);
    srv_stop(s);
    pcv_rest_login_pool_shutdown();
    db_teardown();
}

                                                                           
static void
test_run_results(void)
{
    db_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("run-user", "Right-Passw0rd!", PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
    PcvRestLoginRequest in = { .username = (gchar *)"run-user", .password = (gchar *)"Right-Passw0rd!", .client_ip = (gchar *)"192.0.2.90" };
    PcvRestLoginResult out = { 0 };

    pcv_rest_login_run(&in, &out);
    g_assert_cmpuint(out.status, ==, 200);
    g_assert_nonnull(strstr(out.json, "\"access_token\""));
    g_assert_nonnull(strstr(out.json, "\"refresh_token\""));
    g_assert_cmpint(out.retry_after, ==, 0);
    pcv_rest_login_result_clear(&out);

                                                          
    const gchar *names[] = { "run-user", "run-ghost" };
    for (guint n = 0; n < G_N_ELEMENTS(names); n++) {
        for (int i = 0; i < 4; i++)
            pcv_rbac_attempts_login_record(names[n], FALSE);
        PcvRestLoginRequest bad = { .username = (gchar *)names[n], .password = (gchar *)"wrong-password", .client_ip = (gchar *)"192.0.2.91" };
        pcv_rest_login_run(&bad, &out);
        g_assert_cmpuint(out.status, ==, 429);
        g_assert_cmpstr(out.err_code, ==, "TOO_MANY_REQUESTS");
        g_assert_cmpstr(out.err_msg, ==, "Invalid credentials");
        g_assert_cmpint(out.retry_after, >, 0);
        pcv_rest_login_result_clear(&out);
        gint before = g_atomic_int_get(&g_hash_calls);
        pcv_rest_login_run(&bad, &out);
        g_assert_cmpuint(out.status, ==, 429);
        g_assert_true(g_str_has_prefix(out.err_msg, "Account locked — retry after "));
        g_assert_cmpint(g_atomic_int_get(&g_hash_calls), ==, before);                         
        pcv_rest_login_result_clear(&out);
    }

                                           
    for (int i = 0; i < 18; i++)
        pcv_rbac_ip_record_auth_failure("192.0.2.92");
    PcvRestLoginRequest ipbad = { .username = (gchar *)"run-user-2", .password = (gchar *)"wrong-password", .client_ip = (gchar *)"192.0.2.92" };
    pcv_rest_login_run(&ipbad, &out);
    g_assert_cmpuint(out.status, ==, 401);
    g_assert_cmpstr(out.err_msg, ==, "Invalid credentials");
    pcv_rest_login_result_clear(&out);
    g_assert_cmpint(pcv_rbac_get_ip_remaining_lockout("192.0.2.92"), ==, 0);
    pcv_rbac_ip_record_auth_failure("192.0.2.92");
    g_assert_cmpint(pcv_rbac_get_ip_remaining_lockout("192.0.2.92"), >, 0);

                                                                   
    gint before_ip = g_atomic_int_get(&g_hash_calls);
    pcv_rest_login_run(&ipbad, &out);
    g_assert_cmpuint(out.status, ==, 429);
    g_assert_cmpstr(out.err_msg, ==, "Too many failed login attempts from this IP");
    g_assert_cmpint(out.retry_after, >, 0);
    g_assert_cmpint(g_atomic_int_get(&g_hash_calls), ==, before_ip);
    pcv_rest_login_result_clear(&out);
    db_teardown();
}

                                                        
static void
test_run_bootstrap_refused(void)
{
    db_setup();
    PcvRestLoginRequest in = { .username = (gchar *)"boot-admin", .password = (gchar *)"boot-pass-1234", .client_ip = (gchar *)"192.0.2.93",
                               .cfg_user = (gchar *)"boot-admin", .cfg_pass = (gchar *)"boot-pass-1234" };
    PcvRestLoginResult out = { 0 };
    pcv_rest_login_run(&in, &out);
    g_assert_cmpuint(out.status, ==, 401);
    g_assert_null(out.json);
    g_assert_cmpstr(out.err_msg, ==, "Invalid credentials");
    pcv_rest_login_result_clear(&out);
    db_teardown();
}

void
test_rest_login_register(void)
{
    g_test_add_func("/rest_login/runs_off_rest_thread", test_runs_off_rest_thread);
    g_test_add_func("/rest_login/overflow_is_fast_429", test_overflow_is_fast_429);
    g_test_add_func("/rest_login/slots_return_after_overflow", test_slots_return_after_overflow);
    g_test_add_func("/rest_login/terminating_skips_run", test_terminating_skips_run);
    g_test_add_func("/rest_login/client_disconnect_returns_slot", test_client_disconnect_returns_slot);
    g_test_add_func("/rest_login/real_hash_off_rest_thread", test_real_hash_off_rest_thread);
    g_test_add_func("/rest_login/run_results", test_run_results);
    g_test_add_func("/rest_login/run_bootstrap_refused", test_run_bootstrap_refused);
}
