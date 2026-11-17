   
                  
                                                              
  
                           
                                                    
                                                                                   
                                                              
                                                                       
                                                                
                                               
  
                       
                                                     
                                                       
                                                   
  
                                                                
                                                      
                                                    
                                                    
                                                           
                       
   
#include "vm_diag.h"

#include <math.h>
#include <string.h>

                                                                 

   
                       
                                                
   
typedef struct {
    PcvVmDiagHintCode code;                                 
    const gchar      *name;                                  
    gdouble           notice;                                   
    gdouble           warn;                                   
    gboolean          notice_strict;                                   
    guint             need;                                  
} PcvVmDiagHintRule;

                                                                   
                                                     
                                                                                
static const PcvVmDiagHintRule PCV_VM_DIAG_HINTS[PCV_VM_DIAG_H_COUNT] = {
    { PCV_VM_DIAG_H_CPU_WAIT,          "cpu_wait",          5.0,  20.0,   FALSE, 2 },
    { PCV_VM_DIAG_H_CPU_THROTTLED,     "cpu_throttled",     1.0,  10.0,   FALSE, 2 },
    { PCV_VM_DIAG_H_MEM_SWAP,          "mem_swap",          0.0,  1024.0, TRUE,  2 },
    { PCV_VM_DIAG_H_MEM_PRESSURE,      "mem_pressure",      5.0,  5.0,    FALSE, 1 },
    { PCV_VM_DIAG_H_IO_PRESSURE,       "io_pressure",       20.0, 10.0,   FALSE, 1 },
    { PCV_VM_DIAG_H_DISK_LATENCY,      "disk_latency",      20.0, 100.0,  FALSE, 2 },
    { PCV_VM_DIAG_H_GUEST_STATS_STALE, "guest_stats_stale",
      (gdouble)PCV_VM_DIAG_GUEST_STALE_S, NAN, TRUE, 2 },
};

                                                                              
const gchar *
pcv_vm_diag_hint_code_name(PcvVmDiagHintCode code)
{
    if ((gint)code < 0 || code >= PCV_VM_DIAG_H_COUNT) return NULL;
    return PCV_VM_DIAG_HINTS[code].name;
}

                                                      
#define VM_DIAG_STREAK_CAP 1000u

                                                                              
                                                                                                
#define VM_DIAG_DEV_N 8

                                                                        

                                                        
typedef struct { gboolean have; gint64 mono_us; guint64 cpu_time_ns; } VmCpuBase;
typedef struct { gboolean have; gint64 mono_us; GArray *d; } VmDelayBase;
typedef struct {
    gboolean have; gint64 mono_us; guint64 v[VM_DIAG_DEV_N]; guint count; guint32 hash;
} VmDevBase;
typedef struct {
    gboolean have; gint64 mono_us; PcvCgroupCpuStat sum; GArray *children;
} VmCgBase;
typedef struct {
    gboolean have; guint64 last_update_s;
    gboolean have_si, have_so, have_mf;
    guint64  swap_in_kb, swap_out_kb, major_fault;
    gboolean have_rates;                                                  
    gdouble  swap_in_kbps, swap_out_kbps, major_ps;
} VmGuestBase;

   
                                
                                                          
   
typedef struct {
    gint        domain_id;
    VmCpuBase   cpu;
    VmDelayBase vd;
    VmDevBase   disk;
    VmDevBase   net;
    VmCgBase    cg;
    VmGuestBase g;
    guint       n_streak[PCV_VM_DIAG_H_COUNT];
    guint       w_streak[PCV_VM_DIAG_H_COUNT];
    gint64      last_ok_at_ms;
    guint       seen_epoch;
} VmState;

struct _PcvVmDiagTable {
    GHashTable *by_uuid;                                       
    guint       epoch;                                              
};

                                                                       

                                                  
                                                                    
static gboolean
_tp_u64(const virTypedParameter *p, guint64 *out)
{
    switch (p->type) {
    case VIR_TYPED_PARAM_INT:
        if (p->value.i < 0) return FALSE;
        *out = (guint64)p->value.i;
        return TRUE;
    case VIR_TYPED_PARAM_UINT:
        *out = p->value.ui;
        return TRUE;
    case VIR_TYPED_PARAM_LLONG:
        if (p->value.l < 0) return FALSE;
        *out = (guint64)p->value.l;
        return TRUE;
    case VIR_TYPED_PARAM_ULLONG:
        *out = p->value.ul;
        return TRUE;
    default:
        return FALSE;
    }
}

                                                                      
static gboolean
_tp_int(const virTypedParameter *p, gint *out)
{
    switch (p->type) {
    case VIR_TYPED_PARAM_INT:   *out = p->value.i; return TRUE;
    case VIR_TYPED_PARAM_UINT:  *out = (gint)MIN(p->value.ui, (guint)G_MAXINT); return TRUE;
    case VIR_TYPED_PARAM_LLONG: *out = (gint)CLAMP(p->value.l, G_MININT, G_MAXINT); return TRUE;
    case VIR_TYPED_PARAM_ULLONG:
        *out = (gint)MIN(p->value.ul, (guint64)G_MAXINT);
        return TRUE;
    default:
        return FALSE;
    }
}

   
                                         
                                                               
   
static gboolean
_split_indexed(const gchar *field, const gchar *prefix, guint *idx, const gchar **rest)
{
    gsize pl = strlen(prefix);
    if (strncmp(field, prefix, pl) != 0) return FALSE;
    const gchar *p = field + pl;
    if (!g_ascii_isdigit((guchar)*p)) return FALSE;
    guint64 v = 0;
    while (g_ascii_isdigit((guchar)*p)) {
        v = v * 10 + (guint64)(*p - '0');
        if (v > G_MAXUINT) return FALSE;
        p++;
    }
    if (*p != '.') return FALSE;
    *idx = (guint)v;
    *rest = p + 1;
    return TRUE;
}

                                                      
                                                       
                                                      
static gint
_strp_cmp(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static guint32
_names_hash(GPtrArray *names)
{
    g_ptr_array_sort(names, _strp_cmp);
    guint32 h = 2166136261u;
    for (guint i = 0; i < names->len; i++) {
        for (const guchar *c = (const guchar *)g_ptr_array_index(names, i); *c; c++) {
            h ^= *c;
            h *= 16777619u;
        }
        h ^= 0xffu;                                          
        h *= 16777619u;
    }
    return h;
}

                                                  
static gint
_delay_cmp(gconstpointer a, gconstpointer b)
{
    guint ia = ((const PcvVmDiagVcpuDelay *)a)->idx;
    guint ib = ((const PcvVmDiagVcpuDelay *)b)->idx;
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

                                             
                                                        
static gboolean
_state_is_active(gint state)
{
    return state == VIR_DOMAIN_RUNNING || state == VIR_DOMAIN_BLOCKED ||
           state == VIR_DOMAIN_PAUSED || state == VIR_DOMAIN_SHUTDOWN ||
           state == VIR_DOMAIN_PMSUSPENDED;
}

                                                         
static gboolean
_dt_ok(gint64 dt_us)
{
    return dt_us > 0 && dt_us <= (gint64)PCV_VM_DIAG_MAX_GAP_MS * 1000;
}

                                             
                                                 
const gchar *
pcv_vm_diag_error_name(PcvVmDiagError e)
{
    switch (e) {
    case PCV_VM_DIAG_E_CONNECT_FAILED:       return "connect_failed";
    case PCV_VM_DIAG_E_RUNNING_STATS_FAILED: return "running_stats_failed";
    case PCV_VM_DIAG_E_DEFINED_STATS_FAILED: return "defined_stats_failed";
    case PCV_VM_DIAG_E_NONE:
    default:                                 return NULL;
    }
}

                                                                          

                                                   
                              
void
pcv_vm_diag_sample_clear(PcvVmDiagSample *s)
{
    if (!s) return;
    g_free(s->name);
    g_free(s->uuid);
    if (s->vcpu_delay) g_array_unref(s->vcpu_delay);
    memset(s, 0, sizeof(*s));
}

                                                                  
void
pcv_vm_diag_sample_free(gpointer p)
{
    if (!p) return;
    pcv_vm_diag_sample_clear(p);
    g_free(p);
}

                                             
                                                            
gboolean
pcv_vm_diag_parse(const virTypedParameter *params, gint nparams, const gchar *name,
                  const gchar *uuid, gint domain_id, gboolean balloon_requested,
                  gboolean block_requested, PcvVmDiagSample *out)
{
    if (!out) return FALSE;
    pcv_vm_diag_sample_clear(out);
    if (!name || !uuid || !name[0] || !uuid[0] || (nparams > 0 && !params)) return FALSE;

    out->name = g_strdup(name);
    out->uuid = g_strdup(uuid);
    out->domain_id = domain_id;
    out->state = -1;
    out->vcpu_delay = g_array_new(FALSE, TRUE, sizeof(PcvVmDiagVcpuDelay));

    gboolean have_cpu = FALSE, have_vcur = FALSE, have_bcur = FALSE, have_rss = FALSE;
    gboolean have_bcount = FALSE, have_rd = FALSE, have_ncount = FALSE, have_rx = FALSE;
    g_autoptr(GPtrArray) disks = g_ptr_array_new();
    g_autoptr(GPtrArray) nics = g_ptr_array_new();

    for (gint i = 0; i < nparams; i++) {
        const virTypedParameter *p = &params[i];
        const gchar *f = p->field;
        guint64 v = 0;
        guint idx = 0;
        const gchar *rest = NULL;

        if (strcmp(f, "state.state") == 0) {
            (void)_tp_int(p, &out->state);
        } else if (strcmp(f, "cpu.time") == 0) {
            if (_tp_u64(p, &v)) { out->cpu_time_ns = v; have_cpu = TRUE; }
        } else if (strcmp(f, "vcpu.current") == 0) {
            if (_tp_u64(p, &v)) { out->vcpus = (guint)MIN(v, (guint64)G_MAXUINT); have_vcur = TRUE; }
        } else if (strcmp(f, "vcpu.maximum") == 0) {
            if (_tp_u64(p, &v)) out->vcpus_max = (guint)MIN(v, (guint64)G_MAXUINT);
        } else if (_split_indexed(f, "vcpu.", &idx, &rest)) {
                                                                                    
            if (strcmp(rest, "delay") == 0 && _tp_u64(p, &v)) {
                PcvVmDiagVcpuDelay d = { .idx = idx, .delay_ns = v };
                g_array_append_val(out->vcpu_delay, d);
            }
        } else if (strcmp(f, "balloon.current") == 0) {
            if (_tp_u64(p, &v)) { out->current_kb = v; have_bcur = TRUE; }
        } else if (strcmp(f, "balloon.maximum") == 0) {
            if (_tp_u64(p, &v)) out->maximum_kb = v;
        } else if (strcmp(f, "balloon.rss") == 0) {
            if (_tp_u64(p, &v)) { out->rss_kb = v; have_rss = TRUE; }
        } else if (strcmp(f, "balloon.last-update") == 0) {
            if (_tp_u64(p, &v)) out->last_update_s = v;
        } else if (strcmp(f, "balloon.usable") == 0) {
            out->have_usable = _tp_u64(p, &out->usable_kb);
        } else if (strcmp(f, "balloon.unused") == 0) {
            out->have_unused = _tp_u64(p, &out->unused_kb);
        } else if (strcmp(f, "balloon.available") == 0) {
            out->have_available = _tp_u64(p, &out->available_kb);
        } else if (strcmp(f, "balloon.swap_in") == 0) {
            out->have_swap_in = _tp_u64(p, &out->swap_in_kb);
        } else if (strcmp(f, "balloon.swap_out") == 0) {
            out->have_swap_out = _tp_u64(p, &out->swap_out_kb);
        } else if (strcmp(f, "balloon.major_fault") == 0) {
            out->have_major_fault = _tp_u64(p, &out->major_fault);
        } else if (strcmp(f, "block.count") == 0) {
            if (_tp_u64(p, &v)) { out->disk_count = (guint)MIN(v, (guint64)G_MAXUINT); have_bcount = TRUE; }
        } else if (_split_indexed(f, "block.", &idx, &rest)) {
            if (strcmp(rest, "name") == 0) {
                if (p->type == VIR_TYPED_PARAM_STRING && p->value.s)
                    g_ptr_array_add(disks, p->value.s);
            } else if (_tp_u64(p, &v)) {
                if      (strcmp(rest, "rd.reqs") == 0)  { out->rd_reqs += v; have_rd = TRUE; }
                else if (strcmp(rest, "rd.bytes") == 0)   out->rd_bytes += v;
                else if (strcmp(rest, "rd.times") == 0)   out->rd_times_ns += v;
                else if (strcmp(rest, "wr.reqs") == 0)    out->wr_reqs += v;
                else if (strcmp(rest, "wr.bytes") == 0)   out->wr_bytes += v;
                else if (strcmp(rest, "wr.times") == 0)   out->wr_times_ns += v;
                else if (strcmp(rest, "fl.reqs") == 0)    out->fl_reqs += v;
                else if (strcmp(rest, "fl.times") == 0)   out->fl_times_ns += v;
            }
        } else if (strcmp(f, "net.count") == 0) {
            if (_tp_u64(p, &v)) { out->net_count = (guint)MIN(v, (guint64)G_MAXUINT); have_ncount = TRUE; }
        } else if (_split_indexed(f, "net.", &idx, &rest)) {
            if (strcmp(rest, "name") == 0) {
                if (p->type == VIR_TYPED_PARAM_STRING && p->value.s)
                    g_ptr_array_add(nics, p->value.s);
            } else if (_tp_u64(p, &v)) {
                if      (strcmp(rest, "rx.bytes") == 0) { out->rx_bytes += v; have_rx = TRUE; }
                else if (strcmp(rest, "tx.bytes") == 0)   out->tx_bytes += v;
                else if (strcmp(rest, "rx.pkts") == 0)    out->rx_pkts += v;
                else if (strcmp(rest, "tx.pkts") == 0)    out->tx_pkts += v;
                else if (strcmp(rest, "rx.drop") == 0)    out->rx_drop += v;
                else if (strcmp(rest, "tx.drop") == 0)    out->tx_drop += v;
                else if (strcmp(rest, "rx.errs") == 0)    out->rx_errs += v;
                else if (strcmp(rest, "tx.errs") == 0)    out->tx_errs += v;
            }
        }
    }

    g_array_sort(out->vcpu_delay, _delay_cmp);
    out->disk_set_hash = _names_hash(disks);
    out->net_set_hash = _names_hash(nics);
    out->active = _state_is_active(out->state);

                                                                
    if (out->active && have_cpu) out->present |= PCV_VM_DIAG_B_CPU;
    if (have_vcur) out->present |= PCV_VM_DIAG_B_VCPU;
    if (out->active && out->vcpu_delay->len > 0) out->present |= PCV_VM_DIAG_B_VCPU_DELAY;
    if (have_bcur) out->present |= PCV_VM_DIAG_B_BALLOON;
    if (out->active && out->last_update_s > 0) out->present |= PCV_VM_DIAG_B_GUEST;
    if (out->active && have_rss) out->present |= PCV_VM_DIAG_B_RSS;
    if (out->active && block_requested && have_bcount && (out->disk_count == 0 || have_rd))
        out->present |= PCV_VM_DIAG_B_DISK;
    if (out->active && have_ncount && (out->net_count == 0 || have_rx))
        out->present |= PCV_VM_DIAG_B_NET;

                                                                      
                                       
    out->busy = out->active &&
        ((balloon_requested && !have_rss) ||
         (block_requested && have_bcount && out->disk_count > 0 && !have_rd));

    if (!out->active) {
                                                                 
                                                              
        out->cpu_time_ns = 0;
    }
    return TRUE;
}

                                                                          

                                                                    
void
pcv_vm_diag_config_free(gpointer p)
{
    PcvVmDiagConfig *c = p;
    if (!c) return;
    g_free(c->name);
    g_free(c->uuid);
    g_free(c->owner);
    g_free(c);
}

                                                    
                                             
gboolean
pcv_vm_diag_parse_config(const virTypedParameter *p, gint n, const gchar *name,
                         const gchar *uuid, gint domain_id, PcvVmDiagConfig *out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    out->state = -1;
    out->domain_id = domain_id;
    if (!name || !uuid || !name[0] || !uuid[0] || (n > 0 && !p)) return FALSE;
    out->name = g_strdup(name);
    out->uuid = g_strdup(uuid);

    for (gint i = 0; i < n; i++) {
        guint64 v = 0;
        if (strcmp(p[i].field, "state.state") == 0) {
            (void)_tp_int(&p[i], &out->state);
        } else if (strcmp(p[i].field, "vcpu.current") == 0) {
            if (_tp_u64(&p[i], &v)) { out->vcpus = (guint)MIN(v, (guint64)G_MAXUINT); out->have_vcpus = TRUE; }
        } else if (strcmp(p[i].field, "balloon.maximum") == 0) {
            if (_tp_u64(&p[i], &v)) { out->maximum_kb = v; out->have_balloon = TRUE; }
        } else if (strcmp(p[i].field, "balloon.current") == 0) {
            if (_tp_u64(&p[i], &v)) out->current_kb = v;
        }
    }
    out->active = _state_is_active(out->state);
    return TRUE;
}

                                                                      

                                                
                                                     
PcvVmDiagMemSource
pcv_vm_diag_mem_ladder(guint64 actual_kb, gboolean have_usable, guint64 usable_kb,
                       gboolean have_unused, guint64 unused_kb, gboolean have_rss,
                       guint64 rss_kb, guint64 *used_kb_out)
{
    guint64 used = 0;
    PcvVmDiagMemSource src = PCV_VM_DIAG_MEM_UNAVAILABLE;
    if (actual_kb > 0 && have_usable && usable_kb <= actual_kb) {
        used = actual_kb - usable_kb;
        src = PCV_VM_DIAG_MEM_GUEST_USABLE;
    } else if (actual_kb > 0 && have_unused && unused_kb <= actual_kb) {
        used = actual_kb - unused_kb;
        src = PCV_VM_DIAG_MEM_GUEST_UNUSED;
    } else if (actual_kb > 0 && have_rss) {
        used = MIN(rss_kb, actual_kb);                                             
        src = PCV_VM_DIAG_MEM_RSS;
    }
    if (used_kb_out) *used_kb_out = used;
    return src;
}

                                                  
const gchar *
pcv_vm_diag_mem_source_name(PcvVmDiagMemSource s)
{
    switch (s) {
    case PCV_VM_DIAG_MEM_GUEST_USABLE: return "guest_usable";
    case PCV_VM_DIAG_MEM_GUEST_UNUSED: return "guest_unused";
    case PCV_VM_DIAG_MEM_RSS:          return "rss";
    case PCV_VM_DIAG_MEM_UNAVAILABLE:
    default:                           return "unavailable";
    }
}

                                                                         

                                         
                                                  
void
pcv_vm_diag_allocation(const PcvVmDiagConfig *const *cfgs, guint n,
                       gboolean (*include)(const PcvVmDiagConfig *c, gpointer ud),
                       gpointer ud, PcvVmDiagAllocation *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!cfgs) return;
    for (guint i = 0; i < n; i++) {
        const PcvVmDiagConfig *c = cfgs[i];
        if (!c) continue;
        if (include && !include(c, ud)) continue;
        out->vms_defined++;
        if (c->have_vcpus) out->vcpu_defined += c->vcpus;
        if (c->have_balloon) out->mem_defined_max_kb += c->maximum_kb;
        if (c->active) {
            out->vms_running++;
            if (c->have_vcpus) out->vcpu_running += c->vcpus;
            if (c->have_balloon) {
                out->mem_running_max_kb += c->maximum_kb;
                out->mem_running_alloc_kb += c->current_kb;
            }
        }
    }
}

                                                                         

                                                       
static void
_record_free(gpointer p)
{
    PcvVmDiagRecord *r = p;
    if (!r) return;
    g_free(r->name);
    g_free(r->uuid);
    g_free(r);
}

                                                     
static PcvVmDiagRecord *
_record_new(const gchar *name, const gchar *uuid)
{
    PcvVmDiagRecord *r = g_new0(PcvVmDiagRecord, 1);
    r->name = g_strdup(name);
    r->uuid = g_strdup(uuid);
    r->domain_id = -1;
    r->state = -1;
    gdouble *nan_fields[] = {
        &r->interval_ms, &r->mem_max_mb, &r->mem_alloc_mb, &r->mem_guest_used_mb,
        &r->mem_guest_available_mb, &r->mem_rss_mb, &r->guest_stats_age_s,
        &r->cpu_pct, &r->cpu_host_pct, &r->disk_rd_bps, &r->disk_wr_bps,
        &r->disk_rd_iops, &r->disk_wr_iops, &r->disk_rd_lat_ms, &r->disk_wr_lat_ms,
        &r->disk_fl_lat_ms, &r->net_rx_bps, &r->net_tx_bps, &r->net_rx_pps, &r->net_tx_pps,
        &r->net_drop_ps, &r->net_err_ps, &r->vcpu_delay_pct, &r->vcpu_delay_max_pct,
        &r->throttled_pct, &r->throttled_max_pct, &r->throttled_periods_ratio,
        &r->swap_in_kbps, &r->swap_out_kbps, &r->major_faults_ps,
    };
    for (gsize i = 0; i < G_N_ELEMENTS(nan_fields); i++) *nan_fields[i] = NAN;
    for (gint res = 0; res < PCV_CGROUP_PSI_RES_COUNT; res++) r->psi[res][0] = r->psi[res][1] = NAN;
    return r;
}

                                                              
static gdouble
_latency_ms(guint64 times_now, guint64 times_base, guint64 reqs_now, guint64 reqs_base)
{
    guint64 dr = reqs_now - reqs_base;
    if (dr == 0) return NAN;
    return (gdouble)(times_now - times_base) / (gdouble)dr / 1e6;
}

                                                                           

                                        
static void
_state_free(gpointer p)
{
    VmState *st = p;
    if (!st) return;
    if (st->vd.d) g_array_unref(st->vd.d);
    if (st->cg.children) g_array_unref(st->cg.children);
    g_free(st);
}

                                          
                                          
static void
_state_reset_bases(VmState *st)
{
    if (st->vd.d) g_array_set_size(st->vd.d, 0);
    if (st->cg.children) g_array_set_size(st->cg.children, 0);
    GArray *vd = st->vd.d, *ch = st->cg.children;
    gint64 last_ok = st->last_ok_at_ms;
    guint epoch = st->seen_epoch;
    gint dom = st->domain_id;
    memset(st, 0, sizeof(*st));
    st->vd.d = vd;
    st->cg.children = ch;
    st->last_ok_at_ms = last_ok;
    st->seen_epoch = epoch;
    st->domain_id = dom;
}

static VmState *
_state_new(void)
{
    VmState *st = g_new0(VmState, 1);
    st->domain_id = -1;
    st->vd.d = g_array_new(FALSE, TRUE, sizeof(PcvVmDiagVcpuDelay));
    st->cg.children = g_array_new(FALSE, TRUE, sizeof(PcvCgroupCpuChild));
    return st;
}

                                                        
PcvVmDiagTable *
pcv_vm_diag_table_new(void)
{
    PcvVmDiagTable *t = g_new0(PcvVmDiagTable, 1);
    t->by_uuid = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _state_free);
    return t;
}

                                                             
void
pcv_vm_diag_table_free(PcvVmDiagTable *t)
{
    if (!t) return;
    g_hash_table_destroy(t->by_uuid);
    g_free(t);
}

                                                
static void
_disk_vec(const PcvVmDiagSample *s, guint64 v[VM_DIAG_DEV_N])
{
    v[0] = s->rd_bytes; v[1] = s->wr_bytes; v[2] = s->rd_reqs; v[3] = s->wr_reqs;
    v[4] = s->rd_times_ns; v[5] = s->wr_times_ns; v[6] = s->fl_reqs; v[7] = s->fl_times_ns;
}

static void
_net_vec(const PcvVmDiagSample *s, guint64 v[VM_DIAG_DEV_N])
{
    v[0] = s->rx_bytes; v[1] = s->tx_bytes; v[2] = s->rx_pkts; v[3] = s->tx_pkts;
    v[4] = s->rx_drop; v[5] = s->tx_drop; v[6] = s->rx_errs; v[7] = s->tx_errs;
}

               
typedef enum { VM_B_ABSENT = 0, VM_B_FIRST, VM_B_RESET, VM_B_RATE } VmBundleOutcome;

   
                                    
  
                                                      
                                                       
                           
   
static VmBundleOutcome
_dev_step(VmDevBase *b, gint64 now_us, const guint64 v[VM_DIAG_DEV_N], guint count,
          guint32 hash, guint64 dv[VM_DIAG_DEV_N], gdouble *dt_s)
{
    VmBundleOutcome o = VM_B_FIRST;
    if (b->have) {
        gint64 dt = now_us - b->mono_us;
        gboolean reset = !_dt_ok(dt) || b->count != count || b->hash != hash;
        for (gint i = 0; i < VM_DIAG_DEV_N && !reset; i++)
            if (v[i] < b->v[i]) reset = TRUE;
        if (reset) {
            o = VM_B_RESET;
        } else {
            o = VM_B_RATE;
            for (gint i = 0; i < VM_DIAG_DEV_N; i++) dv[i] = v[i] - b->v[i];
            *dt_s = (gdouble)dt / 1e6;
        }
    }
    b->have = TRUE;
    b->mono_us = now_us;
    memcpy(b->v, v, sizeof(b->v));
    b->count = count;
    b->hash = hash;
    return o;
}

                            
static void
_note(VmBundleOutcome o, gboolean *any_first, gboolean *any_reset)
{
    if (o == VM_B_FIRST) *any_first = TRUE;
    else if (o == VM_B_RESET) *any_reset = TRUE;
}

                                                              
static gboolean
_children_same(GArray *a, GArray *b)
{
    if (a->len != b->len) return FALSE;
    for (guint i = 0; i < a->len; i++) {
        if (strcmp(g_array_index(a, PcvCgroupCpuChild, i).name,
                   g_array_index(b, PcvCgroupCpuChild, i).name) != 0) return FALSE;
    }
    return TRUE;
}

                                                        
                                    
                                                        
static void
_hint_eval(VmState *st, PcvVmDiagRecord *rec, PcvVmDiagHintCode code, gboolean observe,
           gdouble nv, gdouble wv, gboolean emit)
{
    const PcvVmDiagHintRule *r = &PCV_VM_DIAG_HINTS[code];
    if (observe) {
        gboolean n_hit = !isnan(r->notice) && !isnan(nv) &&
                         (r->notice_strict ? nv > r->notice : nv >= r->notice);
        gboolean w_hit = !isnan(r->warn) && !isnan(wv) && wv >= r->warn;
        st->n_streak[code] = n_hit ? MIN(st->n_streak[code] + 1, VM_DIAG_STREAK_CAP) : 0;
        st->w_streak[code] = w_hit ? MIN(st->w_streak[code] + 1, VM_DIAG_STREAK_CAP) : 0;
    }
    if (!emit || rec->n_hints >= PCV_VM_DIAG_MAX_HINTS) return;
    PcvVmDiagHint *h = &rec->hints[rec->n_hints];
    if (st->w_streak[code] >= r->need && !isnan(wv)) {
        *h = (PcvVmDiagHint){ code, PCV_VM_DIAG_L_WARN, wv, r->warn };
        rec->n_hints++;
    } else if (st->n_streak[code] >= r->need && !isnan(nv)) {
        *h = (PcvVmDiagHint){ code, PCV_VM_DIAG_L_NOTICE, nv, r->notice };
        rec->n_hints++;
    }
}

                                               
                                                  
static gdouble
_disk_latency_signal(const PcvVmDiagRecord *rec)
{
    gdouble v = NAN;
    if (!isnan(rec->disk_rd_iops) && rec->disk_rd_iops >= PCV_VM_DIAG_DISK_LAT_MIN_IOPS &&
        !isnan(rec->disk_rd_lat_ms))
        v = rec->disk_rd_lat_ms;
    if (!isnan(rec->disk_wr_iops) && rec->disk_wr_iops >= PCV_VM_DIAG_DISK_LAT_MIN_IOPS &&
        !isnan(rec->disk_wr_lat_ms) && (isnan(v) || rec->disk_wr_lat_ms > v))
        v = rec->disk_wr_lat_ms;
    return v;
}

                                                                                                                   
                                                                                
                                                                                                             
static gdouble
_cpu_wait_signal(const PcvVmDiagRecord *rec)
{
    if (isnan(rec->vcpu_delay_pct)) return NAN;
    if (isnan(rec->throttled_pct)) return rec->vcpu_delay_pct;
    return MAX(0.0, rec->vcpu_delay_pct - rec->throttled_pct);
}

                                       
                                                
static void
_hints_all(VmState *st, PcvVmDiagRecord *rec, gboolean guest_present, gboolean guest_event)
{
    gboolean o;

    gdouble wait = _cpu_wait_signal(rec);                                    
    o = !isnan(wait);
    _hint_eval(st, rec, PCV_VM_DIAG_H_CPU_WAIT, o, wait, wait, o);

    o = !isnan(rec->throttled_pct);
    _hint_eval(st, rec, PCV_VM_DIAG_H_CPU_THROTTLED, o, rec->throttled_pct, rec->throttled_pct, o);

    gdouble swap = (!isnan(rec->swap_in_kbps) || !isnan(rec->swap_out_kbps))
        ? (isnan(rec->swap_in_kbps) ? 0.0 : rec->swap_in_kbps) +
          (isnan(rec->swap_out_kbps) ? 0.0 : rec->swap_out_kbps)
        : NAN;
                                                        
                               
    _hint_eval(st, rec, PCV_VM_DIAG_H_MEM_SWAP, guest_event && !isnan(swap), swap, swap,
               guest_present && !isnan(swap));

    gdouble ms = rec->psi[PCV_CGROUP_PSI_RES_MEMORY][0], mf = rec->psi[PCV_CGROUP_PSI_RES_MEMORY][1];
    o = !isnan(ms) || !isnan(mf);
    _hint_eval(st, rec, PCV_VM_DIAG_H_MEM_PRESSURE, o, ms, mf, o);

    gdouble is = rec->psi[PCV_CGROUP_PSI_RES_IO][0], iff = rec->psi[PCV_CGROUP_PSI_RES_IO][1];
    o = !isnan(is) || !isnan(iff);
    _hint_eval(st, rec, PCV_VM_DIAG_H_IO_PRESSURE, o, is, iff, o);

    gdouble lat = _disk_latency_signal(rec);
    o = !isnan(rec->disk_rd_iops) || !isnan(rec->disk_wr_iops);
    _hint_eval(st, rec, PCV_VM_DIAG_H_DISK_LATENCY, o, lat, lat, o && !isnan(lat));

    o = guest_present && !isnan(rec->guest_stats_age_s);
    _hint_eval(st, rec, PCV_VM_DIAG_H_GUEST_STATS_STALE, o, rec->guest_stats_age_s, NAN, o);
}

   
                                       
  
                                                     
                                           
   
static PcvVmDiagRecord *
_compute_running(PcvVmDiagTable *t, const PcvVmDiagSample *s, const PcvCgroupVmStat *cg,
                 const PcvVmDiagHostIn *h)
{
    VmState *st = g_hash_table_lookup(t->by_uuid, s->uuid);
    gboolean new_vm = FALSE;
    if (!st) {
        st = _state_new();
        g_hash_table_insert(t->by_uuid, g_strdup(s->uuid), st);
        new_vm = TRUE;
    }
    st->seen_epoch = t->epoch;

    gboolean any_first = FALSE, any_reset = FALSE;
                                                         
                                   
    if (!new_vm && st->domain_id >= 0 && s->domain_id >= 0 && st->domain_id != s->domain_id) {
        _state_reset_bases(st);
        any_reset = TRUE;
    }
    st->domain_id = s->domain_id;

    PcvVmDiagRecord *rec = _record_new(s->name, s->uuid);
    rec->domain_id = s->domain_id;
    rec->state = s->state;
    rec->observed_mono_us = s->mono_us;
    rec->observed_wall_ms = s->wall_ms;
    const gint64 now = s->mono_us;
    const guint vcpus = (s->present & PCV_VM_DIAG_B_VCPU) ? s->vcpus : 0;
    rec->vcpus = vcpus;

                                                                        
                                               
    if (s->present & PCV_VM_DIAG_B_CPU) {
        VmBundleOutcome o = VM_B_FIRST;
        if (st->cpu.have) {
            gint64 dt = now - st->cpu.mono_us;
            if (!_dt_ok(dt) || s->cpu_time_ns < st->cpu.cpu_time_ns) {
                o = VM_B_RESET;
            } else {
                o = VM_B_RATE;
                gdouble dns = (gdouble)(s->cpu_time_ns - st->cpu.cpu_time_ns);
                gdouble dt_ns = (gdouble)dt * 1000.0;
                if (vcpus > 0) rec->cpu_pct = dns / (dt_ns * vcpus) * 100.0;
                if (h->logical_cpus > 0) rec->cpu_host_pct = dns / (dt_ns * h->logical_cpus) * 100.0;
                rec->interval_ms = (gdouble)dt / 1000.0;
            }
        }
        _note(o, &any_first, &any_reset);
        st->cpu = (VmCpuBase){ TRUE, now, s->cpu_time_ns };
    }

                                                             
    if (s->present & PCV_VM_DIAG_B_VCPU_DELAY) {
        VmBundleOutcome o = VM_B_FIRST;
        GArray *cur = s->vcpu_delay;
        if (st->vd.have) {
            gint64 dt = now - st->vd.mono_us;
            gboolean reset = !_dt_ok(dt) || st->vd.d->len != cur->len;
            for (guint i = 0; i < cur->len && !reset; i++) {
                const PcvVmDiagVcpuDelay *a = &g_array_index(cur, PcvVmDiagVcpuDelay, i);
                const PcvVmDiagVcpuDelay *b = &g_array_index(st->vd.d, PcvVmDiagVcpuDelay, i);
                if (a->idx != b->idx || a->delay_ns < b->delay_ns) reset = TRUE;
            }
            if (reset) {
                o = VM_B_RESET;
            } else {
                o = VM_B_RATE;
                gdouble dt_ns = (gdouble)dt * 1000.0, sum = 0.0, mx = 0.0;
                for (guint i = 0; i < cur->len; i++) {
                    gdouble d = (gdouble)(g_array_index(cur, PcvVmDiagVcpuDelay, i).delay_ns -
                                          g_array_index(st->vd.d, PcvVmDiagVcpuDelay, i).delay_ns);
                    sum += d;
                    mx = MAX(mx, d / dt_ns * 100.0);
                }
                rec->vcpu_delay_pct = sum / (dt_ns * cur->len) * 100.0;
                rec->vcpu_delay_max_pct = mx;
            }
        }
        _note(o, &any_first, &any_reset);
        st->vd.have = TRUE;
        st->vd.mono_us = now;
        g_array_set_size(st->vd.d, 0);
        g_array_append_vals(st->vd.d, cur->data, cur->len);
    }

                                                           
    if (s->present & PCV_VM_DIAG_B_DISK) {
        guint64 v[VM_DIAG_DEV_N], dv[VM_DIAG_DEV_N];
        gdouble dt_s = 0;
        guint64 base[VM_DIAG_DEV_N];
        memcpy(base, st->disk.v, sizeof(base));
        _disk_vec(s, v);
        VmBundleOutcome o = _dev_step(&st->disk, now, v, s->disk_count, s->disk_set_hash, dv, &dt_s);
        if (o == VM_B_RATE) {
            rec->disk_rd_bps = (gdouble)dv[0] / dt_s;
            rec->disk_wr_bps = (gdouble)dv[1] / dt_s;
            rec->disk_rd_iops = (gdouble)dv[2] / dt_s;
            rec->disk_wr_iops = (gdouble)dv[3] / dt_s;
            rec->disk_rd_lat_ms = _latency_ms(v[4], base[4], v[2], base[2]);
            rec->disk_wr_lat_ms = _latency_ms(v[5], base[5], v[3], base[3]);
            rec->disk_fl_lat_ms = _latency_ms(v[7], base[7], v[6], base[6]);
        }
        _note(o, &any_first, &any_reset);
    }

                                        
    if (s->present & PCV_VM_DIAG_B_NET) {
        guint64 v[VM_DIAG_DEV_N], dv[VM_DIAG_DEV_N];
        gdouble dt_s = 0;
        _net_vec(s, v);
        VmBundleOutcome o = _dev_step(&st->net, now, v, s->net_count, s->net_set_hash, dv, &dt_s);
        if (o == VM_B_RATE) {
            rec->net_rx_bps = (gdouble)dv[0] / dt_s;
            rec->net_tx_bps = (gdouble)dv[1] / dt_s;
            rec->net_rx_pps = (gdouble)dv[2] / dt_s;
            rec->net_tx_pps = (gdouble)dv[3] / dt_s;
            rec->net_drop_ps = (gdouble)(dv[4] + dv[5]) / dt_s;
            rec->net_err_ps = (gdouble)(dv[6] + dv[7]) / dt_s;
        }
        _note(o, &any_first, &any_reset);
    }

                                                    
    gboolean cg_present = FALSE;
    if (cg) {
        for (gint r = 0; r < PCV_CGROUP_PSI_RES_COUNT; r++) {
            if (!cg->have_psi[r]) continue;
            rec->have_psi = TRUE;
            if (cg->psi[r].have_some) rec->psi[r][0] = cg->psi[r].some_avg10;
            if (cg->psi[r].have_full) rec->psi[r][1] = cg->psi[r].full_avg10;
        }
        cg_present = rec->have_psi || cg->sum.have;
    }
    if (cg && cg->sum.have) {
        VmBundleOutcome o = VM_B_FIRST;
        if (st->cg.have) {
            gint64 dt = now - st->cg.mono_us;
            gboolean reset = !_dt_ok(dt) ||
                cg->sum.nr_periods < st->cg.sum.nr_periods ||
                cg->sum.nr_throttled < st->cg.sum.nr_throttled ||
                cg->sum.throttled_usec < st->cg.sum.throttled_usec ||
                !_children_same(cg->children, st->cg.children);
            for (guint i = 0; i < cg->children->len && !reset; i++) {
                if (g_array_index(cg->children, PcvCgroupCpuChild, i).throttled_usec <
                    g_array_index(st->cg.children, PcvCgroupCpuChild, i).throttled_usec)
                    reset = TRUE;
            }
            if (reset) {
                o = VM_B_RESET;
            } else {
                o = VM_B_RATE;
                gdouble dt_us = (gdouble)dt;
                                                                        
                if (vcpus > 0)
                    rec->throttled_pct = (gdouble)(cg->sum.throttled_usec - st->cg.sum.throttled_usec) /
                                         (dt_us * vcpus) * 100.0;
                gdouble mx = NAN;
                for (guint i = 0; i < cg->children->len; i++) {
                    const PcvCgroupCpuChild *c = &g_array_index(cg->children, PcvCgroupCpuChild, i);
                    if (!g_str_has_prefix(c->name, "vcpu")) continue;                      
                    gdouble d = (gdouble)(c->throttled_usec -
                        g_array_index(st->cg.children, PcvCgroupCpuChild, i).throttled_usec) / dt_us * 100.0;
                    mx = isnan(mx) ? d : MAX(mx, d);
                }
                rec->throttled_max_pct = mx;
                guint64 dper = cg->sum.nr_periods - st->cg.sum.nr_periods;
                if (dper > 0)
                    rec->throttled_periods_ratio =
                        (gdouble)(cg->sum.nr_throttled - st->cg.sum.nr_throttled) / (gdouble)dper;
            }
        }
        _note(o, &any_first, &any_reset);
        st->cg.have = TRUE;
        st->cg.mono_us = now;
        st->cg.sum = cg->sum;
        g_array_set_size(st->cg.children, 0);
        g_array_append_vals(st->cg.children, cg->children->data, cg->children->len);
    }

                                                                
    if (s->present & PCV_VM_DIAG_B_BALLOON) {
        rec->mem_max_mb = (gdouble)s->maximum_kb / 1024.0;
        rec->mem_alloc_mb = (gdouble)s->current_kb / 1024.0;
    }
    if (s->present & PCV_VM_DIAG_B_RSS) rec->mem_rss_mb = (gdouble)s->rss_kb / 1024.0;

                                                                 
    gboolean guest_present = (s->present & PCV_VM_DIAG_B_GUEST) != 0;
    gboolean guest_event = FALSE;
    if (guest_present) {
        guint64 cur_kb = s->current_kb;
        if (s->have_usable)
            rec->mem_guest_used_mb = (gdouble)(cur_kb > s->usable_kb ? cur_kb - s->usable_kb : 0) / 1024.0;
        else if (s->have_unused)
            rec->mem_guest_used_mb = (gdouble)(cur_kb > s->unused_kb ? cur_kb - s->unused_kb : 0) / 1024.0;
        if (s->have_available) rec->mem_guest_available_mb = (gdouble)s->available_kb / 1024.0;
        gint64 wall_s = s->wall_ms / 1000;
        rec->guest_stats_age_s = wall_s > (gint64)s->last_update_s
            ? (gdouble)(wall_s - (gint64)s->last_update_s) : 0.0;

        VmGuestBase *g = &st->g;
        gboolean advance = !g->have;                        
        if (g->have) {
            gboolean dec = s->last_update_s < g->last_update_s ||
                (s->have_swap_in && g->have_si && s->swap_in_kb < g->swap_in_kb) ||
                (s->have_swap_out && g->have_so && s->swap_out_kb < g->swap_out_kb) ||
                (s->have_major_fault && g->have_mf && s->major_fault < g->major_fault);
            if (dec) {
                any_reset = TRUE;                                        
                g->have_rates = FALSE;
                advance = TRUE;
            } else if (s->last_update_s > g->last_update_s) {
                gdouble dlu = (gdouble)(s->last_update_s - g->last_update_s);
                g->swap_in_kbps = (s->have_swap_in && g->have_si)
                    ? (gdouble)(s->swap_in_kb - g->swap_in_kb) / dlu : NAN;
                g->swap_out_kbps = (s->have_swap_out && g->have_so)
                    ? (gdouble)(s->swap_out_kb - g->swap_out_kb) / dlu : NAN;
                g->major_ps = (s->have_major_fault && g->have_mf)
                    ? (gdouble)(s->major_fault - g->major_fault) / dlu : NAN;
                g->have_rates = TRUE;
                guest_event = TRUE;
                advance = TRUE;
            }
                                                                   
        }
        if (advance) {
            g->have = TRUE;
            g->last_update_s = s->last_update_s;
            g->have_si = s->have_swap_in;     g->swap_in_kb = s->swap_in_kb;
            g->have_so = s->have_swap_out;    g->swap_out_kb = s->swap_out_kb;
            g->have_mf = s->have_major_fault; g->major_fault = s->major_fault;
        }
        if (g->have_rates) {
            rec->swap_in_kbps = g->swap_in_kbps;
            rec->swap_out_kbps = g->swap_out_kbps;
            rec->major_faults_ps = g->major_ps;
        }
    }

                                    
    if (!(s->present & PCV_VM_DIAG_B_CPU))        rec->missing |= PCV_VM_DIAG_M_CPU;
    if (!(s->present & PCV_VM_DIAG_B_VCPU_DELAY)) rec->missing |= PCV_VM_DIAG_M_VCPU_DELAY;
    if (!guest_present)                           rec->missing |= PCV_VM_DIAG_M_GUEST_MEMORY;
    if (!(s->present & PCV_VM_DIAG_B_RSS))        rec->missing |= PCV_VM_DIAG_M_RSS;
    if (!(s->present & PCV_VM_DIAG_B_DISK))       rec->missing |= PCV_VM_DIAG_M_DISK;
    if (!(s->present & PCV_VM_DIAG_B_NET))        rec->missing |= PCV_VM_DIAG_M_NET;
    if (!cg_present)                              rec->missing |= PCV_VM_DIAG_M_CGROUP;

                                                       
    if (!s->active) {
        rec->quality = PCV_VM_DIAG_Q_STOPPED;
    } else if (new_vm || any_first || any_reset) {
        rec->quality = PCV_VM_DIAG_Q_WARMING_UP;
        rec->reason = any_reset ? PCV_VM_DIAG_R_RESET : PCV_VM_DIAG_R_FIRST_SAMPLE;
    } else if (s->busy) {
        rec->quality = PCV_VM_DIAG_Q_BUSY;
        rec->reason = PCV_VM_DIAG_R_BUSY;
    } else if (rec->missing) {
        rec->quality = PCV_VM_DIAG_Q_PARTIAL;
    } else {
        rec->quality = PCV_VM_DIAG_Q_OK;
    }
    if (!s->busy) st->last_ok_at_ms = s->wall_ms;
    rec->last_ok_at_ms = st->last_ok_at_ms;

    _hints_all(st, rec, guest_present, guest_event);

                                                             
    rec->tot.present = s->present;
    rec->tot.cpu_time_ns = s->cpu_time_ns;
    for (guint i = 0; i < s->vcpu_delay->len; i++)
        rec->tot.vcpu_delay_ns += g_array_index(s->vcpu_delay, PcvVmDiagVcpuDelay, i).delay_ns;
    rec->tot.disk_rd_bytes = s->rd_bytes;   rec->tot.disk_wr_bytes = s->wr_bytes;
    rec->tot.disk_rd_reqs = s->rd_reqs;     rec->tot.disk_wr_reqs = s->wr_reqs;
    rec->tot.disk_rd_times_ns = s->rd_times_ns; rec->tot.disk_wr_times_ns = s->wr_times_ns;
    rec->tot.net_rx_bytes = s->rx_bytes;    rec->tot.net_tx_bytes = s->tx_bytes;
    rec->tot.net_rx_pkts = s->rx_pkts;      rec->tot.net_tx_pkts = s->tx_pkts;
    if (cg && cg->sum.have) {
        rec->tot.have_throttle = TRUE;
        rec->tot.throttled_usec = cg->sum.throttled_usec;
        rec->tot.nr_throttled = cg->sum.nr_throttled;
        rec->tot.nr_periods = cg->sum.nr_periods;
    }
    return rec;
}

                                                      
                                       
static PcvVmDiagRecord *
_stopped_record(const PcvVmDiagConfig *c)
{
    PcvVmDiagRecord *rec = _record_new(c->name, c->uuid);
    rec->domain_id = c->domain_id;
    rec->state = c->state;
    rec->quality = PCV_VM_DIAG_Q_STOPPED;
    rec->vcpus = c->have_vcpus ? c->vcpus : 0;
    if (c->have_balloon) rec->mem_max_mb = (gdouble)c->maximum_kb / 1024.0;
    return rec;
}

                                     
typedef struct {
    const gchar            *name;
    const gchar            *uuid;
    const PcvVmDiagSample  *s;                        
    const PcvVmDiagConfig  *c;                           
} VmEntry;

                                               
static gint
_entry_cmp(gconstpointer a, gconstpointer b)
{
    const VmEntry *x = a, *y = b;
    gint c = strcmp(x->name, y->name);
    return c ? c : strcmp(x->uuid, y->uuid);
}

   
                                             
                                                      
                                                                      
                                                      
   
static const PcvCgroupVmStat *
_cgroup_join(GHashTable *by_id, GPtrArray *cgroup_stats, const PcvVmDiagSample *s)
{
    if (!cgroup_stats) return NULL;
    if (s->domain_id >= 0) {
        const PcvCgroupVmStat *hit = g_hash_table_lookup(by_id, GINT_TO_POINTER(s->domain_id));
        if (hit) return hit;
    }
    gchar norm[PCV_CGROUP_PSI_NAME_MAX];
    pcv_cgroup_psi_normalize_vm_name(s->name, norm, sizeof(norm));
    for (guint i = 0; i < cgroup_stats->len; i++) {
        const PcvCgroupVmStat *cs = g_ptr_array_index(cgroup_stats, i);
        if (!cs || cs->ref.kind != PCV_CGROUP_PSI_KIND_VM) continue;
        if (s->domain_id >= 0 && cs->ref.domain_id >= 0) continue;
        if (strcmp(cs->ref.norm_name, norm) == 0) return cs;
    }
    return NULL;
}

                                   
static gboolean
_state_unseen(gpointer key, gpointer value, gpointer ud)
{
    (void)key;
    return ((VmState *)value)->seen_epoch != GPOINTER_TO_UINT(ud);
}

                                            
                                                           
void
pcv_vm_diag_table_step(PcvVmDiagTable *t, GPtrArray *running_samples, GPtrArray *configs,
                       GPtrArray *cgroup_stats, const PcvVmDiagHostIn *host,
                       PcvVmDiagSnapshot *out)
{
    if (!t || !out) return;
    static const PcvVmDiagHostIn zero_host = { 0 };
    const PcvVmDiagHostIn *h = host ? host : &zero_host;

    out->state = PCV_VM_DIAG_S_OK;
    out->error = PCV_VM_DIAG_E_NONE;
    out->host = *h;
    out->observed_mono_us = h->observed_mono_us;
    out->observed_wall_ms = h->observed_wall_ms;

    t->epoch++;

                          
    g_autoptr(GHashTable) by_id = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (cgroup_stats) {
        for (guint i = 0; i < cgroup_stats->len; i++) {
            const PcvCgroupVmStat *cs = g_ptr_array_index(cgroup_stats, i);
            if (cs && cs->ref.kind == PCV_CGROUP_PSI_KIND_VM && cs->ref.domain_id >= 0 &&
                !g_hash_table_contains(by_id, GINT_TO_POINTER(cs->ref.domain_id)))
                g_hash_table_insert(by_id, GINT_TO_POINTER(cs->ref.domain_id), (gpointer)cs);
        }
    }

                                                         
    g_autoptr(GArray) ents = g_array_new(FALSE, TRUE, sizeof(VmEntry));
    g_autoptr(GHashTable) running_uuid = g_hash_table_new(g_str_hash, g_str_equal);
    if (running_samples) {
        for (guint i = 0; i < running_samples->len; i++) {
            const PcvVmDiagSample *s = g_ptr_array_index(running_samples, i);
            if (!s || !s->uuid || !s->name) continue;
            if (!g_hash_table_add(running_uuid, s->uuid)) continue;
            VmEntry e = { s->name, s->uuid, s, NULL };
            g_array_append_val(ents, e);
        }
    }
    if (configs) {
        for (guint i = 0; i < configs->len; i++) {
            const PcvVmDiagConfig *c = g_ptr_array_index(configs, i);
            if (!c || !c->uuid || !c->name || g_hash_table_contains(running_uuid, c->uuid)) continue;
            VmEntry e = { c->name, c->uuid, NULL, c };
            g_array_append_val(ents, e);
        }
    }
    g_array_sort(ents, _entry_cmp);

    guint n = MIN(ents->len, (guint)PCV_VM_DIAG_MAX_VMS);
    out->truncated = ents->len - n;
    out->tracked = n;
    out->records = g_ptr_array_new_full(n, _record_free);
    for (guint i = 0; i < n; i++) {
        const VmEntry *e = &g_array_index(ents, VmEntry, i);
        PcvVmDiagRecord *rec = e->s
            ? _compute_running(t, e->s, _cgroup_join(by_id, cgroup_stats, e->s), h)
            : _stopped_record(e->c);
        g_ptr_array_add(out->records, rec);
    }

                                                             
    g_hash_table_foreach_remove(t->by_uuid, _state_unseen, GUINT_TO_POINTER(t->epoch));

    out->configs = configs ? g_ptr_array_ref(configs) : NULL;
    pcv_vm_diag_allocation(configs ? (const PcvVmDiagConfig *const *)configs->pdata : NULL,
                           configs ? configs->len : 0, NULL, NULL, &out->alloc_all);
}

                                                                         

                                                                   
void
pcv_vm_diag_snapshot_free(PcvVmDiagSnapshot *s)
{
    if (!s) return;
    if (s->records) g_ptr_array_unref(s->records);
    if (s->configs) g_ptr_array_unref(s->configs);
    g_free(s);
}

                                            
                                                
PcvVmDiagSnapshot *
pcv_vm_diag_snapshot_dup(const PcvVmDiagSnapshot *s)
{
    if (!s) return NULL;
    PcvVmDiagSnapshot *d = g_new0(PcvVmDiagSnapshot, 1);
    *d = *s;
    d->records = NULL;
    d->configs = NULL;
    if (s->records) {
        d->records = g_ptr_array_new_full(s->records->len, _record_free);
        for (guint i = 0; i < s->records->len; i++) {
            const PcvVmDiagRecord *r = g_ptr_array_index(s->records, i);
            PcvVmDiagRecord *c = g_new(PcvVmDiagRecord, 1);
            *c = *r;
            c->name = g_strdup(r->name);
            c->uuid = g_strdup(r->uuid);
            g_ptr_array_add(d->records, c);
        }
    }
    if (s->configs) {
        d->configs = g_ptr_array_new_full(s->configs->len, pcv_vm_diag_config_free);
        for (guint i = 0; i < s->configs->len; i++) {
            const PcvVmDiagConfig *c = g_ptr_array_index(s->configs, i);
            PcvVmDiagConfig *x = g_new(PcvVmDiagConfig, 1);
            *x = *c;
            x->name = g_strdup(c->name);
            x->uuid = g_strdup(c->uuid);
            x->owner = g_strdup(c->owner);
            g_ptr_array_add(d->configs, x);
        }
    }
    return d;
}
