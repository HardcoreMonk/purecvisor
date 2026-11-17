                                                                             
                                                                         
                                                                     
                                                                             
                                                       
                                                             
#include <glib.h>
#include <string.h>

#include "modules/daemons/prometheus_exporter.h"
#include "modules/daemons/vm_diag.h"
#include "modules/daemons/vm_diag_view.h"
#include "modules/auth/pcv_rbac.h"
#include "modules/resource/vm_read_scope.h"

                                             
void pcv_vm_read_set_owner_tenant_fn_for_test(const gchar *(*fn)(const gchar *owner));
guint pcv_vm_read_owner_lookups_for_test(void);

                          
static PcvVmDiagSample *
_run_sample(const gchar *name, gboolean net, guint64 rx)
{
    PcvVmDiagSample *s = g_new0(PcvVmDiagSample, 1);
    s->name = g_strdup(name);
    s->uuid = g_strdup_printf("u-%s", name);
    s->domain_id = 1;
    s->state = 1;
    s->active = TRUE;
    s->vcpu_delay = g_array_new(FALSE, TRUE, sizeof(PcvVmDiagVcpuDelay));
    s->present = PCV_VM_DIAG_B_CPU | PCV_VM_DIAG_B_VCPU | (net ? PCV_VM_DIAG_B_NET : 0);
    s->vcpus = 1;
    s->cpu_time_ns = 1000;
    s->net_count = net ? 1 : 0;
    s->rx_bytes = rx;
    s->tx_bytes = rx * 2;
    s->rx_pkts = 3;
    s->tx_pkts = 4;
    s->mono_us = 1000000;
    s->wall_ms = 1794757394000;
    return s;
}

static PcvVmDiagConfig *
_stopped_cfg(const gchar *name, const gchar *owner)
{
    PcvVmDiagConfig *c = g_new0(PcvVmDiagConfig, 1);
    c->name = g_strdup(name);
    c->uuid = g_strdup_printf("u-%s", name);
    c->owner = g_strdup(owner);
    c->state = 5;
    c->domain_id = -1;
    c->have_vcpus = TRUE;
    c->vcpus = 2;
    c->have_balloon = TRUE;
    c->maximum_kb = 1048576;
    return c;
}

                                              
static PcvVmDiagSnapshot *
_snap(GPtrArray *run, GPtrArray *cfgs)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    PcvVmDiagSnapshot *s = g_new0(PcvVmDiagSnapshot, 1);
    PcvVmDiagHostIn h = { .observed_mono_us = 1000000, .observed_wall_ms = 1794757394000,
                          .logical_cpus = 8, .mem_total_kb = 16777216 };
    pcv_vm_diag_table_step(t, run, cfgs, NULL, &h, s);
    pcv_vm_diag_table_free(t);
    if (run) g_ptr_array_unref(run);
    if (cfgs) g_ptr_array_unref(cfgs);
    return s;
}

static GPtrArray *
_runs(void)
{
    return g_ptr_array_new_with_free_func(pcv_vm_diag_sample_free);
}

static GPtrArray *
_cfgs(void)
{
    return g_ptr_array_new_with_free_func(pcv_vm_diag_config_free);
}

                                                      
                                           
static void
test_store_publish_copy(void)
{
    pcv_vm_diag_store_publish(NULL);
    g_assert_null(pcv_vm_diag_store_copy());                     

    GPtrArray *run = _runs();
    g_ptr_array_add(run, _run_sample("alpha", TRUE, 10));
    g_ptr_array_add(run, _run_sample("beta", TRUE, 20));
    GPtrArray *cfgs = _cfgs();
    g_ptr_array_add(cfgs, _stopped_cfg("gamma", "owner-a"));
    PcvVmDiagSnapshot *a = _snap(run, cfgs);
    GPtrArray *a_records = a->records;
    pcv_vm_diag_store_publish(a);                          

    PcvVmDiagSnapshot *c1 = pcv_vm_diag_store_copy();
    g_assert_nonnull(c1);
    g_assert_true(c1->records != a_records);              
    g_assert_cmpuint(c1->records->len, ==, 3);
    g_assert_nonnull(c1->configs);
    g_assert_cmpuint(c1->configs->len, ==, 1);
    g_assert_cmpstr(((PcvVmDiagConfig *)g_ptr_array_index(c1->configs, 0))->owner, ==, "owner-a");

                                      
    run = _runs();
    g_ptr_array_add(run, _run_sample("delta", FALSE, 0));
    pcv_vm_diag_store_publish(_snap(run, NULL));

                               
    g_assert_cmpstr(((PcvVmDiagRecord *)g_ptr_array_index(c1->records, 0))->name, ==, "alpha");
    g_assert_cmpstr(((PcvVmDiagRecord *)g_ptr_array_index(c1->records, 2))->name, ==, "gamma");
    g_assert_cmpstr(((PcvVmDiagRecord *)g_ptr_array_index(c1->records, 1))->uuid, ==, "u-beta");

    PcvVmDiagSnapshot *c2 = pcv_vm_diag_store_copy();
    g_assert_cmpuint(c2->records->len, ==, 1);
    g_assert_cmpstr(((PcvVmDiagRecord *)g_ptr_array_index(c2->records, 0))->name, ==, "delta");
    g_assert_null(c2->configs);

                           
    g_ptr_array_set_size(c2->records, 0);
    PcvVmDiagSnapshot *c3 = pcv_vm_diag_store_copy();
    g_assert_cmpuint(c3->records->len, ==, 1);

    pcv_vm_diag_snapshot_free(c1);
    pcv_vm_diag_snapshot_free(c2);
    pcv_vm_diag_snapshot_free(c3);
    pcv_vm_diag_store_publish(NULL);
    g_assert_null(pcv_vm_diag_store_copy());
}

                                   
static void
_collect(const gchar *name, const gchar *labels, gdouble value, gint kind, gpointer ud)
{
    g_ptr_array_add((GPtrArray *)ud, g_strdup_printf("%s|%s|%.0f|%d", name, labels, value, kind));
}

static gboolean
_has(GPtrArray *a, const gchar *s)
{
    for (guint i = 0; i < a->len; i++)
        if (strcmp(g_ptr_array_index(a, i), s) == 0) return TRUE;
    return FALSE;
}

static guint
_count_prefix_label(GPtrArray *a, const gchar *label)
{
    guint n = 0;
    for (guint i = 0; i < a->len; i++)
        if (strstr(g_ptr_array_index(a, i), label)) n++;
    return n;
}

                                                                 
                                             
static void
test_prom_series_existing_net(void)
{
    GPtrArray *run = _runs();
    g_ptr_array_add(run, _run_sample("a-net", TRUE, 100));
    g_ptr_array_add(run, _run_sample("b-nonet", FALSE, 0));
    for (gint i = 0; i < 70; i++) {
        gchar n[16];
        g_snprintf(n, sizeof(n), "vm%03d", (i * 11) % 70);              
        g_ptr_array_add(run, _run_sample(n, TRUE, 1000 + i));
    }
    GPtrArray *cfgs = _cfgs();
    g_ptr_array_add(cfgs, _stopped_cfg("c-off", NULL));
    PcvVmDiagSnapshot *s = _snap(run, cfgs);

    GPtrArray *got = g_ptr_array_new_with_free_func(g_free);
    guint trunc = 999;
    pcv_vm_diag_prom_series(s, PCV_PROM_VM_SERIES_MAX, _collect, got, &trunc);

                                                                       
    g_assert_cmpuint(trunc, ==, 8);
                                                                                
    g_assert_cmpuint(_count_prefix_label(got, "purecvisor_vm_net_"), ==, 63 * 4);
                                                        
    g_assert_true(_has(got, "purecvisor_vm_net_rx_bytes_total|vm_name=\"a-net\"|100|2"));
    g_assert_true(_has(got, "purecvisor_vm_net_tx_bytes_total|vm_name=\"a-net\"|200|2"));
    g_assert_true(_has(got, "purecvisor_vm_net_rx_packets_total|vm_name=\"a-net\"|3|2"));
    g_assert_true(_has(got, "purecvisor_vm_net_tx_packets_total|vm_name=\"a-net\"|4|2"));
    g_assert_cmpuint(_count_prefix_label(got, "vm_name=\"b-nonet\""), ==, 0);               
    g_assert_cmpuint(_count_prefix_label(got, "vm_name=\"c-off\""), ==, 0);                        
    g_assert_cmpuint(_count_prefix_label(got, "vm_name=\"vm061\""), ==, 4);
    g_assert_cmpuint(_count_prefix_label(got, "vm_name=\"vm062\""), ==, 0);               
    g_assert_cmpuint(_count_prefix_label(got, "vm_name=\"vm069\""), ==, 0);

                     
    g_ptr_array_set_size(got, 0);
    pcv_vm_diag_prom_series(s, 1000, _collect, got, &trunc);
    g_assert_cmpuint(trunc, ==, 0);
    g_assert_cmpuint(_count_prefix_label(got, "purecvisor_vm_net_"), ==, 71 * 4);

                              
    g_ptr_array_set_size(got, 0);
    trunc = 7;
    pcv_vm_diag_prom_series(NULL, 64, _collect, got, &trunc);
    g_assert_cmpuint(got->len, ==, 0);
    g_assert_cmpuint(trunc, ==, 0);

    g_ptr_array_unref(got);
    pcv_vm_diag_snapshot_free(s);
}

                                                                                 
static PcvVmDiagSample *
_rich_sample(const gchar *name, gint domid)
{
    PcvVmDiagSample *s = _run_sample(name, TRUE, 100);
    s->domain_id = domid;
    s->present |= PCV_VM_DIAG_B_VCPU_DELAY | PCV_VM_DIAG_B_BALLOON | PCV_VM_DIAG_B_GUEST |
                  PCV_VM_DIAG_B_RSS | PCV_VM_DIAG_B_DISK;
    s->vcpus = 2;
    s->cpu_time_ns = 3000000000ULL;
    PcvVmDiagVcpuDelay d0 = { 0, 500000000ULL }, d1 = { 1, 250000000ULL };
    g_array_append_val(s->vcpu_delay, d0);
    g_array_append_val(s->vcpu_delay, d1);
    s->current_kb = 2097152;
    s->maximum_kb = 4194304;
    s->rss_kb = 1048576;
    s->last_update_s = 1794757390;
    s->have_usable = TRUE;
    s->usable_kb = 1048576;                                                  
    s->rd_bytes = 4096;
    s->wr_bytes = 8192;
    s->rd_reqs = 10;
    s->wr_reqs = 20;
    s->rd_times_ns = 2000000000ULL;
    s->wr_times_ns = 1000000000ULL;
    s->disk_count = 1;
    return s;
}

                                                  
static PcvCgroupVmStat *
_cg_throttle(gint domid, const gchar *name)
{
    PcvCgroupVmStat *st = g_new0(PcvCgroupVmStat, 1);
    st->ref.kind = PCV_CGROUP_PSI_KIND_VM;
    st->ref.domain_id = domid;
    g_strlcpy(st->ref.name, name, sizeof(st->ref.name));
    g_strlcpy(st->ref.norm_name, name, sizeof(st->ref.norm_name));
    st->children = g_array_new(FALSE, TRUE, sizeof(PcvCgroupCpuChild));
    st->sum.have = TRUE;
    st->sum.throttled_usec = 1500000;              
    st->sum.nr_throttled = 7;
    st->sum.nr_periods = 300;
    return st;
}

static PcvVmDiagSnapshot *
_snap_cg(GPtrArray *run, GPtrArray *cfgs, GPtrArray *cg)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    PcvVmDiagSnapshot *s = g_new0(PcvVmDiagSnapshot, 1);
    PcvVmDiagHostIn h = { .observed_mono_us = 1000000, .observed_wall_ms = 1794757394000,
                          .logical_cpus = 8, .mem_total_kb = 16777216 };
    pcv_vm_diag_table_step(t, run, cfgs, cg, &h, s);
    pcv_vm_diag_table_free(t);
    if (run) g_ptr_array_unref(run);
    if (cfgs) g_ptr_array_unref(cfgs);
    if (cg) g_ptr_array_unref(cg);
    return s;
}

                        
static void
_collect_g(const gchar *name, const gchar *labels, gdouble value, gint kind, gpointer ud)
{
    g_ptr_array_add((GPtrArray *)ud, g_strdup_printf("%s|%s|%.6g|%d", name, labels, value, kind));
}

static guint
_count_name(GPtrArray *a, const gchar *name)
{
    gchar *pre = g_strdup_printf("%s|", name);
    guint n = 0;
    for (guint i = 0; i < a->len; i++)
        if (g_str_has_prefix(g_ptr_array_index(a, i), pre)) n++;
    g_free(pre);
    return n;
}

static const gchar *const VM_COUNTER_NAMES[] = {
    "purecvisor_vm_cpu_seconds_total", "purecvisor_vm_vcpu_delay_seconds_total",
    "purecvisor_vm_disk_read_bytes_total", "purecvisor_vm_disk_write_bytes_total",
    "purecvisor_vm_disk_read_requests_total", "purecvisor_vm_disk_write_requests_total",
    "purecvisor_vm_disk_read_time_seconds_total", "purecvisor_vm_disk_write_time_seconds_total",
    "purecvisor_vm_cpu_throttled_seconds_total", "purecvisor_vm_cpu_throttled_periods_total",
    "purecvisor_vm_cpu_periods_total",
};

                                                                                                 
                                                                                                      
                                     
static void
test_prom_series_rules(void)
{
    GPtrArray *run = _runs();
    g_ptr_array_add(run, _rich_sample("a-full", 900));
    g_ptr_array_add(run, _run_sample("b-cpu", FALSE, 0));                        
    for (gint i = 0; i < 68; i++) {
        gchar n[16];
        g_snprintf(n, sizeof(n), "vm%03d", (i * 7) % 68);                      
        g_ptr_array_add(run, _run_sample(n, TRUE, 1000 + i));
    }
    GPtrArray *cfgs = _cfgs();
    g_ptr_array_add(cfgs, _stopped_cfg("c-off", NULL));
    GPtrArray *cg = g_ptr_array_new_with_free_func(pcv_cgroup_vm_stat_free);
    g_ptr_array_add(cg, _cg_throttle(900, "a-full"));
    PcvVmDiagSnapshot *s = _snap_cg(run, cfgs, cg);

    GPtrArray *got = g_ptr_array_new_with_free_func(g_free);
    guint trunc = 999;
    pcv_vm_diag_prom_series(s, PCV_PROM_VM_SERIES_MAX, _collect_g, got, &trunc);
    g_assert_cmpuint(trunc, ==, 6);                       

                                                    
    g_assert_true(_has(got, "purecvisor_vm_cpu_seconds_total|vm=\"a-full\"|3|2"));
    g_assert_true(_has(got, "purecvisor_vm_vcpu_delay_seconds_total|vm=\"a-full\"|0.75|2"));
    g_assert_true(_has(got, "purecvisor_vm_disk_read_bytes_total|vm=\"a-full\"|4096|2"));
    g_assert_true(_has(got, "purecvisor_vm_disk_write_bytes_total|vm=\"a-full\"|8192|2"));
    g_assert_true(_has(got, "purecvisor_vm_disk_read_requests_total|vm=\"a-full\"|10|2"));
    g_assert_true(_has(got, "purecvisor_vm_disk_write_requests_total|vm=\"a-full\"|20|2"));
    g_assert_true(_has(got, "purecvisor_vm_disk_read_time_seconds_total|vm=\"a-full\"|2|2"));
    g_assert_true(_has(got, "purecvisor_vm_disk_write_time_seconds_total|vm=\"a-full\"|1|2"));
    g_assert_true(_has(got, "purecvisor_vm_cpu_throttled_seconds_total|vm=\"a-full\"|1.5|2"));
    g_assert_true(_has(got, "purecvisor_vm_cpu_throttled_periods_total|vm=\"a-full\"|7|2"));
    g_assert_true(_has(got, "purecvisor_vm_cpu_periods_total|vm=\"a-full\"|300|2"));
                                        
    g_assert_true(_has(got, "purecvisor_vm_memory_alloc_bytes|vm=\"a-full\"|2.14748e+09|0"));
    g_assert_true(_has(got, "purecvisor_vm_memory_guest_used_bytes|vm=\"a-full\"|1.07374e+09|0"));
    g_assert_true(_has(got, "purecvisor_vm_memory_rss_bytes|vm=\"a-full\"|1.07374e+09|0"));
    g_assert_cmpuint(_count_prefix_label(got, "vm=\"a-full\""), ==, 14);
    g_assert_cmpuint(_count_prefix_label(got, "vm_name=\"a-full\""), ==, 4);                      

                                                                              
    g_assert_cmpuint(_count_prefix_label(got, "vm=\"b-cpu\""), ==, 1);
    g_assert_true(_has(got, "purecvisor_vm_cpu_seconds_total|vm=\"b-cpu\"|1e-06|2"));
                                        
    g_assert_cmpuint(_count_prefix_label(got, "c-off"), ==, 0);
    g_assert_cmpuint(_count_prefix_label(got, "\"vm061\""), ==, 5);                      
    g_assert_cmpuint(_count_prefix_label(got, "\"vm062\""), ==, 0);
    g_assert_cmpuint(_count_prefix_label(got, "\"vm067\""), ==, 0);
    for (gsize k = 0; k < G_N_ELEMENTS(VM_COUNTER_NAMES); k++) {
        for (guint i = 0; i < got->len; i++) {
            const gchar *e = g_ptr_array_index(got, i);
            if (g_str_has_prefix(e, VM_COUNTER_NAMES[k]) && e[strlen(VM_COUNTER_NAMES[k])] == '|')
                g_assert_true(g_str_has_suffix(e, "|2"));
        }
    }
    g_assert_cmpuint(_count_name(got, "purecvisor_vm_cpu_seconds_total"), ==, 64);

                                                                           
    g_assert_true(_has(got, "purecvisor_host_vcpu_allocated|scope=\"running\"|0|0"));
    g_assert_true(_has(got, "purecvisor_host_vcpu_allocated|scope=\"defined\"|2|0"));
    g_assert_true(_has(got, "purecvisor_host_memory_allocated_bytes|scope=\"running_max\"|0|0"));
    g_assert_true(_has(got, "purecvisor_host_memory_allocated_bytes|scope=\"defined_max\"|1.07374e+09|0"));
    g_assert_true(_has(got, "purecvisor_host_memory_allocated_bytes|scope=\"running_alloc\"|0|0"));
    g_assert_true(_has(got, "purecvisor_host_logical_cpus||8|0"));
    g_assert_cmpuint(_count_name(got, "purecvisor_host_vcpu_allocated") +
                     _count_name(got, "purecvisor_host_memory_allocated_bytes") +
                     _count_name(got, "purecvisor_host_logical_cpus"), ==, 6);
                                                                               
    g_assert_cmpuint(_count_name(got, "purecvisor_vm_diag_vms"), ==, 5);
    g_assert_true(_has(got, "purecvisor_vm_diag_vms|quality=\"ok\"|0|0"));
    g_assert_true(_has(got, "purecvisor_vm_diag_vms|quality=\"warming_up\"|70|0"));
    g_assert_true(_has(got, "purecvisor_vm_diag_vms|quality=\"partial\"|0|0"));
    g_assert_true(_has(got, "purecvisor_vm_diag_vms|quality=\"busy\"|0|0"));
    g_assert_true(_has(got, "purecvisor_vm_diag_vms|quality=\"stopped\"|1|0"));
    g_assert_true(_has(got, "purecvisor_vm_diag_truncated||0|0"));
    g_assert_true(_has(got, "purecvisor_vm_diag_prom_truncated||6|0"));
                                                                    
    g_assert_cmpuint(got->len, ==, 18 + 1 + 62 * 5 + 6 + 7);

    g_ptr_array_unref(got);
    pcv_vm_diag_snapshot_free(s);
}

                                                                                                       
                                                                                                   
static void
test_prom_budget(void)
{
    const guint L = 256, base_non_cpu = 585, rpc_worst = 990, vm_per = 30, psi_per = 12, host_sampler = 20;
    g_assert_cmpuint(8 * L + base_non_cpu + rpc_worst + PCV_PROM_VM_SERIES_MAX * vm_per +
                     PCV_CGROUP_PSI_PROM_CONTAINER_MAX * psi_per + host_sampler, <=, PCV_PROM_MAX_METRICS);

    GPtrArray *run = _runs();
    g_ptr_array_add(run, _rich_sample("a-full", 900));
    GPtrArray *cg = g_ptr_array_new_with_free_func(pcv_cgroup_vm_stat_free);
    g_ptr_array_add(cg, _cg_throttle(900, "a-full"));
    PcvVmDiagSnapshot *s = _snap_cg(run, NULL, cg);
    GPtrArray *got = g_ptr_array_new_with_free_func(g_free);
    pcv_vm_diag_prom_series(s, PCV_PROM_VM_SERIES_MAX, _collect_g, got, NULL);
    guint per_vm = _count_prefix_label(got, "\"a-full\"");
    g_assert_cmpuint(per_vm, ==, 18);                                                            
    g_assert_cmpuint(per_vm + psi_per, <=, vm_per);                                   
    g_assert_cmpuint(got->len - per_vm, <=, host_sampler);                       
    g_ptr_array_unref(got);
    pcv_vm_diag_snapshot_free(s);
}

                                                                        
static void
test_prom_label_escape(void)
{
    GPtrArray *run = _runs();
    g_ptr_array_add(run, _run_sample("a\"b\\c", TRUE, 5));
    g_ptr_array_add(run, _run_sample("n\nl", FALSE, 0));
    PcvVmDiagSnapshot *s = _snap(run, NULL);
    GPtrArray *got = g_ptr_array_new_with_free_func(g_free);
    pcv_vm_diag_prom_series(s, PCV_PROM_VM_SERIES_MAX, _collect_g, got, NULL);
    g_assert_true(_has(got, "purecvisor_vm_cpu_seconds_total|vm=\"a\\\"b\\\\c\"|1e-06|2"));
    g_assert_true(_has(got, "purecvisor_vm_net_rx_bytes_total|vm_name=\"a\\\"b\\\\c\"|5|2"));
    g_assert_true(_has(got, "purecvisor_vm_cpu_seconds_total|vm=\"n\\nl\"|1e-06|2"));
    for (guint i = 0; i < got->len; i++)
        g_assert_null(strchr(g_ptr_array_index(got, i), '\n'));                 
    g_ptr_array_unref(got);
    pcv_vm_diag_snapshot_free(s);
}

                                                                            

                                      
static PcvVmDiagSample *
_full_sample(const gchar *name, gint64 t_ms, guint64 base)
{
    PcvVmDiagSample *s = _run_sample(name, TRUE, 1000 * base);
    s->present |= PCV_VM_DIAG_B_VCPU_DELAY | PCV_VM_DIAG_B_BALLOON | PCV_VM_DIAG_B_RSS |
                  PCV_VM_DIAG_B_DISK;
    s->vcpus = 2;
    s->cpu_time_ns = 1000000000ull * base;
    PcvVmDiagVcpuDelay d0 = { 0, 0 }, d1 = { 1, 0 };                       
    g_array_append_val(s->vcpu_delay, d0);
    g_array_append_val(s->vcpu_delay, d1);
    s->current_kb = 2097152;
    s->maximum_kb = 4194304;
    s->rss_kb = 524288;
    s->disk_count = 1;
    s->rd_bytes = 0;                                  
    s->mono_us = t_ms * 1000;
    s->wall_ms = 1794757394000 + t_ms;
    return s;
}

static PcvVmDiagConfig *
_cfg_owned(const gchar *name, gint state, const gchar *owner, guint vcpus, guint64 max_kb)
{
    PcvVmDiagConfig *c = _stopped_cfg(name, owner);
    c->state = state;
    c->active = (state == 1 || state == 3);
    c->domain_id = c->active ? 1 : -1;
    c->vcpus = vcpus;
    c->maximum_kb = max_kb;
    c->current_kb = max_kb;
    return c;
}

static const PcvVmDiagHostIn VIEW_HOST = {
    .observed_mono_us = 7000000, .observed_wall_ms = 1794757401000,
    .logical_cpus = 8, .physical_cores = 4, .mem_total_kb = 16777216,
    .have_psi = { { TRUE, TRUE }, { TRUE, FALSE }, { TRUE, TRUE } },
    .psi = { { 1.5, 0.5 }, { 2.0, 0.0 }, { 3.0, 1.0 } },
};

                                                                           
static PcvVmDiagSnapshot *
_two_cycle_snapshot(GPtrArray *cfgs_keep)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    for (gint k = 0; k < 2; k++) {
        gint64 tms = k == 0 ? 2000 : 7000;
        GPtrArray *run = _runs();
        g_ptr_array_add(run, _full_sample("web", tms, k + 1));                               
        PcvVmDiagSample *db = _full_sample("db", tms, k + 1);
        db->domain_id = 2;                                                          
        g_ptr_array_add(run, db);
        GPtrArray *cgs = g_ptr_array_new_with_free_func(pcv_cgroup_vm_stat_free);
        PcvCgroupVmStat *cg = g_new0(PcvCgroupVmStat, 1);
        cg->ref.kind = PCV_CGROUP_PSI_KIND_VM;
        cg->ref.domain_id = 1;
        g_strlcpy(cg->ref.norm_name, "web", sizeof(cg->ref.norm_name));
        cg->have_psi[PCV_CGROUP_PSI_RES_MEMORY] = TRUE;
        cg->psi[PCV_CGROUP_PSI_RES_MEMORY].have_some = TRUE;
        cg->psi[PCV_CGROUP_PSI_RES_MEMORY].some_avg10 = 6.0;                            
        cg->children = g_array_new(FALSE, TRUE, sizeof(PcvCgroupCpuChild));
        g_ptr_array_add(cgs, cg);
        PcvVmDiagHostIn h = VIEW_HOST;
        h.observed_mono_us = tms * 1000;
        PcvVmDiagSnapshot *s = g_new0(PcvVmDiagSnapshot, 1);
        pcv_vm_diag_table_step(t, run, cfgs_keep, cgs, &h, s);
        g_ptr_array_unref(run);
        g_ptr_array_unref(cgs);
        if (k == 1) { pcv_vm_diag_table_free(t); return s; }
        pcv_vm_diag_snapshot_free(s);
    }
    g_assert_not_reached();
    return NULL;
}

static JsonObject *
_jo(JsonObject *o, const gchar *k)
{
    g_assert_true(json_object_has_member(o, k));
    JsonNode *n = json_object_get_member(o, k);
    g_assert_true(JSON_NODE_HOLDS_OBJECT(n));
    return json_node_get_object(n);
}

static gboolean
_is_null(JsonObject *o, const gchar *k)
{
    g_assert_true(json_object_has_member(o, k));
    return JSON_NODE_HOLDS_NULL(json_object_get_member(o, k));
}

                                      
static void
_keys_exact(JsonObject *o, const gchar *const *want)
{
    guint n = 0;
    for (; want[n]; n++) g_assert_true(json_object_has_member(o, want[n]) || (g_printerr("missing %s\n", want[n]), FALSE));
    g_assert_cmpuint(json_object_get_size(o), ==, n);
}

static JsonObject *
_vm_named(JsonObject *root, const gchar *name)
{
    JsonArray *a = json_object_get_array_member(root, "vms");
    for (guint i = 0; i < json_array_get_length(a); i++) {
        JsonObject *v = json_array_get_object_element(a, i);
        if (g_strcmp0(json_object_get_string_member(v, "name"), name) == 0) return v;
    }
    return NULL;
}

static const PcvVmDiagViewer ADMIN_VIEW = { .all = TRUE };

                                                                                 
static void
test_json_shape(void)
{
    GPtrArray *cfgs = _cfgs();
    g_ptr_array_add(cfgs, _cfg_owned("web", 1, "u1", 2, 4194304));
    g_ptr_array_add(cfgs, _cfg_owned("db", 1, "u1", 2, 4194304));
    g_ptr_array_add(cfgs, _cfg_owned("off", 5, "u1", 4, 1048576));
    PcvVmDiagSnapshot *s = _two_cycle_snapshot(cfgs);
    g_ptr_array_unref(cfgs);
    JsonNode *node = pcv_vm_diag_view_json(s, &ADMIN_VIEW, NULL, 7000000 + 1234000);
    JsonObject *root = json_node_get_object(node);

    static const gchar *const TOP[] = { "schema", "scope", "sampler", "host", "vms", NULL };
    static const gchar *const SAMPLER[] = { "state", "observed_at_ms", "age_ms", "interval_ms", "tracked",
                                            "truncated", "error", NULL };
    static const gchar *const HOST[] = { "logical_cpus", "physical_cores", "mem_total_mb", "allocation", "psi", NULL };
    static const gchar *const ALLOC[] = { "vms_running", "vms_defined", "vcpu_running", "vcpu_defined",
        "vcpu_running_per_logical", "vcpu_defined_per_logical", "mem_running_max_mb", "mem_defined_max_mb",
        "mem_running_alloc_mb", "mem_running_max_ratio", "mem_defined_max_ratio", NULL };
    static const gchar *const PSI[] = { "cpu", "memory", "io", NULL };
    static const gchar *const PSI1[] = { "some_avg10", "full_avg10", NULL };
    static const gchar *const VM[] = { "name", "uuid", "state", "quality", "reason", "missing", "sample_age_ms",
        "interval_ms", "last_ok_at_ms", "vcpus", "mem_max_mb", "mem_alloc_mb", "mem_guest_used_mb",
        "mem_guest_available_mb", "mem_rss_mb", "guest_stats_age_s", "cpu_pct", "cpu_host_pct", "disk", "net",
        "contention", "hints", NULL };
    static const gchar *const DISK[] = { "rd_bps", "wr_bps", "rd_iops", "wr_iops", "rd_lat_ms", "wr_lat_ms",
                                         "fl_lat_ms", NULL };
    static const gchar *const NET[] = { "rx_bps", "tx_bps", "rx_pps", "tx_pps", "drop_ps", "err_ps", NULL };
    static const gchar *const CONT[] = { "vcpu_delay_pct", "vcpu_delay_max_pct", "throttled_pct",
        "throttled_max_pct", "throttled_periods_ratio", "psi", "swap_in_kbps", "swap_out_kbps",
        "major_faults_ps", NULL };
    static const gchar *const HINT[] = { "code", "level", "value", "threshold", NULL };

    _keys_exact(root, TOP);
    g_assert_cmpint(json_object_get_int_member(root, "schema"), ==, 1);
    g_assert_cmpstr(json_object_get_string_member(root, "scope"), ==, "all");
    JsonObject *sm = _jo(root, "sampler");
    _keys_exact(sm, SAMPLER);
    g_assert_cmpstr(json_object_get_string_member(sm, "state"), ==, "ok");
    g_assert_cmpint(json_object_get_int_member(sm, "age_ms"), ==, 1234);
    g_assert_cmpint(json_object_get_int_member(sm, "interval_ms"), ==, 5000);
    g_assert_true(_is_null(sm, "error"));
    JsonObject *host = _jo(root, "host");
    _keys_exact(host, HOST);
    g_assert_cmpint(json_object_get_int_member(host, "mem_total_mb"), ==, 16384);
    JsonObject *al = _jo(host, "allocation");
    _keys_exact(al, ALLOC);
    g_assert_cmpint(json_object_get_int_member(al, "vms_running"), ==, 2);
    g_assert_cmpint(json_object_get_int_member(al, "vcpu_defined"), ==, 8);
    g_assert_cmpfloat(json_object_get_double_member(al, "vcpu_defined_per_logical"), ==, 1.0);
    JsonObject *hp = _jo(host, "psi");
    _keys_exact(hp, PSI);
    _keys_exact(_jo(hp, "cpu"), PSI1);
    g_assert_true(_is_null(_jo(hp, "memory"), "full_avg10"));                             
    g_assert_cmpfloat(json_object_get_double_member(_jo(hp, "io"), "full_avg10"), ==, 1.0);

    JsonObject *web = _vm_named(root, "web");
    g_assert_nonnull(web);
    _keys_exact(web, VM);
    _keys_exact(_jo(web, "disk"), DISK);
    _keys_exact(_jo(web, "net"), NET);
    JsonObject *c = _jo(web, "contention");
    _keys_exact(c, CONT);
    g_assert_cmpstr(json_object_get_string_member(web, "state"), ==, "running");
    g_assert_true(_is_null(web, "guest_stats_age_s"));                                     
    g_assert_cmpint(json_object_get_int_member(web, "sample_age_ms"), ==, 1234);
    g_assert_cmpfloat(json_object_get_double_member(web, "interval_ms"), ==, 5000.0);              
    g_assert_cmpfloat(json_object_get_double_member(_jo(web, "disk"), "rd_bps"), ==, 0.0);             
    g_assert_true(_is_null(_jo(web, "disk"), "rd_lat_ms"));                            
    g_assert_cmpfloat(json_object_get_double_member(c, "vcpu_delay_pct"), ==, 0.0);
    JsonObject *cpsi = _jo(c, "psi");                                                              
    _keys_exact(cpsi, PSI);
    g_assert_cmpfloat(json_object_get_double_member(_jo(cpsi, "memory"), "some_avg10"), ==, 6.0);
    JsonArray *hints = json_object_get_array_member(web, "hints");
    g_assert_cmpuint(json_array_get_length(hints), ==, 1);
    JsonObject *h = json_array_get_object_element(hints, 0);
    _keys_exact(h, HINT);
    g_assert_cmpstr(json_object_get_string_member(h, "code"), ==, "mem_pressure");
    g_assert_cmpstr(json_object_get_string_member(h, "level"), ==, "notice");
    JsonArray *miss = json_object_get_array_member(web, "missing");
    g_assert_cmpuint(json_array_get_length(miss), ==, 1);
    g_assert_cmpstr(json_array_get_string_element(miss, 0), ==, "guest_memory");

    JsonObject *db = _vm_named(root, "db");
    g_assert_true(_is_null(_jo(db, "contention"), "psi"));                                  
    g_assert_cmpstr(json_object_get_string_member(db, "quality"), ==, "partial");
    JsonObject *off = _vm_named(root, "off");
    g_assert_cmpstr(json_object_get_string_member(off, "quality"), ==, "stopped");
    g_assert_cmpstr(json_object_get_string_member(off, "state"), ==, "shutoff");
    g_assert_true(_is_null(off, "sample_age_ms"));
    g_assert_true(_is_null(off, "cpu_pct"));
    g_assert_true(_is_null(off, "reason"));
    g_assert_cmpint(json_object_get_int_member(off, "vcpus"), ==, 4);

              
    JsonArray *vms = json_object_get_array_member(root, "vms");
    g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(vms, 0), "name"), ==, "db");
    json_node_unref(node);
    pcv_vm_diag_snapshot_free(s);
}

static guint _vm_count(JsonObject *root);

                                                  
typedef struct { gint role; const gchar *subject; const gchar *tenant; GHashTable *owner_tenant; } ReadUd;

static gboolean
_read_pred(const PcvVmDiagConfig *c, gpointer ud)
{
    const ReadUd *u = ud;
                                                                                   
    const gchar *ot = c->owner ? g_hash_table_lookup(u->owner_tenant, c->owner) : NULL;
    return pcv_vm_read_decide(u->role, u->subject, u->tenant, c->owner, ot);
}

                                               
static const gchar *
_diag_owner_tenant(const gchar *owner)
{
    if (g_strcmp0(owner, "alice") == 0 || g_strcmp0(owner, "bob") == 0) return "ta";
    if (g_strcmp0(owner, "carol") == 0) return "tb";
    return NULL;
}

                                                                               
static gboolean
_scope_pred(const PcvVmDiagConfig *c, gpointer ud)
{
    return c && pcv_vm_read_visible(ud, c->owner);
}

                                                                                         
                                                                     
static void
test_read_decide_wired(void)
{
    pcv_vm_read_set_owner_tenant_fn_for_test(_diag_owner_tenant);
    GPtrArray *run = _runs();
    g_ptr_array_add(run, _run_sample("a1", TRUE, 1));
    g_ptr_array_add(run, _run_sample("a2", TRUE, 2));
    g_ptr_array_add(run, _run_sample("b1", TRUE, 3));
    g_ptr_array_add(run, _run_sample("nobody", TRUE, 4));
    GPtrArray *cfgs = _cfgs();
    g_ptr_array_add(cfgs, _cfg_owned("a1", 1, "alice", 2, 2097152));
    g_ptr_array_add(cfgs, _cfg_owned("a2", 1, "bob", 4, 4194304));
    g_ptr_array_add(cfgs, _cfg_owned("b1", 1, "carol", 8, 8388608));
    g_ptr_array_add(cfgs, _cfg_owned("nobody", 1, NULL, 16, 16777216));
    PcvVmDiagSnapshot *s = _snap(run, cfgs);

    PcvResCaller c = { .role = PCV_ROLE_OPERATOR, .subject = (gchar *)"alice", .tenant = (gchar *)"ta" };
    PcvVmReadScope *sc = pcv_vm_read_scope_new(&c);
    PcvVmDiagViewer own = { .all = pcv_vm_read_scope_all(sc), .visible = _scope_pred, .ud = sc };
    JsonNode *n = pcv_vm_diag_view_json(s, &own, NULL, 2000000);
    JsonObject *r = json_node_get_object(n);
    g_assert_cmpstr(json_object_get_string_member(r, "scope"), ==, "own");
    g_assert_cmpuint(_vm_count(r), ==, 2);
    g_assert_nonnull(_vm_named(r, "a1"));
    g_assert_nonnull(_vm_named(r, "a2"));
    json_node_unref(n);
    pcv_vm_read_scope_free(sc);

    PcvResCaller lone = { .role = PCV_ROLE_OPERATOR, .subject = (gchar *)"carol", .tenant = NULL };
    sc = pcv_vm_read_scope_new(&lone);
    PcvVmDiagViewer own2 = { .all = pcv_vm_read_scope_all(sc), .visible = _scope_pred, .ud = sc };
    n = pcv_vm_diag_view_json(s, &own2, NULL, 2000000);
    r = json_node_get_object(n);
    g_assert_cmpuint(_vm_count(r), ==, 1);
    g_assert_nonnull(_vm_named(r, "b1"));
    json_node_unref(n);
    pcv_vm_read_scope_free(sc);

    PcvResCaller adm = { .role = PCV_ROLE_ADMIN, .subject = (gchar *)"root", .tenant = NULL };
    sc = pcv_vm_read_scope_new(&adm);
    guint before = pcv_vm_read_owner_lookups_for_test();
    PcvVmDiagViewer all = { .all = pcv_vm_read_scope_all(sc), .visible = _scope_pred, .ud = sc };
    n = pcv_vm_diag_view_json(s, &all, NULL, 2000000);
    r = json_node_get_object(n);
    g_assert_cmpstr(json_object_get_string_member(r, "scope"), ==, "all");
    g_assert_cmpuint(_vm_count(r), ==, 4);
    g_assert_cmpuint(pcv_vm_read_owner_lookups_for_test() - before, ==, 0);
    json_node_unref(n);
    pcv_vm_read_scope_free(sc);
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_read_set_owner_tenant_fn_for_test(NULL);
}

static guint
_vm_count(JsonObject *root)
{
    return json_array_get_length(json_object_get_array_member(root, "vms"));
}

                                                                      
                               
static void
test_scope_own(void)
{
    GPtrArray *run = _runs();
    g_ptr_array_add(run, _run_sample("a1", TRUE, 1));
    g_ptr_array_add(run, _run_sample("a2", TRUE, 2));
    g_ptr_array_add(run, _run_sample("b1", TRUE, 3));
    g_ptr_array_add(run, _run_sample("nobody", TRUE, 4));
    GPtrArray *cfgs = _cfgs();
    g_ptr_array_add(cfgs, _cfg_owned("a1", 1, "alice", 2, 2097152));
    g_ptr_array_add(cfgs, _cfg_owned("a2", 1, "bob", 4, 4194304));
    g_ptr_array_add(cfgs, _cfg_owned("b1", 1, "carol", 8, 8388608));
    g_ptr_array_add(cfgs, _cfg_owned("nobody", 1, NULL, 16, 16777216));
    PcvVmDiagSnapshot *s = _snap(run, cfgs);

    g_autoptr(GHashTable) ot = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(ot, (gpointer)"alice", (gpointer)"ta");
    g_hash_table_insert(ot, (gpointer)"bob", (gpointer)"ta");
    g_hash_table_insert(ot, (gpointer)"carol", (gpointer)"tb");
    ReadUd tu = { PCV_ROLE_VIEWER, "zed", "ta", ot };
    PcvVmDiagViewer own = { .all = FALSE, .visible = _read_pred, .ud = &tu };

    JsonNode *n = pcv_vm_diag_view_json(s, &own, NULL, 2000000);
    JsonObject *r = json_node_get_object(n);
    g_assert_cmpstr(json_object_get_string_member(r, "scope"), ==, "own");
    g_assert_cmpuint(_vm_count(r), ==, 2);
    g_assert_nonnull(_vm_named(r, "a1"));
    g_assert_nonnull(_vm_named(r, "a2"));
    g_assert_null(_vm_named(r, "b1"));
    g_assert_null(_vm_named(r, "nobody"));
    JsonObject *al = _jo(_jo(r, "host"), "allocation");
    g_assert_cmpint(json_object_get_int_member(al, "vms_defined"), ==, 2);
    g_assert_cmpint(json_object_get_int_member(al, "vcpu_running"), ==, 6);
    g_assert_cmpint(json_object_get_int_member(al, "mem_defined_max_mb"), ==, 6144);
    g_assert_cmpint(json_object_get_int_member(_jo(r, "sampler"), "tracked"), ==, 2);
    g_assert_cmpint(json_object_get_int_member(_jo(r, "host"), "logical_cpus"), ==, 8);                  
    json_node_unref(n);

                         
    n = pcv_vm_diag_view_json(s, &ADMIN_VIEW, NULL, 2000000);
    r = json_node_get_object(n);
    g_assert_cmpstr(json_object_get_string_member(r, "scope"), ==, "all");
    g_assert_cmpuint(_vm_count(r), ==, 4);
    g_assert_cmpint(json_object_get_int_member(_jo(_jo(r, "host"), "allocation"), "vms_defined"), ==, 4);
    json_node_unref(n);

                                         
    ReadUd none = { PCV_ROLE_VIEWER, "zed", NULL, ot };
    PcvVmDiagViewer own0 = { .all = FALSE, .visible = _read_pred, .ud = &none };
    n = pcv_vm_diag_view_json(s, &own0, NULL, 2000000);
    g_assert_cmpuint(_vm_count(json_node_get_object(n)), ==, 0);
    g_assert_cmpint(json_object_get_int_member(_jo(_jo(json_node_get_object(n), "host"), "allocation"),
                                               "vms_defined"), ==, 0);
    json_node_unref(n);

                                           
    PcvVmDiagViewer nopred = { .all = FALSE };
    n = pcv_vm_diag_view_json(s, &nopred, NULL, 2000000);
    g_assert_cmpuint(_vm_count(json_node_get_object(n)), ==, 0);
    json_node_unref(n);

                                                                                 
    ReadUd lone = { PCV_ROLE_OPERATOR, "alice", NULL, ot };
    PcvVmDiagViewer own1 = { .all = FALSE, .visible = _read_pred, .ud = &lone };
    n = pcv_vm_diag_view_json(s, &own1, NULL, 2000000);
    r = json_node_get_object(n);
    g_assert_cmpuint(_vm_count(r), ==, 1);
    g_assert_nonnull(_vm_named(r, "a1"));
    g_assert_cmpint(json_object_get_int_member(_jo(_jo(r, "host"), "allocation"), "vms_defined"), ==, 1);
    g_assert_cmpint(json_object_get_int_member(_jo(r, "sampler"), "tracked"), ==, 1);
    json_node_unref(n);

                                                                         
    ReadUd other = { PCV_ROLE_VIEWER, "dave", "tb", ot };
    PcvVmDiagViewer own2 = { .all = FALSE, .visible = _read_pred, .ud = &other };
    n = pcv_vm_diag_view_json(s, &own2, NULL, 2000000);
    r = json_node_get_object(n);
    g_assert_cmpuint(_vm_count(r), ==, 1);
    g_assert_nonnull(_vm_named(r, "b1"));
    g_assert_null(_vm_named(r, "a1"));
    g_assert_null(_vm_named(r, "a2"));
    g_assert_null(_vm_named(r, "nobody"));
    json_node_unref(n);
    pcv_vm_diag_snapshot_free(s);
}

                                                               
static void
test_vm_filter(void)
{
    GPtrArray *run = _runs();
    g_ptr_array_add(run, _run_sample("mine", TRUE, 1));
    g_ptr_array_add(run, _run_sample("theirs", TRUE, 2));
    GPtrArray *cfgs = _cfgs();
    g_ptr_array_add(cfgs, _cfg_owned("mine", 1, "alice", 1, 1048576));
    g_ptr_array_add(cfgs, _cfg_owned("theirs", 1, "carol", 1, 1048576));
    PcvVmDiagSnapshot *s = _snap(run, cfgs);

    JsonNode *n = pcv_vm_diag_view_json(s, &ADMIN_VIEW, "theirs", 1000000);
    g_assert_cmpuint(_vm_count(json_node_get_object(n)), ==, 1);
    json_node_unref(n);
    n = pcv_vm_diag_view_json(s, &ADMIN_VIEW, "ghost", 1000000);
    g_assert_cmpuint(_vm_count(json_node_get_object(n)), ==, 0);
    json_node_unref(n);

    g_autoptr(GHashTable) ot = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(ot, (gpointer)"alice", (gpointer)"ta");
    g_hash_table_insert(ot, (gpointer)"carol", (gpointer)"tb");
    ReadUd tu = { PCV_ROLE_OPERATOR, "bob", "ta", ot };
    PcvVmDiagViewer own = { .all = FALSE, .visible = _read_pred, .ud = &tu };
    n = pcv_vm_diag_view_json(s, &own, "theirs", 1000000);                      
    g_assert_cmpuint(_vm_count(json_node_get_object(n)), ==, 0);
                                                                      
    g_autofree gchar *out_of_scope = json_to_string(n, FALSE);
    json_node_unref(n);
    n = pcv_vm_diag_view_json(s, &own, "ghost", 1000000);
    g_autofree gchar *absent = json_to_string(n, FALSE);
    g_assert_cmpstr(out_of_scope, ==, absent);
    json_node_unref(n);
    n = pcv_vm_diag_view_json(s, &own, "mine", 1000000);
    g_assert_cmpuint(_vm_count(json_node_get_object(n)), ==, 1);
    json_node_unref(n);
    pcv_vm_diag_snapshot_free(s);
}

                                                                                     
static void
test_sampler_states(void)
{
    JsonNode *n = pcv_vm_diag_view_json(NULL, &ADMIN_VIEW, NULL, 123);
    JsonObject *r = json_node_get_object(n);
    JsonObject *sm = _jo(r, "sampler");
    g_assert_cmpstr(json_object_get_string_member(sm, "state"), ==, "starting");
    g_assert_true(_is_null(sm, "age_ms"));
    g_assert_true(_is_null(sm, "observed_at_ms"));
    g_assert_cmpuint(_vm_count(r), ==, 0);
    JsonObject *host = _jo(r, "host");
    g_assert_true(_is_null(host, "logical_cpus"));
    g_assert_true(_is_null(host, "mem_total_mb"));
    g_assert_cmpint(json_object_get_int_member(_jo(host, "allocation"), "vms_running"), ==, 0);
    g_assert_true(_is_null(_jo(host, "allocation"), "vcpu_running_per_logical"));
    json_node_unref(n);

    GPtrArray *run = _runs();
    g_ptr_array_add(run, _run_sample("v", TRUE, 1));
    PcvVmDiagSnapshot *s = _snap(run, NULL);                                     
    s->truncated = 3;
    static const struct { gint64 now_us; const gchar *want; } AGE[] = {
        { 1000000 + 14999000, "ok" }, { 1000000 + 15000000, "ok" }, { 1000000 + 15001000, "stale" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(AGE); i++) {
        n = pcv_vm_diag_view_json(s, &ADMIN_VIEW, NULL, AGE[i].now_us);
        r = json_node_get_object(n);
        sm = _jo(r, "sampler");
        g_assert_cmpstr(json_object_get_string_member(sm, "state"), ==, AGE[i].want);
        g_assert_cmpuint(_vm_count(r), ==, 1);                                              
        g_assert_cmpint(json_object_get_int_member(sm, "truncated"), ==, 3);
        json_node_unref(n);
    }
    s->state = PCV_VM_DIAG_S_ERROR;
    s->error = PCV_VM_DIAG_E_RUNNING_STATS_FAILED;
    n = pcv_vm_diag_view_json(s, &ADMIN_VIEW, NULL, 1000000 + 60000000);
    r = json_node_get_object(n);
    sm = _jo(r, "sampler");
    g_assert_cmpstr(json_object_get_string_member(sm, "state"), ==, "error");
    g_assert_cmpstr(json_object_get_string_member(sm, "error"), ==, "running_stats_failed");
    g_assert_cmpint(json_object_get_int_member(sm, "age_ms"), ==, 60000);
    g_assert_cmpuint(_vm_count(r), ==, 1);                                                 
    json_node_unref(n);
    pcv_vm_diag_snapshot_free(s);
}

                                                                   
static void
test_lookup_guest_mem(void)
{
    pcv_vm_diag_store_publish(NULL);
    gdouble used = 0, alloc = 0;
    g_assert_false(pcv_vm_diag_store_guest_mem("u-g1", &used, &alloc));             
    g_assert_cmpfloat(used, ==, -1);
    g_assert_cmpfloat(alloc, ==, -1);

    GPtrArray *run = _runs();
    PcvVmDiagSample *g1 = _run_sample("g1", TRUE, 1);
    g1->present |= PCV_VM_DIAG_B_BALLOON | PCV_VM_DIAG_B_GUEST;
    g1->current_kb = 2097152;
    g1->maximum_kb = 2097152;
    g1->last_update_s = 1794757390;
    g1->have_usable = TRUE;
    g1->usable_kb = 1719704;
    g_ptr_array_add(run, g1);
    PcvVmDiagSample *g2 = _run_sample("g2", TRUE, 1);                            
    g2->present |= PCV_VM_DIAG_B_BALLOON;
    g2->current_kb = 1048576;
    g_ptr_array_add(run, g2);
    pcv_vm_diag_store_publish(_snap(run, NULL));

    g_assert_true(pcv_vm_diag_store_guest_mem("u-g1", &used, &alloc));
    g_assert_cmpfloat_with_epsilon(used, (2097152.0 - 1719704.0) / 1024.0, 1e-9);
    g_assert_cmpfloat(alloc, ==, 2048.0);
    g_assert_true(pcv_vm_diag_store_guest_mem("u-g2", &used, NULL));
    g_assert_cmpfloat(used, ==, -1);                                                          
    g_assert_false(pcv_vm_diag_store_guest_mem("u-nope", &used, &alloc));
    g_assert_cmpfloat(used, ==, -1);
    g_assert_false(pcv_vm_diag_store_guest_mem(NULL, &used, &alloc));
    pcv_vm_diag_store_publish(NULL);
}

void
test_vm_diag_view_register(void)
{
    g_test_add_func("/vm_diag_view/store_publish_copy", test_store_publish_copy);
    g_test_add_func("/vm_diag_view/prom_series_existing_net", test_prom_series_existing_net);
    g_test_add_func("/vm_diag_view/prom_series_rules", test_prom_series_rules);
    g_test_add_func("/vm_diag_view/prom_budget", test_prom_budget);
    g_test_add_func("/vm_diag_view/prom_label_escape", test_prom_label_escape);
    g_test_add_func("/vm_diag_view/json_shape", test_json_shape);
    g_test_add_func("/vm_diag_view/scope_own", test_scope_own);
    g_test_add_func("/vm_diag_view/read_decide_wired", test_read_decide_wired);
    g_test_add_func("/vm_diag_view/vm_filter", test_vm_filter);
    g_test_add_func("/vm_diag_view/sampler_states", test_sampler_states);
    g_test_add_func("/vm_diag_view/lookup_guest_mem", test_lookup_guest_mem);
}
