#include <glib.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "../src/modules/network/dpdk_pmd.h"
#include "utils/pcv_config.h"

#define FX "tests/fixtures/dpdk_pmd/"

extern void pcv_dpdk_pmd_test_set_appctl_runner(PcvDpdkPmdTestRunner fn, gpointer user_data);
typedef gboolean (*PcvDpdkTestOvsRunner)(const gchar * const *argv, gchar **stdout_out,
                                         gchar **stderr_out, GError **error, gpointer user_data);
extern void pcv_dpdk_test_set_ovs_runner(PcvDpdkTestOvsRunner runner, gpointer user_data,
                                         gint available_override);

static gchar *
fx_read(const gchar *name)
{
    gchar *path = g_strconcat(FX, name, NULL);
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_free(path);
    return text;
}

static void
rxq_free(gpointer data)
{
    PcvDpdkPmdRxq *rxq = data;
    g_free(rxq->port);
    g_free(rxq);
}

static gboolean
argv_has(const gchar * const *argv, const gchar *token)
{
    for (guint i = 0; argv && argv[i]; i++)
        if (g_strcmp0(argv[i], token) == 0) return TRUE;
    return FALSE;
}

typedef struct {
    const gchar *file;
    guint npmd;
    guint cores[4];
    gint numas[4];
    gboolean isolated[4];
    guint nrxq[4];
    guint disabled;
} RxqFx;

static void
test_parse_rxq_show_fixtures(void)
{
    const RxqFx cases[] = {
        {"numa1-two-pmd.txt", 2, {20, 21}, {1, 1}, {FALSE, FALSE}, {3, 2}, 0},
        {"numa1-pinned.txt", 2, {20, 21}, {1, 1}, {FALSE, TRUE}, {2, 2}, 0},
        {"two-numa.txt", 4, {2, 3, 20, 21}, {0, 0, 1, 1}, {FALSE, FALSE, TRUE, TRUE}, {1, 2, 1, 1}, 0},
        {"unconnected.txt", 2, {20, 21}, {1, 1}, {FALSE, TRUE}, {2, 1}, 0},
        {"disabled-queue.txt", 2, {20, 21}, {1, 1}, {FALSE, TRUE}, {3, 2}, 1},
        {"cross-numa.txt", 2, {2, 3}, {0, 0}, {FALSE, TRUE}, {3, 2}, 0},
    };
    for (guint c = 0; c < G_N_ELEMENTS(cases); c++) {
        gchar *text = fx_read(cases[c].file);
        GPtrArray *pmds = NULL;
        GError *error = NULL;
        g_test_message("fixture %s", cases[c].file);
        g_assert_true(pcv_dpdk_pmd_parse_rxq_show(text, &pmds, &error));
        g_assert_no_error(error);
        g_assert_cmpuint(pmds->len, ==, cases[c].npmd);
        guint disabled = 0;
        for (guint i = 0; i < pmds->len; i++) {
            PcvDpdkPmd *pmd = g_ptr_array_index(pmds, i);
            g_assert_cmpuint(pmd->core, ==, cases[c].cores[i]);
            g_assert_cmpint(pmd->numa, ==, cases[c].numas[i]);
            g_assert_cmpint(pmd->isolated, ==, cases[c].isolated[i]);
            g_assert_cmpuint(pmd->rxqs->len, ==, cases[c].nrxq[i]);
            for (guint j = 0; j < pmd->rxqs->len; j++)
                if (!((PcvDpdkPmdRxq *)g_ptr_array_index(pmd->rxqs, j))->enabled) disabled++;
        }
        g_assert_cmpuint(disabled, ==, cases[c].disabled);
        g_ptr_array_unref(pmds);
        g_free(text);
    }

    gchar *text = fx_read("numa1-pinned.txt");
    GPtrArray *pmds = NULL;
    g_assert_true(pcv_dpdk_pmd_parse_rxq_show(text, &pmds, NULL));
    PcvDpdkPmdRxq *q1 = g_ptr_array_index(((PcvDpdkPmd *)g_ptr_array_index(pmds, 1))->rxqs, 1);
    g_assert_cmpstr(q1->port, ==, "dpdk-v-pcv-affv-a");
    g_assert_cmpuint(q1->qid, ==, 1);
    g_ptr_array_unref(pmds);
    g_free(text);

    const gchar *bad[] = {"empty.txt", "garbage.txt"};
    for (guint b = 0; b < G_N_ELEMENTS(bad); b++) {
        gchar *t = fx_read(bad[b]);
        GPtrArray *none = NULL;
        GError *error = NULL;
        g_assert_false(pcv_dpdk_pmd_parse_rxq_show(t, &none, &error));
        g_assert_null(none);
        g_assert_nonnull(error);
        g_clear_error(&error);
        g_free(t);
    }

    GPtrArray *none = NULL;
    g_assert_false(pcv_dpdk_pmd_parse_rxq_show("Displaying last 60 seconds pmd usage %\n", &none, NULL));
    g_assert_false(pcv_dpdk_pmd_parse_rxq_show(
        "pmd thread numa_id 1 core_id 20:\n  port: p  queue-id:  0 (enabled)  pmd usage: 0 %\n", &none, NULL));
    g_assert_false(pcv_dpdk_pmd_parse_rxq_show(NULL, &none, NULL));
}

static void
assert_mask(const gchar *mask, const guint *want, guint n)
{
    GArray *cores = NULL;
    g_assert_true(pcv_dpdk_pmd_parse_mask(mask, &cores));
    g_assert_cmpuint(cores->len, ==, n);
    for (guint i = 0; i < n; i++) g_assert_cmpuint(g_array_index(cores, guint, i), ==, want[i]);
    g_array_unref(cores);
}

static void
test_parse_mask_forms(void)
{
    const guint c2021[] = {20, 21};
    const guint c2_3_20_21[] = {2, 3, 20, 21};
    const guint c71[] = {0, 71};
    assert_mask("0x300000", c2021, 2);
    assert_mask("300000", c2021, 2);
    assert_mask("\"0x30000c\"", c2_3_20_21, 4);
    assert_mask("0x800000000000000001", c71, 2);
    assert_mask("", NULL, 0);
    GArray *cores = NULL;
    g_assert_false(pcv_dpdk_pmd_parse_mask("0xZZ", &cores));
    g_assert_null(cores);
}

static void
test_cache_states(void)
{
    pcv_dpdk_pmd_test_set_appctl_runner(NULL, NULL);
    PcvPmdCacheState st = PCV_PMD_CACHE_VALID;
    GArray *snap = pcv_dpdk_pmd_cache_snapshot(&st);
    g_assert_cmpint(st, ==, PCV_PMD_CACHE_UNKNOWN);
    g_assert_cmpuint(snap->len, ==, 0);
    g_array_unref(snap);

    GArray *mask = NULL;
    g_assert_true(pcv_dpdk_pmd_parse_mask("0x300000", &mask));
    pcv_dpdk_pmd_cache_update(NULL, mask, "mask");
    snap = pcv_dpdk_pmd_cache_snapshot(&st);
    g_assert_cmpint(st, ==, PCV_PMD_CACHE_VALID);
    g_assert_cmpuint(snap->len, ==, 2);

    g_array_index(snap, guint, 0) = 99;
    g_array_set_size(snap, 0);
    g_array_unref(snap);
    snap = pcv_dpdk_pmd_cache_snapshot(&st);
    g_assert_cmpuint(snap->len, ==, 2);
    g_assert_cmpuint(g_array_index(snap, guint, 0), ==, 20);
    g_array_unref(snap);

    gchar *text = fx_read("cross-numa.txt");
    GPtrArray *pmds = NULL;
    g_assert_true(pcv_dpdk_pmd_parse_rxq_show(text, &pmds, NULL));
    pcv_dpdk_pmd_cache_update(pmds, NULL, "pmd-rxq-show");
    snap = pcv_dpdk_pmd_cache_snapshot(&st);
    g_assert_cmpuint(snap->len, ==, 4);
    g_assert_cmpuint(g_array_index(snap, guint, 0), ==, 2);
    g_assert_cmpuint(g_array_index(snap, guint, 3), ==, 21);
    g_array_unref(snap);
    g_ptr_array_unref(pmds);
    g_free(text);

    GArray *empty_mask = g_array_new(FALSE, FALSE, sizeof(guint));
    GPtrArray *empty_pmds = g_ptr_array_new();
    pcv_dpdk_pmd_cache_update(empty_pmds, empty_mask, "pmd-rxq-show");
    pcv_dpdk_pmd_cache_update(NULL, NULL, "mask");
    snap = pcv_dpdk_pmd_cache_snapshot(&st);
    g_assert_cmpint(st, ==, PCV_PMD_CACHE_EMPTY);
    g_assert_cmpuint(snap->len, ==, 0);
    g_array_unref(snap);
    g_ptr_array_unref(empty_pmds);
    g_array_unref(empty_mask);
    g_array_unref(mask);
    pcv_dpdk_pmd_test_set_appctl_runner(NULL, NULL);
}

static gchar *
fn_body(const gchar *src, const gchar *name)
{
    gchar *needle = g_strdup_printf("\n%s(", name);
    const gchar *p = strstr(src, needle);
    g_free(needle);
    if (!p) return NULL;
    const gchar *open = strchr(p, '{');
    gint depth = 0;
    for (const gchar *q = open; q && *q; q++) {
        if (*q == '{') depth++;
        else if (*q == '}' && --depth == 0) return g_strndup(open, (gsize)(q - open + 1));
    }
    return NULL;
}

static void
test_cache_no_gmu(void)
{
    gchar *src = NULL;
    g_assert_true(g_file_get_contents("src/modules/network/dpdk_pmd.c", &src, NULL, NULL));
    const gchar *fns[] = {"pcv_dpdk_pmd_cache_snapshot", "pcv_dpdk_pmd_cache_update", "_cache_union_locked"};
    for (guint i = 0; i < G_N_ELEMENTS(fns); i++) {
        gchar *body = fn_body(src, fns[i]);
        g_assert_nonnull(body);
        g_assert_null(strstr(body, "G.mu"));
        g_assert_null(strstr(body, "_ovs_run_locked"));
        g_assert_null(strstr(body, "pcv_spawn"));
        g_free(body);
    }
    g_free(src);
}

typedef struct {
    gint show_mode;
    const gchar *isolate;
    const gchar *show_file;
    const gchar *mask;
    guint calls;
} QueryFx;

static gboolean
query_runner(const gchar * const *argv, gchar **out, GError **error, gpointer data)
{
    QueryFx *f = data;
    f->calls++;
    if (argv_has(argv, "dpif-netdev/pmd-rxq-show")) {
        g_assert_cmpstr(argv[0], ==, "ovs-appctl");
        g_assert_cmpstr(argv[1], ==, "-T");
        g_assert_cmpstr(argv[2], ==, "2");
        if (f->show_mode == 1) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "exit 124");
            return FALSE;
        }
        *out = f->show_mode == 2 ? g_strdup("") : fx_read(f->show_file);
        return TRUE;
    }
    if (argv_has(argv, "other_config:pmd-rxq-isolate")) {
        g_assert_true(argv_has(argv, "--if-exists"));
        g_assert_true(argv_has(argv, "--timeout=2"));
        *out = g_strdup(f->isolate ? f->isolate : "");
        return TRUE;
    }
    if (argv_has(argv, "other_config:pmd-cpu-mask")) {
        *out = g_strdup(f->mask ? f->mask : "\"0x300000\"\n");
        return TRUE;
    }
    *out = g_strdup("");
    return TRUE;
}

static void
test_query_seam(void)
{
    QueryFx f = { .show_file = "numa1-pinned.txt" };
    pcv_dpdk_pmd_test_set_appctl_runner(query_runner, &f);
    GPtrArray *pmds = NULL;
    gboolean isolate = FALSE;
    GError *error = NULL;

    f.show_mode = 1;
    g_assert_false(pcv_dpdk_pmd_query(&pmds, &isolate, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    f.show_mode = 2;
    g_assert_false(pcv_dpdk_pmd_query(&pmds, &isolate, &error));
    g_clear_error(&error);
    PcvPmdCacheState st = PCV_PMD_CACHE_VALID;
    GArray *snap = pcv_dpdk_pmd_cache_snapshot(&st);
    g_assert_cmpint(st, ==, PCV_PMD_CACHE_UNKNOWN);
    g_array_unref(snap);

    f.show_mode = 0;
    f.isolate = "";
    g_assert_true(pcv_dpdk_pmd_query(&pmds, &isolate, &error));
    g_assert_no_error(error);
    g_assert_true(isolate);
    g_assert_cmpuint(pmds->len, ==, 2);
    g_ptr_array_unref(pmds);
    snap = pcv_dpdk_pmd_cache_snapshot(&st);
    g_assert_cmpint(st, ==, PCV_PMD_CACHE_VALID);
    g_assert_cmpuint(snap->len, ==, 2);
    g_array_unref(snap);

    f.isolate = "\"false\"\n";
    g_assert_true(pcv_dpdk_pmd_query(&pmds, &isolate, &error));
    g_assert_false(isolate);
    g_ptr_array_unref(pmds);

    pcv_dpdk_pmd_test_set_appctl_runner(query_runner, &f);
    f.calls = 0;
    pcv_dpdk_pmd_cache_init_from_mask();
    g_assert_cmpuint(f.calls, ==, 1);
    snap = pcv_dpdk_pmd_cache_snapshot(&st);
    g_assert_cmpint(st, ==, PCV_PMD_CACHE_VALID);
    g_assert_cmpuint(snap->len, ==, 2);
    g_array_unref(snap);
    pcv_dpdk_pmd_test_set_appctl_runner(NULL, NULL);
}

static gboolean
status_runner(const gchar * const *argv, gchar **out, GError **error, gpointer data)
{
    (void)error; (void)data;
    if (argv_has(argv, "dpif-netdev/pmd-rxq-show")) { *out = fx_read("numa1-pinned.txt"); return TRUE; }
    if (argv_has(argv, "find") && argv_has(argv, "external_ids:purecvisor-owner=dpdk-vhost")) {
        *out = g_strdup(
            "dpdk-v-pcv-affv-a\n"
            "purecvisor-bridge=dpdk-br0 purecvisor-owner=dpdk-vhost purecvisor-pmd=\"21\" purecvisor-vm=pcv-affv-a\n"
            "pmd-rxq-affinity=\"0:21,1:21\"\n"
            "mode=client n_rxq=\"2\" numa=\"1\"\n"
            "\n"
            "dpdk-v-pcv-affv-b\n"
            "purecvisor-bridge=dpdk-br0 purecvisor-owner=dpdk-vhost purecvisor-vm=pcv-affv-b\n"
            "\n"
            "mode=client\n");
        return TRUE;
    }
    *out = g_strdup("");
    return TRUE;
}

static void
assert_no_vm_names(JsonNode *node)
{
    gchar *text = json_to_string(node, FALSE);
    g_assert_null(strstr(text, "pcv-affv"));
    g_free(text);
}

static void
test_status_role(void)
{
    pcv_dpdk_pmd_test_set_appctl_runner(status_runner, NULL);
    pcv_dpdk_test_set_ovs_runner(NULL, NULL, 1);

    JsonObject *viewer = pcv_dpdk_pmd_status_object(FALSE);
    g_assert_false(json_object_has_member(viewer, "ports"));
    JsonArray *pmds = json_object_get_array_member(viewer, "pmds");
    g_assert_cmpuint(json_array_get_length(pmds), ==, 2);
    for (guint i = 0; i < 2; i++) {
        JsonObject *p = json_array_get_object_element(pmds, i);
        g_assert_false(json_object_has_member(p, "rxqs"));
        g_assert_true(json_object_has_member(p, "core"));
        g_assert_true(json_object_has_member(p, "product_ports"));
    }
    JsonNode *vn = json_node_init_object(json_node_alloc(), viewer);
    assert_no_vm_names(vn);
    json_node_unref(vn);
    json_object_unref(viewer);

    JsonObject *admin = pcv_dpdk_pmd_status_object(TRUE);
    g_assert_true(json_object_get_boolean_member(admin, "rxq_isolate"));
    JsonArray *ports = json_object_get_array_member(admin, "ports");
    g_assert_cmpuint(json_array_get_length(ports), ==, 2);
    JsonObject *a = json_array_get_object_element(ports, 0);
    g_assert_cmpstr(json_object_get_string_member(a, "vm"), ==, "pcv-affv-a");
    g_assert_cmpstr(json_object_get_string_member(a, "source"), ==, "product");
    g_assert_cmpint(json_object_get_int_member(a, "marker_core"), ==, 21);
    g_assert_cmpint(json_object_get_int_member(a, "port_numa"), ==, 1);
    g_assert_false(json_object_get_boolean_member(a, "drift"));
    g_assert_cmpstr(json_object_get_string_member(a, "last_start_reason"), ==, "unknown");
    JsonObject *b = json_array_get_object_element(ports, 1);
    g_assert_cmpstr(json_object_get_string_member(b, "source"), ==, "none");
    JsonObject *p21 = json_array_get_object_element(json_object_get_array_member(admin, "pmds"), 1);
    g_assert_cmpint(json_object_get_int_member(p21, "product_ports"), ==, 1);
    g_assert_false(json_object_get_boolean_member(p21, "reserved"));
    JsonObject *p20 = json_array_get_object_element(json_object_get_array_member(admin, "pmds"), 0);
    g_assert_true(json_object_get_boolean_member(p20, "reserved"));
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(p21, "rxqs")), ==, 2);
    JsonObject *cache = json_object_get_object_member(admin, "pmd_cache");
    g_assert_cmpstr(json_object_get_string_member(cache, "state"), ==, "valid");
    json_object_unref(admin);

    pcv_dpdk_pmd_test_set_appctl_runner(NULL, NULL);
    pcv_dpdk_test_set_ovs_runner(NULL, NULL, 0);
    JsonObject *off = pcv_dpdk_pmd_status_object(TRUE);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(off, "pmds")), ==, 0);
    g_assert_true(json_object_get_null_member(off, "rxq_isolate"));
    const gchar *state = json_object_get_string_member(json_object_get_object_member(off, "pmd_cache"), "state");
    g_assert_true(g_strcmp0(state, "unknown") == 0 || g_strcmp0(state, "empty") == 0);
    json_object_unref(off);
    pcv_dpdk_test_set_ovs_runner(NULL, NULL, -1);
}

static void
pmd_add(GPtrArray *pmds, guint core, gint numa, gboolean isolated, const gchar *rxqs)
{
    PcvDpdkPmd *pmd = g_new0(PcvDpdkPmd, 1);
    pmd->core = core;
    pmd->numa = numa;
    pmd->isolated = isolated;
    pmd->rxqs = g_ptr_array_new_with_free_func(rxq_free);
    gchar **items = g_strsplit(rxqs ? rxqs : "", ",", -1);
    for (guint i = 0; items[i]; i++) {
        if (!*items[i]) continue;
        gchar **pq = g_strsplit(items[i], ":", 2);
        PcvDpdkPmdRxq *rxq = g_new0(PcvDpdkPmdRxq, 1);
        rxq->port = g_strdup(pq[0]);
        rxq->qid = pq[1] ? (guint)g_ascii_strtoull(pq[1], NULL, 10) : 0;
        rxq->enabled = TRUE;
        g_ptr_array_add(pmd->rxqs, rxq);
        g_strfreev(pq);
    }
    g_strfreev(items);
    g_ptr_array_add(pmds, pmd);
}

static PcvDpdkPmdInput
input_new(gint vm_numa, guint nq, gboolean isolate, const gchar *host_nodes)
{
    PcvDpdkPmdInput in = { .mode = PCV_PMD_MODE_AUTO, .vm_numa = vm_numa, .nq = nq, .isolate = isolate,
                           .query_ok = TRUE };
    in.pmds = g_ptr_array_new_with_free_func(pcv_dpdk_pmd_free);
    in.host_nodes = g_array_new(FALSE, FALSE, sizeof(guint));
    gchar **nodes = g_strsplit(host_nodes ? host_nodes : "", ",", -1);
    for (guint i = 0; nodes[i]; i++)
        if (*nodes[i]) { guint n = (guint)g_ascii_strtoull(nodes[i], NULL, 10); g_array_append_val(in.host_nodes, n); }
    g_strfreev(nodes);
    return in;
}

static GHashTable *
markers_new(const gchar *spec)
{
    GHashTable *m = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    gchar **items = g_strsplit(spec ? spec : "", ",", -1);
    for (guint i = 0; items[i]; i++) {
        gchar **kv = g_strsplit(items[i], "=", 2);
        if (kv[0] && kv[1]) g_hash_table_insert(m, g_strdup(kv[0]), g_strdup(kv[1]));
        g_strfreev(kv);
    }
    g_strfreev(items);
    return m;
}

static void
choose_expect(PcvDpdkPmdInput *in, const gchar *self, const gchar *cur, const gchar *prev,
              const gchar *markers, gint action, guint core, const gchar *reason, gboolean remote)
{
    GHashTable *m = markers_new(markers);
    PcvDpdkPmdPlan plan;
    pcv_dpdk_pmd_choose(in, self, cur, prev, m, &plan);
    g_test_message("self=%s reason=%s core=%u", self, plan.reason, plan.core);
    g_assert_cmpint(plan.action, ==, action);
    g_assert_cmpstr(plan.reason, ==, reason);
    if (action == PCV_PMD_APPLY) {
        g_assert_cmpuint(plan.core, ==, core);
        g_assert_cmpint(plan.numa_remote, ==, remote);
    }
    pcv_dpdk_pmd_plan_clear(&plan);
    g_hash_table_unref(m);
}

static void
test_choose_spec_table(void)
{

    PcvDpdkPmdInput in = input_new(1, 2, TRUE, "0,1");
    pmd_add(in.pmds, 20, 1, FALSE, "dpdk-p-br:0");
    choose_expect(&in, "dpdk-v-a", NULL, NULL, NULL, PCV_PMD_NOT_NEEDED, 0, "not-needed", FALSE);
    pcv_dpdk_pmd_input_clear(&in);

    in = input_new(1, 2, TRUE, "0,1");
    pmd_add(in.pmds, 20, 1, FALSE, "dpdk-p-br:0,dpdk-v-a:0");
    pmd_add(in.pmds, 21, 1, FALSE, "dpdk-v-a:1");
    choose_expect(&in, "dpdk-v-a", NULL, NULL, NULL, PCV_PMD_APPLY, 21, "applied", FALSE);
    pcv_dpdk_pmd_input_clear(&in);

    in = input_new(1, 2, TRUE, "0,1");
    pmd_add(in.pmds, 20, 1, FALSE, "dpdk-p-br:0,dpdk-v-c:0");
    pmd_add(in.pmds, 21, 1, TRUE, "dpdk-v-a:0,dpdk-v-a:1,dpdk-v-b:0,dpdk-v-b:1");
    choose_expect(&in, "dpdk-v-c", NULL, NULL, "dpdk-v-a=21,dpdk-v-b=21", PCV_PMD_APPLY, 21, "applied", FALSE);
    pcv_dpdk_pmd_input_clear(&in);

    const guint want[] = {21, 22, 23, 21};
    GString *markers = g_string_new(NULL);
    GString *pinned[4] = { g_string_new(NULL), g_string_new(NULL), g_string_new(NULL), g_string_new(NULL) };
    for (guint vm = 0; vm < 4; vm++) {
        in = input_new(1, 2, TRUE, "0,1");
        for (guint c = 0; c < 4; c++)
            pmd_add(in.pmds, 20 + c, 1, pinned[c]->len > 0, c == 0 ? "dpdk-p-br:0" : pinned[c]->str);
        gchar *self = g_strdup_printf("dpdk-v-%u", vm);
        choose_expect(&in, self, NULL, NULL, markers->str, PCV_PMD_APPLY, want[vm], "applied", FALSE);
        g_string_append_printf(markers, "%s%s=%u", markers->len ? "," : "", self, want[vm]);
        GString *at = pinned[want[vm] - 20];
        g_string_append_printf(at, "%s%s:0,%s:1", at->len ? "," : "", self, self);
        g_free(self);
        pcv_dpdk_pmd_input_clear(&in);
    }
    g_string_free(markers, TRUE);
    for (guint c = 0; c < 4; c++) g_string_free(pinned[c], TRUE);

    in = input_new(1, 2, TRUE, "0,1");
    pmd_add(in.pmds, 2, 0, FALSE, "dpdk-p-br:0");
    pmd_add(in.pmds, 3, 0, FALSE, "");
    choose_expect(&in, "dpdk-v-a", NULL, NULL, NULL, PCV_PMD_APPLY, 3, "applied", TRUE);
    pcv_dpdk_pmd_input_clear(&in);

    in = input_new(1, 2, TRUE, "0,1");
    pmd_add(in.pmds, 2, 0, FALSE, "");
    pmd_add(in.pmds, 20, 1, FALSE, "dpdk-p-br:0");
    choose_expect(&in, "dpdk-v-a", NULL, NULL, NULL, PCV_PMD_NOT_NEEDED, 0, "not-needed", FALSE);
    pcv_dpdk_pmd_input_clear(&in);

    in = input_new(0, 2, TRUE, "0,1");
    pmd_add(in.pmds, 2, 0, FALSE, "");
    pmd_add(in.pmds, 20, 1, FALSE, "dpdk-p-br:0");
    pmd_add(in.pmds, 21, 1, FALSE, "");
    choose_expect(&in, "dpdk-v-a", NULL, NULL, NULL, PCV_PMD_APPLY, 21, "applied", TRUE);
    pcv_dpdk_pmd_input_clear(&in);

    in = input_new(1, 2, TRUE, "0,1");
    pmd_add(in.pmds, 20, 1, TRUE, "dpdk-v-a:0,dpdk-v-a:1");
    pmd_add(in.pmds, 21, 1, FALSE, "dpdk-p-br:0");
    choose_expect(&in, "dpdk-v-a", "0:20,1:20", NULL, NULL, PCV_PMD_OPERATOR, 0, "operator", FALSE);

    choose_expect(&in, "dpdk-v-b", NULL, NULL, NULL, PCV_PMD_SKIP, 0, "no-eligible-pmd", FALSE);
    pcv_dpdk_pmd_input_clear(&in);
}

static void
plan_of(PcvDpdkPmdInput *in, const gchar *cur, const gchar *prev, const gchar *markers, PcvDpdkPmdPlan *plan)
{
    GHashTable *m = markers_new(markers);
    pcv_dpdk_pmd_choose(in, "dpdk-v-a", cur, prev, m, plan);
    g_hash_table_unref(m);
}

static PcvDpdkPmdInput
two_pmd_host(void)
{
    PcvDpdkPmdInput in = input_new(1, 2, TRUE, "0,1");
    pmd_add(in.pmds, 20, 1, FALSE, "dpdk-p-br:0");
    pmd_add(in.pmds, 21, 1, TRUE, "dpdk-v-a:0");
    return in;
}

static void
test_choose_modes(void)
{
    PcvDpdkPmdInput in = two_pmd_host();
    PcvDpdkPmdPlan plan;
    in.mode = PCV_PMD_MODE_OFF;
    plan_of(&in, "0:21,1:21", "21", NULL, &plan);
    g_assert_cmpint(plan.action, ==, PCV_PMD_CLEAR_OWNED);
    g_assert_true(plan.clear_marker);
    g_assert_cmpstr(plan.reason, ==, "off");
    pcv_dpdk_pmd_plan_clear(&plan);
    plan_of(&in, "0:20,1:20", NULL, NULL, &plan);
    g_assert_cmpint(plan.action, ==, PCV_PMD_OPERATOR);
    g_assert_false(plan.clear_marker);
    pcv_dpdk_pmd_plan_clear(&plan);
    in.mode = PCV_PMD_MODE_INVALID;
    plan_of(&in, "0:21,1:21", "21", NULL, &plan);
    g_assert_cmpint(plan.action, ==, PCV_PMD_SKIP);
    g_assert_cmpstr(plan.reason, ==, "config-invalid");
    g_assert_false(plan.clear_marker);
    g_assert_false(plan.changed);
    pcv_dpdk_pmd_plan_clear(&plan);
    pcv_dpdk_pmd_input_clear(&in);
}

static void
test_choose_ownership(void)
{
    g_assert_true(pcv_dpdk_pmd_affinity_owned("0:21,1:21", "21"));
    g_assert_true(pcv_dpdk_pmd_affinity_owned("0:21,1:21,2:21,3:21", "21"));
    g_assert_false(pcv_dpdk_pmd_affinity_owned("0:20,1:21", "21"));
    g_assert_false(pcv_dpdk_pmd_affinity_owned("0:21,1:21", NULL));
    g_assert_false(pcv_dpdk_pmd_affinity_owned("", "21"));
    g_assert_false(pcv_dpdk_pmd_affinity_owned("0:21,x", "21"));

    PcvDpdkPmdInput in = two_pmd_host();
    PcvDpdkPmdPlan plan;
    in.nq = 4;
    plan_of(&in, "0:21,1:21", "21", NULL, &plan);
    g_assert_cmpint(plan.action, ==, PCV_PMD_APPLY);
    g_assert_cmpstr(plan.affinity, ==, "0:21,1:21,2:21,3:21");
    g_assert_true(plan.changed);
    pcv_dpdk_pmd_plan_clear(&plan);
    plan_of(&in, "0:21,1:21,2:21,3:21", "21", NULL, &plan);
    g_assert_cmpint(plan.action, ==, PCV_PMD_APPLY);
    g_assert_false(plan.changed);
    pcv_dpdk_pmd_plan_clear(&plan);
    in.nq = 2;
    plan_of(&in, "0:20,1:21", "21", NULL, &plan);
    g_assert_cmpint(plan.action, ==, PCV_PMD_OPERATOR);
    pcv_dpdk_pmd_plan_clear(&plan);
    plan_of(&in, NULL, "21", NULL, &plan);
    g_assert_cmpint(plan.action, ==, PCV_PMD_APPLY);
    g_assert_cmpuint(plan.core, ==, 21);
    pcv_dpdk_pmd_plan_clear(&plan);
    pcv_dpdk_pmd_input_clear(&in);

    in = input_new(1, 2, TRUE, "1");
    pmd_add(in.pmds, 20, 1, FALSE, "dpdk-p-br:0");
    plan_of(&in, NULL, "21", NULL, &plan);
    g_assert_cmpint(plan.action, ==, PCV_PMD_NOT_NEEDED);
    g_assert_true(plan.clear_marker);
    pcv_dpdk_pmd_plan_clear(&plan);
    pcv_dpdk_pmd_input_clear(&in);
}

static void
test_choose_query_failed(void)
{
    PcvDpdkPmdInput in = two_pmd_host();
    in.query_ok = FALSE;
    PcvDpdkPmdPlan plan;
    plan_of(&in, "0:21,1:21", "21", NULL, &plan);
    g_assert_cmpint(plan.action, ==, PCV_PMD_SKIP);
    g_assert_cmpstr(plan.reason, ==, "pmd-query-failed");
    g_assert_false(plan.clear_marker);
    g_assert_false(plan.changed);
    pcv_dpdk_pmd_plan_clear(&plan);
    pcv_dpdk_pmd_input_clear(&in);
}

static void
test_choose_numa_cases(void)
{
    PcvDpdkPmdInput in = input_new(-1, 2, TRUE, "0,1");
    pmd_add(in.pmds, 20, 1, FALSE, "dpdk-p-br:0");
    pmd_add(in.pmds, 21, 1, FALSE, "");
    choose_expect(&in, "dpdk-v-a", NULL, NULL, NULL, PCV_PMD_APPLY, 21, "applied", FALSE);
    pcv_dpdk_pmd_input_clear(&in);

    in = input_new(1, 2, TRUE, "1");
    pmd_add(in.pmds, 20, 1, TRUE, "dpdk-v-op:0");
    pmd_add(in.pmds, 21, 1, FALSE, "dpdk-p-br:0");
    PcvDpdkPmdPlan plan;
    plan_of(&in, "0:21,1:21", "21", NULL, &plan);
    g_assert_cmpint(plan.action, ==, PCV_PMD_SKIP);
    g_assert_cmpstr(plan.reason, ==, "no-eligible-pmd");
    g_assert_true(plan.clear_marker);
    g_assert_true(plan.changed);
    pcv_dpdk_pmd_plan_clear(&plan);
    pcv_dpdk_pmd_input_clear(&in);
}

static void
test_choose_stability_balance(void)
{
    PcvDpdkPmdInput in = input_new(1, 2, TRUE, "1");
    pmd_add(in.pmds, 20, 1, FALSE, "dpdk-p-br:0");
    pmd_add(in.pmds, 21, 1, TRUE, "dpdk-v-x:0");
    pmd_add(in.pmds, 22, 1, TRUE, "dpdk-v-y:0,dpdk-v-a:0");
    pmd_add(in.pmds, 23, 1, FALSE, "");

    choose_expect(&in, "dpdk-v-a", "0:22,1:22", "22", "dpdk-v-x=21,dpdk-v-y=22,dpdk-v-a=22",
                  PCV_PMD_APPLY, 22, "applied", FALSE);

    choose_expect(&in, "dpdk-v-b", NULL, NULL, "dpdk-v-x=21,dpdk-v-y=22", PCV_PMD_APPLY, 23, "applied", FALSE);

    choose_expect(&in, "dpdk-v-b", NULL, NULL, "dpdk-v-x=21,dpdk-v-y=22,dpdk-v-s1=23,dpdk-v-s2=23",
                  PCV_PMD_APPLY, 21, "applied", FALSE);
    pcv_dpdk_pmd_input_clear(&in);
}

static void
test_choose_isolate_false(void)
{
    PcvDpdkPmdInput in = input_new(1, 2, FALSE, "1");
    pmd_add(in.pmds, 20, 1, FALSE, "dpdk-p-br:0");
    pmd_add(in.pmds, 21, 1, FALSE, "");
    choose_expect(&in, "dpdk-v-a", NULL, NULL, NULL, PCV_PMD_APPLY, 20, "applied", FALSE);
    pcv_dpdk_pmd_input_clear(&in);
}

static void
test_choose_nq_string(void)
{
    const struct { guint nq; const gchar *want; } cases[] = {
        {1, "0:21"}, {2, "0:21,1:21"}, {4, "0:21,1:21,2:21,3:21"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        PcvDpdkPmdInput in = two_pmd_host();
        in.nq = cases[i].nq;
        PcvDpdkPmdPlan plan;
        plan_of(&in, NULL, NULL, NULL, &plan);
        g_assert_cmpstr(plan.affinity, ==, cases[i].want);
        g_assert_cmpuint(plan.nq, ==, cases[i].nq);
        pcv_dpdk_pmd_plan_clear(&plan);
        pcv_dpdk_pmd_input_clear(&in);
    }
}

typedef struct {
    guint nodes, npmd, cores[6];
    gint numa[6];
    gboolean pinned_other[6];
} RandHost;

static PcvDpdkPmdInput
rand_input(GRand *rng, RandHost *h, GString *markers, gchar **cur, gchar **prev)
{
    h->nodes = (guint)g_rand_int_range(rng, 1, 3);
    h->npmd = (guint)g_rand_int_range(rng, 0, 7);
    gboolean isolate = g_rand_int_range(rng, 0, 4) != 0;
    GString *rx[6];
    for (guint i = 0; i < h->npmd; i++) {
        h->cores[i] = 10 + i * 3 + (guint)g_rand_int_range(rng, 0, 3);
        h->numa[i] = g_rand_int_range(rng, 0, (gint)h->nodes);
        h->pinned_other[i] = FALSE;
        rx[i] = g_string_new(NULL);
    }

    guint nports = (guint)g_rand_int_range(rng, 0, 6);
    for (guint p = 0; p < nports && h->npmd; p++) {
        gint kind = g_rand_int_range(rng, 0, 4);
        guint at = (guint)g_rand_int_range(rng, 0, (gint)h->npmd);
        if (kind >= 2)
            g_string_append_printf(markers, "%sdpdk-v-o%u=%u", markers->len ? "," : "", p, h->cores[at]);

        if (kind == 3) { if (isolate) h->pinned_other[at] = TRUE; continue; }
        g_string_append_printf(rx[at], "%sdpdk-v-o%u:0", rx[at]->len ? "," : "", p);
        if (kind != 0 && isolate) h->pinned_other[at] = TRUE;
    }
    gint self_at = h->npmd && g_rand_boolean(rng) ? g_rand_int_range(rng, 0, (gint)h->npmd) : -1;
    if (self_at >= 0) {
        *prev = g_strdup_printf("%u", h->cores[self_at]);
        *cur = g_strdup_printf("0:%u,1:%u", h->cores[self_at], h->cores[self_at]);
        g_string_append_printf(markers, "%sdpdk-v-self=%u", markers->len ? "," : "", h->cores[self_at]);
        if (isolate) g_string_append_printf(rx[self_at], "%sdpdk-v-self:0", rx[self_at]->len ? "," : "");
    }
    PcvDpdkPmdInput in = input_new(g_rand_int_range(rng, -1, (gint)h->nodes), 2, isolate,
                                   h->nodes == 2 ? "0,1" : "0");
    for (guint i = 0; i < h->npmd; i++) {
        pmd_add(in.pmds, h->cores[i], h->numa[i], h->pinned_other[i] || ((gint)i == self_at && isolate), rx[i]->str);
        g_string_free(rx[i], TRUE);
    }
    return in;
}

static void
test_choose_invariant_never_last_unpinned(void)
{
    GRand *rng = g_rand_new_with_seed(20261120);
    guint applied = 0;
    for (guint iter = 0; iter < 1000; iter++) {
        RandHost h;
        GString *markers = g_string_new(NULL);
        gchar *cur = NULL, *prev = NULL;
        PcvDpdkPmdInput in = rand_input(rng, &h, markers, &cur, &prev);
        GHashTable *m = markers_new(markers->str);
        PcvDpdkPmdPlan plan;
        pcv_dpdk_pmd_choose(&in, "dpdk-v-self", cur, prev, m, &plan);
        if (plan.action == PCV_PMD_APPLY && in.isolate) {
            applied++;
            guint before_global = 0, after_global = 0;
            for (gint n = 0; n < (gint)h.nodes; n++) {
                guint before = 0, after = 0;
                for (guint i = 0; i < h.npmd; i++) {
                    if (h.numa[i] != n || h.pinned_other[i]) continue;
                    before++;
                    if (h.cores[i] != plan.core) after++;
                }
                if (before) g_assert_cmpuint(after, >=, 1);
                before_global += before;
                after_global += after;
            }
            if (before_global) g_assert_cmpuint(after_global, >=, 1);
        }
        pcv_dpdk_pmd_plan_clear(&plan);
        g_hash_table_unref(m);
        pcv_dpdk_pmd_input_clear(&in);
        g_string_free(markers, TRUE);
        g_free(prev);
        g_free(cur);
    }
    g_test_message("APPLY with isolate=true: %u/1000", applied);
    g_assert_cmpuint(applied, >, 100);
    g_rand_free(rng);
}

static void
test_audit_detail(void)
{
    PcvDpdkPmdInput in = two_pmd_host();
    PcvDpdkPmdPlan plan;
    plan_of(&in, NULL, NULL, NULL, &plan);
    gchar *d = pcv_dpdk_pmd_audit_detail(&plan);
    g_assert_cmpstr(d, ==, "applied core=21 nq=2");
    g_free(d);
    pcv_dpdk_pmd_plan_clear(&plan);
    plan_of(&in, "0:21,1:21", "21", NULL, &plan);
    g_assert_null(pcv_dpdk_pmd_audit_detail(&plan));
    pcv_dpdk_pmd_plan_clear(&plan);
    in.mode = PCV_PMD_MODE_OFF;
    plan_of(&in, "0:21,1:21", "21", NULL, &plan);
    d = pcv_dpdk_pmd_audit_detail(&plan);
    g_assert_cmpstr(d, ==, "cleared reason=off");
    g_free(d);
    pcv_dpdk_pmd_plan_clear(&plan);
    plan_of(&in, "0:20,1:20", NULL, NULL, &plan);
    g_assert_null(pcv_dpdk_pmd_audit_detail(&plan));
    pcv_dpdk_pmd_plan_clear(&plan);
    pcv_dpdk_pmd_input_clear(&in);
}

static void
test_input_from_xml(void)
{
    pcv_dpdk_pmd_test_set_appctl_runner(status_runner, NULL);
    const gchar *xml =
        "<domain type='kvm'><name>a</name><memory unit='KiB'>1048576</memory>"
        "<metadata><numatune><memory mode='strict' nodeset='0'/></numatune></metadata>"
        "<numatune><memory mode='strict' nodeset='1'/></numatune>"
        "<devices><interface type='vhostuser'><source type='unix' path='/x' mode='server'/>"
        "<driver queues='2' rx_queue_size='1024' tx_queue_size='256'/></interface></devices></domain>";
    PcvDpdkPmdInput in = { 0 };
    pcv_dpdk_pmd_input_prepare(&in, xml);
    g_assert_cmpint(in.vm_numa, ==, 1);
    g_assert_cmpuint(in.nq, ==, 2);
    g_assert_true(in.query_ok);
    g_assert_cmpuint(in.pmds->len, ==, 2);
    pcv_dpdk_pmd_input_clear(&in);
    pcv_dpdk_pmd_input_prepare(&in, "<domain><devices><interface type='vhostuser'/></devices></domain>");
    g_assert_cmpint(in.vm_numa, ==, -1);
    g_assert_cmpuint(in.nq, ==, 1);
    pcv_dpdk_pmd_input_clear(&in);
    pcv_dpdk_pmd_input_prepare(&in, "<domain><numatune><memory mode='strict' nodeset='0-1'/></numatune></domain>");
    g_assert_cmpint(in.vm_numa, ==, -1);
    pcv_dpdk_pmd_input_clear(&in);
    pcv_dpdk_pmd_test_set_appctl_runner(NULL, NULL);
}

static void
count_warn(const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer data)
{
    (void)domain; (void)level; (void)message;
    (*(guint *)data)++;
}

static void
test_choose_stale_snapshot_two_phase(void)
{
    PcvDpdkPmdInput in = input_new(1, 2, TRUE, "0,1");
    pmd_add(in.pmds, 20, 1, TRUE, "dpdk-v-a:0,dpdk-v-a:1");
    pmd_add(in.pmds, 21, 1, FALSE, "dpdk-p-br:0");
    GHashTable *m = markers_new("dpdk-v-a=20");
    PcvDpdkPmdPlan b, a;
    pcv_dpdk_pmd_choose(&in, "dpdk-v-b", NULL, NULL, m, &b);
    g_hash_table_unref(m);
    m = markers_new(b.action == PCV_PMD_APPLY ? "dpdk-v-a=20,dpdk-v-b=20" : "dpdk-v-a=20");
    if (b.action == PCV_PMD_APPLY) {
        gchar *core = g_strdup_printf("%u", b.core);
        g_hash_table_replace(m, g_strdup("dpdk-v-b"), core);
    }
    pcv_dpdk_pmd_choose(&in, "dpdk-v-a", "0:20,1:20", "20", m, &a);
    gboolean took20 = (b.action == PCV_PMD_APPLY && b.core == 20) || (a.action == PCV_PMD_APPLY && a.core == 20);
    gboolean took21 = (b.action == PCV_PMD_APPLY && b.core == 21) || (a.action == PCV_PMD_APPLY && a.core == 21);
    g_test_message("B %s %u, A %s %u", b.reason, b.core, a.reason, a.core);
    g_assert_false(took20 && took21);
    pcv_dpdk_pmd_plan_clear(&a);
    pcv_dpdk_pmd_plan_clear(&b);
    g_hash_table_unref(m);
    pcv_dpdk_pmd_input_clear(&in);
}

static void
test_choose_concurrent_second_shares(void)
{

    PcvDpdkPmdInput in = input_new(1, 2, TRUE, "0,1");
    pmd_add(in.pmds, 20, 1, TRUE, "dpdk-v-a:0,dpdk-v-a:1");
    pmd_add(in.pmds, 21, 1, FALSE, "dpdk-p-br:0,dpdk-v-b:0");
    GHashTable *m = markers_new("dpdk-v-a=20");
    PcvDpdkPmdPlan a, b;
    pcv_dpdk_pmd_choose(&in, "dpdk-v-a", "0:20,1:20", "20", m, &a);
    g_assert_cmpint(a.action, ==, PCV_PMD_APPLY);
    g_hash_table_unref(m);
    gchar *spec = g_strdup_printf("dpdk-v-a=%u", a.core);
    m = markers_new(spec);
    pcv_dpdk_pmd_choose(&in, "dpdk-v-b", NULL, NULL, m, &b);
    g_test_message("shape1 a=%s %u b=%s %u", a.reason, a.core, b.reason, b.core);
    g_assert_cmpint(b.action, ==, PCV_PMD_APPLY);
    g_assert_cmpuint(b.core, ==, a.core);
    pcv_dpdk_pmd_plan_clear(&a);
    pcv_dpdk_pmd_plan_clear(&b);
    g_hash_table_unref(m);
    g_free(spec);
    pcv_dpdk_pmd_input_clear(&in);

    in = input_new(1, 2, TRUE, "0,1");
    pmd_add(in.pmds, 34, 1, FALSE, "dpdk-v-c:0,dpdk-v-b:0");
    pmd_add(in.pmds, 35, 1, FALSE, "dpdk-p-br:0,dpdk-v-b:1");
    choose_expect(&in, "dpdk-v-c", "0:23,1:23", "23", "dpdk-v-b=35,dpdk-v-c=23",
                  PCV_PMD_APPLY, 35, "applied", FALSE);
    pcv_dpdk_pmd_input_clear(&in);
}

static void
test_parse_isolated_without_colon(void)
{
    GPtrArray *none = NULL;
    gchar *t = fx_read("garbage-isolated.txt");
    g_assert_false(pcv_dpdk_pmd_parse_rxq_show(t, &none, NULL));
    g_assert_null(none);
    g_free(t);
}

static void
test_cache_refresh_rereads_mask(void)
{
    QueryFx f = { .show_file = "numa1-pinned.txt", .isolate = "", .mask = "\"0x30000c\"\n" };
    pcv_dpdk_pmd_test_set_appctl_runner(query_runner, &f);
    pcv_dpdk_test_set_ovs_runner(NULL, NULL, 1);
    pcv_dpdk_pmd_cache_init_from_mask();
    f.mask = "\"0x300000\"\n";
    pcv_dpdk_pmd_cache_refresh();
    PcvPmdCacheState st = PCV_PMD_CACHE_UNKNOWN;
    GArray *snap = pcv_dpdk_pmd_cache_snapshot(&st);
    g_assert_cmpuint(snap->len, ==, 2);
    g_assert_cmpuint(g_array_index(snap, guint, 0), ==, 20);
    g_array_unref(snap);
    pcv_dpdk_pmd_test_set_appctl_runner(query_runner, &f);
    f.mask = "";
    pcv_dpdk_pmd_cache_init_from_mask();
    snap = pcv_dpdk_pmd_cache_snapshot(&st);
    g_assert_cmpint(st, ==, PCV_PMD_CACHE_UNKNOWN);
    g_array_unref(snap);
    pcv_dpdk_test_set_ovs_runner(NULL, NULL, -1);
    pcv_dpdk_pmd_test_set_appctl_runner(NULL, NULL);
}

static void
test_mode_from_config(void)
{
    gchar *dir = g_dir_make_tmp("pcv-dpdk-pmd-cfg-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, "daemon.conf", NULL);
    g_assert_true(g_file_set_contents(path, "[dpdk]\nvhost_pmd_affinity = bogus\n", -1, NULL));
    g_setenv("PCV_CONFIG_PATH", path, TRUE);
    pcv_config_init();
    g_test_expect_message("dpdk_pmd", G_LOG_LEVEL_WARNING,
                          "*vhost_pmd_affinity=bogus is not auto|off*left unchanged*");
    g_assert_cmpint(pcv_dpdk_pmd_mode_from_config(), ==, PCV_PMD_MODE_INVALID);
    g_test_assert_expected_messages();

    guint warns = 0;
    guint id = g_log_set_handler("dpdk_pmd", G_LOG_LEVEL_WARNING, count_warn, &warns);
    g_assert_cmpint(pcv_dpdk_pmd_mode_from_config(), ==, PCV_PMD_MODE_INVALID);
    g_assert_cmpuint(warns, ==, 0);
    pcv_config_shutdown();
    g_assert_true(g_file_set_contents(path, "[dpdk]\nvhost_pmd_affinity = Auto\n", -1, NULL));
    pcv_config_init();
    g_assert_cmpint(pcv_dpdk_pmd_mode_from_config(), ==, PCV_PMD_MODE_INVALID);
    g_assert_cmpint(pcv_dpdk_pmd_mode_from_config(), ==, PCV_PMD_MODE_INVALID);
    g_assert_cmpuint(warns, ==, 1);
    g_log_remove_handler("dpdk_pmd", id);
    pcv_config_shutdown();
    g_assert_true(g_file_set_contents(path, "[dpdk]\nvhost_pmd_affinity = off\n", -1, NULL));
    pcv_config_init();
    g_assert_cmpint(pcv_dpdk_pmd_mode_from_config(), ==, PCV_PMD_MODE_OFF);
    pcv_config_shutdown();
    g_assert_true(g_file_set_contents(path, "[network]\nx = 1\n", -1, NULL));
    pcv_config_init();
    g_assert_cmpint(pcv_dpdk_pmd_mode_from_config(), ==, PCV_PMD_MODE_AUTO);
    pcv_config_shutdown();
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);
    g_remove(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

void test_dpdk_pmd_register(void);

void
test_dpdk_pmd_register(void)
{
    g_test_add_func("/dpdk_pmd/parse/rxq_show_fixtures", test_parse_rxq_show_fixtures);
    g_test_add_func("/dpdk_pmd/parse/mask_forms", test_parse_mask_forms);
    g_test_add_func("/dpdk_pmd/cache/states", test_cache_states);
    g_test_add_func("/dpdk_pmd/cache/no_gmu", test_cache_no_gmu);
    g_test_add_func("/dpdk_pmd/query/seam", test_query_seam);
    g_test_add_func("/dpdk_pmd/status/role", test_status_role);
    g_test_add_func("/dpdk_pmd/choose/spec_table", test_choose_spec_table);
    g_test_add_func("/dpdk_pmd/choose/modes", test_choose_modes);
    g_test_add_func("/dpdk_pmd/choose/ownership", test_choose_ownership);
    g_test_add_func("/dpdk_pmd/choose/query_failed", test_choose_query_failed);
    g_test_add_func("/dpdk_pmd/choose/numa_cases", test_choose_numa_cases);
    g_test_add_func("/dpdk_pmd/choose/stability_balance", test_choose_stability_balance);
    g_test_add_func("/dpdk_pmd/choose/isolate_false", test_choose_isolate_false);
    g_test_add_func("/dpdk_pmd/choose/invariant_never_last_unpinned", test_choose_invariant_never_last_unpinned);
    g_test_add_func("/dpdk_pmd/choose/nq_string", test_choose_nq_string);
    g_test_add_func("/dpdk_pmd/audit/detail_on_change_only", test_audit_detail);
    g_test_add_func("/dpdk_pmd/input/from_xml", test_input_from_xml);
    g_test_add_func("/dpdk_pmd/config/mode_from_config", test_mode_from_config);
    g_test_add_func("/dpdk_pmd/choose/stale_snapshot_two_phase", test_choose_stale_snapshot_two_phase);
    g_test_add_func("/dpdk_pmd/choose/concurrent_second_shares", test_choose_concurrent_second_shares);
    g_test_add_func("/dpdk_pmd/parse/isolated_without_colon", test_parse_isolated_without_colon);
    g_test_add_func("/dpdk_pmd/cache/refresh_rereads_mask", test_cache_refresh_rereads_mask);
}
