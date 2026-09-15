                                                                                        
                                                                                                 
                                                                               
                                                                          
                                                              
                               
  
                                                        
  
                                                       
                                                   
                                                   
                             
  
                                                                      
                                                       
                                                   
                                           
                                                     
                                                 
  
                                                  
                                                         
                                                   
                                      
  
                                                               
                                                      
                                                   
                                                               
             
                                                                
                                                         
                                                                  
                                                    
                                                       
                                                                     
                                                              
                                                                  
                                                       
                                                            
                               
   
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>                         
#include "utils/pcv_spawn.h"
#include "modules/network/pcv_qos.h"
#include "modules/network/pcv_qos_chaos.h"

#define T2_VM_IFACE1 "pcvqosdum0"
#define T2_VM_IFACE2 "pcvqosdum1"

                                                               
                                                              
                                             
                                                       
                                                                              
                                              
               
extern gboolean _chaos_expire_cb(gpointer data);




static gboolean
_fixture_run(const gchar * const *argv)
{
    gchar *out = NULL;
    gchar *err = NULL;
    gboolean ok = pcv_spawn_sync(argv, &out, &err, NULL);
    g_free(out);
    g_free(err);
    return ok;
}



static void
_setup_dummy(const gchar *name)
{
    const gchar *add[] = {"ip", "link", "add", name, "type", "dummy", NULL};
    const gchar *up[] = {"ip", "link", "set", name, "up", NULL};
    if (_fixture_run(add))
        (void)_fixture_run(up);
}


static void
_cleanup_netdevs(void)
{
    const gchar *names[] = {T2_VM_IFACE1, T2_VM_IFACE2, PCV_QOS_IFB_DEV};
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        const gchar *argv[] = {"ip", "link", "del", names[i], NULL};
        (void)_fixture_run(argv);
    }
}

                                                  
                                                 
static gboolean
_output_has_classid(const gchar *out, const gchar *classid)
{
    if (!out || !classid) return FALSE;
    gchar *needle = g_strdup_printf("%s ", classid);
    gboolean found = g_strstr_len(out, -1, needle) != NULL;
    g_free(needle);
    return found;
}

static gchar *
_tc_show(const gchar *cmd)
{
    gchar *out = NULL;
    g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL);
    return out ? out : g_strdup("");
}

static void
test_qos_ifb_lifecycle_root(void)
{
    if (geteuid() != 0) {
        g_test_skip("root 필요 — tc/ip 실제 적용은 CAP_NET_ADMIN 요구");
        return;
    }

    _cleanup_netdevs();                               
    pcv_qos_ids_clear();                                      

    _setup_dummy(T2_VM_IFACE1);
    _setup_dummy(T2_VM_IFACE2);

    GError *e = NULL;
    if (!pcv_qos_ensure_root(1000, &e)) {
        gchar *msg = g_strdup_printf(
            "ensure_root 실패 — ifb/sch_hfsc/sch_cake 커널 지원 부재로 추정: %s",
            e && e->message ? e->message : "unknown");
        g_test_skip(msg);
        g_free(msg);
        g_clear_error(&e);
        _cleanup_netdevs();
        return;
    }

                                                           
                                                             
    {
        gchar *t = NULL, *i = NULL;
        g_assert_false(pcv_qos_lookup_applied("vm1", &t, &i));
        g_assert_null(t);
        g_assert_null(i);
    }

                                                         
    PcvQosSla sla1 = { "acme", "vm1", 100, 500, 256 };
    g_assert_true(pcv_qos_apply_vm(T2_VM_IFACE1, &sla1, &e));
    g_assert_no_error(e);

    PcvQosSla sla2 = { "acme", "vm2", 50, 200, 256 };
    g_assert_true(pcv_qos_apply_vm(T2_VM_IFACE2, &sla2, &e));
    g_assert_no_error(e);

                                                                    
                                                                
                                                          
                  
    {
        gchar *t1 = NULL, *i1 = NULL;
        g_assert_true(pcv_qos_lookup_applied("vm1", &t1, &i1));
        g_assert_cmpstr(t1, ==, "acme");
        g_assert_cmpstr(i1, ==, T2_VM_IFACE1);
        g_free(t1); g_free(i1);

        gchar *t2 = NULL, *i2 = NULL;
        g_assert_true(pcv_qos_lookup_applied("vm2", &t2, &i2));
        g_assert_cmpstr(t2, ==, "acme");
        g_assert_cmpstr(i2, ==, T2_VM_IFACE2);
        g_free(t2); g_free(i2);
    }

                                                                       
                                                               
                  
    {
        g_assert_true(pcv_qos_iface_is_managed(T2_VM_IFACE1));
        g_assert_true(pcv_qos_iface_is_managed(T2_VM_IFACE2));
        g_assert_false(pcv_qos_iface_is_managed("pcvqos-unrelated-iface"));
    }

    gchar *tenant_cid = pcv_qos_tenant_classid("acme");
    gchar *vm1_cid = pcv_qos_classid("acme", "vm1");
    gchar *vm2_cid = pcv_qos_classid("acme", "vm2");

                                                                 
    {
        gchar *class_out = _tc_show("tc class show dev " PCV_QOS_IFB_DEV);
        g_assert_true(_output_has_classid(class_out, tenant_cid));
        g_assert_true(_output_has_classid(class_out, vm1_cid));
        g_assert_true(_output_has_classid(class_out, vm2_cid));
                                                                  
                                                               
                                                                              
                          
        g_assert_true(_output_has_classid(class_out, "1:fffe"));
        g_free(class_out);
    }

                                           
    {
        gchar *qdisc_out = _tc_show("tc qdisc show dev " PCV_QOS_IFB_DEV);
        g_assert_nonnull(g_strstr_len(qdisc_out, -1, "cake"));
        g_free(qdisc_out);
    }

                                                                
                                                   
    {
        gchar *filter_out = _tc_show("tc filter show dev " PCV_QOS_IFB_DEV " parent 1:");
        g_assert_nonnull(g_strstr_len(filter_out, -1, "indev"));
        g_assert_true(_output_has_classid(filter_out, vm1_cid));
        g_assert_true(_output_has_classid(filter_out, vm2_cid));
        g_free(filter_out);

        gchar *redirect_out = _tc_show("tc filter show dev " T2_VM_IFACE1 " parent ffff:");
        g_assert_nonnull(g_strstr_len(redirect_out, -1, "mirred"));
        g_assert_nonnull(g_strstr_len(redirect_out, -1, PCV_QOS_IFB_DEV));
        g_free(redirect_out);
    }

                                                        
                               
    g_assert_true(pcv_qos_remove_vm(T2_VM_IFACE1, "acme", "vm1", &e));
    g_assert_no_error(e);
                                                         
    {
        gchar *t = NULL, *i = NULL;
        g_assert_false(pcv_qos_lookup_applied("vm1", &t, &i));
        g_assert_null(t);
        g_assert_null(i);

        gchar *t2 = NULL, *i2 = NULL;
        g_assert_true(pcv_qos_lookup_applied("vm2", &t2, &i2));
        g_assert_cmpstr(t2, ==, "acme");
        g_assert_cmpstr(i2, ==, T2_VM_IFACE2);
        g_free(t2); g_free(i2);
    }
                                                                     
                                                              
    {
        g_assert_false(pcv_qos_iface_is_managed(T2_VM_IFACE1));
        g_assert_true(pcv_qos_iface_is_managed(T2_VM_IFACE2));
    }
    {
        gchar *class_out = _tc_show("tc class show dev " PCV_QOS_IFB_DEV);
        g_assert_false(_output_has_classid(class_out, vm1_cid));
        g_assert_true(_output_has_classid(class_out, tenant_cid));                      
        g_assert_true(_output_has_classid(class_out, vm2_cid));
        g_free(class_out);
    }
    {
                                                        
        gchar *redirect_out = _tc_show("tc filter show dev " T2_VM_IFACE1 " parent ffff:");
        g_assert_null(g_strstr_len(redirect_out, -1, "mirred"));
        g_free(redirect_out);
    }

                                                    
    g_assert_true(pcv_qos_remove_vm(T2_VM_IFACE2, "acme", "vm2", &e));
    g_assert_no_error(e);
                                                    
    {
        gchar *t = NULL, *i = NULL;
        g_assert_false(pcv_qos_lookup_applied("vm2", &t, &i));
        g_assert_null(t);
        g_assert_null(i);
    }
    {
        gchar *class_out = _tc_show("tc class show dev " PCV_QOS_IFB_DEV);
        g_assert_false(_output_has_classid(class_out, vm2_cid));
        g_assert_false(_output_has_classid(class_out, tenant_cid));
                                                              
        g_assert_true(_output_has_classid(class_out, "1:1"));
        g_assert_true(_output_has_classid(class_out, "1:fffe"));
        g_free(class_out);
    }

    g_free(tenant_cid);
    g_free(vm1_cid);
    g_free(vm2_cid);

    _cleanup_netdevs();
    pcv_qos_ids_clear();
}

                                                                    
                                                    
                                                    
                                                  
                                                           
                                                                     
                                               
                                                        
                                               
static gchar *
_class_line_for(const gchar *out, const gchar *classid)
{
    if (!out || !classid) return NULL;
    gchar **lines = g_strsplit(out, "\n", -1);
    gchar *found = NULL;
    for (guint i = 0; lines[i] && !found; i++) {
        gchar **raw = g_strsplit_set(lines[i], " \t", -1);
        const gchar *tok[3] = { NULL };
        guint n = 0;
        for (guint ti = 0; raw[ti] && n < 3; ti++)
            if (*raw[ti]) tok[n++] = raw[ti];
        if (n == 3 && g_strcmp0(tok[0], "class") == 0 && g_strcmp0(tok[2], classid) == 0)
            found = g_strdup(lines[i]);
        g_strfreev(raw);
    }
    g_strfreev(lines);
    return found;
}

                                                           
                                                              
                                                   
                                                   
                            
static void
test_qos_tenant_sla_live_update_root(void)
{
    if (geteuid() != 0) {
        g_test_skip("root 필요 — tc/ip 실제 적용은 CAP_NET_ADMIN 요구");
        return;
    }

    _cleanup_netdevs();
    pcv_qos_ids_clear();
    pcv_qos_tenant_sla_clear();

    _setup_dummy(T2_VM_IFACE1);

    GError *e = NULL;
    if (!pcv_qos_ensure_root(1000, &e)) {
        gchar *msg = g_strdup_printf(
            "ensure_root 실패 — ifb/sch_hfsc/sch_cake 커널 지원 부재로 추정: %s",
            e && e->message ? e->message : "unknown");
        g_test_skip(msg);
        g_free(msg);
        g_clear_error(&e);
        _cleanup_netdevs();
        return;
    }

                                                  
                                                                    
                                                         
                            
    PcvQosSla sla = { "beta", "vm1", 50, 200, 256 };
    g_assert_true(pcv_qos_apply_vm(T2_VM_IFACE1, &sla, &e));
    g_assert_no_error(e);

    gchar *tenant_cid = pcv_qos_tenant_classid("beta");
    {
        gchar *class_out = _tc_show("tc class show dev " PCV_QOS_IFB_DEV);
        gchar *line = _class_line_for(class_out, tenant_cid);
        g_assert_nonnull(line);
        g_assert_nonnull(g_strstr_len(line, -1, "1Gbit"));                                     
        g_free(line);
        g_free(class_out);
    }

                                                         
                                                   
                                                 
                                                      
    gboolean live_applied = FALSE;
    gchar *live_error = NULL;
    g_assert_true(pcv_qos_tenant_sla_set("beta", 100, 300, &live_applied, &live_error, &e));
    g_assert_no_error(e);
    g_assert_true(live_applied);
    g_assert_null(live_error);
    {
        gchar *class_out = _tc_show("tc class show dev " PCV_QOS_IFB_DEV);
        gchar *line = _class_line_for(class_out, tenant_cid);
        g_assert_nonnull(line);
        g_assert_nonnull(g_strstr_len(line, -1, "100Mbit"));           
        g_assert_nonnull(g_strstr_len(line, -1, "300Mbit"));           
        g_free(line);
        g_free(class_out);
    }

    g_free(tenant_cid);
    g_assert_true(pcv_qos_remove_vm(T2_VM_IFACE1, "beta", "vm1", &e));
    g_assert_no_error(e);

    _cleanup_netdevs();
    pcv_qos_ids_clear();
    pcv_qos_tenant_sla_clear();
}

                                                              
                                                    
                                                         
                                                     
            
static void
test_qos_chaos_inject_expire_root(void)
{
    if (geteuid() != 0) {
        g_test_skip("root 필요 — tc/ip 실제 적용은 CAP_NET_ADMIN 요구");
        return;
    }

    _cleanup_netdevs();
    pcv_qos_ids_clear();
    pcv_qos_chaos_clear();

    _setup_dummy(T2_VM_IFACE1);

    GError *e = NULL;
    if (!pcv_qos_ensure_root(1000, &e)) {
        gchar *msg = g_strdup_printf(
            "ensure_root 실패 — ifb/sch_hfsc/sch_cake 커널 지원 부재로 추정: %s",
            e && e->message ? e->message : "unknown");
        g_test_skip(msg);
        g_free(msg);
        g_clear_error(&e);
        _cleanup_netdevs();
        return;
    }

    PcvQosSla sla = { "acme", "chaosvm1", 100, 500, 256 };
    g_assert_true(pcv_qos_apply_vm(T2_VM_IFACE1, &sla, &e));
    g_assert_no_error(e);

    gchar *vm_cid = pcv_qos_classid("acme", "chaosvm1");

                                                           
    {
        gchar *qdisc_out = _tc_show("tc qdisc show dev " PCV_QOS_IFB_DEV);
        g_assert_nonnull(g_strstr_len(qdisc_out, -1, "cake"));
        g_assert_null(g_strstr_len(qdisc_out, -1, "netem"));
        g_free(qdisc_out);
    }

    g_assert_true(pcv_qos_chaos_start("chaosvm1", "delay 100ms", 2, "tester", FALSE, &e));
    g_assert_no_error(e);

                                                       
                                                 
                                        
    {
        gchar *qdisc_out = _tc_show("tc qdisc show dev " PCV_QOS_IFB_DEV);
        g_assert_nonnull(g_strstr_len(qdisc_out, -1, "netem"));
        gchar *parent_needle = g_strdup_printf("parent %s", vm_cid);
        g_assert_nonnull(g_strstr_len(qdisc_out, -1, parent_needle));
        g_free(parent_needle);
        g_free(qdisc_out);
    }

                                       
    {
        GPtrArray *st = pcv_qos_chaos_status();
        g_assert_cmpuint(st->len, ==, 1);
        PcvQosChaosStatusEntry *s = g_ptr_array_index(st, 0);
        g_assert_cmpstr(s->vm, ==, "chaosvm1");
        g_assert_cmpstr(s->profile, ==, "delay 100ms");
        g_assert_cmpstr(s->admin, ==, "tester");
        g_assert_cmpuint(s->timebox_sec, ==, 2);
        g_ptr_array_unref(st);
    }

                                                         
                                                          
                                             
                                              
                                                           
                                                         
                                                  
                                                 
                              
                                                         
                                     
    g_assert_false(_chaos_expire_cb((gpointer)"chaosvm1"));

                                                       
    {
        gchar *qdisc_out = _tc_show("tc qdisc show dev " PCV_QOS_IFB_DEV);
        g_assert_null(g_strstr_len(qdisc_out, -1, "netem"));
        g_assert_nonnull(g_strstr_len(qdisc_out, -1, "cake"));
        g_free(qdisc_out);
    }
    {
        GPtrArray *st = pcv_qos_chaos_status();
        g_assert_cmpuint(st->len, ==, 0);
        g_ptr_array_unref(st);
    }

    g_free(vm_cid);
    g_assert_true(pcv_qos_remove_vm(T2_VM_IFACE1, "acme", "chaosvm1", &e));
    g_assert_no_error(e);

    _cleanup_netdevs();
    pcv_qos_ids_clear();
    pcv_qos_chaos_clear();
}

                                                          
                                                  
           
static void
test_qos_chaos_stop_root(void)
{
    if (geteuid() != 0) {
        g_test_skip("root 필요 — tc/ip 실제 적용은 CAP_NET_ADMIN 요구");
        return;
    }

    _cleanup_netdevs();
    pcv_qos_ids_clear();
    pcv_qos_chaos_clear();

    _setup_dummy(T2_VM_IFACE1);

    GError *e = NULL;
    if (!pcv_qos_ensure_root(1000, &e)) {
        gchar *msg = g_strdup_printf(
            "ensure_root 실패 — ifb/sch_hfsc/sch_cake 커널 지원 부재로 추정: %s",
            e && e->message ? e->message : "unknown");
        g_test_skip(msg);
        g_free(msg);
        g_clear_error(&e);
        _cleanup_netdevs();
        return;
    }

    PcvQosSla sla = { "acme", "chaosvm2", 100, 500, 256 };
    g_assert_true(pcv_qos_apply_vm(T2_VM_IFACE1, &sla, &e));
    g_assert_no_error(e);

                                                
                                                  
    g_assert_true(pcv_qos_chaos_start("chaosvm2", "loss 5%", 60, "tester", FALSE, &e));
    g_assert_no_error(e);

    {
        gchar *qdisc_out = _tc_show("tc qdisc show dev " PCV_QOS_IFB_DEV);
        g_assert_nonnull(g_strstr_len(qdisc_out, -1, "netem"));
        g_free(qdisc_out);
    }

                                                       
    {
        GError *dup_err = NULL;
        g_assert_false(pcv_qos_chaos_start("chaosvm2", "loss 5%", 60, "tester", FALSE, &dup_err));
        g_assert_nonnull(dup_err);
        g_clear_error(&dup_err);
    }

    g_assert_true(pcv_qos_chaos_stop("chaosvm2", "tester", &e));
    g_assert_no_error(e);

    {
        gchar *qdisc_out = _tc_show("tc qdisc show dev " PCV_QOS_IFB_DEV);
        g_assert_null(g_strstr_len(qdisc_out, -1, "netem"));
        g_assert_nonnull(g_strstr_len(qdisc_out, -1, "cake"));
        g_free(qdisc_out);
    }

                                     
    {
        GError *stop2_err = NULL;
        g_assert_false(pcv_qos_chaos_stop("chaosvm2", "tester", &stop2_err));
        g_assert_nonnull(stop2_err);
        g_clear_error(&stop2_err);
    }

    g_assert_true(pcv_qos_remove_vm(T2_VM_IFACE1, "acme", "chaosvm2", &e));
    g_assert_no_error(e);

    _cleanup_netdevs();
    pcv_qos_ids_clear();
    pcv_qos_chaos_clear();
}

                                                            
                                                   
                                        
static void
test_qos_chaos_purge_all_root(void)
{
    if (geteuid() != 0) {
        g_test_skip("root 필요 — tc/ip 실제 적용은 CAP_NET_ADMIN 요구");
        return;
    }

    _cleanup_netdevs();
    pcv_qos_ids_clear();
    pcv_qos_chaos_clear();

    _setup_dummy(T2_VM_IFACE1);

    GError *e = NULL;
    if (!pcv_qos_ensure_root(1000, &e)) {
        gchar *msg = g_strdup_printf(
            "ensure_root 실패 — ifb/sch_hfsc/sch_cake 커널 지원 부재로 추정: %s",
            e && e->message ? e->message : "unknown");
        g_test_skip(msg);
        g_free(msg);
        g_clear_error(&e);
        _cleanup_netdevs();
        return;
    }

    PcvQosSla sla = { "acme", "chaosvm3", 100, 500, 256 };
    g_assert_true(pcv_qos_apply_vm(T2_VM_IFACE1, &sla, &e));
    g_assert_no_error(e);

    gchar *vm_cid = pcv_qos_classid("acme", "chaosvm3");

                                                      
                                                  
    {
        const gchar *argv[] = {"tc", "qdisc", "replace", "dev", PCV_QOS_IFB_DEV,
            "parent", vm_cid, "netem", "delay", "50ms", NULL};
        g_assert_true(_fixture_run(argv));
    }
    {
        gchar *qdisc_out = _tc_show("tc qdisc show dev " PCV_QOS_IFB_DEV);
        g_assert_nonnull(g_strstr_len(qdisc_out, -1, "netem"));
        g_free(qdisc_out);
    }

    pcv_qos_chaos_purge_all();

    {
        gchar *qdisc_out = _tc_show("tc qdisc show dev " PCV_QOS_IFB_DEV);
        g_assert_null(g_strstr_len(qdisc_out, -1, "netem"));
        g_assert_nonnull(g_strstr_len(qdisc_out, -1, "cake"));
        g_free(qdisc_out);
    }

    g_free(vm_cid);
    g_assert_true(pcv_qos_remove_vm(T2_VM_IFACE1, "acme", "chaosvm3", &e));
    g_assert_no_error(e);

    _cleanup_netdevs();
    pcv_qos_ids_clear();
    pcv_qos_chaos_clear();
}

                                                                             
                                                         
                                                                                

                                                             
                                               
                                                              
                                                
                       
static PcvQosExpectedEntry g_recon_stub_entry;
static guint g_recon_stub_count = 0;

static GPtrArray *
_recon_stub_provider(void)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    if (g_recon_stub_count > 0) {
        PcvQosExpectedEntry *copy = g_new0(PcvQosExpectedEntry, 1);
        *copy = g_recon_stub_entry;
        g_ptr_array_add(arr, copy);
    }
    return arr;
}

                                              
static GPtrArray *
_recon_stub_provider_empty(void)
{
    return g_ptr_array_new_with_free_func(g_free);
}

                                                                        
                                                     
                                                             
                                              
                                                           
                                             
  
                                                        
                                                   
                                                  
                                              
                                                                 
                                                      
                                
static void
test_qos_reconcile_orphan_cleanup_root(void)
{
    if (geteuid() != 0) {
        g_test_skip("root 필요 — tc/ip 실제 적용은 CAP_NET_ADMIN 요구");
        return;
    }

    _cleanup_netdevs();
    pcv_qos_ids_clear();
    pcv_qos_set_expected_provider(NULL);                     

    GError *e = NULL;
    if (!pcv_qos_ensure_root(1000, &e)) {
        gchar *msg = g_strdup_printf(
            "ensure_root 실패 — ifb/sch_hfsc/sch_cake 커널 지원 부재로 추정: %s",
            e && e->message ? e->message : "unknown");
        g_test_skip(msg);
        g_free(msg);
        g_clear_error(&e);
        _cleanup_netdevs();
        return;
    }

                                                               
    const gchar *add_class[] = {"tc", "class", "add", "dev", PCV_QOS_IFB_DEV,
        "parent", "1:1", "classid", "1:1234", "hfsc", "ls", "m2", "10Mbit",
        "ul", "m2", "10Mbit", NULL};
    const gchar *add_qdisc[] = {"tc", "qdisc", "add", "dev", PCV_QOS_IFB_DEV,
        "parent", "1:1234", "cake", "besteffort", NULL};
    g_assert_true(_fixture_run(add_class));
    g_assert_true(_fixture_run(add_qdisc));

    {
        gchar *class_out = _tc_show("tc class show dev " PCV_QOS_IFB_DEV);
        g_assert_true(_output_has_classid(class_out, "1:1234"));
        g_free(class_out);
    }

                                                      
                                  
    g_recon_stub_count = 0;
    pcv_qos_set_expected_provider(_recon_stub_provider_empty);

    GError *rec_err = NULL;
    g_assert_true(pcv_qos_reconcile(&rec_err));
    g_assert_no_error(rec_err);

    pcv_qos_set_expected_provider(NULL);           

    {
        gchar *class_out = _tc_show("tc class show dev " PCV_QOS_IFB_DEV);
        g_assert_false(_output_has_classid(class_out, "1:1234"));
        g_free(class_out);
    }

    _cleanup_netdevs();
    pcv_qos_ids_clear();
}

                                                                          
                                                       
                                                      
                                              
                                                
                                                 
                                                            
                               
static void
test_qos_reconcile_missing_reapply_root(void)
{
    if (geteuid() != 0) {
        g_test_skip("root 필요 — tc/ip 실제 적용은 CAP_NET_ADMIN 요구");
        return;
    }

    _cleanup_netdevs();
    pcv_qos_ids_clear();
    pcv_qos_set_expected_provider(NULL);

    _setup_dummy(T2_VM_IFACE1);

    GError *e = NULL;
    if (!pcv_qos_ensure_root(1000, &e)) {
        gchar *msg = g_strdup_printf(
            "ensure_root 실패 — ifb/sch_hfsc/sch_cake 커널 지원 부재로 추정: %s",
            e && e->message ? e->message : "unknown");
        g_test_skip(msg);
        g_free(msg);
        g_clear_error(&e);
        _cleanup_netdevs();
        return;
    }

    PcvQosSla sla = { "acme", "reconvm1", 100, 500, 256 };
    g_assert_true(pcv_qos_apply_vm(T2_VM_IFACE1, &sla, &e));
    g_assert_no_error(e);

    gchar *vm_cid = pcv_qos_classid("acme", "reconvm1");
    guint16 vm_minor = pcv_qos_vm_minor("acme", "reconvm1");

                                                            
                                                              
                                                             
                                                    
             
    {
        gchar *pref = g_strdup_printf("%u", vm_minor);
        gchar *handle = g_strdup_printf("0x%x", vm_minor);
        const gchar *filter_del[] = {"tc", "filter", "del", "dev", PCV_QOS_IFB_DEV,
            "parent", "1:", "pref", pref, "handle", handle, "flower", NULL};
        (void)_fixture_run(filter_del);
        g_free(pref);
        g_free(handle);

        const gchar *cake_del[] = {"tc", "qdisc", "del", "dev", PCV_QOS_IFB_DEV,
            "parent", vm_cid, NULL};
        (void)_fixture_run(cake_del);
        const gchar *class_del[] = {"tc", "class", "del", "dev", PCV_QOS_IFB_DEV,
            "classid", vm_cid, NULL};
        g_assert_true(_fixture_run(class_del));
    }
    {
        gchar *class_out = _tc_show("tc class show dev " PCV_QOS_IFB_DEV);
        g_assert_false(_output_has_classid(class_out, vm_cid));
        g_free(class_out);
    }

                                                           
                                                              
                                   
    PcvQosExpectedEntry stub = {0};
    g_strlcpy(stub.tenant, "acme", sizeof stub.tenant);
    g_strlcpy(stub.vm, "reconvm1", sizeof stub.vm);
    g_strlcpy(stub.iface, T2_VM_IFACE1, sizeof stub.iface);
    stub.sla = sla;
    g_recon_stub_entry = stub;
    g_recon_stub_count = 1;
    pcv_qos_set_expected_provider(_recon_stub_provider);

    GError *rec_err = NULL;
    g_assert_true(pcv_qos_reconcile(&rec_err));
    g_assert_no_error(rec_err);

    pcv_qos_set_expected_provider(NULL);
    g_recon_stub_count = 0;

    {
        gchar *class_out = _tc_show("tc class show dev " PCV_QOS_IFB_DEV);
        g_assert_true(_output_has_classid(class_out, vm_cid));
        g_free(class_out);
    }
    {
        gchar *qdisc_out = _tc_show("tc qdisc show dev " PCV_QOS_IFB_DEV);
        g_assert_nonnull(g_strstr_len(qdisc_out, -1, "cake"));
        g_free(qdisc_out);
    }

    g_free(vm_cid);
    g_assert_true(pcv_qos_remove_vm(T2_VM_IFACE1, "acme", "reconvm1", &e));
    g_assert_no_error(e);

    _cleanup_netdevs();
    pcv_qos_ids_clear();
}

                                                                      
                                                                 
                                                          
                                                           
                                                         
                                                 
                                                           
                                                           
                                                    
                                                             
                                     
static void
test_qos_vm_gone_cleanup_root(void)
{
    if (geteuid() != 0) {
        g_test_skip("root 필요 — tc/ip 실제 적용은 CAP_NET_ADMIN 요구");
        return;
    }

    _cleanup_netdevs();
    pcv_qos_ids_clear();

    _setup_dummy(T2_VM_IFACE1);

    GError *e = NULL;
    if (!pcv_qos_ensure_root(1000, &e)) {
        gchar *msg = g_strdup_printf(
            "ensure_root 실패 — ifb/sch_hfsc/sch_cake 커널 지원 부재로 추정: %s",
            e && e->message ? e->message : "unknown");
        g_test_skip(msg);
        g_free(msg);
        g_clear_error(&e);
        _cleanup_netdevs();
        return;
    }

    PcvQosSla sla = { "gonetenant", "gonevm1", 10, 50, 256 };
    g_assert_true(pcv_qos_apply_vm(T2_VM_IFACE1, &sla, &e));
    g_assert_no_error(e);

    gchar *tenant_cid = pcv_qos_tenant_classid("gonetenant");
    gchar *vm_cid = pcv_qos_classid("gonetenant", "gonevm1");

                                                          
                                                     
    gchar *recovered_tenant = NULL, *recovered_iface = NULL;
    g_assert_true(pcv_qos_lookup_applied("gonevm1", &recovered_tenant, &recovered_iface));
    g_assert_cmpstr(recovered_tenant, ==, "gonetenant");
    g_assert_cmpstr(recovered_iface, ==, T2_VM_IFACE1);

    g_assert_true(pcv_qos_remove_vm(recovered_iface, recovered_tenant, "gonevm1", &e));
    g_assert_no_error(e);

    {
        gchar *t = NULL, *i = NULL;
        g_assert_false(pcv_qos_lookup_applied("gonevm1", &t, &i));
        g_assert_null(t);
        g_assert_null(i);
    }
    {
        gchar *class_out = _tc_show("tc class show dev " PCV_QOS_IFB_DEV);
        g_assert_false(_output_has_classid(class_out, vm_cid));
        g_assert_false(_output_has_classid(class_out, tenant_cid));                          
        g_free(class_out);
    }

    g_free(recovered_tenant);
    g_free(recovered_iface);
    g_free(tenant_cid);
    g_free(vm_cid);

    _cleanup_netdevs();
    pcv_qos_ids_clear();
}

                                                         
                                                               
                                                   
  
                                                               
                                                           
                                               
                                                               
                                                        
                                                              
                                                     
                                                     
                                                                  
                                                             
                  
static void
test_qos_remove_vm_postcondition_failure_root(void)
{
    if (geteuid() != 0) {
        g_test_skip("root 필요 — tc/ip 실제 적용은 CAP_NET_ADMIN 요구");
        return;
    }

    _cleanup_netdevs();
    pcv_qos_ids_clear();

    _setup_dummy(T2_VM_IFACE1);

    GError *e = NULL;
    if (!pcv_qos_ensure_root(1000, &e)) {
        gchar *msg = g_strdup_printf(
            "ensure_root 실패 — ifb/sch_hfsc/sch_cake 커널 지원 부재로 추정: %s",
            e && e->message ? e->message : "unknown");
        g_test_skip(msg);
        g_free(msg);
        g_clear_error(&e);
        _cleanup_netdevs();
        return;
    }

    PcvQosSla sla = { "acme", "postcondvm1", 100, 500, 256 };
    g_assert_true(pcv_qos_apply_vm(T2_VM_IFACE1, &sla, &e));
    g_assert_no_error(e);

    gchar *vm_cid = pcv_qos_classid("acme", "postcondvm1");
    guint16 vm_minor = pcv_qos_vm_minor("acme", "postcondvm1");
    guint32 foreign_pref = (guint32)vm_minor + 1;                                                   

    {
        gchar *pref = g_strdup_printf("%u", foreign_pref);
        gchar *handle = g_strdup_printf("0x%x", foreign_pref);
        const gchar *argv[] = {"tc", "filter", "add", "dev", PCV_QOS_IFB_DEV,
            "parent", "1:", "protocol", "all", "pref", pref, "handle", handle,
            "flower", "indev", T2_VM_IFACE1, "classid", vm_cid, NULL};
        gboolean installed = _fixture_run(argv);
        g_free(pref);
        g_free(handle);
        g_assert_true(installed);
    }

    GError *rerr = NULL;
    gboolean removed = pcv_qos_remove_vm(T2_VM_IFACE1, "acme", "postcondvm1", &rerr);
    g_assert_false(removed);
    g_assert_error(rerr, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(g_strstr_len(rerr->message, -1, "사후조건"));
    g_clear_error(&rerr);

                                                          
                                  
    {
        gchar *class_out = _tc_show("tc class show dev " PCV_QOS_IFB_DEV);
        g_assert_true(_output_has_classid(class_out, vm_cid));
        g_free(class_out);
    }

                                                        
                                                
                       
    {
        gchar *t = NULL, *i = NULL;
        g_assert_false(pcv_qos_lookup_applied("postcondvm1", &t, &i));
        g_assert_null(t);
        g_assert_null(i);
    }

    g_free(vm_cid);
                                                          
                                                       
    _cleanup_netdevs();
    pcv_qos_ids_clear();
}

void
test_qos_integration_register(void)
{
    g_test_add_func("/qos/ifb_lifecycle_root", test_qos_ifb_lifecycle_root);
    g_test_add_func("/qos/tenant_sla_live_update_root", test_qos_tenant_sla_live_update_root);
    g_test_add_func("/qos/chaos_inject_expire_root", test_qos_chaos_inject_expire_root);
    g_test_add_func("/qos/chaos_stop_root", test_qos_chaos_stop_root);
    g_test_add_func("/qos/chaos_purge_all_root", test_qos_chaos_purge_all_root);
    g_test_add_func("/qos/reconcile_orphan_cleanup", test_qos_reconcile_orphan_cleanup_root);
    g_test_add_func("/qos/reconcile_missing_reapply", test_qos_reconcile_missing_reapply_root);
    g_test_add_func("/qos/vm_gone_cleanup", test_qos_vm_gone_cleanup_root);
    g_test_add_func("/qos/remove_vm_postcondition_failure_root", test_qos_remove_vm_postcondition_failure_root);
}
