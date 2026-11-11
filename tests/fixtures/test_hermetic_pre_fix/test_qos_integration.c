#include <glib.h>

#define T2_VM_IFACE1 "pcvqosdum0"
#define T2_VM_IFACE2 "pcvqosdum1"

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

static gchar *
_tc_show(const gchar *cmd)
{
    gchar *out = NULL;
    g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL);
    return out ? out : g_strdup("");
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

void test_qos_integration_pre_fix_register(void) {
    g_test_add_func("/qos/tenant_sla_live_update_root", test_qos_tenant_sla_live_update_root);
}
