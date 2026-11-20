#include "modules/dispatcher/handler_vm_config_history.h"

#include <string.h>

#include "modules/auth/pcv_rbac.h"
#include "modules/core/vm_config_history_store.h"
#include "modules/daemons/vm_config_history.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/resource/vm_read_scope.h"
#include "modules/virt/vm_config_history_decide.h"
#include "modules/virt/vm_config_history_view.h"

static JsonObject *
_bad(gint *code, gchar **msg, const gchar *text)
{
    *code = PURE_RPC_ERR_INVALID_PARAMS;
    *msg = g_strdup(text);
    return NULL;
}

static gboolean
_opt_int(JsonObject *p, const gchar *key, gint64 def, gint64 *out)
{
    *out = def;
    if (!p || !json_object_has_member(p, key))
        return TRUE;
    JsonNode *n = json_object_get_member(p, key);
    if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64)
        return FALSE;
    *out = json_node_get_int(n);
    return TRUE;
}

static gboolean
_opt_str(JsonObject *p, const gchar *key, const gchar **out)
{
    *out = NULL;
    if (!p || !json_object_has_member(p, key))
        return TRUE;
    JsonNode *n = json_object_get_member(p, key);
    if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING)
        return FALSE;
    *out = json_node_get_string(n);
    return TRUE;
}

static gboolean
_is_uuid(const gchar *s)
{
    if (!s || strlen(s) != 36)
        return FALSE;
    for (guint i = 0; i < 36; i++) {
        gboolean dash = (i == 8 || i == 13 || i == 18 || i == 23);
        if (dash ? s[i] != '-' : !g_ascii_isxdigit(s[i]))
            return FALSE;
    }
    return TRUE;
}

static gboolean
_row_visible_cb(const gchar *owner, const gchar *owner_tenant, gpointer ud)
{
    return pcv_vm_cfg_row_visible(ud, owner, owner_tenant);
}

static JsonObject *
_store_obj(const gchar *state, const PcvVmCfgStoreInfo *info, gint retention_days)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "state", state);
    if (info && info->oldest_at > 0)
        json_object_set_int_member(o, "oldest_at", info->oldest_at);
    else
        json_object_set_null_member(o, "oldest_at");
    if (info && info->pruned_before > 0)
        json_object_set_int_member(o, "pruned_before", info->pruned_before);
    else
        json_object_set_null_member(o, "pruned_before");
    json_object_set_int_member(o, "retention_days", retention_days);
    return o;
}

static JsonObject *
_empty(const gchar *state, const PcvVmCfgStoreInfo *info, gint retention_days, gboolean admin, JsonArray *gaps)
{
    JsonObject *o = json_object_new();
    json_object_set_array_member(o, "entries", json_array_new());
    json_object_set_array_member(o, "gaps", gaps ? gaps : json_array_new());
    json_object_set_object_member(o, "store", _store_obj(state, info, retention_days));
    json_object_set_string_member(o, "scope", admin ? "all" : "own");
    json_object_set_null_member(o, "next_cursor");
    return o;
}

static JsonArray *
_gaps(PcvVmCfgStore *st, gint64 since, gint64 until, gint64 pruned_before, const PcvVmReadScope *scope,
      const gchar *uuid_filter)
{
    JsonArray *out = json_array_new();
    GArray *eps = pcv_vm_cfg_store_epochs(st, since, until);
    GArray *gaps = pcv_vm_cfg_gaps((const PcvVmCfgEpoch *)eps->data, eps->len, since, until, pruned_before);
    for (guint i = 0; i < gaps->len; i++) {
        PcvVmCfgGap *g = &g_array_index(gaps, PcvVmCfgGap, i);
        JsonObject *o = json_object_new();
        json_object_set_int_member(o, "start", g->start);
        json_object_set_int_member(o, "end", g->end);
        json_object_set_string_member(o, "reason", g->reason);
        json_array_add_object_element(out, o);
    }
    g_array_unref(gaps);
    g_array_unref(eps);
    JsonArray *fails = pcv_vm_cfg_hist_capture_failures(since, until);
    for (guint i = 0; i < json_array_get_length(fails); i++) {
        JsonObject *f = json_array_get_object_element(fails, i);
        const gchar *u = json_object_get_string_member(f, "vm_uuid");
        if (uuid_filter && g_strcmp0(u, uuid_filter))
            continue;
        PcvVmCfgLatestRow l;
        gboolean show = pcv_vm_cfg_store_latest(st, u, &l, NULL) && l.last &&
                        pcv_vm_cfg_row_visible(scope, l.last->owner, l.last->owner_tenant);
        pcv_vm_cfg_latest_clear(&l);
        if (!show)
            continue;
        JsonObject *o = json_object_new();
        json_object_set_int_member(o, "start", MAX(json_object_get_int_member(f, "start"), since));
        json_object_set_int_member(o, "end", MIN(json_object_get_int_member(f, "end"), until));
        json_object_set_string_member(o, "reason", "capture_failed");
        json_object_set_string_member(o, "vm_uuid", u);
        json_array_add_object_element(out, o);
    }
    json_array_unref(fails);
    return out;
}

static gsize
_node_size(JsonNode *n)
{
    gsize len = 0;
    gchar *s = json_to_string(n, FALSE);
    len = strlen(s);
    g_free(s);
    return len;
}

JsonObject *
pcv_vm_cfg_history_list_build(JsonObject *params, const PcvResCaller *caller, gint *err_code, gchar **err_msg)
{
    const gchar *vm = NULL, *uuid = NULL, *cursor = NULL;
    gint64 since = 0, until = 0, limit = 0;
    gint64 now = g_get_real_time() / 1000;
    if (!_opt_str(params, "vm", &vm) || !_opt_str(params, "uuid", &uuid) || !_opt_str(params, "cursor", &cursor))
        return _bad(err_code, err_msg, "Invalid params: vm, uuid, cursor must be strings");
    if (!_opt_int(params, "until", now, &until))
        return _bad(err_code, err_msg, "Invalid params: since, until, limit must be integers");
    if (until < 0)
        return _bad(err_code, err_msg, "Invalid params: since must be >= 0 and <= until");
    if (!_opt_int(params, "since", MAX(0, until - PCV_VMCH_LIST_DEFAULT_SPAN_MS), &since) ||
        !_opt_int(params, "limit", PCV_VMCH_LIST_DEFAULT_LIMIT, &limit))
        return _bad(err_code, err_msg, "Invalid params: since, until, limit must be integers");
    if (since < 0 || until < since)
        return _bad(err_code, err_msg, "Invalid params: since must be >= 0 and <= until");
    if (limit < 1 || limit > PCV_VMCH_LIST_MAX_LIMIT)
        return _bad(err_code, err_msg, "Invalid params: limit must be 1..200");
    if (uuid && !_is_uuid(uuid))
        return _bad(err_code, err_msg, "Invalid params: uuid must be a UUID");
    if (vm && (!*vm || strlen(vm) > 128))
        return _bad(err_code, err_msg, "Invalid params: vm must be a VM name or UUID");
    PcvVmCfgListFilter f = {.since = since, .until = until};
    if (cursor && !pcv_vm_cfg_cursor_decode(cursor, &f.cursor_at, f.cursor_id))
        return _bad(err_code, err_msg, "Invalid params: cursor");
    gboolean admin = caller->role >= PCV_ROLE_ADMIN;
    PcvVmCfgHistStatus status;
    pcv_vm_cfg_hist_status(&status);
    if (g_strcmp0(status.state, "ok") && g_strcmp0(status.state, "degraded") && g_strcmp0(status.state, "over_cap")) {
        JsonObject *o = _empty(status.state, NULL, status.retention_days, admin, NULL);
        pcv_vm_cfg_hist_status_clear(&status);
        return o;
    }
    PcvVmCfgStore *st = pcv_vm_cfg_store_open(status.db_path, FALSE, NULL);
    if (!st) {
        JsonObject *o = _empty("unavailable", NULL, status.retention_days, admin, NULL);
        pcv_vm_cfg_hist_status_clear(&status);
        return o;
    }
    PcvVmCfgStoreInfo info;
    pcv_vm_cfg_store_info(st, &info);
    PcvVmReadScope *scope = pcv_vm_read_scope_new(caller);
    gchar *target = NULL;
    if (uuid)
        target = g_ascii_strdown(uuid, -1);
    else if (vm && _is_uuid(vm))
        target = g_ascii_strdown(vm, -1);
    else if (vm)
        target = pcv_vm_cfg_store_resolve_name(st, vm);
    JsonArray *gaps = _gaps(st, since, until, info.pruned_before, scope, target);
    JsonObject *out = NULL;
    if (vm && !target) {
        out = _empty(status.state, &info, status.retention_days, admin, gaps);
    } else {
        f.uuid = target;
        f.narrow = !admin;
        f.narrow_subject = caller->subject;
        f.narrow_tenant = caller->tenant;
        GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)pcv_vm_cfg_row_free);
        gboolean more = FALSE;
        pcv_vm_cfg_store_list(st, &f, (guint)limit, _row_visible_cb, scope, rows, &more, NULL);
        JsonArray *entries = json_array_new();
        gsize total = 512;
        gboolean truncated = FALSE;
        guint used = 0;
        for (; used < rows->len; used++) {
            PcvVmCfgRow *r = g_ptr_array_index(rows, used);
            JsonNode *n = pcv_vm_cfg_row_project(r, caller->role, caller->subject, caller->tenant, FALSE);
            gsize sz = _node_size(n);
            if (total + sz > PCV_VMCH_RESP_LIMIT) {
                JsonObject *o = json_node_get_object(n);
                json_object_set_array_member(o, "changes", json_array_new());
                json_object_set_boolean_member(o, "truncated", TRUE);
                truncated = TRUE;
                sz = _node_size(n);
                if (total + sz > PCV_VMCH_RESP_LIMIT) {
                    json_node_unref(n);
                    more = TRUE;
                    break;
                }
            }
            total += sz;
            json_array_add_element(entries, n);
        }
        out = json_object_new();
        json_object_set_array_member(out, "entries", entries);
        json_object_set_array_member(out, "gaps", gaps);
        json_object_set_object_member(out, "store", _store_obj(status.state, &info, status.retention_days));
        json_object_set_string_member(out, "scope", admin ? "all" : "own");
        if (truncated)
            json_object_set_boolean_member(out, "truncated", TRUE);
        if (more && used > 0) {
            PcvVmCfgRow *last = g_ptr_array_index(rows, used - 1);
            gchar *nc = pcv_vm_cfg_cursor_encode(last->observed_at, last->entry_id);
            json_object_set_string_member(out, "next_cursor", nc);
            g_free(nc);
        } else {
            json_object_set_null_member(out, "next_cursor");
        }
        g_ptr_array_unref(rows);
    }
    g_free(target);
    pcv_vm_read_scope_free(scope);
    pcv_vm_cfg_store_close(st);
    pcv_vm_cfg_hist_status_clear(&status);
    return out;
}

JsonObject *
pcv_vm_cfg_history_get_build(JsonObject *params, const PcvResCaller *caller, gint *err_code, gchar **err_msg)
{
    const gchar *id = NULL;
    if (!_opt_str(params, "id", &id) || !id || !pcv_vm_cfg_entry_id_valid(id))
        return _bad(err_code, err_msg, "Missing or invalid param: id (32 lowercase hex)");
    PcvVmCfgHistStatus status;
    pcv_vm_cfg_hist_status(&status);
    PcvVmCfgStore *st = (!g_strcmp0(status.state, "disabled") || !g_strcmp0(status.state, "unavailable"))
                            ? NULL
                            : pcv_vm_cfg_store_open(status.db_path, FALSE, NULL);
    pcv_vm_cfg_hist_status_clear(&status);
    PcvVmReadScope *scope = pcv_vm_read_scope_new(caller);
    PcvVmCfgRow *row = st ? pcv_vm_cfg_store_get(st, id) : NULL;
    JsonObject *out = NULL;
    if (row && pcv_vm_cfg_row_visible(scope, row->owner, row->owner_tenant)) {
        PcvVmCfgRow *prev = pcv_vm_cfg_store_get_prev(st, row);
        if (prev && pcv_vm_cfg_row_visible(scope, prev->owner, prev->owner_tenant))
            row->prev_entry_id = g_strdup(prev->entry_id);
        pcv_vm_cfg_row_free(prev);
        JsonNode *n = pcv_vm_cfg_row_project(row, caller->role, caller->subject, caller->tenant, TRUE);
        if (_node_size(n) > PCV_VMCH_RESP_LIMIT) {
            json_object_set_array_member(json_node_get_object(n), "changes", json_array_new());
            json_object_set_boolean_member(json_node_get_object(n), "truncated", TRUE);
        }
        out = json_object_ref(json_node_get_object(n));
        json_node_unref(n);
    } else {
        *err_code = PURE_RPC_ERR_FORBIDDEN;
        *err_msg = g_strdup(PCV_VMCH_GET_DENY);
    }
    pcv_vm_cfg_row_free(row);
    pcv_vm_read_scope_free(scope);
    pcv_vm_cfg_store_close(st);
    return out;
}

static void
_respond(JsonObject *obj, gint code, gchar *msg, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    gchar *resp = NULL;
    if (obj) {
        JsonNode *n = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(n, obj);
        resp = pure_rpc_build_success_response(rpc_id, n);
    } else {
        resp = pure_rpc_build_error_response(rpc_id, code, msg ? msg : "error");
    }
    pure_uds_server_send_response(server, connection, resp);
    g_free(resp);
    g_free(msg);
}

void
handle_vm_config_history_list(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    PcvResCaller rc = {0};
    pcv_res_caller_init(&rc, params, connection);
    gint code = 0;
    gchar *msg = NULL;
    JsonObject *obj = pcv_vm_cfg_history_list_build(params, &rc, &code, &msg);
    pcv_res_caller_clear(&rc);
    _respond(obj, code, msg, rpc_id, server, connection);
}

void
handle_vm_config_history_get(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection)
{
    if (!params || !json_object_has_member(params, "id")) {
        _respond(NULL, PURE_RPC_ERR_INVALID_PARAMS, g_strdup("Missing param: id"), rpc_id, server, connection);
        return;
    }
    PcvResCaller rc = {0};
    pcv_res_caller_init(&rc, params, connection);
    gint code = 0;
    gchar *msg = NULL;
    JsonObject *obj = pcv_vm_cfg_history_get_build(params, &rc, &code, &msg);
    pcv_res_caller_clear(&rc);
    _respond(obj, code, msg, rpc_id, server, connection);
}
