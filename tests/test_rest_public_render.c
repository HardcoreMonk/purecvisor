   
                                  
                                                                        
  
                           
                                                                                            
                                                                                                      
                                                                                                   
                                                                        
                                                                                                         
  
                       
                                                                                       
   
#include <glib.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>
#include <libvirt/libvirt.h>

#include "api/rest_public_render.h"
#include "modules/daemons/prometheus_exporter.h"
#include "modules/daemons/vm_diag_view.h"
#include "modules/virt/pcv_virt_open.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/resource/vm_read_scope.h"

void pcv_test_prom_render_set(const gchar *text);                           

static void
_expect_body(PcvRestRendered *r, guint status, const gchar *content_type, const gchar *body)
{
    g_assert_cmpuint(r->status, ==, status);
    g_assert_cmpstr(r->content_type, ==, content_type);
    g_assert_cmpstr(r->body, ==, body);
    pcv_rest_rendered_clear(r);
}

#define METRICS_CT "text/plain; version=0.0.4; charset=utf-8"

                                                                                                                                     
#define FLEET_FIXTURE \
    "{\"result\":{\"host\":{\"cpus\":8,\"cpu_total_ticks\":1000,\"cpu_idle_ticks\":800,\"mem_percent\":40.25," \
    "\"disk_percent\":70.5,\"mem_total_gb\":64.0,\"cpu_temp_c\":55.5,\"load_1\":1.25},\"fleet\":[" \
    "{\"name\":\"vm-a\",\"vcpu\":4,\"mem_max_mb\":4096.0,\"mem_used_mb\":1024.0,\"state\":\"RUNNING\"}," \
    "{\"name\":\"vm-b\",\"vcpu\":2,\"mem_max_mb\":2048.0,\"mem_used_mb\":-1,\"state\":\"OFFLINE\"}]}}"

                                                                                                  
                                                                                                
static void
test_metrics_from_fixture(void)
{
    PcvRestRendered r = { 0 };
    pcv_vm_diag_store_publish(NULL);                    
    pcv_rest_render_metrics(FLEET_FIXTURE, NULL, NULL, &r);
    _expect_body(&r, 200, METRICS_CT,
    "# HELP purecvisor_host_disk_percent Host disk utilization\n"
    "# TYPE purecvisor_host_disk_percent gauge\n"
    "purecvisor_host_disk_percent 70.50\n"
    "# HELP purecvisor_host_memory_total_bytes Host total memory\n"
    "# TYPE purecvisor_host_memory_total_bytes gauge\n"
    "purecvisor_host_memory_total_bytes 68719476736\n"
    "# HELP purecvisor_host_cpu_temp_celsius CPU temperature\n"
    "# TYPE purecvisor_host_cpu_temp_celsius gauge\n"
    "purecvisor_host_cpu_temp_celsius 55.5\n"
    "# HELP purecvisor_host_load1 1-minute load average\n"
    "# TYPE purecvisor_host_load1 gauge\n"
    "purecvisor_host_load1 1.25\n"
    "# HELP purecvisor_vm_vcpu VM vCPU count\n"
    "# TYPE purecvisor_vm_vcpu gauge\n"
    "purecvisor_vm_vcpu{vm=\"vm-a\"} 4\n"
    "purecvisor_vm_vcpu{vm=\"vm-b\"} 2\n"
    "# HELP purecvisor_vm_memory_max_mb VM max memory MB\n"
    "# TYPE purecvisor_vm_memory_max_mb gauge\n"
    "purecvisor_vm_memory_max_mb{vm=\"vm-a\"} 4096\n"
    "purecvisor_vm_memory_max_mb{vm=\"vm-b\"} 2048\n"
    "# HELP purecvisor_vm_memory_used_mb VM used memory MB\n"
    "# TYPE purecvisor_vm_memory_used_mb gauge\n"
    "purecvisor_vm_memory_used_mb{vm=\"vm-a\"} 1024\n"
    "# HELP purecvisor_vm_running VM running state\n"
    "# TYPE purecvisor_vm_running gauge\n"
    "purecvisor_vm_running{vm=\"vm-a\"} 1\n"
    "purecvisor_vm_running{vm=\"vm-b\"} 0\n");
                                                                       
    pcv_rest_render_metrics("{\"result\":{\"host\":{\"cpu_total_ticks\":2000.0,\"cpu_idle_ticks\":1500.0}}}", NULL, NULL, &r);
    _expect_body(&r, 200, METRICS_CT,
    "# HELP purecvisor_host_disk_percent Host disk utilization\n"
    "# TYPE purecvisor_host_disk_percent gauge\n"
    "purecvisor_host_disk_percent 0.00\n");
    pcv_rest_render_metrics("{\"result\":{}}", NULL, NULL, &r);
    _expect_body(&r, 200, METRICS_CT, "");
}

                                                                                         
                      
static const gchar *
_labels_end(const gchar *p)
{
    if (*p != '{') return p;
    p++;
    while (*p && *p != '}') {
        if (!(g_ascii_isalpha(*p) || *p == '_')) return NULL;
        while (g_ascii_isalnum(*p) || *p == '_') p++;
        if (*p++ != '=' || *p++ != '"') return NULL;
        while (*p && *p != '"') {
            if (*p == '\\') {
                if (p[1] != '\\' && p[1] != '"' && p[1] != 'n') return NULL;
                p += 2;
            } else {
                p++;
            }
        }
        if (*p++ != '"') return NULL;
        if (*p == ',') p++;
    }
    return *p == '}' ? p + 1 : NULL;
}

                                                                                                         
                                                                                    
                                      
static guint
_exposition_violations(const gchar *text, GString *why)
{
    g_autoptr(GHashTable) types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autoptr(GHashTable) started = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GHashTable) closed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GHashTable) series = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gchar **lines = g_strsplit(text ? text : "", "\n", -1);
    gchar *cur = NULL;
    guint bad = 0;
    for (gchar **lp = lines; *lp; lp++) {
        const gchar *l = *lp;
        if (!*l) continue;
        if (g_str_has_prefix(l, "# TYPE ")) {
            gchar **f = g_strsplit(l + 7, " ", 2);
            if (!f[0] || !f[1]) { bad++; g_string_append_printf(why, "TYPE 문법: %s\n", l); }
            else if (g_hash_table_contains(types, f[0])) { bad++; g_string_append_printf(why, "TYPE 두 번: %s\n", f[0]); }
            else if (g_hash_table_contains(started, f[0])) { bad++; g_string_append_printf(why, "TYPE 이 시계열 뒤: %s\n", f[0]); }
            else g_hash_table_insert(types, g_strdup(f[0]), g_strdup(f[1]));
            g_strfreev(f);
            continue;
        }
        if (l[0] == '#') continue;
        gsize nlen = strspn(l, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_:");
        const gchar *after = nlen ? _labels_end(l + nlen) : NULL;
        if (!after || *after != ' ' || !after[1]) { bad++; g_string_append_printf(why, "줄 문법: %s\n", l); continue; }
        gchar *name = g_strndup(l, nlen);
        gchar *key = g_strndup(l, (gsize)(after - l));
        if (!g_hash_table_contains(types, name)) { bad++; g_string_append_printf(why, "TYPE 없음: %s\n", name); }
        if (!cur || strcmp(cur, name) != 0) {
            if (g_hash_table_contains(closed, name)) { bad++; g_string_append_printf(why, "흩어진 계열: %s\n", name); }
            if (cur) g_hash_table_add(closed, g_strdup(cur));
            g_free(cur);
            cur = g_strdup(name);
        }
        g_hash_table_add(started, g_strdup(name));
        if (g_hash_table_contains(series, key)) { bad++; g_string_append_printf(why, "중복 계열: %s\n", key); g_free(key); }
        else g_hash_table_add(series, key);
        g_free(name);
    }
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, types);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        gboolean total = g_str_has_suffix(k, "_total"), counter = g_strcmp0(v, "counter") == 0;
        gboolean hist = g_str_has_suffix(k, "_bucket") || g_str_has_suffix(k, "_sum") || g_str_has_suffix(k, "_count");
        if (total && !counter) { bad++; g_string_append_printf(why, "_total 인데 %s: %s\n", (gchar *)v, (gchar *)k); }
        if (counter && !total && !hist) { bad++; g_string_append_printf(why, "counter 인데 _total 아님: %s\n", (gchar *)k); }
    }
    g_free(cur);
    g_strfreev(lines);
    return bad;
}

                                                                                       
                                                                                                  
static void
test_metrics_format_contract(void)
{
    PcvRestRendered r = { 0 };
    GString *why = g_string_new(NULL);
                                                     
    g_assert_cmpuint(_exposition_violations("# TYPE a gauge\na 1\n# TYPE a gauge\na 1\n", why), >=, 2);
    g_assert_cmpuint(_exposition_violations("# TYPE a gauge\n# TYPE b gauge\na{x=\"1\"} 1\nb 1\na{x=\"2\"} 1\n", why), ==, 1);
    g_assert_cmpuint(_exposition_violations("# TYPE c_total gauge\nc_total 1\n", why), ==, 1);
    g_string_truncate(why, 0);

    pcv_vm_diag_store_publish(NULL);
    pcv_test_prom_render_set("# TYPE purecvisor_host_cpu_percent gauge\npurecvisor_host_cpu_percent 1.5\n"
                             "# TYPE purecvisor_host_memory_percent gauge\npurecvisor_host_memory_percent 2.5\n"
                             "# TYPE node_forks_total counter\nnode_forks_total 7\n");
    pcv_rest_render_metrics(FLEET_FIXTURE, NULL, NULL, &r);
    pcv_test_prom_render_set(NULL);
    g_assert_cmpuint(r.status, ==, 200);
    guint bad = _exposition_violations(r.body, why);
    if (bad) g_test_message("형식 위반:\n%s", why->str);
    g_assert_cmpuint(bad, ==, 0);
                                     
    g_assert_nonnull(strstr(r.body, "\npurecvisor_host_cpu_percent 1.5\n"));
    g_assert_nonnull(strstr(r.body, "\npurecvisor_host_memory_percent 2.5\n"));
    g_assert_null(strstr(r.body, "Host CPU utilization"));
    pcv_rest_rendered_clear(&r);
    g_string_free(why, TRUE);
}

                                                             
static void
_rec_free(gpointer p)
{
    PcvVmDiagRecord *r = p;
    g_free(r->name);
    g_free(r->uuid);
    g_free(r);
}

                                                                              
typedef struct { GPtrArray *order; GHashTable *by_name; } SeriesText;
static void
_series_text_cb(const gchar *name, const gchar *labels, gdouble value, gint kind, gpointer ud)
{
    SeriesText *st = ud;
    GString *g = g_hash_table_lookup(st->by_name, name);
    if (!g) {
        g = g_string_new(NULL);
        g_string_append_printf(g, "# TYPE %s %s\n", name, kind == PCV_VM_DIAG_PROM_SOURCE_COUNTER ? "counter" : "gauge");
        g_hash_table_insert(st->by_name, g_strdup(name), g);
        g_ptr_array_add(st->order, g_strdup(name));
    }
    if (labels && *labels) g_string_append_printf(g, "%s{%s} %.17g\n", name, labels, value);
    else g_string_append_printf(g, "%s %.17g\n", name, value);
}

static void
_gstring_free_full(gpointer p)
{
    g_string_free(p, TRUE);
}

                                                                                      
static gchar *
_fleet_json_names(const gchar *const *names, guint n)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "result");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "fleet");
    json_builder_begin_array(b);
    for (guint i = 0; i < n; i++) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name");        json_builder_add_string_value(b, names[i]);
        json_builder_set_member_name(b, "vcpu");        json_builder_add_int_value(b, 2);
        json_builder_set_member_name(b, "mem_max_mb");  json_builder_add_double_value(b, 1024.0);
        json_builder_set_member_name(b, "mem_used_mb"); json_builder_add_double_value(b, 512.0);
        json_builder_set_member_name(b, "state");       json_builder_add_string_value(b, "RUNNING");
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    json_builder_end_object(b);
    JsonNode *root = json_builder_get_root(b);
    gchar *s = json_to_string(root, FALSE);
    json_node_unref(root);
    g_object_unref(b);
    return s;
}

                                                                                 
                                                                                                
                                             
static void
test_metrics_label_escape(void)
{
    static const gchar *const NAMES[] = { "plain", "q\"uo\\te", "nl\nx}y,z=\"w\"" };
    PcvRestRendered r = { 0 };

                                                                         
    PcvVmDiagSnapshot *s = g_new0(PcvVmDiagSnapshot, 1);
    s->state = PCV_VM_DIAG_S_OK;
    s->host.logical_cpus = 4;
    s->records = g_ptr_array_new_with_free_func(_rec_free);
    for (guint i = 0; i < G_N_ELEMENTS(NAMES); i++) {
        PcvVmDiagRecord *rec = g_new0(PcvVmDiagRecord, 1);
        rec->name = g_strdup(NAMES[i]);
        rec->uuid = g_strdup_printf("u-%u", i);
        rec->quality = PCV_VM_DIAG_Q_OK;
        rec->mem_alloc_mb = 512.0;
        rec->mem_guest_used_mb = rec->mem_rss_mb = NAN;
        rec->tot.present = PCV_VM_DIAG_B_CPU | PCV_VM_DIAG_B_NET;
        rec->tot.cpu_time_ns = 1000000000ULL * (i + 1);
        g_ptr_array_add(s->records, rec);
    }
    SeriesText st = { g_ptr_array_new_with_free_func(g_free),
                      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _gstring_free_full) };
    pcv_vm_diag_prom_series(s, PCV_PROM_VM_SERIES_MAX, _series_text_cb, &st, NULL);
    pcv_vm_diag_snapshot_free(s);
    GString *reg = g_string_new(NULL);
    for (guint i = 0; i < st.order->len; i++)
        g_string_append(reg, ((GString *)g_hash_table_lookup(st.by_name, g_ptr_array_index(st.order, i)))->str);
    g_ptr_array_unref(st.order);
    g_hash_table_unref(st.by_name);

    pcv_vm_diag_store_publish(NULL);
    pcv_test_prom_render_set(reg->str);
    gchar *fleet = _fleet_json_names(NAMES, G_N_ELEMENTS(NAMES));
    pcv_rest_render_metrics(fleet, NULL, NULL, &r);
    pcv_test_prom_render_set(NULL);
    g_free(fleet);
    g_string_free(reg, TRUE);

    const gchar *dump = g_getenv("PCV_TEST_PROM_DUMP");
    if (dump && *dump) g_assert_true(g_file_set_contents(dump, r.body, -1, NULL));

    GString *why = g_string_new(NULL);
    guint bad = _exposition_violations(r.body, why);
    if (bad) g_test_message("형식 위반:\n%s", why->str);
    g_assert_cmpuint(bad, ==, 0);
                                                                
    g_assert_nonnull(strstr(r.body, "\npurecvisor_vm_vcpu{vm=\"q\\\"uo\\\\te\"} 2\n"));
    g_assert_nonnull(strstr(r.body, "\npurecvisor_vm_running{vm=\"nl\\nx}y,z=\\\"w\\\"\"} 1\n"));
    g_assert_nonnull(strstr(r.body, "\npurecvisor_vm_vcpu{vm=\"plain\"} 2\n"));
                         
    g_assert_nonnull(strstr(r.body, "\npurecvisor_vm_cpu_seconds_total{vm=\"q\\\"uo\\\\te\"} 2\n"));
    g_assert_nonnull(strstr(r.body, "\npurecvisor_vm_net_rx_bytes_total{vm_name=\"nl\\nx}y,z=\\\"w\\\"\"} 0\n"));
    g_assert_null(strstr(r.body, "{vm=\"nl\nx"));                          
    g_string_free(why, TRUE);
    pcv_rest_rendered_clear(&r);
}

                                                                                          
static void
test_sample_age_series(void)
{
    PcvRestRendered r = { 0 };
    pcv_vm_diag_store_publish(NULL);
    pcv_rest_render_metrics("{\"result\":{}}", NULL, NULL, &r);
    g_assert_null(strstr(r.body, "purecvisor_vm_diag_sample_age_seconds"));
    pcv_rest_rendered_clear(&r);

    PcvVmDiagSnapshot *s = g_new0(PcvVmDiagSnapshot, 1);
    s->state = PCV_VM_DIAG_S_ERROR;
    s->error = PCV_VM_DIAG_E_CONNECT_FAILED;                              
    pcv_vm_diag_store_publish(s);
    pcv_rest_render_metrics(NULL, NULL, NULL, &r);
    g_assert_null(strstr(r.body, "purecvisor_vm_diag_sample_age_seconds"));
    pcv_rest_rendered_clear(&r);

    s = g_new0(PcvVmDiagSnapshot, 1);
    s->state = PCV_VM_DIAG_S_OK;
    s->observed_mono_us = g_get_monotonic_time() - 2 * G_USEC_PER_SEC;
    pcv_vm_diag_store_publish(s);
    pcv_rest_render_metrics("{\"result\":{}}", NULL, NULL, &r);
    pcv_vm_diag_store_publish(NULL);
    g_assert_nonnull(strstr(r.body, "# TYPE purecvisor_vm_diag_sample_age_seconds gauge\n"));
    const gchar *p = strstr(r.body, "\npurecvisor_vm_diag_sample_age_seconds ");
    g_assert_nonnull(p);
    g_assert_null(strstr(p + 1, "\npurecvisor_vm_diag_sample_age_seconds "));            
    gdouble age = g_ascii_strtod(p + strlen("\npurecvisor_vm_diag_sample_age_seconds "), NULL);
    g_assert_cmpfloat(age, >=, 2.0);
    g_assert_cmpfloat(age, <, 120.0);
    pcv_rest_rendered_clear(&r);
}

                                                
static void
test_metrics_daemon_unavailable(void)
{
    PcvRestRendered r = { 0 };
    pcv_rest_render_metrics(NULL, NULL, NULL, &r);
    _expect_body(&r, 200, METRICS_CT, "");
    pcv_rest_render_metrics("{\"error\":{\"code\":\"DAEMON_UNAVAILABLE\",\"message\":\"x\"}}", NULL, NULL, &r);
    _expect_body(&r, 200, METRICS_CT, "");
    pcv_rest_render_metrics("not json", NULL, NULL, &r);
    _expect_body(&r, 200, METRICS_CT, "");
}

                                                                                 
static void
test_version_parity(void)
{
    PcvRestRendered r = { 0 };
    pcv_rest_render_rpc_result("{\"jsonrpc\":\"2.0\",\"id\":\"rest\",\"result\":{\"version\":\"2.1.0\"}}", "Version RPC failed", &r);
    _expect_body(&r, 200, NULL, "{\"version\":\"2.1.0\"}");
    pcv_rest_render_rpc_result("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32000,\"message\":\"m\"}}", "Version RPC failed", &r);
    _expect_body(&r, 200, NULL, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32000,\"message\":\"m\"}}");
    pcv_rest_render_rpc_result("not json", "Version RPC failed", &r);
    _expect_body(&r, 200, NULL, "not json");
    pcv_rest_render_rpc_result(NULL, "Update-check RPC failed", &r);
    _expect_body(&r, 500, NULL, "{\"error\":{\"code\":\"INTERNAL_ERROR\",\"message\":\"Update-check RPC failed\"}}");
}

                                    
static guint g_open_timeout;
static gboolean g_open_read_only;
static virConnectPtr
_open_fails(const gchar *uri, gboolean read_only, guint timeout_sec, GError **error)
{
    (void)uri; (void)error;
    g_open_timeout = timeout_sec;
    g_open_read_only = read_only;
    return NULL;
}

                                                
static void
test_health_critical_when_open_fails(void)
{
    PcvRestRendered r = { 0 };
    const PcvRestHealthInput in = { .mode = PCV_REST_TLS_INTERNAL };
    pcv_rest_render_set_open_fn_for_test(_open_fails);
    pcv_rest_render_health(&in, &r);
    pcv_rest_render_set_open_fn_for_test(NULL);
    g_assert_cmpuint(g_open_timeout, ==, 3);
    g_assert_true(g_open_read_only);
    g_assert_cmpuint(r.status, ==, 503);
    g_assert_null(r.content_type);
    JsonNode *n = json_from_string(r.body, NULL);
    g_assert_nonnull(n);
    JsonObject *o = json_node_get_object(n);
    g_assert_cmpstr(json_object_get_string_member(o, "status"), ==, "critical");
    g_assert_cmpstr(json_object_get_string_member(o, "service"), ==, "purecvisorsd");
    JsonObject *lv = json_object_get_object_member(json_object_get_object_member(o, "checks"), "libvirt");
    g_assert_false(json_object_get_boolean_member(lv, "ok"));
    g_assert_cmpstr(json_object_get_string_member(lv, "error"), ==, "connection failed");
    json_node_unref(n);
    pcv_rest_rendered_clear(&r);
}

                                                                       
static void
test_health_single_flight(void)
{
    gint w1, w2, w3;
    gchar *body = NULL;
    guint code = 0;
    const gint64 t0 = 100 * G_USEC_PER_SEC;
    pcv_rest_health_reset_for_test();
    g_assert_cmpint(pcv_rest_health_enter(t0, &w1, &body, &code), ==, PCV_REST_HEALTH_PROBE);
    g_assert_cmpint(pcv_rest_health_enter(t0, &w2, &body, &code), ==, PCV_REST_HEALTH_WAIT);
    g_assert_cmpint(pcv_rest_health_enter(t0, &w3, &body, &code), ==, PCV_REST_HEALTH_WAIT);
    g_assert_null(body);
    GPtrArray *waiters = pcv_rest_health_complete("{\"a\":1}", 503, t0);
    g_assert_cmpuint(waiters->len, ==, 3);
    g_assert_true(g_ptr_array_index(waiters, 0) == &w1);
    g_assert_true(g_ptr_array_index(waiters, 2) == &w3);
    g_ptr_array_unref(waiters);
    pcv_rest_health_reset_for_test();
}

                                                                     
static void
test_health_cache_then_stale(void)
{
    gint w1, w2;
    gchar *body = NULL;
    guint code = 0;
    const gint64 t0 = 100 * G_USEC_PER_SEC;
    pcv_rest_health_reset_for_test();
    g_assert_cmpint(pcv_rest_health_enter(t0, &w1, &body, &code), ==, PCV_REST_HEALTH_PROBE);
    g_ptr_array_unref(pcv_rest_health_complete("{\"old\":1}", 200, t0));

    g_assert_cmpint(pcv_rest_health_enter(t0 + 4 * G_USEC_PER_SEC, &w2, &body, &code), ==, PCV_REST_HEALTH_CACHED);
    g_assert_cmpstr(body, ==, "{\"old\":1}");
    g_assert_cmpuint(code, ==, 200);
    g_clear_pointer(&body, g_free);

    const gint64 t1 = t0 + 5 * G_USEC_PER_SEC;
    g_assert_cmpint(pcv_rest_health_enter(t1, &w1, &body, &code), ==, PCV_REST_HEALTH_PROBE);
    g_assert_null(body);
    g_assert_cmpint(pcv_rest_health_enter(t1, &w2, &body, &code), ==, PCV_REST_HEALTH_CACHED);
    g_assert_cmpstr(body, ==, "{\"old\":1}");
    g_clear_pointer(&body, g_free);

    GPtrArray *waiters = pcv_rest_health_complete("{\"new\":1}", 503, t1);
    g_assert_cmpuint(waiters->len, ==, 1);
    g_ptr_array_unref(waiters);
    g_assert_cmpint(pcv_rest_health_enter(t1 + 1, &w2, &body, &code), ==, PCV_REST_HEALTH_CACHED);
    g_assert_cmpstr(body, ==, "{\"new\":1}");
    g_assert_cmpuint(code, ==, 503);
    g_free(body);
    pcv_rest_health_reset_for_test();
}

                                                                                       

static char g_fake_slot;
static GMutex g_cl_mu;
static GCond g_cl_cond;
static gboolean g_cl_release;
static gint g_cl_started, g_cl_done, g_open_calls_t11;

                                              
static virConnectPtr
_open_ok(const gchar *uri, gboolean read_only, guint timeout_sec, GError **error)
{
    (void)uri; (void)read_only; (void)timeout_sec; (void)error;
    g_atomic_int_inc(&g_open_calls_t11);
    return (virConnectPtr)&g_fake_slot;
}
static void
_close_blocks(virConnectPtr c)
{
    (void)c;
    g_atomic_int_inc(&g_cl_started);
    g_mutex_lock(&g_cl_mu);
    while (!g_cl_release)
        g_cond_wait(&g_cl_cond, &g_cl_mu);
    g_mutex_unlock(&g_cl_mu);
    g_atomic_int_inc(&g_cl_done);
}
static void
_close_begin(void)
{
    g_cl_release = FALSE;
    g_atomic_int_set(&g_cl_started, 0);
    g_atomic_int_set(&g_cl_done, 0);
    g_atomic_int_set(&g_open_calls_t11, 0);
    pcv_rest_health_reset_for_test();
    pcv_rest_render_set_open_fn_for_test(_open_ok);
    pcv_virt_open_set_fn_for_test(NULL, NULL, _close_blocks);
}
                                             
static void
_close_end(void)
{
    g_mutex_lock(&g_cl_mu);
    g_cl_release = TRUE;
    g_cond_broadcast(&g_cl_cond);
    g_mutex_unlock(&g_cl_mu);
    for (int i = 0; i < 500 && pcv_virt_close_detached_pending() > 0; i++)
        g_usleep(10000);
    g_assert_cmpuint(pcv_virt_close_detached_pending(), ==, 0);
    pcv_rest_render_set_open_fn_for_test(NULL);
    pcv_virt_open_set_fn_for_test(NULL, NULL, NULL);
}
static JsonObject *
_libvirt_check(const gchar *body, JsonNode **keep)
{
    *keep = json_from_string(body, NULL);
    g_assert_nonnull(*keep);
    return json_object_get_object_member(
        json_object_get_object_member(json_node_get_object(*keep), "checks"), "libvirt");
}

                                                    
static void
test_health_close_is_detached(void)
{
    PcvRestRendered r = { 0 };
    const PcvRestHealthInput in = { .mode = PCV_REST_TLS_INTERNAL };
    _close_begin();
    gint64 t0 = g_get_monotonic_time();
    pcv_rest_render_health(&in, &r);
    g_assert_cmpint(g_get_monotonic_time() - t0, <, 200 * 1000);
    JsonNode *n;
    JsonObject *lv = _libvirt_check(r.body, &n);
    g_assert_true(json_object_get_boolean_member(lv, "ok"));
    json_node_unref(n);
    pcv_rest_rendered_clear(&r);
    g_assert_cmpuint(pcv_virt_close_detached_pending(), ==, 1);
    _close_end();
    g_assert_cmpint(g_atomic_int_get(&g_cl_done), ==, 1);
}

                                                    
static void
test_health_close_backlog_critical(void)
{
    PcvRestRendered r = { 0 };
    const PcvRestHealthInput in = { .mode = PCV_REST_TLS_INTERNAL };
    _close_begin();
    for (int i = 0; i < PCV_VIRT_CLOSE_DETACHED_MAX; i++)
        pcv_virt_close_detached((virConnectPtr)&g_fake_slot);
    g_assert_cmpuint(pcv_virt_close_detached_pending(), ==, PCV_VIRT_CLOSE_DETACHED_MAX);
    pcv_rest_render_health(&in, &r);
    g_assert_cmpint(g_atomic_int_get(&g_open_calls_t11), ==, 0);
    g_assert_cmpuint(r.status, ==, 503);
    JsonNode *n;
    JsonObject *lv = _libvirt_check(r.body, &n);
    g_assert_false(json_object_get_boolean_member(lv, "ok"));
    g_assert_cmpstr(json_object_get_string_member(lv, "error"), ==, "previous libvirt connections are still closing");
    json_node_unref(n);
    pcv_rest_rendered_clear(&r);
    _close_end();

    _close_begin();
    pcv_rest_render_health(&in, &r);
    g_assert_cmpint(g_atomic_int_get(&g_open_calls_t11), ==, 1);
    pcv_rest_rendered_clear(&r);
    _close_end();
}

                                                 
static gint g_vms_calls, g_vms_sleep_ms, g_count_calls;
static JsonArray *
_vms_slow(void)
{
    g_atomic_int_inc(&g_vms_calls);
    g_usleep((gulong)g_atomic_int_get(&g_vms_sleep_ms) * 1000);
    JsonArray *a = json_array_new();
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "name", "late");
    json_array_add_object_element(a, o);
    return a;
}
static gint
_count_slow(void)
{
    g_atomic_int_inc(&g_count_calls);
    g_usleep((gulong)g_atomic_int_get(&g_vms_sleep_ms) * 1000);
    return 7;
}
static void
_internal_wait_idle(void)
{
                                                        
    g_usleep(1300 * 1000);
    pcv_rest_render_set_internal_fns_for_test(NULL, NULL, 0);
}

                                                   
static void
test_internal_vms_deadline(void)
{
    PcvRestRendered r = { 0 };
    g_atomic_int_set(&g_vms_sleep_ms, 1000);
    g_atomic_int_set(&g_vms_calls, 0);
    pcv_rest_render_set_internal_fns_for_test(_vms_slow, _count_slow, 100);
    g_test_expect_message("rest_render", G_LOG_LEVEL_WARNING,
                          "*/internal/vms: libvirt query exceeded 10 s; returning the unavailable result*");
    gint64 t0 = g_get_monotonic_time();
    pcv_rest_render_internal_vms(&r);
    g_assert_cmpint(g_get_monotonic_time() - t0, <, 200 * 1000);
    g_test_assert_expected_messages();
    g_assert_cmpuint(r.status, ==, 200);
    g_assert_cmpstr(r.body, ==, "[]");
    pcv_rest_rendered_clear(&r);
    _internal_wait_idle();
}

                                                                    
static void
test_internal_telemetry_deadline(void)
{
    PcvRestRendered r = { 0 };
    g_atomic_int_set(&g_vms_sleep_ms, 1000);
    g_atomic_int_set(&g_count_calls, 0);
    pcv_rest_render_set_internal_fns_for_test(_vms_slow, _count_slow, 100);
    g_test_expect_message("rest_render", G_LOG_LEVEL_WARNING,
                          "*/internal/telemetry: libvirt query exceeded 10 s; returning the unavailable result*");
    gint64 t0 = g_get_monotonic_time();
    pcv_rest_render_internal_telemetry(&r);
    g_assert_cmpint(g_get_monotonic_time() - t0, <, 200 * 1000);
    g_test_assert_expected_messages();
    JsonNode *n = json_from_string(r.body, NULL);
    g_assert_nonnull(n);
    JsonObject *o = json_node_get_object(n);
    g_assert_false(json_object_has_member(o, "vm_count"));
    g_assert_true(json_object_has_member(o, "cpu_percent"));
    g_assert_true(json_object_has_member(o, "mem_total_kb"));
    json_node_unref(n);
    pcv_rest_rendered_clear(&r);
    _internal_wait_idle();
}

                                                
static void
test_internal_abandoned_cap(void)
{
    PcvRestRendered r = { 0 };
    g_atomic_int_set(&g_vms_sleep_ms, 1000);
    g_atomic_int_set(&g_vms_calls, 0);
    pcv_rest_render_set_internal_fns_for_test(_vms_slow, _count_slow, 50);
    for (int i = 0; i < 2; i++) {
        g_test_expect_message("rest_render", G_LOG_LEVEL_WARNING, "*/internal/vms: libvirt query exceeded 10 s*");
        pcv_rest_render_internal_vms(&r);
        pcv_rest_rendered_clear(&r);
    }
    g_test_assert_expected_messages();
    g_assert_cmpint(g_atomic_int_get(&g_vms_calls), ==, 2);
    pcv_rest_render_internal_vms(&r);                                         
    g_assert_cmpint(g_atomic_int_get(&g_vms_calls), ==, 2);
    g_assert_cmpstr(r.body, ==, "[]");
    pcv_rest_rendered_clear(&r);
    _internal_wait_idle();
}

                                      
static JsonArray *
_vms_fast(void)
{
    JsonArray *a = json_array_new();
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "name", "fast-vm");
    json_array_add_object_element(a, o);
    return a;
}
static gint _count_fast(void) { return 7; }
static gint _count_fail(void) { return -1; }

static void
test_internal_normal_path(void)
{
    PcvRestRendered r = { 0 };
    pcv_rest_render_set_internal_fns_for_test(_vms_fast, _count_fast, 5000);
    pcv_rest_render_internal_vms(&r);
    g_assert_cmpuint(r.status, ==, 200);
    g_assert_cmpstr(r.body, ==, "[{\"name\":\"fast-vm\"}]");
    pcv_rest_rendered_clear(&r);

    pcv_rest_render_internal_telemetry(&r);
    JsonNode *n = json_from_string(r.body, NULL);
    g_assert_nonnull(n);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(n), "vm_count"), ==, 7);
    json_node_unref(n);
    pcv_rest_rendered_clear(&r);

                                                      
    pcv_rest_render_set_internal_fns_for_test(_vms_fast, _count_fail, 5000);
    pcv_rest_render_internal_telemetry(&r);
    n = json_from_string(r.body, NULL);
    g_assert_nonnull(n);
    g_assert_false(json_object_has_member(json_node_get_object(n), "vm_count"));
    json_node_unref(n);
    pcv_rest_rendered_clear(&r);
    pcv_rest_render_set_internal_fns_for_test(NULL, NULL, 0);
}

                                                                        
void pcv_vm_read_set_owner_tenant_fn_for_test(const gchar *(*fn)(const gchar *owner));

static const gchar *
_rr_owner_tenant(const gchar *owner)
{
    return g_strcmp0(owner, "alice") == 0 ? "ta" : (g_strcmp0(owner, "carol") == 0 ? "tb" : NULL);
}

                                                                       
static void
_rr_publish_map(void)
{
    PcvVmDiagSnapshot *s = g_new0(PcvVmDiagSnapshot, 1);
    s->state = PCV_VM_DIAG_S_OK;
    s->observed_mono_us = g_get_monotonic_time() - 1000000;
    s->configs = g_ptr_array_new_with_free_func(pcv_vm_diag_config_free);
    static const struct { const gchar *name, *uuid, *owner; } rows[] = {
        { "vm-a", "aaaaaaaa-0000-0000-0000-0000000000a1", "alice" },
        { "vm-b", "bbbbbbbb-0000-0000-0000-0000000000b1", "carol" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        PcvVmDiagConfig *c = g_new0(PcvVmDiagConfig, 1);
        c->name = g_strdup(rows[i].name);
        c->uuid = g_strdup(rows[i].uuid);
        c->owner = g_strdup(rows[i].owner);
        c->domain_id = -1;
        c->state = -1;
        g_ptr_array_add(s->configs, c);
    }
    pcv_vm_diag_store_publish(s);
}

#define FLEET3 \
    "{\"result\":{\"host\":{\"disk_percent\":1.0},\"fleet\":[" \
    "{\"name\":\"vm-a\",\"vcpu\":4,\"mem_max_mb\":4096.0,\"mem_used_mb\":1024.0,\"state\":\"RUNNING\"}," \
    "{\"name\":\"vm-b\",\"vcpu\":2,\"mem_max_mb\":2048.0,\"mem_used_mb\":512.0,\"state\":\"RUNNING\"}," \
    "{\"name\":\"vm-c\",\"vcpu\":1,\"mem_max_mb\":1024.0,\"mem_used_mb\":256.0,\"state\":\"RUNNING\"}]}}"

static PcvVmReadScope *
_rr_viewer(void)
{
    PcvResCaller c = { .role = PCV_ROLE_VIEWER, .subject = (gchar *)"zed", .tenant = (gchar *)"ta" };
    return pcv_vm_read_scope_new(&c);
}

                                                        
static void
test_metrics_scope_direct_series(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_rr_owner_tenant);
    _rr_publish_map();
    pcv_test_prom_render_set(NULL);
    PcvVmReadScope *s = _rr_viewer();
    PcvVmOwnerMap *m = pcv_vm_owner_map_current(g_get_monotonic_time());
    PcvRestRendered r = { 0 };
    pcv_rest_render_metrics(FLEET3, s, m, &r);
    g_assert_nonnull(strstr(r.body, "purecvisor_vm_vcpu{vm=\"vm-a\"} 4\n"));
    g_assert_nonnull(strstr(r.body, "purecvisor_vm_running{vm=\"vm-a\"} 1\n"));
    g_assert_null(strstr(r.body, "vm=\"vm-b\""));
    g_assert_null(strstr(r.body, "vm=\"vm-c\""));
    for (const gchar *const *t = (const gchar *const[]){ "# TYPE purecvisor_vm_vcpu gauge", "# TYPE purecvisor_vm_memory_max_mb gauge",
                                                         "# TYPE purecvisor_vm_memory_used_mb gauge", "# TYPE purecvisor_vm_running gauge",
                                                         "purecvisor_host_disk_percent 1.00", NULL }; *t; t++)
        g_assert_nonnull(strstr(r.body, *t));
    pcv_rest_rendered_clear(&r);
    pcv_vm_owner_map_free(m);
    pcv_vm_read_scope_free(s);
    pcv_vm_diag_store_publish(NULL);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

#define REG_LINES \
    "# HELP vm_metric x\n# TYPE vm_metric gauge\nhost_metric 1\nvm_metric{vm=\"vm-a\"} 1\nvm_metric{vm=\"vm-b\"} 2\n" \
    "vm_id_metric{vm_id=\"aaaaaaaa-0000-0000-0000-0000000000a1\"} 3\nvm_name_metric{vm_name=\"vm-a\",kind=\"vm\"} 4\n" \
    "psi_metric{vm_name=\"ct1\",kind=\"container\"} 5\nesc_metric{vm=\"vm\\\"q\"} 6\nbroken_metric{vm=\"vm-a} 7\n"

                                                                                           
                  
static void
test_metrics_scope_registry_lines(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_rr_owner_tenant);
    _rr_publish_map();
    pcv_test_prom_render_set(REG_LINES);
    PcvVmReadScope *s = _rr_viewer();
    PcvVmOwnerMap *m = pcv_vm_owner_map_current(g_get_monotonic_time());
    PcvRestRendered r = { 0 };
    pcv_rest_render_metrics(NULL, s, m, &r);
    static const gchar *const keep[] = { "# HELP vm_metric x\n", "# TYPE vm_metric gauge\n", "\nhost_metric 1\n", "vm_metric{vm=\"vm-a\"} 1\n",
                                         "vm_id_metric{vm_id=\"aaaaaaaa-0000-0000-0000-0000000000a1\"} 3\n",
                                         "vm_name_metric{vm_name=\"vm-a\",kind=\"vm\"} 4\n",
                                         "psi_metric{vm_name=\"ct1\",kind=\"container\"} 5\n" };
    for (guint i = 0; i < G_N_ELEMENTS(keep); i++)
        if (!strstr(r.body, keep[i])) g_error("kept line missing: %s", keep[i]);
    g_assert_null(strstr(r.body, "vm=\"vm-b\""));
    g_assert_null(strstr(r.body, "esc_metric"));
    g_assert_null(strstr(r.body, "broken_metric"));
    pcv_rest_rendered_clear(&r);
    pcv_vm_owner_map_free(m);
    pcv_vm_read_scope_free(s);
    pcv_test_prom_render_set(NULL);
    pcv_vm_diag_store_publish(NULL);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

                                            
static void
test_metrics_map_absent_drops_vm(void)
{
    pcv_vm_diag_store_publish(NULL);
    pcv_test_prom_render_set(REG_LINES);
    PcvVmReadScope *s = _rr_viewer();
    PcvRestRendered r = { 0 };
    pcv_rest_render_metrics(FLEET3, s, NULL, &r);
    g_assert_null(strstr(r.body, "{vm=\"vm-"));
    g_assert_null(strstr(r.body, "vm_id_metric"));
    g_assert_null(strstr(r.body, "vm_name_metric"));
    g_assert_nonnull(strstr(r.body, "\nhost_metric 1\n"));
    g_assert_nonnull(strstr(r.body, "psi_metric{vm_name=\"ct1\",kind=\"container\"} 5\n"));
    pcv_rest_rendered_clear(&r);
    pcv_vm_read_scope_free(s);
    pcv_test_prom_render_set(NULL);
}

                                                        
static void
test_metrics_admin_unchanged(void)
{
    pcv_vm_diag_store_publish(NULL);
    pcv_test_prom_render_set(REG_LINES);
    PcvResCaller c = { .role = PCV_ROLE_ADMIN, .subject = (gchar *)"root" };
    PcvVmReadScope *s = pcv_vm_read_scope_new(&c);
    PcvRestRendered a = { 0 }, b = { 0 };
    pcv_rest_render_metrics(FLEET3, NULL, NULL, &a);
    pcv_rest_render_metrics(FLEET3, s, NULL, &b);
    g_assert_cmpstr(a.body, ==, b.body);
    g_assert_nonnull(strstr(a.body, "broken_metric"));                            
    pcv_rest_rendered_clear(&a);
    pcv_rest_rendered_clear(&b);
    pcv_vm_read_scope_free(s);
    pcv_test_prom_render_set(NULL);
}

void
test_rest_public_render_register(void)
{
    g_test_add_func("/rest_render/metrics_from_fixture", test_metrics_from_fixture);
    g_test_add_func("/rest_render/metrics_format_contract", test_metrics_format_contract);
    g_test_add_func("/rest_render/sample_age_series", test_sample_age_series);
    g_test_add_func("/rest_render/metrics_label_escape", test_metrics_label_escape);
    g_test_add_func("/rest_render/metrics_daemon_unavailable", test_metrics_daemon_unavailable);
    g_test_add_func("/rest_render/version_parity", test_version_parity);
    g_test_add_func("/rest_render/health_critical_when_open_fails", test_health_critical_when_open_fails);
    g_test_add_func("/rest_render/health_single_flight", test_health_single_flight);
    g_test_add_func("/rest_render/health_cache_then_stale", test_health_cache_then_stale);
    g_test_add_func("/rest_render/health_close_is_detached", test_health_close_is_detached);
    g_test_add_func("/rest_render/health_close_backlog_critical", test_health_close_backlog_critical);
    g_test_add_func("/rest_render/internal_vms_deadline", test_internal_vms_deadline);
    g_test_add_func("/rest_render/internal_telemetry_deadline", test_internal_telemetry_deadline);
    g_test_add_func("/rest_render/internal_abandoned_cap", test_internal_abandoned_cap);
    g_test_add_func("/rest_render/internal_normal_path", test_internal_normal_path);
    g_test_add_func("/rest_render/metrics_scope_direct_series", test_metrics_scope_direct_series);
    g_test_add_func("/rest_render/metrics_scope_registry_lines", test_metrics_scope_registry_lines);
    g_test_add_func("/rest_render/metrics_map_absent_drops_vm", test_metrics_map_absent_drops_vm);
    g_test_add_func("/rest_render/metrics_admin_unchanged", test_metrics_admin_unchanged);
}
