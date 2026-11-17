                                                                
                                                                           
                                                              
                                
                                                        
                                                          
                                                         
#include <glib.h>
#include <math.h>
#include <string.h>

#include "modules/daemons/vm_diag.h"

                                                                        

                                             
typedef struct {
    GArray    *a;                            
    GPtrArray *strs;              
} TP;

static void
_tp_init(TP *t)
{
    t->a = g_array_new(FALSE, TRUE, sizeof(virTypedParameter));
    t->strs = g_ptr_array_new_with_free_func(g_free);
}

static void
_tp_clear(TP *t)
{
    g_array_unref(t->a);
    g_ptr_array_unref(t->strs);
}

static virTypedParameter *
_tp_push(TP *t, const gchar *field, int type)
{
    virTypedParameter p;
    memset(&p, 0, sizeof(p));
    g_strlcpy(p.field, field, sizeof(p.field));
    p.type = type;
    g_array_append_val(t->a, p);
    return &g_array_index(t->a, virTypedParameter, t->a->len - 1);
}

static void
_tp_ull(TP *t, const gchar *field, guint64 v)
{
    _tp_push(t, field, VIR_TYPED_PARAM_ULLONG)->value.ul = v;
}

static void
_tp_uint(TP *t, const gchar *field, guint v)
{
    _tp_push(t, field, VIR_TYPED_PARAM_UINT)->value.ui = v;
}

static void
_tp_int(TP *t, const gchar *field, gint v)
{
    _tp_push(t, field, VIR_TYPED_PARAM_INT)->value.i = v;
}

static void
_tp_str(TP *t, const gchar *field, const gchar *v)
{
    gchar *s = g_strdup(v);
    g_ptr_array_add(t->strs, s);
    _tp_push(t, field, VIR_TYPED_PARAM_STRING)->value.s = s;
}

static void
_tp_bool(TP *t, const gchar *field, gboolean v)
{
    _tp_push(t, field, VIR_TYPED_PARAM_BOOLEAN)->value.b = v ? 1 : 0;
}

                                                                         
typedef struct {
    gint     state;                                    
    gboolean busy;                                                                        
    gboolean kvm_noise;                                
    gboolean no_cpu;                          
    gboolean no_delay;                              
    gboolean no_net;                           
    gboolean no_rss;                                                      
    guint64  cpu_ns;
    guint    vcpus;
    guint64  delay[8];
    guint64  bal_cur, bal_max;
    guint64  lu;                                                   
    guint64  unused, available, usable, swap_in, swap_out, major;
    guint64  rss;
    guint    ndisk;
    const gchar *disk[4];
    guint64  rd_bytes, wr_bytes, rd_reqs, wr_reqs, rd_times, wr_times, fl_reqs, fl_times;
    guint    nnet;
    guint64  rx_bytes, tx_bytes, rx_pkts, tx_pkts, rx_drop, tx_drop, rx_errs, tx_errs;
} FixVm;

static FixVm
_fix_default(void)
{
    FixVm f = {
        .state = 1, .cpu_ns = 22698649000ull, .vcpus = 2,
        .delay = { 3104193ull, 1069081ull },
        .bal_cur = 2097152, .bal_max = 2097152,
        .lu = 1794757389ull, .unused = 1691352, .available = 2014860, .usable = 1719704,
        .swap_in = 0, .swap_out = 0, .major = 1072, .rss = 522732,
        .ndisk = 1, .disk = { "vda" },
        .rd_bytes = 225878528ull, .wr_bytes = 8850944ull, .rd_reqs = 7320, .wr_reqs = 318,
        .rd_times = 5414637423ull, .wr_times = 815706277ull, .fl_reqs = 56, .fl_times = 154447055ull,
        .nnet = 1, .rx_bytes = 2857, .tx_bytes = 2848, .rx_pkts = 20, .tx_pkts = 16,
    };
    return f;
}

                                                                          
static void
_fix_params(const FixVm *f, TP *t)
{
    _tp_int(t, "state.state", f->state);
    _tp_int(t, "state.reason", 1);
    if (!f->no_cpu) {
        _tp_ull(t, "cpu.time", f->cpu_ns);
        _tp_ull(t, "cpu.user", f->cpu_ns / 4 * 3);
        _tp_ull(t, "cpu.system", f->cpu_ns / 4);
    }
    _tp_uint(t, "cpu.cache.monitor.count", 0);
    _tp_ull(t, "balloon.current", f->bal_cur);
    _tp_ull(t, "balloon.maximum", f->bal_max);
    if (!f->busy && f->state == 1) {
        if (f->lu > 0) {
            _tp_ull(t, "balloon.swap_in", f->swap_in);
            _tp_ull(t, "balloon.swap_out", f->swap_out);
            _tp_ull(t, "balloon.major_fault", f->major);
            _tp_ull(t, "balloon.minor_fault", 172750);
            _tp_ull(t, "balloon.unused", f->unused);
            _tp_ull(t, "balloon.available", f->available);
            _tp_ull(t, "balloon.usable", f->usable);
        }
        _tp_ull(t, "balloon.last-update", f->lu);
        if (f->lu > 0) _tp_ull(t, "balloon.disk_caches", 162940);
        if (!f->no_rss) _tp_ull(t, "balloon.rss", f->rss);
    }
    _tp_uint(t, "vcpu.current", f->vcpus);
    _tp_uint(t, "vcpu.maximum", f->vcpus);
    if (f->state == 1) {
        for (guint i = 0; i < f->vcpus; i++) {
            gchar k[64];
            g_snprintf(k, sizeof(k), "vcpu.%u.state", i);  _tp_int(t, k, 1);
            g_snprintf(k, sizeof(k), "vcpu.%u.time", i);   _tp_ull(t, k, 7330000000ull);
            g_snprintf(k, sizeof(k), "vcpu.%u.wait", i);   _tp_ull(t, k, 0);
            if (!f->no_delay) {
                g_snprintf(k, sizeof(k), "vcpu.%u.delay", i);
                _tp_ull(t, k, f->delay[i]);
            }
            if (f->kvm_noise && !f->busy) {
                                                                
                g_snprintf(k, sizeof(k), "vcpu.%u.exits.sum", i);           _tp_ull(t, k, 473776);
                g_snprintf(k, sizeof(k), "vcpu.%u.halt_wait_ns.sum", i);    _tp_ull(t, k, 112573509785ull);
                g_snprintf(k, sizeof(k), "vcpu.%u.preemption_reported.sum", i); _tp_ull(t, k, 28);
                g_snprintf(k, sizeof(k), "vcpu.%u.guest_mode.cur", i);      _tp_bool(t, k, FALSE);
                g_snprintf(k, sizeof(k), "vcpu.%u.l1d_flush.sum", i);       _tp_ull(t, k, 339562);
            }
        }
    }
    if (f->state == 1 && !f->no_net) {
        _tp_uint(t, "net.count", f->nnet);
        for (guint i = 0; i < f->nnet; i++) {
            gchar k[64], nm[16];
            g_snprintf(nm, sizeof(nm), "vnet%u", i);
            g_snprintf(k, sizeof(k), "net.%u.name", i); _tp_str(t, k, nm);
            guint64 z = (i == 0);
            g_snprintf(k, sizeof(k), "net.%u.rx.bytes", i); _tp_ull(t, k, z * f->rx_bytes);
            g_snprintf(k, sizeof(k), "net.%u.rx.pkts", i);  _tp_ull(t, k, z * f->rx_pkts);
            g_snprintf(k, sizeof(k), "net.%u.rx.errs", i);  _tp_ull(t, k, z * f->rx_errs);
            g_snprintf(k, sizeof(k), "net.%u.rx.drop", i);  _tp_ull(t, k, z * f->rx_drop);
            g_snprintf(k, sizeof(k), "net.%u.tx.bytes", i); _tp_ull(t, k, z * f->tx_bytes);
            g_snprintf(k, sizeof(k), "net.%u.tx.pkts", i);  _tp_ull(t, k, z * f->tx_pkts);
            g_snprintf(k, sizeof(k), "net.%u.tx.errs", i);  _tp_ull(t, k, z * f->tx_errs);
            g_snprintf(k, sizeof(k), "net.%u.tx.drop", i);  _tp_ull(t, k, z * f->tx_drop);
        }
    }
    _tp_uint(t, "block.count", f->ndisk);
    for (guint i = 0; i < f->ndisk; i++) {
        gchar k[64];
        guint64 z = (i == 0);
        g_snprintf(k, sizeof(k), "block.%u.name", i); _tp_str(t, k, f->disk[i]);
        g_snprintf(k, sizeof(k), "block.%u.path", i); _tp_str(t, k, "/var/tmp/x.qcow2");
        if (f->state == 1) { g_snprintf(k, sizeof(k), "block.%u.backingIndex", i); _tp_uint(t, k, 1); }
        if (f->state == 1 && !f->busy) {
            g_snprintf(k, sizeof(k), "block.%u.rd.reqs", i);  _tp_ull(t, k, z * f->rd_reqs);
            g_snprintf(k, sizeof(k), "block.%u.rd.bytes", i); _tp_ull(t, k, z * f->rd_bytes);
            g_snprintf(k, sizeof(k), "block.%u.rd.times", i); _tp_ull(t, k, z * f->rd_times);
            g_snprintf(k, sizeof(k), "block.%u.wr.reqs", i);  _tp_ull(t, k, z * f->wr_reqs);
            g_snprintf(k, sizeof(k), "block.%u.wr.bytes", i); _tp_ull(t, k, z * f->wr_bytes);
            g_snprintf(k, sizeof(k), "block.%u.wr.times", i); _tp_ull(t, k, z * f->wr_times);
            g_snprintf(k, sizeof(k), "block.%u.fl.reqs", i);  _tp_ull(t, k, z * f->fl_reqs);
            g_snprintf(k, sizeof(k), "block.%u.fl.times", i); _tp_ull(t, k, z * f->fl_times);
        }
        if (!f->busy) {
            g_snprintf(k, sizeof(k), "block.%u.allocation", i); _tp_ull(t, k, 13828096);
            g_snprintf(k, sizeof(k), "block.%u.capacity", i);   _tp_ull(t, k, 8589934592ull);
            g_snprintf(k, sizeof(k), "block.%u.physical", i);   _tp_ull(t, k, 2483200);
        }
    }
}

                                             
static PcvVmDiagSample *
_sample(const FixVm *f, const gchar *name, gint domid, gdouble t_s)
{
    TP t;
    _tp_init(&t);
    _fix_params(f, &t);
    PcvVmDiagSample *s = g_new0(PcvVmDiagSample, 1);
    gchar *uuid = g_strdup_printf("00000000-0000-0000-0000-%012x", g_str_hash(name) & 0xffffffu);
    g_assert_true(pcv_vm_diag_parse((virTypedParameter *)t.a->data, (gint)t.a->len, name, uuid,
                                    domid, TRUE, TRUE, s));
    g_free(uuid);
    s->mono_us = (gint64)(t_s * 1e6);
    s->wall_ms = (gint64)(1794757394.0 * 1000.0 + t_s * 1000.0);
    _tp_clear(&t);
    return s;
}

                                                                                
static PcvCgroupVmStat *
_cg(gint domid, const gchar *norm, gdouble mem_some, gdouble mem_full, gdouble io_some,
    gdouble io_full, guint nchild, const guint64 *throttle_child, guint64 periods,
    guint64 throttled)
{
    PcvCgroupVmStat *st = g_new0(PcvCgroupVmStat, 1);
    st->ref.kind = PCV_CGROUP_PSI_KIND_VM;
    st->ref.domain_id = domid;
    g_strlcpy(st->ref.name, norm, sizeof(st->ref.name));
    g_strlcpy(st->ref.norm_name, norm, sizeof(st->ref.norm_name));
    for (gint r = 0; r < PCV_CGROUP_PSI_RES_COUNT; r++) {
        st->have_psi[r] = TRUE;
        st->psi[r].have_some = st->psi[r].have_full = TRUE;
    }
    st->psi[PCV_CGROUP_PSI_RES_MEMORY].some_avg10 = mem_some;
    st->psi[PCV_CGROUP_PSI_RES_MEMORY].full_avg10 = mem_full;
    st->psi[PCV_CGROUP_PSI_RES_IO].some_avg10 = io_some;
    st->psi[PCV_CGROUP_PSI_RES_IO].full_avg10 = io_full;
    st->children = g_array_new(FALSE, TRUE, sizeof(PcvCgroupCpuChild));
    st->sum.have = TRUE;
    st->sum.nr_periods = periods;
    st->sum.nr_throttled = throttled;
    for (guint i = 0; i < nchild; i++) {
        PcvCgroupCpuChild c = { .throttled_usec = throttle_child ? throttle_child[i] : 0 };
        g_snprintf(c.name, sizeof(c.name), "vcpu%u", i);
        g_array_append_val(st->children, c);
        st->sum.throttled_usec += c.throttled_usec;
    }
    return st;
}

                      
static PcvCgroupVmStat *
_cg_quiet(gint domid, const gchar *norm)
{
    return _cg(domid, norm, 0, 0, 0, 0, 2, NULL, 100, 0);
}

static const PcvVmDiagHostIn HOST8 = { .logical_cpus = 8, .physical_cores = 4,
                                        .mem_total_kb = 16 * 1024 * 1024 };

                                                                    
static PcvVmDiagSnapshot *
_step(PcvVmDiagTable *t, PcvVmDiagSample *s, PcvCgroupVmStat *cg)
{
    GPtrArray *run = g_ptr_array_new_with_free_func(pcv_vm_diag_sample_free);
    GPtrArray *cgs = g_ptr_array_new_with_free_func(pcv_cgroup_vm_stat_free);
    if (s) g_ptr_array_add(run, s);
    if (cg) g_ptr_array_add(cgs, cg);
    PcvVmDiagSnapshot *snap = g_new0(PcvVmDiagSnapshot, 1);
    PcvVmDiagHostIn h = HOST8;
    h.observed_mono_us = s ? s->mono_us : 0;
    h.observed_wall_ms = s ? s->wall_ms : 0;
    pcv_vm_diag_table_step(t, run, NULL, cgs, &h, snap);
    g_ptr_array_unref(run);
    g_ptr_array_unref(cgs);
    return snap;
}

static const PcvVmDiagRecord *
_rec(const PcvVmDiagSnapshot *s, const gchar *name)
{
    g_assert_nonnull(s);
    g_assert_nonnull(s->records);
    for (guint i = 0; i < s->records->len; i++) {
        const PcvVmDiagRecord *r = g_ptr_array_index(s->records, i);
        if (strcmp(r->name, name) == 0) return r;
    }
    return NULL;
}

static const PcvVmDiagHint *
_hint(const PcvVmDiagRecord *r, PcvVmDiagHintCode code)
{
    for (guint i = 0; i < r->n_hints; i++)
        if (r->hints[i].code == code) return &r->hints[i];
    return NULL;
}

#define ASSERT_NULL(x)   g_assert_true(isnan(x))
#define ASSERT_NEAR(x, v) g_assert_cmpfloat_with_epsilon((x), (v), 1e-6 * MAX(1.0, fabs(v)))

                                                                          

static void
test_parse_running_full(void)
{
    FixVm f = _fix_default();
    f.kvm_noise = TRUE;
    PcvVmDiagSample *s = _sample(&f, "survey-sample-vm", 2, 0);

    guint want = PCV_VM_DIAG_B_CPU | PCV_VM_DIAG_B_VCPU | PCV_VM_DIAG_B_VCPU_DELAY |
                 PCV_VM_DIAG_B_BALLOON | PCV_VM_DIAG_B_GUEST | PCV_VM_DIAG_B_RSS |
                 PCV_VM_DIAG_B_DISK | PCV_VM_DIAG_B_NET;
    g_assert_cmpuint(s->present, ==, want);
    g_assert_false(s->busy);
    g_assert_true(s->active);
    g_assert_cmpint(s->state, ==, 1);
    g_assert_cmpint(s->domain_id, ==, 2);
    g_assert_cmpuint(s->cpu_time_ns, ==, 22698649000ull);
    g_assert_cmpuint(s->vcpus, ==, 2);
    g_assert_cmpuint(s->vcpu_delay->len, ==, 2);
    g_assert_cmpuint(g_array_index(s->vcpu_delay, PcvVmDiagVcpuDelay, 0).delay_ns, ==, 3104193);
    g_assert_cmpuint(g_array_index(s->vcpu_delay, PcvVmDiagVcpuDelay, 1).delay_ns, ==, 1069081);
    g_assert_cmpuint(s->current_kb, ==, 2097152);
    g_assert_cmpuint(s->rss_kb, ==, 522732);
    g_assert_true(s->have_usable && s->have_unused && s->have_available);
    g_assert_cmpuint(s->usable_kb, ==, 1719704);
    g_assert_cmpuint(s->major_fault, ==, 1072);
    g_assert_cmpuint(s->rd_reqs, ==, 7320);
    g_assert_cmpuint(s->rd_times_ns, ==, 5414637423ull);
    g_assert_cmpuint(s->fl_reqs, ==, 56);
    g_assert_cmpuint(s->disk_count, ==, 1);
    g_assert_cmpuint(s->net_count, ==, 1);
    g_assert_cmpuint(s->rx_bytes, ==, 2857);
    g_assert_cmpuint(s->tx_pkts, ==, 16);
    pcv_vm_diag_sample_free(s);
}

                                                                 
static void
test_parse_stopped_drops_cpu(void)
{
    TP t;
    _tp_init(&t);
    _tp_int(&t, "state.state", 5);
    _tp_int(&t, "state.reason", 0);
    _tp_ull(&t, "cpu.time", 79400000000ull);
    _tp_ull(&t, "cpu.user", 43610000000ull);
    _tp_ull(&t, "cpu.system", 35790000000ull);
    _tp_ull(&t, "balloon.current", 262144);
    _tp_ull(&t, "balloon.maximum", 262144);
    _tp_uint(&t, "vcpu.current", 1);
    _tp_uint(&t, "vcpu.maximum", 1);
    _tp_uint(&t, "block.count", 1);
    _tp_str(&t, "block.0.name", "vda");
    _tp_str(&t, "block.0.path", "/var/tmp/specreview-20261116/c.qcow2");
    _tp_ull(&t, "block.0.allocation", 17408);
    _tp_ull(&t, "block.0.capacity", 1073741824ull);
    _tp_ull(&t, "block.0.physical", 196624);
    g_assert_cmpuint(t.a->len, ==, 15);                     

    PcvVmDiagSample s = { 0 };
    g_assert_true(pcv_vm_diag_parse((virTypedParameter *)t.a->data, (gint)t.a->len,
                                    "specreview-inact", "u-inact", -1, TRUE, TRUE, &s));
    g_assert_false(s.active);
    g_assert_false(s.busy);                                              
    g_assert_cmpuint(s.present, ==, PCV_VM_DIAG_B_VCPU | PCV_VM_DIAG_B_BALLOON);
    g_assert_cmpuint(s.cpu_time_ns, ==, 0);                 
    g_assert_false(s.present & PCV_VM_DIAG_B_DISK);                             
    pcv_vm_diag_sample_clear(&s);
    _tp_clear(&t);
}

                                                                         
static void
test_parse_busy_partial(void)
{
    TP t;
    _tp_init(&t);
    _tp_int(&t, "state.state", 1);
    _tp_int(&t, "state.reason", 1);
    _tp_ull(&t, "cpu.time", 231920000);
    _tp_ull(&t, "cpu.user", 148143000);
    _tp_ull(&t, "cpu.system", 83777000);
    _tp_uint(&t, "cpu.cache.monitor.count", 0);
    _tp_ull(&t, "balloon.current", 262144);
    _tp_ull(&t, "balloon.maximum", 262144);
    _tp_uint(&t, "vcpu.current", 1);
    _tp_uint(&t, "vcpu.maximum", 1);
    _tp_int(&t, "vcpu.0.state", 1);
    _tp_ull(&t, "vcpu.0.time", 140000000);
    _tp_ull(&t, "vcpu.0.wait", 0);
    _tp_ull(&t, "vcpu.0.delay", 33660);
    _tp_uint(&t, "net.count", 0);
    _tp_uint(&t, "block.count", 2);
    _tp_str(&t, "block.0.name", "vda");
    _tp_str(&t, "block.0.path", "/var/tmp/specreview-20261116/a.qcow2");
    _tp_uint(&t, "block.0.backingIndex", 2);
    _tp_str(&t, "block.1.name", "vdb");
    _tp_str(&t, "block.1.path", "/var/tmp/specreview-20261116/b.qcow2");
    _tp_uint(&t, "block.1.backingIndex", 1);
    g_assert_cmpuint(t.a->len, ==, 22);
                                                                             
                                                          
    _tp_ull(&t, "cpu.haltpoll.success.time", 47268317);
    _tp_ull(&t, "cpu.haltpoll.fail.time", 76908458);
    g_assert_cmpuint(t.a->len, ==, 24);

    PcvVmDiagSample s = { 0 };
    g_assert_true(pcv_vm_diag_parse((virTypedParameter *)t.a->data, (gint)t.a->len,
                                    "specreview-busy", "u-busy", 5, TRUE, TRUE, &s));
    g_assert_true(s.active);
    g_assert_true(s.busy);
    guint keep = PCV_VM_DIAG_B_CPU | PCV_VM_DIAG_B_VCPU | PCV_VM_DIAG_B_VCPU_DELAY |
                 PCV_VM_DIAG_B_BALLOON | PCV_VM_DIAG_B_NET;
    g_assert_cmpuint(s.present, ==, keep);
    g_assert_false(s.present & PCV_VM_DIAG_B_RSS);
    g_assert_false(s.present & PCV_VM_DIAG_B_GUEST);
    g_assert_false(s.present & PCV_VM_DIAG_B_DISK);
    g_assert_cmpuint(s.rd_reqs, ==, 0);                                        
    pcv_vm_diag_sample_clear(&s);
    _tp_clear(&t);
}

                                                                        
static void
test_parse_guest_stats_absent(void)
{
    FixVm f = _fix_default();
    f.lu = 0;
    PcvVmDiagSample *s = _sample(&f, "specreview-busy", 5, 0);
    g_assert_false(s->busy);
    g_assert_false(s->present & PCV_VM_DIAG_B_GUEST);
    g_assert_true(s->present & PCV_VM_DIAG_B_RSS);
    g_assert_true(s->present & PCV_VM_DIAG_B_DISK);
    g_assert_false(s->have_usable);
    pcv_vm_diag_sample_free(s);
}

static void
test_parse_no_vcpu_delay(void)
{
    FixVm f = _fix_default();
    f.no_delay = TRUE;
    PcvVmDiagSample *s = _sample(&f, "nodelay", 3, 0);
    g_assert_false(s->present & PCV_VM_DIAG_B_VCPU_DELAY);
    g_assert_true(s->present & PCV_VM_DIAG_B_VCPU);
    g_assert_cmpuint(s->vcpus, ==, 2);
    g_assert_cmpuint(s->vcpu_delay->len, ==, 0);
    pcv_vm_diag_sample_free(s);
}

                                                                 
static void
test_parse_skips_kvm_stats(void)
{
    FixVm f = _fix_default();
    PcvVmDiagSample *a = _sample(&f, "kvm", 2, 0);
    f.kvm_noise = TRUE;
    PcvVmDiagSample *b = _sample(&f, "kvm", 2, 0);
    g_assert_cmpuint(a->present, ==, b->present);
    g_assert_cmpuint(a->vcpu_delay->len, ==, b->vcpu_delay->len);
    for (guint i = 0; i < a->vcpu_delay->len; i++)
        g_assert_cmpuint(g_array_index(a->vcpu_delay, PcvVmDiagVcpuDelay, i).delay_ns, ==,
                         g_array_index(b->vcpu_delay, PcvVmDiagVcpuDelay, i).delay_ns);
    g_assert_cmpuint(a->cpu_time_ns, ==, b->cpu_time_ns);
    pcv_vm_diag_sample_free(a);
    pcv_vm_diag_sample_free(b);
}

static void
test_parse_device_set(void)
{
    FixVm f = _fix_default();
    f.ndisk = 2;
    f.disk[0] = "vda"; f.disk[1] = "vdb";
    PcvVmDiagSample *a = _sample(&f, "dev", 2, 0);
    f.disk[0] = "vdb"; f.disk[1] = "vda";                                
    PcvVmDiagSample *b = _sample(&f, "dev", 2, 0);
    f.disk[0] = "vda"; f.disk[1] = "vdc";                                
    PcvVmDiagSample *c = _sample(&f, "dev", 2, 0);
    g_assert_cmpuint(a->disk_set_hash, ==, b->disk_set_hash);
    g_assert_cmpuint(a->disk_set_hash, !=, c->disk_set_hash);
    g_assert_cmpuint(a->disk_count, ==, 2);
    pcv_vm_diag_sample_free(a);
    pcv_vm_diag_sample_free(b);
    pcv_vm_diag_sample_free(c);
}

                                                                          

static void
test_rates_basic(void)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm f = _fix_default();
    pcv_vm_diag_snapshot_free(_step(t, _sample(&f, "web", 2, 0), _cg_quiet(2, "web")));

    f.cpu_ns += 2500000000ull;                                               
    f.rd_bytes += 50u * 1000 * 1000; f.rd_reqs += 100; f.rd_times += 200u * 1000 * 1000;
    f.wr_bytes += 5u * 1000 * 1000;  f.wr_reqs += 10;  f.wr_times += 50u * 1000 * 1000;
    f.fl_reqs += 2; f.fl_times += 8u * 1000 * 1000;
    f.rx_bytes += 1000000; f.tx_bytes += 500000; f.rx_pkts += 1000; f.tx_pkts += 500;
    f.rx_drop += 5; f.tx_drop += 5; f.rx_errs += 1; f.tx_errs += 4;
    PcvVmDiagSnapshot *s = _step(t, _sample(&f, "web", 2, 5), _cg_quiet(2, "web"));
    const PcvVmDiagRecord *r = _rec(s, "web");
    g_assert_nonnull(r);
    g_assert_cmpint(r->quality, ==, PCV_VM_DIAG_Q_OK);
    ASSERT_NEAR(r->interval_ms, 5000.0);
    ASSERT_NEAR(r->cpu_pct, 25.0);                                
    ASSERT_NEAR(r->cpu_host_pct, 6.25);                           
    ASSERT_NEAR(r->disk_rd_bps, 10e6);
    ASSERT_NEAR(r->disk_wr_bps, 1e6);
    ASSERT_NEAR(r->disk_rd_iops, 20.0);
    ASSERT_NEAR(r->disk_wr_iops, 2.0);
    ASSERT_NEAR(r->disk_rd_lat_ms, 2.0);                             
    ASSERT_NEAR(r->disk_wr_lat_ms, 5.0);
    ASSERT_NEAR(r->disk_fl_lat_ms, 4.0);
    ASSERT_NEAR(r->net_rx_bps, 200000.0);
    ASSERT_NEAR(r->net_tx_bps, 100000.0);
    ASSERT_NEAR(r->net_rx_pps, 200.0);
    ASSERT_NEAR(r->net_tx_pps, 100.0);
    ASSERT_NEAR(r->net_drop_ps, 2.0);
    ASSERT_NEAR(r->net_err_ps, 1.0);
    ASSERT_NEAR(r->mem_max_mb, 2048.0);
    ASSERT_NEAR(r->mem_alloc_mb, 2048.0);
    ASSERT_NEAR(r->mem_guest_used_mb, (2097152.0 - 1719704.0) / 1024.0);
    ASSERT_NEAR(r->mem_guest_available_mb, 2014860.0 / 1024.0);
    ASSERT_NEAR(r->mem_rss_mb, 522732.0 / 1024.0);
    pcv_vm_diag_snapshot_free(s);

                                                                  
    f.cpu_ns += 12000000000ull;                                 
    s = _step(t, _sample(&f, "web", 2, 10), _cg_quiet(2, "web"));
    ASSERT_NEAR(_rec(s, "web")->cpu_pct, 120.0);
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);
}

static void
test_latency_null_when_no_requests(void)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm f = _fix_default();
    pcv_vm_diag_snapshot_free(_step(t, _sample(&f, "idle", 2, 0), _cg_quiet(2, "idle")));
    PcvVmDiagSnapshot *s = _step(t, _sample(&f, "idle", 2, 5), _cg_quiet(2, "idle"));
    const PcvVmDiagRecord *r = _rec(s, "idle");
                                              
    g_assert_cmpfloat(r->disk_rd_iops, ==, 0.0);
    g_assert_cmpfloat(r->disk_rd_bps, ==, 0.0);
    ASSERT_NULL(r->disk_rd_lat_ms);
    ASSERT_NULL(r->disk_wr_lat_ms);
    ASSERT_NULL(r->disk_fl_lat_ms);
    g_assert_false(r->missing & PCV_VM_DIAG_M_DISK);                                
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);
}

                                                           
static void
test_interval_bounds(void)
{
    static const struct { gdouble dt_s; gboolean ok; } CASES[] = {
        { 0.0, FALSE }, { -0.001, FALSE }, { 30.001, FALSE }, { 30.0, TRUE },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(CASES); i++) {
        PcvVmDiagTable *t = pcv_vm_diag_table_new();
        FixVm f = _fix_default();
        pcv_vm_diag_snapshot_free(_step(t, _sample(&f, "gap", 2, 100), _cg_quiet(2, "gap")));
        f.cpu_ns += 1000000000ull;
        PcvVmDiagSnapshot *s = _step(t, _sample(&f, "gap", 2, 100 + CASES[i].dt_s),
                                     _cg_quiet(2, "gap"));
        const PcvVmDiagRecord *r = _rec(s, "gap");
        if (CASES[i].ok) {
            g_assert_cmpint(r->quality, ==, PCV_VM_DIAG_Q_OK);
            ASSERT_NEAR(r->interval_ms, 30000.0);
            g_assert_false(isnan(r->cpu_pct));
        } else {
            g_assert_cmpint(r->quality, ==, PCV_VM_DIAG_Q_WARMING_UP);
            g_assert_cmpint(r->reason, ==, PCV_VM_DIAG_R_RESET);
            ASSERT_NULL(r->cpu_pct);
            ASSERT_NULL(r->disk_rd_bps);
            ASSERT_NULL(r->interval_ms);
        }
        pcv_vm_diag_snapshot_free(s);
        pcv_vm_diag_table_free(t);
    }
}

                                             
static void
test_reset_rules(void)
{
                                       
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm f = _fix_default();
    pcv_vm_diag_snapshot_free(_step(t, _sample(&f, "r", 2, 0), _cg_quiet(2, "r")));
    FixVm g = _fix_default();
    g.cpu_ns = 1000000000ull; g.rd_bytes = 1000; g.rd_reqs = 3;                    
    PcvVmDiagSnapshot *s = _step(t, _sample(&g, "r", 9, 5), _cg_quiet(9, "r"));
    const PcvVmDiagRecord *r = _rec(s, "r");
    g_assert_cmpint(r->quality, ==, PCV_VM_DIAG_Q_WARMING_UP);
    g_assert_cmpint(r->reason, ==, PCV_VM_DIAG_R_RESET);
    ASSERT_NULL(r->cpu_pct);
    ASSERT_NULL(r->disk_rd_bps);
    ASSERT_NULL(r->net_rx_bps);
    pcv_vm_diag_snapshot_free(s);
                                    
    g.cpu_ns += 500000000ull;
    s = _step(t, _sample(&g, "r", 9, 10), _cg_quiet(9, "r"));
    ASSERT_NEAR(_rec(s, "r")->cpu_pct, 5.0);
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);

                                               
    t = pcv_vm_diag_table_new();
    f = _fix_default();
    pcv_vm_diag_snapshot_free(_step(t, _sample(&f, "d", 2, 0), _cg_quiet(2, "d")));
    f.cpu_ns += 1000000000ull;
    f.rd_bytes -= 1;
    s = _step(t, _sample(&f, "d", 2, 5), _cg_quiet(2, "d"));
    r = _rec(s, "d");
    g_assert_cmpint(r->reason, ==, PCV_VM_DIAG_R_RESET);
    ASSERT_NULL(r->disk_rd_bps);
    ASSERT_NEAR(r->cpu_pct, 10.0);
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);

                                          
    t = pcv_vm_diag_table_new();
    f = _fix_default();
    pcv_vm_diag_snapshot_free(_step(t, _sample(&f, "v", 2, 0), _cg_quiet(2, "v")));
    f.vcpus = 3; f.delay[2] = 5; f.cpu_ns += 1000000000ull;
    s = _step(t, _sample(&f, "v", 2, 5), _cg_quiet(2, "v"));
    r = _rec(s, "v");
    g_assert_cmpint(r->reason, ==, PCV_VM_DIAG_R_RESET);
    ASSERT_NULL(r->vcpu_delay_pct);
    g_assert_false(isnan(r->cpu_pct));
    g_assert_false(isnan(r->disk_rd_bps));
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);

                                        
    t = pcv_vm_diag_table_new();
    f = _fix_default();
    pcv_vm_diag_snapshot_free(_step(t, _sample(&f, "x", 2, 0), _cg_quiet(2, "x")));
    f.disk[0] = "vdb";
    s = _step(t, _sample(&f, "x", 2, 5), _cg_quiet(2, "x"));
    r = _rec(s, "x");
    g_assert_cmpint(r->reason, ==, PCV_VM_DIAG_R_RESET);
    ASSERT_NULL(r->disk_rd_bps);
    g_assert_false(isnan(r->net_rx_bps));
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);
}

                                                              
                                                                    
static void
test_missing_bundle_not_reset(void)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm f = _fix_default();
    pcv_vm_diag_snapshot_free(_step(t, _sample(&f, "h2", 4, 0), _cg_quiet(4, "h2")));

    FixVm busy = f;
    busy.busy = TRUE;
    busy.cpu_ns += 500000000ull;
    PcvVmDiagSnapshot *s = _step(t, _sample(&busy, "h2", 4, 5), _cg_quiet(4, "h2"));
    const PcvVmDiagRecord *r = _rec(s, "h2");
    g_assert_cmpint(r->quality, ==, PCV_VM_DIAG_Q_BUSY);
    g_assert_cmpint(r->reason, ==, PCV_VM_DIAG_R_BUSY);
    g_assert_true(r->missing & PCV_VM_DIAG_M_DISK);
    g_assert_true(r->missing & PCV_VM_DIAG_M_RSS);
    ASSERT_NULL(r->disk_rd_bps);
    ASSERT_NEAR(r->cpu_pct, 5.0);                                     
    g_assert_false(isnan(r->net_rx_bps));
    pcv_vm_diag_snapshot_free(s);

    f.cpu_ns = busy.cpu_ns + 500000000ull;
    f.rd_bytes += 500u * 1000 * 1000;                                       
    s = _step(t, _sample(&f, "h2", 4, 10), _cg_quiet(4, "h2"));
    r = _rec(s, "h2");
    g_assert_cmpint(r->quality, ==, PCV_VM_DIAG_Q_OK);
    ASSERT_NEAR(r->disk_rd_bps, 50e6);                                        
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);
}

                                                                
                                                 
static void
test_guest_rate_by_last_update(void)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm f = _fix_default();
    f.lu = 1000; f.swap_in = 0; f.major = 10;
    PcvVmDiagSnapshot *s = _step(t, _sample(&f, "g", 2, 0), _cg_quiet(2, "g"));
    ASSERT_NULL(_rec(s, "g")->swap_in_kbps);                               
    pcv_vm_diag_snapshot_free(s);

    s = _step(t, _sample(&f, "g", 2, 5), _cg_quiet(2, "g"));               
    const PcvVmDiagRecord *r = _rec(s, "g");
    ASSERT_NULL(r->swap_in_kbps);
    gdouble age5 = r->guest_stats_age_s;
    pcv_vm_diag_snapshot_free(s);

    f.lu = 1010; f.swap_in = 1000; f.major = 60;                                       
    s = _step(t, _sample(&f, "g", 2, 10), _cg_quiet(2, "g"));
    r = _rec(s, "g");
    ASSERT_NEAR(r->swap_in_kbps, 100.0);
    ASSERT_NEAR(r->swap_out_kbps, 0.0);
    ASSERT_NEAR(r->major_faults_ps, 5.0);
    pcv_vm_diag_snapshot_free(s);

    s = _step(t, _sample(&f, "g", 2, 15), _cg_quiet(2, "g"));                          
    r = _rec(s, "g");
    ASSERT_NEAR(r->swap_in_kbps, 100.0);
    gdouble age15 = r->guest_stats_age_s;
    pcv_vm_diag_snapshot_free(s);
    s = _step(t, _sample(&f, "g", 2, 20), _cg_quiet(2, "g"));
    r = _rec(s, "g");
    ASSERT_NEAR(r->swap_in_kbps, 100.0);
    g_assert_cmpfloat(r->guest_stats_age_s, >, age15);
    g_assert_cmpfloat(age15, >=, 0.0);
    (void)age5;
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);
}

                                                                   
static void
test_throttle_normalized(void)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm f = _fix_default();
    f.vcpus = 4;
    guint64 th0[4] = { 0, 0, 0, 0 };
    pcv_vm_diag_snapshot_free(_step(t, _sample(&f, "th", 2, 0),
                                    _cg(2, "th", 0, 0, 0, 0, 4, th0, 100, 0)));
    guint64 th1[4] = { 2500000, 2500000, 2500000, 2500000 };                      
    PcvVmDiagSnapshot *s = _step(t, _sample(&f, "th", 2, 5),
                                 _cg(2, "th", 0, 0, 0, 0, 4, th1, 150, 20));
    const PcvVmDiagRecord *r = _rec(s, "th");
    ASSERT_NEAR(r->throttled_pct, 50.0);                                
    ASSERT_NEAR(r->throttled_max_pct, 50.0);
    ASSERT_NEAR(r->throttled_periods_ratio, 0.4);              
    pcv_vm_diag_snapshot_free(s);

                                                                     
    s = _step(t, _sample(&f, "th", 2, 10), _cg(2, "th", 0, 0, 0, 0, 4, th1, 150, 20));
    r = _rec(s, "th");
    ASSERT_NULL(r->throttled_periods_ratio);
    g_assert_cmpfloat(r->throttled_pct, ==, 0.0);
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);
}

                                                                          

                                              
static void
_quality_after(const FixVm *a, const FixVm *b, gboolean with_cg, PcvVmDiagQuality *q,
               PcvVmDiagReason *why, guint *missing)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    pcv_vm_diag_snapshot_free(_step(t, _sample(a, "q", 2, 0), with_cg ? _cg_quiet(2, "q") : NULL));
    PcvVmDiagSnapshot *s = _step(t, _sample(b, "q", 2, 5), with_cg ? _cg_quiet(2, "q") : NULL);
    const PcvVmDiagRecord *r = _rec(s, "q");
    *q = r->quality; *why = r->reason; *missing = r->missing;
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);
}

static void
test_quality_table(void)
{
    PcvVmDiagQuality q;
    PcvVmDiagReason why;
    guint miss;
    FixVm f = _fix_default();

            
    _quality_after(&f, &f, TRUE, &q, &why, &miss);
    g_assert_cmpint(q, ==, PCV_VM_DIAG_Q_OK);
    g_assert_cmpint(why, ==, PCV_VM_DIAG_R_NONE);
    g_assert_cmpuint(miss, ==, 0);

                                                                    
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm b = f;
    b.busy = TRUE;
    PcvVmDiagSnapshot *s = _step(t, _sample(&b, "q", 2, 0), _cg_quiet(2, "q"));
    g_assert_cmpint(_rec(s, "q")->quality, ==, PCV_VM_DIAG_Q_WARMING_UP);
    g_assert_cmpint(_rec(s, "q")->reason, ==, PCV_VM_DIAG_R_FIRST_SAMPLE);
    g_assert_true(_rec(s, "q")->missing & PCV_VM_DIAG_M_DISK);                       
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);

                                    
    FixVm d = f;
    d.cpu_ns -= 1;
    _quality_after(&f, &d, TRUE, &q, &why, &miss);
    g_assert_cmpint(q, ==, PCV_VM_DIAG_Q_WARMING_UP);
    g_assert_cmpint(why, ==, PCV_VM_DIAG_R_RESET);

                                                           
    _quality_after(&f, &b, TRUE, &q, &why, &miss);
    g_assert_cmpint(q, ==, PCV_VM_DIAG_Q_BUSY);
    g_assert_cmpint(why, ==, PCV_VM_DIAG_R_BUSY);
    g_assert_cmpuint(miss, ==, PCV_VM_DIAG_M_GUEST_MEMORY | PCV_VM_DIAG_M_RSS | PCV_VM_DIAG_M_DISK);

                                 
    static const struct { const gchar *what; guint bit; } PARTS[] = {
        { "guest_memory", PCV_VM_DIAG_M_GUEST_MEMORY }, { "vcpu_delay", PCV_VM_DIAG_M_VCPU_DELAY },
        { "net", PCV_VM_DIAG_M_NET }, { "rss", PCV_VM_DIAG_M_RSS },
        { "cpu", PCV_VM_DIAG_M_CPU }, { "cgroup", PCV_VM_DIAG_M_CGROUP },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(PARTS); i++) {
        FixVm p = f;
        gboolean cg = TRUE;
        if (PARTS[i].bit == PCV_VM_DIAG_M_GUEST_MEMORY) p.lu = 0;
        if (PARTS[i].bit == PCV_VM_DIAG_M_VCPU_DELAY) p.no_delay = TRUE;
        if (PARTS[i].bit == PCV_VM_DIAG_M_NET) p.no_net = TRUE;
        if (PARTS[i].bit == PCV_VM_DIAG_M_CPU) p.no_cpu = TRUE;
        if (PARTS[i].bit == PCV_VM_DIAG_M_CGROUP) cg = FALSE;
        if (PARTS[i].bit == PCV_VM_DIAG_M_RSS) {
                                                               
            PcvVmDiagTable *tt = pcv_vm_diag_table_new();
            p.no_rss = TRUE;
            for (gint k = 0; k < 2; k++) {
                TP tp;
                _tp_init(&tp);
                _fix_params(&p, &tp);
                PcvVmDiagSample *sm = g_new0(PcvVmDiagSample, 1);
                g_assert_true(pcv_vm_diag_parse((virTypedParameter *)tp.a->data, (gint)tp.a->len,
                                                "q", "u-q", 2, FALSE, TRUE, sm));
                sm->mono_us = k * 5000000;
                sm->wall_ms = 1794757394000 + k * 5000;
                PcvVmDiagSnapshot *ss = _step(tt, sm, _cg_quiet(2, "q"));
                if (k == 1) {
                    g_assert_cmpint(_rec(ss, "q")->quality, ==, PCV_VM_DIAG_Q_PARTIAL);
                    g_assert_cmpuint(_rec(ss, "q")->missing, ==, PCV_VM_DIAG_M_RSS);
                }
                pcv_vm_diag_snapshot_free(ss);
                _tp_clear(&tp);
            }
            pcv_vm_diag_table_free(tt);
            continue;
        }
        _quality_after(&p, &p, cg, &q, &why, &miss);
        g_assert_cmpint(q, ==, PCV_VM_DIAG_Q_PARTIAL);
        g_assert_cmpint(why, ==, PCV_VM_DIAG_R_NONE);
        g_assert_cmpuint(miss, ==, PARTS[i].bit);
    }
                            
    {
        PcvVmDiagTable *tt = pcv_vm_diag_table_new();
        for (gint k = 0; k < 2; k++) {
            TP tp;
            _tp_init(&tp);
            _fix_params(&f, &tp);
            PcvVmDiagSample *sm = g_new0(PcvVmDiagSample, 1);
            g_assert_true(pcv_vm_diag_parse((virTypedParameter *)tp.a->data, (gint)tp.a->len,
                                            "q", "u-q", 2, TRUE, FALSE, sm));
            sm->mono_us = k * 5000000;
            sm->wall_ms = 1794757394000 + k * 5000;
            PcvVmDiagSnapshot *ss = _step(tt, sm, _cg_quiet(2, "q"));
            if (k == 1) {
                g_assert_cmpint(_rec(ss, "q")->quality, ==, PCV_VM_DIAG_Q_PARTIAL);
                g_assert_cmpuint(_rec(ss, "q")->missing, ==, PCV_VM_DIAG_M_DISK);
            }
            pcv_vm_diag_snapshot_free(ss);
            _tp_clear(&tp);
        }
        pcv_vm_diag_table_free(tt);
    }

                                    
    t = pcv_vm_diag_table_new();
    GPtrArray *cfgs = g_ptr_array_new_with_free_func(pcv_vm_diag_config_free);
    PcvVmDiagConfig *c = g_new0(PcvVmDiagConfig, 1);
    c->name = g_strdup("off"); c->uuid = g_strdup("u-off"); c->state = 5; c->domain_id = -1;
    c->have_vcpus = TRUE; c->vcpus = 4; c->have_balloon = TRUE;
    c->maximum_kb = 4194304; c->current_kb = 4194304;
    g_ptr_array_add(cfgs, c);
    s = g_new0(PcvVmDiagSnapshot, 1);
    pcv_vm_diag_table_step(t, NULL, cfgs, NULL, &HOST8, s);
    const PcvVmDiagRecord *r = _rec(s, "off");
    g_assert_cmpint(r->quality, ==, PCV_VM_DIAG_Q_STOPPED);
    g_assert_cmpuint(r->vcpus, ==, 4);
    ASSERT_NEAR(r->mem_max_mb, 4096.0);
    ASSERT_NULL(r->mem_alloc_mb);
    ASSERT_NULL(r->cpu_pct);
    ASSERT_NULL(r->disk_rd_bps);
    g_assert_cmpuint(r->n_hints, ==, 0);
    pcv_vm_diag_snapshot_free(s);
    g_ptr_array_unref(cfgs);
    pcv_vm_diag_table_free(t);
}

static void
test_last_ok_at_kept_on_busy(void)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm f = _fix_default();
    pcv_vm_diag_snapshot_free(_step(t, _sample(&f, "lk", 2, 0), _cg_quiet(2, "lk")));
    PcvVmDiagSnapshot *s = _step(t, _sample(&f, "lk", 2, 5), _cg_quiet(2, "lk"));
    gint64 ok5 = _rec(s, "lk")->last_ok_at_ms;
    g_assert_cmpint(ok5, ==, _rec(s, "lk")->observed_wall_ms);
    pcv_vm_diag_snapshot_free(s);

    FixVm b = f;
    b.busy = TRUE;
    s = _step(t, _sample(&b, "lk", 2, 10), _cg_quiet(2, "lk"));
    g_assert_cmpint(_rec(s, "lk")->quality, ==, PCV_VM_DIAG_Q_BUSY);
    g_assert_cmpint(_rec(s, "lk")->last_ok_at_ms, ==, ok5);                           
    pcv_vm_diag_snapshot_free(s);

    s = _step(t, _sample(&f, "lk", 2, 15), _cg_quiet(2, "lk"));
    g_assert_cmpint(_rec(s, "lk")->last_ok_at_ms, ==, ok5 + 10000);
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);
}

                                                                          

                                                      
typedef void (*AdvanceFn)(FixVm *f, gint step, gpointer ud);
typedef PcvCgroupVmStat *(*CgFn)(gint step, gpointer ud);

static PcvVmDiagRecord *
_run(gint n, AdvanceFn adv, CgFn cgf, gpointer ud)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm f = _fix_default();
    PcvVmDiagRecord *last = NULL;
    for (gint i = 0; i < n; i++) {
        if (adv) adv(&f, i, ud);
        PcvVmDiagSnapshot *s = _step(t, _sample(&f, "hv", 2, i * 5.0),
                                     cgf ? cgf(i, ud) : _cg_quiet(2, "hv"));
        if (i == n - 1) {
            last = g_new(PcvVmDiagRecord, 1);
            *last = *_rec(s, "hv");
            last->name = g_strdup(last->name);
            last->uuid = g_strdup(last->uuid);
        }
        pcv_vm_diag_snapshot_free(s);
    }
    pcv_vm_diag_table_free(t);
    return last;
}

static void
_rec_free(PcvVmDiagRecord *r)
{
    g_free(r->name);
    g_free(r->uuid);
    g_free(r);
}

                                             
static void
_adv_delay(FixVm *f, gint step, gpointer ud)
{
    if (step == 0) return;
    gdouble pct = *(gdouble *)ud;
    guint64 inc = (guint64)llround(pct / 100.0 * 5e9);
    f->delay[0] += inc;
    f->delay[1] += inc;
}

typedef struct { gdouble pct; guint64 acc; } ThrUd;
static PcvCgroupVmStat *
_cg_throttle(gint step, gpointer ud)
{
    ThrUd *u = ud;
    if (step > 0) u->acc += (guint64)llround(u->pct / 100.0 * 5e6);                  
    guint64 th[2] = { u->acc, u->acc };
    return _cg(2, "hv", 0, 0, 0, 0, 2, th, 100 + 50 * step, 0);
}

typedef struct { gdouble ms, ma, is, ia; } PsiUd;
static PcvCgroupVmStat *
_cg_psi(gint step, gpointer ud)
{
    (void)step;
    PsiUd *u = ud;
    return _cg(2, "hv", u->ms, u->ma, u->is, u->ia, 2, NULL, 100, 0);
}

                                                
static void
_adv_swap(FixVm *f, gint step, gpointer ud)
{
    if (step == 0) return;
    gdouble kbps = *(gdouble *)ud;
    f->lu += 5;
    f->swap_in += (guint64)llround(kbps * 5.0);
}

                          
typedef struct { gdouble iops, lat_ms; } LatUd;
static void
_adv_lat(FixVm *f, gint step, gpointer ud)
{
    if (step == 0) return;
    LatUd *u = ud;
    guint64 reqs = (guint64)llround(u->iops * 5.0);
    f->rd_reqs += reqs;
    f->rd_times += (guint64)llround((gdouble)reqs * u->lat_ms * 1e6);
}

static void
_expect(PcvVmDiagRecord *r, PcvVmDiagHintCode code, gint level             )
{
    const PcvVmDiagHint *h = _hint(r, code);
    if (level == 0) g_assert_null(h);
    else { g_assert_nonnull(h); g_assert_cmpint(h->level, ==, level); }
    _rec_free(r);
}

static void
test_hints_thresholds(void)
{
    gdouble v;
                                                                  
    v = 4.99; _expect(_run(3, _adv_delay, NULL, &v), PCV_VM_DIAG_H_CPU_WAIT, 0);
    v = 5.0;  _expect(_run(3, _adv_delay, NULL, &v), PCV_VM_DIAG_H_CPU_WAIT, PCV_VM_DIAG_L_NOTICE);
    v = 20.0; _expect(_run(3, _adv_delay, NULL, &v), PCV_VM_DIAG_H_CPU_WAIT, PCV_VM_DIAG_L_WARN);

                                                       
    ThrUd tu = { 0.99, 0 }; _expect(_run(3, NULL, _cg_throttle, &tu), PCV_VM_DIAG_H_CPU_THROTTLED, 0);
    tu = (ThrUd){ 1.0, 0 };  _expect(_run(3, NULL, _cg_throttle, &tu), PCV_VM_DIAG_H_CPU_THROTTLED, PCV_VM_DIAG_L_NOTICE);
    tu = (ThrUd){ 10.0, 0 }; _expect(_run(3, NULL, _cg_throttle, &tu), PCV_VM_DIAG_H_CPU_THROTTLED, PCV_VM_DIAG_L_WARN);

                                                                  
    v = 0.0;    _expect(_run(3, _adv_swap, NULL, &v), PCV_VM_DIAG_H_MEM_SWAP, 0);
    v = 1.0;    _expect(_run(3, _adv_swap, NULL, &v), PCV_VM_DIAG_H_MEM_SWAP, PCV_VM_DIAG_L_NOTICE);
    v = 1023.8; _expect(_run(3, _adv_swap, NULL, &v), PCV_VM_DIAG_H_MEM_SWAP, PCV_VM_DIAG_L_NOTICE);
    v = 1024.0; _expect(_run(3, _adv_swap, NULL, &v), PCV_VM_DIAG_H_MEM_SWAP, PCV_VM_DIAG_L_WARN);

                                                             
    PsiUd pu = { 4.99, 0, 0, 0 }; _expect(_run(1, NULL, _cg_psi, &pu), PCV_VM_DIAG_H_MEM_PRESSURE, 0);
    pu = (PsiUd){ 5.0, 0, 0, 0 };  _expect(_run(1, NULL, _cg_psi, &pu), PCV_VM_DIAG_H_MEM_PRESSURE, PCV_VM_DIAG_L_NOTICE);
    pu = (PsiUd){ 9.0, 5.0, 0, 0 }; _expect(_run(1, NULL, _cg_psi, &pu), PCV_VM_DIAG_H_MEM_PRESSURE, PCV_VM_DIAG_L_WARN);
                                           
    pu = (PsiUd){ 0, 0, 19.99, 0 }; _expect(_run(1, NULL, _cg_psi, &pu), PCV_VM_DIAG_H_IO_PRESSURE, 0);
    pu = (PsiUd){ 0, 0, 20.0, 9.99 }; _expect(_run(1, NULL, _cg_psi, &pu), PCV_VM_DIAG_H_IO_PRESSURE, PCV_VM_DIAG_L_NOTICE);
    pu = (PsiUd){ 0, 0, 30.0, 10.0 }; _expect(_run(1, NULL, _cg_psi, &pu), PCV_VM_DIAG_H_IO_PRESSURE, PCV_VM_DIAG_L_WARN);

                                                       
    LatUd lu = { 10.0, 19.9 }; _expect(_run(3, _adv_lat, NULL, &lu), PCV_VM_DIAG_H_DISK_LATENCY, 0);
    lu = (LatUd){ 10.0, 20.0 };  _expect(_run(3, _adv_lat, NULL, &lu), PCV_VM_DIAG_H_DISK_LATENCY, PCV_VM_DIAG_L_NOTICE);
    lu = (LatUd){ 10.0, 100.0 }; _expect(_run(3, _adv_lat, NULL, &lu), PCV_VM_DIAG_H_DISK_LATENCY, PCV_VM_DIAG_L_WARN);
    lu = (LatUd){ 4.0, 500.0 };  _expect(_run(3, _adv_lat, NULL, &lu), PCV_VM_DIAG_H_DISK_LATENCY, 0);

                    
    v = 20.0;
    PcvVmDiagRecord *r = _run(3, _adv_delay, NULL, &v);
    const PcvVmDiagHint *h = _hint(r, PCV_VM_DIAG_H_CPU_WAIT);
    ASSERT_NEAR(h->value, 20.0);
    ASSERT_NEAR(h->threshold, 20.0);
    _rec_free(r);
}

                                                                                                 
                                                                                         
                                                     
typedef struct { gdouble delay; ThrUd thr; gboolean no_cg; } WaitUd;
static void
_adv_wait(FixVm *f, gint step, gpointer ud)
{
    _adv_delay(f, step, &((WaitUd *)ud)->delay);
}
static PcvCgroupVmStat *
_cg_wait(gint step, gpointer ud)
{
    WaitUd *u = ud;
    return u->no_cg ? NULL : _cg_throttle(step, &u->thr);
}

static void
test_hints_cpu_wait_minus_throttle(void)
{
                                                                                                  
    WaitUd u = { 80.0, { 80.0, 0 }, FALSE };
    PcvVmDiagRecord *r = _run(3, _adv_wait, _cg_wait, &u);
    ASSERT_NEAR(r->vcpu_delay_pct, 80.0);                    
    g_assert_nonnull(_hint(r, PCV_VM_DIAG_H_CPU_THROTTLED));
    _expect(r, PCV_VM_DIAG_H_CPU_WAIT, 0);
    u = (WaitUd){ 80.8, { 80.0, 0 }, FALSE };
    _expect(_run(3, _adv_wait, _cg_wait, &u), PCV_VM_DIAG_H_CPU_WAIT, 0);

                                                                     
    u = (WaitUd){ 85.5, { 80.0, 0 }, FALSE };
    r = _run(3, _adv_wait, _cg_wait, &u);
    ASSERT_NEAR(r->vcpu_delay_pct, 85.5);
    const PcvVmDiagHint *h = _hint(r, PCV_VM_DIAG_H_CPU_WAIT);
    g_assert_nonnull(h);
    g_assert_cmpint(h->level, ==, PCV_VM_DIAG_L_NOTICE);
    ASSERT_NEAR(h->value, 5.5);
    ASSERT_NEAR(h->threshold, 5.0);
    _rec_free(r);
    u = (WaitUd){ 84.5, { 80.0, 0 }, FALSE };
    _expect(_run(3, _adv_wait, _cg_wait, &u), PCV_VM_DIAG_H_CPU_WAIT, 0);
    u = (WaitUd){ 95.0, { 70.0, 0 }, FALSE };
    _expect(_run(3, _adv_wait, _cg_wait, &u), PCV_VM_DIAG_H_CPU_WAIT, PCV_VM_DIAG_L_WARN);

                                                            
    u = (WaitUd){ 20.0, { 0, 0 }, TRUE };
    r = _run(3, _adv_wait, _cg_wait, &u);
    g_assert_true(isnan(r->throttled_pct));
    _expect(r, PCV_VM_DIAG_H_CPU_WAIT, PCV_VM_DIAG_L_WARN);
}

                                                  
                                
typedef struct { const gint *pattern; } PatUd;                                      
static void
_adv_pattern(FixVm *f, gint step, gpointer ud)
{
    const gint *pat = ((PatUd *)ud)->pattern;
    f->no_delay = (pat[step] < 0);
    if (step == 0) return;
                                                              
    gboolean hit = pat[step] != 0;
    guint64 inc = (guint64)llround((hit ? 6.0 : 1.0) / 100.0 * 5e9);
    f->delay[0] += inc;
    f->delay[1] += inc;
}

static void
test_hints_streak(void)
{
                                                         
    static const gint P1[] = { 0, 1, -1, 1 };
    PatUd u = { P1 };
    PcvVmDiagRecord *r = _run(4, _adv_pattern, NULL, &u);
                                                                   
                         
    g_assert_nonnull(_hint(r, PCV_VM_DIAG_H_CPU_WAIT));
    ASSERT_NEAR(r->vcpu_delay_pct, 6.0);
    _rec_free(r);

                           
    static const gint P2[] = { 0, 1, 0, 1 };
    u.pattern = P2;
    r = _run(4, _adv_pattern, NULL, &u);
    g_assert_null(_hint(r, PCV_VM_DIAG_H_CPU_WAIT));
    _rec_free(r);

                   
    static const gint P3[] = { 0, 0, 1 };
    u.pattern = P3;
    r = _run(3, _adv_pattern, NULL, &u);
    g_assert_null(_hint(r, PCV_VM_DIAG_H_CPU_WAIT));
    _rec_free(r);

                          
    PsiUd pu = { 6.0, 0, 0, 0 };
    r = _run(1, NULL, _cg_psi, &pu);
    g_assert_nonnull(_hint(r, PCV_VM_DIAG_H_MEM_PRESSURE));
    _rec_free(r);

                                                            
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm f = _fix_default();
    PcvVmDiagSnapshot *s;
    for (gint i = 0; i < 4; i++) {
        if (i == 1) { f.lu += 10; f.swap_in += 100000; }              
        s = _step(t, _sample(&f, "hv", 2, i * 5.0), _cg_quiet(2, "hv"));
        if (i == 3) {
            ASSERT_NEAR(_rec(s, "hv")->swap_in_kbps, 10000.0);             
            g_assert_null(_hint(_rec(s, "hv"), PCV_VM_DIAG_H_MEM_SWAP));
        }
        pcv_vm_diag_snapshot_free(s);
    }
                         
    f.lu += 10; f.swap_in += 100000;
    s = _step(t, _sample(&f, "hv", 2, 20.0), _cg_quiet(2, "hv"));
    g_assert_nonnull(_hint(_rec(s, "hv"), PCV_VM_DIAG_H_MEM_SWAP));
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);
}

                                                                     
static void
test_guest_stale_rule(void)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm f = _fix_default();
                                                                   
    f.lu = 1794757394ull - 26;
    PcvVmDiagSnapshot *s;
    for (gint i = 0; i < 3; i++) {                         
        s = _step(t, _sample(&f, "st", 2, i * 5.0), _cg_quiet(2, "st"));
        const PcvVmDiagRecord *r = _rec(s, "st");
        if (i == 0) ASSERT_NEAR(r->guest_stats_age_s, 26.0);
        if (i < 2) g_assert_null(_hint(r, PCV_VM_DIAG_H_GUEST_STATS_STALE));
        else {
            const PcvVmDiagHint *h = _hint(r, PCV_VM_DIAG_H_GUEST_STATS_STALE);
            g_assert_nonnull(h);
            g_assert_cmpint(h->level, ==, PCV_VM_DIAG_L_NOTICE);
            ASSERT_NEAR(h->value, 36.0);
        }
        pcv_vm_diag_snapshot_free(s);
    }
    pcv_vm_diag_table_free(t);

                                                                 
    t = pcv_vm_diag_table_new();
    f = _fix_default();
    f.lu = 0;
    for (gint i = 0; i < 4; i++) {
        s = _step(t, _sample(&f, "st", 2, i * 5.0), _cg_quiet(2, "st"));
        ASSERT_NULL(_rec(s, "st")->guest_stats_age_s);
        g_assert_null(_hint(_rec(s, "st"), PCV_VM_DIAG_H_GUEST_STATS_STALE));
        pcv_vm_diag_snapshot_free(s);
    }
    pcv_vm_diag_table_free(t);
}

                                                                         

static PcvVmDiagConfig *
_cfg(const gchar *name, gint state, guint vcpus, guint64 max_kb, guint64 cur_kb, const gchar *owner)
{
    PcvVmDiagConfig *c = g_new0(PcvVmDiagConfig, 1);
    c->name = g_strdup(name);
    c->uuid = g_strdup_printf("u-%s", name);
    c->owner = g_strdup(owner);
    c->state = state;
    c->domain_id = (state == 5) ? -1 : 1;
    c->active = (state != 5);
    c->have_vcpus = TRUE; c->vcpus = vcpus;
    c->have_balloon = TRUE; c->maximum_kb = max_kb; c->current_kb = cur_kb;
    return c;
}

static gboolean
_owner_is(const PcvVmDiagConfig *c, gpointer ud)
{
    return c->owner && strcmp(c->owner, (const gchar *)ud) == 0;
}

static void
test_allocation_table(void)
{
    const guint64 G = 1024 * 1024;                    
    PcvVmDiagConfig *cfgs[] = {
        _cfg("run", 1, 2, 2 * G, 1 * G, "ta"),                   
        _cfg("pause", 3, 4, 4 * G, 4 * G, "tb"),                           
        _cfg("off", 5, 8, 8 * G, 8 * G, "ta"),                                
        _cfg("tmp", 1, 1, G / 2, G / 2, NULL),                             
    };
    PcvVmDiagAllocation a;
    pcv_vm_diag_allocation((const PcvVmDiagConfig *const *)cfgs, G_N_ELEMENTS(cfgs), NULL, NULL, &a);
    g_assert_cmpuint(a.vms_running, ==, 3);
    g_assert_cmpuint(a.vms_defined, ==, 4);
    g_assert_cmpuint(a.vcpu_running, ==, 7);
    g_assert_cmpuint(a.vcpu_defined, ==, 15);
    g_assert_cmpuint(a.mem_running_max_kb, ==, 6 * G + G / 2);
    g_assert_cmpuint(a.mem_running_alloc_kb, ==, 5 * G + G / 2);
    g_assert_cmpuint(a.mem_defined_max_kb, ==, 14 * G + G / 2);
                                               
    g_assert_cmpfloat((gdouble)a.vcpu_defined / HOST8.logical_cpus, >, 1.0);

                                 
    pcv_vm_diag_allocation((const PcvVmDiagConfig *const *)cfgs, G_N_ELEMENTS(cfgs),
                           _owner_is, (gpointer)"ta", &a);
    g_assert_cmpuint(a.vms_defined, ==, 2);
    g_assert_cmpuint(a.vms_running, ==, 1);
    g_assert_cmpuint(a.vcpu_defined, ==, 10);
    g_assert_cmpuint(a.mem_running_alloc_kb, ==, 1 * G);

              
    pcv_vm_diag_allocation(NULL, 0, NULL, NULL, &a);
    g_assert_cmpuint(a.vms_defined, ==, 0);

    for (gsize i = 0; i < G_N_ELEMENTS(cfgs); i++) pcv_vm_diag_config_free(cfgs[i]);
}

                                                                        

                                                                                                  
static void
test_mem_ladder(void)
{
    guint64 used = 999;
    g_assert_cmpint(pcv_vm_diag_mem_ladder(2097152, TRUE, 1719704, TRUE, 1691352, TRUE, 522732, &used), ==,
                    PCV_VM_DIAG_MEM_GUEST_USABLE);
    g_assert_cmpuint(used, ==, 2097152 - 1719704);
    g_assert_cmpint(pcv_vm_diag_mem_ladder(2097152, FALSE, 0, TRUE, 1691352, TRUE, 522732, &used), ==,
                    PCV_VM_DIAG_MEM_GUEST_UNUSED);
    g_assert_cmpuint(used, ==, 2097152 - 1691352);
    g_assert_cmpint(pcv_vm_diag_mem_ladder(2097152, FALSE, 0, FALSE, 0, TRUE, 522732, &used), ==,
                    PCV_VM_DIAG_MEM_RSS);
    g_assert_cmpuint(used, ==, 522732);
    g_assert_cmpint(pcv_vm_diag_mem_ladder(1048576, FALSE, 0, FALSE, 0, TRUE, 1200000, &used), ==,
                    PCV_VM_DIAG_MEM_RSS);
    g_assert_cmpuint(used, ==, 1048576);                                         
                                                                
    g_assert_cmpint(pcv_vm_diag_mem_ladder(1048576, TRUE, 0, TRUE, 10, FALSE, 0, &used), ==,
                    PCV_VM_DIAG_MEM_GUEST_USABLE);
    g_assert_cmpuint(used, ==, 1048576);
                            
    used = 999;
    g_assert_cmpint(pcv_vm_diag_mem_ladder(2097152, FALSE, 0, FALSE, 0, FALSE, 0, &used), ==,
                    PCV_VM_DIAG_MEM_UNAVAILABLE);
    g_assert_cmpuint(used, ==, 0);
    g_assert_cmpint(pcv_vm_diag_mem_ladder(0, TRUE, 0, TRUE, 0, TRUE, 100, &used), ==,
                    PCV_VM_DIAG_MEM_UNAVAILABLE);                        
                                             
    g_assert_cmpint(pcv_vm_diag_mem_ladder(1000, TRUE, 2000, FALSE, 0, TRUE, 300, &used), ==,
                    PCV_VM_DIAG_MEM_RSS);
    g_assert_cmpstr(pcv_vm_diag_mem_source_name(PCV_VM_DIAG_MEM_GUEST_USABLE), ==, "guest_usable");
    g_assert_cmpstr(pcv_vm_diag_mem_source_name(PCV_VM_DIAG_MEM_GUEST_UNUSED), ==, "guest_unused");
    g_assert_cmpstr(pcv_vm_diag_mem_source_name(PCV_VM_DIAG_MEM_RSS), ==, "rss");
    g_assert_cmpstr(pcv_vm_diag_mem_source_name(PCV_VM_DIAG_MEM_UNAVAILABLE), ==, "unavailable");
}

                                                                           

                                                 
static GPtrArray *
_many(gint n, gdouble t_s, const gchar *skip)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(pcv_vm_diag_sample_free);
    FixVm f = _fix_default();
    for (gint i = 0; i < n; i++) {
        gint k = (i * 7) % n;                            
        gchar name[16];
        g_snprintf(name, sizeof(name), "vm%03d", k);
        if (skip && strcmp(name, skip) == 0) continue;
        g_ptr_array_add(a, _sample(&f, name, k + 1, t_s));
    }
    return a;
}

static PcvVmDiagSnapshot *
_step_many(PcvVmDiagTable *t, GPtrArray *run)
{
    PcvVmDiagSnapshot *s = g_new0(PcvVmDiagSnapshot, 1);
    pcv_vm_diag_table_step(t, run, NULL, NULL, &HOST8, s);
    g_ptr_array_unref(run);
    return s;
}

static void
test_table_cap_truncate(void)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    PcvVmDiagSnapshot *s = _step_many(t, _many(300, 0, NULL));
    g_assert_cmpuint(s->records->len, ==, 256);
    g_assert_cmpuint(s->tracked, ==, 256);
    g_assert_cmpuint(s->truncated, ==, 44);
    for (guint i = 0; i < 256; i++) {                   
        gchar want[16];
        g_snprintf(want, sizeof(want), "vm%03u", i);
        g_assert_cmpstr(((PcvVmDiagRecord *)g_ptr_array_index(s->records, i))->name, ==, want);
    }
    pcv_vm_diag_snapshot_free(s);

                                        
    s = _step_many(t, _many(300, 5, "vm010"));
    g_assert_null(_rec(s, "vm010"));
    g_assert_cmpint(_rec(s, "vm011")->reason, !=, PCV_VM_DIAG_R_FIRST_SAMPLE);
    pcv_vm_diag_snapshot_free(s);

                                                      
    s = _step_many(t, _many(300, 10, NULL));
    g_assert_cmpint(_rec(s, "vm010")->quality, ==, PCV_VM_DIAG_Q_WARMING_UP);
    g_assert_cmpint(_rec(s, "vm010")->reason, ==, PCV_VM_DIAG_R_FIRST_SAMPLE);
    g_assert_cmpint(_rec(s, "vm011")->reason, !=, PCV_VM_DIAG_R_FIRST_SAMPLE);
                                   
    g_assert_null(_rec(s, "vm260"));
    pcv_vm_diag_snapshot_free(s);

                                                                       
    s = _step_many(t, _many(250, 15, NULL));
    g_assert_cmpuint(s->truncated, ==, 0);
    g_assert_cmpuint(s->records->len, ==, 250);
    pcv_vm_diag_snapshot_free(s);
    pcv_vm_diag_table_free(t);
}

static void
test_cgroup_join(void)
{
    PcvVmDiagTable *t = pcv_vm_diag_table_new();
    FixVm f = _fix_default();
    GPtrArray *run = g_ptr_array_new_with_free_func(pcv_vm_diag_sample_free);
    g_ptr_array_add(run, _sample(&f, "web-01", 4, 0));                      
    g_ptr_array_add(run, _sample(&f, "spec_review_x1", 7, 0));                          
    g_ptr_array_add(run, _sample(&f, "lonely", 8, 0));                    
    g_ptr_array_add(run, _sample(&f, "oldname", 11, 0));                               

    GPtrArray *cgs = g_ptr_array_new_with_free_func(pcv_cgroup_vm_stat_free);
    g_ptr_array_add(cgs, _cg(4, "web-01", 1.5, 0, 0, 0, 2, NULL, 100, 0));
    g_ptr_array_add(cgs, _cg(-1, "specreviewx1", 2.5, 0, 0, 0, 2, NULL, 100, 0));
    g_ptr_array_add(cgs, _cg(3, "oldname", 3.5, 0, 0, 0, 2, NULL, 100, 0));

    PcvVmDiagSnapshot *s = g_new0(PcvVmDiagSnapshot, 1);
    pcv_vm_diag_table_step(t, run, NULL, cgs, &HOST8, s);
    const PcvVmDiagRecord *r = _rec(s, "web-01");
    g_assert_true(r->have_psi);
    ASSERT_NEAR(r->psi[PCV_CGROUP_PSI_RES_MEMORY][0], 1.5);
    g_assert_false(r->missing & PCV_VM_DIAG_M_CGROUP);

    r = _rec(s, "spec_review_x1");
    g_assert_true(r->have_psi);
    ASSERT_NEAR(r->psi[PCV_CGROUP_PSI_RES_MEMORY][0], 2.5);

    r = _rec(s, "lonely");
    g_assert_false(r->have_psi);
    g_assert_true(r->missing & PCV_VM_DIAG_M_CGROUP);
    ASSERT_NULL(r->psi[PCV_CGROUP_PSI_RES_MEMORY][0]);

    r = _rec(s, "oldname");                                       
    g_assert_false(r->have_psi);
    g_assert_true(r->missing & PCV_VM_DIAG_M_CGROUP);

    pcv_vm_diag_snapshot_free(s);
    g_ptr_array_unref(run);
    g_ptr_array_unref(cgs);
    pcv_vm_diag_table_free(t);
}

                                                                           

void
test_vm_diag_register(void)
{
    g_test_add_func("/vm_diag/parse_running_full", test_parse_running_full);
    g_test_add_func("/vm_diag/parse_stopped_drops_cpu", test_parse_stopped_drops_cpu);
    g_test_add_func("/vm_diag/parse_busy_partial", test_parse_busy_partial);
    g_test_add_func("/vm_diag/parse_guest_stats_absent", test_parse_guest_stats_absent);
    g_test_add_func("/vm_diag/parse_no_vcpu_delay", test_parse_no_vcpu_delay);
    g_test_add_func("/vm_diag/parse_skips_kvm_stats", test_parse_skips_kvm_stats);
    g_test_add_func("/vm_diag/parse_device_set", test_parse_device_set);
    g_test_add_func("/vm_diag/rates_basic", test_rates_basic);
    g_test_add_func("/vm_diag/latency_null_when_no_requests", test_latency_null_when_no_requests);
    g_test_add_func("/vm_diag/interval_bounds", test_interval_bounds);
    g_test_add_func("/vm_diag/reset_rules", test_reset_rules);
    g_test_add_func("/vm_diag/missing_bundle_not_reset", test_missing_bundle_not_reset);
    g_test_add_func("/vm_diag/guest_rate_by_last_update", test_guest_rate_by_last_update);
    g_test_add_func("/vm_diag/throttle_normalized", test_throttle_normalized);
    g_test_add_func("/vm_diag/quality_table", test_quality_table);
    g_test_add_func("/vm_diag/last_ok_at_kept_on_busy", test_last_ok_at_kept_on_busy);
    g_test_add_func("/vm_diag/hints_thresholds", test_hints_thresholds);
    g_test_add_func("/vm_diag/hints_cpu_wait_minus_throttle", test_hints_cpu_wait_minus_throttle);
    g_test_add_func("/vm_diag/hints_streak", test_hints_streak);
    g_test_add_func("/vm_diag/guest_stale_rule", test_guest_stale_rule);
    g_test_add_func("/vm_diag/allocation_table", test_allocation_table);
    g_test_add_func("/vm_diag/mem_ladder", test_mem_ladder);
    g_test_add_func("/vm_diag/table_cap_truncate", test_table_cap_truncate);
    g_test_add_func("/vm_diag/cgroup_join", test_cgroup_join);
}
