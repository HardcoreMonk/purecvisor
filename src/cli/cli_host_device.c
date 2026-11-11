   
                          
                                                                                                        
  
                           
                                                                                                   
                                                                                     
                                                                                             
                                                                                                  
                                                                   
                                                                                              
                                                                                                                 
                                                                                                                   
                                                                                            
                                                                                                       
                                                                                              
                                                                                                 
                                                                                                
                                                                                                    
                                                                    
  
                                                                                 
                                                               
   
#include "cli_host_device.h"
#include "cli_rpc.h"
#include "cli_output.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

                                                                                     
static const gchar *
_hd_str(JsonObject *o, const gchar *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL;
}

                     
static gboolean
_hd_flag(JsonObject *o, const gchar *key)
{
    return o && json_object_get_boolean_member_with_default(o, key, FALSE);
}

                              
static JsonArray *
_hd_arr(JsonObject *o, const gchar *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL;
}

                                
G_GNUC_PRINTF(2, 3) static void
_hd_tok(GString *s, const gchar *fmt, ...)
{
    va_list ap;
    if (s->len)
        g_string_append_c(s, ';');
    va_start(ap, fmt);
    g_string_append_vprintf(s, fmt, ap);
    va_end(ap);
}

                                                      
static const gchar *
_hd_label(JsonObject *a)
{
    const gchar *vm = _hd_flag(a, "vm_visible") ? _hd_str(a, "vm") : NULL;
    return !a ? "host" : vm ? vm : "(other VM)";
}

                                                                                            
                                                                                         
static gchar *
_hd_state(JsonObject *dev, JsonObject *a, gboolean dev_tok)
{
    GString *s = g_string_new(NULL);
    gboolean usb = g_strcmp0(_hd_str(dev, "kind"), "usb") == 0, present = _hd_flag(dev, "present");
    const gchar *reason = _hd_str(dev, "blocked_reason");
    if (a) {
        gboolean live = _hd_flag(a, "live"), persistent = _hd_flag(a, "persistent");
        if (live)
            _hd_tok(s, "live");
        if (persistent)
            _hd_tok(s, "persistent");
        else if (live)
            _hd_tok(s, "live-only");                               
        if (_hd_flag(a, "missing"))
            _hd_tok(s, "placeholder");                                            
        if (_hd_flag(a, "stale_live_address"))
            _hd_tok(s, "stale-address");                                         
        if (_hd_flag(a, "ambiguous"))
            _hd_tok(s, "ambiguous");
        if (usb && persistent && !live && present && _hd_flag(a, "vm_active"))
            _hd_tok(s, "attach-now");                                                   
    }
    if (dev_tok && !present)
        _hd_tok(s, "not-on-host");
    if (dev_tok && _hd_flag(dev, "host_display"))
        _hd_tok(s, "host-display");
    if (dev_tok && !usb && present)
        _hd_tok(s, "driver=%s", _hd_str(dev, "driver") ? _hd_str(dev, "driver") : "-");
    if (a && !_hd_flag(a, "vm_active"))
        _hd_tok(s, "vm-off");                                           
    if (dev_tok && _hd_flag(dev, "partial_assignment"))
        _hd_tok(s, "partial");                                                 
    if (a && !usb && present && g_strcmp0(_hd_str(a, "managed"), "yes") == 0)
        _hd_tok(s, "managed=yes");                                                    
    if (dev_tok && reason && !(g_strcmp0(reason, "host_display") == 0 && _hd_flag(dev, "host_display"))
        && !(g_strcmp0(reason, "not_present") == 0 && !present))
        _hd_tok(s, "blocked=%s", reason);                                        
    if (!s->len)
        g_string_append_c(s, '-');
    return g_string_free(s, FALSE);
}

                                                                                                   
static gchar *
_hd_companions(JsonObject *dev)
{
    JsonArray *comps = _hd_arr(dev, "companions"), *mine = _hd_arr(dev, "assignments");
    if (!comps || json_array_get_length(comps) == 0)
        return NULL;
    GString *s = g_string_new(NULL);
    for (guint k = 0; k < json_array_get_length(comps); k++) {
        JsonNode *cn = json_array_get_element(comps, k);
        JsonObject *c = JSON_NODE_HOLDS_OBJECT(cn) ? json_node_get_object(cn) : NULL;
        JsonArray *theirs = _hd_arr(c, "assignments");
        guint n = theirs ? json_array_get_length(theirs) : 0, m = mine ? json_array_get_length(mine) : 0;
        gboolean same = n == m;
        for (guint i = 0; same && i < n; i++)
            same = g_strcmp0(_hd_label(json_array_get_object_element(theirs, i)),
                             _hd_label(json_array_get_object_element(mine, i))) == 0;
        g_string_append_printf(s, "%s  + %s %s", s->len ? "\n" : "", _hd_str(c, "pci_addr") ? _hd_str(c, "pci_addr") : "-",
                               _hd_str(c, "name") ? _hd_str(c, "name") : "-");
        if (!same && n == 0)
            g_string_append(s, " [host]");
        for (guint i = 0; !same && i < n; i++) {
            JsonObject *a = json_array_get_object_element(theirs, i);
            g_autofree gchar *st = _hd_state(dev, a, FALSE);
            g_string_append_printf(s, " [%s %s]", _hd_label(a), st);
        }
    }
    return g_string_free(s, FALSE);
}

                                                                                            
static void
_hd_rows(PcvTable *t, GPtrArray *aux, JsonObject *dev)
{
    const gchar *name = _hd_str(dev, "name");
    const gchar *group = _hd_str(dev, "group"), *id = _hd_str(dev, "id");
    JsonArray *asg = _hd_arr(dev, "assignments");
    guint n = asg ? json_array_get_length(asg) : 0;
    for (guint i = 0; i < MAX(n, 1u); i++) {
        JsonObject *a = n ? json_array_get_object_element(asg, i) : NULL;
        g_autofree gchar *state = _hd_state(dev, a, TRUE);
        ptbl_row(t, group ? group : "-", id ? id : "-", name ? name : "-", _hd_label(a), state);
        g_ptr_array_add(aux, i + 1 < MAX(n, 1u) ? NULL : _hd_companions(dev));
    }
}

                                                                         
static void
_hd_print_table(PcvTable *t, GPtrArray *aux)
{
    static const char *head[] = { "GROUP", "ID", "NAME", "ASSIGNED", "STATE" };
    glong w[4];
    for (int c = 0; c < 4; c++) {
        w[c] = (glong)strlen(head[c]);
        for (guint r = 0; r < t->rows->len; r++)
            w[c] = MAX(w[c], g_utf8_strlen(((gchar **)g_ptr_array_index(t->rows, r))[c], -1));
    }
    for (gint r = -1; r < (gint)t->rows->len; r++) {
        const char *const *row = r < 0 ? head : (const char *const *)g_ptr_array_index(t->rows, r);
        printf("%s ", r < 0 ? cc(CYBER_BOLD) : "");
        for (int c = 0; c < 4; c++)
            printf("%s%*s ", row[c], (int)(w[c] - g_utf8_strlen(row[c], -1)), "");
        printf("%s%s\n", row[4], r < 0 ? cc(CYBER_RESET) : "");
        if (r >= 0 && g_ptr_array_index(aux, r))
            printf("%s\n", (const gchar *)g_ptr_array_index(aux, r));
    }
    if (t->rows->len == 0)
        printf(" (no host devices)\n");
}

                                                                                  
static void
_hd_notes(JsonObject *res, guint ndev)
{
    JsonNode *sn = json_object_get_member(res, "sources");
    JsonObject *src = sn && JSON_NODE_HOLDS_OBJECT(sn) ? json_node_get_object(sn) : NULL;
    if (_hd_flag(res, "truncated"))
        printf("(truncated: showing %u devices — full list: pcvctl --format=json host devices --all)\n", ndev);
    if (g_strcmp0(_hd_str(src, "usb"), "absent") == 0)
        printf("(host USB device list not available on this host)\n");
    if (g_strcmp0(_hd_str(src, "pci"), "absent") == 0)
        printf("(host PCI device list not available on this host)\n");
    if (g_strcmp0(_hd_str(src, "names"), "unavailable") == 0)
        printf("(device name database unavailable — IDs only)\n");
}

void
cmd_host_devices(int argc, char *argv[])
{
    gboolean all = argc == 3 && g_strcmp0(argv[2], "--all") == 0;
    if (argc > 3 || (argc == 3 && !all)) {                                        
        printf("%sUsage: pcvctl host devices [--all]%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET));
        printf("JSON: pcvctl --format=json host devices --all\n");
        return;
    }
    GError *error = NULL;
    gchar *resp = purectl_send_request("host.device.list", NULL, &error);
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
    JsonNode *rn = ro ? json_object_get_member(ro, "result") : NULL;
    JsonObject *res = rn && JSON_NODE_HOLDS_OBJECT(rn) ? json_node_get_object(rn) : NULL;
    JsonArray *devs = _hd_arr(res, "devices");
    if (ro && json_object_has_member(ro, "error")) {
        JsonNode *en = json_object_get_member(ro, "error");
        const gchar *msg = JSON_NODE_HOLDS_OBJECT(en) ? _hd_str(json_node_get_object(en), "message") : NULL;
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), msg ? msg : "host device list failed", ce(CYBER_RESET));
    } else if (devs) {
        const char *hdrs[] = { "group", "id", "name", "assigned", "state" };
        PcvTable *t = ptbl_new(hdrs, 5);
        g_autoptr(GPtrArray) aux = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < json_array_get_length(devs); i++) {
            JsonNode *d = json_array_get_element(devs, i);
            if (JSON_NODE_HOLDS_OBJECT(d) && (all || !_hd_flag(json_node_get_object(d), "hidden_by_default")))
                _hd_rows(t, aux, json_node_get_object(d));
        }
        if (g_ctx.fmt == FMT_CSV)
            ptbl_print_csv(t);
        else if (g_ctx.fmt == FMT_PLAIN)
            ptbl_print_plain(t);
        else
            _hd_print_table(t, aux);
        if (g_ctx.fmt != FMT_CSV)
            _hd_notes(res, json_array_get_length(devs));
        ptbl_free(t);
    } else {
        print_action_response(resp, "HOST_DEVICES");
    }
    g_free(resp);
}

                                        
                                                              
void cmd_gpu_list(int argc, char *argv[]) {
    GError *e = NULL; gchar *r = purectl_send_request("gpu.list", NULL, &e);
    if (e) { g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET)); g_error_free(e); return; }
    if (!r) return;
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(r); g_free(r); return; }
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, r, -1, NULL)) { g_object_unref(parser); g_free(r); return; }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *err = json_object_get_object_member(root, "error");
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), json_object_get_string_member(err, "message"), ce(CYBER_RESET));
    } else {
        JsonArray *arr = json_object_has_member(root, "result")
            ? json_node_get_array(json_object_get_member(root, "result")) : NULL;
        if (arr && json_array_get_length(arr) > 0) {
            printf("%s%-12s %-40s %-10s%s\n", cc(CYBER_BOLD), "PCI_ADDR", "DEVICE", "DRIVER", cc(CYBER_RESET));
            printf("──────────────────────────────────────────────────────────────────\n");
            for (guint i = 0; i < json_array_get_length(arr); i++) {
                JsonObject *g = json_array_get_object_element(arr, i);
                printf("%-12s %-40s %-10s\n",
                    json_object_get_string_member_with_default(g, "pci_addr", "-"),
                    json_object_get_string_member_with_default(g, "device", "-"),
                    json_object_get_string_member_with_default(g, "driver", "-"));
            }
        } else {
            printf("No GPUs found.\n");
        }
    }
    g_object_unref(parser); g_free(r);
}

                                                               
void cmd_gpu_assignment(int argc, char *argv[]) {
    if (argc < 4) {
        printf("%sUsage: pcvctl gpu %s <vm_name> <canonical_pci_addr>%s\n",
            cc(CYBER_YELLOW), argc >= 2 ? argv[1] : "attach|detach", cc(CYBER_RESET));
        return;
    }
    gboolean attach = g_strcmp0(argv[1], "attach") == 0;
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_name", argv[2]);
    json_object_set_string_member(params, "pci_addr", argv[3]);
    GError *error = NULL;
    gchar *resp = purectl_send_request(
        attach ? "device.gpu.attach" : "device.gpu.detach", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error);
        return;
    }
    if (resp) {
        print_action_response(resp, attach ? "GPU_ATTACH" : "GPU_DETACH");
        g_free(resp);
    }
}
