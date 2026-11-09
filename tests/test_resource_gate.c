   
                             
                                                                                                     
  
                           
                                                                                                                           
                                                                                                                                 
                                                                                                                      
                                                                                              
                                                                                                
                                                                                               
                                                                                               
                                                                                                   
                                                                                         
                                         
  
                                                                            
                                                                      
   
#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "api/grpc_server.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/resource/resource_gate.h"
#include "modules/resource/resource_registry.h"
#include "utils/pcv_config.h"

                                 
void pcv_test_audit_reset(void);
gint pcv_test_audit_call_count(void);
const gchar *pcv_test_audit_last_method(void);
const gchar *pcv_test_audit_last_target(void);
const gchar *pcv_test_audit_last_result(void);
gint pcv_test_audit_last_error_code(void);
const gchar *pcv_test_audit_last_source(void);
void pcv_test_uds_response_reset(void);
const gchar *pcv_test_last_uds_response(void);

                                                              
static JsonObject *
params_from(const gchar *json)
{
    g_autoptr(JsonParser) p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, json, -1, NULL));
    JsonNode *root = json_parser_get_root(p);
    g_assert_true(JSON_NODE_HOLDS_OBJECT(root));
    return json_object_ref(json_node_get_object(root));
}

                                                               
static void
assert_caller(const gchar *json, gint want_role, const gchar *want_tenant)
{
    JsonObject *params = params_from(json);
    PcvResCaller c = { 0 };
    pcv_res_caller_init(&c, params, NULL);
    g_assert_cmpint(c.role, ==, want_role);
    g_assert_cmpstr(c.tenant, ==, want_tenant);
    pcv_res_caller_clear(&c);
    g_assert_null(c.subject);
    g_assert_null(c.tenant);
    json_object_unref(params);
}

                                                                            
static void
test_caller_tenant_sources(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("pcv-resgate-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree gchar *db = g_build_filename(dir, "rbac.db", NULL);
    pcv_rbac_init(db);
    static const struct { const gchar *user; const gchar *tenant; } users[] = {
        { "u2", "ta" }, { "u3", "" }, { "grpc", "ta" }, { "ubad", "bad name!" }, { "unull", NULL },
    };
    for (guint i = 0; i < G_N_ELEMENTS(users); i++) {
        g_autoptr(GError) error = NULL;
        g_assert_true(pcv_rbac_user_create(users[i].user, "pw", PCV_ROLE_OPERATOR, users[i].tenant, &error));
        g_assert_no_error(error);
    }

    assert_caller("{\"_pcv_caller_sub\":\"u2\",\"_pcv_caller_role\":1}", PCV_ROLE_OPERATOR, "ta");
                                         
    assert_caller("{\"_pcv_caller_sub\":\"u2\",\"_pcv_caller_role\":1,\"_pcv_caller_tenant\":\"tz\"}",
                  PCV_ROLE_OPERATOR, "tz");
                                                                                      
    assert_caller("{\"_pcv_caller_sub\":\"grpc\",\"_pcv_caller_role\":1,\"_pcv_caller_tenant\":null}",
                  PCV_ROLE_OPERATOR, NULL);
                                                   
    assert_caller("{\"_pcv_caller_sub\":\"u2\",\"_pcv_caller_role\":1,\"_pcv_caller_tenant\":7}",
                  PCV_ROLE_OPERATOR, NULL);
    assert_caller("{\"_pcv_caller_sub\":\"u2\",\"_pcv_caller_role\":1,\"_pcv_caller_tenant\":\"\"}",
                  PCV_ROLE_OPERATOR, NULL);
    assert_caller("{\"_pcv_caller_sub\":\"u2\",\"_pcv_caller_role\":1,\"_pcv_caller_tenant\":\"bad name!\"}",
                  PCV_ROLE_OPERATOR, NULL);
    assert_caller("{\"_pcv_caller_sub\":\"u3\",\"_pcv_caller_role\":1}", PCV_ROLE_OPERATOR, NULL);              
    assert_caller("{\"_pcv_caller_sub\":\"ubad\",\"_pcv_caller_role\":1}", PCV_ROLE_OPERATOR, NULL);             
    assert_caller("{\"_pcv_caller_sub\":\"unull\",\"_pcv_caller_role\":1}", PCV_ROLE_OPERATOR, NULL);
    assert_caller("{\"_pcv_caller_sub\":\"ghost\",\"_pcv_caller_role\":0}", PCV_ROLE_VIEWER, NULL);
                                                                        
    assert_caller("{}", PCV_ROLE_ADMIN, NULL);
    assert_caller("{\"_pcv_caller_sub\":\"u2\",\"_pcv_caller_role\":2,\"_pcv_caller_tenant\":\"tz\"}",
                  PCV_ROLE_ADMIN, NULL);

                                                      
    JsonObject *params = params_from("{\"_pcv_caller_sub\":\"u2\",\"_pcv_caller_role\":1}");
    PcvResCaller c = { 0 };
    pcv_res_caller_init(&c, params, NULL);
    json_object_unref(params);
    g_assert_cmpstr(c.subject, ==, "u2");
    g_assert_cmpstr(c.tenant, ==, "ta");
    pcv_res_caller_clear(&c);
    pcv_res_caller_clear(&c);                   
    g_assert_null(c.subject);

    pcv_rbac_shutdown();
    g_autofree gchar *wal = g_strconcat(db, "-wal", NULL);
    g_autofree gchar *shm = g_strconcat(db, "-shm", NULL);
    g_remove(wal);
    g_remove(shm);
    g_remove(db);
    g_rmdir(dir);
}

                                                                                              
                                                                
static gchar *
owner_cfg_load(const gchar *dir, const gchar *extra)
{
    gchar *cfg = g_build_filename(dir, "daemon.conf", NULL);
    g_autofree gchar *body = g_strdup_printf("[daemon]\nsocket_path = %s/run/daemon.sock\ndb_path = %s/db/pcv.db\n%s",
                                             dir, dir, extra);
    g_assert_true(g_file_set_contents(cfg, body, -1, NULL));
    pcv_config_shutdown();
    g_setenv("PCV_CONFIG_PATH", cfg, TRUE);
    pcv_config_init();
    return cfg;
}

                                                                                  
                                                                                                     
                                                                            
static void
test_owner_tenant_sources(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("pcv-resowner-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree gchar *db = g_build_filename(dir, "rbac.db", NULL);
    pcv_rbac_init(db);
    static const struct { const gchar *user; const gchar *tenant; } users[] = {
        { "u2", "ta" }, { PCV_GRPC_CALLER_SUBJECT, "tb" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(users); i++) {
        g_autoptr(GError) error = NULL;
        g_assert_true(pcv_rbac_user_create(users[i].user, "pw", PCV_ROLE_OPERATOR, users[i].tenant, &error));
        g_assert_no_error(error);
    }

    g_free(owner_cfg_load(dir, "[grpc]\ntenant = ta\n"));
    g_assert_cmpstr(pcv_res_owner_tenant(PCV_GRPC_CALLER_SUBJECT), ==, "ta");
    g_assert_cmpstr(pcv_res_owner_tenant("u2"), ==, "ta");                        
    g_assert_null(pcv_res_owner_tenant("ghost"));                                
    g_assert_null(pcv_res_owner_tenant(NULL));
    g_assert_null(pcv_res_owner_tenant(""));

    g_free(owner_cfg_load(dir, ""));                                                        
    g_assert_null(pcv_res_owner_tenant(PCV_GRPC_CALLER_SUBJECT));
    g_assert_cmpstr(pcv_res_owner_tenant("u2"), ==, "ta");

    g_free(owner_cfg_load(dir, "[grpc]\ntenant = bad name!\n"));                 
    g_assert_null(pcv_res_owner_tenant(PCV_GRPC_CALLER_SUBJECT));

    pcv_config_shutdown();
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);
    pcv_rbac_shutdown();
    gchar *argv[] = { "rm", "-rf", dir, NULL };
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
}

typedef struct {
    gchar *tmp;                          
} GFx;

                                                                                                               
static GFx *
gfx_new(void)
{
    GFx *f = g_new0(GFx, 1);
    g_autofree gchar *raw = g_dir_make_tmp("pcv-resgate2-XXXXXX", NULL);
    g_assert_nonnull(raw);
    f->tmp = realpath(raw, NULL);
    g_assert_nonnull(f->tmp);
    const gchar *dirs[] = { "iso", "other" };
    for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
        g_autofree gchar *d = g_build_filename(f->tmp, dirs[i], NULL);
        g_assert_cmpint(g_mkdir_with_parents(d, 0755), ==, 0);
    }
    const gchar *files[][2] = { { "iso/a.iso", "A" }, { "iso/b.iso", "B" } };
    for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
        g_autofree gchar *p = g_build_filename(f->tmp, files[i][0], NULL);
        g_assert_true(g_file_set_contents(p, files[i][1], -1, NULL));
    }
    g_autofree gchar *a = g_build_filename(f->tmp, "iso/a.iso", NULL);
    g_autofree gchar *l = g_build_filename(f->tmp, "other/l.iso", NULL);
    g_assert_cmpint(symlink(a, l), ==, 0);

    g_autofree gchar *reg = g_build_filename(f->tmp, "reg.db", NULL);
    g_autofree gchar *b = g_build_filename(f->tmp, "iso/b.iso", NULL);
    g_autoptr(GError) err = NULL;
    g_assert_true(pcv_res_registry_open(reg, &err));
    g_assert_true(pcv_res_register(PCV_RES_ISO, "iso-a", a, "admin", NULL, &err));
    g_assert_true(pcv_res_register(PCV_RES_ISO, "iso-b", b, "admin", NULL, &err));
    g_assert_true(pcv_res_grant("iso-a", "ta", &err));
    g_assert_true(pcv_res_grant("iso-b", "tb", &err));

    g_autofree gchar *rb = g_build_filename(f->tmp, "rbac.db", NULL);
    pcv_rbac_init(rb);
    g_assert_true(pcv_rbac_user_create("u2", "pw", PCV_ROLE_OPERATOR, "ta", &err));
    g_assert_true(pcv_rbac_user_create("u5", "pw", PCV_ROLE_OPERATOR, "", &err));
    return f;
}

static void
gfx_free(GFx *f)
{
    pcv_rbac_shutdown();
    pcv_res_registry_close();
    gchar *argv[] = { "rm", "-rf", f->tmp, NULL };
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free(f->tmp);
    g_free(f);
}

static gchar *
gpath(const GFx *f, const gchar *rel)
{
    return g_build_filename(f->tmp, rel, NULL);
}

                                                 
static void
test_decision_table(void)
{
    for (gint role = 0; role <= 2; role++)
        for (gint t = 0; t <= 1; t++)
            for (gint n = 0; n <= 1; n++)
                for (gint a = 0; a <= 1; a++) {
                    PcvResDecision want = role == 2 ? PCV_RES_ALLOW
                                        : !t       ? PCV_RES_DENY_NO_TENANT
                                        : (n && a) ? PCV_RES_ALLOW
                                                   : PCV_RES_DENY_NOT_ALLOWED;
                    g_assert_cmpint(pcv_res_decide(role, t, n, a), ==, want);
                }
}

                                                       
static void
test_check_admin_keeps_raw_value(void)
{
    GFx *f = gfx_new();
    g_autofree gchar *l = gpath(f, "other/l.iso");
    PcvResCaller c = { PCV_ROLE_ADMIN, NULL, NULL };
    g_autofree gchar *eff = NULL;
    PcvResPin pin = { 1, 1, 1 };
    g_assert_cmpint(pcv_res_check(&c, PCV_RES_ISO, l, &eff, &pin), ==, PCV_RES_ALLOW);
    g_assert_cmpstr(eff, ==, l);
    g_assert_cmpuint(pin.dev, ==, 0);
    g_assert_cmpuint(pin.ino, ==, 0);
    g_assert_cmpuint(pin.gen, ==, 0);
    gfx_free(f);
}

                                                                                   
                                                   
static void
test_check_operator_gets_canonical(void)
{
    GFx *f = gfx_new();
    g_autofree gchar *l = gpath(f, "other/l.iso");
    g_autofree gchar *a = gpath(f, "iso/a.iso");
    PcvResCaller c = { PCV_ROLE_OPERATOR, (gchar *)"u2", (gchar *)"ta" };
    g_autofree gchar *eff = NULL;
    PcvResPin pin = { 0 };
    g_assert_cmpint(pcv_res_check(&c, PCV_RES_ISO, l, &eff, &pin), ==, PCV_RES_ALLOW);
    g_assert_cmpstr(eff, ==, a);
    PcvResNorm want = {0};                                                                        
    g_assert_true(pcv_res_normalize(PCV_RES_ISO, a, &want, NULL));
    g_assert_cmpuint(pin.dev, ==, want.dev);
    g_assert_cmpuint(pin.ino, ==, want.ino);
    g_assert_cmpuint(pin.gen, ==, want.gen);
    g_assert_cmpuint(pin.ino, !=, 0);
    pcv_res_norm_clear(&want);

    PcvResPin pin2 = { 9, 9, 9 };
    g_autofree gchar *eff2 = NULL;
    g_assert_cmpint(pcv_res_check(&c, PCV_RES_BRIDGE, "br-nope", &eff2, &pin2), ==, PCV_RES_DENY_NOT_ALLOWED);
    g_assert_null(eff2);
    g_assert_cmpuint(pin2.ino, ==, 0);
    gfx_free(f);
}

                               
static void
test_check_tenant_name(void)
{
    PcvResCaller admin = { PCV_ROLE_ADMIN, NULL, NULL };
    PcvResCaller op = { PCV_ROLE_OPERATOR, (gchar *)"u2", (gchar *)"ta" };
    PcvResCaller none = { PCV_ROLE_OPERATOR, (gchar *)"u5", NULL };
    g_assert_cmpint(pcv_res_check_tenant_name(&admin, "tb"), ==, PCV_RES_ALLOW);
    g_assert_cmpint(pcv_res_check_tenant_name(&op, "ta"), ==, PCV_RES_ALLOW);
    g_assert_cmpint(pcv_res_check_tenant_name(&op, "tb"), ==, PCV_RES_DENY_NOT_ALLOWED);
    g_assert_cmpint(pcv_res_check_tenant_name(&none, "tc"), ==, PCV_RES_DENY_NO_TENANT);
    g_assert_cmpint(pcv_res_check_tenant_name(&op, NULL), ==, PCV_RES_ALLOW);
    g_assert_cmpint(pcv_res_check_tenant_name(&op, ""), ==, PCV_RES_ALLOW);
}

                                                           
static void
test_deny_messages_identical(void)
{
    GFx *f = gfx_new();
    g_autofree gchar *nope = gpath(f, "nope.iso");
    g_autofree gchar *b = gpath(f, "iso/b.iso");
    g_autofree gchar *dir = gpath(f, "iso");
    PcvResCaller c = { PCV_ROLE_OPERATOR, (gchar *)"u2", (gchar *)"ta" };
    g_autofree gchar *e1 = NULL, *e2 = NULL, *e3 = NULL;
    g_assert_cmpint(pcv_res_check(&c, PCV_RES_ISO, nope, &e1, NULL), ==, PCV_RES_DENY_NOT_ALLOWED);
    g_assert_cmpint(pcv_res_check(&c, PCV_RES_ISO, b, &e2, NULL), ==, PCV_RES_DENY_NOT_ALLOWED);
    g_assert_cmpint(pcv_res_check(&c, PCV_RES_ISO, dir, &e3, NULL), ==, PCV_RES_DENY_NOT_ALLOWED);
    g_assert_null(e1);
    g_assert_null(e2);
    g_assert_null(e3);

    g_autofree gchar *msg = pcv_res_decision_message(PCV_RES_DENY_NOT_ALLOWED, "iso_path");
    g_assert_cmpstr(msg, ==, "Permission denied: iso_path is not a resource allowed for this tenant");
    g_assert_null(strstr(msg, f->tmp));
    g_autofree gchar *m2 = pcv_res_decision_message(PCV_RES_DENY_NO_TENANT, "iso_path");
    g_assert_cmpstr(m2, ==, "Permission denied: this account has no tenant \u2014 host resource references are disabled "
                            "until an admin assigns one");
    g_assert_null(pcv_res_decision_message(PCV_RES_ALLOW, "iso_path"));
    gfx_free(f);
}

                                                                                 
static void
test_gate_sends_forbidden_and_audits(void)
{
    GFx *f = gfx_new();
    g_autofree gchar *nope = gpath(f, "nope.iso");
    g_autofree gchar *l = gpath(f, "other/l.iso");
    g_autofree gchar *a = gpath(f, "iso/a.iso");
    JsonObject *params = params_from("{\"_pcv_caller_sub\":\"u2\",\"_pcv_caller_role\":1}");
    g_autofree gchar *eff = NULL;

    pcv_test_audit_reset();
    pcv_test_uds_response_reset();
    g_assert_false(pcv_res_gate(params, NULL, "vm.mount_iso", "iso_path", PCV_RES_ISO, nope, &eff, NULL, "7", NULL));
    g_assert_null(eff);
    g_assert_nonnull(strstr(pcv_test_last_uds_response(), "-32006"));
    g_assert_null(strstr(pcv_test_last_uds_response(), f->tmp));
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "vm.mount_iso");
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "denied");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, -32006);
    g_autofree gchar *want_target = g_strdup_printf("iso_path=%s", nope);
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, want_target);
    g_assert_cmpstr(pcv_test_audit_last_source(), ==, "resource");

    pcv_test_audit_reset();
    pcv_test_uds_response_reset();
    g_assert_true(pcv_res_gate(params, NULL, "vm.mount_iso", "iso_path", PCV_RES_ISO, l, &eff, NULL, "8", NULL));
    g_assert_cmpstr(eff, ==, a);
    g_clear_pointer(&eff, g_free);
    g_assert_true(pcv_res_gate(params, NULL, "vm.mount_iso", "iso_path", PCV_RES_ISO, "", &eff, NULL, "9", NULL));
    g_assert_null(eff);
    g_assert_true(pcv_res_gate(params, NULL, "vm.mount_iso", "iso_path", PCV_RES_ISO, NULL, &eff, NULL, "9", NULL));
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 0);
    g_assert_cmpstr(pcv_test_last_uds_response(), ==, "");
    json_object_unref(params);
    gfx_free(f);
}

                                                                                                     
                                                                            
                                               
static void
test_audit_denied_truncates_utf8(void)
{
    GString *v = g_string_new("\xff");                                                 
    for (gint i = 0; i < 250; i++)
        g_string_append(v, "\xea\xb0\x80");                                   
    pcv_test_audit_reset();
    pcv_res_audit_denied("vm.create", "iso_path", v->str);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    const gchar *t = pcv_test_audit_last_target();
    g_assert_true(g_str_has_prefix(t, "iso_path="));
    const gchar *head = t + strlen("iso_path=");
    g_assert_true(g_utf8_validate(head, -1, NULL));
    g_assert_cmpint(g_utf8_strlen(head, -1), ==, 200);
    g_assert_cmpuint(strlen(head), ==, 3 + 199 * 3);
    g_assert_true(g_str_has_prefix(head, "\xef\xbf\xbd\xea\xb0\x80"));
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "vm.create");
    g_assert_cmpstr(pcv_test_audit_last_result(), ==, "denied");
    g_assert_cmpint(pcv_test_audit_last_error_code(), ==, -32006);
    g_assert_cmpstr(pcv_test_audit_last_source(), ==, "resource");

    pcv_test_audit_reset();
    pcv_res_audit_denied(NULL, NULL, NULL);
    g_assert_cmpint(pcv_test_audit_call_count(), ==, 1);
    g_assert_cmpstr(pcv_test_audit_last_method(), ==, "");
    g_assert_cmpstr(pcv_test_audit_last_target(), ==, "value=");
    g_string_free(v, TRUE);
}

                                                   
static JsonObject *
mk_template(const gchar *iso, const gchar *bridge)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "name", "t1");
    if (iso)
        json_object_set_string_member(o, "iso_path", iso);
    if (bridge)
        json_object_set_string_member(o, "network_bridge", bridge);
    return o;
}

                                                                   
static void
test_filter_array_and_template_view(void)
{
    GFx *f = gfx_new();
    g_autoptr(GError) err = NULL;
    g_autofree gchar *a = gpath(f, "iso/a.iso");
    g_assert_true(pcv_res_register(PCV_RES_PCI, "pci-1", "0000:3b:00.1", "admin", NULL, &err));
    g_assert_true(pcv_res_grant("pci-1", "ta", &err));
    g_assert_true(pcv_res_register(PCV_RES_BRIDGE, "br-ta", "br-ta0", "admin", NULL, &err));
    g_assert_true(pcv_res_grant("br-ta", "ta", &err));

    PcvResCaller ta = { PCV_ROLE_OPERATOR, (gchar *)"u2", (gchar *)"ta" };
    PcvResCaller tb = { PCV_ROLE_OPERATOR, (gchar *)"u9", (gchar *)"tb" };
    PcvResCaller none = { PCV_ROLE_OPERATOR, (gchar *)"u5", NULL };
    PcvResCaller admin = { PCV_ROLE_ADMIN, NULL, NULL };
    const PcvResCaller *callers[] = { &ta, &admin, &none };
    const guint want_len[] = { 1, 2, 0 };
    for (guint i = 0; i < G_N_ELEMENTS(callers); i++) {
        JsonArray *arr = json_array_new();
        const gchar *addrs[] = { "0000:3b:00.0", "0000:3b:00.1" };
        for (guint j = 0; j < 2; j++) {
            JsonObject *o = json_object_new();
            json_object_set_string_member(o, "pci_addr", addrs[j]);
            json_array_add_object_element(arr, o);
        }
        arr = pcv_res_filter_array(callers[i], PCV_RES_PCI, arr, "pci_addr");
        g_assert_cmpuint(json_array_get_length(arr), ==, want_len[i]);
        if (i == 0)
            g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(arr, 0), "pci_addr"), ==,
                            "0000:3b:00.1");
        json_array_unref(arr);
    }

                                                
    JsonObject *v = pcv_res_template_view(&ta, mk_template(a, "pcvnat0"));
    g_assert_nonnull(v);
    g_assert_false(json_object_has_member(v, "iso_path"));
    g_assert_true(json_object_has_member(v, "network_bridge"));
    json_object_unref(v);

    g_assert_null(pcv_res_template_view(&tb, mk_template(a, "pcvnat0")));
    g_assert_null(pcv_res_template_view(&none, mk_template(a, "pcvnat0")));
    JsonObject *av = pcv_res_template_view(&admin, mk_template(a, "pcvnat0"));
    g_assert_nonnull(av);
    g_assert_true(json_object_has_member(av, "iso_path"));
    json_object_unref(av);

                                         
    g_autofree gchar *b = gpath(f, "iso/b.iso");
    g_assert_null(pcv_res_template_view(&ta, mk_template(b, "pcvnat0")));

                                                                        
    g_assert_null(pcv_res_template_view(&tb, mk_template(NULL, "br-ta0")));
    JsonObject *ok = pcv_res_template_view(&ta, mk_template(NULL, "br-ta0"));
    g_assert_nonnull(ok);
    json_object_unref(ok);
    gfx_free(f);
}

void test_resource_gate_register(void);

                                                 
void
test_resource_gate_register(void)
{
    g_test_add_func("/resource_gate/caller_tenant_sources", test_caller_tenant_sources);
    g_test_add_func("/resource_gate/owner_tenant_sources", test_owner_tenant_sources);
    g_test_add_func("/resource_gate/decision_table", test_decision_table);
    g_test_add_func("/resource_gate/check_admin_keeps_raw_value", test_check_admin_keeps_raw_value);
    g_test_add_func("/resource_gate/check_operator_gets_canonical", test_check_operator_gets_canonical);
    g_test_add_func("/resource_gate/check_tenant_name", test_check_tenant_name);
    g_test_add_func("/resource_gate/deny_messages_identical", test_deny_messages_identical);
    g_test_add_func("/resource_gate/gate_sends_forbidden_and_audits", test_gate_sends_forbidden_and_audits);
    g_test_add_func("/resource_gate/audit_denied_truncates_utf8", test_audit_denied_truncates_utf8);
    g_test_add_func("/resource_gate/filter_array_and_template_view", test_filter_array_and_template_view);
}
