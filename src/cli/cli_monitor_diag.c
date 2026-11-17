   
                           
                                                                                                        
  
                           
                                                                                                             
                                                                             
                                                                          
                                                                                              
                                          
                                                                                                                 
                                                                                               
                                                                                                                      
                                                                            
                                                                                             
                            
  
                                                                                 
                                                              
   
#include "cli_monitor_diag.h"
#include "cli_rpc.h"
#include "cli_output.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

                      
static JsonNode *
_md_node(JsonObject *o, const gchar *key)
{
    return o && json_object_has_member(o, key) ? json_object_get_member(o, key) : NULL;
}

                                
static const gchar *
_md_str(JsonObject *o, const gchar *key)
{
    JsonNode *n = _md_node(o, key);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}

                                 
static gdouble
_md_num(JsonObject *o, const gchar *key)
{
    JsonNode *n = _md_node(o, key);
    if (!n || !JSON_NODE_HOLDS_VALUE(n)) return NAN;
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_INT64) return (gdouble)json_node_get_int(n);
    if (t == G_TYPE_DOUBLE) return json_node_get_double(n);
    return NAN;
}

                              
static JsonObject *
_md_obj(JsonObject *o, const gchar *key)
{
    JsonNode *n = _md_node(o, key);
    return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
}

                                            
static gchar *
_md_fmt(gdouble v, gint decimals, const gchar *suffix)
{
    if (isnan(v)) return g_strdup("-");
    return g_strdup_printf("%.*f%s", decimals, v, suffix);
}

                                                                   
static gchar *
_md_rate(gdouble v)
{
    if (isnan(v)) return g_strdup("-");
    static const char *const U[] = { "", "K", "M", "G", "T" };
    gsize i = 0;
    while (v >= 1000.0 && i + 1 < G_N_ELEMENTS(U)) { v /= 1000.0; i++; }
    return i == 0 ? g_strdup_printf("%.0f", v) : g_strdup_printf("%.1f%s", v, U[i]);
}

                                  
static gchar *
_md_pair(gdouble a, gdouble b)
{
    if (isnan(a) && isnan(b)) return g_strdup("-");
    g_autofree gchar *x = _md_rate(a);
    g_autofree gchar *y = _md_rate(b);
    return g_strdup_printf("%s/%s", x, y);
}

                                      
static gchar *
_md_hints(JsonObject *vm)
{
    JsonNode *n = _md_node(vm, "hints");
    JsonArray *a = n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL;
    GString *s = g_string_new(NULL);
    for (guint i = 0; a && i < json_array_get_length(a); i++) {
        JsonNode *e = json_array_get_element(a, i);
        if (!JSON_NODE_HOLDS_OBJECT(e)) continue;
        JsonObject *h = json_node_get_object(e);
        const gchar *code = _md_str(h, "code"), *lvl = _md_str(h, "level");
        if (!code) continue;
        g_string_append_printf(s, "%s%s:%s", s->len ? "," : "", code, lvl ? lvl : "?");
    }
    if (!s->len) g_string_assign(s, "-");
    return g_string_free(s, FALSE);
}

                                                                    
static void
_md_sampler_line(JsonObject *res)
{
    JsonObject *sm = _md_obj(res, "sampler");
    const gchar *state = _md_str(sm, "state");
    const gchar *err = _md_str(sm, "error");
    gdouble age = _md_num(sm, "age_ms"), tracked = _md_num(sm, "tracked"), trunc = _md_num(sm, "truncated");
    g_autofree gchar *age_s = _md_fmt(age, 0, " ms");
    printf("sampler: %s%s%s, age %s, tracked %.0f, truncated %.0f, scope %s\n",
           state ? state : "-", err ? " " : "", err ? err : "", age_s,
           isnan(tracked) ? 0.0 : tracked, isnan(trunc) ? 0.0 : trunc, _md_str(res, "scope") ? _md_str(res, "scope") : "-");
}

                       
static void
_md_print_table(PcvTable *t)
{
    static const char *head[] = { "NAME", "STATE", "QUALITY", "CPU%", "HOST%", "MEM_ALLOC", "RSS",
                                  "DISK_R/W", "NET_RX/TX", "HINTS" };
    enum { NC = G_N_ELEMENTS(head) };
    glong w[NC];
    for (int c = 0; c < NC; c++) {
        w[c] = (glong)strlen(head[c]);
        for (guint r = 0; r < t->rows->len; r++)
            w[c] = MAX(w[c], g_utf8_strlen(((gchar **)g_ptr_array_index(t->rows, r))[c], -1));
    }
    for (gint r = -1; r < (gint)t->rows->len; r++) {
        const char *const *row = r < 0 ? head : (const char *const *)g_ptr_array_index(t->rows, r);
        printf("%s ", r < 0 ? cc(CYBER_BOLD) : "");
        for (int c = 0; c < NC - 1; c++)
            printf("%s%*s ", row[c], (int)(w[c] - g_utf8_strlen(row[c], -1)), "");
        printf("%s%s\n", row[NC - 1], r < 0 ? cc(CYBER_RESET) : "");
    }
    if (t->rows->len == 0)
        printf(" (no VMs)\n");
}

                                                            
void
cmd_monitor_diag(int argc, char *argv[])
{
    const gchar *vm = NULL;
    if (argc == 4 && g_strcmp0(argv[2], "--vm") == 0 && argv[3][0] != '\0') {
        vm = argv[3];
    } else if (argc != 2) {                                         
        printf("%sUsage: pcvctl monitor diag [--vm NAME]%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET));
        printf("JSON: pcvctl --format=json monitor diag [--vm NAME]\n");
        return;
    }
    JsonObject *params = NULL;
    if (vm) {
        params = json_object_new();
        json_object_set_string_member(params, "vm", vm);
    }
    GError *error = NULL;
    gchar *resp = purectl_send_request("monitor.vm.diag", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error);
        return;
    }
    if (!resp)
        return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root = json_parser_load_from_data(parser, resp, -1, NULL) ? json_parser_get_root(parser) : NULL;
    JsonObject *ro = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
    JsonObject *res = _md_obj(ro, "result");
    JsonNode *vn = _md_node(res, "vms");
    JsonArray *vms = vn && JSON_NODE_HOLDS_ARRAY(vn) ? json_node_get_array(vn) : NULL;
    if (ro && json_object_has_member(ro, "error")) {
        JsonObject *eo = _md_obj(ro, "error");
        const gchar *msg = _md_str(eo, "message");
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), msg ? msg : "VM diagnostics failed", ce(CYBER_RESET));
    } else if (vms) {
        const char *hdrs[] = { "name", "state", "quality", "cpu_pct", "cpu_host_pct", "mem_alloc_mb", "mem_rss_mb",
                               "disk_rd_bps", "disk_wr_bps", "net_rx_bps", "net_tx_bps", "hints" };
        gboolean raw = g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV;
        PcvTable *t = ptbl_new(hdrs, raw ? 12 : 10);
        for (guint i = 0; i < json_array_get_length(vms); i++) {
            JsonNode *e = json_array_get_element(vms, i);
            if (!JSON_NODE_HOLDS_OBJECT(e)) continue;
            JsonObject *o = json_node_get_object(e);
            JsonObject *d = _md_obj(o, "disk"), *n = _md_obj(o, "net");
            const gchar *name = _md_str(o, "name"), *state = _md_str(o, "state"), *q = _md_str(o, "quality");
            g_autofree gchar *hints = _md_hints(o);
            if (raw) {
                g_autofree gchar *c1 = _md_fmt(_md_num(o, "cpu_pct"), 2, "");
                g_autofree gchar *c2 = _md_fmt(_md_num(o, "cpu_host_pct"), 2, "");
                g_autofree gchar *m1 = _md_fmt(_md_num(o, "mem_alloc_mb"), 0, "");
                g_autofree gchar *m2 = _md_fmt(_md_num(o, "mem_rss_mb"), 0, "");
                g_autofree gchar *d1 = _md_fmt(_md_num(d, "rd_bps"), 0, "");
                g_autofree gchar *d2 = _md_fmt(_md_num(d, "wr_bps"), 0, "");
                g_autofree gchar *n1 = _md_fmt(_md_num(n, "rx_bps"), 0, "");
                g_autofree gchar *n2 = _md_fmt(_md_num(n, "tx_bps"), 0, "");
                ptbl_row(t, name ? name : "-", state ? state : "-", q ? q : "-", c1, c2, m1, m2, d1, d2, n1, n2, hints);
            } else {
                g_autofree gchar *c1 = _md_fmt(_md_num(o, "cpu_pct"), 1, "");
                g_autofree gchar *c2 = _md_fmt(_md_num(o, "cpu_host_pct"), 1, "");
                g_autofree gchar *m1 = _md_fmt(_md_num(o, "mem_alloc_mb"), 0, "M");
                g_autofree gchar *m2 = _md_fmt(_md_num(o, "mem_rss_mb"), 0, "M");
                g_autofree gchar *dk = _md_pair(_md_num(d, "rd_bps"), _md_num(d, "wr_bps"));
                g_autofree gchar *nt = _md_pair(_md_num(n, "rx_bps"), _md_num(n, "tx_bps"));
                ptbl_row(t, name ? name : "-", state ? state : "-", q ? q : "-", c1, c2, m1, m2, dk, nt, hints);
            }
        }
        if (g_ctx.fmt == FMT_CSV) {
            ptbl_print_csv(t);
        } else {
            _md_sampler_line(res);
            if (g_ctx.fmt == FMT_PLAIN) ptbl_print_plain(t);
            else _md_print_table(t);
        }
        ptbl_free(t);
    } else {
        print_action_response(resp, "MONITOR_DIAG");
    }
    g_free(resp);
}

   
                                                                  
                                                        
                                 
   
void cmd_monitor_metrics(int argc, char *argv[]) {
    if (argc < 3) {
        printf("%sUsage: pcvctl monitor metrics <vm_id>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params,"vm_id",argv[2]);
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("vm.metrics", params, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser,resp,-1,NULL)||!json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root,"result")) {
        JsonObject *res     = json_object_get_object_member(root,"result");
        int         cpu_pct = (int)json_object_get_int_member(res,"cpu");
        int         mem_pct = (int)json_object_get_int_member(res,"mem");

        if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
                                                                                          
            if (mem_pct < 0) printf("vm\tcpu\tmem\n%s\t%d\tN/A\n", argv[2], cpu_pct);
            else printf("vm\tcpu\tmem\n%s\t%d\t%d\n", argv[2], cpu_pct, mem_pct);
        } else {
            print_cyber_banner();
            printf("%s%s>>> REALTIME TELEMETRY: %s <<<\n\n%s",
                cc(CYBER_YELLOW), cc(CYBER_BOLD), argv[2], cc(CYBER_RESET));
            print_metrics_bar("CPU",    cpu_pct, cc(CYBER_GREEN));
            print_metrics_bar("MEMORY", mem_pct, cc(CYBER_RED));
            printf("\n");
        }
    }
    g_object_unref(parser);
    g_free(resp);
}
