#include "modules/virt/vm_config_history_view.h"

#include <string.h>

#include "modules/auth/pcv_rbac.h"
#include "modules/virt/vm_config_history_norm.h"

void
pcv_vm_cfg_row_free(PcvVmCfgRow *r)
{
    if (!r)
        return;
    g_free(r->entry_id);
    g_free(r->vm_uuid);
    g_free(r->vm_name);
    g_free(r->owner);
    g_free(r->owner_tenant);
    g_free(r->kind);
    g_free(r->scope);
    g_free(r->cause);
    g_free(r->attribution);
    g_free(r->actor);
    g_free(r->actor_tenant);
    g_free(r->actor_method);
    g_free(r->actor_result);
    g_free(r->config_digest);
    g_free(r->live_digest);
    g_free(r->snapshot_json);
    g_free(r->changes_json);
    g_free(r->prev_entry_id);
    g_free(r->salt_id);
    g_free(r);
}

gboolean
pcv_vm_cfg_row_visible(const PcvVmReadScope *s, const gchar *owner, const gchar *owner_tenant)
{
    if (!s)
        return FALSE;
    PcvJobScope js = {0};
    pcv_vm_read_job_scope(s, &js);
    if (js.all)
        return TRUE;
    return pcv_vm_read_decide(js.role, js.subject, js.tenant, owner, owner_tenant);
}

static JsonNode *
_parse(const gchar *text)
{
    if (!text)
        return NULL;
    JsonParser *p = json_parser_new();
    JsonNode *n = json_parser_load_from_data(p, text, -1, NULL) ? json_node_copy(json_parser_get_root(p)) : NULL;
    g_object_unref(p);
    return n;
}

static JsonNode *
_project_value(const gchar *path, JsonNode *v, gboolean admin, const gchar *subject, const gchar *row_owner)
{
    if (!v || JSON_NODE_HOLDS_NULL(v))
        return json_node_new(JSON_NODE_NULL);
    if (admin || json_node_get_value_type(v) != G_TYPE_STRING)
        return json_node_copy(v);
    if (path && g_str_has_prefix(path, "live."))
        path += strlen("live.");
    const gchar *s = json_node_get_string(v);
    JsonNode *out = json_node_new(JSON_NODE_VALUE);
    if (path && g_str_has_prefix(path, "opaque.")) {
        json_node_set_string(out, PCV_VMCH_OPAQUE_MASK);
    } else if (pcv_vm_cfg_path_is_host_path(path)) {
        gchar *base = g_path_get_basename(s);
        json_node_set_string(out, base);
        g_free(base);
    } else if (!g_strcmp0(path, "owner") && g_strcmp0(s, row_owner) && g_strcmp0(s, subject)) {
        json_node_set_string(out, PCV_VMCH_OTHER_OWNER);
    } else {
        json_node_set_string(out, s);
    }
    return out;
}

static JsonArray *
_project_changes(const gchar *changes_json, guint limit, gboolean admin, const gchar *subject, const gchar *row_owner,
                 guint *total)
{
    JsonArray *out = json_array_new();
    JsonNode *n = _parse(changes_json);
    *total = 0;
    if (n && JSON_NODE_HOLDS_ARRAY(n)) {
        JsonArray *a = json_node_get_array(n);
        *total = json_array_get_length(a);
        for (guint i = 0; i < *total && (!limit || i < limit); i++) {
            JsonObject *c = json_array_get_object_element(a, i);
            if (!c)
                continue;
            const gchar *path = json_object_get_string_member_with_default(c, "path", "");
            JsonObject *o = json_object_new();
            json_object_set_string_member(o, "path", path);
            json_object_set_member(o, "before", _project_value(path, json_object_get_member(c, "before"), admin,
                                                                subject, row_owner));
            json_object_set_member(o, "after", _project_value(path, json_object_get_member(c, "after"), admin,
                                                               subject, row_owner));
            json_array_add_object_element(out, o);
        }
    }
    if (n)
        json_node_unref(n);
    return out;
}

static JsonNode *
_project_flat(JsonNode *flat, gboolean admin, const gchar *subject, const gchar *row_owner)
{
    if (!flat || !JSON_NODE_HOLDS_OBJECT(flat))
        return json_node_new(JSON_NODE_NULL);
    JsonObject *src = json_node_get_object(flat), *dst = json_object_new();
    GList *members = json_object_get_members(src);
    for (GList *m = members; m; m = m->next)
        json_object_set_member(dst, m->data, _project_value(m->data, json_object_get_member(src, m->data), admin,
                                                             subject, row_owner));
    g_list_free(members);
    JsonNode *out = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(out, dst);
    return out;
}

JsonNode *
pcv_vm_cfg_row_project(const PcvVmCfgRow *row, gint role, const gchar *subject, const gchar *caller_tenant,
                       gboolean full)
{
    gboolean admin = role >= PCV_ROLE_ADMIN;
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "id", row->entry_id);
    json_object_set_string_member(o, "vm_uuid", row->vm_uuid);
    json_object_set_string_member(o, "vm_name", row->vm_name);
    json_object_set_int_member(o, "observed_at", row->observed_at);
    if (row->window_start > 0)
        json_object_set_int_member(o, "window_start", row->window_start);
    else
        json_object_set_null_member(o, "window_start");
    json_object_set_string_member(o, "kind", row->kind);
    json_object_set_string_member(o, "cause", row->cause);
    json_object_set_string_member(o, "scope", row->scope);
    json_object_set_string_member(o, "attribution", row->attribution);

    gboolean hidden = FALSE;
    const gchar *actor = row->actor_expired ? NULL : row->actor;
    if (actor && !admin) {
        gboolean mine = subject && *subject && !g_strcmp0(actor, subject);
        gboolean same_tenant = caller_tenant && *caller_tenant && row->actor_tenant &&
                               !g_strcmp0(row->actor_tenant, caller_tenant);
        if (!mine && !same_tenant) {
            hidden = TRUE;
            actor = NULL;
        }
    }
    if (actor)
        json_object_set_string_member(o, "actor", actor);
    else
        json_object_set_null_member(o, "actor");
    json_object_set_boolean_member(o, "actor_hidden", hidden);
    json_object_set_boolean_member(o, "actor_expired", row->actor_expired);
    if (row->actor_method)
        json_object_set_string_member(o, "actor_method", row->actor_method);
    else
        json_object_set_null_member(o, "actor_method");
    if (row->actor_result)
        json_object_set_string_member(o, "actor_result", row->actor_result);
    else
        json_object_set_null_member(o, "actor_result");
    guint total = 0;
    JsonArray *changes = _project_changes(row->changes_json, full ? 0 : PCV_VMCH_SUMMARY_CHANGES, admin, subject,
                                          row->owner, &total);
    json_object_set_int_member(o, "change_count", total);
    json_object_set_array_member(o, "changes", changes);
    if (full) {
        JsonNode *snap = _parse(row->snapshot_json);
        JsonObject *so = json_object_new();
        JsonObject *src = (snap && JSON_NODE_HOLDS_OBJECT(snap)) ? json_node_get_object(snap) : NULL;
        json_object_set_member(so, "config", _project_flat(src ? json_object_get_member(src, "config") : NULL, admin,
                                                           subject, row->owner));
        json_object_set_member(so, "live_overlay",
                               _project_flat(src ? json_object_get_member(src, "live_overlay") : NULL, admin, subject,
                                             row->owner));
        json_object_set_boolean_member(so, "autostart",
                                       src ? json_object_get_boolean_member_with_default(src, "autostart", FALSE) : FALSE);
        if (src && json_object_get_boolean_member_with_default(src, "transient", FALSE))
            json_object_set_boolean_member(so, "transient", TRUE);
        json_object_set_object_member(o, "snapshot", so);
        if (snap)
            json_node_unref(snap);
        if (row->prev_entry_id)
            json_object_set_string_member(o, "prev_id", row->prev_entry_id);
        else
            json_object_set_null_member(o, "prev_id");
    }
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, o);
    return n;
}

gboolean
pcv_vm_cfg_entry_id_valid(const gchar *id)
{
    if (!id || strlen(id) != 32)
        return FALSE;
    for (const gchar *p = id; *p; p++) {
        if (!g_ascii_isdigit(*p) && !(*p >= 'a' && *p <= 'f'))
            return FALSE;
    }
    return TRUE;
}

gchar *
pcv_vm_cfg_cursor_encode(gint64 observed_at, const gchar *entry_id)
{
    gchar *raw = g_strdup_printf("%" G_GINT64_FORMAT ":%s", observed_at, entry_id ? entry_id : "");
    gchar *b64 = g_base64_encode((const guchar *)raw, strlen(raw));
    g_free(raw);
    return b64;
}

gboolean
pcv_vm_cfg_cursor_decode(const gchar *cursor, gint64 *observed_at, gchar entry_id_out[33])
{
    if (!cursor || !*cursor || strlen(cursor) > 128)
        return FALSE;
    for (const gchar *p = cursor; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '+' && *p != '/' && *p != '=')
            return FALSE;
    }
    gsize len = 0;
    guchar *raw = g_base64_decode(cursor, &len);
    gboolean ok = FALSE;
    if (raw && len > 34 && len < 64) {
        gchar *text = g_strndup((const gchar *)raw, len);
        gchar *colon = strchr(text, ':');
        if (colon) {
            *colon = '\0';
            gchar *end = NULL;
            gint64 v = g_ascii_strtoll(text, &end, 10);
            if (end && !*end && end != text && v >= 0 && pcv_vm_cfg_entry_id_valid(colon + 1)) {
                *observed_at = v;
                g_strlcpy(entry_id_out, colon + 1, 33);
                ok = TRUE;
            }
        }
        g_free(text);
    }
    g_free(raw);
    return ok;
}
