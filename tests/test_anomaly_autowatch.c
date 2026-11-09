                                                                                         
                                                                                                
                                                                         
                                                                   
                               
                                 
  
                                                          
                      
  
                                                                               
                 
                                                                               
                                                                    
  
                                                            
                                                                  
                                         
                                                      
                                                  
                                                                  
                                                           
                     
                                                       
                                                                   
                                            
  
                  
                                                    
                                                                
                                                         
                                                         
                                          
  
                                                                      
                                                              
                                                                               
   
#include <glib.h>
#include <math.h>
#include <string.h>
#include <json-glib/json-glib.h>                            

#include "modules/ai/anomaly_detector.h"
#include "modules/ai/self_healing.h"
#include "modules/daemons/cgroup_psi.h"
#include "modules/daemons/prometheus_exporter.h"                        
#include "utils/pcv_log.h"                              

                                                                      
                                                 
#define FIXED_WATCHES 10

                                                        
                                                     
                                       
#define EXPECT_MAX_AUTO 64
                                           
#define EXPECT_RECLAIM_MISSES 3

                                                                          

                                              
                                                       
static void
_ensure_init(void)
{
    static gboolean done = FALSE;
    if (!done) {
        pcv_anomaly_init();
        done = TRUE;
    }
                                             
                                                
                                                
    for (int i = 0; i < EXPECT_RECLAIM_MISSES + 1; i++)
        pcv_anomaly_sync_auto_watches("", NULL);
    g_assert_cmpint(pcv_anomaly_watch_count(), ==, FIXED_WATCHES);
}

                                                  
                                                                
                                                              
                                                
                      
#define EXPECT_LABELS_BUF 160

                                                                
#define PSI_NAME_MAX_LEN (PCV_CGROUP_PSI_NAME_MAX - 1)

                                                    
                                                                              
static gchar *
_render_lines(int n)
{
    GString *s = g_string_new("# HELP purecvisor_cgroup_pressure_memory_full_avg10 psi\n"
                              "# TYPE purecvisor_cgroup_pressure_memory_full_avg10 gauge\n");
    for (int i = 0; i < n; i++)
        g_string_append_printf(s, "%s{vm_name=\"vm%d\",kind=\"vm\"} %.17g\n",
                               PCV_ANOMALY_AUTO_METRIC_MEM_FULL, i, 1.5);
    return g_string_free(s, FALSE);
}

void pcv_test_self_healing_ensure_init(void);
void pcv_test_anomaly_scores_reset(void);
gboolean pcv_test_anomaly_score_seen(const gchar *labels);

static void
test_read_value_labeled_watch_sums_series(void)
{
    const gchar *render =
        "# TYPE node_disk_io_time_seconds_total gauge\n"
        "node_disk_io_time_seconds_total{device=\"nvme0n1\"} 12.5\n"
        "node_disk_io_time_seconds_total{device=\"sda\"} 2.5\n"
        "# TYPE node_disk_io_time_weighted_seconds_total gauge\n"
        "node_disk_io_time_weighted_seconds_total{device=\"nvme0n1\"} 9000\n"
        "# TYPE node_disk_io_time_seconds_total_extra gauge\n"
        "node_disk_io_time_seconds_total_extra{device=\"sda\"} 7000\n"
        "node_disk_io_time_seconds_total_extra 7000\n";

    g_assert_cmpfloat(pcv_anomaly_read_value(render, "node_disk_io_time_seconds_total", ""), ==, 15.0);
    g_assert_cmpfloat(pcv_anomaly_read_value(render, "node_disk_io_time_seconds_total", NULL), ==, 15.0);
    g_assert_cmpfloat(pcv_anomaly_read_value(render, "node_disk_io_time_weighted_seconds_total", ""), ==, 9000.0);

    g_assert_cmpfloat(pcv_anomaly_read_value("x{a=\"1\"} 1\nx{a=\"2\"} 2", "x", ""), ==, 3.0);
}

static void
test_read_value_type_line_and_prefix_are_not_values(void)
{

    const gchar *only_type = "# TYPE node_vmstat_pswpout gauge\n# HELP node_vmstat_pswpout x\n";
    g_assert_true(isnan(pcv_anomaly_read_value(only_type, "node_vmstat_pswpout", "")));

    g_assert_true(isnan(pcv_anomaly_read_value("other{m=\"node_x\"} 5\nmy_node_x 6\n", "node_x", "")));

    g_assert_true(isnan(pcv_anomaly_read_value("node_x_extra 5\nnode_x_extra{a=\"1\"} 6\n", "node_x", "")));
    g_assert_true(isnan(pcv_anomaly_read_value(NULL, "node_x", "")));
    g_assert_true(isnan(pcv_anomaly_read_value("", "node_x", "")));
    g_assert_true(isnan(pcv_anomaly_read_value("node_x{a=\"1\"} abc\n", "node_x", "")));
}

static void
test_read_value_unlabeled_line_wins_over_sum(void)
{

    g_assert_cmpfloat(pcv_anomaly_read_value("x 5\nx{a=\"1\"} 3\n", "x", ""), ==, 5.0);
    g_assert_cmpfloat(pcv_anomaly_read_value("x{a=\"1\"} 3\nx 5\n", "x", ""), ==, 5.0);
}

static void
test_read_value_labeled_filter_is_exact(void)
{
    const gchar *render =
        "node_hwmon_temp_celsius{chip=\"coretemp\",sensor=\"temp1\"} 41\n"
        "node_hwmon_temp_celsius{chip=\"coretemp\",sensor=\"temp10\"} 99\n"
        "purecvisor_rpc_duration_ms{method=\"vm.list\"} 3.5\n";
    g_assert_cmpfloat(pcv_anomaly_read_value(render, "node_hwmon_temp_celsius",
                                             "chip=\"coretemp\",sensor=\"temp1\""), ==, 41.0);
    g_assert_true(isnan(pcv_anomaly_read_value(render, "node_hwmon_temp_celsius", "chip=\"coretemp\"")));
    g_assert_cmpfloat(pcv_anomaly_read_value(render, "purecvisor_rpc_duration_ms", "method=\"vm.list\""), ==, 3.5);

    g_assert_cmpfloat(pcv_anomaly_read_value("x{a=\"b c} d\"} 7\n", "x", "a=\"b c} d\""), ==, 7.0);
}

static void
test_read_value_skips_non_finite_series(void)
{

    g_assert_cmpfloat(pcv_anomaly_read_value("x{a=\"1\"} nan\nx{a=\"2\"} 2\nx{a=\"3\"} inf\n", "x", ""), ==, 2.0);
    g_assert_true(isnan(pcv_anomaly_read_value("x{a=\"1\"} nan\n", "x", "")));
    g_assert_true(isnan(pcv_anomaly_read_value("x inf\n", "x", "")));
}

static gchar *
_render_fixed_watches(gdouble counter, gdouble jitter)
{
    GString *s = g_string_new(NULL);
    gdouble v = 100.0 + jitter;
    gdouble d = counter;
    g_string_append_printf(s, "# TYPE purecvisor_host_cpu_percent gauge\npurecvisor_host_cpu_percent %.17g\n", v);
    g_string_append_printf(s, "# TYPE purecvisor_host_memory_percent gauge\npurecvisor_host_memory_percent %.17g\n", v);
    g_string_append_printf(s, "# TYPE node_disk_io_time_seconds_total gauge\n"
                              "node_disk_io_time_seconds_total{device=\"nvme0n1\"} %.17g\n"
                              "node_disk_io_time_seconds_total{device=\"sda\"} %.17g\n", d, d);
    g_string_append_printf(s, "# TYPE node_disk_io_time_weighted_seconds_total gauge\n"
                              "node_disk_io_time_weighted_seconds_total{device=\"sda\"} %.17g\n", v);
    g_string_append_printf(s, "# TYPE node_network_receive_errs_total gauge\n"
                              "node_network_receive_errs_total{device=\"lo\"} 0\n"
                              "node_network_receive_errs_total{device=\"eno1\"} %.17g\n", d);
    g_string_append_printf(s, "# TYPE purecvisor_rpc_duration_ms gauge\npurecvisor_rpc_duration_ms{method=\"vm.list\"} %.17g\n", v);
    g_string_append_printf(s, "# TYPE node_vmstat_pswpout gauge\nnode_vmstat_pswpout %.17g\n", v);
    g_string_append_printf(s, "# TYPE node_pressure_cpu_some_seconds_total gauge\nnode_pressure_cpu_some_seconds_total %.17g\n", v);
    g_string_append_printf(s, "# TYPE node_hwmon_temp_celsius gauge\n"
                              "node_hwmon_temp_celsius{chip=\"coretemp\",sensor=\"temp1\"} %.17g\n"
                              "node_hwmon_temp_celsius{chip=\"k10temp\",sensor=\"temp1\"} %.17g\n", v, v);
    g_string_append_printf(s, "# TYPE node_nf_conntrack_entries gauge\nnode_nf_conntrack_entries %.17g\n", v);
    return g_string_free(s, FALSE);
}

static void
test_fixed_watches_write_score_from_real_render(void)
{
    _ensure_init();
    pcv_anomaly_reset_baseline();
    pcv_test_anomaly_scores_reset();

    for (int cycle = 0; cycle < 2; cycle++) {
        gchar *render = _render_fixed_watches(100.0 + cycle, 0.0);
        pcv_anomaly_evaluate_text(render);
        g_free(render);
    }
    static const gchar *const expected[] = {
        "purecvisor_host_cpu_percent", "purecvisor_host_memory_percent",
        "node_disk_io_time_seconds_total", "node_network_receive_errs_total",
        "purecvisor_rpc_duration_ms", "node_vmstat_pswpout", "node_pressure_cpu_some_seconds_total",
        "node_hwmon_temp_celsius", "node_nf_conntrack_entries",
    };
    guint missing = 0;
    for (guint i = 0; i < G_N_ELEMENTS(expected); i++) {
        gchar *labels = g_strdup_printf("metric=\"%s\"", expected[i]);
        gboolean seen = pcv_test_anomaly_score_seen(labels);
        g_test_message("감시 %s: 점수 게이지 %s", expected[i], seen ? "있음" : "없음");
        if (!seen) missing++;
        g_free(labels);
    }
    g_assert_cmpuint(missing, ==, 0);
    pcv_anomaly_reset_baseline();
}

static gdouble
_counter_warmup(gdouble start, int cycles, gdouble step)
{
    gdouble counter = start;
    for (int cycle = 0; cycle < cycles; cycle++) {
        counter += step + ((cycle % 2) ? 0.5 : -0.5);
        gchar *render = _render_fixed_watches(counter, (cycle % 2) ? 0.5 : -0.5);
        pcv_anomaly_evaluate_text(render);
        g_free(render);
    }
    return counter;
}

static void
test_labeled_watches_reach_policies_after_warmup(void)
{
    _ensure_init();
    pcv_test_self_healing_ensure_init();
    pcv_anomaly_reset_baseline();
    gdouble counter = _counter_warmup(100.0, 13, 10.0);
    gchar *quiet = pcv_anomaly_get_recent_json();
    g_assert_cmpstr(quiet, ==, "[]");
    g_free(quiet);

    gchar *spike = _render_fixed_watches(counter + 100000.0, 0.0);
    pcv_anomaly_evaluate_text(spike);
    g_free(spike);

    gchar *recent = pcv_anomaly_get_recent_json();
    g_assert_nonnull(strstr(recent, "\"metric\":\"node_disk_io_time_seconds_total\""));
    g_assert_nonnull(strstr(recent, "\"metric\":\"node_network_receive_errs_total\""));
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, recent, -1, NULL));
    g_assert_cmpuint(json_array_get_length(json_node_get_array(json_parser_get_root(parser))), ==, 2);
    g_object_unref(parser);
    g_free(recent);

    gchar *hist = pcv_healing_get_history_json();
    g_assert_nonnull(strstr(hist, "\"target\":\"disk-saturated\""));
    g_assert_nonnull(strstr(hist, "\"target\":\"net-errors\""));
    g_free(hist);
    pcv_anomaly_reset_baseline();
}

static void
test_counter_idle_to_load_fires(void)
{
    _ensure_init();
    pcv_anomaly_reset_baseline();
    gdouble counter = _counter_warmup(1000.0, 30, 10.0);
    gchar *quiet = pcv_anomaly_get_recent_json();
    g_assert_cmpstr(quiet, ==, "[]");
    g_free(quiet);
    gchar *load = _render_fixed_watches(counter + 30.0, 0.0);
    pcv_anomaly_evaluate_text(load);
    g_free(load);
    gchar *recent = pcv_anomaly_get_recent_json();
    g_assert_nonnull(strstr(recent, "\"metric\":\"node_disk_io_time_seconds_total\""));
    g_free(recent);
    pcv_anomaly_reset_baseline();
}

static void
test_counter_decrease_is_quiet(void)
{
    _ensure_init();
    pcv_anomaly_reset_baseline();
    gdouble counter = _counter_warmup(1000000.0, 20, 10.0);
    (void)counter;
    gchar *reset = _render_fixed_watches(5.0, 0.0);
    pcv_anomaly_evaluate_text(reset);
    g_free(reset);
    _counter_warmup(5.0, 3, 10.0);
    gchar *recent = pcv_anomaly_get_recent_json();
    g_assert_cmpstr(recent, ==, "[]");
    g_free(recent);
    pcv_anomaly_reset_baseline();
}

static void
test_counter_vmstat_pswpout_is_cumulative(void)
{
    _ensure_init();
    pcv_test_self_healing_ensure_init();
    pcv_anomaly_reset_baseline();
    gdouble counter = 1000.0;
    for (int cycle = 0; cycle < 30; cycle++) {
        counter += 10.0 + ((cycle % 2) ? 0.5 : -0.5);
        gchar *render = g_strdup_printf("node_vmstat_pswpout %.17g\n", counter);
        pcv_anomaly_evaluate_text(render);
        g_free(render);
    }
    gchar *quiet = pcv_anomaly_get_recent_json();
    g_assert_cmpstr(quiet, ==, "[]");
    g_free(quiet);
    gchar *load = g_strdup_printf("node_vmstat_pswpout %.17g\n", counter + 30.0);
    pcv_anomaly_evaluate_text(load);
    g_free(load);
    gchar *recent = pcv_anomaly_get_recent_json();
    g_assert_nonnull(strstr(recent, "\"metric\":\"node_vmstat_pswpout\""));
    g_free(recent);

    pcv_anomaly_reset_baseline();
    counter = 1000000.0;
    for (int cycle = 0; cycle < 20; cycle++) {
        counter += 10.0 + ((cycle % 2) ? 0.5 : -0.5);
        gchar *render = g_strdup_printf("node_vmstat_pswpout %.17g\n", counter);
        pcv_anomaly_evaluate_text(render);
        g_free(render);
    }
    pcv_anomaly_evaluate_text("node_vmstat_pswpout 5\n");
    recent = pcv_anomaly_get_recent_json();
    g_assert_cmpstr(recent, ==, "[]");
    g_free(recent);
    pcv_anomaly_reset_baseline();
}

static void
test_counter_load_end_is_quiet(void)
{
    _ensure_init();
    pcv_anomaly_reset_baseline();
    gdouble counter = _counter_warmup(1000.0, 30, 1000.0);
    gchar *idle = _render_fixed_watches(counter + 1.0, 0.0);
    pcv_anomaly_evaluate_text(idle);
    g_free(idle);
    gchar *recent = pcv_anomaly_get_recent_json();
    g_assert_cmpstr(recent, ==, "[]");
    g_free(recent);
    pcv_anomaly_reset_baseline();
}

static void
test_gauge_downward_still_fires(void)
{
    _ensure_init();
    pcv_test_self_healing_ensure_init();
    pcv_anomaly_reset_baseline();
    for (int cycle = 0; cycle < 30; cycle++) {
        gchar *render = g_strdup_printf("node_nf_conntrack_entries %.17g\n", 5000.0 + ((cycle % 2) ? 0.5 : -0.5));
        pcv_anomaly_evaluate_text(render);
        g_free(render);
    }
    pcv_anomaly_evaluate_text("node_nf_conntrack_entries 1\n");
    gchar *recent = pcv_anomaly_get_recent_json();
    g_assert_nonnull(strstr(recent, "\"metric\":\"node_nf_conntrack_entries\""));
    g_free(recent);
    pcv_anomaly_reset_baseline();
}

static void
test_counter_gap_does_not_double(void)
{
    _ensure_init();
    pcv_anomaly_reset_baseline();
    gdouble counter = _counter_warmup(1000.0, 20, 10.0);
    pcv_anomaly_evaluate_text("# 값 없음\n");

    gchar *after = _render_fixed_watches(counter + 500.0, 0.0);
    pcv_anomaly_evaluate_text(after);
    g_free(after);
    gchar *recent = pcv_anomaly_get_recent_json();
    g_assert_cmpstr(recent, ==, "[]");
    g_free(recent);
    pcv_anomaly_reset_baseline();
}

                                                                           

static void
test_extract_vm_name_basic(void)
{
    gchar out[PCV_ANOMALY_VM_NAME_MAX];

                                   
    g_assert_true(pcv_anomaly_extract_vm_name("vm_name=\"web-prod\",kind=\"vm\"",
                                              out, sizeof(out)));
    g_assert_cmpstr(out, ==, "web-prod");

                         
    g_assert_true(pcv_anomaly_extract_vm_name("kind=\"container\",vm_name=\"ct-1\"",
                                              out, sizeof(out)));
    g_assert_cmpstr(out, ==, "ct-1");

                                  
    g_assert_true(pcv_anomaly_extract_vm_name("a=\"1\", vm_name=\"mid\" , kind=\"vm\"",
                                              out, sizeof(out)));
    g_assert_cmpstr(out, ==, "mid");

                                                
                                                
    g_assert_true(pcv_anomaly_extract_vm_name("vm_name=\"a,b\",kind=\"vm\"",
                                              out, sizeof(out)));
    g_assert_cmpstr(out, ==, "a,b");
}

static void
test_extract_vm_name_key_collisions(void)
{
    gchar out[PCV_ANOMALY_VM_NAME_MAX];

                                                    
    g_assert_false(pcv_anomaly_extract_vm_name("kind=\"vm\"", out, sizeof(out)));
    g_assert_cmpstr(out, ==, "");

                                           
    g_assert_false(pcv_anomaly_extract_vm_name("xvm_name=\"evil\"", out, sizeof(out)));
                                    
    g_assert_false(pcv_anomaly_extract_vm_name("vm_names=\"evil\"", out, sizeof(out)));
    g_assert_false(pcv_anomaly_extract_vm_name("vm_name_x=\"evil\"", out, sizeof(out)));

                                                     
    g_assert_true(pcv_anomaly_extract_vm_name("xvm_name=\"evil\",vm_name=\"real\"",
                                              out, sizeof(out)));
    g_assert_cmpstr(out, ==, "real");

                                                       
    g_assert_false(pcv_anomaly_extract_vm_name("note=\"vm_name=spoof\",kind=\"vm\"",
                                               out, sizeof(out)));
}

static void
test_extract_vm_name_malformed(void)
{
    gchar out[PCV_ANOMALY_VM_NAME_MAX];

                     
    g_assert_false(pcv_anomaly_extract_vm_name(NULL, out, sizeof(out)));
    g_assert_false(pcv_anomaly_extract_vm_name("", out, sizeof(out)));

                                                 
    g_assert_false(pcv_anomaly_extract_vm_name("vm_name=web-prod", out, sizeof(out)));
    g_assert_false(pcv_anomaly_extract_vm_name("vm_name=web,kind=\"vm\"",
                                               out, sizeof(out)));

                                                    
    g_assert_true(pcv_anomaly_extract_vm_name("junk=raw,vm_name=\"ok\"",
                                              out, sizeof(out)));
    g_assert_cmpstr(out, ==, "ok");

                   
    g_assert_false(pcv_anomaly_extract_vm_name("vm_name=\"unterminated",
                                               out, sizeof(out)));

                                 
    g_assert_false(pcv_anomaly_extract_vm_name("vm_name=\"\",kind=\"vm\"",
                                               out, sizeof(out)));

                           
    g_assert_true(pcv_anomaly_extract_vm_name("orphan,vm_name=\"after\"",
                                              out, sizeof(out)));
    g_assert_cmpstr(out, ==, "after");

                                                     
    g_assert_false(pcv_anomaly_extract_vm_name("vm_name=\"a\\\"b\"", out, sizeof(out)));
}

static void
test_extract_vm_name_buffer_limits(void)
{
    gchar small[5];                  

                     
    g_assert_true(pcv_anomaly_extract_vm_name("vm_name=\"abcd\"", small, sizeof(small)));
    g_assert_cmpstr(small, ==, "abcd");

                                                        
    g_assert_false(pcv_anomaly_extract_vm_name("vm_name=\"abcde\"", small, sizeof(small)));
    g_assert_cmpstr(small, ==, "");

                                  
    g_assert_false(pcv_anomaly_extract_vm_name("vm_name=\"x\"", small, 0));
}

                                                                  

static void
test_series_labels_basic(void)
{
    gchar out[128];
    const gchar *line = PCV_ANOMALY_AUTO_METRIC_MEM_FULL
                        "{vm_name=\"web\",kind=\"vm\"} 1.5\n";

    g_assert_true(pcv_anomaly_series_labels(line, PCV_ANOMALY_AUTO_METRIC_MEM_FULL,
                                            out, sizeof(out)));
    g_assert_cmpstr(out, ==, "vm_name=\"web\",kind=\"vm\"");
}

static void
test_series_labels_rejects_non_matches(void)
{
    gchar out[128];
    const gchar *name = PCV_ANOMALY_AUTO_METRIC_MEM_FULL;

                                            
    g_assert_false(pcv_anomaly_series_labels(
        PCV_ANOMALY_AUTO_METRIC_MEM_FULL "_extra{vm_name=\"a\"} 1\n", name,
        out, sizeof(out)));

                                       
    g_assert_false(pcv_anomaly_series_labels(
        PCV_ANOMALY_AUTO_METRIC_MEM_FULL " 1.5\n", name, out, sizeof(out)));

                                                      
    g_assert_false(pcv_anomaly_series_labels(
        "# TYPE " PCV_ANOMALY_AUTO_METRIC_MEM_FULL " gauge\n", name,
        out, sizeof(out)));

                  
    g_assert_false(pcv_anomaly_series_labels(
        "purecvisor_host_memory_percent 42\n", name, out, sizeof(out)));

                                    
    g_assert_false(pcv_anomaly_series_labels(
        PCV_ANOMALY_AUTO_METRIC_MEM_FULL "{} 1\n", name, out, sizeof(out)));

                               
    g_assert_false(pcv_anomaly_series_labels(
        PCV_ANOMALY_AUTO_METRIC_MEM_FULL "{vm_name=\"a\"\nother{x=\"1\"} 2\n", name,
        out, sizeof(out)));

                                                
    gchar tiny[8];
    g_assert_false(pcv_anomaly_series_labels(
        PCV_ANOMALY_AUTO_METRIC_MEM_FULL "{vm_name=\"web\",kind=\"vm\"} 1.5\n", name,
        tiny, sizeof(tiny)));
    g_assert_cmpstr(tiny, ==, "");

                 
    g_assert_false(pcv_anomaly_series_labels(NULL, name, out, sizeof(out)));
    g_assert_false(pcv_anomaly_series_labels("x", NULL, out, sizeof(out)));
}

                                                                   

static void
test_auto_register_and_reclaim(void)
{
    _ensure_init();

                                        
    gchar *r = _render_lines(3);
    gint dropped = -1;
    g_assert_cmpint(pcv_anomaly_sync_auto_watches(r, &dropped), ==, 3);
    g_assert_cmpint(dropped, ==, 0);
    g_assert_cmpint(pcv_anomaly_watch_count(), ==, FIXED_WATCHES + 3);
    g_free(r);

                                                
    r = _render_lines(3);
    g_assert_cmpint(pcv_anomaly_sync_auto_watches(r, &dropped), ==, 3);
    g_assert_cmpint(pcv_anomaly_watch_count(), ==, FIXED_WATCHES + 3);
    g_free(r);

                         
    r = _render_lines(4);
    g_assert_cmpint(pcv_anomaly_sync_auto_watches(r, &dropped), ==, 4);
    g_free(r);

                                        
                                              
    for (int i = 1; i < EXPECT_RECLAIM_MISSES; i++) {
        g_assert_cmpint(pcv_anomaly_sync_auto_watches("", &dropped), ==, 4);
        g_assert_cmpint(pcv_anomaly_watch_count(), ==, FIXED_WATCHES + 4);
    }
    g_assert_cmpint(pcv_anomaly_sync_auto_watches("", &dropped), ==, 0);
    g_assert_cmpint(pcv_anomaly_watch_count(), ==, FIXED_WATCHES);
}

static void
test_auto_reclaim_grace_resets_on_reappear(void)
{
    _ensure_init();

    gchar *r = _render_lines(1);
    g_assert_cmpint(pcv_anomaly_sync_auto_watches(r, NULL), ==, 1);
    g_free(r);

                                                
                                                    
    for (int i = 1; i < EXPECT_RECLAIM_MISSES; i++)
        g_assert_cmpint(pcv_anomaly_sync_auto_watches("", NULL), ==, 1);

    r = _render_lines(1);
    g_assert_cmpint(pcv_anomaly_sync_auto_watches(r, NULL), ==, 1);
    g_free(r);

                                      
    for (int i = 1; i < EXPECT_RECLAIM_MISSES; i++)
        g_assert_cmpint(pcv_anomaly_sync_auto_watches("", NULL), ==, 1);
    g_assert_cmpint(pcv_anomaly_sync_auto_watches("", NULL), ==, 0);
}

static void
test_auto_sync_null_render_is_not_a_wipe(void)
{
    _ensure_init();

    gchar *r = _render_lines(2);
    g_assert_cmpint(pcv_anomaly_sync_auto_watches(r, NULL), ==, 2);
    g_free(r);

                                             
                                      
                                          
    g_assert_cmpint(pcv_anomaly_sync_auto_watches(NULL, NULL), ==, 2);
    g_assert_cmpint(pcv_anomaly_watch_count(), ==, FIXED_WATCHES + 2);
}

static void
test_auto_slot_cap(void)
{
    _ensure_init();

                              
    const int over = 5;
    gchar *r = _render_lines(EXPECT_MAX_AUTO + over);
    gint dropped = -1;
    gint active = pcv_anomaly_sync_auto_watches(r, &dropped);
    g_free(r);

                   
    g_assert_cmpint(active, ==, EXPECT_MAX_AUTO);
                                               
    g_assert_cmpint(dropped, ==, over);
                                                  
    g_assert_cmpint(pcv_anomaly_watch_count(), ==, FIXED_WATCHES + EXPECT_MAX_AUTO);
}

                                                                    

static void
test_auto_metric_name_matches_producer(void)
{
                                                          
                                                  
                                          
    gchar produced[128];
    pcv_cgroup_psi_format_metric("memory", "full", "avg10",
                                 produced, sizeof(produced));
    g_assert_cmpstr(produced, ==, PCV_ANOMALY_AUTO_METRIC_MEM_FULL);
}

                                                            
  
                                                                    
                                                                          
                                                          
                                                            
                                  
static void test_label_json_escape_roundtrip(void)
{
    const gchar *labels = "vm_name=\"web-01\",kind=\"vm\"";
    gchar *esc = pcv_json_escape(labels);
    g_assert_nonnull(esc);

                                                  
    g_assert_nonnull(g_strstr_len(esc, -1, "\\\"web-01\\\""));

                                                      
    gchar *doc = g_strdup_printf("{\"labels\":\"%s\"}", esc);
    JsonParser *p = json_parser_new();
    GError *e = NULL;
    g_assert_true(json_parser_load_from_data(p, doc, -1, &e));
    g_assert_no_error(e);

    JsonObject *o = json_node_get_object(json_parser_get_root(p));
                                                     
    g_assert_cmpstr(json_object_get_string_member(o, "labels"), ==, labels);

    g_object_unref(p);
    g_free(doc);
    g_free(esc);
}

                                                                     

   
                                                  
  
                                                       
                                                           
                                               
                                                              
                                              
   
static void
test_score_label_budget_roundtrip_54_char_vm(void)
{
    gchar *name    = g_strnfill(54, 'a');
    gchar *dirname = g_strdup_printf("machine-qemu\\x2d3\\x2d%s.scope", name);

    PcvCgroupPsiRef ref;
    g_assert_true(pcv_cgroup_psi_ref_from_machine_dir(dirname, &ref));
    g_assert_cmpstr(ref.name, ==, name);

                                                                  
    gchar psi_labels[256];
    pcv_cgroup_psi_format_labels(&ref, psi_labels, sizeof(psi_labels));
    g_assert_cmpuint(strlen(psi_labels), ==, 74);
    g_assert_true(pcv_prom_labels_fit(psi_labels));

                                            
    gchar *line = g_strdup_printf("%s{%s} %.17g\n",
                                  PCV_ANOMALY_AUTO_METRIC_MEM_FULL, psi_labels, 1.5);
    gchar extracted[128];
    g_assert_true(pcv_anomaly_series_labels(line, PCV_ANOMALY_AUTO_METRIC_MEM_FULL,
                                            extracted, sizeof(extracted)));
    g_assert_cmpstr(extracted, ==, psi_labels);

                                                                                
    gchar *score = g_strdup_printf("metric=\"%s\",%s",
                                   PCV_ANOMALY_AUTO_METRIC_MEM_FULL, extracted);
                                                             
    g_assert_cmpuint(strlen(score), ==, 128);
    g_assert_true(pcv_prom_labels_fit(score));                       

                                                      
    gint quotes = 0;
    for (const gchar *p = score; *p; p++)
        if (*p == '"') quotes++;
    g_assert_cmpint(quotes % 2, ==, 0);
    g_assert_true(g_str_has_suffix(score, "kind=\"vm\""));

    g_free(score);
    g_free(line);
    g_free(dirname);
    g_free(name);
}

                                                               

   
                                                      
  
                                                    
                                                                    
                                                              
                                       
  
                                                                
                                                                              
                                                   
                                                          
   
static void
test_longest_producible_label_is_watchable(void)
{
    _ensure_init();

                                                        
                                         
    PcvCgroupPsiRef ref = {0};
    gchar *name = g_strnfill(PSI_NAME_MAX_LEN, 'c');
    g_strlcpy(ref.name, name, sizeof(ref.name));
    ref.kind = PCV_CGROUP_PSI_KIND_CONTAINER;                            

    gchar labels[256];
    pcv_cgroup_psi_format_labels(&ref, labels, sizeof(labels));
    g_assert_cmpuint(strlen(labels), ==, 154);                            
    g_assert_cmpuint(strlen(labels), <, EXPECT_LABELS_BUF);
    g_assert_true(pcv_prom_labels_fit(labels));                                

                                                         
                                              
    gchar *line = g_strdup_printf("%s{%s} %.17g\n",
                                  PCV_ANOMALY_AUTO_METRIC_MEM_FULL, labels, 1.5);
    g_assert_cmpint(pcv_anomaly_sync_auto_watches(line, NULL), ==, 1);
    g_assert_cmpint(pcv_anomaly_watch_count(), ==, FIXED_WATCHES + 1);

                                                       
                                                               
    gchar *score = g_strdup_printf("metric=\"%s\",%s",
                                   PCV_ANOMALY_AUTO_METRIC_MEM_FULL, labels);
    g_assert_cmpuint(strlen(score), ==, 208);
    g_assert_true(pcv_prom_labels_fit(score));

    g_free(score);
    g_free(line);
    g_free(name);
}

   
                                      
  
                                                    
                                             
                                                   
               
   
static void
test_series_labels_budget_boundary(void)
{
    _ensure_init();

                                                                           
    const gsize fixed = 20;                            

                                                                
    gchar *pad_ok = g_strnfill(EXPECT_LABELS_BUF - 1 - fixed, 'a');
    gchar *lbl_ok = g_strdup_printf("vm_name=\"%s\",kind=\"vm\"", pad_ok);
    g_assert_cmpuint(strlen(lbl_ok), ==, EXPECT_LABELS_BUF - 1);
    gchar *line_ok = g_strdup_printf("%s{%s} %.17g\n",
                                     PCV_ANOMALY_AUTO_METRIC_MEM_FULL, lbl_ok, 1.5);
    g_assert_cmpint(pcv_anomaly_sync_auto_watches(line_ok, NULL), ==, 1);

                                                    
                                                     
    _ensure_init();

                                                                 
                                                                      
                                                        
                                                     
    gchar *pad_over = g_strnfill(EXPECT_LABELS_BUF - fixed, 'a');
    gchar *lbl_over = g_strdup_printf("vm_name=\"%s\",kind=\"vm\"", pad_over);
    g_assert_cmpuint(strlen(lbl_over), ==, EXPECT_LABELS_BUF);
    gchar *line_over = g_strdup_printf("%s{%s} %.17g\n",
                                       PCV_ANOMALY_AUTO_METRIC_MEM_FULL, lbl_over, 1.5);
    g_assert_cmpint(pcv_anomaly_sync_auto_watches(line_over, NULL), ==, 0);
    g_assert_cmpint(pcv_anomaly_watch_count(), ==, FIXED_WATCHES);

    g_free(line_over);
    g_free(lbl_over);
    g_free(pad_over);
    g_free(line_ok);
    g_free(lbl_ok);
    g_free(pad_ok);
}

                                                                           

void
test_anomaly_autowatch_register(void)
{
    g_test_add_func("/anomaly/label_json_escape_roundtrip", test_label_json_escape_roundtrip);
    g_test_add_func("/anomaly/read_value_labeled_watch_sums_series", test_read_value_labeled_watch_sums_series);
    g_test_add_func("/anomaly/read_value_type_line_and_prefix", test_read_value_type_line_and_prefix_are_not_values);
    g_test_add_func("/anomaly/read_value_unlabeled_line_wins", test_read_value_unlabeled_line_wins_over_sum);
    g_test_add_func("/anomaly/read_value_labeled_filter_exact", test_read_value_labeled_filter_is_exact);
    g_test_add_func("/anomaly/read_value_skips_non_finite", test_read_value_skips_non_finite_series);
    g_test_add_func("/anomaly/fixed_watches_write_score", test_fixed_watches_write_score_from_real_render);
    g_test_add_func("/anomaly/labeled_watches_reach_policies", test_labeled_watches_reach_policies_after_warmup);
    g_test_add_func("/anomaly/counter_idle_to_load_fires", test_counter_idle_to_load_fires);
    g_test_add_func("/anomaly/counter_decrease_is_quiet", test_counter_decrease_is_quiet);
    g_test_add_func("/anomaly/counter_gap_does_not_double", test_counter_gap_does_not_double);
    g_test_add_func("/anomaly/counter_vmstat_pswpout", test_counter_vmstat_pswpout_is_cumulative);
    g_test_add_func("/anomaly/counter_load_end_is_quiet", test_counter_load_end_is_quiet);
    g_test_add_func("/anomaly/gauge_downward_still_fires", test_gauge_downward_still_fires);
    g_test_add_func("/anomaly/extract_vm_name_basic", test_extract_vm_name_basic);
    g_test_add_func("/anomaly/extract_vm_name_key_collisions", test_extract_vm_name_key_collisions);
    g_test_add_func("/anomaly/extract_vm_name_malformed", test_extract_vm_name_malformed);
    g_test_add_func("/anomaly/extract_vm_name_buffer_limits", test_extract_vm_name_buffer_limits);
    g_test_add_func("/anomaly/series_labels_basic", test_series_labels_basic);
    g_test_add_func("/anomaly/series_labels_rejects_non_matches", test_series_labels_rejects_non_matches);
    g_test_add_func("/anomaly/auto_register_and_reclaim", test_auto_register_and_reclaim);
    g_test_add_func("/anomaly/auto_reclaim_grace_resets", test_auto_reclaim_grace_resets_on_reappear);
    g_test_add_func("/anomaly/auto_sync_null_render", test_auto_sync_null_render_is_not_a_wipe);
    g_test_add_func("/anomaly/auto_slot_cap", test_auto_slot_cap);
    g_test_add_func("/anomaly/auto_metric_name_matches_producer", test_auto_metric_name_matches_producer);
    g_test_add_func("/anomaly/score_label_budget_roundtrip",
                    test_score_label_budget_roundtrip_54_char_vm);
    g_test_add_func("/anomaly/longest_producible_label_watchable",
                    test_longest_producible_label_is_watchable);
    g_test_add_func("/anomaly/series_labels_budget_boundary",
                    test_series_labels_budget_boundary);
}
