#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "../src/modules/core/vm_config_history_store.h"
#include "../src/modules/daemons/vm_config_history.h"
#include "../src/modules/daemons/vm_config_history_nudge.h"
#include "../src/api/dispatcher_policy.h"
#include "../src/modules/dispatcher/rpc_completion.h"

#define NU "44444444-4444-4444-8444-444444444444"

static GMutex g_mu;
static gint   g_vcpus = 2;

static gchar *
xml_now(void)
{
    g_mutex_lock(&g_mu);
    gint v = g_vcpus;
    g_mutex_unlock(&g_mu);
    return g_strdup_printf("<domain type='kvm'><name>web</name><uuid>%s</uuid><metadata>"
                           "<pcv:owner xmlns:pcv='urn:purecvisor:metadata'>alice</pcv:owner></metadata>"
                           "<memory unit='KiB'>1048576</memory><vcpu>%d</vcpu></domain>", NU, v);
}

static GPtrArray *
n_list(gpointer d, GError **e)
{
    (void)d;
    (void)e;
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(a, g_strdup(NU));
    return a;
}

static gboolean
n_read(gpointer d, const gchar *uuid, PcvVmCfgRaw *out, gboolean *exists, GError **e)
{
    (void)d;
    (void)e;
    *exists = !g_strcmp0(uuid, NU);
    if (*exists) {
        out->inactive_xml = xml_now();
        out->persistent = TRUE;
        out->domain_id = -1;
        out->name = g_strdup("web");
    }
    return TRUE;
}

static gchar *
n_resolve(gpointer d, const gchar *n)
{
    (void)d;
    return (!g_strcmp0(n, "web") || !g_strcmp0(n, NU)) ? g_strdup(NU) : NULL;
}

static gchar *
n_tenant(gpointer d, const gchar *o)
{
    (void)d;
    (void)o;
    return g_strdup("t1");
}

static const PcvVmCfgHistSource k_src = {.list_domains = n_list, .read = n_read, .resolve = n_resolve,
                                         .owner_tenant = n_tenant};

typedef struct {
    gchar *dir;
    gchar *db;
} Env;

static void
env_start(Env *e)
{
    static gboolean hook_registered;
    if (!hook_registered) {
        pcv_dispatcher_register_authorized_hook(pcv_vm_cfg_nudge_authorized_hook, NULL);
        hook_registered = TRUE;
    }
    g_vcpus = 2;
    e->dir = g_dir_make_tmp("pcv-vmch-XXXXXX", NULL);
    e->db = g_build_filename(e->dir, "vm_config_history.db", NULL);
    PcvVmCfgHistConfig c = {.db_path = e->db, .enabled = TRUE, .retention_days = 365, .max_db_mb = 128,
                            .actor_retention_days = 30, .debounce_ms = 100, .undefine_grace_ms = 300,
                            .coalesce_ms = 1000, .settle_ms = 300, .shutdown_timeout_ms = 3000, .heartbeat_s = 300,
                            .prune_interval_s = 3600, .pending_max = 256};
    g_assert_true(pcv_vm_cfg_hist_start_with(&c, &k_src, NULL));
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
}

static void
env_stop(Env *e)
{
    pcv_vm_cfg_hist_shutdown();
    GDir *d = g_dir_open(e->dir, 0, NULL);
    const gchar *n;
    while (d && (n = g_dir_read_name(d))) {
        gchar *p = g_build_filename(e->dir, n, NULL);
        g_unlink(p);
        g_free(p);
    }
    if (d)
        g_dir_close(d);
    g_rmdir(e->dir);
    g_free(e->dir);
    g_free(e->db);
}

static PcvVmCfgRow *
last_row(const Env *e, guint *count)
{
    PcvVmCfgStore *ro = pcv_vm_cfg_store_open(e->db, FALSE, NULL);
    g_assert_nonnull(ro);
    PcvVmCfgListFilter f = {.since = 0, .until = G_MAXINT64};
    GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)pcv_vm_cfg_row_free);
    gboolean more = FALSE;
    g_assert_true(pcv_vm_cfg_store_list(ro, &f, 100, NULL, NULL, rows, &more, NULL));
    guint best = 0;
    for (guint i = 1; i < rows->len; i++) {
        if (((PcvVmCfgRow *)g_ptr_array_index(rows, i))->id > ((PcvVmCfgRow *)g_ptr_array_index(rows, best))->id)
            best = i;
    }
    *count = rows->len;
    g_assert_cmpuint(rows->len, >, 0);
    PcvVmCfgRow *out = g_ptr_array_steal_index(rows, best);
    g_ptr_array_unref(rows);
    pcv_vm_cfg_store_close(ro);
    return out;
}

static gboolean
dispatch_hook(const gchar *method, const gchar *params_json, const gchar *actor, const gchar *tenant)
{
    JsonParser *p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, params_json, -1, NULL));
    pcv_rpc_completion_begin(method);
    pcv_rpc_completion_set_actor(actor);
    pcv_rpc_completion_set_actor_tenant(tenant);
    pcv_dispatcher_run_authorized_hooks(method, json_node_get_object(json_parser_get_root(p)));
    pcv_rpc_completion_finish();
    g_object_unref(p);
    return TRUE;
}

static void
test_nudge_classify(void)
{
    const PcvVmCfgNudgeEntry *e = NULL;
    g_assert_cmpint(pcv_vm_cfg_nudge_classify("vm.limit", &e), ==, PCV_VMCH_NUDGE_INTENT);
    g_assert_null(e->dev_param);
    g_assert_cmpint(pcv_vm_cfg_nudge_classify("vm.resize_disk", &e), ==, PCV_VMCH_NUDGE_INTENT);
    g_assert_cmpstr(e->dev_param, ==, "target");
    g_assert_cmpstr(e->size_gb_param, ==, "new_size_gb");
    g_assert_cmpint(pcv_vm_cfg_nudge_classify("vm.stop", &e), ==, PCV_VMCH_NUDGE_EXEMPT);
    g_assert_null(e);
    g_assert_cmpint(pcv_vm_cfg_nudge_classify("plugin.custom", NULL), ==, PCV_VMCH_NUDGE_UNKNOWN);
    g_assert_cmpint(pcv_vm_cfg_nudge_classify(NULL, NULL), ==, PCV_VMCH_NUDGE_UNKNOWN);
}

static void
test_nudge_hook_only_enqueues(void)
{
    Env e;
    env_start(&e);
    g_assert_true(dispatch_hook("vm.stop", "{\"name\":\"web\"}", "bob", "t1"));
    g_mutex_lock(&g_mu);
    g_vcpus = 3;
    g_mutex_unlock(&g_mu);
    pcv_vm_cfg_hist_note_uuid(NU, "libvirt_defined");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    guint n = 0;
    PcvVmCfgRow *r = last_row(&e, &n);
    g_assert_cmpstr(r->attribution, ==, "none");
    pcv_vm_cfg_row_free(r);
    g_assert_true(dispatch_hook("vm.set_vcpu", "{\"name\":\"web\",\"vcpus\":4}", "alice", "t1"));
    g_mutex_lock(&g_mu);
    g_vcpus = 4;
    g_mutex_unlock(&g_mu);
    pcv_vm_cfg_hist_note_uuid(NU, "libvirt_defined");
    pcv_vm_cfg_nudge_audit_tap("alice", "vm.set_vcpu", "web", "ok", 0);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    r = last_row(&e, &n);
    g_assert_cmpstr(r->attribution, ==, "rpc");
    g_assert_cmpstr(r->actor, ==, "alice");
    g_assert_cmpstr(r->actor_tenant, ==, "t1");
    g_assert_cmpstr(r->actor_method, ==, "vm.set_vcpu");
    g_assert_cmpstr(r->actor_result, ==, "ok");
    pcv_vm_cfg_row_free(r);
    g_assert_true(dispatch_hook("vm.set_vcpu", "{}", "alice", NULL));
    env_stop(&e);
}

static gpointer
tap_from_thread(gpointer data)
{
    (void)data;
    pcv_vm_cfg_nudge_audit_tap("carol", "vm.limit", "web", "fail", 0);
    return NULL;
}

static void
test_audit_tap_closes_intents(void)
{
    Env e;
    env_start(&e);
    g_assert_true(dispatch_hook("vm.limit", "{\"name\":\"web\",\"cpu_quota\":50}", "carol", "t1"));
    g_mutex_lock(&g_mu);
    g_vcpus = 5;
    g_mutex_unlock(&g_mu);
    GThread *t = g_thread_new("audit-tap", tap_from_thread, NULL);
    g_thread_join(t);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    guint n = 0;
    PcvVmCfgRow *r = last_row(&e, &n);
    g_assert_cmpstr(r->attribution, ==, "rpc");
    g_assert_cmpstr(r->actor, ==, "carol");
    g_assert_cmpstr(r->actor_result, ==, "fail");
    pcv_vm_cfg_row_free(r);
    g_assert_true(dispatch_hook("vm.set_memory", "{\"name\":\"web\"}", "mallory", NULL));
    pcv_vm_cfg_nudge_audit_tap("mallory", "vm.set_memory", "web", "denied", -32006);
    pcv_vm_cfg_nudge_audit_tap("mallory", "plugin.custom", "web", "ok", 0);
    g_mutex_lock(&g_mu);
    g_vcpus = 6;
    g_mutex_unlock(&g_mu);
    pcv_vm_cfg_hist_note_uuid(NU, "reconcile");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    r = last_row(&e, &n);
    g_assert_cmpstr(r->attribution, ==, "none");
    g_assert_null(r->actor);
    pcv_vm_cfg_row_free(r);
    env_stop(&e);
}

static void
test_audit_tap_rejected_codes_drop_intent(void)
{
    static const gint codes[] = {-32602, -32006};
    for (guint k = 0; k < G_N_ELEMENTS(codes); k++) {
        Env e;
        env_start(&e);
        g_assert_true(dispatch_hook("vm.set_vcpu", "{\"name\":\"web\"}", "mallory", "t9"));
        pcv_vm_cfg_nudge_audit_tap("mallory", "vm.set_vcpu", "web", "fail", codes[k]);
        g_mutex_lock(&g_mu);
        g_vcpus = 7 + (gint)k;
        g_mutex_unlock(&g_mu);
        pcv_vm_cfg_hist_note_uuid(NU, "libvirt_defined");
        g_assert_true(pcv_vm_cfg_hist_sync(5000));
        guint n = 0;
        PcvVmCfgRow *r = last_row(&e, &n);
        g_assert_cmpstr(r->attribution, ==, "none");
        g_assert_null(r->actor);
        pcv_vm_cfg_row_free(r);
        env_stop(&e);
    }
}

static void
test_nudge_resize_allowed_params_only(void)
{
    Env e;
    env_start(&e);
    g_assert_true(dispatch_hook("vm.resize_disk",
                                "{\"name\":\"web\",\"target\":\"vda\",\"new_size_gb\":20,"
                                "\"cloud_init_password\":\"S3cr3t-PCV-pw\",\"note\":\"S3cr3t-PCV-note\"}",
                                "alice", "t1"));
    pcv_vm_cfg_nudge_audit_tap("alice", "vm.resize_disk", "web", "ok", 0);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    guint n = 0;
    PcvVmCfgRow *r = last_row(&e, &n);
    g_assert_cmpuint(n, ==, 2);
    g_assert_cmpstr(r->kind, ==, "resized");
    g_assert_nonnull(strstr(r->changes_json, "\"after\":21474836480"));
    g_assert_nonnull(strstr(r->changes_json, "disk[vda].requested_size_bytes"));
    const gchar *fields[] = {r->changes_json, r->snapshot_json, r->actor_method, r->cause, NULL};
    for (guint i = 0; fields[i]; i++)
        g_assert_null(strstr(fields[i], "S3cr3t"));
    pcv_vm_cfg_row_free(r);
    env_stop(&e);
}

void test_vm_config_history_nudge_register(void);
void
test_vm_config_history_nudge_register(void)
{
    g_test_add_func("/vm_config_history/nudge/nudge_classify", test_nudge_classify);
    g_test_add_func("/vm_config_history/nudge/nudge_hook_only_enqueues", test_nudge_hook_only_enqueues);
    g_test_add_func("/vm_config_history/nudge/audit_tap_closes_intents", test_audit_tap_closes_intents);
    g_test_add_func("/vm_config_history/nudge/audit_tap_rejected_codes_drop_intent", test_audit_tap_rejected_codes_drop_intent);
    g_test_add_func("/vm_config_history/nudge/nudge_resize_allowed_params_only", test_nudge_resize_allowed_params_only);
}
