   
                       
                                                                                                    
  
                           
                                                                                                                
                                                                                    
                                                                                     
                                                                                  
                                                                                         
                                                                       
                                                                           
                                                                                       
                                                                                                                 
                                                                                                 
  
                                                                              
                                                                                 
                  
   
#include "cli_resource.h"
#include "cli_rpc.h"
#include "cli_output.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>

                                                      
static void
_tenant_usage(void)
{
    printf("%sUsage: pcvctl auth tenant <username> <tenant>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET));
}

                                                                            
void
cmd_auth_tenant(int argc, char *argv[])
{
    if (argc != 4 || !argv[2][0] || !argv[3][0]) {
        _tenant_usage();
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "username", argv[2]);
    json_object_set_string_member(params, "tenant", argv[3]);
    GError *e = NULL;
    gchar *resp = purectl_send_request("auth.user.tenant.set", params, &e);                         
    if (!resp) {                                             
        if (e)
            g_printerr("%s[!] auth.user.tenant.set 호출 실패: %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET));
        g_clear_error(&e);
        return;
    }
    print_action_response(resp, "AUTH_USER_TENANT_SET");
    g_free(resp);
}

                                                                           

                                                      
static void
_resource_usage(void)
{
    printf("%sUsage: pcvctl resource register <image|iso|pci|usb|storage|bridge> <name> <ref>\n"
           "       pcvctl resource unregister <name>\n"
           "       pcvctl resource grant <name> <tenant>\n"
           "       pcvctl resource revoke <name> <tenant>\n"
           "       pcvctl resource list [--kind K]\n"
           "       pcvctl resource unregistered [--tenant T] [--json]\n"
           "       pcvctl resource import <file.json>%s\n",
           cc(CYBER_YELLOW), cc(CYBER_RESET));
}

                                                                                 
static JsonNode *
_resource_result(JsonParser *parser, const gchar *resp)
{
    if (!resp || !json_parser_load_from_data(parser, resp, -1, NULL))
        return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return NULL;
    return json_object_get_member(json_node_get_object(root), "result");
}

                                                                                 
static gchar *
_resource_join(JsonArray *arr, const gchar *member)
{
    GString *s = g_string_new("");
    for (guint i = 0; arr && i < json_array_get_length(arr); i++) {
        JsonNode *n = json_array_get_element(arr, i);
        const gchar *v = NULL;
        if (member && JSON_NODE_HOLDS_OBJECT(n))
            v = json_object_get_string_member_with_default(json_node_get_object(n), member, "");
        else if (!member && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
            v = json_node_get_string(n);
        if (v)
            g_string_append_printf(s, "%s%s", s->len ? "," : "", v);
    }
    return g_string_free(s, FALSE);
}

                                                                           
static const gchar *
_resource_str(JsonObject *o, const gchar *member)
{
    JsonNode *n = o ? json_object_get_member(o, member) : NULL;
    return (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING) ? json_node_get_string(n) : NULL;
}

                          
static JsonArray *
_resource_array(JsonObject *o, const gchar *member)
{
    JsonNode *n = o ? json_object_get_member(o, member) : NULL;
    return (n && JSON_NODE_HOLDS_ARRAY(n)) ? json_node_get_array(n) : NULL;
}

                                                                             
static void
_resource_print_table(PcvTable *t, const int *widths, const gchar *empty)
{
    if (g_ctx.fmt == FMT_CSV) {
        ptbl_print_csv(t);
        return;
    }
    if (g_ctx.fmt == FMT_PLAIN) {
        ptbl_print_plain(t);
        return;
    }
    if (t->rows->len == 0) {
        printf("%s\n", empty);
        return;
    }
    for (gint r = -1; r < (gint)t->rows->len; r++) {
        const char *const *row = r < 0 ? t->headers : (const char *const *)g_ptr_array_index(t->rows, r);
        printf("  ");
        for (size_t c = 0; c < t->ncols; c++) {
            if (c + 1 < t->ncols)
                printf("%-*s ", widths[c], row[c]);
            else
                printf("%s\n", row[c]);
        }
    }
}

                                                   
static void
_resource_print_list(JsonNode *result)
{
    JsonArray *arr = JSON_NODE_HOLDS_ARRAY(result) ? json_node_get_array(result) : NULL;
    const char *hdrs[] = { "NAME", "KIND", "REF", "TENANTS" };
    PcvTable *t = ptbl_new(hdrs, 4);
    for (guint i = 0; arr && i < json_array_get_length(arr); i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        g_autofree gchar *tenants = _resource_join(_resource_array(o, "tenants"), NULL);
        ptbl_row(t, json_object_get_string_member_with_default(o, "name", ""),
                 json_object_get_string_member_with_default(o, "kind", ""),
                 json_object_get_string_member_with_default(o, "ref", ""), tenants);
    }
    static const int widths[] = { 24, 8, 48 };
    _resource_print_table(t, widths, "(등록된 호스트 자원 없음)");
    ptbl_free(t);
}

                                                                 
static void
_resource_print_unregistered(JsonNode *result)
{
    JsonObject *res = JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    JsonArray *items = _resource_array(res, "items");
    const char *hdrs[] = { "KIND", "REF", "SUGGESTED", "VMS" };
    PcvTable *t = ptbl_new(hdrs, 4);
    for (guint i = 0; items && i < json_array_get_length(items); i++) {
        JsonObject *o = json_array_get_object_element(items, i);
        g_autofree gchar *vms = _resource_join(_resource_array(o, "vms"), "name");
        ptbl_row(t, json_object_get_string_member_with_default(o, "kind", ""),
                 json_object_get_string_member_with_default(o, "ref", ""),
                 json_object_get_string_member_with_default(o, "suggested_name", ""), vms);
    }
    static const int widths[] = { 8, 48, 32 };
    _resource_print_table(t, widths, "(미등록 호스트 자원 참조 없음)");
    ptbl_free(t);
}

                                                                             
static void
_resource_print_json(JsonNode *result)
{
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, result);
    gchar *s = json_generator_to_data(gen, NULL);
    printf("%s\n", s);
    g_free(s);
    g_object_unref(gen);
}

                                                                                           
static void
_resource_finish(gchar *resp, GError *e, const gchar *method, const gchar *label, void (*print)(JsonNode *result),
                 gboolean as_json)
{
    if (!resp) {                                             
        if (e)
            g_printerr("%s[!] %s 호출 실패: %s%s\n", ce(CYBER_RED), method, e->message, ce(CYBER_RESET));
        g_clear_error(&e);
        return;
    }
    g_clear_error(&e);
    JsonParser *parser = json_parser_new();
    JsonNode *result = (print && (as_json || g_ctx.fmt != FMT_JSON)) ? _resource_result(parser, resp) : NULL;
    if (result && as_json)
        _resource_print_json(result);
    else if (result)
        print(result);
    else
        print_action_response(resp, label);
    g_object_unref(parser);
    g_free(resp);
}

                                                                                                           
                                                                 
static JsonObject *
_resource_import_params(const gchar *path, gchar **why)
{
    gchar *text = NULL;
    GError *e = NULL;
    if (!g_file_get_contents(path, &text, NULL, &e)) {
        *why = g_strdup(e->message);
        g_error_free(e);
        return NULL;
    }
    JsonParser *parser = json_parser_new();
    gboolean parsed = json_parser_load_from_data(parser, text, -1, &e);
    g_free(text);
    JsonNode *root = parsed ? json_parser_get_root(parser) : NULL;
    JsonArray *items = (root && JSON_NODE_HOLDS_OBJECT(root)) ? _resource_array(json_node_get_object(root), "items") : NULL;
    if (!items) {
        *why = g_strdup_printf("%s: expected a JSON object with an items array (%s)", path,
                               e ? e->message : "no items array");
        g_clear_error(&e);
        g_object_unref(parser);
        return NULL;
    }
    JsonArray *out = json_array_new();
    for (guint i = 0; i < json_array_get_length(items); i++) {
        JsonNode *n = json_array_get_element(items, i);
        JsonObject *src = JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
        JsonObject *dst = json_object_new();
        const gchar *name = _resource_str(src, "name");
        if (!name)
            name = _resource_str(src, "suggested_name");
        if (src && json_object_has_member(src, "kind"))
            json_object_set_member(dst, "kind", json_node_copy(json_object_get_member(src, "kind")));
        if (name)
            json_object_set_string_member(dst, "name", name);
        if (src && json_object_has_member(src, "ref"))
            json_object_set_member(dst, "ref", json_node_copy(json_object_get_member(src, "ref")));
        if (src && json_object_has_member(src, "tenants"))
            json_object_set_member(dst, "tenants", json_node_copy(json_object_get_member(src, "tenants")));
        json_array_add_object_element(out, dst);                                   
    }
    g_object_unref(parser);
    JsonObject *params = json_object_new();
    json_object_set_array_member(params, "items", out);
    return params;
}

                                                           
static JsonObject *
_resource_params(const gchar *k1, const gchar *v1, const gchar *k2, const gchar *v2)
{
    JsonObject *p = json_object_new();
    if (k1)
        json_object_set_string_member(p, k1, v1);
    if (k2)
        json_object_set_string_member(p, k2, v2);
    return p;
}

                                                               
static gboolean
_resource_unregistered_opts(int argc, char *argv[], const gchar **tenant, gboolean *as_json)
{
    for (int i = 2; i < argc; i++) {
        if (g_strcmp0(argv[i], "--json") == 0)
            *as_json = TRUE;
        else if (g_strcmp0(argv[i], "--tenant") == 0 && i + 1 < argc && argv[i + 1][0])
            *tenant = argv[++i];
        else
            return FALSE;
    }
    return TRUE;
}

                                                                                                         
                                                        
void
cmd_resource(int argc, char *argv[])
{
    const gchar *sub = argc > 1 ? argv[1] : NULL;
    GError *e = NULL;
    if (g_strcmp0(sub, "register") == 0 && argc == 5) {
        JsonObject *p = _resource_params("kind", argv[2], "name", argv[3]);
        json_object_set_string_member(p, "ref", argv[4]);
        gchar *r = purectl_send_request("resource.register", p, &e);
        _resource_finish(r, e, "resource.register", "RESOURCE_REGISTER", NULL, FALSE);
    } else if (g_strcmp0(sub, "unregister") == 0 && argc == 3) {
        gchar *r = purectl_send_request("resource.unregister", _resource_params("name", argv[2], NULL, NULL), &e);
        _resource_finish(r, e, "resource.unregister", "RESOURCE_UNREGISTER", NULL, FALSE);
    } else if (g_strcmp0(sub, "grant") == 0 && argc == 4) {
        gchar *r = purectl_send_request("resource.grant", _resource_params("name", argv[2], "tenant", argv[3]), &e);
        _resource_finish(r, e, "resource.grant", "RESOURCE_GRANT", NULL, FALSE);
    } else if (g_strcmp0(sub, "revoke") == 0 && argc == 4) {
        gchar *r = purectl_send_request("resource.revoke", _resource_params("name", argv[2], "tenant", argv[3]), &e);
        _resource_finish(r, e, "resource.revoke", "RESOURCE_REVOKE", NULL, FALSE);
    } else if (g_strcmp0(sub, "list") == 0 && (argc == 2 || (argc == 4 && g_strcmp0(argv[2], "--kind") == 0))) {
        JsonObject *p = argc == 4 ? _resource_params("kind", argv[3], NULL, NULL) : json_object_new();
        gchar *r = purectl_send_request("resource.list", p, &e);
        _resource_finish(r, e, "resource.list", "RESOURCE_LIST", _resource_print_list, FALSE);
    } else if (g_strcmp0(sub, "unregistered") == 0) {
        const gchar *tenant = NULL;
        gboolean as_json = FALSE;
        if (!_resource_unregistered_opts(argc, argv, &tenant, &as_json)) {
            _resource_usage();
            return;
        }
        JsonObject *p = tenant ? _resource_params("tenant", tenant, NULL, NULL) : json_object_new();
        gchar *r = purectl_send_request("resource.unregistered.list", p, &e);
        _resource_finish(r, e, "resource.unregistered.list", "RESOURCE_UNREGISTERED_LIST",
                         _resource_print_unregistered, as_json);
    } else if (g_strcmp0(sub, "import") == 0 && argc == 3) {
        gchar *why = NULL;
        JsonObject *p = _resource_import_params(argv[2], &why);
        if (!p) {                                                        
            g_printerr("%s[!] resource import: %s%s\n", ce(CYBER_RED), why, ce(CYBER_RESET));
            g_free(why);
            pcv_cli_command_mark_runtime_failure();
            return;
        }
        gchar *r = purectl_send_request("resource.import", p, &e);
        _resource_finish(r, e, "resource.import", "RESOURCE_IMPORT", NULL, FALSE);
    } else {
        _resource_usage();
    }
}
