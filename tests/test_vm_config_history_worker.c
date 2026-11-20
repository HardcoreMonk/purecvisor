#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../src/modules/core/vm_config_history_store.h"
#include "../src/modules/daemons/vm_config_history.h"

typedef struct {
    gchar   *xml;
    gchar   *live;
    gboolean persistent;
    gint64   domain_id;
} FakeDom;

typedef struct {
    GMutex      mu;
    GHashTable *doms;
    gboolean    gate;
    gboolean    in_read;
    GCond       cond;
    gchar      *tenant_fail_owner;
    gint        after_shutdown;
    gint        calls_after;
} FakeSrc;

static FakeSrc g_fake;

static gchar *
dom_xml(const gchar *name, const gchar *uuid, gint vcpus, const gchar *owner, const gchar *extra)
{
    return g_strdup_printf("<domain type='kvm'><name>%s</name><uuid>%s</uuid><metadata>"
                           "<pcv:owner xmlns:pcv='urn:purecvisor:metadata'>%s</pcv:owner></metadata>"
                           "<memory unit='KiB'>1048576</memory><vcpu>%d</vcpu><devices>"
                           "<disk type='file' device='disk'><source file='/var/lib/pcv/%s.qcow2'/><target dev='vda' bus='virtio'/></disk>"
                           "%s</devices></domain>",
                           name, uuid, owner, vcpus, name, extra ? extra : "");
}

static void
fake_set(const gchar *uuid, gchar *xml, gchar *live, gboolean persistent, gint64 domain_id)
{
    g_mutex_lock(&g_fake.mu);
    FakeDom *d = g_new0(FakeDom, 1);
    d->xml = xml;
    d->live = live;
    d->persistent = persistent;
    d->domain_id = domain_id;
    g_hash_table_replace(g_fake.doms, g_strdup(uuid), d);
    g_mutex_unlock(&g_fake.mu);
}

static void
fake_remove(const gchar *uuid)
{
    g_mutex_lock(&g_fake.mu);
    g_hash_table_remove(g_fake.doms, uuid);
    g_mutex_unlock(&g_fake.mu);
}

static void
fake_dom_free(gpointer p)
{
    FakeDom *d = p;
    g_free(d->xml);
    g_free(d->live);
    g_free(d);
}

static GPtrArray *
f_list(gpointer data, GError **err)
{
    (void)data;
    (void)err;
    if (g_atomic_int_get(&g_fake.after_shutdown))
        g_atomic_int_inc(&g_fake.calls_after);
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    g_mutex_lock(&g_fake.mu);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_fake.doms);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_ptr_array_add(out, g_strdup(k));
    g_mutex_unlock(&g_fake.mu);
    return out;
}

static gboolean
f_read(gpointer data, const gchar *uuid, PcvVmCfgRaw *out, gboolean *exists, GError **err)
{
    (void)data;
    (void)err;
    g_mutex_lock(&g_fake.mu);
    g_fake.in_read = TRUE;
    g_cond_broadcast(&g_fake.cond);
    while (g_fake.gate)
        g_cond_wait(&g_fake.cond, &g_fake.mu);
    FakeDom *d = g_hash_table_lookup(g_fake.doms, uuid);
    *exists = d != NULL;
    if (d) {
        out->inactive_xml = g_strdup(d->xml);
        out->live_xml = g_strdup(d->live);
        out->persistent = d->persistent;
        out->domain_id = d->domain_id;
        out->name = g_strdup("fake");
    }
    g_fake.in_read = FALSE;
    g_mutex_unlock(&g_fake.mu);
    return TRUE;
}

static gchar *
f_resolve(gpointer data, const gchar *name_or_uuid)
{
    (void)data;
    if (g_atomic_int_get(&g_fake.after_shutdown))
        g_atomic_int_inc(&g_fake.calls_after);
    gchar *out = NULL;
    g_mutex_lock(&g_fake.mu);
    if (g_hash_table_contains(g_fake.doms, name_or_uuid)) {
        out = g_strdup(name_or_uuid);
    } else {
        gchar *needle = g_strdup_printf("<name>%s</name>", name_or_uuid);
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, g_fake.doms);
        while (!out && g_hash_table_iter_next(&it, &k, &v)) {
            if (strstr(((FakeDom *)v)->xml, needle))
                out = g_strdup(k);
        }
        g_free(needle);
    }
    g_mutex_unlock(&g_fake.mu);
    return out;
}

static gchar *
f_tenant(gpointer data, const gchar *owner)
{
    (void)data;
    if (g_atomic_int_get(&g_fake.after_shutdown))
        g_atomic_int_inc(&g_fake.calls_after);
    if (!g_strcmp0(owner, g_fake.tenant_fail_owner))
        return NULL;
    return g_strdup("t1");
}

static const PcvVmCfgHistSource k_fake = {.list_domains = f_list, .read = f_read, .resolve = f_resolve,
                                          .owner_tenant = f_tenant};

typedef struct {
    gchar *dir;
    gchar *db;
} Env;

static PcvVmCfgHistConfig
cfg_for(const Env *e)
{
    PcvVmCfgHistConfig c = {.db_path = e->db, .enabled = TRUE, .retention_days = 365, .max_db_mb = 128,
                            .actor_retention_days = 30, .reconcile_interval_s = 0, .debounce_ms = 150,
                            .undefine_grace_ms = 300, .coalesce_ms = 1500, .settle_ms = 400,
                            .shutdown_timeout_ms = 3000, .heartbeat_s = 300, .prune_interval_s = 3600,
                            .pending_max = 256};
    return c;
}

static void
env_init(Env *e)
{
    g_mutex_init(&g_fake.mu);
    g_cond_init(&g_fake.cond);
    g_fake.doms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, fake_dom_free);
    g_fake.gate = FALSE;
    g_fake.tenant_fail_owner = NULL;
    g_atomic_int_set(&g_fake.after_shutdown, 0);
    g_atomic_int_set(&g_fake.calls_after, 0);
    e->dir = g_dir_make_tmp("pcv-vmch-XXXXXX", NULL);
    e->db = g_build_filename(e->dir, "vm_config_history.db", NULL);
}

static void
env_clear(Env *e)
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
    g_hash_table_unref(g_fake.doms);
    g_mutex_clear(&g_fake.mu);
    g_cond_clear(&g_fake.cond);
}

static gint
by_id_desc(gconstpointer a, gconstpointer b)
{
    const PcvVmCfgRow *x = *(PcvVmCfgRow *const *)a, *y = *(PcvVmCfgRow *const *)b;
    return (x->id < y->id) - (x->id > y->id);
}

static GPtrArray *
rows_of(const Env *e, const gchar *uuid)
{
    PcvVmCfgStore *ro = pcv_vm_cfg_store_open(e->db, FALSE, NULL);
    g_assert_nonnull(ro);
    PcvVmCfgListFilter f = {.since = 0, .until = G_MAXINT64, .uuid = uuid};
    GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)pcv_vm_cfg_row_free);
    gboolean more = FALSE;
    g_assert_true(pcv_vm_cfg_store_list(ro, &f, 1000, NULL, NULL, rows, &more, NULL));
    pcv_vm_cfg_store_close(ro);
    g_ptr_array_sort(rows, by_id_desc);
    return rows;
}

static GArray *
epochs_of(const Env *e)
{
    PcvVmCfgStore *ro = pcv_vm_cfg_store_open(e->db, FALSE, NULL);
    g_assert_nonnull(ro);
    GArray *a = pcv_vm_cfg_store_epochs(ro, 0, G_MAXINT64);
    pcv_vm_cfg_store_close(ro);
    return a;
}

static void
start(const Env *e, PcvVmCfgHistConfig *c)
{
    g_assert_true(pcv_vm_cfg_hist_start_with(c, &k_fake, NULL));
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
}

#define U1 "11111111-1111-4111-8111-111111111111"
#define U2 "22222222-2222-4222-8222-222222222222"
#define U3 "33333333-3333-4333-8333-333333333333"

static void
test_worker_first_boot_baselines(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("a", U1, 2, "alice", NULL), NULL, TRUE, -1);
    fake_set(U2, dom_xml("b", U2, 2, "bob", NULL), NULL, TRUE, -1);
    fake_set(U3, dom_xml("c", U3, 2, "carol", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    start(&e, &c);
    GPtrArray *rows = rows_of(&e, NULL);
    g_assert_cmpuint(rows->len, ==, 3);
    for (guint i = 0; i < rows->len; i++) {
        PcvVmCfgRow *r = g_ptr_array_index(rows, i);
        g_assert_cmpstr(r->kind, ==, "baseline");
        g_assert_cmpstr(r->cause, ==, "startup");
        g_assert_cmpint(r->window_start, ==, 0);
        g_assert_cmpstr(r->attribution, ==, "none");
        g_assert_cmpstr(r->owner_tenant, ==, "t1");
    }
    g_ptr_array_unref(rows);
    PcvVmCfgHistStatus st;
    pcv_vm_cfg_hist_status(&st);
    g_assert_cmpstr(st.state, ==, "ok");
    pcv_vm_cfg_hist_status_clear(&st);
    pcv_vm_cfg_hist_shutdown();
    start(&e, &c);
    rows = rows_of(&e, NULL);
    g_assert_cmpuint(rows->len, ==, 3);
    g_ptr_array_unref(rows);
    GArray *eps = epochs_of(&e);
    g_assert_cmpuint(eps->len, ==, 2);
    g_assert_cmpstr(g_array_index(eps, PcvVmCfgEpoch, 0).end_reason, ==, "shutdown");
    g_array_unref(eps);
    env_clear(&e);
}

static void
test_worker_debounce_merges(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("a", U1, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    start(&e, &c);
    guint64 reads = pcv_vm_cfg_hist_test_counter("source_reads");
    fake_set(U1, dom_xml("a", U1, 4, "alice", NULL), NULL, TRUE, -1);
    for (gint i = 0; i < 5; i++)
        pcv_vm_cfg_hist_note_uuid(U1, i % 2 ? "device_changed" : "libvirt_defined");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    g_assert_cmpuint(pcv_vm_cfg_hist_test_counter("source_reads") - reads, ==, 1);
    GPtrArray *rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 2);
    PcvVmCfgRow *r = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(r->kind, ==, "changed");
    g_assert_cmpstr(r->cause, ==, "libvirt_defined");
    g_assert_nonnull(strstr(r->changes_json, "\"path\":\"vcpu.max\""));
    g_assert_cmpint(r->prev_id, ==, ((PcvVmCfgRow *)g_ptr_array_index(rows, 1))->id);
    g_ptr_array_unref(rows);
    env_clear(&e);
}

static void
test_worker_coalesce_after_30_rows(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("a", U1, 1, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    c.debounce_ms = 20;
    start(&e, &c);
    for (gint i = 2; i <= 32; i++) {
        fake_set(U1, dom_xml("a", U1, i, "alice", NULL), NULL, TRUE, -1);
        pcv_vm_cfg_hist_note_uuid(U1, "rpc");
        g_assert_true(pcv_vm_cfg_hist_sync(5000));
    }
    guint64 before = pcv_vm_cfg_hist_test_counter("coalesced");
    fake_set(U1, dom_xml("a", U1, 64, "alice", NULL), NULL, TRUE, -1);
    pcv_vm_cfg_hist_note_uuid(U1, "rpc");
    g_usleep(400000);
    GPtrArray *rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 32);
    g_ptr_array_unref(rows);
    g_assert_cmpuint(pcv_vm_cfg_hist_test_counter("coalesced") - before, ==, 1);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 33);
    g_ptr_array_unref(rows);
    env_clear(&e);
}

static void
test_worker_undefine_grace_rename(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("old", U1, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    start(&e, &c);
    fake_remove(U1);
    pcv_vm_cfg_hist_note_undefined(U1);
    g_usleep(100000);
    fake_set(U1, dom_xml("new", U1, 2, "alice", NULL), NULL, TRUE, -1);
    pcv_vm_cfg_hist_note_uuid(U1, "libvirt_defined");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    GPtrArray *rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 2);
    PcvVmCfgRow *r = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(r->kind, ==, "changed");
    g_assert_cmpstr(r->vm_name, ==, "new");
    g_assert_nonnull(strstr(r->changes_json, "\"path\":\"name\""));
    g_ptr_array_unref(rows);

    fake_remove(U1);
    pcv_vm_cfg_hist_note_undefined(U1);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 3);
    g_assert_cmpstr(((PcvVmCfgRow *)g_ptr_array_index(rows, 0))->kind, ==, "undefined");
    g_ptr_array_unref(rows);
    env_clear(&e);
}

static void
test_worker_undefine_transient(void)
{
    Env e;
    env_init(&e);
    gchar *x = dom_xml("t", U1, 2, "alice", NULL);
    fake_set(U1, g_strdup(x), g_strdup(x), TRUE, 5);
    PcvVmCfgHistConfig c = cfg_for(&e);
    start(&e, &c);
    fake_set(U1, g_strdup(x), g_strdup(x), FALSE, 5);
    pcv_vm_cfg_hist_note_undefined(U1);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    GPtrArray *rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 2);
    PcvVmCfgRow *r = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(r->kind, ==, "undefined");
    g_assert_nonnull(strstr(r->snapshot_json, "\"transient\":true"));
    g_ptr_array_unref(rows);
    pcv_vm_cfg_hist_note_uuid(U1, "reconcile");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 2);
    g_ptr_array_unref(rows);
    g_free(x);
    env_clear(&e);
}

static void
test_worker_settle_no_row(void)
{
    Env e;
    env_init(&e);
    gchar *x = dom_xml("s", U1, 2, "alice", NULL);
    gchar *pinned = dom_xml("s", U1, 2, "alice", NULL);
    gchar *tmp = pinned;
    pinned = g_strconcat("<!-- live -->", tmp, NULL);
    g_free(tmp);
    fake_set(U1, g_strdup(x), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    start(&e, &c);
    fake_set(U1, g_strdup(x), g_strdup(x), TRUE, 41);
    pcv_vm_cfg_hist_note_started(U1);
    g_usleep(200000);
    gchar *live_pin = g_strdup_printf("<domain type='kvm'><name>s</name><uuid>%s</uuid><vcpu>2</vcpu>"
                                      "<cputune><vcpupin vcpu='0' cpuset='9'/></cputune>"
                                      "<memory unit='KiB'>1048576</memory></domain>", U1);
    fake_set(U1, g_strdup(x), g_strdup(live_pin), TRUE, 41);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    fake_set(U1, g_strdup(x), NULL, TRUE, -1);
    pcv_vm_cfg_hist_note_uuid(U1, "reconcile");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    GPtrArray *rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 1);
    g_ptr_array_unref(rows);
    fake_set(U1, g_strdup(x), g_strdup(x), TRUE, 42);
    pcv_vm_cfg_hist_note_started(U1);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    fake_set(U1, g_strdup(x), g_strdup(live_pin), TRUE, 42);
    pcv_vm_cfg_hist_note_uuid(U1, "rpc");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 2);
    PcvVmCfgRow *r = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(r->scope, ==, "live");
    g_assert_nonnull(strstr(r->changes_json, "live."));
    g_ptr_array_unref(rows);
    g_free(live_pin);
    g_free(pinned);
    g_free(x);
    env_clear(&e);
}

static void
test_worker_reconcile_window(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("r", U1, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    c.reconcile_interval_s = 1;
    start(&e, &c);
    GPtrArray *rows = rows_of(&e, U1);
    gint64 base_at = ((PcvVmCfgRow *)g_ptr_array_index(rows, 0))->observed_at;
    g_ptr_array_unref(rows);
    fake_set(U1, dom_xml("r", U1, 3, "alice", NULL), NULL, TRUE, -1);
    g_usleep(1600000);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 2);
    PcvVmCfgRow *r = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(r->cause, ==, "reconcile");
    g_assert_cmpint(r->window_start, >=, base_at);
    g_assert_cmpint(r->window_start, <, r->observed_at);
    g_assert_cmpstr(r->attribution, ==, "none");
    g_ptr_array_unref(rows);
    env_clear(&e);
}

static void
test_worker_crash_inferred(void)
{
    Env e;
    env_init(&e);
    PcvVmCfgStore *st = pcv_vm_cfg_store_open(e.db, TRUE, NULL);
    gint64 ep = pcv_vm_cfg_store_epoch_begin(st, g_get_real_time() / 1000 - 60000, NULL);
    pcv_vm_cfg_store_epoch_heartbeat(st, ep, g_get_real_time() / 1000 - 30000, NULL);
    pcv_vm_cfg_store_close(st);
    pcv_vm_cfg_store_close(pcv_vm_cfg_store_open(e.db, TRUE, NULL));

    fake_set(U1, dom_xml("k", U1, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    start(&e, &c);
    GPtrArray *rows = rows_of(&e, U1);
    g_assert_cmpstr(((PcvVmCfgRow *)g_ptr_array_index(rows, 0))->kind, ==, "created");
    g_ptr_array_unref(rows);
    GArray *eps = epochs_of(&e);
    g_assert_cmpuint(eps->len, ==, 2);
    PcvVmCfgEpoch *first = &g_array_index(eps, PcvVmCfgEpoch, 0);
    g_assert_cmpstr(first->end_reason, ==, "crash_inferred");
    g_assert_cmpint(first->ended_at, ==, first->last_alive_at);
    gint64 crash_end = first->ended_at;
    g_array_unref(eps);
    rows = rows_of(&e, U1);
    g_assert_cmpint(((PcvVmCfgRow *)g_ptr_array_index(rows, 0))->window_start, ==, crash_end);
    g_ptr_array_unref(rows);
    pcv_vm_cfg_hist_shutdown();
    fake_set(U1, dom_xml("k", U1, 8, "alice", NULL), NULL, TRUE, -1);
    start(&e, &c);
    rows = rows_of(&e, U1);
    PcvVmCfgRow *r = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(r->kind, ==, "changed");
    g_assert_cmpstr(r->cause, ==, "startup");
    g_assert_cmpint(r->window_start, >, 0);
    g_ptr_array_unref(rows);
    env_clear(&e);
}

static void
test_worker_shutdown_closes_epoch(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("z", U1, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    start(&e, &c);
    pcv_vm_cfg_hist_shutdown();
    GArray *eps = epochs_of(&e);
    g_assert_cmpuint(eps->len, ==, 1);
    g_assert_cmpint(g_array_index(eps, PcvVmCfgEpoch, 0).ended_at, >, 0);
    g_assert_cmpstr(g_array_index(eps, PcvVmCfgEpoch, 0).end_reason, ==, "shutdown");
    g_array_unref(eps);
    env_clear(&e);
}

static void
scan_batch_changes(void (*apply)(void))
{
    g_mutex_lock(&g_fake.mu);
    g_fake.gate = TRUE;
    g_fake.in_read = FALSE;
    gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (!g_fake.in_read && g_cond_wait_until(&g_fake.cond, &g_fake.mu, end))
        ;
    g_assert_true(g_fake.in_read);
    g_mutex_unlock(&g_fake.mu);
    apply();
    g_mutex_lock(&g_fake.mu);
    g_fake.gate = FALSE;
    g_cond_broadcast(&g_fake.cond);
    g_mutex_unlock(&g_fake.mu);
}

static void
apply_abc3(void)
{
    fake_set(U1, dom_xml("a", U1, 3, "alice", NULL), NULL, TRUE, -1);
    fake_set(U2, dom_xml("b", U2, 3, "alice", NULL), NULL, TRUE, -1);
    fake_set(U3, dom_xml("c", U3, 3, "alice", NULL), NULL, TRUE, -1);
}

static void
apply_op3(void)
{
    fake_set(U1, dom_xml("o", U1, 3, "alice", NULL), NULL, TRUE, -1);
    fake_set(U2, dom_xml("p", U2, 3, "alice", NULL), NULL, TRUE, -1);
}

static void
test_worker_store_write_fail_degraded(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("a", U1, 2, "alice", NULL), NULL, TRUE, -1);
    fake_set(U2, dom_xml("b", U2, 2, "alice", NULL), NULL, TRUE, -1);
    fake_set(U3, dom_xml("c", U3, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    c.pending_max = 2;
    c.reconcile_interval_s = 1;
    start(&e, &c);
    pcv_vm_cfg_hist_test_fail_writes(TRUE);
    scan_batch_changes(apply_abc3);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    PcvVmCfgHistStatus st;
    pcv_vm_cfg_hist_status(&st);
    g_assert_cmpstr(st.state, ==, "degraded");
    pcv_vm_cfg_hist_status_clear(&st);
    g_assert_cmpuint(pcv_vm_cfg_hist_test_counter("write_failures"), >=, 3);
    GArray *eps = epochs_of(&e);
    g_assert_cmpstr(g_array_index(eps, PcvVmCfgEpoch, 0).end_reason, ==, "store_write_failed");
    g_array_unref(eps);
    pcv_vm_cfg_hist_test_fail_writes(FALSE);
    g_usleep(1600000);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    pcv_vm_cfg_hist_status(&st);
    g_assert_cmpstr(st.state, ==, "ok");
    pcv_vm_cfg_hist_status_clear(&st);
    eps = epochs_of(&e);
    g_assert_cmpuint(eps->len, ==, 2);
    g_array_unref(eps);
    GPtrArray *rows = rows_of(&e, NULL);
    g_assert_cmpuint(rows->len, ==, 6);
    g_ptr_array_unref(rows);
    env_clear(&e);
}

static void
test_worker_degraded_no_duplicate_rows(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("d", U1, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    c.reconcile_interval_s = 1;
    start(&e, &c);
    pcv_vm_cfg_hist_test_fail_writes(TRUE);
    fake_set(U1, dom_xml("d", U1, 3, "alice", NULL), NULL, TRUE, -1);
    pcv_vm_cfg_hist_note_uuid(U1, "rpc");
    g_usleep(3500000);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    pcv_vm_cfg_hist_test_fail_writes(FALSE);
    g_usleep(4500000);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    GPtrArray *rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 2);
    g_ptr_array_unref(rows);
    env_clear(&e);
}

static void
test_worker_overflow_closes_epoch_id(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("o", U1, 2, "alice", NULL), NULL, TRUE, -1);
    fake_set(U2, dom_xml("p", U2, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    c.pending_max = 1;
    c.reconcile_interval_s = 1;
    start(&e, &c);
    pcv_vm_cfg_hist_test_fail_writes(TRUE);
    scan_batch_changes(apply_op3);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    GArray *eps = epochs_of(&e);
    g_assert_cmpuint(eps->len, ==, 1);
    gint64 closed_id = g_array_index(eps, PcvVmCfgEpoch, 0).id;
    gint64 closed_end = g_array_index(eps, PcvVmCfgEpoch, 0).ended_at;
    g_assert_cmpstr(g_array_index(eps, PcvVmCfgEpoch, 0).end_reason, ==, "store_write_failed");
    g_array_unref(eps);
    g_usleep(20000);
    pcv_vm_cfg_hist_intent_open("vm.resize_disk", "o", "alice", "t1", "vda", (gint64)20 << 30);
    pcv_vm_cfg_hist_intent_close("vm.resize_disk", "alice", "o", "ok");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    pcv_vm_cfg_hist_test_fail_writes(FALSE);
    g_usleep(1600000);
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    GPtrArray *rows = rows_of(&e, NULL);
    for (guint i = 0; i < rows->len; i++) {
        PcvVmCfgRow *r = g_ptr_array_index(rows, i);
        if (r->epoch_id == closed_id)
            g_assert_cmpint(r->observed_at, <=, closed_end);
    }
    g_ptr_array_unref(rows);
    eps = epochs_of(&e);
    g_assert_cmpuint(eps->len, ==, 2);
    g_array_unref(eps);
    env_clear(&e);
}

static void
test_worker_tenant_lookup_fail(void)
{
    Env e;
    env_init(&e);
    g_fake.tenant_fail_owner = "ghost";
    fake_set(U1, dom_xml("g", U1, 2, "ghost", NULL), NULL, TRUE, -1);
    guint64 before = pcv_vm_cfg_hist_test_counter("tenant_unknown");
    PcvVmCfgHistConfig c = cfg_for(&e);
    start(&e, &c);
    GPtrArray *rows = rows_of(&e, U1);
    PcvVmCfgRow *r = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(r->owner, ==, "ghost");
    g_assert_null(r->owner_tenant);
    g_ptr_array_unref(rows);
    g_assert_cmpuint(pcv_vm_cfg_hist_test_counter("tenant_unknown") - before, ==, 1);
    env_clear(&e);
}

static void
test_worker_resize_annotation(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("disky", U1, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    start(&e, &c);
    pcv_vm_cfg_hist_intent_open("vm.resize_disk", "disky", "alice", "t1", "vda", 21474836480LL);
    pcv_vm_cfg_hist_intent_close("vm.resize_disk", "alice", "disky", "ok");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    pcv_vm_cfg_hist_intent_open("vm.resize_disk", "disky", "alice", "t1", "vda", 32212254720LL);
    pcv_vm_cfg_hist_intent_close("vm.resize_disk", "alice", "disky", "ok");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    GPtrArray *rows = rows_of(&e, U1);
    g_assert_cmpuint(rows->len, ==, 3);
    PcvVmCfgRow *latest = g_ptr_array_index(rows, 0), *first = g_ptr_array_index(rows, 1);
    PcvVmCfgRow *base = g_ptr_array_index(rows, 2);
    g_assert_cmpstr(first->kind, ==, "resized");
    g_assert_cmpstr(first->changes_json, ==,
                    "[{\"after\":21474836480,\"before\":null,\"path\":\"disk[vda].requested_size_bytes\"}]");
    g_assert_cmpstr(latest->changes_json, ==,
                    "[{\"after\":32212254720,\"before\":21474836480,\"path\":\"disk[vda].requested_size_bytes\"}]");
    g_assert_cmpstr(latest->actor, ==, "alice");
    g_assert_cmpstr(latest->attribution, ==, "rpc");
    g_assert_cmpstr(latest->snapshot_json, ==, base->snapshot_json);
    g_assert_cmpstr(latest->config_digest, ==, base->config_digest);
    g_ptr_array_unref(rows);
    env_clear(&e);
}

static void
test_worker_intent_attribution(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("w", U1, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    start(&e, &c);
    pcv_vm_cfg_hist_intent_open("vm.set_vcpu", "w", "alice", "t1", NULL, 0);
    fake_set(U1, dom_xml("w", U1, 4, "alice", NULL), NULL, TRUE, -1);
    pcv_vm_cfg_hist_note_uuid(U1, "libvirt_defined");
    pcv_vm_cfg_hist_intent_close("vm.set_vcpu", "alice", "w", "ok");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    GPtrArray *rows = rows_of(&e, U1);
    PcvVmCfgRow *r = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(r->attribution, ==, "rpc");
    g_assert_cmpstr(r->actor, ==, "alice");
    g_assert_cmpstr(r->actor_tenant, ==, "t1");
    g_assert_cmpstr(r->actor_method, ==, "vm.set_vcpu");
    g_ptr_array_unref(rows);
    pcv_vm_cfg_hist_intent_open("vm.set_vcpu", "w", "mallory", NULL, NULL, 0);
    pcv_vm_cfg_hist_intent_close("vm.set_vcpu", "mallory", "w", "denied");
    fake_set(U1, dom_xml("w", U1, 6, "alice", NULL), NULL, TRUE, -1);
    pcv_vm_cfg_hist_note_uuid(U1, "libvirt_defined");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    rows = rows_of(&e, U1);
    r = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(r->attribution, ==, "none");
    g_assert_null(r->actor);
    g_ptr_array_unref(rows);

    pcv_vm_cfg_hist_intent_open("vm.autostart", "w", "carol", "t7", NULL, 0);
    pcv_vm_cfg_hist_intent_close("vm.autostart", "carol", "w", "ok");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    rows = rows_of(&e, U1);
    r = g_ptr_array_index(rows, 0);
    g_assert_cmpstr(r->actor, ==, "carol");
    g_assert_cmpstr(r->actor_tenant, ==, "t7");
    g_ptr_array_unref(rows);
    env_clear(&e);
}

static void
test_worker_shutdown_bounded(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("h", U1, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    c.shutdown_timeout_ms = 300;
    start(&e, &c);
    g_mutex_lock(&g_fake.mu);
    g_fake.gate = TRUE;
    g_mutex_unlock(&g_fake.mu);
    pcv_vm_cfg_hist_note_uuid(U1, "rpc");
    g_mutex_lock(&g_fake.mu);
    gint64 end = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    while (!g_fake.in_read && g_cond_wait_until(&g_fake.cond, &g_fake.mu, end))
        ;
    g_assert_true(g_fake.in_read);
    g_mutex_unlock(&g_fake.mu);

    fake_set(U1, dom_xml("h", U1, 3, "alice", NULL), NULL, TRUE, -1);
    gint64 t0 = g_get_monotonic_time();
    pcv_vm_cfg_hist_shutdown();
    gint64 took_ms = (g_get_monotonic_time() - t0) / 1000;
    g_assert_cmpint(took_ms, <, 1500);

    g_atomic_int_set(&g_fake.after_shutdown, 1);
    g_mutex_lock(&g_fake.mu);
    g_fake.gate = FALSE;
    g_cond_broadcast(&g_fake.cond);
    g_mutex_unlock(&g_fake.mu);
    g_usleep(500000);
    g_assert_cmpint(g_atomic_int_get(&g_fake.calls_after), ==, 0);
    env_clear(&e);
}

static void
test_worker_disabled_and_unavailable(void)
{
    Env e;
    env_init(&e);
    fake_set(U1, dom_xml("d", U1, 2, "alice", NULL), NULL, TRUE, -1);
    PcvVmCfgHistConfig c = cfg_for(&e);
    start(&e, &c);
    pcv_vm_cfg_hist_shutdown();
    c.enabled = FALSE;
    start(&e, &c);
    PcvVmCfgHistStatus st;
    pcv_vm_cfg_hist_status(&st);
    g_assert_cmpstr(st.state, ==, "disabled");
    pcv_vm_cfg_hist_status_clear(&st);
    pcv_vm_cfg_hist_shutdown();
    GArray *eps = epochs_of(&e);
    g_assert_cmpstr(g_array_index(eps, PcvVmCfgEpoch, eps->len - 1).end_reason, ==, "disabled");
    g_array_unref(eps);

    gchar *key = g_build_filename(e.dir, "vm_config_history.key", NULL);
    g_assert_true(g_file_test(key, G_FILE_TEST_EXISTS));
    g_unlink(key);
    start(&e, &c);
    pcv_vm_cfg_hist_shutdown();
    g_assert_false(g_file_test(key, G_FILE_TEST_EXISTS));
    g_free(key);
    c.enabled = TRUE;
    gchar *bad = g_build_filename(e.dir, "missing-dir", "x.db", NULL);
    c.db_path = bad;
    start(&e, &c);
    pcv_vm_cfg_hist_status(&st);
    g_assert_cmpstr(st.state, ==, "unavailable");
    pcv_vm_cfg_hist_status_clear(&st);
    g_free(bad);
    env_clear(&e);
}

void test_vm_config_history_worker_register(void);
void
test_vm_config_history_worker_register(void)
{
    g_test_add_func("/vm_config_history/worker/worker_first_boot_baselines", test_worker_first_boot_baselines);
    g_test_add_func("/vm_config_history/worker/worker_debounce_merges", test_worker_debounce_merges);
    g_test_add_func("/vm_config_history/worker/worker_coalesce_after_30_rows", test_worker_coalesce_after_30_rows);
    g_test_add_func("/vm_config_history/worker/worker_undefine_grace_rename", test_worker_undefine_grace_rename);
    g_test_add_func("/vm_config_history/worker/worker_undefine_transient", test_worker_undefine_transient);
    g_test_add_func("/vm_config_history/worker/worker_settle_no_row", test_worker_settle_no_row);
    g_test_add_func("/vm_config_history/worker/worker_reconcile_window", test_worker_reconcile_window);
    g_test_add_func("/vm_config_history/worker/worker_crash_inferred", test_worker_crash_inferred);
    g_test_add_func("/vm_config_history/worker/worker_shutdown_closes_epoch", test_worker_shutdown_closes_epoch);
    g_test_add_func("/vm_config_history/worker/worker_store_write_fail_degraded", test_worker_store_write_fail_degraded);
    g_test_add_func("/vm_config_history/worker/worker_degraded_no_duplicate_rows", test_worker_degraded_no_duplicate_rows);
    g_test_add_func("/vm_config_history/worker/worker_overflow_closes_epoch_id", test_worker_overflow_closes_epoch_id);
    g_test_add_func("/vm_config_history/worker/worker_tenant_lookup_fail", test_worker_tenant_lookup_fail);
    g_test_add_func("/vm_config_history/worker/worker_resize_annotation", test_worker_resize_annotation);
    g_test_add_func("/vm_config_history/worker/worker_intent_attribution", test_worker_intent_attribution);
    g_test_add_func("/vm_config_history/worker/worker_shutdown_bounded", test_worker_shutdown_bounded);
    g_test_add_func("/vm_config_history/worker/worker_disabled_and_unavailable", test_worker_disabled_and_unavailable);
}
