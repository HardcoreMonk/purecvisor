   
                       
                                                                                              
                                                                                             
  
                           
                                                                                           
                                                                                      
                                                                               
                                                                                  
                                                                              
                                                                                                                     
                                                                                 
                                                       
  
                                                                           
                                                         
                                                                      
                                                                                
   
#include "cli_vpc_mgmt.h"
#include "cli_rpc.h"
#include "cli_output.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>

                                                       
static void
_confirm_usage(void)
{
    printf("%sUsage: pcvctl vpc subnet-confirm <subnet-id>%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET));
}

                                                                                    
static gboolean
_confirm_ok(const gchar *resp)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!resp || !json_parser_load_from_data(parser, resp, -1, NULL))
        return FALSE;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return FALSE;
    JsonNode *r = json_object_get_member(json_node_get_object(root), "result");
    return r && JSON_NODE_HOLDS_OBJECT(r) &&
           json_object_get_boolean_member_with_default(json_node_get_object(r), "confirmed", FALSE);
}

void
cmd_vpc_subnet_confirm(int argc, char *argv[])
{
    if (argc != 3 || !g_uuid_string_is_valid(argv[2])) {
        _confirm_usage();
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "subnet_id", argv[2]);
    GError *e = NULL;
    gchar *resp = purectl_send_request("vpc.subnet.confirm", params, &e);                         
    if (!resp) {                                                                
        if (e)
            g_printerr("%s[!] vpc.subnet.confirm 호출 실패: %s%s\n", ce(CYBER_RED), e->message,
                       ce(CYBER_RESET));
        g_clear_error(&e);
        return;
    }
    if (g_ctx.fmt == FMT_TABLE && _confirm_ok(resp))
        printf("%s[+] subnet %s 확인 완료 — 자동 원복을 취소했습니다%s\n", cc(CYBER_GREEN), argv[2],
               cc(CYBER_RESET));
    else
        print_action_response(resp, "VPC_SUBNET_CONFIRM");
    g_free(resp);
}

                                                                     

                                                       
static void
_pool_usage(void)
{
    printf("%sUsage: pcvctl vpc pool-set <tenant> <cidr>[,<cidr>...]   (\"\" clears the pool)\n"
           "       pcvctl vpc pool-list [--tenant <tenant>]%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET));
}

                                                                                        
static JsonNode *
_pool_result(JsonParser *parser, const gchar *resp)
{
    if (!resp || !json_parser_load_from_data(parser, resp, -1, NULL))
        return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return NULL;
    return json_object_get_member(json_node_get_object(root), "result");
}

                                                              
static gchar *
_pool_join(JsonObject *o, const gchar *member, const gchar *empty)
{
    JsonNode *n = json_object_get_member(o, member);
    JsonArray *a = (n && JSON_NODE_HOLDS_ARRAY(n)) ? json_node_get_array(n) : NULL;
    GString *s = g_string_new(NULL);
    for (guint i = 0; a && i < json_array_get_length(a); i++)
        g_string_append_printf(s, "%s%s", i ? ", " : "", json_array_get_string_element(a, i));
    if (!s->len)
        g_string_assign(s, empty);
    return g_string_free(s, FALSE);
}

                                                                                     
static gboolean
_pool_print_set(JsonNode *result, const gchar *resp)
{
    if (!JSON_NODE_HOLDS_OBJECT(result))
        return FALSE;
    JsonObject *o = json_node_get_object(result);
    JsonNode *wn = json_object_get_member(o, "warnings");
    JsonArray *w = (wn && JSON_NODE_HOLDS_ARRAY(wn)) ? json_node_get_array(wn) : NULL;
    if (g_ctx.fmt == FMT_TABLE) {
        g_autofree gchar *cidrs = _pool_join(o, "cidrs", "(empty — OPERATOR subnet creation is denied)");
        printf("%s[+] tenant %s VPC 주소 풀: %s%s\n", cc(CYBER_GREEN),
               json_object_get_string_member_with_default(o, "tenant", ""), cidrs, cc(CYBER_RESET));
    } else {
        print_action_response(resp, "VPC_POOL_SET");
    }
    for (guint i = 0; w && i < json_array_get_length(w); i++) {
        if (g_ctx.fmt == FMT_TABLE)
            printf("%s[!] 경고: %s%s\n", cc(CYBER_YELLOW), json_array_get_string_element(w, i), cc(CYBER_RESET));
        else
            g_printerr("[!] warning: %s\n", json_array_get_string_element(w, i));
    }
    return TRUE;
}

                                                            
static gboolean
_pool_print_list(JsonNode *result, const gchar *resp)
{
    (void)resp;
    if (!JSON_NODE_HOLDS_ARRAY(result))
        return FALSE;
    JsonArray *pools = json_node_get_array(result);
    const char *hdrs[] = { "TENANT", "CIDR" };
    PcvTable *t = ptbl_new(hdrs, 2);
    for (guint i = 0; i < json_array_get_length(pools); i++) {
        JsonObject *p = json_array_get_object_element(pools, i);
        const gchar *tenant = json_object_get_string_member_with_default(p, "tenant", "");
        JsonNode *cn = json_object_get_member(p, "cidrs");
        JsonArray *c = (cn && JSON_NODE_HOLDS_ARRAY(cn)) ? json_node_get_array(cn) : NULL;
        guint n = c ? json_array_get_length(c) : 0;
        if (n == 0)
            ptbl_row(t, tenant, "");
        for (guint j = 0; j < n; j++)
            ptbl_row(t, tenant, json_array_get_string_element(c, j));
    }
    if (g_ctx.fmt == FMT_CSV) {
        ptbl_print_csv(t);
    } else if (g_ctx.fmt == FMT_PLAIN) {
        ptbl_print_plain(t);
    } else if (t->rows->len == 0) {
        printf("(배정된 VPC 주소 풀 없음)\n");
    } else {
        printf("  %-24s %s\n", "TENANT", "CIDR");
        for (guint i = 0; i < t->rows->len; i++) {
            gchar **row = g_ptr_array_index(t->rows, i);
            printf("  %-24s %s\n", row[0], row[1][0] ? row[1] : "(empty)");
        }
    }
    ptbl_free(t);
    return TRUE;
}

                                                                                               
static void
_pool_finish(gchar *resp, GError *e, const gchar *label, gboolean (*print)(JsonNode *result, const gchar *resp))
{
    if (!resp) {
        if (e)
            g_printerr("%s[!] %s 호출 실패: %s%s\n", ce(CYBER_RED), label, e->message, ce(CYBER_RESET));
        g_clear_error(&e);
        return;
    }
    g_clear_error(&e);
    JsonParser *parser = json_parser_new();
    JsonNode *result = g_ctx.fmt != FMT_JSON ? _pool_result(parser, resp) : NULL;
    if (!result || !print(result, resp))
        print_action_response(resp, label);
    g_object_unref(parser);
    g_free(resp);
}

void
cmd_vpc_pool_set(int argc, char *argv[])
{
    if (argc != 4 || !argv[2][0]) {
        _pool_usage();
        return;
    }
    JsonArray *cidrs = json_array_new();
    gchar **parts = argv[3][0] ? g_strsplit(argv[3], ",", -1) : g_new0(gchar *, 1);
    gboolean ok = TRUE;
    for (guint i = 0; ok && parts[i]; i++) {
        gchar *c = g_strstrip(parts[i]);
        ok = *c != '\0';
        if (ok)
            json_array_add_string_element(cidrs, c);
    }
    g_strfreev(parts);
    if (!ok) {
        json_array_unref(cidrs);
        _pool_usage();
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "tenant", argv[2]);
    json_object_set_array_member(params, "cidrs", cidrs);
    GError *e = NULL;
    gchar *resp = purectl_send_request("vpc.pool.set", params, &e);                         
    _pool_finish(resp, e, "VPC_POOL_SET", _pool_print_set);
}

void
cmd_vpc_pool_list(int argc, char *argv[])
{
    JsonObject *params = NULL;
    if (argc == 4 && g_strcmp0(argv[2], "--tenant") == 0 && argv[3][0]) {
        params = json_object_new();
        json_object_set_string_member(params, "tenant", argv[3]);
    } else if (argc != 2) {
        _pool_usage();
        return;
    }
    GError *e = NULL;
    gchar *resp = purectl_send_request("vpc.pool.list", params, &e);
    _pool_finish(resp, e, "VPC_POOL_LIST", _pool_print_list);
}
