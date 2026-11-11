#include <glib.h>

static JsonObject *_rule(const gchar *direction, const gchar *protocol,
                         const gchar *source, gint port) {
    JsonObject *o = json_object_new();
    if (direction) json_object_set_string_member(o, "direction", direction);
    if (protocol)  json_object_set_string_member(o, "protocol", protocol);
    if (source)    json_object_set_string_member(o, "source", source);
    if (port > 0)  json_object_set_int_member(o, "port", port);
    return o;
}

static void test_sg_rule_add_rolls_back_on_db_write_fail(void) {
    if (geteuid() != 0) { g_test_skip("root(+netns) 필요"); return; }

                                                                 
                                            
    const char *db_path = "/var/lib/purecvisor/security_groups.db";

    g_assert_true(pcv_security_group_create("sgr8", "R8 write-fail"));

                                                  
    sqlite3 *aux = NULL;
    g_assert_cmpint(sqlite3_open(db_path, &aux), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(aux, "DELETE FROM security_groups WHERE name='sgr8';",
                                 NULL, NULL, NULL), ==, SQLITE_OK);
    sqlite3_close(aux);

                                                                          
    JsonObject *rule = _rule("ingress", "tcp", "10.55.44.0/24", 8080);
    gboolean added = pcv_security_group_rule_add("sgr8", rule);
    json_object_unref(rule);
    g_assert_false(added);                                             

                                                      
                                                                      
    gchar *out = NULL;
    const gchar *ls[] = {"nft", "list", "table", "bridge", "pcv_sg", NULL};
    if (pcv_spawn_sync(ls, &out, NULL, NULL)) {
        g_assert_null(strstr(out ? out : "", "10.55.44.0/24"));
        g_free(out);
    }

                                                                      
    g_assert_true(pcv_security_group_delete("sgr8"));
}

void test_security_group_pre_fix_register(void) {
    g_test_add_func("/security_group/rule_add_rolls_back_on_db_write_fail",
                    test_sg_rule_add_rolls_back_on_db_write_fail);
}
