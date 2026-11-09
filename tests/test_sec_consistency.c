                                                                                                             
                                                                                                      
                                                               
                                                                       
                                              
   
                               
                                                                                                        
  
                                        
                                                
   
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>
#include <string.h>

#include "modules/auth/pcv_rbac.h"
#include "api/rest_server.h"
#include "modules/dispatcher/dispatcher_shared.h"
#include "modules/dispatcher/handler_auth.h"
#include "modules/dispatcher/rpc_completion.h"
#include "utils/pcv_config.h"
#include "utils/pcv_jwt.h"
#include "utils/pcv_ssrf.h"

                            
void pcv_test_audit_reset(void);
gint pcv_test_audit_call_count(void);
const gchar *pcv_test_audit_username_at(gint row);
const gchar *pcv_test_audit_method_at(gint row);
const gchar *pcv_test_audit_target_at(gint row);
const gchar *pcv_test_audit_result_at(gint row);
const gchar *pcv_test_audit_source_at(gint row);
const gchar *pcv_test_last_uds_response(void);

static gchar *g_dir;
static gchar *g_db;

static void
sc_setup(void)
{
    g_dir = g_dir_make_tmp("pcv-sec-consistency-XXXXXX", NULL);
    g_assert_nonnull(g_dir);
    g_db = g_build_filename(g_dir, "rbac.db", NULL);
    pcv_jwt_init("sec-consistency-test-secret-32-bytes!");
    pcv_rbac_init(g_db);
    pcv_test_audit_reset();
}

static void
sc_teardown(void)
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
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) g_error("raw_exec failed: %s", err ? err : "?");
    sqlite3_close(db);
}

                                                             
static gchar *
raw_text(const gchar *sql)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    g_assert_cmpint(sqlite3_open(g_db, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_prepare_v2(db, sql, -1, &st, NULL), ==, SQLITE_OK);
    gchar *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        out = g_strdup((const gchar *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
}

                                                                      

                                      
static gchar *
issue_key(const gchar *client)
{
    gchar *key = NULL;
    GError *err = NULL;
    g_assert_true(pcv_rbac_apikey_create(client, PCV_ROLE_OPERATOR, NULL, 0, &key, &err));
    g_assert_no_error(err);
    return key;
}

                             
static void
verify_ok(const gchar *key)
{
    PcvRole role = PCV_ROLE_VIEWER;
    GError *err = NULL;
    g_autofree gchar *client = pcv_rbac_verify_api_key(key, &role, &err);
    g_assert_no_error(err);
    g_assert_nonnull(client);
    g_assert_cmpint(role, ==, PCV_ROLE_OPERATOR);
}

                                                                                                   
static void
test_d1_verify_writes_last_used(void)
{
    sc_setup();
    g_autofree gchar *key = issue_key("ci-bot");
    g_autofree gchar *before = raw_text("SELECT last_used_at FROM api_keys WHERE client_name='ci-bot'");
    g_assert_null(before);
    verify_ok(key);
    g_autofree gchar *after = raw_text("SELECT last_used_at FROM api_keys WHERE client_name='ci-bot'");
    g_assert_nonnull(after);
    sc_teardown();
}

                                                      
static void
test_d1_last_used_throttled_60s(void)
{
    sc_setup();
    g_autofree gchar *key = issue_key("ci-bot");
    verify_ok(key);

                           
    raw_exec("UPDATE api_keys SET last_used_at=datetime('now','-30 seconds') WHERE client_name='ci-bot'");
    g_autofree gchar *in_window = raw_text("SELECT last_used_at FROM api_keys WHERE client_name='ci-bot'");
    verify_ok(key);
    g_autofree gchar *still = raw_text("SELECT last_used_at FROM api_keys WHERE client_name='ci-bot'");
    g_assert_cmpstr(still, ==, in_window);

                            
    raw_exec("UPDATE api_keys SET last_used_at=datetime('now','-61 seconds') WHERE client_name='ci-bot'");
    g_autofree gchar *stale = raw_text("SELECT last_used_at FROM api_keys WHERE client_name='ci-bot'");
    verify_ok(key);
    g_autofree gchar *fresh = raw_text("SELECT last_used_at FROM api_keys WHERE client_name='ci-bot'");
    g_assert_cmpstr(fresh, !=, stale);
    g_autofree gchar *recent = raw_text(
        "SELECT 1 FROM api_keys WHERE client_name='ci-bot' AND last_used_at >= datetime('now','-5 seconds')");
    g_assert_nonnull(recent);

                                                 
    raw_exec("UPDATE api_keys SET last_used_at=datetime('now','+1 day') WHERE client_name='ci-bot'");
    verify_ok(key);
    g_autofree gchar *not_future = raw_text(
        "SELECT 1 FROM api_keys WHERE client_name='ci-bot' AND last_used_at <= datetime('now')");
    g_assert_nonnull(not_future);
    sc_teardown();
}

                                                    
static void
test_d1_failed_auth_writes_nothing(void)
{
    sc_setup();
    g_autofree gchar *key = issue_key("ci-bot");

    g_autofree gchar *bogus = g_strdup(key);
    bogus[10] = (bogus[10] == 'a') ? 'b' : 'a';
    GError *err = NULL;
    g_assert_null(pcv_rbac_verify_api_key(bogus, NULL, &err));
    g_clear_error(&err);
    g_autofree gchar *after_bogus = raw_text("SELECT last_used_at FROM api_keys WHERE client_name='ci-bot'");
    g_assert_null(after_bogus);

    g_assert_true(pcv_rbac_apikey_revoke("ci-bot", &err));
    g_assert_no_error(err);
    g_assert_null(pcv_rbac_verify_api_key(key, NULL, &err));
    g_clear_error(&err);
    g_autofree gchar *after_revoked = raw_text("SELECT last_used_at FROM api_keys WHERE client_name='ci-bot'");
    g_assert_null(after_revoked);
    sc_teardown();
}

                                                              

                                                        
static void
scope_begin(const gchar *method, const gchar *actor)
{
    pcv_rpc_completion_begin(method);
    pcv_rpc_completion_set_actor(actor);
    pcv_test_audit_reset();
}

static void
scope_end(void)
{
    (void)pcv_rpc_completion_finish();
}

                                                 
static void
assert_one_row(const gchar *actor, const gchar *method, const gchar *target, const gchar *result)
{
    if (pcv_test_audit_call_count() != 1)
        g_error("감사 행 %d개(기대 1): 0행 %s/%s", pcv_test_audit_call_count(),
                pcv_test_audit_method_at(0) ? pcv_test_audit_method_at(0) : "-",
                pcv_test_audit_method_at(1) ? pcv_test_audit_method_at(1) : "-");
    g_assert_cmpstr(pcv_test_audit_username_at(0), ==, actor);
    g_assert_cmpstr(pcv_test_audit_method_at(0), ==, method);
    g_assert_cmpstr(pcv_test_audit_target_at(0), ==, target);
    g_assert_cmpstr(pcv_test_audit_result_at(0), ==, result);
}

static void
call_create(const gchar *actor, const gchar *username)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "username", username);
    json_object_set_string_member(p, "password", "Sec-Consistency-Pw-73!");
    json_object_set_string_member(p, "role", "viewer");
    scope_begin("auth.user.create", actor);
    handle_auth_user_create(p, "1", NULL, NULL);
    json_object_unref(p);
}

static void
call_delete(const gchar *actor, const gchar *username)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "username", username);
    scope_begin("auth.user.delete", actor);
    handle_auth_user_delete(p, "1", NULL, NULL);
    json_object_unref(p);
}

                                                                                 
static void
call_role_set(const gchar *actor, const gchar *caller_sub, const gchar *username, const gchar *role)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "username", username);
    json_object_set_string_member(p, "role", role);
    if (caller_sub)
        json_object_set_string_member(p, "_pcv_caller_sub", caller_sub);
    scope_begin("auth.role.set", actor);
    handle_auth_role_set(p, "1", NULL, NULL);
    json_object_unref(p);
}

                                               
static void
test_d2_user_create_one_row(void)
{
    sc_setup();
    call_create("alice", "bob");
    assert_one_row("alice", "auth.user.create", "bob", "ok");
    scope_end();

    call_create("alice", "bob");
    assert_one_row("alice", "auth.user.create", "bob", "fail");
    scope_end();
    sc_teardown();
}

                               
static void
test_d2_user_delete_one_row(void)
{
    sc_setup();
    call_create("alice", "bob");
    scope_end();

    call_delete("alice", "bob");
    assert_one_row("alice", "auth.user.delete", "bob", "ok");
    scope_end();

    call_delete("alice", "bob");
    assert_one_row("alice", "auth.user.delete", "bob", "fail");
    scope_end();
    sc_teardown();
}

                                                              
static void
test_d2_role_set_one_row_keeps_before_after(void)
{
    sc_setup();
    call_create("alice", "bob");
    scope_end();

    call_role_set("alice", "alice", "bob", "operator");
    assert_one_row("alice", "auth.role.set", "bob", "ok");
    g_assert_cmpstr(pcv_test_audit_source_at(0), ==, "viewer->operator");
    scope_end();

    call_role_set("alice", "alice", "bob", "admin");
    assert_one_row("alice", "auth.role.set", "bob", "ok");
    g_assert_cmpstr(pcv_test_audit_source_at(0), ==, "operator->admin");
    scope_end();

    call_role_set("alice", "alice", "nobody", "admin");
    assert_one_row("alice", "auth.role.set", "nobody", "fail");
    scope_end();
    sc_teardown();
}

                                                                    
                                                                
static void
test_d2_role_set_self_denied_one_row(void)
{
    sc_setup();
    call_create("alice", "bob");
    scope_end();

    call_role_set("bob", "bob", "bob", "admin");
    assert_one_row("bob", "auth.role.set", "bob", "denied");
    scope_end();
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "Self role change"));
    g_autofree gchar *role = raw_text("SELECT role FROM users WHERE username='bob'");
    g_assert_cmpstr(role, ==, "0");                   

                                                 
    call_role_set("local-admin", NULL, "bob", "operator");
    assert_one_row("local-admin", "auth.role.set", "bob", "ok");
    scope_end();
    sc_teardown();
}

                                                                         
                                
static void
test_d2_rbac_layer_writes_no_audit_row(void)
{
    sc_setup();
    GError *err = NULL;
    pcv_test_audit_reset();
    g_assert_true(pcv_rbac_user_create("carol", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_assert_no_error(err);
    g_assert_false(pcv_rbac_user_create("carol", "pw", PCV_ROLE_VIEWER, NULL, &err));
    g_clear_error(&err);
    g_assert_true(pcv_rbac_user_set_role("carol", PCV_ROLE_OPERATOR, NULL, &err));
    g_assert_no_error(err);
    g_assert_true(pcv_rbac_user_delete("carol", &err));
    g_assert_no_error(err);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 0);
    sc_teardown();
}

                                                                       
static void
test_d2_bootstrap_admin_audit_kept(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("pcv-sec-consistency-boot-XXXXXX", NULL);
    g_autofree gchar *conf = g_build_filename(dir, "daemon.conf", NULL);
    g_assert_true(g_file_set_contents(conf,
        "[daemon]\nadmin_user=bootadmin\nadmin_password=Sec-Consistency-Boot-73!\n", -1, NULL));
    g_autofree gchar *saved = g_strdup(g_getenv("PCV_CONFIG_PATH"));
    g_setenv("PCV_CONFIG_PATH", conf, TRUE);
    pcv_config_init();

    g_dir = g_strdup(dir);
    g_db = g_build_filename(g_dir, "rbac.db", NULL);
    pcv_jwt_init("sec-consistency-test-secret-32-bytes!");
    pcv_test_audit_reset();
    pcv_rbac_init(g_db);                               
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_method_at(0), ==, "auth.user.create");
    g_assert_cmpstr(pcv_test_audit_target_at(0), ==, "bootadmin");
    g_assert_cmpstr(pcv_test_audit_result_at(0), ==, "ok");
    g_assert_cmpstr(pcv_test_audit_source_at(0), ==, "bootstrap");

                                          
    pcv_rbac_shutdown();
    pcv_test_audit_reset();
    pcv_rbac_init(g_db);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 0);

    pcv_config_shutdown();
                                                                                       
    g_setenv("PCV_CONFIG_PATH", saved ? saved : "/nonexistent/pcv-test-isolated.conf", TRUE);
    pcv_config_init();
    g_unlink(conf);
    sc_teardown();
}

                                                   
static void
test_d2_rest_gate_denial_method_set(void)
{
    g_assert_true(pcv_rest_method_is_user_admin("auth.user.create"));
    g_assert_true(pcv_rest_method_is_user_admin("auth.user.delete"));
    g_assert_true(pcv_rest_method_is_user_admin("auth.role.set"));
    g_assert_false(pcv_rest_method_is_user_admin("auth.user.list"));
    g_assert_false(pcv_rest_method_is_user_admin("vm.list"));
    g_assert_false(pcv_rest_method_is_user_admin(NULL));
}

                                                              

                                                                                   
                                                                
static void
test_d3_invalid_refresh_actor_is_dash(void)
{
    sc_setup();
    pcv_test_audit_reset();
    GError *err = NULL;
    g_autofree gchar *access = pcv_rbac_refresh_token("not-a-real-refresh-token", NULL, &err);
    g_assert_null(access);
    g_clear_error(&err);
    assert_one_row("-", "auth.token.refresh", "unknown", "invalid token");

                                                    
    scope_begin("auth.token.refresh", "someone");
    g_autofree gchar *again = pcv_rbac_refresh_token("not-a-real-refresh-token", NULL, &err);
    g_assert_null(again);
    g_clear_error(&err);
    assert_one_row("-", "auth.token.refresh", "unknown", "invalid token");
    scope_end();
    sc_teardown();
}

                                                                   

                                                           
static void
test_d8_redact_origin(void)
{
    static const struct { const gchar *in; const gchar *out; } cases[] = {
        { "https://hooks.slack.com/services/T000/B000/XXXX", "https://hooks.slack.com" },
        { "https://api.telegram.org/bot123:ABC/sendMessage", "https://api.telegram.org" },
        { "http://user:pw@alerts.example.com:8443/hook?k=v#f", "http://alerts.example.com" },
        { "https://[2001:db8::1]:8443/hook", "https://[2001:db8::1]" },
        { "", "" },
        { "not a url /with/SECRET", "(invalid-url)" },
        { "https:///no-host/SECRET", "(invalid-url)" },
                                               
        { "https://user:p@ss@host.example/p", "(invalid-url)" },
        { "https://host\\secret/path", "(invalid-url)" },
        { "https://SECRET%2Fhost.example/x", "(invalid-url)" },
        { "https://host%20secret.example/", "(invalid-url)" },
        { "https://a%3Asecret/x", "(invalid-url)" },
        { "https://host%25secret/x", "(invalid-url)" },
                         
        { "https://user:p%40ss@host.example/", "https://host.example" },
        { "https://[2001:db8::1]/", "https://[2001:db8::1]" },
        { "https://[fe80::1%25eth0]:8443/hook", "https://[fe80::1%eth0]" },
        { "https://my_host-1.example:8443/hook", "https://my_host-1.example" },
        { "https://host.example?token=SECRET", "https://host.example" },
        { "https://host.example#token=SECRET", "https://host.example" },
        { "http://192.0.2.1:9/SECRET", "http://192.0.2.1" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree gchar *got = pcv_url_redact_origin(cases[i].in);
        g_assert_cmpstr(got, ==, cases[i].out);
    }
    g_autofree gchar *none = pcv_url_redact_origin(NULL);
    g_assert_cmpstr(none, ==, "");
}

                                                                                       
                                                 
static void
test_d8_member_for_caller(void)
{
    static const gchar *const url = "https://hooks.slack.com/services/T000/B000/XXXX";
    static const struct { gboolean inject; gint role; const gchar *expect; } cases[] = {
        { FALSE, 0, "https://hooks.slack.com/services/T000/B000/XXXX" },               
        { TRUE, PCV_ROLE_ADMIN, "https://hooks.slack.com/services/T000/B000/XXXX" },
        { TRUE, PCV_ROLE_OPERATOR, "https://hooks.slack.com" },
        { TRUE, PCV_ROLE_VIEWER, "https://hooks.slack.com" },
        { TRUE, 99, "https://hooks.slack.com" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        JsonObject *params = json_object_new();
        if (cases[i].inject)
            json_object_set_int_member(params, "_pcv_caller_role", cases[i].role);
        JsonObject *cfg = json_object_new();
        json_object_set_string_member(cfg, "webhook_url", url);
        json_object_set_int_member(cfg, "cpu_warn", 80);
        pcv_disp_webhook_member_for_caller(cfg, "webhook_url", params, NULL);
        pcv_disp_webhook_member_for_caller(cfg, "webhook_crit_url", params, NULL);              
        pcv_disp_webhook_member_for_caller(cfg, "cpu_warn", params, NULL);                       
        g_assert_cmpstr(json_object_get_string_member(cfg, "webhook_url"), ==, cases[i].expect);
        g_assert_false(json_object_has_member(cfg, "webhook_crit_url"));
        g_assert_cmpint(json_object_get_int_member(cfg, "cpu_warn"), ==, 80);
        json_object_unref(cfg);
        json_object_unref(params);
    }
}

void
test_sec_consistency_register(void)
{
    g_test_add_func("/sec_consistency/d1/verify_writes_last_used", test_d1_verify_writes_last_used);
    g_test_add_func("/sec_consistency/d1/last_used_throttled_60s", test_d1_last_used_throttled_60s);
    g_test_add_func("/sec_consistency/d1/failed_auth_writes_nothing", test_d1_failed_auth_writes_nothing);
    g_test_add_func("/sec_consistency/d2/user_create_one_row", test_d2_user_create_one_row);
    g_test_add_func("/sec_consistency/d2/user_delete_one_row", test_d2_user_delete_one_row);
    g_test_add_func("/sec_consistency/d2/role_set_one_row_keeps_before_after",
                    test_d2_role_set_one_row_keeps_before_after);
    g_test_add_func("/sec_consistency/d2/role_set_self_denied_one_row", test_d2_role_set_self_denied_one_row);
    g_test_add_func("/sec_consistency/d2/rbac_layer_writes_no_audit_row", test_d2_rbac_layer_writes_no_audit_row);
    g_test_add_func("/sec_consistency/d2/bootstrap_admin_audit_kept", test_d2_bootstrap_admin_audit_kept);
    g_test_add_func("/sec_consistency/d2/rest_gate_denial_method_set", test_d2_rest_gate_denial_method_set);
    g_test_add_func("/sec_consistency/d3/invalid_refresh_actor_is_dash", test_d3_invalid_refresh_actor_is_dash);
    g_test_add_func("/sec_consistency/d8/redact_origin", test_d8_redact_origin);
    g_test_add_func("/sec_consistency/d8/member_for_caller", test_d8_member_for_caller);
}
