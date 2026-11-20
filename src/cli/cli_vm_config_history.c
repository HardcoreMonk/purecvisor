#include "cli_vm_config_history.h"
#include "cli_output.h"
#include "cli_rpc.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

static void
_usage_list(void)
{
    printf("%sUsage: pcvctl vm config-history [--vm NAME|UUID] [--uuid UUID] [--since TIME] [--until TIME] [--limit N]%s\n",
           cc(CYBER_YELLOW), cc(CYBER_RESET));
    printf("TIME: unix ms or ISO 8601 (e.g. 2026-11-20T09:00:00+09:00). JSON: pcvctl --format=json vm config-history ...\n");
}

static gint64
_parse_time(const gchar *s)
{
    gchar *end = NULL;
    gint64 v = g_ascii_strtoll(s, &end, 10);
    if (end && *end == '\0' && end != s)
        return v >= 0 ? v : -1;
    GTimeZone *tz = g_time_zone_new_local();
    GDateTime *dt = g_date_time_new_from_iso8601(s, tz);
    g_time_zone_unref(tz);
    if (!dt)
        return -1;
    gint64 ms = g_date_time_to_unix(dt) * 1000 + g_date_time_get_microsecond(dt) / 1000;
    g_date_time_unref(dt);
    return ms;
}

static const gchar *
_str(JsonObject *o, const gchar *k)
{
    JsonNode *n = o ? json_object_get_member(o, k) : NULL;
    return (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING) ? json_node_get_string(n) : NULL;
}

static gint64
_int(JsonObject *o, const gchar *k, gint64 def)
{
    JsonNode *n = o ? json_object_get_member(o, k) : NULL;
    return (n && JSON_NODE_HOLDS_VALUE(n)) ? json_node_get_int(n) : def;
}

static gchar *
_fmt_time(gint64 ms)
{
    GDateTime *dt = g_date_time_new_from_unix_local(ms / 1000);
    gchar *s = dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M:%S") : g_strdup("-");
    if (dt)
        g_date_time_unref(dt);
    return s;
}

static const gchar *
_actor(JsonObject *e)
{
    if (json_object_get_boolean_member_with_default(e, "actor_expired", FALSE))
        return "(expired)";
    if (json_object_get_boolean_member_with_default(e, "actor_hidden", FALSE))
        return "(hidden)";
    const gchar *a = _str(e, "actor");
    return a ? a : "-";
}

static gchar *
_val(JsonNode *n)
{
    if (!n || JSON_NODE_HOLDS_NULL(n))
        return g_strdup("-");
    if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
        return g_strdup(json_node_get_string(n));
    return json_to_string(n, FALSE);
}

static JsonObject *
_result(JsonParser *parser, const gchar *resp)
{
    JsonNode *root = json_parser_load_from_data(parser, resp, -1, NULL) ? json_parser_get_root(parser) : NULL;
    JsonObject *ro = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
    if (ro && json_object_has_member(ro, "error")) {
        JsonObject *eo = json_object_get_object_member(ro, "error");
        const gchar *m = _str(eo, "message");
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), m ? m : "request failed", ce(CYBER_RESET));
        return NULL;
    }
    JsonNode *rn = ro ? json_object_get_member(ro, "result") : NULL;
    return rn && JSON_NODE_HOLDS_OBJECT(rn) ? json_node_get_object(rn) : NULL;
}

static gchar *
_send(const gchar *method, JsonObject *params)
{
    GError *error = NULL;
    gchar *resp = purectl_send_request(method, params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error);
        return NULL;
    }
    return resp;
}

static void
_print_table(PcvTable *t)
{
    glong w[16] = {0};
    for (size_t c = 0; c < t->ncols; c++) {
        w[c] = g_utf8_strlen(t->headers[c], -1);
        for (guint r = 0; r < t->rows->len; r++)
            w[c] = MAX(w[c], g_utf8_strlen(((gchar **)g_ptr_array_index(t->rows, r))[c], -1));
    }
    for (gint r = -1; r < (gint)t->rows->len; r++) {
        const char *const *row = r < 0 ? t->headers : (const char *const *)g_ptr_array_index(t->rows, r);
        printf("%s ", r < 0 ? cc(CYBER_BOLD) : "");
        for (size_t c = 0; c + 1 < t->ncols; c++)
            printf("%s%*s ", row[c], (int)(w[c] - g_utf8_strlen(row[c], -1)), "");
        printf("%s%s\n", row[t->ncols - 1], r < 0 ? cc(CYBER_RESET) : "");
    }
}

static void
_emit(PcvTable *t)
{
    if (g_ctx.fmt == FMT_CSV)
        ptbl_print_csv(t);
    else if (g_ctx.fmt == FMT_PLAIN)
        ptbl_print_plain(t);
    else
        _print_table(t);
}

void
cmd_vm_config_history(int argc, char *argv[])
{
    JsonObject *params = json_object_new();
    gboolean ok = TRUE;
    for (int i = 2; ok && i < argc; i += 2) {
        const gchar *flag = argv[i], *val = i + 1 < argc ? argv[i + 1] : NULL;
        if (!val || !*val) {
            ok = FALSE;
        } else if (!g_strcmp0(flag, "--vm") || !g_strcmp0(flag, "--uuid")) {
            json_object_set_string_member(params, flag + 2, val);
        } else if (!g_strcmp0(flag, "--since") || !g_strcmp0(flag, "--until")) {
            gint64 ms = _parse_time(val);
            ok = ms >= 0;
            if (ok)
                json_object_set_int_member(params, flag + 2, ms);
        } else if (!g_strcmp0(flag, "--limit")) {
            gchar *end = NULL;
            gint64 n = g_ascii_strtoll(val, &end, 10);
            ok = end && *end == '\0' && n >= 1 && n <= 200;
            if (ok)
                json_object_set_int_member(params, "limit", n);
        } else {
            ok = FALSE;
        }
    }
    if (!ok) {
        json_object_unref(params);
        _usage_list();
        return;
    }
    gchar *resp = _send("vm.config.history.list", params);
    if (!resp)
        return;
    if (g_ctx.fmt == FMT_JSON) {
        print_raw_response(resp);
        g_free(resp);
        return;
    }
    JsonParser *parser = json_parser_new();
    JsonObject *res = _result(parser, resp);
    if (res) {
        static const char *hdr[] = {"ID", "TIME", "VM", "KIND", "CAUSE", "ACTOR", "CHANGES"};
        PcvTable *t = ptbl_new(hdr, G_N_ELEMENTS(hdr));
        JsonArray *entries = json_object_get_array_member(res, "entries");
        for (guint i = 0; entries && i < json_array_get_length(entries); i++) {
            JsonObject *e = json_array_get_object_element(entries, i);
            const gchar *id = _str(e, "id");
            gchar *sid = g_strdup_printf("%.8s…", id ? id : "");
            gchar *when = _fmt_time(_int(e, "observed_at", 0));
            gchar *tcell = _int(e, "window_start", 0) > 0 ? g_strconcat("~", when, NULL) : g_strdup(when);
            JsonArray *ch = json_object_get_array_member(e, "changes");
            JsonObject *c0 = ch && json_array_get_length(ch) ? json_array_get_object_element(ch, 0) : NULL;
            gint64 n = _int(e, "change_count", 0);
            gchar *chg = c0 ? g_strdup_printf("%" G_GINT64_FORMAT " %s%s", n, _str(c0, "path"), n > 1 ? ",…" : "")
                            : g_strdup_printf("%" G_GINT64_FORMAT, n);
            ptbl_row(t, sid, tcell, _str(e, "vm_name") ? _str(e, "vm_name") : "-", _str(e, "kind") ? _str(e, "kind") : "-",
                     _str(e, "cause") ? _str(e, "cause") : "-", _actor(e), chg);
            g_free(sid);
            g_free(when);
            g_free(tcell);
            g_free(chg);
        }
        _emit(t);
        if (g_ctx.fmt == FMT_TABLE) {
            if (t->rows->len == 0)
                printf(" (no configuration changes in range)\n");
            JsonObject *st = json_object_get_object_member(res, "store");
            JsonArray *gaps = json_object_get_array_member(res, "gaps");
            printf("store: %s, gaps %u%s\n", _str(st, "state") ? _str(st, "state") : "-",
                   gaps ? json_array_get_length(gaps) : 0, _str(res, "next_cursor") ? ", more (use --format=json for cursor)" : "");
        }
        ptbl_free(t);
    }
    g_object_unref(parser);
    g_free(resp);
}

void
cmd_vm_config_history_show(int argc, char *argv[])
{
    gboolean ok = argc == 3 && strlen(argv[2]) == 32;
    for (const gchar *p = ok ? argv[2] : ""; *p; p++)
        ok = ok && (g_ascii_isdigit(*p) || (*p >= 'a' && *p <= 'f'));
    if (!ok) {
        printf("%sUsage: pcvctl vm config-history-show <id>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET));
        printf("id: 32 lowercase hex from `pcvctl --format=json vm config-history`\n");
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "id", argv[2]);
    gchar *resp = _send("vm.config.history.get", params);
    if (!resp)
        return;
    if (g_ctx.fmt == FMT_JSON) {
        print_raw_response(resp);
        g_free(resp);
        return;
    }
    JsonParser *parser = json_parser_new();
    JsonObject *res = _result(parser, resp);
    if (res) {
        if (g_ctx.fmt == FMT_TABLE) {
            gchar *when = _fmt_time(_int(res, "observed_at", 0));
            printf("%s %s %s (%s)%s\n", cc(CYBER_CYAN), _str(res, "vm_name") ? _str(res, "vm_name") : "-",
                   _str(res, "kind") ? _str(res, "kind") : "-", _str(res, "cause") ? _str(res, "cause") : "-",
                   cc(CYBER_RESET));
            printf(" TIME   : %s%s\n", _int(res, "window_start", 0) > 0 ? "~" : "", when);
            printf(" ACTOR  : %s (%s)\n", _actor(res), _str(res, "attribution") ? _str(res, "attribution") : "-");
            g_free(when);
        }
        static const char *hdr[] = {"PATH", "BEFORE", "AFTER"};
        PcvTable *t = ptbl_new(hdr, G_N_ELEMENTS(hdr));
        JsonArray *ch = json_object_get_array_member(res, "changes");
        for (guint i = 0; ch && i < json_array_get_length(ch); i++) {
            JsonObject *c = json_array_get_object_element(ch, i);
            gchar *b = _val(json_object_get_member(c, "before")), *a = _val(json_object_get_member(c, "after"));
            ptbl_row(t, _str(c, "path") ? _str(c, "path") : "-", b, a);
            g_free(b);
            g_free(a);
        }
        _emit(t);
        ptbl_free(t);
    }
    g_object_unref(parser);
    g_free(resp);
}
