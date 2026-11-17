   
                                
                                                                                   
  
                           
                                                                                                                        
                                                                                                       
                                                                                                       
                                                                                                        
                                                                                                           
                                                                             
                                            
  
                                                                                  
                                                           
   
#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "api/dispatcher_owner_scope.h"
#include "api/dispatcher_policy.h"
#include "modules/auth/pcv_rbac.h"

                             
void pcv_disp_set_vm_owner_lookup_for_test(gchar *(*fn)(const gchar *vm_name));
void pcv_vm_read_set_owner_tenant_fn_for_test(const gchar *(*fn)(const gchar *owner));

#define READ_DENY "Permission denied: VM not found or not visible"
#define MANIP_DENY "Permission denied: operators can access only VMs they created"

                                                                                         
static const struct { const gchar *method; const gchar *key; gboolean precheck; } k_read[] = {
    { "vm.snapshot.list", NULL, FALSE }, { "vm.blkio.get", NULL, FALSE }, { "vm.usb.list", NULL, FALSE },
    { "vm.sla.report", NULL, FALSE }, { "vm.cpu.stats", NULL, FALSE }, { "vm.memory.stats", NULL, FALSE },
    { "device.nic.list", "vm_id", FALSE }, { "qos.vm.get", "vm", FALSE }, { "backup.history", "vm_name", FALSE },
    { "vm.metrics", NULL, TRUE }, { "vm.guest.agent.status", NULL, TRUE }, { "vm.guest.fsinfo", NULL, TRUE },
    { "vm.guest.ping", NULL, TRUE }, { "monitor.metrics", "vm_id", TRUE },
};
                
static const gchar *const k_manip[] = {
    "vm.delete", "vm.start", "vm.stop", "vm.pause", "vm.resume", "vm.limit", "vm.rename", "vm.snapshot.create",
    "vm.guest.shutdown", "vm.guest.agent.ensure_channel", "vm.mount_iso", "vm.eject", "vm.vnc", "vm.resize_disk",
    "vm.clone", "vm.blkio.set", "vm.security_group.set", "vm.set_bandwidth", "vm.set_memory", "vm.set_vcpu",
    "vm.pin_vcpu", "vm.snapshot.schedule.set", "vm.snapshot.schedule.delete", "vm.usb.attach", "vm.usb.detach",
    "vm.disk.live_resize", "vm.batch", "vm.autostart", "vm.schedule.set",
};
               
static const gchar *const k_exempt[] = {
    "vm.list", "vm.delete.status", "vm.snapshot.schedule.list", "vm.event.webhook.list", "vm.numa.info",
    "vm.billing.report", "vm.schedule.list", "vm.create", "vm.import.ova",
};

                                                                                             
static guint g_lookups;
static gchar *g_last_lookup;

static gchar *
_fake_owner(const gchar *vm_name)
{
    g_lookups++;
    g_free(g_last_lookup);
    g_last_lookup = g_strdup(vm_name);
    if (g_strcmp0(vm_name, "mine") == 0) return g_strdup("alice");
    if (g_strcmp0(vm_name, "peer") == 0) return g_strdup("bob");
    if (g_strcmp0(vm_name, "theirs") == 0) return g_strdup("carol");
    if (g_strcmp0(vm_name, "noowner") == 0) return g_strdup("");
    return NULL;                   
}

static const gchar *
_fake_owner_tenant(const gchar *owner)
{
    if (g_strcmp0(owner, "alice") == 0 || g_strcmp0(owner, "bob") == 0) return "ta";
    if (g_strcmp0(owner, "carol") == 0) return "tb";
    return NULL;
}

static void
_seams_on(void)
{
    g_lookups = 0;
    g_clear_pointer(&g_last_lookup, g_free);
    pcv_disp_set_vm_owner_lookup_for_test(_fake_owner);
    pcv_vm_read_set_owner_tenant_fn_for_test(_fake_owner_tenant);
}

static void
_seams_off(void)
{
    pcv_disp_set_vm_owner_lookup_for_test(NULL);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
    g_clear_pointer(&g_last_lookup, g_free);
}

                                                                                              
static JsonObject *
_params(gint role, const gchar *key, const gchar *vm)
{
    JsonObject *p = json_object_new();
    json_object_set_string_member(p, "_pcv_caller_sub", "alice");
    json_object_set_int_member(p, "_pcv_caller_role", role);
    json_object_set_string_member(p, "_pcv_caller_tenant", "ta");
    if (key && vm)
        json_object_set_string_member(p, key, vm);
    return p;
}

                                         
static gboolean
_read_allowed(const gchar *method, JsonObject *p, gchar **msg_out)
{
    *msg_out = NULL;
    return pcv_disp_vm_read_allowed(method, p, NULL, msg_out);
}

                                                                     
static void
test_table_and_classes(void)
{
    for (guint i = 0; i < G_N_ELEMENTS(k_read); i++) {
        const gchar *key = (const gchar *)0x1;
        g_assert_true(pcv_disp_vm_method_is_read(k_read[i].method));
        g_assert_true(pcv_disp_vm_read_method_lookup(k_read[i].method, &key));
        g_assert_cmpstr(key, ==, k_read[i].key);
        g_assert_false(pcv_disp_vm_method_requires_owner_scope(k_read[i].method));
        g_assert_cmpint(pcv_disp_vm_read_needs_precheck(k_read[i].method), ==, k_read[i].precheck);
    }
    g_assert_false(pcv_disp_vm_read_needs_precheck("vm.start"));
    g_assert_false(pcv_disp_vm_read_needs_precheck(NULL));
    for (guint i = 0; i < G_N_ELEMENTS(k_manip); i++) {
        if (!pcv_disp_vm_method_requires_owner_scope(k_manip[i]) || pcv_disp_vm_method_is_read(k_manip[i]))
            g_error("조작 범위가 아니다: %s", k_manip[i]);
    }
    for (guint i = 0; i < G_N_ELEMENTS(k_exempt); i++) {
        if (pcv_disp_vm_method_requires_owner_scope(k_exempt[i]) || pcv_disp_vm_method_is_read(k_exempt[i]))
            g_error("면제가 아니다: %s", k_exempt[i]);
    }
    const gchar *key = (const gchar *)0x1;
    g_assert_false(pcv_disp_vm_read_method_lookup("vm.start", &key));
    g_assert_null(key);
    g_assert_false(pcv_disp_vm_method_is_read(NULL));
}

                                                                              
static void
test_target_keys(void)
{
    _seams_on();
    static const struct { const gchar *method; const gchar *key; } rows[] = {
        { "device.nic.list", "vm_id" }, { "qos.vm.get", "vm" }, { "backup.history", "vm_name" },
        { "vm.blkio.get", "name" }, { "vm.blkio.get", "vm_id" }, { "vm.blkio.get", "vm_name" }, { "vm.blkio.get", "vm" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        JsonObject *p = _params(PCV_ROLE_VIEWER, rows[i].key, "mine");
        gchar *msg = NULL;
        g_assert_true(_read_allowed(rows[i].method, p, &msg));
        g_assert_cmpstr(g_last_lookup, ==, "mine");
        g_free(msg);
        json_object_unref(p);
    }
                                                                
    guint before = g_lookups;
    JsonObject *p = _params(PCV_ROLE_VIEWER, "name", "mine");
    gchar *msg = NULL;
    g_assert_false(_read_allowed("device.nic.list", p, &msg));
    g_assert_cmpstr(msg, ==, READ_DENY);
    g_assert_cmpuint(g_lookups, ==, before);
    g_free(msg);
    json_object_unref(p);
                                
    p = _params(PCV_ROLE_VIEWER, "vm_name", "peer");
    json_object_set_string_member(p, "name", "mine");
    g_assert_true(_read_allowed("vm.usb.list", p, &msg));
    g_assert_cmpstr(g_last_lookup, ==, "mine");
    g_free(msg);
    json_object_unref(p);
    _seams_off();
}

                                                                                      
                                                                                               
                     
static void
test_deny_same_message(void)
{
    _seams_on();
    static const gchar *const denied[] = { NULL, "ghost", "noowner", "fail", "theirs" };
    for (guint i = 0; i < G_N_ELEMENTS(k_read); i++) {
        const gchar *key = k_read[i].key ? k_read[i].key : "name";
        for (gint role = PCV_ROLE_VIEWER; role <= PCV_ROLE_OPERATOR; role++) {
            for (guint d = 0; d < G_N_ELEMENTS(denied); d++) {
                JsonObject *p = _params(role, key, denied[d]);
                gchar *msg = NULL;
                if (_read_allowed(k_read[i].method, p, &msg))
                    g_error("%s role=%d vm=%s: 통과하면 안 된다", k_read[i].method, role, denied[d] ? denied[d] : "(없음)");
                g_assert_cmpstr(msg, ==, READ_DENY);
                g_free(msg);
                json_object_unref(p);
            }
            static const gchar *const allowed[] = { "mine", "peer" };
            for (guint a = 0; a < G_N_ELEMENTS(allowed); a++) {
                JsonObject *p = _params(role, key, allowed[a]);
                gchar *msg = NULL;
                if (!_read_allowed(k_read[i].method, p, &msg))
                    g_error("%s role=%d vm=%s: 거절하면 안 된다", k_read[i].method, role, allowed[a]);
                g_assert_null(msg);
                json_object_unref(p);
            }
        }
        guint before = g_lookups;
        JsonObject *p = _params(PCV_ROLE_ADMIN, key, "theirs");
        gchar *msg = NULL;
        g_assert_true(_read_allowed(k_read[i].method, p, &msg));
        g_assert_null(msg);
        g_assert_cmpuint(g_lookups, ==, before);                            
        json_object_unref(p);
    }
                                         
    JsonObject *p = _params(PCV_ROLE_VIEWER, "name", "peer");
    json_object_set_null_member(p, "_pcv_caller_tenant");
    gchar *msg = NULL;
    g_assert_false(_read_allowed("vm.snapshot.list", p, &msg));
    g_assert_cmpstr(msg, ==, READ_DENY);
    g_free(msg);
    json_object_set_string_member(p, "name", "mine");
    g_assert_true(_read_allowed("vm.snapshot.list", p, &msg));
    json_object_unref(p);
    _seams_off();
}

                                                                            
static void
test_aliases_nonprefix(void)
{
    static const struct { const gchar *method; const gchar *key; } rows[] = {
        { "device.nic.list", "vm_id" }, { "qos.vm.get", "vm" }, { "backup.history", "vm_name" }, { "vm.cpu.stats", "name" },
        { "monitor.metrics", "vm_id" }, { "vm.metrics", "name" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        const gchar *other = g_strcmp0(rows[i].key, "vm") == 0 ? "vm_id" : "vm";
        JsonObject *p = json_object_new();
        json_object_set_string_member(p, rows[i].key, "a");
        json_object_set_string_member(p, other, "b");
        const gchar *reason = NULL;
        if (pcv_dispatcher_vm_target_aliases_valid(rows[i].method, p, &reason))
            g_error("%s: 다른 VM 두 별칭을 받았다", rows[i].method);
        g_assert_nonnull(strstr(reason, "must name the same VM"));
        json_object_set_string_member(p, other, "a");
        g_assert_true(pcv_dispatcher_vm_target_aliases_valid(rows[i].method, p, &reason));
        g_assert_null(reason);
        json_object_unref(p);
    }
}

                                                                                
static void
test_exempt_no_missing_param(void)
{
    static const gchar *const m[] = { "vm.numa.info", "vm.billing.report", "vm.schedule.list" };
    for (guint i = 0; i < G_N_ELEMENTS(m); i++) {
        g_assert_false(pcv_disp_vm_method_requires_owner_scope(m[i]));
        g_assert_false(pcv_disp_vm_method_is_read(m[i]));
    }
}

                                                                
static void
test_operator_manipulation_unchanged(void)
{
    _seams_on();
    JsonObject *p = _params(PCV_ROLE_OPERATOR, "name", "peer");
    gchar *msg = NULL;
    g_assert_false(pcv_disp_vm_owner_scoped_method_allowed("vm.start", p, NULL, PCV_ROLE_OPERATOR, &msg));
    g_assert_cmpstr(msg, ==, MANIP_DENY);
    g_clear_pointer(&msg, g_free);
    json_object_set_string_member(p, "name", "mine");
    g_assert_true(pcv_disp_vm_owner_scoped_method_allowed("vm.start", p, NULL, PCV_ROLE_OPERATOR, &msg));
    g_assert_null(msg);
    json_object_unref(p);
    _seams_off();
}

void
test_vm_read_precheck_register(void)
{
    g_test_add_func("/vm_read_precheck/table_and_classes", test_table_and_classes);
    g_test_add_func("/vm_read_precheck/target_keys", test_target_keys);
    g_test_add_func("/vm_read_precheck/deny_same_message", test_deny_same_message);
    g_test_add_func("/vm_read_precheck/aliases_nonprefix", test_aliases_nonprefix);
    g_test_add_func("/vm_read_precheck/exempt_no_missing_param", test_exempt_no_missing_param);
    g_test_add_func("/vm_read_precheck/operator_manipulation_unchanged", test_operator_manipulation_unchanged);
}
