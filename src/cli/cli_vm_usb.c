   
                     
                                                                                                 
  
                           
                                                                                                                     
                                                                                                
                                                                                      
                                                                                                
                                                                           
                                                                                                  
                                                                                          
                                                                                                                
                                                                                                  
                                                                                               
                                                                                       
  
                                                                              
                                                                          
                       
   
#include "cli_vm_usb.h"
#include "cli_rpc.h"
#include "cli_output.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

                                                                                       
static void
_attach_usage(void)
{
    printf("%sUsage: pcvctl vm usb-attach <name> <vendor_id> <product_id> [--startup-policy mandatory|requisite|optional]"
           " [--live-only]%s\n", cc(CYBER_YELLOW), cc(CYBER_RESET));
    printf("Example: pcvctl vm usb-attach web-prod 0x1234 0x5678 --startup-policy optional\n");
}

                                                                                   
                                                                                              
                                                       
                                                                                       
static void
_change_summary(const gchar *resp)
{
    if (g_ctx.fmt != FMT_TABLE || !resp)
        return;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL))
        return;
    JsonNode *root = json_parser_get_root(parser);
    JsonNode *rn = root && JSON_NODE_HOLDS_OBJECT(root)
        ? json_object_get_member(json_node_get_object(root), "result") : NULL;
    if (!rn || !JSON_NODE_HOLDS_OBJECT(rn))
        return;
    JsonObject *r = json_node_get_object(rn);
    JsonNode *pol = json_object_get_member(r, "startup_policy");
    const gchar *policy = pol && JSON_NODE_HOLDS_VALUE(pol) && json_node_get_value_type(pol) == G_TYPE_STRING
        ? json_node_get_string(pol) : "-";
    printf("changed: live=%s, persistent=%s · startup_policy: %s\n",
           json_object_get_boolean_member_with_default(r, "live", FALSE) ? "yes" : "no",
           json_object_get_boolean_member_with_default(r, "config", FALSE) ? "yes" : "no", policy);
}

                                                                                          
static void
_change_print(gchar *resp, GError *error, const gchar *action)
{
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error);
        return;
    }
    print_action_response(resp, action);
    _change_summary(resp);
    g_free(resp);
}

void
cmd_vm_usb_attach(int argc, char *argv[])
{
    const char *policy = NULL;
    gboolean live_only = FALSE, bad = argc < 5;
    for (int i = 2; !bad && i < 5; i++)                                  
        bad = g_str_has_prefix(argv[i], "--");
    for (int i = 5; !bad && i < argc; i++) {
        if (g_strcmp0(argv[i], "--startup-policy") == 0 && i + 1 < argc)
            policy = argv[++i];
        else if (g_strcmp0(argv[i], "--live-only") == 0)                                     
            live_only = TRUE;
        else
            bad = TRUE;                                                      
    }
    if (bad) {
        _attach_usage();
        return;
    }
    JsonObject *params = json_object_new();                                         
    json_object_set_string_member(params, "vm_id", argv[2]);
    json_object_set_string_member(params, "vendor_id", argv[3]);
    json_object_set_string_member(params, "product_id", argv[4]);
    if (policy)
        json_object_set_string_member(params, "startup_policy", policy);
    if (live_only)                                                                  
        json_object_set_boolean_member(params, "persistent", FALSE);
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.usb.attach", params, &error);
    _change_print(resp, error, "USB_ATTACH");
}

void
cmd_vm_usb_detach(int argc, char *argv[])
{
    if (argc < 5) {
        printf("%sUsage: pcvctl vm usb-detach <name> <vendor_id> <product_id>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        printf("Example: pcvctl vm usb-detach web-prod 0x1234 0x5678\n");
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", argv[2]);
    json_object_set_string_member(params, "vendor_id", argv[3]);
    json_object_set_string_member(params, "product_id", argv[4]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.usb.detach", params, &error);
    _change_print(resp, error, "USB_DETACH");
}

                                                       
static gboolean
_flag(JsonObject *usb, const gchar *key)
{
    return json_object_get_boolean_member_with_default(usb, key, FALSE);
}

void
cmd_vm_usb_list(int argc, char *argv[])
{
    if (argc < 3) {
        printf("%sUsage: pcvctl vm usb-list <name>%s\n",
            cc(CYBER_YELLOW), cc(CYBER_RESET));
        return;
    }
    JsonObject *params = json_object_new();
    json_object_set_string_member(params, "vm_id", argv[2]);
    GError *error = NULL;
    gchar *resp = purectl_send_request("vm.usb.list", params, &error);
    if (error) {
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }

    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "error")) {
        JsonObject *err = json_object_get_object_member(root, "error");
        g_printerr("%s[!] %s%s\n", ce(CYBER_RED),
            json_object_get_string_member_with_default(err, "message", "USB list failed"),
            ce(CYBER_RESET));
    } else if (json_object_has_member(root, "result") &&
               JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "result"))) {
        JsonArray *arr = json_node_get_array(json_object_get_member(root, "result"));
        gboolean table = g_ctx.fmt != FMT_PLAIN && g_ctx.fmt != FMT_CSV;
        if (table) {
            print_cyber_banner();
            printf("%s [ VM USB DEVICES ]\n\n%s", cc(CYBER_CYAN), cc(CYBER_RESET));
            printf("%s%-12s %-12s %-8s %-11s %s%s\n", cc(CYBER_CYAN),
                   "VENDOR_ID", "PRODUCT_ID", "LIVE", "PERSISTENT", "POLICY", cc(CYBER_RESET));
            printf("──────────────────────────────────────────────────────\n");
        }
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonObject *usb = json_array_get_object_element(arr, i);
            const gchar *v = json_object_get_string_member_with_default(usb, "vendor_id", "-");
            const gchar *p = json_object_get_string_member_with_default(usb, "product_id", "-");
            const gchar *pol = json_object_get_string_member_with_default(usb, "startup_policy", "-");
            if (!table) {
                printf("%s,%s,%s,%s,%s,%s\n", v, p, _flag(usb, "live") ? "true" : "false",
                       _flag(usb, "persistent") ? "true" : "false", pol, _flag(usb, "missing") ? "true" : "false");
                continue;
            }
            const gchar *live = !_flag(usb, "live") ? "no" : _flag(usb, "missing") ? "missing" : "yes";
            printf(" %-12s %-12s %-8s %-11s %s\n", v, p, live, _flag(usb, "persistent") ? "yes" : "no", pol);
        }
        if (table && json_array_get_length(arr) == 0)
            printf(" (no USB hostdev attached)\n");
        if (table)
            printf("──────────────────────────────────────────────────────\n");
    } else {
        print_action_response(resp, "USB_LIST");
    }
    g_object_unref(parser);
    g_free(resp);
}
