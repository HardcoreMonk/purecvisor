   
                                  
                                                                        
  
                           
                                                                                            
                                                                                                      
                                                                                                         
  
                       
                                                                                       
   
#include <glib.h>
#include <json-glib/json-glib.h>
#include <libvirt/libvirt.h>

#include "api/rest_public_render.h"
#include "modules/virt/pcv_virt_open.h"

static void
_expect_body(PcvRestRendered *r, guint status, const gchar *content_type, const gchar *body)
{
    g_assert_cmpuint(r->status, ==, status);
    g_assert_cmpstr(r->content_type, ==, content_type);
    g_assert_cmpstr(r->body, ==, body);
    pcv_rest_rendered_clear(r);
}

#define METRICS_CT "text/plain; version=0.0.4; charset=utf-8"

                                                                            
static void
test_metrics_from_fixture(void)
{
    PcvRestRendered r = { 0 };
    pcv_rest_render_metrics_reset_for_test();
    pcv_rest_render_metrics("{\"result\":{\"host\":{\"cpu_percent\":12.5,\"mem_percent\":40.25,\"disk_percent\":70.5,"
                            "\"mem_total_gb\":64.0,\"cpu_temp_c\":55.5,\"load_1\":1.25},\"fleet\":["
                            "{\"name\":\"vm-a\",\"vcpu\":4,\"mem_max_mb\":4096.0,\"mem_used_mb\":1024.0,\"state\":\"RUNNING\"},"
                            "{\"name\":\"vm-b\",\"vcpu\":2,\"mem_max_mb\":2048.0,\"mem_used_mb\":0.0,\"state\":\"SHUTOFF\"}]}}", &r);
    _expect_body(&r, 200, METRICS_CT,
    "# HELP purecvisor_host_cpu_percent Host CPU utilization\n"
    "# TYPE purecvisor_host_cpu_percent gauge\n"
    "purecvisor_host_cpu_percent 12.50\n"
    "# HELP purecvisor_host_memory_percent Host memory utilization\n"
    "# TYPE purecvisor_host_memory_percent gauge\n"
    "purecvisor_host_memory_percent 40.25\n"
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
    "purecvisor_vm_memory_used_mb{vm=\"vm-b\"} 0\n"
    "# HELP purecvisor_vm_running VM running state\n"
    "# TYPE purecvisor_vm_running gauge\n"
    "purecvisor_vm_running{vm=\"vm-a\"} 1\n"
    "purecvisor_vm_running{vm=\"vm-b\"} 0\n");
    pcv_rest_render_metrics("{\"result\":{\"host\":{\"cpu_total_ticks\":1000.0,\"cpu_idle_ticks\":800.0}}}", &r);
    _expect_body(&r, 200, METRICS_CT,
    "# HELP purecvisor_host_cpu_percent Host CPU utilization\n"
    "# TYPE purecvisor_host_cpu_percent gauge\n"
    "purecvisor_host_cpu_percent 0.00\n"
    "# HELP purecvisor_host_memory_percent Host memory utilization\n"
    "# TYPE purecvisor_host_memory_percent gauge\n"
    "purecvisor_host_memory_percent 0.00\n"
    "# HELP purecvisor_host_disk_percent Host disk utilization\n"
    "# TYPE purecvisor_host_disk_percent gauge\n"
    "purecvisor_host_disk_percent 0.00\n");
    pcv_rest_render_metrics("{\"result\":{\"host\":{\"cpu_total_ticks\":2000.0,\"cpu_idle_ticks\":1500.0}}}", &r);
    _expect_body(&r, 200, METRICS_CT,
    "# HELP purecvisor_host_cpu_percent Host CPU utilization\n"
    "# TYPE purecvisor_host_cpu_percent gauge\n"
    "purecvisor_host_cpu_percent 30.00\n"
    "# HELP purecvisor_host_memory_percent Host memory utilization\n"
    "# TYPE purecvisor_host_memory_percent gauge\n"
    "purecvisor_host_memory_percent 0.00\n"
    "# HELP purecvisor_host_disk_percent Host disk utilization\n"
    "# TYPE purecvisor_host_disk_percent gauge\n"
    "purecvisor_host_disk_percent 0.00\n");
    pcv_rest_render_metrics("{\"result\":{}}", &r);
    _expect_body(&r, 200, METRICS_CT, "");
    pcv_rest_render_metrics_reset_for_test();
}

                                                
static void
test_metrics_daemon_unavailable(void)
{
    PcvRestRendered r = { 0 };
    pcv_rest_render_metrics(NULL, &r);
    _expect_body(&r, 200, METRICS_CT, "");
    pcv_rest_render_metrics("{\"error\":{\"code\":\"DAEMON_UNAVAILABLE\",\"message\":\"x\"}}", &r);
    _expect_body(&r, 200, METRICS_CT, "");
    pcv_rest_render_metrics("not json", &r);
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

void
test_rest_public_render_register(void)
{
    g_test_add_func("/rest_render/metrics_from_fixture", test_metrics_from_fixture);
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
}
