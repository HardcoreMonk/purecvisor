   
                       
                                                                 
  
                           
                                                             
                                                            
                                                           
                                                              
  
                       
                                                    
                                                
  
                                                             
                                                      
   
#include "vm_diag_view.h"

#include <math.h>
#include <string.h>


#include "prometheus_exporter.h"                                               

                                                                
static GMutex             G_store_mu;
static PcvVmDiagSnapshot *G_store;

                                                 
                                                 
void
pcv_vm_diag_store_publish(PcvVmDiagSnapshot *take)
{
    g_mutex_lock(&G_store_mu);
    PcvVmDiagSnapshot *old = G_store;
    G_store = take;
    g_mutex_unlock(&G_store_mu);
    pcv_vm_diag_snapshot_free(old);
}

                                          
                              
PcvVmDiagSnapshot *
pcv_vm_diag_store_copy(void)
{
    g_mutex_lock(&G_store_mu);
    PcvVmDiagSnapshot *copy = pcv_vm_diag_snapshot_dup(G_store);
    g_mutex_unlock(&G_store_mu);
    return copy;
}

                                                   
                                                     
gint64
pcv_vm_diag_store_age_ms(gint64 now_mono_us)
{
    g_mutex_lock(&G_store_mu);
    gint64 observed = G_store ? G_store->observed_mono_us : 0;
    g_mutex_unlock(&G_store_mu);
    return observed > 0 ? MAX(0, (now_mono_us - observed) / 1000) : -1;
}

                                                           
                                                   
gboolean
pcv_vm_diag_store_guest_mem(const gchar *uuid, gdouble *guest_used_mb_out, gdouble *alloc_mb_out)
{
    gdouble used = -1, alloc = -1;
    gboolean found = FALSE;
    g_mutex_lock(&G_store_mu);
    if (uuid && G_store && G_store->records) {
        for (guint i = 0; i < G_store->records->len; i++) {
            const PcvVmDiagRecord *r = g_ptr_array_index(G_store->records, i);
            if (g_strcmp0(r->uuid, uuid) != 0) continue;
            found = TRUE;
            if (!isnan(r->mem_guest_used_mb)) used = r->mem_guest_used_mb;
            if (!isnan(r->mem_alloc_mb)) alloc = r->mem_alloc_mb;
            break;
        }
    }
    g_mutex_unlock(&G_store_mu);
    if (guest_used_mb_out) *guest_used_mb_out = used;
    if (alloc_mb_out) *alloc_mb_out = alloc;
    return found;
}

                                                      
static void
_owner_row_free(gpointer p)
{
    PcvVmDiagOwnerRow *r = p;
    if (!r) return;
    g_free(r->name);
    g_free(r->uuid);
    g_free(r->owner);
    g_free(r);
}

                                                    
                                                                       
GPtrArray *
pcv_vm_diag_store_owner_rows(gint64 now_mono_us)
{
    GPtrArray *rows = NULL;
    g_mutex_lock(&G_store_mu);
    const PcvVmDiagSnapshot *s = G_store;
    if (s && s->state == PCV_VM_DIAG_S_OK && s->observed_mono_us > 0
        && (now_mono_us - s->observed_mono_us) / 1000 <= PCV_VM_DIAG_STALE_MS) {
        const guint n = s->configs ? s->configs->len : 0;
        rows = g_ptr_array_new_full(n, _owner_row_free);
        for (guint i = 0; i < n; i++) {
            const PcvVmDiagConfig *c = g_ptr_array_index(s->configs, i);
            PcvVmDiagOwnerRow *r = g_new0(PcvVmDiagOwnerRow, 1);
            r->name = g_strdup(c->name);
            r->uuid = g_strdup(c->uuid);
            r->owner = g_strdup(c->owner);
            r->domain_id = c->domain_id;
            g_ptr_array_add(rows, r);
        }
    }
    g_mutex_unlock(&G_store_mu);
    return rows;
}

static const gchar *_quality_name(PcvVmDiagQuality q);

                                                           
static const struct {
    const gchar *name;
    gsize        off;                                         
} VM_NET_SERIES[] = {
    { "purecvisor_vm_net_rx_bytes_total",   G_STRUCT_OFFSET(PcvVmDiagTotals, net_rx_bytes) },
    { "purecvisor_vm_net_tx_bytes_total",   G_STRUCT_OFFSET(PcvVmDiagTotals, net_tx_bytes) },
    { "purecvisor_vm_net_rx_packets_total", G_STRUCT_OFFSET(PcvVmDiagTotals, net_rx_pkts) },
    { "purecvisor_vm_net_tx_packets_total", G_STRUCT_OFFSET(PcvVmDiagTotals, net_tx_pkts) },
};

                                                                                                        
static const struct {
    const gchar *name;
    guint        bundle;
    gsize        off;
    gdouble      scale;
} VM_COUNTER_SERIES[] = {
    { "purecvisor_vm_cpu_seconds_total",            PCV_VM_DIAG_B_CPU,        G_STRUCT_OFFSET(PcvVmDiagTotals, cpu_time_ns),      1e-9 },
    { "purecvisor_vm_vcpu_delay_seconds_total",     PCV_VM_DIAG_B_VCPU_DELAY, G_STRUCT_OFFSET(PcvVmDiagTotals, vcpu_delay_ns),    1e-9 },
    { "purecvisor_vm_disk_read_bytes_total",        PCV_VM_DIAG_B_DISK,       G_STRUCT_OFFSET(PcvVmDiagTotals, disk_rd_bytes),    1.0 },
    { "purecvisor_vm_disk_write_bytes_total",       PCV_VM_DIAG_B_DISK,       G_STRUCT_OFFSET(PcvVmDiagTotals, disk_wr_bytes),    1.0 },
    { "purecvisor_vm_disk_read_requests_total",     PCV_VM_DIAG_B_DISK,       G_STRUCT_OFFSET(PcvVmDiagTotals, disk_rd_reqs),     1.0 },
    { "purecvisor_vm_disk_write_requests_total",    PCV_VM_DIAG_B_DISK,       G_STRUCT_OFFSET(PcvVmDiagTotals, disk_wr_reqs),     1.0 },
    { "purecvisor_vm_disk_read_time_seconds_total", PCV_VM_DIAG_B_DISK,       G_STRUCT_OFFSET(PcvVmDiagTotals, disk_rd_times_ns), 1e-9 },
    { "purecvisor_vm_disk_write_time_seconds_total",PCV_VM_DIAG_B_DISK,       G_STRUCT_OFFSET(PcvVmDiagTotals, disk_wr_times_ns), 1e-9 },
    { "purecvisor_vm_cpu_throttled_seconds_total",  0,                        G_STRUCT_OFFSET(PcvVmDiagTotals, throttled_usec),   1e-6 },
    { "purecvisor_vm_cpu_throttled_periods_total",  0,                        G_STRUCT_OFFSET(PcvVmDiagTotals, nr_throttled),     1.0 },
    { "purecvisor_vm_cpu_periods_total",            0,                        G_STRUCT_OFFSET(PcvVmDiagTotals, nr_periods),       1.0 },
};

                                                      
                                                                          
                                                      
void
pcv_vm_diag_prom_series(const PcvVmDiagSnapshot *s, guint vm_cap, PcvVmDiagPromFn fn,
                        gpointer ud, guint *prom_truncated_out)
{
    if (prom_truncated_out) *prom_truncated_out = 0;
    if (!s || !s->records || !fn) return;

    guint taken = 0, over = 0;
    guint by_quality[PCV_VM_DIAG_Q_STOPPED + 1] = { 0 };
                                                                          
    for (guint i = 0; i < s->records->len; i++) {
        const PcvVmDiagRecord *r = g_ptr_array_index(s->records, i);
        if ((guint)r->quality < G_N_ELEMENTS(by_quality)) by_quality[r->quality]++;
        if (r->quality == PCV_VM_DIAG_Q_STOPPED) continue;                          
        if (taken >= vm_cap) { over++; continue; }
        taken++;

        gchar labels[PCV_PROM_LABELS_MAX];
        if (pcv_prom_labels_build(labels, sizeof(labels), "vm", r->name, NULL)) {                      
            for (gsize k = 0; k < G_N_ELEMENTS(VM_COUNTER_SERIES); k++) {
                gboolean have = VM_COUNTER_SERIES[k].bundle ? (r->tot.present & VM_COUNTER_SERIES[k].bundle) != 0
                                                            : r->tot.have_throttle;
                if (!have) continue;
                guint64 v = G_STRUCT_MEMBER(guint64, &r->tot, VM_COUNTER_SERIES[k].off);
                fn(VM_COUNTER_SERIES[k].name, labels, (gdouble)v * VM_COUNTER_SERIES[k].scale,
                   PCV_VM_DIAG_PROM_SOURCE_COUNTER, ud);
            }
            if (!isnan(r->mem_alloc_mb))
                fn("purecvisor_vm_memory_alloc_bytes", labels, r->mem_alloc_mb * 1048576.0, PCV_VM_DIAG_PROM_GAUGE, ud);
            if (!isnan(r->mem_guest_used_mb))
                fn("purecvisor_vm_memory_guest_used_bytes", labels, r->mem_guest_used_mb * 1048576.0,
                   PCV_VM_DIAG_PROM_GAUGE, ud);
            if (!isnan(r->mem_rss_mb))
                fn("purecvisor_vm_memory_rss_bytes", labels, r->mem_rss_mb * 1048576.0, PCV_VM_DIAG_PROM_GAUGE, ud);
        }

        if (!(r->tot.present & PCV_VM_DIAG_B_NET)) continue;                          
        if (!pcv_prom_labels_build(labels, sizeof(labels), "vm_name", r->name, NULL)) continue;                      
        for (gsize k = 0; k < G_N_ELEMENTS(VM_NET_SERIES); k++) {
            guint64 v = G_STRUCT_MEMBER(guint64, &r->tot, VM_NET_SERIES[k].off);
            fn(VM_NET_SERIES[k].name, labels, (gdouble)v, PCV_VM_DIAG_PROM_SOURCE_COUNTER, ud);                      
        }
    }

                                                       
    const PcvVmDiagAllocation *al = &s->alloc_all;
    fn("purecvisor_host_vcpu_allocated", "scope=\"running\"", (gdouble)al->vcpu_running, PCV_VM_DIAG_PROM_GAUGE, ud);
    fn("purecvisor_host_vcpu_allocated", "scope=\"defined\"", (gdouble)al->vcpu_defined, PCV_VM_DIAG_PROM_GAUGE, ud);
    fn("purecvisor_host_memory_allocated_bytes", "scope=\"running_max\"", (gdouble)al->mem_running_max_kb * 1024.0,
       PCV_VM_DIAG_PROM_GAUGE, ud);
    fn("purecvisor_host_memory_allocated_bytes", "scope=\"defined_max\"", (gdouble)al->mem_defined_max_kb * 1024.0,
       PCV_VM_DIAG_PROM_GAUGE, ud);
    fn("purecvisor_host_memory_allocated_bytes", "scope=\"running_alloc\"", (gdouble)al->mem_running_alloc_kb * 1024.0,
       PCV_VM_DIAG_PROM_GAUGE, ud);
    if (s->host.logical_cpus > 0)
        fn("purecvisor_host_logical_cpus", "", (gdouble)s->host.logical_cpus, PCV_VM_DIAG_PROM_GAUGE, ud);

                
    for (guint q = 0; q < G_N_ELEMENTS(by_quality); q++) {
        gchar ql[48];
        if (pcv_prom_labels_build(ql, sizeof(ql), "quality", _quality_name((PcvVmDiagQuality)q), NULL))
            fn("purecvisor_vm_diag_vms", ql, (gdouble)by_quality[q], PCV_VM_DIAG_PROM_GAUGE, ud);
    }
    fn("purecvisor_vm_diag_truncated", "", (gdouble)s->truncated, PCV_VM_DIAG_PROM_GAUGE, ud);
    fn("purecvisor_vm_diag_prom_truncated", "", (gdouble)over, PCV_VM_DIAG_PROM_GAUGE, ud);
    if (prom_truncated_out) *prom_truncated_out = over;
}

                                                                         

                                                               
static void
_num(JsonObject *o, const gchar *key, gdouble v)
{
    if (isnan(v)) json_object_set_null_member(o, key);
    else json_object_set_double_member(o, key, v);
}

                             
static void
_int_or_null(JsonObject *o, const gchar *key, gint64 v)
{
    if (v <= 0) json_object_set_null_member(o, key);
    else json_object_set_int_member(o, key, v);
}

                        
static void
_ratio(JsonObject *o, const gchar *key, gdouble num, gdouble den)
{
    if (den <= 0) json_object_set_null_member(o, key);
    else json_object_set_double_member(o, key, num / den);
}

                                                        
static const gchar *
_state_name(gint st)
{
    static const gchar *const NAMES[] = {
        "nostate", "running", "blocked", "paused", "shutdown", "shutoff", "crashed", "pmsuspended",
    };
    return (st >= 0 && st < (gint)G_N_ELEMENTS(NAMES)) ? NAMES[st] : "unknown";
}

static const gchar *
_quality_name(PcvVmDiagQuality q)
{
    switch (q) {
    case PCV_VM_DIAG_Q_OK:         return "ok";
    case PCV_VM_DIAG_Q_WARMING_UP: return "warming_up";
    case PCV_VM_DIAG_Q_PARTIAL:    return "partial";
    case PCV_VM_DIAG_Q_BUSY:       return "busy";
    case PCV_VM_DIAG_Q_STOPPED:    return "stopped";
    }
    return "partial";
}

static const gchar *
_reason_name(PcvVmDiagReason r)
{
    switch (r) {
    case PCV_VM_DIAG_R_FIRST_SAMPLE: return "first_sample";
    case PCV_VM_DIAG_R_RESET:        return "reset";
    case PCV_VM_DIAG_R_BUSY:         return "busy";
    case PCV_VM_DIAG_R_NONE:         return NULL;
    }
    return NULL;
}

                                                                 
static const gchar *const MISSING_NAMES[] = {
    "cpu", "vcpu_delay", "guest_memory", "rss", "disk", "net", "cgroup",
};

                                                                                        
static JsonObject *
_psi_obj(const gdouble psi[PCV_CGROUP_PSI_RES_COUNT][2],
         const gboolean (*have)[2])
{
    static const gchar *const RES[PCV_CGROUP_PSI_RES_COUNT] = { "cpu", "memory", "io" };
    JsonObject *o = json_object_new();
    for (gint r = 0; r < PCV_CGROUP_PSI_RES_COUNT; r++) {
        JsonObject *x = json_object_new();
        _num(x, "some_avg10", (have && !have[r][0]) ? NAN : psi[r][0]);
        _num(x, "full_avg10", (have && !have[r][1]) ? NAN : psi[r][1]);
        json_object_set_object_member(o, RES[r], x);
    }
    return o;
}

                                                                     
static JsonObject *
_vm_obj(const PcvVmDiagRecord *r, gint64 now_mono_us)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "name", r->name);
    json_object_set_string_member(o, "uuid", r->uuid);
    json_object_set_string_member(o, "state", _state_name(r->state));
    json_object_set_string_member(o, "quality", _quality_name(r->quality));
    const gchar *why = _reason_name(r->reason);
    if (why) json_object_set_string_member(o, "reason", why);
    else json_object_set_null_member(o, "reason");
    JsonArray *miss = json_array_new();
    for (guint b = 0; b < G_N_ELEMENTS(MISSING_NAMES); b++)
        if (r->missing & (1u << b)) json_array_add_string_element(miss, MISSING_NAMES[b]);
    json_object_set_array_member(o, "missing", miss);
    if (r->observed_mono_us > 0)
        json_object_set_int_member(o, "sample_age_ms", MAX(0, (now_mono_us - r->observed_mono_us) / 1000));
    else
        json_object_set_null_member(o, "sample_age_ms");
    _num(o, "interval_ms", r->interval_ms);
    _int_or_null(o, "last_ok_at_ms", r->last_ok_at_ms);
    _int_or_null(o, "vcpus", r->vcpus);
    _num(o, "mem_max_mb", r->mem_max_mb);
    _num(o, "mem_alloc_mb", r->mem_alloc_mb);
    _num(o, "mem_guest_used_mb", r->mem_guest_used_mb);
    _num(o, "mem_guest_available_mb", r->mem_guest_available_mb);
    _num(o, "mem_rss_mb", r->mem_rss_mb);
    _num(o, "guest_stats_age_s", r->guest_stats_age_s);
    _num(o, "cpu_pct", r->cpu_pct);
    _num(o, "cpu_host_pct", r->cpu_host_pct);

    JsonObject *d = json_object_new();
    _num(d, "rd_bps", r->disk_rd_bps);
    _num(d, "wr_bps", r->disk_wr_bps);
    _num(d, "rd_iops", r->disk_rd_iops);
    _num(d, "wr_iops", r->disk_wr_iops);
    _num(d, "rd_lat_ms", r->disk_rd_lat_ms);
    _num(d, "wr_lat_ms", r->disk_wr_lat_ms);
    _num(d, "fl_lat_ms", r->disk_fl_lat_ms);
    json_object_set_object_member(o, "disk", d);

    JsonObject *n = json_object_new();
    _num(n, "rx_bps", r->net_rx_bps);
    _num(n, "tx_bps", r->net_tx_bps);
    _num(n, "rx_pps", r->net_rx_pps);
    _num(n, "tx_pps", r->net_tx_pps);
    _num(n, "drop_ps", r->net_drop_ps);
    _num(n, "err_ps", r->net_err_ps);
    json_object_set_object_member(o, "net", n);

    JsonObject *c = json_object_new();
    _num(c, "vcpu_delay_pct", r->vcpu_delay_pct);
    _num(c, "vcpu_delay_max_pct", r->vcpu_delay_max_pct);
    _num(c, "throttled_pct", r->throttled_pct);
    _num(c, "throttled_max_pct", r->throttled_max_pct);
    _num(c, "throttled_periods_ratio", r->throttled_periods_ratio);
    if (r->have_psi) json_object_set_object_member(c, "psi", _psi_obj(r->psi, NULL));
    else json_object_set_null_member(c, "psi");                                          
    _num(c, "swap_in_kbps", r->swap_in_kbps);
    _num(c, "swap_out_kbps", r->swap_out_kbps);
    _num(c, "major_faults_ps", r->major_faults_ps);
    json_object_set_object_member(o, "contention", c);

    JsonArray *hints = json_array_new();
    for (guint i = 0; i < r->n_hints; i++) {
        JsonObject *h = json_object_new();
        json_object_set_string_member(h, "code", pcv_vm_diag_hint_code_name(r->hints[i].code));
        json_object_set_string_member(h, "level",
            r->hints[i].level == PCV_VM_DIAG_L_WARN ? "warn" : "notice");
        json_object_set_double_member(h, "value", r->hints[i].value);
        json_object_set_double_member(h, "threshold", r->hints[i].threshold);
        json_array_add_object_element(hints, h);
    }
    json_object_set_array_member(o, "hints", hints);
    return o;
}

                                                                
static GHashTable *
_config_index(const PcvVmDiagSnapshot *s)
{
    GHashTable *h = g_hash_table_new(g_str_hash, g_str_equal);
    if (s && s->configs) {
        for (guint i = 0; i < s->configs->len; i++) {
            PcvVmDiagConfig *c = g_ptr_array_index(s->configs, i);
            if (c && c->uuid) g_hash_table_insert(h, c->uuid, c);
        }
    }
    return h;
}

                                                                
static gboolean
_record_visible(const PcvVmDiagViewer *v, GHashTable *cfg_by_uuid, const PcvVmDiagRecord *r)
{
    if (v && v->all) return TRUE;
    if (!v || !v->visible) return FALSE;
    const PcvVmDiagConfig *c = g_hash_table_lookup(cfg_by_uuid, r->uuid);
    return c && v->visible(c, v->ud);
}

                                         
                                                 
JsonNode *
pcv_vm_diag_view_json(const PcvVmDiagSnapshot *s, const PcvVmDiagViewer *v,
                      const gchar *vm_filter, gint64 now_mono_us)
{
    const gboolean all = v && v->all;
    JsonObject *root = json_object_new();
    json_object_set_int_member(root, "schema", 1);
    json_object_set_string_member(root, "scope", all ? "all" : "own");

    g_autoptr(GHashTable) cfg_by_uuid = _config_index(s);
    JsonArray *vms = json_array_new();
    guint shown = 0;
    if (s && s->records) {
        for (guint i = 0; i < s->records->len; i++) {
            const PcvVmDiagRecord *r = g_ptr_array_index(s->records, i);
            if (!_record_visible(v, cfg_by_uuid, r)) continue;
            shown++;
            if (vm_filter && g_strcmp0(r->name, vm_filter) != 0) continue;
            json_array_add_object_element(vms, _vm_obj(r, now_mono_us));
        }
    }

                
    JsonObject *sm = json_object_new();
    const gchar *state = "starting";
    const gchar *err = NULL;
    if (s) {
        gint64 age_ms = s->observed_mono_us > 0 ? (now_mono_us - s->observed_mono_us) / 1000 : -1;
        if (s->state == PCV_VM_DIAG_S_ERROR) {
            state = "error";
            err = pcv_vm_diag_error_name(s->error);
        } else if (s->state == PCV_VM_DIAG_S_OK) {
            state = (age_ms > PCV_VM_DIAG_STALE_MS) ? "stale" : "ok";
        }
        _int_or_null(sm, "observed_at_ms", s->observed_wall_ms);
        if (age_ms >= 0) json_object_set_int_member(sm, "age_ms", age_ms);
        else json_object_set_null_member(sm, "age_ms");
    } else {
        json_object_set_null_member(sm, "observed_at_ms");
        json_object_set_null_member(sm, "age_ms");
    }
    json_object_set_string_member(sm, "state", state);
    json_object_set_int_member(sm, "interval_ms", PCV_VM_DIAG_INTERVAL_MS);
    guint truncated = 0;
    if (s && all) {
        truncated = s->truncated;
    } else if (s && s->configs && v && v->visible) {
                                                        
        g_autoptr(GHashTable) has_rec = g_hash_table_new(g_str_hash, g_str_equal);
        for (guint i = 0; s->records && i < s->records->len; i++)
            g_hash_table_add(has_rec, ((PcvVmDiagRecord *)g_ptr_array_index(s->records, i))->uuid);
        for (guint i = 0; i < s->configs->len; i++) {
            const PcvVmDiagConfig *c = g_ptr_array_index(s->configs, i);
            if (c && v->visible(c, v->ud) && !g_hash_table_contains(has_rec, c->uuid)) truncated++;
        }
    }
    json_object_set_int_member(sm, "tracked", all ? (s ? s->tracked : 0) : shown);
    json_object_set_int_member(sm, "truncated", truncated);
    if (err) json_object_set_string_member(sm, "error", err);
    else json_object_set_null_member(sm, "error");
    json_object_set_object_member(root, "sampler", sm);

                                            
    JsonObject *host = json_object_new();
    guint lcpu = s ? s->host.logical_cpus : 0;
    guint64 mem_total_kb = s ? s->host.mem_total_kb : 0;
    _int_or_null(host, "logical_cpus", lcpu);
    _int_or_null(host, "physical_cores", s ? s->host.physical_cores : 0);
    _int_or_null(host, "mem_total_mb", (gint64)(mem_total_kb / 1024));
    PcvVmDiagAllocation a = { 0 };
    if (s && all) {
        a = s->alloc_all;
    } else if (s && s->configs && v && v->visible) {
        pcv_vm_diag_allocation((const PcvVmDiagConfig *const *)s->configs->pdata, s->configs->len,
                               v->visible, v->ud, &a);
    }
    JsonObject *al = json_object_new();
    json_object_set_int_member(al, "vms_running", a.vms_running);
    json_object_set_int_member(al, "vms_defined", a.vms_defined);
    json_object_set_int_member(al, "vcpu_running", a.vcpu_running);
    json_object_set_int_member(al, "vcpu_defined", a.vcpu_defined);
    _ratio(al, "vcpu_running_per_logical", a.vcpu_running, lcpu);
    _ratio(al, "vcpu_defined_per_logical", a.vcpu_defined, lcpu);
    json_object_set_int_member(al, "mem_running_max_mb", (gint64)(a.mem_running_max_kb / 1024));
    json_object_set_int_member(al, "mem_defined_max_mb", (gint64)(a.mem_defined_max_kb / 1024));
    json_object_set_int_member(al, "mem_running_alloc_mb", (gint64)(a.mem_running_alloc_kb / 1024));
    _ratio(al, "mem_running_max_ratio", (gdouble)a.mem_running_max_kb, (gdouble)mem_total_kb);
    _ratio(al, "mem_defined_max_ratio", (gdouble)a.mem_defined_max_kb, (gdouble)mem_total_kb);
    json_object_set_object_member(host, "allocation", al);
    if (s) {
        json_object_set_object_member(host, "psi",
            _psi_obj(s->host.psi, (const gboolean (*)[2])s->host.have_psi));
    } else {
        static const gdouble none[PCV_CGROUP_PSI_RES_COUNT][2] = { { NAN, NAN }, { NAN, NAN }, { NAN, NAN } };
        json_object_set_object_member(host, "psi", _psi_obj(none, NULL));
    }
    json_object_set_object_member(root, "host", host);
    json_object_set_array_member(root, "vms", vms);

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, root);
    return node;
}
