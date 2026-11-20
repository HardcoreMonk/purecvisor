#include <glib.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "../src/modules/auth/pcv_rbac.h"
#include "../src/modules/core/vm_config_history_store.h"
#include "../src/modules/daemons/vm_config_history.h"
#include "../src/modules/dispatcher/handler_vm_config_history.h"
#include "../src/modules/dispatcher/rpc_utils.h"

#define UA "aaaaaaaa-0000-4000-8000-00000000000a"
#define UB "bbbbbbbb-0000-4000-8000-00000000000b"
#define UC "cccccccc-0000-4000-8000-00000000000c"
#define UD "dddddddd-0000-4000-8000-00000000000d"
#define UF "ffffffff-0000-4000-8000-00000000000f"

static GPtrArray *
h_list(gpointer d, GError **e)
{
    (void)d;
    (void)e;
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(a, g_strdup(UF));
    return a;
}

static gboolean
h_read(gpointer d, const gchar *u, PcvVmCfgRaw *o, gboolean *x, GError **e)
{
    (void)d;
    (void)u;
    (void)o;
    (void)x;
    g_set_error(e, G_IO_ERROR, G_IO_ERROR_FAILED, "fake read failure");
    return FALSE;
}

static const PcvVmCfgHistSource k_hsrc = {.list_domains = h_list, .read = h_read};

typedef struct {
    gchar         *dir;
    gchar         *db;
    PcvVmCfgStore *w;
    gint64         epoch;
} Env;

static gchar *
put(Env *e, const gchar *uuid, const gchar *name, const gchar *owner, const gchar *tenant, gint64 at, gboolean present,
    const gchar *changes)
{
    PcvVmCfgRow *r = g_new0(PcvVmCfgRow, 1);
    r->vm_uuid = g_strdup(uuid);
    r->vm_name = g_strdup(name);
    r->owner = g_strdup(owner);
    r->owner_tenant = g_strdup(tenant);
    r->kind = g_strdup("changed");
    r->scope = g_strdup("config");
    r->cause = g_strdup("rpc");
    r->observed_at = at;
    r->attribution = g_strdup("rpc");
    r->actor = g_strdup("alice");
    r->actor_tenant = g_strdup("t1");
    r->actor_method = g_strdup("vm.set_vcpu");
    r->actor_result = g_strdup("ok");
    r->config_digest = g_strdup("d");
    r->snapshot_json = g_strdup("{\"autostart\":false,\"config\":{\"vcpu.max\":\"2\"},\"live_overlay\":null}");
    r->changes_json = g_strdup(changes ? changes : "[{\"after\":\"4\",\"before\":\"2\",\"path\":\"vcpu.max\"}]");
    r->normalizer_version = 1;
    r->epoch_id = e->epoch;
    PcvVmCfgLatestUpdate lu = {.present = present, .current_name = name, .confirmed_at = at};
    g_assert_true(pcv_vm_cfg_store_insert(e->w, r, &lu, NULL));
    gchar *id = g_strdup(r->entry_id);
    pcv_vm_cfg_row_free(r);
    return id;
}

static void
env_start(Env *e, gboolean enabled, gboolean bad_path)
{
    memset(e, 0, sizeof *e);
    e->dir = g_dir_make_tmp("pcv-vmch-XXXXXX", NULL);
    e->db = bad_path ? g_build_filename(e->dir, "missing", "x.db", NULL)
                     : g_build_filename(e->dir, "vm_config_history.db", NULL);
    PcvVmCfgHistConfig c = {.db_path = e->db, .enabled = enabled, .retention_days = 365, .max_db_mb = 128,
                            .actor_retention_days = 30, .debounce_ms = 100, .undefine_grace_ms = 300,
                            .coalesce_ms = 1000, .settle_ms = 300, .shutdown_timeout_ms = 3000, .heartbeat_s = 300,
                            .prune_interval_s = 3600, .pending_max = 256};
    g_assert_true(pcv_vm_cfg_hist_start_with(&c, &k_hsrc, NULL));
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    if (!enabled || bad_path)
        return;
    e->w = pcv_vm_cfg_store_open(e->db, TRUE, NULL);
    g_assert_nonnull(e->w);
    gint64 now = g_get_real_time() / 1000;
    gint64 old = pcv_vm_cfg_store_epoch_begin(e->w, now - 3 * 86400000LL, NULL);
    pcv_vm_cfg_store_epoch_end(e->w, old, now - 2 * 86400000LL, "shutdown", NULL);
    e->epoch = pcv_vm_cfg_store_epoch_begin(e->w, now - 86400000LL, NULL);
    g_free(put(e, UA, "web", "alice", "t1", now - 5000, TRUE, NULL));
    g_free(put(e, UA, "web", "alice", "t1", now - 4000, FALSE, NULL));
    g_free(put(e, UB, "db", "bob", "t2", now - 3000, TRUE, NULL));
    g_free(put(e, UC, "orphan", NULL, NULL, now - 2000, TRUE, NULL));
    g_free(put(e, UD, "web", "alice", "t1", now - 1000, TRUE, NULL));
    g_free(put(e, UF, "flaky", "bob", "t2", now - 900, TRUE, NULL));
}

static void
env_stop(Env *e)
{
    pcv_vm_cfg_hist_shutdown();
    pcv_vm_cfg_store_close(e->w);
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

static PcvResCaller
caller(gint role, const gchar *subject, const gchar *tenant)
{
    PcvResCaller c = {.role = role, .subject = g_strdup(subject), .tenant = g_strdup(tenant)};
    return c;
}

static JsonObject *
params(const gchar *json)
{
    JsonParser *p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, json, -1, NULL));
    JsonObject *o = json_object_ref(json_node_get_object(json_parser_get_root(p)));
    g_object_unref(p);
    return o;
}

static JsonObject *
list_ok(const PcvResCaller *c, const gchar *json)
{
    JsonObject *p = params(json);
    gint code = 0;
    gchar *msg = NULL;
    JsonObject *o = pcv_vm_cfg_history_list_build(p, c, &code, &msg);
    g_assert_nonnull(o);
    g_assert_null(msg);
    json_object_unref(p);
    return o;
}

static gchar *
uuids_of(JsonObject *o)
{
    JsonArray *a = json_object_get_array_member(o, "entries");
    GPtrArray *u = g_ptr_array_new();
    for (guint i = 0; i < json_array_get_length(a); i++) {
        const gchar *x = json_object_get_string_member(json_array_get_object_element(a, i), "vm_uuid");
        gboolean dup = FALSE;
        for (guint k = 0; k < u->len; k++)
            dup = dup || !g_strcmp0(g_ptr_array_index(u, k), x);
        if (!dup)
            g_ptr_array_add(u, (gpointer)x);
    }
    GString *s = g_string_new(NULL);
    gchar **v = g_new0(gchar *, u->len + 1);
    for (guint k = 0; k < u->len; k++)
        v[k] = g_ptr_array_index(u, k);
    for (guint i = 0; i < u->len; i++) {
        for (guint j = i + 1; j < u->len; j++) {
            if (strcmp(v[j], v[i]) < 0) {
                gchar *t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
        g_string_append_printf(s, "%s%c", v[i], ',');
    }
    g_free(v);
    g_ptr_array_unref(u);
    return g_string_free(s, FALSE);
}

static void
test_handler_scope_admin_all(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    PcvResCaller c = caller(PCV_ROLE_ADMIN, "root", NULL);
    JsonObject *o = list_ok(&c, "{}");
    gchar *u = uuids_of(o);
    g_assert_cmpstr(u, ==, UA "," UB "," UC "," UD "," UF ",");
    g_assert_cmpstr(json_object_get_string_member(o, "scope"), ==, "all");
    g_free(u);
    json_object_unref(o);
    pcv_res_caller_clear(&c);
    env_stop(&e);
}

static void
test_handler_scope_same_tenant_operator(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    PcvResCaller c = caller(PCV_ROLE_OPERATOR, "carl", "t1");
    JsonObject *o = list_ok(&c, "{}");
    gchar *u = uuids_of(o);
    g_assert_cmpstr(u, ==, UA "," UD ",");
    g_assert_cmpstr(json_object_get_string_member(o, "scope"), ==, "own");
    g_free(u);
    json_object_unref(o);
    pcv_res_caller_clear(&c);
    env_stop(&e);
}

static void
test_handler_scope_other_tenant_empty(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    PcvResCaller c = caller(PCV_ROLE_OPERATOR, "dave", "t3");
    JsonObject *o = list_ok(&c, "{}");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "entries")), ==, 0);
    json_object_unref(o);
    pcv_res_caller_clear(&c);

    gint64 now = g_get_real_time() / 1000;
    g_free(put(&e, UB, "db", "eve", "bad tenant!", now - 10, TRUE, NULL));
    PcvResCaller x = caller(PCV_ROLE_VIEWER, "mallory", "bad tenant!");
    o = list_ok(&x, "{}");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "entries")), ==, 0);
    json_object_unref(o);
    pcv_res_caller_clear(&x);
    env_stop(&e);
}

static void
test_handler_scope_tenantless_viewer_own_only(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    PcvResCaller c = caller(PCV_ROLE_VIEWER, "bob", NULL);
    JsonObject *o = list_ok(&c, "{}");
    gchar *u = uuids_of(o);
    g_assert_cmpstr(u, ==, UB "," UF ",");
    g_free(u);
    json_object_unref(o);
    pcv_res_caller_clear(&c);
    env_stop(&e);
}

static void
test_handler_scope_api_key_null_tenant(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    PcvResCaller c = caller(PCV_ROLE_VIEWER, "apikey:ci-reader", NULL);
    JsonObject *o = list_ok(&c, "{}");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "entries")), ==, 0);
    json_object_unref(o);
    pcv_res_caller_clear(&c);
    env_stop(&e);
}

static void
test_handler_get_out_of_scope_equals_missing(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    gint64 now = g_get_real_time() / 1000;
    gchar *bob_id = put(&e, UB, "db", "bob", "t2", now - 100, TRUE, NULL);
    PcvResCaller carl = caller(PCV_ROLE_OPERATOR, "carl", "t1");
    PcvResCaller root = caller(PCV_ROLE_ADMIN, "root", NULL);
    gchar *pj = g_strdup_printf("{\"id\":\"%s\"}", bob_id);
    JsonObject *p = params(pj);
    gint code = 0;
    gchar *msg = NULL;
    g_assert_null(pcv_vm_cfg_history_get_build(p, &carl, &code, &msg));
    g_assert_cmpint(code, ==, PURE_RPC_ERR_FORBIDDEN);
    g_assert_cmpstr(msg, ==, PCV_VMCH_GET_DENY);
    g_free(msg);
    msg = NULL;
    JsonObject *missing = params("{\"id\":\"00000000000000000000000000000000\"}");
    g_assert_null(pcv_vm_cfg_history_get_build(missing, &root, &code, &msg));
    g_assert_cmpint(code, ==, PURE_RPC_ERR_FORBIDDEN);
    g_assert_cmpstr(msg, ==, PCV_VMCH_GET_DENY);
    g_free(msg);
    msg = NULL;
    JsonObject *bad = params("{\"id\":\"42\"}");
    g_assert_null(pcv_vm_cfg_history_get_build(bad, &root, &code, &msg));
    g_assert_cmpint(code, ==, PURE_RPC_ERR_INVALID_PARAMS);
    g_free(msg);
    JsonObject *o = pcv_vm_cfg_history_get_build(p, &root, &code, &msg);
    g_assert_nonnull(o);
    g_assert_cmpstr(json_object_get_string_member(o, "id"), ==, bob_id);
    g_assert_false(json_object_get_null_member(o, "prev_id"));
    g_assert_nonnull(json_object_get_object_member(o, "snapshot"));
    json_object_unref(o);
    json_object_unref(p);
    json_object_unref(missing);
    json_object_unref(bad);
    g_free(pj);
    g_free(bob_id);
    pcv_res_caller_clear(&carl);
    pcv_res_caller_clear(&root);
    env_stop(&e);
}

static void
test_handler_name_resolves_present_only(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    PcvResCaller c = caller(PCV_ROLE_ADMIN, "root", NULL);
    JsonObject *o = list_ok(&c, "{\"vm\":\"web\"}");
    gchar *u = uuids_of(o);
    g_assert_cmpstr(u, ==, UD ",");
    g_free(u);
    json_object_unref(o);
    o = list_ok(&c, "{\"vm\":\"no-such-vm\"}");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "entries")), ==, 0);
    json_object_unref(o);
    pcv_res_caller_clear(&c);
    env_stop(&e);
}

static void
test_handler_deleted_vm_uuid(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    PcvResCaller carl = caller(PCV_ROLE_OPERATOR, "carl", "t1");
    PcvResCaller dave = caller(PCV_ROLE_OPERATOR, "dave", "t3");
    JsonObject *o = list_ok(&carl, "{\"uuid\":\"" UA "\"}");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "entries")), ==, 2);
    json_object_unref(o);
    o = list_ok(&dave, "{\"uuid\":\"" UA "\"}");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "entries")), ==, 0);
    json_object_unref(o);
    pcv_res_caller_clear(&carl);
    pcv_res_caller_clear(&dave);
    env_stop(&e);
}

static void
test_handler_gaps_store_same_for_all(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    PcvResCaller root = caller(PCV_ROLE_ADMIN, "root", NULL);
    PcvResCaller dave = caller(PCV_ROLE_VIEWER, "dave", "t3");
    JsonObject *a = list_ok(&root, "{}"), *b = list_ok(&dave, "{}");
    JsonNode *ga = json_node_new(JSON_NODE_OBJECT), *gb = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(ga, json_object_get_object_member(a, "store"));
    json_node_set_object(gb, json_object_get_object_member(b, "store"));
    gchar *sa = json_to_string(ga, FALSE), *sb = json_to_string(gb, FALSE);
    g_assert_cmpstr(sa, ==, sb);
    g_assert_nonnull(strstr(sa, "\"state\":\"ok\""));
    g_assert_nonnull(strstr(sa, "\"retention_days\":365"));
    gboolean saw_shutdown = FALSE;
    JsonArray *gaps = json_object_get_array_member(b, "gaps");
    for (guint i = 0; i < json_array_get_length(gaps); i++)
        saw_shutdown = saw_shutdown ||
                       !g_strcmp0(json_object_get_string_member(json_array_get_object_element(gaps, i), "reason"),
                                  "shutdown");
    g_assert_true(saw_shutdown);
    g_free(sa);
    g_free(sb);
    json_node_unref(ga);
    json_node_unref(gb);
    json_object_unref(a);
    json_object_unref(b);
    pcv_res_caller_clear(&root);
    pcv_res_caller_clear(&dave);
    env_stop(&e);
}

static void
test_handler_vm_gap_only_visible_uuid(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    pcv_vm_cfg_hist_note_uuid(UF, "rpc");
    g_assert_true(pcv_vm_cfg_hist_sync(5000));
    PcvResCaller root = caller(PCV_ROLE_ADMIN, "root", NULL);
    PcvResCaller carl = caller(PCV_ROLE_OPERATOR, "carl", "t1");
    JsonObject *a = list_ok(&root, "{}"), *b = list_ok(&carl, "{}");
    gchar *sa = json_to_string(json_object_get_member(a, "gaps"), FALSE);
    gchar *sb = json_to_string(json_object_get_member(b, "gaps"), FALSE);
    g_assert_nonnull(strstr(sa, "capture_failed"));
    g_assert_nonnull(strstr(sa, UF));
    g_assert_null(strstr(sb, UF));
    g_free(sa);
    g_free(sb);
    json_object_unref(a);
    json_object_unref(b);
    pcv_res_caller_clear(&root);
    pcv_res_caller_clear(&carl);
    env_stop(&e);
}

static void
test_handler_response_cap_truncated(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    GString *big = g_string_new("[");
    for (gint i = 0; i < 8; i++)
        g_string_append_printf(big, "%s{\"after\":\"%0*d\",\"before\":null,\"path\":\"opaque.p%d\"}", i ? "," : "", 4000, i,
                               i);
    g_string_append_c(big, ']');
    gint64 now = g_get_real_time() / 1000;
    for (gint i = 0; i < 12; i++)
        g_free(put(&e, UB, "db", "bob", "t2", now - 50 + i, TRUE, big->str));
    PcvResCaller root = caller(PCV_ROLE_ADMIN, "root", NULL);
    JsonObject *o = list_ok(&root, "{\"limit\":50}");
    g_assert_true(json_object_get_boolean_member_with_default(o, "truncated", FALSE));
    gchar *s = json_to_string(json_object_get_member(o, "entries"), FALSE);
    g_assert_cmpuint(strlen(s), <=, PCV_VMCH_RESP_LIMIT);
    g_free(s);
    json_object_unref(o);
    g_string_free(big, TRUE);
    pcv_res_caller_clear(&root);
    env_stop(&e);
}

static void
test_handler_disabled_and_unavailable_shapes(void)
{
    Env e;
    env_start(&e, FALSE, FALSE);
    PcvResCaller root = caller(PCV_ROLE_ADMIN, "root", NULL);
    JsonObject *o = list_ok(&root, "{}");
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(o, "store"), "state"), ==, "disabled");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "entries")), ==, 0);
    json_object_unref(o);
    JsonObject *p = params("{\"id\":\"0123456789abcdef0123456789abcdef\"}");
    gint code = 0;
    gchar *msg = NULL;
    g_assert_null(pcv_vm_cfg_history_get_build(p, &root, &code, &msg));
    g_assert_cmpint(code, ==, PURE_RPC_ERR_FORBIDDEN);
    g_free(msg);
    env_stop(&e);
    env_start(&e, TRUE, TRUE);
    o = list_ok(&root, "{}");
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(o, "store"), "state"), ==, "unavailable");
    json_object_unref(o);
    json_object_unref(p);
    pcv_res_caller_clear(&root);
    env_stop(&e);
}

static void
test_handler_params_and_cursor(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    PcvResCaller root = caller(PCV_ROLE_ADMIN, "root", NULL);
    const gchar *bad[] = {"{\"limit\":0}", "{\"limit\":201}", "{\"limit\":\"5\"}", "{\"cursor\":\"!!\"}",
                          "{\"uuid\":\"web\"}", "{\"since\":10,\"until\":5}", "{\"vm\":7}",
                          "{\"until\":-9223372036854775807}", "{\"until\":-1}", NULL};
    for (guint i = 0; bad[i]; i++) {
        JsonObject *p = params(bad[i]);
        gint code = 0;
        gchar *msg = NULL;
        g_assert_null(pcv_vm_cfg_history_list_build(p, &root, &code, &msg));
        g_assert_cmpint(code, ==, PURE_RPC_ERR_INVALID_PARAMS);
        g_free(msg);
        json_object_unref(p);
    }

    const gchar *bad_get[] = {"{\"id\":{}}", "{\"id\":[1]}", "{\"id\":7}", NULL};
    for (guint i = 0; bad_get[i]; i++) {
        JsonObject *p = params(bad_get[i]);
        gint code = 0;
        gchar *msg = NULL;
        g_assert_null(pcv_vm_cfg_history_get_build(p, &root, &code, &msg));
        g_assert_cmpint(code, ==, PURE_RPC_ERR_INVALID_PARAMS);
        g_free(msg);
        json_object_unref(p);
    }
    JsonObject *a = list_ok(&root, "{\"limit\":3}");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(a, "entries")), ==, 3);
    const gchar *cur = json_object_get_string_member(a, "next_cursor");
    g_assert_nonnull(cur);
    gchar *pj = g_strdup_printf("{\"limit\":3,\"cursor\":\"%s\"}", cur);
    JsonObject *b = list_ok(&root, pj);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(b, "entries")), ==, 3);
    const gchar *first_b = json_object_get_string_member(
        json_array_get_object_element(json_object_get_array_member(b, "entries"), 0), "id");
    for (guint i = 0; i < 3; i++)
        g_assert_cmpstr(json_object_get_string_member(
                            json_array_get_object_element(json_object_get_array_member(a, "entries"), i), "id"),
                        !=, first_b);
    g_free(pj);
    json_object_unref(a);
    json_object_unref(b);
    pcv_res_caller_clear(&root);
    env_stop(&e);
}

static void
test_handler_readonly_open_cost(void)
{
    Env e;
    env_start(&e, TRUE, FALSE);
    gint64 t0 = g_get_monotonic_time();
    for (gint i = 0; i < 200; i++)
        pcv_vm_cfg_store_close(pcv_vm_cfg_store_open(e.db, FALSE, NULL));
    gdouble avg_ms = (gdouble)(g_get_monotonic_time() - t0) / 1000.0 / 200.0;
    g_test_message("vm_config_history readonly open+close avg %.3f ms (200 runs)", avg_ms);
    g_assert_cmpfloat(avg_ms, <, 50.0);
    env_stop(&e);
}

static gint
cmp_double(gconstpointer a, gconstpointer b)
{
    gdouble x = *(const gdouble *)a, y = *(const gdouble *)b;
    return (x > y) - (x < y);
}

static void
test_handler_list_p95_bench(void)
{
    const gchar *env = g_getenv("PCV_VMCH_BENCH_ROWS");
    gint64 n = env ? g_ascii_strtoll(env, NULL, 10) : 0;
    if (n <= 0) {
        g_test_skip("PCV_VMCH_BENCH_ROWS 없음 — Level 2 측정용");
        return;
    }
    Env e;
    env_start(&e, TRUE, FALSE);
    gint64 now = g_get_real_time() / 1000, t0 = g_get_monotonic_time();
    for (gint64 i = 0; i < n; i++) {
        gchar uuid[37];
        g_snprintf(uuid, sizeof uuid, "%08x-0000-4000-8000-%012x", (guint)(i % 400), (guint)(i % 400));
        gchar owner[8], tenant[8];
        g_snprintf(owner, sizeof owner, "o%u", (guint)(i % 400) % 20);
        g_snprintf(tenant, sizeof tenant, "t%u", (guint)(i % 400) % 20 / 4);
        g_free(put(&e, uuid, "bench", owner, tenant, now - 29 * 86400000LL + i * (29 * 86400000LL / n), TRUE, NULL));
    }
    g_test_message("vm_config_history bench insert %" G_GINT64_FORMAT " rows %.1f s", n,
                   (gdouble)(g_get_monotonic_time() - t0) / 1e6);
    const struct {
        const gchar *label;
        gint role;
        const gchar *subject, *tenant;
    } who[] = {{"admin", PCV_ROLE_ADMIN, "root", NULL}, {"operator-same-tenant", PCV_ROLE_OPERATOR, "o0", "t0"},
               {"viewer-other-tenant", PCV_ROLE_VIEWER, "zz", "t9"}};
    for (guint k = 0; k < G_N_ELEMENTS(who); k++) {
        PcvResCaller c = caller(who[k].role, who[k].subject, who[k].tenant);
        GArray *ms = g_array_new(FALSE, FALSE, sizeof(gdouble));
        for (gint r = 0; r < 100; r++) {
            gint64 s = g_get_monotonic_time();
            JsonObject *o = list_ok(&c, "{}");
            gdouble d = (gdouble)(g_get_monotonic_time() - s) / 1000.0;
            g_array_append_val(ms, d);
            json_object_unref(o);
        }
        g_array_sort(ms, cmp_double);
        gdouble p50 = g_array_index(ms, gdouble, 49), p95 = g_array_index(ms, gdouble, 94);
        g_test_message("vm_config_history list p50 %.2f ms p95 %.2f ms (%s, rows %" G_GINT64_FORMAT ")", p50, p95,
                       who[k].label, n);
        g_print("# vmch-bench list %s rows=%" G_GINT64_FORMAT " p50_ms=%.2f p95_ms=%.2f\n", who[k].label, n, p50, p95);
        g_assert_cmpfloat(p95, <, 2000.0);
        g_array_unref(ms);
        pcv_res_caller_clear(&c);
    }
    env_stop(&e);
}

void test_vm_config_history_handler_register(void);
void
test_vm_config_history_handler_register(void)
{
    g_test_add_func("/vm_config_history/handler/handler_scope_admin_all", test_handler_scope_admin_all);
    g_test_add_func("/vm_config_history/handler/handler_scope_same_tenant_operator",
                    test_handler_scope_same_tenant_operator);
    g_test_add_func("/vm_config_history/handler/handler_scope_other_tenant_empty", test_handler_scope_other_tenant_empty);
    g_test_add_func("/vm_config_history/handler/handler_scope_tenantless_viewer_own_only",
                    test_handler_scope_tenantless_viewer_own_only);
    g_test_add_func("/vm_config_history/handler/handler_scope_api_key_null_tenant",
                    test_handler_scope_api_key_null_tenant);
    g_test_add_func("/vm_config_history/handler/handler_get_out_of_scope_equals_missing",
                    test_handler_get_out_of_scope_equals_missing);
    g_test_add_func("/vm_config_history/handler/handler_name_resolves_present_only",
                    test_handler_name_resolves_present_only);
    g_test_add_func("/vm_config_history/handler/handler_deleted_vm_uuid", test_handler_deleted_vm_uuid);
    g_test_add_func("/vm_config_history/handler/handler_gaps_store_same_for_all", test_handler_gaps_store_same_for_all);
    g_test_add_func("/vm_config_history/handler/handler_vm_gap_only_visible_uuid", test_handler_vm_gap_only_visible_uuid);
    g_test_add_func("/vm_config_history/handler/handler_response_cap_truncated", test_handler_response_cap_truncated);
    g_test_add_func("/vm_config_history/handler/handler_disabled_and_unavailable_shapes",
                    test_handler_disabled_and_unavailable_shapes);
    g_test_add_func("/vm_config_history/handler/handler_params_and_cursor", test_handler_params_and_cursor);
    g_test_add_func("/vm_config_history/handler/handler_readonly_open_cost", test_handler_readonly_open_cost);
    g_test_add_func("/vm_config_history/handler/handler_list_p95_bench", test_handler_list_p95_bench);
}
