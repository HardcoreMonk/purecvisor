   
                             
                                                                                  
  
                           
                                                                                     
                                                                                       
                                                                                
  
                                                                    
                                                    
   
#include "cli_backup_hostkey.h"
#include "cli_rpc.h"
#include "cli_output.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>

                                                       
static void
_hk_usage(void)
{
    printf("%sUsage: pcvctl backup hostkey scan <target>\n"
           "       pcvctl backup hostkey trust <target> <SHA256:fingerprint>\n"
           "       pcvctl backup hostkey list\n"
           "       pcvctl backup hostkey remove <target>%s\n",
           cc(CYBER_YELLOW), cc(CYBER_RESET));
}

                                                                                        
static JsonObject *
_hk_result(JsonParser *parser, const gchar *resp)
{
    if (!resp || !json_parser_load_from_data(parser, resp, -1, NULL))
        return NULL;
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root))
        return NULL;
    JsonNode *r = json_object_get_member(json_node_get_object(root), "result");
    return (r && JSON_NODE_HOLDS_OBJECT(r)) ? json_node_get_object(r) : NULL;
}

static JsonArray *
_hk_array(JsonObject *o, const gchar *member)
{
    JsonNode *n = json_object_get_member(o, member);
    return (n && JSON_NODE_HOLDS_ARRAY(n)) ? json_node_get_array(n) : NULL;
}

                                          
static void
_hk_print_scan(JsonObject *res)
{
    const gchar *target = json_object_get_string_member_with_default(res, "target_node", "");
    const gchar *state = json_object_get_string_member_with_default(res, "state", "");
    JsonArray *keys = _hk_array(res, "keys");
    guint n = keys ? json_array_get_length(keys) : 0;
    if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
        const char *hdrs[] = { "TARGET", "STATE", "TYPE", "FINGERPRINT" };
        PcvTable *t = ptbl_new(hdrs, 4);
        for (guint i = 0; i < n; i++) {
            JsonObject *k = json_array_get_object_element(keys, i);
            ptbl_row(t, target, state, json_object_get_string_member_with_default(k, "type", ""),
                     json_object_get_string_member_with_default(k, "fingerprint", ""));
        }
        if (g_ctx.fmt == FMT_CSV) ptbl_print_csv(t); else ptbl_print_plain(t);
        ptbl_free(t);
        return;
    }
    printf("%sTARGET%s %s   %sSTATE%s %s\n", cc(CYBER_CYAN), cc(CYBER_RESET), target,
           cc(CYBER_CYAN), cc(CYBER_RESET), state);
    printf("  %-22s %s\n", "TYPE", "FINGERPRINT");
    for (guint i = 0; i < n; i++) {
        JsonObject *k = json_array_get_object_element(keys, i);
        printf("  %-22s %s\n", json_object_get_string_member_with_default(k, "type", ""),
               json_object_get_string_member_with_default(k, "fingerprint", ""));
    }
    JsonNode *tn = json_object_get_member(res, "trusted");
    if (tn && JSON_NODE_HOLDS_OBJECT(tn)) {
        JsonObject *tr = json_node_get_object(tn);
        printf("  trusted: %s %s\n", json_object_get_string_member_with_default(tr, "type", ""),
               json_object_get_string_member_with_default(tr, "fingerprint", ""));
    }
    printf("%s스캔은 인증되지 않은 채널입니다. 원격 콘솔에서 `ssh-keygen -lf /etc/ssh/ssh_host_<형식>_key.pub`로\n"
           "지문을 대조한 뒤 `pcvctl backup hostkey trust %s <SHA256:…>`로 등록하세요.%s\n",
           cc(CYBER_DIM), target, cc(CYBER_RESET));
    if (g_strcmp0(state, "mismatch") == 0)
        printf("%s등록된 키와 다릅니다. 원격 재설치가 확실할 때만 remove 뒤 trust 하세요.%s\n",
               cc(CYBER_RED), cc(CYBER_RESET));
}

static void
_hk_print_list(JsonObject *res)
{
    JsonArray *hosts = _hk_array(res, "hosts");
    guint n = hosts ? json_array_get_length(hosts) : 0;
    const char *hdrs[] = { "TARGET", "TYPE", "FINGERPRINT" };
    PcvTable *t = ptbl_new(hdrs, 3);
    for (guint i = 0; i < n; i++) {
        JsonObject *h = json_array_get_object_element(hosts, i);
        ptbl_row(t, json_object_get_string_member_with_default(h, "target_node", ""),
                 json_object_get_string_member_with_default(h, "type", ""),
                 json_object_get_string_member_with_default(h, "fingerprint", ""));
    }
    if (g_ctx.fmt == FMT_CSV) {
        ptbl_print_csv(t);
    } else if (g_ctx.fmt == FMT_PLAIN) {
        ptbl_print_plain(t);
    } else if (n == 0) {
        printf("(등록된 복제 대상 호스트 키 없음)\n");
    } else {
        printf("  %-24s %-22s %s\n", "TARGET", "TYPE", "FINGERPRINT");
        for (guint i = 0; i < t->rows->len; i++) {
            gchar **row = g_ptr_array_index(t->rows, i);
            printf("  %-24s %-22s %s\n", row[0], row[1], row[2]);
        }
    }
    ptbl_free(t);
}

                                                            
static void
_hk_finish(gchar *resp, GError *e, const gchar *label, void (*print)(JsonObject *res))
{
    if (e) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), e->message, ce(CYBER_RESET));
        g_error_free(e);
        g_free(resp);
        return;
    }
    JsonParser *parser = json_parser_new();
    JsonObject *res = (g_ctx.fmt != FMT_JSON && print) ? _hk_result(parser, resp) : NULL;
    if (res)
        print(res);
    else
        print_action_response(resp, label);
    g_object_unref(parser);
    g_free(resp);
}

                                                                                  
static JsonObject *
_hk_params(const gchar *target, const gchar *fingerprint)
{
    JsonObject *p = json_object_new();
    if (target) json_object_set_string_member(p, "target_node", target);
    if (fingerprint) json_object_set_string_member(p, "fingerprint", fingerprint);
    return p;
}

void
cmd_backup_hostkey(int argc, char *argv[])
{
    const gchar *sub = argc > 2 ? argv[2] : NULL;
    GError *e = NULL;
    if (g_strcmp0(sub, "scan") == 0 && argc == 4) {
        gchar *r = purectl_send_request("backup.replication.hostkey.scan", _hk_params(argv[3], NULL), &e);
        _hk_finish(r, e, "HOSTKEY_SCAN", _hk_print_scan);
    } else if (g_strcmp0(sub, "trust") == 0 && argc == 5) {
        gchar *r = purectl_send_request("backup.replication.hostkey.trust", _hk_params(argv[3], argv[4]), &e);
        _hk_finish(r, e, "HOSTKEY_TRUST", NULL);
    } else if (g_strcmp0(sub, "list") == 0 && argc == 3) {
        gchar *r = purectl_send_request("backup.replication.hostkey.list", NULL, &e);
        _hk_finish(r, e, "HOSTKEY_LIST", _hk_print_list);
    } else if (g_strcmp0(sub, "remove") == 0 && argc == 4) {
        gchar *r = purectl_send_request("backup.replication.hostkey.remove", _hk_params(argv[3], NULL), &e);
        _hk_finish(r, e, "HOSTKEY_REMOVE", NULL);
    } else {
        _hk_usage();
    }
}
