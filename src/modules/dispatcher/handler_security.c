/**
 * @file handler_security.c
 * @brief security.* JSON-RPC 핸들러 — Security Guard(HIDS/HIPS)의 단일 UDS 표면.
 *
 * [책임]
 *   HIDS 이벤트 조회(list/get), HIPS 대응 액션의 pending/approve/dismiss, 파일
 *   무결성 baseline status/refresh, security config get/set 을 코어 모듈 호출로
 *   잇는다. dispatcher 가 인증·RBAC 를 통과시킨 뒤 params 에 caller 신원을 주입해
 *   이 파일로 들여보낸다.
 *
 * [RPC 계약 — ADR-0018 / ADR-0024]
 *   - 조회 계열은 read-only: 현재 로컬 상태를 그대로 반환한다.
 *   - approve 만 실제 부작용(nftables DROP·API 키 폐기)을 일으킨다. fire-and-forget:
 *     accepted + job_id 를 먼저 응답하고, GTask 워커가 execute→감사→WS 완료 순으로
 *     최종 결과를 소유한다(ADR-0018). 실행이 approved 마킹보다 먼저다 — 방화벽/RBAC
 *     조작이 실패했는데 UI·감사가 "해결됨"으로 오인하지 않도록.
 *   - 실행 가능한 액션은 block_ip·revoke_api_key 뿐(ADR-0024). 그 외는
 *     is_executable 게이트에서 "manual runbook 필요"로 거부한다.
 *
 * Operator note:
 *   이 파일은 "탐지된 위협을 실제로 차단할지"를 사람이 승인하는 지점이다. 여기서
 *   거부해야 할 요청을 통과시키면 오탐 하나가 정상 IP 차단·API 키 폐기로 이어져
 *   서비스 장애가 될 수 있다. 사고 시 security.action.approve 의 감사 로그와
 *   job 완료 WS 브로드캐스트(job_id)를 함께 확인한다.
 */
#include "handler_security.h"
#include "rpc_utils.h"

#include "../audit/pcv_audit.h"
#include "../security/hids_file_integrity.h"
#include "../security/hips_actions.h"
#include "../security/security_store.h"
#include "../../api/uds_server.h"
#include "../../api/ws_server.h"
#include "../../utils/pcv_config.h"

#include <glib/gstdio.h>

/* PCV_SECURITY_DB_DEFAULT 는 security_store.h 로 이동 (store 모듈 소유, SG restore 와 공유). */

/*
 * security.* handlers are the single UDS surface for Security Guard. Read calls
 * return current local state; side-effecting action approval follows ADR-0018:
 * send accepted first, execute in a worker, then audit and broadcast completion.
 */

typedef struct {
    gchar *event_id;
    gchar *action;
    gchar *target;
    gchar *admin_user;
    gchar *job_id;
} SecurityApproveTaskData;

static const gchar *
security_db_path(void)
{
    return pcv_config_get_string("security", "db_path", PCV_SECURITY_DB_DEFAULT);
}

static gboolean
ensure_security_store_open(void)
{
    /* store 모듈의 ensure-open 에 위임 — 경로/기본값/동시 open 직렬화를 store 가
     * 단일 소유(중복 플래그 제거). 부팅 경로(SG restore)와 lazy RPC 오픈이 같은
     * 진입점을 공유해 이중 open 이 사라진다. */
    return pcv_security_store_ensure_open();
}

static const gchar *
json_string_default(JsonObject *params, const gchar *key, const gchar *def)
{
    if (!params || !key || !json_object_has_member(params, key)) {
        return def;
    }
    JsonNode *node = json_object_get_member(params, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING) {
        return def;
    }
    const gchar *value = json_node_get_string(node);
    return value ? value : def;
}

static gint
json_int_default(JsonObject *params, const gchar *key, gint def)
{
    if (!params || !key || !json_object_has_member(params, key)) {
        return def;
    }
    JsonNode *node = json_object_get_member(params, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) {
        return def;
    }
    GType t = json_node_get_value_type(node);
    if (t != G_TYPE_INT && t != G_TYPE_INT64 &&
        t != G_TYPE_UINT && t != G_TYPE_UINT64 &&
        t != G_TYPE_LONG && t != G_TYPE_ULONG) {
        return def;
    }
    return (gint)json_node_get_int(node);
}

static const gchar *
admin_user_from_params(JsonObject *params)
{
    /*
     * REST middleware injects _pcv_username/_pcv_caller_sub. Direct UDS tests may
     * omit both, so "system" is the explicit audit subject fallback.
     */
    const gchar *user = json_string_default(params, "_pcv_username", NULL);
    if (user && *user) {
        return user;
    }
    user = json_string_default(params, "_pcv_caller_sub", NULL);
    return user && *user ? user : "system";
}

static void
send_error(UdsServer *server,
           GSocketConnection *connection,
           const gchar *rpc_id,
           PureRpcErrorCode code,
           const gchar *message)
{
    gchar *resp = pure_rpc_build_error_response(rpc_id, code, message);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void
send_success_object(UdsServer *server,
                    GSocketConnection *connection,
                    const gchar *rpc_id,
                    JsonObject *obj)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, obj);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void
send_success_array(UdsServer *server,
                   GSocketConnection *connection,
                   const gchar *rpc_id,
                   JsonArray *arr)
{
    JsonNode *node = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(node, arr);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static void
send_success_boolean(UdsServer *server,
                     GSocketConnection *connection,
                     const gchar *rpc_id,
                     gboolean value)
{
    JsonNode *node = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(node, value);
    gchar *resp = pure_rpc_build_success_response(rpc_id, node);
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
}

static const gchar *
baseline_status_to_string(PcvHidsBaselineStatus status)
{
    switch (status) {
    case PCV_HIDS_BASELINE_TRUSTED:
        return "trusted";
    case PCV_HIDS_BASELINE_STALE:
        return "stale";
    case PCV_HIDS_BASELINE_UNKNOWN:
    default:
        return "unknown";
    }
}

static gint
compute_open_risk(void)
{
    /*
     * This is intentionally a simple operator-facing score, not ML. CRIT events
     * dominate the number so a single control-plane risk is visible immediately.
     */
    JsonArray *events = pcv_security_store_list_events(0, 500, NULL, NULL, "open");
    gint risk = 0;
    guint len = json_array_get_length(events);
    for (guint i = 0; i < len; i++) {
        JsonObject *ev = json_array_get_object_element(events, i);
        const gchar *severity = ev && json_object_has_member(ev, "severity")
            ? json_object_get_string_member(ev, "severity")
            : "";
        if (g_strcmp0(severity, "crit") == 0) {
            risk += 100;
        } else if (g_strcmp0(severity, "warn") == 0) {
            risk += 10;
        } else {
            risk += 1;
        }
    }
    json_array_unref(events);
    return risk;
}

static gchar **
extract_path_array(JsonObject *params, gsize *out_len, GError **error)
{
    /* Baseline refresh is admin supplied, so reject malformed path arrays early. */
    *out_len = 0;
    if (!params || !json_object_has_member(params, "paths")) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "paths array is required");
        return NULL;
    }

    JsonNode *node = json_object_get_member(params, "paths");
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "paths must be an array of strings");
        return NULL;
    }

    JsonArray *arr = json_node_get_array(node);
    guint len = json_array_get_length(arr);
    if (len == 0) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "paths must not be empty");
        return NULL;
    }

    gchar **paths = g_new0(gchar *, len + 1);
    for (guint i = 0; i < len; i++) {
        JsonNode *item = json_array_get_element(arr, i);
        if (!item || !JSON_NODE_HOLDS_VALUE(item) ||
            json_node_get_value_type(item) != G_TYPE_STRING) {
            g_strfreev(paths);
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                        "paths must contain strings only");
            return NULL;
        }
        const gchar *path = json_node_get_string(item);
        if (!path || !*path) {
            g_strfreev(paths);
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                        "paths must not contain empty strings");
            return NULL;
        }
        paths[i] = g_strdup(path);
    }

    *out_len = len;
    return paths;
}

static void
security_approve_task_data_free(SecurityApproveTaskData *d)
{
    if (!d) {
        return;
    }
    g_free(d->event_id);
    g_free(d->action);
    g_free(d->target);
    g_free(d->admin_user);
    g_free(d->job_id);
    g_free(d);
}

static void
security_action_approve_worker(GTask *task,
                               gpointer source __attribute__((unused)),
                               gpointer task_data,
                               GCancellable *cancel __attribute__((unused)))
{
    /*
     * Execute first, mark approved second. That ordering prevents the UI/audit
     * layer from treating a failed firewall or RBAC operation as resolved.
     */
    SecurityApproveTaskData *d = task_data;
    gint64 start_us = g_get_monotonic_time();
    GError *error = NULL;

    gboolean ok = pcv_hips_action_run_approval(d->event_id, d->action, d->target,
                                               d->admin_user,
                                               pcv_hips_action_execute, &error);
    if (ok) {
        GError *status_error = NULL;
        (void)pcv_security_store_update_event_status(d->event_id,
                                                     PCV_SECURITY_STATUS_RESOLVED,
                                                     &status_error);
        g_clear_error(&status_error);
    }

    gint64 duration_ms = (g_get_monotonic_time() - start_us) / 1000;
    if (ok) {
        pcv_audit_log(d->admin_user, "security.action.approve", d->event_id,
                      "ok", 0, duration_ms, "local");
        pcv_ws_broadcast_job_complete_mt(d->job_id, "security.action.approve",
                                         "completed", NULL);
        g_task_return_boolean(task, TRUE);
        return;
    }

    const gchar *message = error ? error->message : "security action approval failed";
    pcv_audit_log(d->admin_user, "security.action.approve", d->event_id,
                  "fail", PURE_RPC_ERR_INTERNAL_ERROR, duration_ms, "local");
    pcv_ws_broadcast_job_complete_mt(d->job_id, "security.action.approve",
                                     "failed", message);
    if (error) {
        g_task_return_error(task, error);
    } else {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "security action approval failed");
    }
}

void
handle_security_event_list(JsonObject *params,
                           const gchar *rpc_id,
                           UdsServer *server,
                           GSocketConnection *connection)
{
    if (!ensure_security_store_open()) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                   "security store is unavailable");
        return;
    }

    gint offset = json_int_default(params, "offset", 0);
    gint limit = json_int_default(params, "limit", 100);
    const gchar *severity = json_string_default(params, "severity", NULL);
    const gchar *source = json_string_default(params, "source", NULL);
    const gchar *status = json_string_default(params, "status", NULL);

    JsonArray *arr = pcv_security_store_list_events(offset, limit,
                                                    severity, source, status);
    send_success_array(server, connection, rpc_id, arr);
}

void
handle_security_event_get(JsonObject *params,
                          const gchar *rpc_id,
                          UdsServer *server,
                          GSocketConnection *connection)
{
    const gchar *event_id = json_string_default(params, "event_id", "");
    if (!event_id[0]) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                   "event_id is required");
        return;
    }
    if (!ensure_security_store_open()) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                   "security store is unavailable");
        return;
    }

    JsonObject *obj = pcv_security_store_get_event(event_id);
    if (!obj) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
                   "security event not found");
        return;
    }
    send_success_object(server, connection, rpc_id, obj);
}

void
handle_security_action_pending(JsonObject *params __attribute__((unused)),
                               const gchar *rpc_id,
                               UdsServer *server,
                               GSocketConnection *connection)
{
    if (!ensure_security_store_open()) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                   "security store is unavailable");
        return;
    }

    JsonArray *arr = pcv_hips_action_list_pending();
    send_success_array(server, connection, rpc_id, arr);
}

void
handle_security_action_approve(JsonObject *params,
                               const gchar *rpc_id,
                               UdsServer *server,
                               GSocketConnection *connection)
{
    /*
     * Approval is fire-and-forget. The accepted response gives the caller a
     * trackable job_id while the worker owns the final audit/WS result.
     */
    const gchar *event_id = json_string_default(params, "event_id", "");
    const gchar *admin_user = admin_user_from_params(params);
    if (!event_id[0]) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                   "event_id is required");
        return;
    }
    if (!ensure_security_store_open()) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                   "security store is unavailable");
        return;
    }

    g_autoptr(JsonObject) action = pcv_security_store_get_action(event_id);
    if (!action) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_VM_NOT_FOUND,
                   "pending action not found");
        return;
    }

    const gchar *status = json_string_default(action, "status", "");
    const gchar *action_name = json_string_default(action, "action", "");
    /* 이중 게이트 — 부작용 실행(GTask) 전에 여기서 반드시 막아야 한다:
     *   (1) 이미 처리된(resolved/suppressed) 액션의 재승인을 CONFLICT 로 차단해
     *       같은 차단/폐기가 중복 실행되지 않게 한다.
     *   (2) is_executable 가 아닌 액션(lock_user·restart_service 등)은 자동 실행
     *       대상이 아니라 manual runbook 후보다(ADR-0024). 통과시키면 노출만
     *       하려던 조치가 실제 부작용으로 새어나간다. */
    if (g_strcmp0(status, "pending") != 0) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_CONFLICT,
                   "security action is not pending");
        return;
    }
    if (!pcv_hips_action_is_executable(action_name)) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                   "security action requires manual runbook");
        return;
    }

    g_autofree gchar *job_id = g_strdup_printf("security.action.approve:%s:%"
                                               G_GINT64_FORMAT,
                                               event_id, g_get_real_time());
    JsonObject *accepted = json_object_new();
    json_object_set_string_member(accepted, "status", "accepted");
    json_object_set_string_member(accepted, "job_id", job_id);
    json_object_set_string_member(accepted, "event_id", event_id);
    send_success_object(server, connection, rpc_id, accepted);

    SecurityApproveTaskData *d = g_new0(SecurityApproveTaskData, 1);
    d->event_id = g_strdup(event_id);
    d->action = g_strdup(action_name);
    d->target = g_strdup(json_string_default(action, "target", ""));
    d->admin_user = g_strdup(admin_user);
    d->job_id = g_strdup(job_id);

    /* ADR-0018-audit: security.action.approve */
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, d, (GDestroyNotify)security_approve_task_data_free);
    g_task_run_in_thread(task, security_action_approve_worker);
    g_object_unref(task);
}

void
handle_security_action_dismiss(JsonObject *params,
                               const gchar *rpc_id,
                               UdsServer *server,
                               GSocketConnection *connection)
{
    /*
     * Dismissal is synchronous because it only changes local decision state.
     * Operator can suppress noise; admin is required only for executable action.
     */
    const gchar *event_id = json_string_default(params, "event_id", "");
    const gchar *reason = json_string_default(params, "reason", "");
    const gchar *admin_user = admin_user_from_params(params);
    if (!event_id[0]) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                   "event_id is required");
        return;
    }
    if (!ensure_security_store_open()) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                   "security store is unavailable");
        return;
    }

    GError *error = NULL;
    gboolean ok = pcv_hips_action_dismiss(event_id, admin_user, reason, &error);
    if (ok) {
        GError *status_error = NULL;
        (void)pcv_security_store_update_event_status(event_id,
                                                     PCV_SECURITY_STATUS_SUPPRESSED,
                                                     &status_error);
        g_clear_error(&status_error);
        send_success_boolean(server, connection, rpc_id, TRUE);
        return;
    }

    send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
               error ? error->message : "failed to dismiss security action");
    g_clear_error(&error);
}

void
handle_security_baseline_status(JsonObject *params __attribute__((unused)),
                                const gchar *rpc_id,
                                UdsServer *server,
                                GSocketConnection *connection)
{
    PcvHidsBaselineStatus status = pcv_hids_baseline_status(security_db_path());
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "status", baseline_status_to_string(status));
    send_success_object(server, connection, rpc_id, obj);
}

void
handle_security_baseline_refresh(JsonObject *params,
                                 const gchar *rpc_id,
                                 UdsServer *server,
                                 GSocketConnection *connection)
{
    /*
     * Baseline trust is explicit. No scan result can auto-promote unknown files
     * into trusted state; an admin must refresh the path set.
     */
    const gchar *admin_user = admin_user_from_params(params);
    GError *error = NULL;
    gsize n_paths = 0;
    gchar **paths = extract_path_array(params, &n_paths, &error);
    if (!paths) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                   error ? error->message : "paths array is required");
        g_clear_error(&error);
        return;
    }

    gboolean ok = pcv_hids_baseline_refresh(security_db_path(),
                                            (const gchar * const *)paths,
                                            n_paths, admin_user, &error);
    g_strfreev(paths);
    if (!ok) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                   error ? error->message : "baseline refresh failed");
        g_clear_error(&error);
        return;
    }

    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "status", "trusted");
    json_object_set_int_member(obj, "path_count", (gint)n_paths);
    send_success_object(server, connection, rpc_id, obj);
}

void
handle_security_config_get(JsonObject *params __attribute__((unused)),
                           const gchar *rpc_id,
                           UdsServer *server,
                           GSocketConnection *connection)
{
    if (!ensure_security_store_open()) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                   "security store is unavailable");
        return;
    }

    JsonArray *pending = pcv_hips_action_list_pending();
    guint pending_count = json_array_get_length(pending);
    json_array_unref(pending);

    g_autoptr(JsonObject) health = pcv_security_store_health();
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "enabled",
                                   pcv_security_store_get_bool_config("enabled", FALSE));
    json_object_set_string_member(obj, "baseline_status",
                                  baseline_status_to_string(
                                      pcv_hids_baseline_status(security_db_path())));
    json_object_set_int_member(obj, "open_risk", compute_open_risk());
    json_object_set_int_member(obj, "pending_actions", (gint)pending_count);
    json_object_set_boolean_member(obj, "degraded",
                                   json_object_get_boolean_member(health, "degraded"));
    send_success_object(server, connection, rpc_id, obj);
}

void
handle_security_config_set(JsonObject *params,
                           const gchar *rpc_id,
                           UdsServer *server,
                           GSocketConnection *connection)
{
    if (!params || !json_object_has_member(params, "enabled")) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                   "enabled boolean is required");
        return;
    }
    JsonNode *enabled_node = json_object_get_member(params, "enabled");
    if (!enabled_node || !JSON_NODE_HOLDS_VALUE(enabled_node) ||
        json_node_get_value_type(enabled_node) != G_TYPE_BOOLEAN) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INVALID_PARAMS,
                   "enabled must be a boolean");
        return;
    }
    if (!ensure_security_store_open()) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                   "security store is unavailable");
        return;
    }

    const gchar *admin_user = admin_user_from_params(params);
    gboolean enabled = json_node_get_boolean(enabled_node);
    GError *error = NULL;
    gboolean ok = pcv_security_store_set_bool_config("enabled", enabled,
                                                     admin_user, &error);
    if (!ok) {
        send_error(server, connection, rpc_id, PURE_RPC_ERR_INTERNAL_ERROR,
                   error ? error->message : "security config update failed");
        g_clear_error(&error);
        return;
    }

    pcv_audit_log(admin_user, "security.config.set", "enabled",
                  "ok", 0, 0, "local");
    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "enabled", enabled);
    send_success_object(server, connection, rpc_id, obj);
}
