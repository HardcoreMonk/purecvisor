   
                  
                                                                            
  
                                                                                       
                                          
   
#include "cli_iso.h"
#include "cli_rpc.h"
#include "cli_output.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdio.h>

   
                                                            
                                                                           
                                                      
   
static const gchar *iso_list_element(JsonArray *arr, guint i, gint64 *size_mb) {
    JsonNode *n = json_array_get_element(arr, i);
    *size_mb = -1;
    if (!n) return NULL;
    if (JSON_NODE_HOLDS_OBJECT(n)) {
        JsonObject *o = json_node_get_object(n);
        JsonNode *nm = json_object_get_member(o, "name");
        if (!nm || !JSON_NODE_HOLDS_VALUE(nm) || json_node_get_value_type(nm) != G_TYPE_STRING) return NULL;
        JsonNode *sz = json_object_get_member(o, "size_mb");
        if (sz && JSON_NODE_HOLDS_VALUE(sz)) *size_mb = json_node_get_int(sz);
        return json_node_get_string(nm);
    }
    if (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
        return json_node_get_string(n);
    return NULL;
}

   
                                     
                                                         
                         
                   
   
void cmd_iso_list(int argc, char *argv[]) {
    (void)argc; (void)argv;
    GError *error = NULL;
    gchar  *resp  = purectl_send_request("iso.list", NULL, &error);
    if (error) {
        g_printerr("%s[!] LINK_SEVERED: %s%s\n",
            ce(CYBER_RED), error->message, ce(CYBER_RESET));
        g_error_free(error); return;
    }
    if (g_ctx.fmt == FMT_JSON) { print_raw_response(resp); g_free(resp); return; }

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, resp, -1, NULL) || !json_parser_get_root(parser)) {
        g_object_unref(parser); g_free(resp); return;
    }
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(root, "result")) {
        JsonNode *res_node = json_object_get_member(root, "result");
        if (res_node && JSON_NODE_HOLDS_ARRAY(res_node)) {
            JsonArray *arr = json_node_get_array(res_node);
            if (g_ctx.fmt == FMT_PLAIN || g_ctx.fmt == FMT_CSV) {
                for (guint i = 0; i < json_array_get_length(arr); i++) {
                    gint64 size_mb;
                    const gchar *name = iso_list_element(arr, i, &size_mb);
                    if (name) printf("%s\n", name);
                }
            } else {
                print_cyber_banner();
                printf("%s [ ISO FILES ]\n\n%s", cc(CYBER_CYAN), cc(CYBER_RESET));
                printf("%s%-48s %10s%s\n", cc(CYBER_CYAN), "FILENAME", "SIZE(MB)", cc(CYBER_RESET));
                printf("────────────────────────────────────────────────────────────\n");
                for (guint i = 0; i < json_array_get_length(arr); i++) {
                    gint64 size_mb;
                    const gchar *name = iso_list_element(arr, i, &size_mb);
                    if (!name) continue;
                    if (size_mb >= 0) printf(" %-48s %10" G_GINT64_FORMAT "\n", name, size_mb);
                    else              printf(" %-48s %10s\n", name, "-");
                }
                printf("────────────────────────────────────────────────────────────\n");
            }
        } else {
            print_action_response(resp, "ISO_LIST");
        }
    } else {
        print_action_response(resp, "ISO_LIST");
    }
    g_object_unref(parser);
    g_free(resp);
}

