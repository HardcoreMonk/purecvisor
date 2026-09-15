#!/usr/bin/env python3
import os
from pathlib import Path
import re
import resource
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def function(source, name):
    match = re.search(r"(?m)^(?:static[^\n]*|void)\n" + re.escape(name) + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(name)
    depth = 1
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*|[{}]', source[match.end():], re.S)
    for token in tokens:
        depth += (token.group() == "{") - (token.group() == "}")
        if not depth:
            return source[match.start():match.end() + token.end()]
    raise ValueError(name + " is unterminated")


PREAMBLE = r'''
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "modules/dispatcher/rpc_utils.h"
#include "utils/pcv_job_queue.h"
typedef GObject UdsServer;
typedef enum { PCV_ROLE_VIEWER, PCV_ROLE_OPERATOR, PCV_ROLE_ADMIN } PcvRole;
typedef struct {
    GAsyncReadyCallback callback;
    gpointer data;
    gchar *name;
    gchar *snapshot;
} Pending;
typedef struct {
    gchar *id;
    gchar *target;
    PcvJobStatus status;
    gchar *detail;
    guint updates;
} Job;
typedef struct {
    gchar *actor;
    gchar *method;
    gchar *target;
    gchar *result;
    gint error;
} Audit;
static Pending pending[2];
static Job jobs[2];
static Audit audits[2];
static guint pending_count, job_count, audit_count, ws_count, responses;
static gboolean reject_job;
static gchar *response_json;

gchar *pure_rpc_build_success_response(const gchar *id, JsonNode *node) {
    (void)id;
    gchar *json = json_to_string(node, FALSE);
    json_node_unref(node);
    return json;
}
gchar *pure_rpc_build_error_response(const gchar *id, PureRpcErrorCode code, const gchar *message) {
    (void)id; (void)code; (void)message;
    return g_strdup("{\"error\":true}");
}
static void pure_uds_server_send_response(UdsServer *server, GSocketConnection *conn, const gchar *json) {
    (void)server; (void)conn;
    g_free(response_json);
    response_json = g_strdup(json);
    responses++;
}
static gboolean pcv_validate_vm_name(const gchar *name) { return name && *name; }
static gboolean pcv_validate_snap_name(const gchar *name) { return name && *name; }
gchar *pcv_job_create(const gchar *type, const gchar *target, const gchar *params) {
    (void)type; (void)params;
    if (reject_job) return NULL;
    g_assert_cmpuint(job_count, <, G_N_ELEMENTS(jobs));
    Job *job = &jobs[job_count++];
    job->id = g_strdup_printf("job-%u", job_count);
    job->target = g_strdup(target);
    return g_strdup(job->id);
}
JsonObject *pcv_job_get(const gchar *id) {
    g_assert_nonnull(id);
    return json_object_new();
}
void pcv_job_update_status(const gchar *id, PcvJobStatus status, gint progress, const gchar *detail) {
    for (guint i = 0; i < job_count; i++) {
        if (!g_str_equal(id, jobs[i].id)) continue;
        jobs[i].status = status;
        jobs[i].updates++;
        g_free(jobs[i].detail);
        jobs[i].detail = g_strdup(detail);
        g_assert_cmpint(progress, ==, status == PCV_JOB_RUNNING ? 0 : 100);
        return;
    }
    g_assert_not_reached();
}
static void pcv_audit_log(const gchar *actor, const gchar *method, const gchar *target,
                          const gchar *result, gint error, gint64 duration, const gchar *source) {
    (void)duration;
    g_assert_cmpstr(source, ==, "local");
    g_assert_cmpuint(audit_count, <, G_N_ELEMENTS(audits));
    Audit *audit = &audits[audit_count++];
    audit->actor = g_strdup(actor ? actor : "-");
    audit->method = g_strdup(method);
    audit->target = g_strdup(target);
    audit->result = g_strdup(result);
    audit->error = error;
}
static void pcv_ws_broadcast_job_complete(const gchar *id, const gchar *method,
                                          const gchar *status, const gchar *detail) {
    g_assert_cmpuint(audit_count, ==, ws_count + 1);
    const Audit *audit = &audits[ws_count++];
    g_assert_cmpstr(audit->method, ==, method);
    gboolean found = FALSE;
    for (guint i = 0; i < job_count; i++) {
        if (!g_str_equal(id, jobs[i].id)) continue;
        found = TRUE;
        g_assert_cmpstr(jobs[i].detail, ==, detail);
        g_assert_cmpstr(status, ==, jobs[i].status == PCV_JOB_COMPLETED ? "completed" : "failed");
    }
    g_assert_true(found);
}
static void fixture_schedule(const gchar *name, const gchar *snapshot, GCancellable *cancel,
                              GAsyncReadyCallback callback, gpointer data) {
    (void)cancel;
    g_assert_cmpuint(pending_count, <, G_N_ELEMENTS(pending));
    Pending *item = &pending[pending_count++];
    item->name = g_strdup(name);
    item->snapshot = g_strdup(snapshot);
    item->callback = callback;
    item->data = data;
}
static gboolean fixture_finish(GAsyncResult *result, GError **error) {
    return g_task_propagate_boolean(G_TASK(result), error);
}
#define pcv_lxc_snapshot_create_async fixture_schedule
#define pcv_lxc_snapshot_rollback_async fixture_schedule
#define pcv_lxc_snapshot_delete_async fixture_schedule
#define pcv_lxc_snapshot_create_finish fixture_finish
#define pcv_lxc_snapshot_rollback_finish fixture_finish
#define pcv_lxc_snapshot_delete_finish fixture_finish
'''


MAIN = r'''
typedef void (*Handler)(JsonObject *, const gchar *, UdsServer *, GSocketConnection *);

static JsonObject *request_params(const gchar *subject, const gchar *snapshot) {
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "name", "source");
    json_object_set_string_member(params, "snap_name", snapshot);
    json_object_set_string_member(params, "_pcv_caller_sub", "spoofed-body-actor");
    json_object_set_int_member(params, "_pcv_caller_role", PCV_ROLE_ADMIN);
    JsonObject *rpc = json_object_new();
    json_object_set_object_member(rpc, "params", params);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, rpc);
    gchar *json = json_to_string(node, FALSE);
    gchar *authenticated = _rpc_attach_auth_context(json, subject, PCV_ROLE_OPERATOR);
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, authenticated, -1, NULL));
    JsonObject *trusted = json_object_get_object_member(json_node_get_object(json_parser_get_root(parser)), "params");
    json_object_ref(trusted);
    g_assert_cmpint(json_object_get_int_member(trusted, "_pcv_caller_role"), ==, PCV_ROLE_OPERATOR);
    if (!subject || !*subject) g_assert_false(json_object_has_member(trusted, "_pcv_caller_sub"));
    g_object_unref(parser);
    g_free(authenticated);
    g_free(json);
    json_node_unref(node);
    return trusted;
}

static void complete(guint index, gboolean success) {
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    if (success) g_task_return_boolean(task, TRUE);
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "storage failed for second actor");
    pending[index].callback(NULL, G_ASYNC_RESULT(task), pending[index].data);
    g_object_unref(task);
}

static void assert_audit(guint index, const gchar *actor, const gchar *method,
                          const gchar *snapshot, gboolean success) {
    gchar *target = g_strdup_printf("source@%s", snapshot);
    g_assert_cmpstr(audits[index].actor, ==, actor);
    g_assert_cmpstr(audits[index].target, ==, target);
    g_assert_cmpstr(audits[index].method, ==, method);
    g_assert_cmpstr(audits[index].result, ==, success ? "ok" : "fail");
    g_assert_cmpint(audits[index].error, ==, success ? 0 : PURE_RPC_ERR_INTERNAL_ERROR);
    g_free(target);
}

int main(int argc, char **argv) {
    g_assert_cmpint(argc, ==, 3);
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL);
    const gchar *operation = argv[1];
    const gchar *scenario = argv[2];
    Handler handler = g_str_equal(operation, "create") ? handle_container_snapshot_create :
                      g_str_equal(operation, "rollback") ? handle_container_snapshot_rollback :
                      handle_container_snapshot_delete;
    gchar *method = g_strdup_printf("container.snapshot.%s", operation);
    gboolean missing = g_str_equal(scenario, "missing-subject");
    gboolean empty = g_str_equal(scenario, "empty-subject");
    reject_job = g_str_equal(scenario, "job-rejected");
    UdsServer *server = g_object_new(G_TYPE_OBJECT, NULL);
    GSocketConnection *conn = (GSocketConnection *)g_object_new(G_TYPE_OBJECT, NULL);
    guint requests = reject_job ? 1 : 2;
    for (guint i = 0; i < requests; i++) {
        const gchar *subject = missing ? NULL : empty ? "" : i == 0 ? "operator-alice" : "operator-bob";
        JsonObject *params = request_params(subject, i == 0 ? "first-snapshot" : "second-snapshot");
        handler(params, "1", server, conn);
        json_object_set_string_member(params, "_pcv_caller_sub", "request-reused-after-dispatch");
        json_object_set_string_member(params, "snap_name", "request-snapshot-reused");
        json_object_unref(params);
        g_assert_cmpuint(audit_count, ==, 0);
        g_assert_cmpuint(ws_count, ==, 0);
        g_assert_cmpuint(responses, ==, i + 1);
        g_assert_nonnull(strstr(response_json, reject_job ? "error" : "accepted"));
    }
    if (reject_job) {
        g_assert_cmpuint(pending_count, ==, 0);
        g_assert_cmpuint(job_count, ==, 0);
    } else {
        g_assert_cmpuint(pending_count, ==, 2);
        g_assert_cmpuint(server->ref_count, ==, 3);
        g_assert_cmpstr(jobs[0].target, ==, "source@first-snapshot");
        g_assert_cmpstr(jobs[1].target, ==, "source@second-snapshot");
        g_assert_cmpint(jobs[0].status, ==, PCV_JOB_RUNNING);
        g_assert_cmpint(jobs[1].status, ==, PCV_JOB_RUNNING);
        complete(1, FALSE);
        g_assert_cmpuint(audit_count, ==, 1);
        assert_audit(0, missing || empty ? "-" : "operator-bob", method, "second-snapshot", FALSE);
        g_assert_cmpint(jobs[1].status, ==, PCV_JOB_FAILED);
        g_assert_cmpstr(jobs[1].detail, ==, "storage failed for second actor");
        complete(0, TRUE);
        assert_audit(1, missing || empty ? "-" : "operator-alice", method, "first-snapshot", TRUE);
        g_assert_cmpint(jobs[0].status, ==, PCV_JOB_COMPLETED);
        g_assert_null(jobs[0].detail);
        g_assert_cmpuint(jobs[0].updates, ==, 2);
        g_assert_cmpuint(jobs[1].updates, ==, 2);
        g_assert_cmpuint(audit_count, ==, 2);
        g_assert_cmpuint(ws_count, ==, 2);
    }
    g_assert_cmpuint(server->ref_count, ==, 1);
    g_assert_cmpuint(G_OBJECT(conn)->ref_count, ==, 1);
    for (guint i = 0; i < G_N_ELEMENTS(jobs); i++) {
        g_free(jobs[i].id); g_free(jobs[i].target); g_free(jobs[i].detail);
        g_free(pending[i].name); g_free(pending[i].snapshot);
        g_free(audits[i].actor); g_free(audits[i].method);
        g_free(audits[i].target); g_free(audits[i].result);
    }
    g_free(method);
    g_free(response_json);
    g_object_unref(server);
    g_object_unref(conn);
    return 0;
}
'''


class SnapshotAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="pcv-snapshot-audit-")
        cls.exe = Path(cls.tmp.name) / "test"
        source = (ROOT / "src/modules/dispatcher/handler_container.c").read_text()
        context = re.search(r"typedef struct \{[^{}]*\} ContainerCtx;", source).group()
        names = ["_ctx_new", "_ctx_free"]
        if re.search(r"(?m)^_ctx_set_actor\(", source):
            names.append("_ctx_set_actor")
        names += ["_send_error", "_accept_container_job", "_accept_snapshot",
                  "_on_snap_create_done", "handle_container_snapshot_create",
                  "_on_snap_rollback_done", "handle_container_snapshot_rollback",
                  "_on_snap_delete_done", "handle_container_snapshot_delete"]
        rest = (ROOT / "src/api/rest_server.c").read_text()
        unit = Path(cls.tmp.name) / "test.c"
        unit.write_text(PREAMBLE + context + "\n" + function(rest, "_rpc_attach_auth_context") +
                        "\n" + "\n".join(function(source, name) for name in names) + MAIN)
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "gio-2.0", "json-glib-1.0"], text=True))
        subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu23", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "src"), "-I" + str(ROOT / "include"),
                        str(unit), "-o", str(cls.exe), *flags], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_completion_keeps_each_authenticated_actor_and_snapshot(self):
        for operation in ("create", "rollback", "delete"):
            for scenario in ("distinct-actors", "missing-subject", "empty-subject", "job-rejected"):
                with self.subTest(operation=operation, scenario=scenario):
                    result = subprocess.run([str(self.exe), operation, scenario], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
