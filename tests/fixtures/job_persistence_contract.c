



#include <sqlite3.h>
#include <stdio.h>
#include "src/api/ws_server.c"
#include "src/modules/dispatcher/handler_backup.c"

static const gchar *mode;
static sqlite3 *external;
static GPtrArray *frames;
static gchar *rpc_response;
static gint effects, audit_count;
static gchar *audit_result;
static GThread *main_thread;

void _pcv_log(GLogLevelFlags level, const gchar *domain, const gchar *fmt, ...)
{ (void)level; (void)domain; (void)fmt; }
const gchar *pcv_config_get_string(const gchar *section, const gchar *key, const gchar *fallback)
{ (void)section; (void)key; return fallback; }


void __wrap_soup_websocket_connection_send_text(SoupWebsocketConnection *connection, const char *text)
{
    (void)connection;
    g_assert_true(g_thread_self() == main_thread);
    g_ptr_array_add(frames, g_strdup(text));
}

void pure_uds_server_send_response(UdsServer *server, GSocketConnection *connection, const gchar *text)
{
    (void)server; (void)connection;
    g_free(rpc_response); rpc_response = g_strdup(text);
}

static void sql(const gchar *query)
{ g_assert_cmpint(sqlite3_exec(external, query, NULL, NULL, NULL), ==, SQLITE_OK); }

gboolean pcv_backup_restore(const gchar *vm, const gchar *snapshot, gboolean *was_running, GError **error)
{
    if (was_running) *was_running = g_str_has_prefix(mode, "running");
    (void)vm; (void)snapshot;
    g_atomic_int_inc(&effects);
    if (g_str_has_prefix(mode, "locked")) sql("BEGIN IMMEDIATE");
    if (g_str_equal(mode, "locked-failure") || g_str_equal(mode, "running-rollback-fail")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "backend failed\n\"quoted\" \\path");
        return FALSE;
    }
    return TRUE;
}

static gint start_submits;
static gint fixture_lock_token;
PcvVmLock *pcv_vm_lock_acquire(const gchar *id, gint operation, gchar **error)
{
    (void)id; (void)operation;
    if (g_str_equal(mode, "vm-busy")) { if (error) *error = g_strdup("fixture: VM busy"); return NULL; }
    return (PcvVmLock *)&fixture_lock_token;
}
void pcv_vm_lock_release(PcvVmLock *lock) { (void)lock; }
PcvVmStartSubmit pcv_vm_start_submit(const PcvVmStartRequest *req, PcvVmStartDoneFn done,
                                     gpointer user_data, gchar **err_msg)
{
    g_atomic_int_inc(&start_submits);
    if (g_str_equal(mode, "running-submit-rejected")) { *err_msg = g_strdup("fixture: start busy"); return PCV_VM_START_REJECTED; }
    gboolean ok = !g_str_equal(mode, "running-restart-fail");
    GError *e = ok ? NULL : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "fixture: create failed");
    done(req->vm_id, ok, e, user_data);
    if (e) g_error_free(e);
    return PCV_VM_START_SUBMITTED;
}
void pcv_vm_start_report_rejected(const gchar *vm_id, const gchar *origin, const gchar *reason, const gchar *actor)
{ (void)vm_id; (void)origin; (void)reason; (void)actor; }

void pcv_audit_log(const gchar *user, const gchar *method, const gchar *target,
                   const gchar *result, gint code, gint64 duration, const gchar *source)
{
    (void)user; (void)method; (void)target; (void)code; (void)duration; (void)source;
    g_atomic_int_inc(&audit_count);
    g_free(audit_result); audit_result = g_strdup(result);
}

static GIOStream *peer_stream;
static void setup_ws(void)
{
    int sockets[2];
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    GSocket *first = g_socket_new_from_fd(sockets[0], NULL);
    GSocket *second = g_socket_new_from_fd(sockets[1], NULL);
    GSocketConnection *stream = g_socket_connection_factory_create_connection(first);
    peer_stream = G_IO_STREAM(g_socket_connection_factory_create_connection(second));
    GUri *uri = g_uri_parse("ws://localhost/fixture", G_URI_FLAGS_NONE, NULL);
    SoupWebsocketConnection *connection = soup_websocket_connection_new(
        G_IO_STREAM(stream), uri, SOUP_WEBSOCKET_CONNECTION_SERVER, NULL, NULL, NULL);
    g_uri_unref(uri); g_object_unref(stream); g_object_unref(first); g_object_unref(second);

    g_object_set_data(G_OBJECT(connection), "pcv-ws-authed", GINT_TO_POINTER(1));
    g_mutex_init(&G.mu);
    G.clients = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(G.clients, connection);

    G.ctx = g_main_context_ref(g_main_context_default());
    G.initialized = TRUE;
}

static void wait_frame(void)
{
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (!frames->len && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    g_assert_cmpuint(frames->len, ==, 1);
}

static void print_frame(void)
{
    printf("FRAME %s\n", (gchar *)g_ptr_array_index(frames, 0));
    printf("EFFECTS %d AUDIT %d %s\n", g_atomic_int_get(&effects),
           g_atomic_int_get(&audit_count), audit_result ? audit_result : "none");
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 3); mode = argv[1];
    main_thread = g_thread_self();
    g_assert_true(g_main_context_acquire(g_main_context_default()));
    frames = g_ptr_array_new_with_free_func(g_free);
    pcv_drain_init(); setup_ws();
    g_setenv("PCV_JOBS_DB_PATH", argv[2], TRUE); pcv_job_queue_init();
    g_assert_cmpint(sqlite3_open(argv[2], &external), ==, SQLITE_OK);

    if (g_str_equal(mode, "legacy")) {
        pcv_ws_broadcast_job_complete("legacy:probe", "vm.start", "completed", NULL);
    } else if (g_str_equal(mode, "admission")) {
        sql("BEGIN IMMEDIATE");
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "vm_name", "probe");
        json_object_set_string_member(params, "snapshot_name", "snapshot");
        handle_backup_restore(params, "1", NULL, NULL);
        g_assert_cmpint(effects, ==, 0); g_assert_cmpuint(frames->len, ==, 0);
        printf("DENIED %s\n", rpc_response);
        sql("ROLLBACK");
        handle_backup_restore(params, "2", NULL, NULL);
        json_object_unref(params); wait_frame();
        printf("ACCEPTED %s\n", rpc_response);
    } else if (g_str_equal(mode, "vm-busy")) {
        JsonObject *params = json_object_new();
        json_object_set_string_member(params, "vm_name", "probe");
        json_object_set_string_member(params, "snapshot_name", "snapshot");
        handle_backup_restore(params, "1", NULL, NULL);
        json_object_unref(params);
        g_assert_cmpint(effects, ==, 0); g_assert_cmpuint(frames->len, ==, 0);
        printf("BUSY %s\n", rpc_response);
    } else {
        PcvJobRef *ref = pcv_job_admit("backup.restore", "probe@snapshot", NULL, "backup.restore");
        g_assert_nonnull(ref);
        gchar *id = g_strdup(pcv_job_id(ref));
        RestoreTaskData *data = g_new0(RestoreTaskData, 1);
        data->vm_name = g_strdup("probe"); data->snapshot_name = g_strdup("snapshot");
        data->job_id = id;
        data->job_ref = ref;
        GTask *task = _restore_task_new(data);
        g_task_run_in_thread(task, _restore_worker);
        g_object_unref(task); wait_frame();
        if (g_str_has_prefix(mode, "locked")) sql("ROLLBACK");
    }
    if (!g_str_equal(mode, "vm-busy")) print_frame();
    printf("STARTS %d\n", g_atomic_int_get(&start_submits));
    while (pcv_drain_get_work() > 0) g_main_context_iteration(NULL, TRUE);
    sqlite3_close(external); pcv_job_queue_shutdown(); pcv_drain_shutdown();
    G.initialized = FALSE;
    g_ptr_array_unref(G.clients); G.clients = NULL; g_clear_pointer(&G.ctx, g_main_context_unref);
    g_mutex_clear(&G.mu);
    g_io_stream_close(peer_stream, NULL, NULL); g_object_unref(peer_stream);
    g_free(rpc_response); g_free(audit_result); g_ptr_array_unref(frames);
    g_main_context_release(g_main_context_default());
    return 0;
}
