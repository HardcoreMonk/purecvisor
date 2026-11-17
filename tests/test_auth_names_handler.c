                                                                                                                  
                                                                                          
                                                                                                   
                                                                                     
                                                                         
                                                 
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>
#include <string.h>

#include "modules/auth/pcv_rbac.h"
#include "modules/dispatcher/handler_auth.h"
#include "modules/dispatcher/handler_auth_names.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/rpc_utils.h"
#include "utils/pcv_jwt.h"

                            
void pcv_test_audit_reset(void);
gint pcv_test_audit_call_count(void);
const gchar *pcv_test_audit_last_result(void);
gint pcv_test_audit_last_error_code(void);
const gchar *pcv_test_last_uds_response(void);
void pcv_test_uds_responses_reset(void);

#define PW "Principal-Names-Pw-73!"

static gchar *g_dir;
static gchar *g_db;

static void
h_setup(void)
{
    g_dir = g_dir_make_tmp("pcv-auth-names-XXXXXX", NULL);
    g_assert_nonnull(g_dir);
    g_db = g_build_filename(g_dir, "rbac.db", NULL);
    pcv_jwt_init("auth-names-handler-secret-32-bytes!!");
    pcv_rbac_init(g_db);
    pcv_test_audit_reset();
}

static void
h_teardown(void)
{
    pcv_rbac_shutdown();
    pcv_jwt_shutdown();
    gchar *wal = g_strconcat(g_db, "-wal", NULL), *shm = g_strconcat(g_db, "-shm", NULL);
    g_unlink(g_db); g_unlink(wal); g_unlink(shm);
    g_free(wal); g_free(shm);
    g_rmdir(g_dir);
    g_clear_pointer(&g_db, g_free);
    g_clear_pointer(&g_dir, g_free);
}

                               
static void
raw_exec(const gchar *sql)
{
    sqlite3 *db = NULL;
    g_assert_cmpint(sqlite3_open(g_db, &db), ==, SQLITE_OK);
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK)
        g_error("raw_exec failed: %s", err ? err : "?");
    sqlite3_close(db);
}

                           
static gint64
raw_int_count(const gchar *sql)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_open(g_db, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_prepare_v2(db, sql, -1, &st, NULL), ==, SQLITE_OK);
    gint64 v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    sqlite3_close(db);
    return v;
}

                                                                                       
static gint
last_error(gchar **msg_out)
{
    g_autoptr(JsonParser) p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, pcv_test_last_uds_response(), -1, NULL));
    JsonObject *root = json_node_get_object(json_parser_get_root(p));
    if (!json_object_has_member(root, "error")) {
        if (msg_out) *msg_out = NULL;
        return 0;
    }
    JsonObject *e = json_object_get_object_member(root, "error");
    if (msg_out) *msg_out = g_strdup(json_object_get_string_member(e, "message"));
    return (gint)json_object_get_int_member(e, "code");
}

                                                                                              
static void
call_user_create(const gchar *username)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "username", username);
    json_object_set_string_member(p, "password", PW);
    json_object_set_string_member(p, "role", "viewer");
    pcv_rpc_completion_begin("auth.user.create");
    pcv_rpc_completion_set_actor("root-admin");
    pcv_test_audit_reset();
    pcv_test_uds_responses_reset();
    handle_auth_user_create(p, "1", NULL, NULL);
    (void)pcv_rpc_completion_finish();
    json_object_unref(p);
}

                                             
static void
call_key_create(const gchar *name)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "name", name);
    json_object_set_int_member(p, "role", 0);
    pcv_test_uds_responses_reset();
    handle_apikey_create(p, "1", NULL, NULL);
    json_object_unref(p);
}

                                                            
static void
assert_user_create_fails(const gchar *username, gint code, const gchar *msg)
{
    call_user_create(username);
    g_autofree gchar *got = NULL;
    g_assert_cmpint(last_error(&got), ==, code);
    if (msg)
        g_assert_cmpstr(got, ==, msg);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "fail");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, code);
}

                                                                                                        
static void
test_user_create_codes_and_audit(void)
{
    h_setup();
    assert_user_create_fails("grpc", PURE_RPC_ERR_INVALID_PARAMS, "username is reserved");
    gchar *key = NULL;
    GError *err = NULL;
    g_assert_true(pcv_rbac_apikey_create("svc1", PCV_ROLE_VIEWER, NULL, 0, &key, &err));
    g_assert_no_error(err);
    g_free(key);
    assert_user_create_fails("svc1", PURE_RPC_ERR_CONFLICT, "username is already used by an API key");
    raw_exec("INSERT INTO deleted_user_names (username) VALUES ('gone1');");
    assert_user_create_fails("gone1", PURE_RPC_ERR_CONFLICT,
                             "username belongs to a deleted account — set reuse_deleted_name to recreate it with that "
                             "account's ownership records");
    call_user_create("dup1");
    g_assert_cmpint(last_error(NULL), ==, 0);
    assert_user_create_fails("dup1", PURE_RPC_ERR_INTERNAL_ERROR, NULL);
    h_teardown();
}

                                                                                   
static void
test_apikey_create_codes(void)
{
    h_setup();
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("alice2", PW, PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
    g_autofree gchar *m1 = NULL, *m2 = NULL, *m3 = NULL, *m4 = NULL, *m5 = NULL;
    call_key_create("alice2");
    g_assert_cmpint(last_error(&m1), ==, PURE_RPC_ERR_CONFLICT);
    g_assert_cmpstr(m1, ==, "name is already used by a user account");
    call_key_create("grpc");
    g_assert_cmpint(last_error(&m2), ==, PURE_RPC_ERR_INVALID_PARAMS);
    g_assert_cmpstr(m2, ==, "name is reserved");
    raw_exec("INSERT INTO deleted_user_names (username) VALUES ('gone2');");
    call_key_create("gone2");
    g_assert_cmpint(last_error(&m3), ==, PURE_RPC_ERR_CONFLICT);
    g_assert_cmpstr(m3, ==, "name belongs to a deleted account");
    call_key_create("svc2");
    g_assert_cmpint(last_error(&m4), ==, 0);
                                                
    raw_exec("CREATE TRIGGER pcv_test_block_keys BEFORE INSERT ON api_keys BEGIN SELECT RAISE(ABORT, 'blocked'); END;");
    call_key_create("svc3");
    g_assert_cmpint(last_error(&m5), ==, PURE_RPC_ERR_ZFS_OPERATION);
    g_assert_cmpstr(m5, ==, "Failed to create API key");
    h_teardown();
}

                                                                 
static gint
result_reused(void)
{
    g_autoptr(JsonParser) p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, pcv_test_last_uds_response(), -1, NULL));
    JsonObject *res = json_object_get_object_member(json_node_get_object(json_parser_get_root(p)), "result");
    if (!json_object_has_member(res, "reused_deleted_name"))
        return -1;
    return json_object_get_boolean_member(res, "reused_deleted_name") ? 1 : 0;
}

                                                                                              
static void
call_user_create_reuse(const gchar *username, JsonNode *node)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "username", username);
    json_object_set_string_member(p, "password", PW);
    json_object_set_string_member(p, "role", "viewer");
    if (node)
        json_object_set_member(p, "reuse_deleted_name", node);
    pcv_rpc_completion_begin("auth.user.create");
    pcv_rpc_completion_set_actor("root-admin");
    pcv_test_audit_reset();
    pcv_test_uds_responses_reset();
    handle_auth_user_create(p, "1", NULL, NULL);
    (void)pcv_rpc_completion_finish();
    json_object_unref(p);
}

                                                                                                  
                                           
static void
test_reuse_param(void)
{
    h_setup();
    g_autofree gchar *m1 = NULL, *m2 = NULL, *m3 = NULL;
    JsonNode *s = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(s, "true");
    call_user_create_reuse("mia", s);
    g_assert_cmpint(last_error(&m1), ==, PURE_RPC_ERR_INVALID_PARAMS);
    g_assert_cmpstr(m1, ==, "reuse_deleted_name must be a boolean");
    JsonNode *i = json_node_new(JSON_NODE_VALUE);
    json_node_set_int(i, 1);
    call_user_create_reuse("mia", i);
    g_assert_cmpint(last_error(&m2), ==, PURE_RPC_ERR_INVALID_PARAMS);
    call_user_create_reuse("mia", json_node_new(JSON_NODE_NULL));
    g_assert_cmpint(last_error(&m3), ==, PURE_RPC_ERR_INVALID_PARAMS);
    g_assert_cmpint(raw_int_count("SELECT COUNT(*) FROM users"), ==, 0);

    raw_exec("INSERT INTO deleted_user_names (username) VALUES ('mia');");
    JsonNode *t = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(t, TRUE);
    call_user_create_reuse("mia", t);
    g_assert_cmpint(last_error(NULL), ==, 0);
    g_assert_cmpint(result_reused(), ==, 1);

    JsonNode *t2 = json_node_new(JSON_NODE_VALUE);
    json_node_set_boolean(t2, TRUE);
    call_user_create_reuse("nora", t2);                          
    g_assert_cmpint(last_error(NULL), ==, 0);
    g_assert_cmpint(result_reused(), ==, 0);

    call_user_create_reuse("olga", NULL);                    
    g_assert_cmpint(last_error(NULL), ==, 0);
    g_assert_cmpint(result_reused(), ==, -1);
    g_assert_null(strstr(pcv_test_last_uds_response(), "reused_deleted_name"));
    h_teardown();
}

                                                                                             
static void
call_deleted_name_add(const gchar *json)
{
    g_autoptr(JsonParser) p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, json, -1, NULL));
    JsonObject *params = json_object_ref(json_node_get_object(json_parser_get_root(p)));
    pcv_test_audit_reset();
    pcv_test_uds_responses_reset();
    handle_auth_deleted_name_add(params, "1", NULL, NULL);
    json_object_unref(params);
}

                                                                                               
                                                                               
static void
test_deleted_name_add_codes(void)
{
    h_setup();
    static const struct { const gchar *json; gint code; const gchar *msg; } cases[] = {
        { "{}", PURE_RPC_ERR_INVALID_PARAMS, "Missing required param: username" },
        { "{\"username\":\"\"}", PURE_RPC_ERR_INVALID_PARAMS, "username must be a non-empty string" },
        { "{\"username\":7}", PURE_RPC_ERR_INVALID_PARAMS, "username must be a non-empty string" },
        { "{\"username\":\"a b\"}", PURE_RPC_ERR_INVALID_PARAMS, "username must match [A-Za-z0-9][A-Za-z0-9._-]{0,62}" },
        { "{\"username\":\"-x\"}", PURE_RPC_ERR_INVALID_PARAMS, "username must match [A-Za-z0-9][A-Za-z0-9._-]{0,62}" },
        { "{\"username\":\"system\"}", PURE_RPC_ERR_INVALID_PARAMS, "name is reserved" },
        { "{\"username\":\"anna\"}", PURE_RPC_ERR_CONFLICT,
          "username belongs to an existing account — delete the account instead" },
        { "{\"username\":\"svc5\"}", PURE_RPC_ERR_CONFLICT, "name is already used by an API key" },
    };
    GError *err = NULL;
    g_assert_true(pcv_rbac_user_create("anna", PW, PCV_ROLE_VIEWER, NULL, &err));
    gchar *key = NULL;
    g_assert_true(pcv_rbac_apikey_create("svc5", PCV_ROLE_VIEWER, NULL, 0, &key, &err));
    g_assert_no_error(err);
    g_free(key);
    for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
        call_deleted_name_add(cases[i].json);
        g_autofree gchar *m = NULL;
        g_assert_cmpint(last_error(&m), ==, cases[i].code);
        g_assert_cmpstr(m, ==, cases[i].msg);
    }
    g_assert_cmpint(raw_int_count("SELECT COUNT(*) FROM deleted_user_names"), ==, 0);

    for (gint round = 0; round < 2; round++) {
        call_deleted_name_add("{\"username\":\"zz1\"}");
        g_assert_cmpint(last_error(NULL), ==, 0);
        g_autoptr(JsonParser) p = json_parser_new();
        g_assert_true(json_parser_load_from_data(p, pcv_test_last_uds_response(), -1, NULL));
        JsonObject *res = json_object_get_object_member(json_node_get_object(json_parser_get_root(p)), "result");
        g_assert_cmpstr(json_object_get_string_member(res, "username"), ==, "zz1");
        g_assert_true(json_object_get_boolean_member(res, "added") == (round == 0));
        g_assert_cmpint(pcv_test_audit_call_count(), ==, 0);
    }
    g_assert_cmpint(raw_int_count("SELECT COUNT(*) FROM deleted_user_names"), ==, 1);
    h_teardown();
}

                               

#define MSG_TENANT_GRAMMAR "tenant must be a string matching [A-Za-z0-9][A-Za-z0-9._-]{0,62}"
#define MSG_KEYNAME_GRAMMAR "name must match [A-Za-z0-9][A-Za-z0-9._-]{0,62}"

                                                                           
static void
call_user_create_tenant(const gchar *username, JsonNode *node)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "username", username);
    json_object_set_string_member(p, "password", PW);
    json_object_set_string_member(p, "role", "viewer");
    if (node)
        json_object_set_member(p, "tenant", node);
    pcv_rpc_completion_begin("auth.user.create");
    pcv_rpc_completion_set_actor("root-admin");
    pcv_test_audit_reset();
    pcv_test_uds_responses_reset();
    handle_auth_user_create(p, "1", NULL, NULL);
    (void)pcv_rpc_completion_finish();
    json_object_unref(p);
}

                                                 
static gchar *
result_tenant(void)
{
    g_autoptr(JsonParser) p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, pcv_test_last_uds_response(), -1, NULL));
    JsonObject *res = json_object_get_object_member(json_node_get_object(json_parser_get_root(p)), "result");
    g_assert_true(json_object_has_member(res, "tenant"));
    JsonNode *n = json_object_get_member(res, "tenant");
    return JSON_NODE_HOLDS_NULL(n) ? g_strdup("<null>") : g_strdup(json_node_get_string(n));
}

                                                                                  
static JsonNode *
obj_node(void)
{
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, json_object_new());
    return n;
}

static JsonNode *
arr_node(void)
{
    JsonNode *n = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(n, json_array_new());
    return n;
}

static JsonNode *
str_node(const gchar *v)
{
    JsonNode *n = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(n, v);
    return n;
}

                                                                                                
static void
test_tenant_param_types(void)
{
    h_setup();
    g_autofree gchar *t1 = NULL, *t2 = NULL, *t3 = NULL;
    call_user_create_tenant("tn-absent", NULL);
    g_assert_cmpint(last_error(NULL), ==, 0);
    t1 = result_tenant();
    g_assert_cmpstr(t1, ==, "<null>");
    call_user_create_tenant("tn-null", json_node_new(JSON_NODE_NULL));
    g_assert_cmpint(last_error(NULL), ==, 0);
    t2 = result_tenant();
    g_assert_cmpstr(t2, ==, "<null>");
    call_user_create_tenant("tn-ok", str_node("ta"));
    g_assert_cmpint(last_error(NULL), ==, 0);
    t3 = result_tenant();
    g_assert_cmpstr(t3, ==, "ta");

    g_autofree gchar *long64 = g_strnfill(64, 'a');
    JsonNode *bad[] = {
        str_node(""), str_node("a b"), str_node("-x"), str_node(long64),
        json_node_init_int(json_node_alloc(), 1), json_node_init_boolean(json_node_alloc(), TRUE),
        obj_node(), arr_node(),
    };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_autofree gchar *name = g_strdup_printf("tn-bad%u", i);
        call_user_create_tenant(name, bad[i]);
        g_autofree gchar *m = NULL;
        g_assert_cmpint(last_error(&m), ==, PURE_RPC_ERR_INVALID_PARAMS);
        g_assert_cmpstr(m, ==, MSG_TENANT_GRAMMAR);
    }
    g_assert_cmpint(raw_int_count("SELECT COUNT(*) FROM users WHERE username LIKE 'tn-bad%'"), ==, 0);
    h_teardown();
}

                                                          
static void
call_key_create_member(const gchar *member, const gchar *name)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, member, name);
    json_object_set_int_member(p, "role", 0);
    pcv_test_uds_responses_reset();
    handle_apikey_create(p, "1", NULL, NULL);
    json_object_unref(p);
}

                                                                                      
static void
test_apikey_name_grammar(void)
{
    h_setup();
    g_autofree gchar *long64 = g_strnfill(64, 'k');
    const gchar *bad[] = { "ci bot", "-", ".x", long64 };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        call_key_create_member("name", bad[i]);
        g_autofree gchar *m = NULL;
        g_assert_cmpint(last_error(&m), ==, PURE_RPC_ERR_INVALID_PARAMS);
        g_assert_cmpstr(m, ==, MSG_KEYNAME_GRAMMAR);
    }
    call_key_create_member("client_name", "ci bot");
    g_autofree gchar *ma = NULL;
    g_assert_cmpint(last_error(&ma), ==, PURE_RPC_ERR_INVALID_PARAMS);
    g_assert_cmpstr(ma, ==, MSG_KEYNAME_GRAMMAR);
    g_assert_cmpint(raw_int_count("SELECT COUNT(*) FROM api_keys"), ==, 0);
    call_key_create_member("name", "ci-bot");
    g_assert_cmpint(last_error(NULL), ==, 0);
    g_assert_cmpint(raw_int_count("SELECT COUNT(*) FROM api_keys"), ==, 1);
    h_teardown();
}

void
test_auth_names_handler_register(void)
{
    g_test_add_func("/auth_names_handler/user_create_codes_and_audit", test_user_create_codes_and_audit);
    g_test_add_func("/auth_names_handler/apikey_create_codes", test_apikey_create_codes);
    g_test_add_func("/auth_names_handler/reuse_param", test_reuse_param);
    g_test_add_func("/auth_names_handler/deleted_name_add_codes", test_deleted_name_add_codes);
    g_test_add_func("/auth_names_handler/tenant_param_types", test_tenant_param_types);
    g_test_add_func("/auth_names_handler/apikey_name_grammar", test_apikey_name_grammar);
}
