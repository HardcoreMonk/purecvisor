
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <json-glib/json-glib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    char *s = (char *)malloc(size + 1);
    if (!s) return 0;
    memcpy(s, data, size);
    s[size] = '\0';

    JsonParser *parser = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(parser, s, -1, &err)) {
        if (err) g_error_free(err);
        g_object_unref(parser);
        free(s);
        return 0;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
        JsonObject *obj = json_node_get_object(root);

        if (json_object_has_member(obj, "method")) {
            const gchar *method = json_object_get_string_member(obj, "method");
            (void)method;
        }

        if (json_object_has_member(obj, "id")) {
            JsonNode *id_node = json_object_get_member(obj, "id");
            if (id_node && json_node_get_value_type(id_node) == G_TYPE_STRING) {
                const gchar *sid = json_node_get_string(id_node);
                (void)sid;
            } else if (id_node) {
                gint64 nid = json_node_get_int(id_node);
                (void)nid;
            }
        }

        if (json_object_has_member(obj, "params")) {
            JsonNode *p = json_object_get_member(obj, "params");
            if (p && JSON_NODE_HOLDS_OBJECT(p)) {
                JsonObject *po = json_node_get_object(p);
                (void)po;
            }
        }
    }

    g_object_unref(parser);
    free(s);
    return 0;
}
