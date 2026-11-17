   
                              
                                                                                                  
  
                           
                                                                                   
                                                                                                    
                                                                                              
                                                                                
                                                                                    
                                                                
  
                                                                                 
                      
   
#include <glib.h>
#include <string.h>

#include "modules/auth/pcv_rbac.h"
#include "modules/resource/resource_gate.h"
#include "modules/resource/vm_read_filter.h"
#include "modules/resource/vm_read_scope.h"
#include "modules/daemons/vm_diag_view.h"

void pcv_vm_read_set_owner_tenant_fn_for_test(const gchar *(*fn)(const gchar *owner));

#define NS "urn:purecvisor:metadata"
#define XML_OF(owner) "<domain><name>v</name><metadata><pcv:owner xmlns:pcv='" NS "'>" owner "</pcv:owner></metadata></domain>"

static const gchar *
_fake_owner_tenant(const gchar *owner)
{
    if (g_strcmp0(owner, "alice") == 0 || g_strcmp0(owner, "bob") == 0) return "ta";
    if (g_strcmp0(owner, "carol") == 0) return "tb";
    return NULL;
}

static PcvVmReadScope *
_scope(gint role, const gchar *subject, const gchar *tenant)
{
    PcvResCaller c = { .role = role, .subject = (gchar *)subject, .tenant = (gchar *)tenant };
    return pcv_vm_read_scope_new(&c);
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

                                                     
static PcvVmOwnerMap *
_map_ok(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_fake_owner_tenant);
    _publish_map(MAP3, G_N_ELEMENTS(MAP3), PCV_VM_DIAG_S_OK, 1000);
    return pcv_vm_owner_map_current(g_get_monotonic_time());
}

static void
_map_done(PcvVmOwnerMap *m)
{
    pcv_vm_owner_map_free(m);
    pcv_vm_diag_store_publish(NULL);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

static JsonNode *
_parse(const gchar *json)
{
    g_autoptr(GError) error = NULL;
    JsonNode *n = json_from_string(json, &error);
    g_assert_no_error(error);
    return n;
}

static gchar *
_dump(JsonNode *n)
{
    return json_to_string(n, FALSE);
}

                                   
static gchar *
_ints(JsonArray *a, const gchar *member)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < json_array_get_length(a); i++)
        g_string_append_printf(s, "%s%" G_GINT64_FORMAT, i ? "," : "",
                               json_object_get_int_member(json_array_get_object_element(a, i), member));
    return g_string_free(s, FALSE);
}

#define QSCOPE(id, name) "/machine.slice/machine-qemu\\\\x2d" #id "\\\\x2d" name ".scope/libvirt/vcpu0"
static const gchar *PROC_ROWS =
    "[{\"pid\":1,\"type\":\"vm\",\"comm\":\"qemu-system-x86\",\"cgroup\":\"" QSCOPE(4, "a1") "\"},"
    "{\"pid\":2,\"type\":\"vm\",\"comm\":\"qemu-system-x86\",\"cgroup\":\"" QSCOPE(7, "b1") "\"},"
    "{\"pid\":3,\"type\":\"vm\",\"comm\":\"qemu-img\",\"cgroup\":\"/user.slice/user-1000.slice/session-2.scope\"},"
    "{\"pid\":4,\"type\":\"host\",\"comm\":\"bash\",\"cgroup\":\"/user.slice/user-1000.slice/session-2.scope\"},"
    "{\"pid\":5,\"type\":\"system\",\"comm\":\"libvirtd\",\"cgroup\":\"/system.slice/libvirtd.service\"},"
    "{\"pid\":6,\"type\":\"container\",\"comm\":\"init\",\"cgroup\":\"/lxc.payload.web/init.scope\"},"
    "{\"pid\":7,\"type\":\"vm\",\"comm\":\"qemu-system-x86\",\"cgroup\":\"" QSCOPE(99, "gone") "\"},"
    "{\"pid\":8,\"type\":\"vm\",\"comm\":\"init\",\"cgroup\":\"/machine.slice/machine-lxc\\\\x2d12\\\\x2dct.scope\"}]";

                                                                                                      
                                                                                                
static void
test_processes_domain_map(void)
{
    PcvVmOwnerMap *m = _map_ok();
    PcvResCaller c = { .role = PCV_ROLE_VIEWER, .subject = (gchar *)"zed", .tenant = (gchar *)"ta" };
    PcvVmReadScope *s = pcv_vm_read_scope_new(&c);

    JsonNode *n = _parse(PROC_ROWS);
    pcv_vm_read_filter_processes(s, m, json_node_get_array(n), 0);
    g_autofree gchar *kept = _ints(json_node_get_array(n), "pid");
    g_assert_cmpstr(kept, ==, "1,4,5,6");
    json_node_unref(n);

    n = _parse(PROC_ROWS);                                                               
    pcv_vm_read_filter_processes(s, NULL, json_node_get_array(n), 0);
    g_autofree gchar *nomap = _ints(json_node_get_array(n), "pid");
    g_assert_cmpstr(nomap, ==, "4,5,6");
    json_node_unref(n);
    pcv_vm_read_scope_free(s);

    c = (PcvResCaller){ .role = PCV_ROLE_ADMIN, .subject = (gchar *)"root" };
    s = pcv_vm_read_scope_new(&c);
    n = _parse(PROC_ROWS);                                                    
    pcv_vm_read_filter_processes(s, NULL, json_node_get_array(n), 0);
    g_assert_cmpuint(json_array_get_length(json_node_get_array(n)), ==, 8);
    json_node_unref(n);
    pcv_vm_read_scope_free(s);
    _map_done(m);
}

                                                                                 
static void
test_processes_cut_after_filter(void)
{
    PcvVmOwnerMap *m = _map_ok();
    GString *j = g_string_new("[");
    for (gint i = 0; i < 10; i++)
        g_string_append_printf(j, "{\"pid\":%d,\"type\":\"vm\",\"cgroup\":\"%s\"},", 100 + i, QSCOPE(7, "b1"));
    for (gint i = 0; i < 5; i++)
        g_string_append_printf(j, "%s{\"pid\":%d,\"type\":\"host\",\"cgroup\":\"/\"}", i ? "," : "", 200 + i);
    g_string_append(j, "]");
    PcvResCaller c = { .role = PCV_ROLE_OPERATOR, .subject = (gchar *)"zed", .tenant = (gchar *)"ta" };
    PcvVmReadScope *s = pcv_vm_read_scope_new(&c);
    JsonNode *n = _parse(j->str);
    pcv_vm_read_filter_processes(s, m, json_node_get_array(n), 3);
    g_autofree gchar *top = _ints(json_node_get_array(n), "pid");
    g_assert_cmpstr(top, ==, "200,201,202");
    json_node_unref(n);
    pcv_vm_read_scope_free(s);

    c = (PcvResCaller){ .role = PCV_ROLE_ADMIN, .subject = (gchar *)"root" };
    s = pcv_vm_read_scope_new(&c);
    n = _parse(j->str);
    pcv_vm_read_filter_processes(s, NULL, json_node_get_array(n), 3);
    g_autofree gchar *atop = _ints(json_node_get_array(n), "pid");
    g_assert_cmpstr(atop, ==, "100,101,102");
    json_node_unref(n);
    pcv_vm_read_scope_free(s);
    g_string_free(j, TRUE);
    _map_done(m);
}

static const gchar *NUMA_JSON =
    "{\"total_cores\":3,\"numa_nodes\":[{\"node\":0,\"total\":3,\"isolated\":3,\"allocated\":2,\"free\":1}],"
    "\"cores\":[{\"logical_id\":0,\"numa_node\":0,\"isolated\":true,\"owner\":\"a1\"},"
    "{\"logical_id\":1,\"numa_node\":0,\"isolated\":true,\"owner\":\"b1\"},"
    "{\"logical_id\":2,\"numa_node\":0,\"isolated\":true,\"owner\":null}]}";

                                                                                                          
                                                             
static void
test_numa_owner_visible(void)
{
    PcvVmOwnerMap *m = _map_ok();
    PcvResCaller c = { .role = PCV_ROLE_VIEWER, .subject = (gchar *)"zed", .tenant = (gchar *)"ta" };
    PcvVmReadScope *s = pcv_vm_read_scope_new(&c);
    JsonNode *n = _parse(NUMA_JSON);
    JsonObject *o = json_node_get_object(n);
    g_autofree gchar *nodes_before = json_to_string(json_object_get_member(o, "numa_nodes"), FALSE);
    pcv_vm_read_mask_numa(s, m, o);
    JsonArray *cores = json_object_get_array_member(o, "cores");
    JsonObject *c0 = json_array_get_object_element(cores, 0), *c1 = json_array_get_object_element(cores, 1),
               *c2 = json_array_get_object_element(cores, 2);
    g_assert_cmpstr(json_object_get_string_member(c0, "owner"), ==, "a1");
    g_assert_true(json_object_get_boolean_member(c0, "owner_visible"));
    g_assert_true(json_object_get_null_member(c1, "owner"));
    g_assert_false(json_object_get_boolean_member(c1, "owner_visible"));
    g_assert_true(json_object_get_null_member(c2, "owner"));
    g_assert_true(json_object_get_boolean_member(c2, "owner_visible"));
    g_autofree gchar *nodes_after = json_to_string(json_object_get_member(o, "numa_nodes"), FALSE);
    g_assert_cmpstr(nodes_after, ==, nodes_before);
    g_assert_cmpint(json_object_get_int_member(o, "total_cores"), ==, 3);
    json_node_unref(n);
    pcv_vm_read_scope_free(s);

    c = (PcvResCaller){ .role = PCV_ROLE_ADMIN, .subject = (gchar *)"root" };
    s = pcv_vm_read_scope_new(&c);
    n = _parse(NUMA_JSON);
    pcv_vm_read_mask_numa(s, NULL, json_node_get_object(n));
    cores = json_object_get_array_member(json_node_get_object(n), "cores");
    for (guint i = 0; i < 3; i++)
        g_assert_true(json_object_get_boolean_member(json_array_get_object_element(cores, i), "owner_visible"));
    g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(cores, 1), "owner"), ==, "b1");
    json_node_unref(n);
    pcv_vm_read_scope_free(s);
    _map_done(m);
}

                                                                                                            
static void
test_by_member_star_kept(void)
{
    PcvVmOwnerMap *m = _map_ok();
    static const gchar *ROWS = "[{\"vm_name\":\"a1\",\"n\":1},{\"vm_name\":\"b1\",\"n\":2},{\"vm_name\":\"*\",\"n\":3},"
                               "{\"vm_name\":\"zz\",\"n\":4},{\"n\":5}]";
    PcvResCaller c = { .role = PCV_ROLE_VIEWER, .subject = (gchar *)"zed", .tenant = (gchar *)"ta" };
    PcvVmReadScope *s = pcv_vm_read_scope_new(&c);
    JsonNode *n = _parse(ROWS);
    JsonArray *out = pcv_vm_read_filter_by_member(s, m, json_array_ref(json_node_get_array(n)), "vm_name", TRUE);
    g_autofree gchar *with_star = _ints(out, "n");
    g_assert_cmpstr(with_star, ==, "1,3");
    json_array_unref(out);
    out = pcv_vm_read_filter_by_member(s, m, json_array_ref(json_node_get_array(n)), "vm_name", FALSE);
    g_autofree gchar *no_star = _ints(out, "n");
    g_assert_cmpstr(no_star, ==, "1");
    json_array_unref(out);
    out = pcv_vm_read_filter_by_member(s, NULL, json_array_ref(json_node_get_array(n)), "vm_name", TRUE);
    g_autofree gchar *nomap = _ints(out, "n");
    g_assert_cmpstr(nomap, ==, "3");                                                                  
    json_array_unref(out);
    pcv_vm_read_scope_free(s);

    c = (PcvResCaller){ .role = PCV_ROLE_ADMIN, .subject = (gchar *)"root" };
    s = pcv_vm_read_scope_new(&c);
    JsonArray *in = json_array_ref(json_node_get_array(n));
    out = pcv_vm_read_filter_by_member(s, NULL, in, "vm_name", TRUE);
    g_assert_true(out == in);
    g_assert_cmpuint(json_array_get_length(out), ==, 5);
    json_array_unref(out);
    pcv_vm_read_scope_free(s);
    json_node_unref(n);
    _map_done(m);
}

                                                                   
static void
test_sg_vms_only(void)
{
    PcvVmOwnerMap *m = _map_ok();
    static const gchar *GROUPS = "[{\"name\":\"web\",\"vms\":[\"a1\",\"b1\",\"zz\"],\"rules\":[{\"port\":80}]},"
                                 "{\"name\":\"db\",\"vms\":[\"b1\"],\"rules\":[]}]";
    PcvResCaller c = { .role = PCV_ROLE_VIEWER, .subject = (gchar *)"zed", .tenant = (gchar *)"ta" };
    PcvVmReadScope *s = pcv_vm_read_scope_new(&c);
    JsonNode *n = _parse(GROUPS);
    pcv_vm_read_filter_sg_vms(s, m, json_node_get_array(n));
    g_autofree gchar *got = _dump(n);
    g_assert_cmpstr(got, ==, "[{\"name\":\"web\",\"vms\":[\"a1\"],\"rules\":[{\"port\":80}]},"
                             "{\"name\":\"db\",\"vms\":[],\"rules\":[]}]");
    json_node_unref(n);
    pcv_vm_read_scope_free(s);

    c = (PcvResCaller){ .role = PCV_ROLE_ADMIN, .subject = (gchar *)"root" };
    s = pcv_vm_read_scope_new(&c);
    n = _parse(GROUPS);
    pcv_vm_read_filter_sg_vms(s, NULL, json_node_get_array(n));
    g_autofree gchar *admin = _dump(n);
    g_assert_cmpstr(admin, ==, GROUPS);
    json_node_unref(n);
    pcv_vm_read_scope_free(s);
    _map_done(m);
}

                                                                             
static void
test_view_admin_skips_map(void)
{
    PcvVmOwnerMap *m = _map_ok();
    JsonNode *n = _parse("{\"_pcv_caller_role\":2,\"_pcv_caller_sub\":\"root\"}");
    PcvVmReadView v;
    pcv_vm_read_view_init(&v, json_node_get_object(n), NULL);
    g_assert_true(pcv_vm_read_scope_all(v.scope));
    g_assert_null(v.map);
    pcv_vm_read_view_clear(&v);
    g_assert_null(v.scope);
    json_node_unref(n);
    n = _parse("{\"_pcv_caller_role\":0,\"_pcv_caller_sub\":\"zed\",\"_pcv_caller_tenant\":\"ta\"}");
    pcv_vm_read_view_init(&v, json_node_get_object(n), NULL);
    g_assert_false(pcv_vm_read_scope_all(v.scope));
    g_assert_nonnull(v.map);
    g_assert_true(pcv_vm_read_visible_name(v.scope, v.map, "a1"));
    pcv_vm_read_view_clear(&v);
    json_node_unref(n);
    _map_done(m);
}

                                         
static PcvVmReadScope *
_viewer_ta(void)
{
    PcvResCaller c = { .role = PCV_ROLE_VIEWER, .subject = (gchar *)"zed", .tenant = (gchar *)"ta" };
    return pcv_vm_read_scope_new(&c);
}

static PcvVmReadScope *
_admin(void)
{
    PcvResCaller c = { .role = PCV_ROLE_ADMIN, .subject = (gchar *)"root" };
    return pcv_vm_read_scope_new(&c);
}

                                             
static gchar *
_strs(JsonArray *a, const gchar *member)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < json_array_get_length(a); i++) {
        JsonObject *o = json_array_get_object_element(a, i);
        const gchar *v = json_object_has_member(o, member) ? json_object_get_string_member(o, member) : "-";
        g_string_append_printf(s, "%s%s", i ? "," : "", v);
    }
    return g_string_free(s, FALSE);
}

                                                                                              
static void
test_healing_target_only(void)
{
    PcvVmOwnerMap *m = _map_ok();
    static const gchar *ROWS = "[{\"action\":\"restart\",\"target\":\"a1\"},{\"action\":\"restart\",\"target\":\"b1\"},"
                               "{\"action\":\"alert_only\",\"target\":\"host-cpu-policy\"},{\"action\":\"migrate\",\"target\":\"gone\"},"
                               "{\"action\":\"restart\"},{\"action\":\"restart\",\"target\":\"11111111-1111-1111-1111-111111111111\"},"
                               "{\"action\":\"restart\",\"target\":\"22222222-2222-2222-2222-222222222222\"},"
                               "{\"action\":\"restart\",\"target\":\"11111111-1111-1111-1111-1111111111FF\"}]";
    PcvVmReadScope *s = _viewer_ta();
    JsonNode *n = _parse(ROWS);
    JsonArray *out = pcv_vm_read_filter_healing_history(s, m, json_array_ref(json_node_get_array(n)));
    g_autofree gchar *kept = _strs(out, "target");
    g_assert_cmpstr(kept, ==, "a1,11111111-1111-1111-1111-111111111111");                                                 
    json_array_unref(out);
    pcv_vm_read_scope_free(s);
    s = _admin();
    JsonArray *in = json_array_ref(json_node_get_array(n));
    out = pcv_vm_read_filter_healing_history(s, NULL, in);
    g_assert_true(out == in);
    json_array_unref(out);
    pcv_vm_read_scope_free(s);
    json_node_unref(n);
    _map_done(m);
}

                                                                                            
typedef struct { gint total; gint own_at; gint read; gint calls; } SecSrc;
static JsonArray *
_sec_src(gint offset, gint limit, gpointer ud)
{
    SecSrc *src = ud;
    src->calls++;
    JsonArray *a = json_array_new();
    for (gint i = offset; i < src->total && i < offset + limit; i++) {
        JsonObject *o = json_object_new();
        json_object_set_int_member(o, "n", i);
        const gboolean own = i >= src->own_at && i < src->own_at + 3, host = i >= src->own_at + 3 && i < src->own_at + 5;
        json_object_set_string_member(o, "target_kind", host ? "host" : "vm");
        json_object_set_string_member(o, "target", own ? "a1" : (host ? "pcv-host" : "b1"));
        json_array_add_object_element(a, o);
        src->read++;
    }
    return a;
}

                                                                                                      
                                                                                           
                              
static void
test_security_events_batched(void)
{
    PcvVmOwnerMap *m = _map_ok();
    PcvVmReadScope *s = _viewer_ta();
    SecSrc src = { 600, 400, 0, 0 };
    JsonArray *out = pcv_vm_read_collect_security_rows(s, m, _sec_src, &src, 0, 100);
    g_autofree gchar *kept = _ints(out, "n");
    g_assert_cmpstr(kept, ==, "400,401,402,403,404");
    g_assert_cmpint(src.read, ==, 600);
    json_array_unref(out);
    src = (SecSrc){ 600, 400, 0, 0 };
    out = pcv_vm_read_collect_security_rows(s, m, _sec_src, &src, 2, 2);
    g_autofree gchar *page = _ints(out, "n");
    g_assert_cmpstr(page, ==, "402,403");
    g_assert_cmpint(src.read, ==, 600);                                                       
    json_array_unref(out);
    src = (SecSrc){ 20000, 19000, 0, 0 };                                        
    out = pcv_vm_read_collect_security_rows(s, m, _sec_src, &src, 0, 100);
    g_assert_cmpuint(json_array_get_length(out), ==, 0);
    g_assert_cmpint(src.read, ==, PCV_VM_READ_SOURCE_CAP);
    g_assert_cmpint(src.calls, ==, PCV_VM_READ_SOURCE_CAP / PCV_VM_READ_BATCH);
    json_array_unref(out);
    pcv_vm_read_scope_free(s);
    s = _admin();
    src = (SecSrc){ 600, 400, 0, 0 };
    out = pcv_vm_read_collect_security_rows(s, NULL, _sec_src, &src, 10, 5);
    g_autofree gchar *admin = _ints(out, "n");
    g_assert_cmpstr(admin, ==, "10,11,12,13,14");
    g_assert_cmpint(src.calls, ==, 1);
    json_array_unref(out);
    pcv_vm_read_scope_free(s);
    _map_done(m);
}

                                                                                        
static void
test_security_actions_vm_rows(void)
{
    PcvVmOwnerMap *m = _map_ok();
    static const gchar *ROWS = "[{\"id\":\"x1\",\"target_kind\":\"vm\",\"target\":\"a1\"},{\"id\":\"x2\",\"target_kind\":\"vm\",\"target\":\"b1\"},"
                               "{\"id\":\"x3\",\"target_kind\":\"ip\",\"target\":\"10.0.0.9\"},{\"id\":\"x4\",\"target_kind\":\"vm\"}]";
    PcvVmReadScope *s = _viewer_ta();
    JsonNode *n = _parse(ROWS);
    JsonArray *out = pcv_vm_read_filter_security_rows(s, m, json_array_ref(json_node_get_array(n)));
    g_autofree gchar *kept = _strs(out, "id");
    g_assert_cmpstr(kept, ==, "x1,x3");
    json_array_unref(out);
    JsonArray *a = json_node_get_array(n);
    g_assert_true(pcv_vm_read_security_row_visible(s, m, json_array_get_object_element(a, 0)));
    g_assert_false(pcv_vm_read_security_row_visible(s, m, json_array_get_object_element(a, 1)));
    g_assert_true(pcv_vm_read_security_row_visible(s, m, json_array_get_object_element(a, 2)));
    g_assert_false(pcv_vm_read_security_row_visible(s, NULL, json_array_get_object_element(a, 0)));              
    pcv_vm_read_scope_free(s);
    json_node_unref(n);
    _map_done(m);
}

                                                      
static gboolean
_fake_event_target(const gchar *event_id, gchar **kind_out, gchar **target_out, gpointer ud)
{
    (void)ud;
    const gchar *kind = NULL, *target = NULL;
    if (g_strcmp0(event_id, "e1") == 0) { kind = "vm"; target = "a1"; }
    else if (g_strcmp0(event_id, "e2") == 0) { kind = "vm"; target = "b1"; }
    else if (g_strcmp0(event_id, "e3") == 0) { kind = "host"; target = "pcv-host"; }
    if (!kind) return FALSE;
    *kind_out = g_strdup(kind);
    *target_out = g_strdup(target);
    return TRUE;
}

                                                                                                             
                                                           
static void
test_security_alert_mask(void)
{
    PcvVmOwnerMap *m = _map_ok();
    static const gchar *ROWS =
        "[{\"metric\":\"Security\",\"message\":\"[warning] Security event e1: ssh brute force on a1\"},"
        "{\"metric\":\"Security\",\"message\":\"[crit] Security event e2: sg sync failed for b1\"},"
        "{\"metric\":\"Security\",\"message\":\"[warning] Security event e3: host file changed\"},"
        "{\"metric\":\"Security\",\"message\":\"[warning] Security event e9: unknown\"},"
        "{\"metric\":\"CPU\",\"message\":\"[crit] CPU 97%\"},"
        "{\"metric\":\"Security\",\"message\":\"garbled\"}]";
    PcvVmReadScope *s = _viewer_ta();
    JsonNode *n = _parse(ROWS);
    pcv_vm_read_mask_security_alerts(s, m, json_node_get_array(n), _fake_event_target, NULL);
    JsonArray *a = json_node_get_array(n);
    g_assert_cmpuint(json_array_get_length(a), ==, 6);
    g_autofree gchar *msgs = _strs(a, "message");
    g_assert_cmpstr(msgs, ==, "[warning] Security event e1: ssh brute force on a1,"
                              "[crit] Security event: details hidden,"
                              "[warning] Security event e3: host file changed,"
                              "[warning] Security event: details hidden,"
                              "[crit] CPU 97%,"
                              "[warn] Security event: details hidden");
    json_node_unref(n);
    pcv_vm_read_scope_free(s);
    s = _admin();
    n = _parse(ROWS);
    pcv_vm_read_mask_security_alerts(s, NULL, json_node_get_array(n), _fake_event_target, NULL);
    g_autofree gchar *raw = _dump(n);
    g_assert_cmpstr(raw, ==, ROWS);
    json_node_unref(n);
    pcv_vm_read_scope_free(s);
    _map_done(m);
}

                                                                                        
static void
test_agent_comparison_rows(void)
{
    PcvVmOwnerMap *m = _map_ok();
    static const gchar *CMP = "{\"consensus\":\"migrate\",\"providers\":["
                              "{\"provider\":\"p1\",\"target_vm\":\"a1\",\"reason\":\"load\"},"
                              "{\"provider\":\"p2\",\"target_vm\":\"b1\",\"reason\":\"b1 hot\"},"
                              "{\"provider\":\"p3\",\"target_vm\":\"\",\"reason\":\"mentions b1\"}]}";
    PcvVmReadScope *s = _viewer_ta();
    JsonNode *n = pcv_vm_read_filter_agent_comparison(s, m, _parse(CMP));
    g_autofree gchar *got = _dump(n);
    g_assert_cmpstr(got, ==, "{\"consensus\":\"migrate\",\"providers\":["
                             "{\"provider\":\"p1\",\"target_vm\":\"a1\",\"reason\":\"load\"},"
                             "{\"provider\":\"p3\",\"target_vm\":\"\",\"reason\":\"\"}]}");
    json_node_unref(n);
    n = pcv_vm_read_filter_agent_comparison(s, m, _parse("{\"status\":\"no_data\"}"));
    g_autofree gchar *nodata = _dump(n);
    g_assert_cmpstr(nodata, ==, "{\"status\":\"no_data\"}");
    json_node_unref(n);
    g_assert_null(pcv_vm_read_filter_agent_comparison(s, m, NULL));
    pcv_vm_read_scope_free(s);
    s = _admin();
    n = pcv_vm_read_filter_agent_comparison(s, NULL, _parse(CMP));
    g_autofree gchar *admin = _dump(n);
    g_assert_cmpstr(admin, ==, CMP);
    json_node_unref(n);
    pcv_vm_read_scope_free(s);
    _map_done(m);
}

                                                                                
static void
test_vnc_close_rule(void)
{
    g_assert_true(pcv_vm_read_vnc_should_close(PCV_ROLE_VIEWER, TRUE));
    g_assert_true(pcv_vm_read_vnc_should_close(PCV_ROLE_OPERATOR, TRUE));
    g_assert_false(pcv_vm_read_vnc_should_close(PCV_ROLE_ADMIN, TRUE));
    g_assert_false(pcv_vm_read_vnc_should_close(PCV_ROLE_OPERATOR, FALSE));
    g_assert_false(pcv_vm_read_vnc_should_close(PCV_ROLE_VIEWER, FALSE));
}

void pcv_ws_subject_cache_set_lookup_for_test(gboolean (*fn)(const gchar *subject, gint *role, const gchar **tenant));
guint pcv_ws_subject_lookups_for_test(void);

                                                                                      
                                                                          
static void
test_ws_kind_table(void)
{
    static const struct { const gchar *type; PcvWsScopeKind kind; } rows[] = {
        { "metric", PCV_WS_SCOPE_ALL }, { "metrics-update", PCV_WS_SCOPE_ALL }, { "forecast", PCV_WS_SCOPE_ALL },
        { "healing", PCV_WS_SCOPE_ADMIN }, { "healing-request", PCV_WS_SCOPE_ADMIN }, { "anomaly", PCV_WS_SCOPE_ADMIN },
        { "agent-comparison", PCV_WS_SCOPE_FILTER }, { "job.complete", PCV_WS_SCOPE_JOB }, { "something-new", PCV_WS_SCOPE_ALL },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++)
        if (pcv_ws_scope_kind(rows[i].type) != rows[i].kind)
            g_error("%s: got %d want %d", rows[i].type, pcv_ws_scope_kind(rows[i].type), rows[i].kind);
    g_assert_cmpint(pcv_ws_scope_kind(NULL), ==, PCV_WS_SCOPE_ADMIN);                    
}

                                                                                               
                                                                                 
static void
test_ws_job_rule(void)
{
    const PcvWsScopeKind J = PCV_WS_SCOPE_JOB;
    g_assert_true(pcv_ws_scope_should_send(J, PCV_ROLE_ADMIN, "root", NULL, NULL, NULL));
    g_assert_true(pcv_ws_scope_should_send(J, PCV_ROLE_VIEWER, "alice", NULL, "alice", NULL));
    g_assert_true(pcv_ws_scope_should_send(J, PCV_ROLE_VIEWER, "bob", "ta", "alice", "ta"));
    g_assert_false(pcv_ws_scope_should_send(J, PCV_ROLE_VIEWER, "bob", "ta", "alice", NULL));
    g_assert_false(pcv_ws_scope_should_send(J, PCV_ROLE_OPERATOR, "bob", "ta", "alice", "tb"));
    g_assert_false(pcv_ws_scope_should_send(J, PCV_ROLE_OPERATOR, "bob", "ta", NULL, "ta"));
    g_assert_false(pcv_ws_scope_should_send(J, PCV_ROLE_VIEWER, "bob", NULL, "alice", NULL));
    g_assert_false(pcv_ws_scope_should_send(PCV_WS_SCOPE_ADMIN, PCV_ROLE_OPERATOR, "bob", "ta", NULL, NULL));
    g_assert_true(pcv_ws_scope_should_send(PCV_WS_SCOPE_ADMIN, PCV_ROLE_ADMIN, "root", NULL, NULL, NULL));
    g_assert_true(pcv_ws_scope_should_send(PCV_WS_SCOPE_ALL, PCV_ROLE_VIEWER, "bob", NULL, NULL, NULL));
    g_assert_true(pcv_ws_scope_should_send(PCV_WS_SCOPE_FILTER, PCV_ROLE_VIEWER, "bob", NULL, NULL, NULL));
}

static gboolean
_ws_fake_lookup(const gchar *subject, gint *role, const gchar **tenant)
{
    if (g_strcmp0(subject, "gone") == 0) return FALSE;
    *role = g_strcmp0(subject, "root") == 0 ? PCV_ROLE_ADMIN : PCV_ROLE_VIEWER;
    *tenant = *role < PCV_ROLE_ADMIN ? "ta" : NULL;
    return TRUE;
}

                                                                    
static void
test_ws_subject_once(void)
{
    pcv_ws_subject_cache_set_lookup_for_test(_ws_fake_lookup);
    guint before = pcv_ws_subject_lookups_for_test();
    PcvWsSubjectCache *c = pcv_ws_subject_cache_new();
    gint role = -1;
    const gchar *tenant = NULL;
    for (guint i = 0; i < 3; i++) {
        g_assert_true(pcv_ws_subject_cache_lookup(c, "alice", &role, &tenant));
        g_assert_cmpint(role, ==, PCV_ROLE_VIEWER);
        g_assert_cmpstr(tenant, ==, "ta");
    }
    g_assert_true(pcv_ws_subject_cache_lookup(c, "root", &role, &tenant));
    g_assert_null(tenant);
    g_assert_false(pcv_ws_subject_cache_lookup(c, "gone", &role, &tenant));
    g_assert_false(pcv_ws_subject_cache_lookup(c, "gone", &role, &tenant));
    g_assert_cmpuint(pcv_ws_subject_lookups_for_test() - before, ==, 3);
    pcv_ws_subject_cache_free(c);
    pcv_ws_subject_cache_set_lookup_for_test(NULL);
}

                                                                      
static void
test_xml_visible(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_fake_owner_tenant);
    static const struct { const gchar *label; gint role; const gchar *sub; const gchar *tenant; const gchar *xml; gboolean want; } rows[] = {
        { "본인",              PCV_ROLE_OPERATOR, "alice", NULL, XML_OF("alice"), TRUE },
        { "같은 테넌트",       PCV_ROLE_VIEWER,   "zed",   "ta", XML_OF("bob"),   TRUE },
        { "다른 테넌트",       PCV_ROLE_VIEWER,   "zed",   "ta", XML_OF("carol"), FALSE },
        { "테넌트 없는 남",    PCV_ROLE_VIEWER,   "zed",   "ta", XML_OF("dave"),  FALSE },
        { "테넌트 없는 호출자", PCV_ROLE_VIEWER,  "zed",   NULL, XML_OF("bob"),   FALSE },
        { "소유자 없음",       PCV_ROLE_OPERATOR, "alice", "ta", "<domain><name>v</name></domain>", FALSE },
        { "이름공간 없는 owner", PCV_ROLE_OPERATOR, "alice", "ta",
          "<domain><metadata><owner>alice</owner></metadata></domain>", FALSE },
        { "파싱 실패",         PCV_ROLE_OPERATOR, "alice", "ta", "<domain", FALSE },
        { "XML 없음",          PCV_ROLE_OPERATOR, "alice", "ta", NULL, FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        PcvVmReadScope *s = _scope(rows[i].role, rows[i].sub, rows[i].tenant);
        gchar *owner = (gchar *)0x1;
        gboolean got = pcv_vm_read_xml_visible(s, rows[i].xml, &owner);
        if (got != rows[i].want)
            g_error("%s: got %d want %d", rows[i].label, got, rows[i].want);
        g_assert_null(owner);                                  
        pcv_vm_read_scope_free(s);
    }
                                                                         
    PcvVmReadScope *a = _scope(PCV_ROLE_ADMIN, "root", NULL);
    gchar *owner = NULL;
    g_assert_true(pcv_vm_read_xml_visible(a, XML_OF("carol"), &owner));
    g_assert_cmpstr(owner, ==, "carol");
    g_free(owner);
    g_assert_true(pcv_vm_read_xml_visible(a, "<domain/>", &owner));
    g_assert_null(owner);
    g_assert_true(pcv_vm_read_xml_visible(a, NULL, &owner));
    g_assert_null(owner);
    g_assert_true(pcv_vm_read_xml_visible(a, XML_OF("carol"), NULL));
    pcv_vm_read_scope_free(a);
    g_assert_false(pcv_vm_read_xml_visible(NULL, XML_OF("alice"), NULL));
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

void
test_vm_read_filter_register(void)
{
    g_test_add_func("/vm_read_filter/xml_visible", test_xml_visible);
    g_test_add_func("/vm_read_filter/processes_domain_map", test_processes_domain_map);
    g_test_add_func("/vm_read_filter/processes_cut_after_filter", test_processes_cut_after_filter);
    g_test_add_func("/vm_read_filter/numa_owner_visible", test_numa_owner_visible);
    g_test_add_func("/vm_read_filter/by_member_star_kept", test_by_member_star_kept);
    g_test_add_func("/vm_read_filter/sg_vms_only", test_sg_vms_only);
    g_test_add_func("/vm_read_filter/view_admin_skips_map", test_view_admin_skips_map);
    g_test_add_func("/vm_read_filter/healing_target_only", test_healing_target_only);
    g_test_add_func("/vm_read_filter/security_events_batched", test_security_events_batched);
    g_test_add_func("/vm_read_filter/security_actions_vm_rows", test_security_actions_vm_rows);
    g_test_add_func("/vm_read_filter/security_alert_mask", test_security_alert_mask);
    g_test_add_func("/vm_read_filter/agent_comparison_rows", test_agent_comparison_rows);
    g_test_add_func("/vm_read_filter/vnc_close_rule", test_vnc_close_rule);
    g_test_add_func("/vm_read_filter/ws_kind_table", test_ws_kind_table);
    g_test_add_func("/vm_read_filter/ws_job_rule", test_ws_job_rule);
    g_test_add_func("/vm_read_filter/ws_subject_once", test_ws_subject_once);
}
