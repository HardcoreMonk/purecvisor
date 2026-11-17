   
                             
                                                                                                
  
                           
                                                                                                      
                                                                                 
                                                                                
                                                                                                  
                                                                                 
                                                                                     
                                                                                                       
                                                                                   
                                         
  
                                                                             
                                                                                   
        
   
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "modules/auth/pcv_rbac.h"
#include "modules/resource/resource_gate.h"
#include "modules/resource/vm_read_scope.h"
#include "modules/daemons/vm_diag_view.h"

                                             
void pcv_vm_read_set_owner_tenant_fn_for_test(const gchar *(*fn)(const gchar *owner));
guint pcv_vm_read_owner_lookups_for_test(void);
void pcv_vm_read_set_caller_tenant_fn_for_test(const gchar *(*fn)(const gchar *subject));
guint pcv_vm_read_caller_lookups_for_test(void);

#define BAD_TENANT "bad name!"                                        

                                     
static const gchar *
_fake_owner_tenant(const gchar *owner)
{
    static const struct { const gchar *owner; const gchar *tenant; } t[] = {
        { "alice", "ta" }, { "bob", "ta" }, { "carol", "tb" }, { "dave", NULL }, { "grpc", "ta" }, { "erin", "ta" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(t); i++)
        if (g_strcmp0(owner, t[i].owner) == 0)
            return t[i].tenant;
    return NULL;
}

                                                             
static const gchar *
_overwriting_owner_tenant(const gchar *owner)
{
    static gchar buf[64];
    const gchar *t = _fake_owner_tenant(owner);
    if (!t)
        return NULL;
    g_strlcpy(buf, t, sizeof buf);
    return buf;
}

                                                                                    
static PcvVmReadScope *
_scope(gint role, const gchar *subject, const gchar *tenant)
{
    PcvResCaller c = { .role = role, .subject = g_strdup(subject), .tenant = g_strdup(tenant) };
    PcvVmReadScope *s = pcv_vm_read_scope_new(&c);
    pcv_res_caller_clear(&c);
    return s;
}

                                                                  
typedef struct {
    gint role;
    const gchar *caller_key;
    const gchar *caller_tenant;
    const gchar *owner_key;
    const gchar *owner_tenant;
    gboolean want;
} DecideRow;

                                                                                             
                                                                                    
                                                                                                  
static void
test_decide_truth_table(void)
{
    static const gchar *const ctenants[] = { "ta", NULL, BAD_TENANT };
                                                               
    static const struct {
        const gchar *owner;
        const gchar *otenant[3];
        gboolean want[3];
    } owners[] = {
        { NULL,    { NULL, NULL, NULL },             { FALSE, FALSE, FALSE } },                        
        { "alice", { "ta", NULL, BAD_TENANT },       { TRUE,  TRUE,  TRUE  } },                     
        { "bob",   { "ta", NULL, BAD_TENANT },       { TRUE,  FALSE, FALSE } },                                   
        { "carol", { "tb", "tb", "tb" },             { FALSE, FALSE, FALSE } },                 
        { "dave",  { NULL, NULL, NULL },             { FALSE, FALSE, FALSE } },                 
        { "grpc",  { "ta", "ta", "ta" },             { TRUE,  FALSE, FALSE } },                                
    };
    guint rows = 0;
    for (gint role = PCV_ROLE_VIEWER; role <= PCV_ROLE_ADMIN; role++) {
        for (guint o = 0; o < G_N_ELEMENTS(owners); o++) {
            for (guint t = 0; t < G_N_ELEMENTS(ctenants); t++) {
                DecideRow r = {
                    .role = role, .caller_key = "alice", .caller_tenant = ctenants[t],
                    .owner_key = owners[o].owner, .owner_tenant = owners[o].otenant[t],
                    .want = role >= PCV_ROLE_ADMIN ? TRUE : owners[o].want[t],
                };
                gboolean got = pcv_vm_read_decide(r.role, r.caller_key, r.caller_tenant, r.owner_key, r.owner_tenant);
                if (got != r.want)
                    g_error("decide row role=%d owner=%s otenant=%s ctenant=%s: got %d want %d", r.role,
                            r.owner_key ? r.owner_key : "(null)", r.owner_tenant ? r.owner_tenant : "(null)",
                            r.caller_tenant ? r.caller_tenant : "(null)", got, r.want);
                rows++;
            }
        }
    }
    g_assert_cmpuint(rows, ==, 54);
                                    
    g_assert_false(pcv_vm_read_decide(PCV_ROLE_OPERATOR, "alice", "ta", "", "ta"));
    g_assert_false(pcv_vm_read_decide(PCV_ROLE_OPERATOR, "", "ta", "", NULL));
    g_assert_false(pcv_vm_read_decide(PCV_ROLE_OPERATOR, NULL, "", "bob", ""));
    g_assert_false(pcv_vm_read_decide(PCV_ROLE_VIEWER, NULL, NULL, "bob", "ta"));
    g_assert_true(pcv_vm_read_decide(PCV_ROLE_ADMIN, NULL, NULL, NULL, NULL));
                                  
    g_assert_false(pcv_vm_read_decide(PCV_ROLE_OPERATOR, "alice", "ta", "alice2", NULL));
    g_assert_false(pcv_vm_read_decide(PCV_ROLE_OPERATOR, "alice", "ta", "bob", "ta2"));
}

                                                                      
static void
test_scope_cache_one_lookup(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_fake_owner_tenant);
    PcvVmReadScope *s = _scope(PCV_ROLE_VIEWER, "zed", "ta");
    guint before = pcv_vm_read_owner_lookups_for_test();
    for (int i = 0; i < 5; i++)
        g_assert_true(pcv_vm_read_visible(s, "bob"));
    g_assert_cmpuint(pcv_vm_read_owner_lookups_for_test() - before, ==, 1);
    for (int i = 0; i < 5; i++)
        g_assert_false(pcv_vm_read_visible(s, "carol"));
    g_assert_cmpuint(pcv_vm_read_owner_lookups_for_test() - before, ==, 2);
                                             
    g_assert_false(pcv_vm_read_visible(s, "dave"));
    g_assert_false(pcv_vm_read_visible(s, "dave"));
    g_assert_cmpuint(pcv_vm_read_owner_lookups_for_test() - before, ==, 3);
    pcv_vm_read_scope_free(s);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

                                         
static void
test_scope_admin_no_lookup(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_fake_owner_tenant);
    PcvVmReadScope *s = _scope(PCV_ROLE_ADMIN, "root-admin", NULL);
    guint before = pcv_vm_read_owner_lookups_for_test();
    g_assert_true(pcv_vm_read_scope_all(s));
    g_assert_true(pcv_vm_read_visible(s, NULL));
    g_assert_true(pcv_vm_read_visible(s, ""));
    g_assert_true(pcv_vm_read_visible(s, "carol"));
    g_assert_true(pcv_vm_read_visible(s, "dave"));
    g_assert_cmpuint(pcv_vm_read_owner_lookups_for_test() - before, ==, 0);
    pcv_vm_read_scope_free(s);
                                        
    s = _scope(PCV_ROLE_OPERATOR, "alice", "ta");
    g_assert_false(pcv_vm_read_scope_all(s));
    pcv_vm_read_scope_free(s);
    g_assert_false(pcv_vm_read_scope_all(NULL));
    g_assert_false(pcv_vm_read_visible(NULL, "alice"));
    pcv_vm_read_scope_free(NULL);
                                      
    s = pcv_vm_read_scope_new(NULL);
    g_assert_false(pcv_vm_read_scope_all(s));
    g_assert_false(pcv_vm_read_visible(s, "alice"));
    g_assert_false(pcv_vm_read_visible(s, "bob"));
    pcv_vm_read_scope_free(s);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

                                                                             
static void
test_scope_borrowed_value_copied(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_overwriting_owner_tenant);
    PcvVmReadScope *s = _scope(PCV_ROLE_VIEWER, "zed", "ta");
    g_assert_true(pcv_vm_read_visible(s, "bob"));                    
    g_assert_false(pcv_vm_read_visible(s, "carol"));                      
    g_assert_true(pcv_vm_read_visible(s, "bob"));                           
    g_assert_true(pcv_vm_read_visible(s, "erin"));                   
    g_assert_false(pcv_vm_read_visible(s, "carol"));
    pcv_vm_read_scope_free(s);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

                                                                                 
static void
test_scope_no_tenant_operator_own(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_fake_owner_tenant);
    guint before = pcv_vm_read_owner_lookups_for_test();
    PcvVmReadScope *s = _scope(PCV_ROLE_OPERATOR, "dave", NULL);
    g_assert_true(pcv_vm_read_visible(s, "dave"));
    g_assert_false(pcv_vm_read_visible(s, "bob"));
    g_assert_false(pcv_vm_read_visible(s, "carol"));
    g_assert_false(pcv_vm_read_visible(s, "grpc"));
    g_assert_false(pcv_vm_read_visible(s, NULL));
    pcv_vm_read_scope_free(s);
                                      
    s = _scope(PCV_ROLE_VIEWER, "alice", "ta");
    g_assert_true(pcv_vm_read_visible(s, "alice"));
    pcv_vm_read_scope_free(s);
    g_assert_cmpuint(pcv_vm_read_owner_lookups_for_test() - before, ==, 0);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

                                                            
static PcvVmReadScope *
_scope_from_params(const gchar *json)
{
    g_autoptr(JsonParser) p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, json, -1, NULL));
    JsonObject *params = json_node_get_object(json_parser_get_root(p));
    PcvResCaller c = { 0 };
    pcv_res_caller_init(&c, params, NULL);
    PcvVmReadScope *s = pcv_vm_read_scope_new(&c);
    pcv_res_caller_clear(&c);
    return s;
}

                                                                               
static void
test_scope_caller_init_rules(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_fake_owner_tenant);
                                                   
    PcvVmReadScope *s = _scope_from_params("{\"_pcv_caller_sub\":\"alice\",\"_pcv_caller_role\":1,"
                                           "\"_pcv_caller_tenant\":\"ta\"}");
    g_assert_false(pcv_vm_read_scope_all(s));
    g_assert_true(pcv_vm_read_visible(s, "bob"));
    g_assert_true(pcv_vm_read_visible(s, "grpc"));
    g_assert_false(pcv_vm_read_visible(s, "carol"));
    g_assert_true(pcv_vm_read_visible(s, "alice"));
    g_assert_false(pcv_vm_read_visible(s, NULL));
    pcv_vm_read_scope_free(s);
                                 
    s = _scope_from_params("{\"_pcv_caller_sub\":\"alice\",\"_pcv_caller_role\":0,\"_pcv_caller_tenant\":null}");
    g_assert_false(pcv_vm_read_visible(s, "bob"));
    g_assert_true(pcv_vm_read_visible(s, "alice"));
    pcv_vm_read_scope_free(s);
                           
    s = _scope_from_params("{\"_pcv_caller_sub\":\"alice\",\"_pcv_caller_role\":1,\"_pcv_caller_tenant\":\"" BAD_TENANT
                           "\"}");
    g_assert_false(pcv_vm_read_visible(s, "bob"));
    g_assert_true(pcv_vm_read_visible(s, "alice"));
    pcv_vm_read_scope_free(s);
                                       
    guint before = pcv_vm_read_owner_lookups_for_test();
    s = _scope_from_params("{\"_pcv_caller_sub\":\"alice\",\"_pcv_caller_role\":2,\"_pcv_caller_tenant\":\"tb\"}");
    g_assert_true(pcv_vm_read_scope_all(s));
    g_assert_true(pcv_vm_read_visible(s, "bob"));
    g_assert_true(pcv_vm_read_visible(s, NULL));
    pcv_vm_read_scope_free(s);
                               
    s = _scope_from_params("{}");
    g_assert_true(pcv_vm_read_scope_all(s));
    pcv_vm_read_scope_free(s);
    g_assert_cmpuint(pcv_vm_read_owner_lookups_for_test() - before, ==, 0);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

                                                                          
typedef struct { const gchar *name, *uuid, *owner; gint domain_id; } MapRow;
static void
_publish_map(const MapRow *rows, guint n, PcvVmDiagSamplerState state, gint64 age_ms)
{
    PcvVmDiagSnapshot *s = g_new0(PcvVmDiagSnapshot, 1);
    s->state = state;
    s->observed_mono_us = g_get_monotonic_time() - age_ms * 1000;
    s->configs = g_ptr_array_new_with_free_func(pcv_vm_diag_config_free);
    for (guint i = 0; i < n; i++) {
        PcvVmDiagConfig *c = g_new0(PcvVmDiagConfig, 1);
        c->name = g_strdup(rows[i].name);
        c->uuid = g_strdup(rows[i].uuid);
        c->owner = g_strdup(rows[i].owner);
        c->domain_id = rows[i].domain_id;
        c->state = -1;
        g_ptr_array_add(s->configs, c);
    }
    pcv_vm_diag_store_publish(s);
}

                                                                                         
static const MapRow MAP3[] = {
    { "a1", "11111111-1111-1111-1111-111111111111", "alice", 4 },
    { "b1", "22222222-2222-2222-2222-222222222222", "carol", 7 },
    { "c1", "33333333-3333-3333-3333-333333333333", "bob", -1 },
};

                                                                                               
                                                                                    
static void
test_owner_map_rules(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_fake_owner_tenant);
    pcv_vm_diag_store_publish(NULL);
    g_assert_null(pcv_vm_owner_map_current(g_get_monotonic_time()));
    _publish_map(MAP3, G_N_ELEMENTS(MAP3), PCV_VM_DIAG_S_ERROR, 1000);
    g_assert_null(pcv_vm_owner_map_current(g_get_monotonic_time()));
    _publish_map(MAP3, G_N_ELEMENTS(MAP3), PCV_VM_DIAG_S_OK, PCV_VM_DIAG_STALE_MS + 1000);
    g_assert_null(pcv_vm_owner_map_current(g_get_monotonic_time()));
    _publish_map(MAP3, G_N_ELEMENTS(MAP3), PCV_VM_DIAG_S_OK, 1000);
    PcvVmOwnerMap *m = pcv_vm_owner_map_current(g_get_monotonic_time());
    g_assert_nonnull(m);

    PcvResCaller c = { .role = PCV_ROLE_VIEWER, .subject = (gchar *)"zed", .tenant = (gchar *)"ta" };
    PcvVmReadScope *s = pcv_vm_read_scope_new(&c);
    g_assert_true(pcv_vm_read_visible_name(s, m, "a1"));
    g_assert_false(pcv_vm_read_visible_name(s, m, "b1"));
    g_assert_true(pcv_vm_read_visible_name(s, m, "c1"));                                     
    g_assert_false(pcv_vm_read_visible_name(s, m, "zz"));                                 
    g_assert_false(pcv_vm_read_visible_name(s, m, NULL));
    g_assert_true(pcv_vm_read_visible_uuid(s, m, "11111111-1111-1111-1111-111111111111"));
    g_assert_false(pcv_vm_read_visible_uuid(s, m, "22222222-2222-2222-2222-222222222222"));
    g_assert_false(pcv_vm_read_visible_uuid(s, m, "44444444-4444-4444-4444-444444444444"));
    g_assert_true(pcv_vm_read_visible_domain_id(s, m, 4));
    g_assert_false(pcv_vm_read_visible_domain_id(s, m, 7));
    g_assert_false(pcv_vm_read_visible_domain_id(s, m, -1));                        
    g_assert_false(pcv_vm_read_visible_domain_id(s, m, 99));
    g_assert_false(pcv_vm_read_visible_name(s, NULL, "a1"));                      
    pcv_vm_read_scope_free(s);

    c = (PcvResCaller){ .role = PCV_ROLE_ADMIN, .subject = (gchar *)"root" };
    s = pcv_vm_read_scope_new(&c);
    g_assert_true(pcv_vm_read_visible_name(s, NULL, "b1"));                             
    g_assert_true(pcv_vm_read_visible_domain_id(s, NULL, -1));
    pcv_vm_read_scope_free(s);
    pcv_vm_owner_map_free(m);
    pcv_vm_owner_map_free(NULL);
    pcv_vm_diag_store_publish(NULL);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

                                      
static const gchar *
_fake_caller_tenant(const gchar *subject)
{
    return g_strcmp0(subject, "alice") == 0 ? "ta" : (g_strcmp0(subject, "badt") == 0 ? BAD_TENANT : NULL);
}

                                                                                                
                                                                
static void
test_new_parts_rules(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_fake_owner_tenant);
    pcv_vm_read_set_caller_tenant_fn_for_test(_fake_caller_tenant);
    guint before = pcv_vm_read_caller_lookups_for_test();
    PcvVmReadScope *s = pcv_vm_read_scope_new_parts(PCV_ROLE_VIEWER, "alice", FALSE, NULL);
    g_assert_cmpuint(pcv_vm_read_caller_lookups_for_test() - before, ==, 1);
    g_assert_true(pcv_vm_read_visible(s, "bob"));                         
    g_assert_false(pcv_vm_read_visible(s, "carol"));
    pcv_vm_read_scope_free(s);

    before = pcv_vm_read_caller_lookups_for_test();
    s = pcv_vm_read_scope_new_parts(PCV_ROLE_ADMIN, "alice", FALSE, NULL);
    g_assert_true(pcv_vm_read_scope_all(s));
    g_assert_true(pcv_vm_read_visible(s, "carol"));
    pcv_vm_read_scope_free(s);
    s = pcv_vm_read_scope_new_parts(PCV_ROLE_OPERATOR, "alice", TRUE, NULL);                         
    g_assert_false(pcv_vm_read_visible(s, "bob"));
    g_assert_true(pcv_vm_read_visible(s, "alice"));
    pcv_vm_read_scope_free(s);
    s = pcv_vm_read_scope_new_parts(PCV_ROLE_VIEWER, "zed", TRUE, "ta");                
    g_assert_true(pcv_vm_read_visible(s, "bob"));
    pcv_vm_read_scope_free(s);
    g_assert_cmpuint(pcv_vm_read_caller_lookups_for_test() - before, ==, 0);

    s = pcv_vm_read_scope_new_parts(PCV_ROLE_VIEWER, "badt", FALSE, NULL);                               
    g_assert_false(pcv_vm_read_visible(s, "bob"));
    pcv_vm_read_scope_free(s);
    pcv_vm_read_set_caller_tenant_fn_for_test(NULL);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

void
test_vm_read_scope_register(void)
{
    g_test_add_func("/vm_read_scope/decide_truth_table", test_decide_truth_table);
    g_test_add_func("/vm_read_scope/scope_cache_one_lookup", test_scope_cache_one_lookup);
    g_test_add_func("/vm_read_scope/scope_admin_no_lookup", test_scope_admin_no_lookup);
    g_test_add_func("/vm_read_scope/scope_borrowed_value_copied", test_scope_borrowed_value_copied);
    g_test_add_func("/vm_read_scope/scope_no_tenant_operator_own", test_scope_no_tenant_operator_own);
    g_test_add_func("/vm_read_scope/scope_caller_init_rules", test_scope_caller_init_rules);
    g_test_add_func("/vm_read_scope/new_parts_rules", test_new_parts_rules);
    g_test_add_func("/vm_read_scope/owner_map_rules", test_owner_map_rules);
}
