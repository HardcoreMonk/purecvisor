                                                                                                
                                                                                           
                                                              
                                                                       
                                         
                          
  
                                                          
  
                                                                               
                 
                                                                               
                                                                      
  
                                                            
                                                 
                                        
                                                                      
                                                 
                                                                
                                      
                                                                       
                                                                       
                                     
  
                  
                                                           
                                                      
                                                             
                      
  
                                                                  
                                                             
                                       
                                                                               
   
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/daemons/cgroup_psi.h"
#include "modules/daemons/prometheus_exporter.h"

                                                                        

                                                      
                                                         
static void
_make_guest(const gchar *root, const gchar *rel)
{
    gchar *dir = g_build_filename(root, rel, NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);

    static const struct { const gchar *file; const gchar *body; } FILES[] = {
        { "cpu.pressure",
          "some avg10=1.25 avg60=0.50 avg300=0.10 total=1234567\n"
          "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n" },
        { "io.pressure",
          "some avg10=2.00 avg60=1.00 avg300=0.20 total=2000000\n"
          "full avg10=1.00 avg60=0.50 avg300=0.10 total=1000000\n" },
        { "memory.pressure",
          "some avg10=3.00 avg60=1.50 avg300=0.30 total=3000000\n"
          "full avg10=1.50 avg60=0.75 avg300=0.15 total=1500000\n" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(FILES); i++) {
        gchar *p = g_build_filename(dir, FILES[i].file, NULL);
        g_assert_true(g_file_set_contents(p, FILES[i].body, -1, NULL));
        g_free(p);
    }
    g_free(dir);
}

                                                                   
                                                                
                                                                         
static void
_make_cpu_stat(const gchar *root, const gchar *rel, guint64 nr_periods,
               guint64 nr_throttled, guint64 throttled_usec)
{
    gchar *dir = g_build_filename(root, rel, NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
    gchar *body = g_strdup_printf(
        "usage_usec 1000\nuser_usec 600\nsystem_usec 400\n"
        "nr_periods %" G_GUINT64_FORMAT "\n"
        "nr_throttled %" G_GUINT64_FORMAT "\n"
        "throttled_usec %" G_GUINT64_FORMAT "\n"
        "nr_bursts 0\nburst_usec 0\n",
        nr_periods, nr_throttled, throttled_usec);
    gchar *p = g_build_filename(dir, "cpu.stat", NULL);
    g_assert_true(g_file_set_contents(p, body, -1, NULL));
    g_free(p);
    g_free(body);
    g_free(dir);
}

                                                             
static void
_rm_rf(const gchar *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    if (d) {
        const gchar *e;
        while ((e = g_dir_read_name(d)) != NULL) {
            gchar *child = g_build_filename(path, e, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR))
                _rm_rf(child);
            else
                g_remove(child);
            g_free(child);
        }
        g_dir_close(d);
    }
    g_rmdir(path);
}

                                                
static const PcvCgroupPsiRef *
_find_ref(const PcvCgroupPsiRef *refs, gint n, const gchar *name, PcvCgroupPsiKind kind)
{
    for (gint i = 0; i < n; i++) {
        if (refs[i].kind == kind && strcmp(refs[i].name, name) == 0)
            return &refs[i];
    }
    return NULL;
}

                                                                     

static void
test_unescape_basic(void)
{
    gchar out[128];

                                              
                                                                              
    g_assert_true(pcv_cgroup_psi_unescape("web\\x2dprod", out, sizeof(out)));
    g_assert_cmpstr(out, ==, "web-prod");

    g_assert_true(pcv_cgroup_psi_unescape("web\\x2dprod\\x2d01", out, sizeof(out)));
    g_assert_cmpstr(out, ==, "web-prod-01");

    g_assert_true(pcv_cgroup_psi_unescape("a\\x20b", out, sizeof(out)));
    g_assert_cmpstr(out, ==, "a b");

                              
    g_assert_true(pcv_cgroup_psi_unescape("plainvm", out, sizeof(out)));
    g_assert_cmpstr(out, ==, "plainvm");

                                                       
    g_assert_true(pcv_cgroup_psi_unescape("vm_1.test", out, sizeof(out)));
    g_assert_cmpstr(out, ==, "vm_1.test");
}

static void
test_unescape_rejects_unsafe(void)
{
    gchar out[128];

                                                               
                                             
    g_assert_false(pcv_cgroup_psi_unescape("bad\\x22name", out, sizeof(out)));
    g_assert_false(pcv_cgroup_psi_unescape("bad\\x5cname", out, sizeof(out)));
    g_assert_false(pcv_cgroup_psi_unescape("bad\\x0aname", out, sizeof(out)));
                               
    g_assert_false(pcv_cgroup_psi_unescape("bad\"name", out, sizeof(out)));

                                              
    g_assert_false(pcv_cgroup_psi_unescape("bad\\zz", out, sizeof(out)));
    g_assert_false(pcv_cgroup_psi_unescape("trailing\\", out, sizeof(out)));

                           
    g_assert_false(pcv_cgroup_psi_unescape("", out, sizeof(out)));

                 
    g_assert_false(pcv_cgroup_psi_unescape(NULL, out, sizeof(out)));
    g_assert_false(pcv_cgroup_psi_unescape("x", NULL, sizeof(out)));
    g_assert_false(pcv_cgroup_psi_unescape("x", out, 0));
}

static void
test_unescape_rejects_truncation(void)
{
                                                     
    gchar small[4];
    g_assert_true(pcv_cgroup_psi_unescape("abc", small, sizeof(small)));
    g_assert_cmpstr(small, ==, "abc");
    g_assert_false(pcv_cgroup_psi_unescape("abcd", small, sizeof(small)));
}

                                                                    

static void
test_machine_dir_qemu_vm(void)
{
    PcvCgroupPsiRef r;

                                                                          
    g_assert_true(pcv_cgroup_psi_ref_from_machine_dir(
        "machine-qemu\\x2d3\\x2dweb\\x2dprod.scope", &r));
    g_assert_cmpstr(r.name, ==, "web-prod");
    g_assert_cmpint(r.kind, ==, PCV_CGROUP_PSI_KIND_VM);
    g_assert_cmpstr(r.rel_path, ==,
                    "machine.slice/machine-qemu\\x2d3\\x2dweb\\x2dprod.scope");

                      
    g_assert_true(pcv_cgroup_psi_ref_from_machine_dir(
        "machine-qemu\\x2d127\\x2dalpha.scope", &r));
    g_assert_cmpstr(r.name, ==, "alpha");
    g_assert_cmpint(r.kind, ==, PCV_CGROUP_PSI_KIND_VM);

                                               
    g_assert_true(pcv_cgroup_psi_ref_from_machine_dir(
        "machine-qemu\\x2d1\\x2d01vm.scope", &r));
    g_assert_cmpstr(r.name, ==, "01vm");
}

static void
test_machine_dir_containers(void)
{
    PcvCgroupPsiRef r;

                     
    g_assert_true(pcv_cgroup_psi_ref_from_machine_dir(
        "machine-lxc\\x2d7\\x2dct\\x2done.scope", &r));
    g_assert_cmpstr(r.name, ==, "ct-one");
    g_assert_cmpint(r.kind, ==, PCV_CGROUP_PSI_KIND_CONTAINER);

                                                        
    g_assert_true(pcv_cgroup_psi_ref_from_machine_dir("machine-nspawnbox.scope", &r));
    g_assert_cmpstr(r.name, ==, "nspawnbox");
    g_assert_cmpint(r.kind, ==, PCV_CGROUP_PSI_KIND_CONTAINER);
}

static void
test_machine_dir_rejects_non_guests(void)
{
    PcvCgroupPsiRef r;

                                                
    g_assert_false(pcv_cgroup_psi_ref_from_machine_dir("cgroup.procs", &r));
    g_assert_false(pcv_cgroup_psi_ref_from_machine_dir("cpu.pressure", &r));
    g_assert_false(pcv_cgroup_psi_ref_from_machine_dir("machine-qemu\\x2d1\\x2dvm", &r));
    g_assert_false(pcv_cgroup_psi_ref_from_machine_dir("system.slice", &r));
    g_assert_false(pcv_cgroup_psi_ref_from_machine_dir("machine-.scope", &r));
    g_assert_false(pcv_cgroup_psi_ref_from_machine_dir(NULL, &r));

                                           
    g_assert_false(pcv_cgroup_psi_ref_from_machine_dir(
        "machine-qemu\\x2d1\\x2dev\\x22il.scope", &r));
}

static void
test_lxc_dir_mapping(void)
{
    PcvCgroupPsiRef r;

                                                             
    g_assert_true(pcv_cgroup_psi_ref_from_lxc_dir(NULL, "lxc.payload.ct1", &r));
    g_assert_cmpstr(r.name, ==, "ct1");
    g_assert_cmpint(r.kind, ==, PCV_CGROUP_PSI_KIND_CONTAINER);
    g_assert_cmpstr(r.rel_path, ==, "lxc.payload.ct1");

    g_assert_true(pcv_cgroup_psi_ref_from_lxc_dir("", "lxc.payload.ct1", &r));
    g_assert_cmpstr(r.name, ==, "ct1");

                                          
    g_assert_true(pcv_cgroup_psi_ref_from_lxc_dir("lxc", "ct2", &r));
    g_assert_cmpstr(r.name, ==, "ct2");
    g_assert_cmpstr(r.rel_path, ==, "lxc/ct2");

                                                          
    g_assert_false(pcv_cgroup_psi_ref_from_lxc_dir(NULL, "system.slice", &r));
    g_assert_false(pcv_cgroup_psi_ref_from_lxc_dir(NULL, "lxc.payload.", &r));
    g_assert_false(pcv_cgroup_psi_ref_from_lxc_dir("lxc", "", &r));
    g_assert_false(pcv_cgroup_psi_ref_from_lxc_dir("lxc", NULL, &r));
}

                                                                         

static void
test_psi_parse_some_and_full(void)
{
    PcvCgroupPsiSample s;

    g_assert_true(pcv_cgroup_psi_parse(
        "some avg10=1.25 avg60=0.50 avg300=0.10 total=1234567\n"
        "full avg10=0.75 avg60=0.25 avg300=0.05 total=500000\n", &s));

    g_assert_true(s.have_some);
    g_assert_true(s.have_full);
    g_assert_cmpfloat(s.some_avg10, ==, 1.25);
    g_assert_cmpfloat(s.some_avg60, ==, 0.50);
    g_assert_cmpfloat(s.some_avg300, ==, 0.10);
                                                    
    g_assert_cmpfloat(s.some_total_sec, >, 1.2345);
    g_assert_cmpfloat(s.some_total_sec, <, 1.2346);
    g_assert_cmpfloat(s.full_avg10, ==, 0.75);
    g_assert_cmpfloat(s.full_total_sec, ==, 0.5);
}

static void
test_psi_parse_some_only(void)
{
    PcvCgroupPsiSample s;

                                                                
    g_assert_true(pcv_cgroup_psi_parse(
        "some avg10=0.00 avg60=0.00 avg300=0.00 total=69592278\n", &s));
    g_assert_true(s.have_some);
    g_assert_false(s.have_full);
    g_assert_cmpfloat(s.full_avg10, ==, 0.0);                      
}

static void
test_psi_parse_tolerates_junk(void)
{
    PcvCgroupPsiSample s;

                                                      
    g_assert_true(pcv_cgroup_psi_parse(
        "weird avg10=9.99 avg60=9.99 avg300=9.99 total=9\n"
        "some avg10=1.00 avg60=1.00 avg300=1.00 total=1000000\n", &s));
    g_assert_true(s.have_some);
    g_assert_cmpfloat(s.some_avg10, ==, 1.00);

                       
    g_assert_false(pcv_cgroup_psi_parse("", &s));
    g_assert_false(pcv_cgroup_psi_parse("garbage\n", &s));
    g_assert_false(pcv_cgroup_psi_parse(NULL, &s));
}

                                                                     

static void
test_labels_and_metric_names(void)
{
    PcvCgroupPsiRef r;
    gchar buf[256];

    g_assert_true(pcv_cgroup_psi_ref_from_machine_dir(
        "machine-qemu\\x2d3\\x2dweb\\x2dprod.scope", &r));
    pcv_cgroup_psi_format_labels(&r, buf, sizeof(buf));
    g_assert_cmpstr(buf, ==, "vm_name=\"web-prod\",kind=\"vm\"");

    g_assert_true(pcv_cgroup_psi_ref_from_lxc_dir(NULL, "lxc.payload.ct1", &r));
    pcv_cgroup_psi_format_labels(&r, buf, sizeof(buf));
    g_assert_cmpstr(buf, ==, "vm_name=\"ct1\",kind=\"container\"");

    pcv_cgroup_psi_format_metric("cpu", "some", "seconds_total", buf, sizeof(buf));
    g_assert_cmpstr(buf, ==, "purecvisor_cgroup_pressure_cpu_some_seconds_total");
    pcv_cgroup_psi_format_metric("memory", "full", "avg10", buf, sizeof(buf));
    g_assert_cmpstr(buf, ==, "purecvisor_cgroup_pressure_memory_full_avg10");
}

  
                                                  
                                                                    
                                                                 
                                                        
                                             
   
static void
test_labels_are_sweepable(void)
{
    PcvCgroupPsiRef r;
    gchar labels[256];

    g_assert_true(pcv_cgroup_psi_ref_from_machine_dir(
        "machine-qemu\\x2d3\\x2dweb\\x2dprod.scope", &r));
    pcv_cgroup_psi_format_labels(&r, labels, sizeof(labels));
    g_assert_true(pcv_prom_labels_are_high_cardinality(labels));

                                                
    g_assert_true(pcv_cgroup_psi_ref_from_lxc_dir(NULL, "lxc.payload.ct1", &r));
    pcv_cgroup_psi_format_labels(&r, labels, sizeof(labels));
    g_assert_true(pcv_prom_labels_are_high_cardinality(labels));

                                                   
    g_assert_false(pcv_prom_labels_are_high_cardinality("cpu=\"0\",mode=\"idle\""));
    g_assert_false(pcv_prom_labels_are_high_cardinality(""));
    g_assert_false(pcv_prom_labels_are_high_cardinality(NULL));
}

                                                              

static void
test_scan_fixture_tree(void)
{
    gchar *root = g_dir_make_tmp("pcv-cgpsi-XXXXXX", NULL);
    g_assert_nonnull(root);

                             
    _make_guest(root, "machine.slice/machine-qemu\\x2d3\\x2dweb\\x2dprod.scope");
    _make_guest(root, "machine.slice/machine-lxc\\x2d7\\x2dct\\x2done.scope");
    _make_guest(root, "lxc.payload.ct1");
    _make_guest(root, "lxc/ct2");
                               
    _make_guest(root, "machine.slice/system-noise.slice");
    _make_guest(root, "system.slice");

    PcvCgroupPsiRef refs[PCV_CGROUP_PSI_MAX_ENTRIES];
    gboolean truncated = TRUE;                            
    gint n = pcv_cgroup_psi_scan(root, refs, PCV_CGROUP_PSI_MAX_ENTRIES, &truncated);

    g_assert_false(truncated);
    g_assert_cmpint(n, ==, 4);

    const PcvCgroupPsiRef *vm = _find_ref(refs, n, "web-prod", PCV_CGROUP_PSI_KIND_VM);
    g_assert_nonnull(vm);
    g_assert_cmpstr(vm->rel_path, ==,
                    "machine.slice/machine-qemu\\x2d3\\x2dweb\\x2dprod.scope");
    g_assert_nonnull(_find_ref(refs, n, "ct-one", PCV_CGROUP_PSI_KIND_CONTAINER));
    g_assert_nonnull(_find_ref(refs, n, "ct1", PCV_CGROUP_PSI_KIND_CONTAINER));
    g_assert_nonnull(_find_ref(refs, n, "ct2", PCV_CGROUP_PSI_KIND_CONTAINER));

                                                   
    PcvCgroupPsiSample s;
    g_assert_true(pcv_cgroup_psi_read(root, vm, "cpu", &s));
    g_assert_true(s.have_some);
    g_assert_cmpfloat(s.some_avg10, ==, 1.25);
    g_assert_true(pcv_cgroup_psi_read(root, vm, "memory", &s));
    g_assert_cmpfloat(s.full_avg10, ==, 1.50);
                        
    g_assert_false(pcv_cgroup_psi_read(root, vm, "nosuch", &s));

    _rm_rf(root);
    g_free(root);
}

static void
test_scan_respects_cap(void)
{
    gchar *root = g_dir_make_tmp("pcv-cgpsi-cap-XXXXXX", NULL);
    g_assert_nonnull(root);

    for (int i = 0; i < 5; i++) {
        gchar *rel = g_strdup_printf(
            "machine.slice/machine-qemu\\x2d%d\\x2dvm%d.scope", i, i);
        _make_guest(root, rel);
        g_free(rel);
    }

                                                    
    PcvCgroupPsiRef refs[3];
    gboolean truncated = FALSE;
    gint n = pcv_cgroup_psi_scan(root, refs, 3, &truncated);
    g_assert_cmpint(n, ==, 3);
    g_assert_true(truncated);

                                            
    truncated = TRUE;
    g_assert_cmpint(pcv_cgroup_psi_scan(NULL, refs, 3, &truncated), ==, 0);
    g_assert_false(truncated);
    g_assert_cmpint(pcv_cgroup_psi_scan(root, refs, 0, NULL), ==, 0);

    _rm_rf(root);
    g_free(root);
}

static void
test_collect_cycle_counts_samples(void)
{
    gchar *root = g_dir_make_tmp("pcv-cgpsi-col-XXXXXX", NULL);
    g_assert_nonnull(root);

    _make_guest(root, "machine.slice/machine-qemu\\x2d3\\x2dweb\\x2dprod.scope");
    _make_guest(root, "lxc.payload.ct1");

                                           
                                                     
                                                              
                                                                    
    g_assert_cmpint(pcv_cgroup_psi_collect(root, NULL), ==, 6);

                                                      
    gchar *empty = g_dir_make_tmp("pcv-cgpsi-empty-XXXXXX", NULL);
    g_assert_nonnull(empty);
    g_assert_cmpint(pcv_cgroup_psi_collect(empty, NULL), ==, 0);

    _rm_rf(empty);
    g_free(empty);
    _rm_rf(root);
    g_free(root);
}

                                                              

                                                     
                                                       
                                                    
static const char *
_psi_gate_skip_reason(void)
{
    if (!g_file_test(PCV_CGROUP_PSI_ROOT_DEFAULT "/cgroup.controllers",
                     G_FILE_TEST_EXISTS))
        return "cgroup v2 미마운트 — 실 sysfs 형식 대조 스킵";
    if (!g_file_test(PCV_CGROUP_PSI_ROOT_DEFAULT "/cpu.pressure", G_FILE_TEST_EXISTS))
        return "커널 PSI 미지원(CONFIG_PSI off 또는 <4.20) — 실 sysfs 형식 대조 스킵";
    return NULL;
}

static void
test_real_sysfs_format_gated(void)
{
    const char *skip = _psi_gate_skip_reason();
    if (skip) { g_test_skip(skip); return; }

                                               
                                                  
                                           
    PcvCgroupPsiRef root_ref = { .kind = PCV_CGROUP_PSI_KIND_VM };
    g_strlcpy(root_ref.name, "host", sizeof(root_ref.name));
    root_ref.rel_path[0] = '\0';              

    PcvCgroupPsiSample s;
    g_assert_true(pcv_cgroup_psi_read(PCV_CGROUP_PSI_ROOT_DEFAULT, &root_ref, "cpu", &s));
    g_assert_true(s.have_some);
    g_assert_cmpfloat(s.some_avg10, >=, 0.0);
    g_assert_cmpfloat(s.some_total_sec, >=, 0.0);
}

                                                                         

                                                               
static void
test_domain_id_from_scope(void)
{
                                                   
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope(
        "machine-qemu\\x2d4\\x2dspecreviewx1.scope"), ==, 4);
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope(
        "machine-qemu\\x2d12\\x2dweb\\x2d01.scope"), ==, 12);
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope(
        "machine-qemu\\x2d0\\x2da.scope"), ==, 0);

                                                                        
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope(
        "machine-lxc\\x2d7\\x2dct\\x2done.scope"), ==, -1);
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope("machine-nspawnbox.scope"), ==, -1);
                    
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope(
        "machine-qemu\\x2dx\\x2dweb.scope"), ==, -1);
                         
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope(
        "machine-qemu\\x2d12web.scope"), ==, -1);
               
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope(
        "machine-qemu\\x2d12\\x2d.scope"), ==, -1);
                   
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope(
        "machine-qemu\\x2d99999999999\\x2dvm.scope"), ==, -1);
                         
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope(
        "machine-qemu\\x2d3\\x2dvm"), ==, -1);
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope("cgroup.procs"), ==, -1);
    g_assert_cmpint(pcv_cgroup_psi_domain_id_from_scope(NULL), ==, -1);

                                       
    PcvCgroupPsiRef r;
    g_assert_true(pcv_cgroup_psi_ref_from_machine_dir(
        "machine-qemu\\x2d3\\x2dweb\\x2dprod.scope", &r));
    g_assert_cmpint(r.domain_id, ==, 3);
    g_assert_true(pcv_cgroup_psi_ref_from_machine_dir(
        "machine-lxc\\x2d7\\x2dct\\x2done.scope", &r));
    g_assert_cmpint(r.domain_id, ==, -1);
    g_assert_true(pcv_cgroup_psi_ref_from_lxc_dir(NULL, "lxc.payload.ct1", &r));
    g_assert_cmpint(r.domain_id, ==, -1);
}

                                                                  
                                                
static void
test_normalize_vm_name_underscore(void)
{
    gchar out[PCV_CGROUP_PSI_NAME_MAX];

    pcv_cgroup_psi_normalize_vm_name("spec_review_x1", out, sizeof(out));
    g_assert_cmpstr(out, ==, "specreviewx1");
    pcv_cgroup_psi_normalize_vm_name("web-01", out, sizeof(out));
    g_assert_cmpstr(out, ==, "web-01");

                                                     
    PcvCgroupPsiRef r;
    g_assert_true(pcv_cgroup_psi_ref_from_machine_dir(
        "machine-qemu\\x2d4\\x2dspecreviewx1.scope", &r));
    g_assert_cmpstr(r.name, ==, "specreviewx1");
    pcv_cgroup_psi_normalize_vm_name("spec_review_x1", out, sizeof(out));
    g_assert_cmpstr(out, ==, r.norm_name);
    g_assert_true(pcv_cgroup_psi_ref_from_lxc_dir(NULL, "lxc.payload.ct_1", &r));
    g_assert_cmpstr(r.norm_name, ==, "ct_1");                     

                                               
    pcv_cgroup_psi_normalize_vm_name(NULL, out, sizeof(out));
    g_assert_cmpstr(out, ==, "");
    gchar small[4];
    pcv_cgroup_psi_normalize_vm_name("abcdef", small, sizeof(small));
    g_assert_cmpstr(small, ==, "abc");
    pcv_cgroup_psi_normalize_vm_name("abc", small, 0);                          
}

static void
test_cpu_stat_parse(void)
{
    PcvCgroupCpuStat s;

                                                   
    g_assert_true(pcv_cgroup_cpu_stat_parse(
        "usage_usec 5000\nuser_usec 3000\nsystem_usec 2000\n"
        "nr_periods 10\nnr_throttled 2\nthrottled_usec 345\n"
        "nr_bursts 0\nburst_usec 0\n", &s));
    g_assert_true(s.have);
    g_assert_cmpuint(s.nr_periods, ==, 10);
    g_assert_cmpuint(s.nr_throttled, ==, 2);
    g_assert_cmpuint(s.throttled_usec, ==, 345);

                                                         
    g_assert_false(pcv_cgroup_cpu_stat_parse(
        "usage_usec 5\nuser_usec 3\nsystem_usec 2\n", &s));
    g_assert_false(s.have);
                                                    
    g_assert_false(pcv_cgroup_cpu_stat_parse("nr_periods 10\nnr_throttled 2\n", &s));
    g_assert_false(s.have);

                                            
    g_assert_true(pcv_cgroup_cpu_stat_parse(
        "garbage\n\nnr_periods_extra 99\nnr_periods 7\nnr_throttled x\n"
        "throttled_usec 9\nnr_throttled 1\n", &s));
    g_assert_cmpuint(s.nr_periods, ==, 7);
    g_assert_cmpuint(s.nr_throttled, ==, 1);
    g_assert_cmpuint(s.throttled_usec, ==, 9);

                    
    g_assert_false(pcv_cgroup_cpu_stat_parse("", &s));
    g_assert_false(s.have);
    g_assert_false(pcv_cgroup_cpu_stat_parse(NULL, &s));
    g_assert_false(s.have);
}

static const PcvCgroupVmStat *
_find_vm_stat(GPtrArray *arr, const gchar *name)
{
    for (guint i = 0; i < arr->len; i++) {
        const PcvCgroupVmStat *st = g_ptr_array_index(arr, i);
        if (strcmp(st->ref.name, name) == 0) return st;
    }
    return NULL;
}

                                                                            
                                
static void
test_scan_reads_cpu_stat_children(void)
{
    gchar *root = g_dir_make_tmp("pcv-cgpsi-cpu-XXXXXX", NULL);
    g_assert_nonnull(root);

    static const gchar SCOPE[] = "machine.slice/machine-qemu\\x2d9\\x2dweb\\x2d01.scope";
    _make_guest(root, SCOPE);
    _make_cpu_stat(root, SCOPE, 100, 1, 10);
    gchar *c0 = g_strdup_printf("%s/libvirt/vcpu0", SCOPE);
    gchar *c1 = g_strdup_printf("%s/libvirt/vcpu1", SCOPE);
    gchar *ce = g_strdup_printf("%s/libvirt/emulator", SCOPE);
    gchar *lv = g_strdup_printf("%s/libvirt", SCOPE);
    _make_cpu_stat(root, c0, 50, 5, 100);
    _make_cpu_stat(root, c1, 50, 6, 300);
    _make_cpu_stat(root, ce, 10, 1, 5);
                                                   
    _make_cpu_stat(root, lv, 999, 999, 99999);
                                    
    gchar *ctl = g_build_filename(root, lv, "cgroup.procs", NULL);
    g_assert_true(g_file_set_contents(ctl, "", -1, NULL));
    g_free(ctl);

                                                    
    _make_guest(root, "machine.slice/machine-qemu\\x2d10\\x2dnostat.scope");
                                       
    _make_guest(root, "lxc.payload.ct1");

    GPtrArray *arr = g_ptr_array_new_with_free_func(pcv_cgroup_vm_stat_free);
    g_assert_cmpint(pcv_cgroup_psi_collect(root, arr), ==, 9);                      
    g_assert_cmpuint(arr->len, ==, 2);

    const PcvCgroupVmStat *st = _find_vm_stat(arr, "web-01");
    g_assert_nonnull(st);
    g_assert_cmpint(st->ref.domain_id, ==, 9);
    g_assert_cmpint(st->ref.kind, ==, PCV_CGROUP_PSI_KIND_VM);
    g_assert_true(st->sum.have);
    g_assert_cmpuint(st->sum.throttled_usec, ==, 415);                           
    g_assert_cmpuint(st->sum.nr_periods, ==, 210);                               
    g_assert_cmpuint(st->sum.nr_throttled, ==, 13);                         
    g_assert_nonnull(st->children);
    g_assert_cmpuint(st->children->len, ==, 3);
                  
    g_assert_cmpstr(g_array_index(st->children, PcvCgroupCpuChild, 0).name, ==, "emulator");
    g_assert_cmpstr(g_array_index(st->children, PcvCgroupCpuChild, 1).name, ==, "vcpu0");
    g_assert_cmpstr(g_array_index(st->children, PcvCgroupCpuChild, 2).name, ==, "vcpu1");
    g_assert_cmpuint(g_array_index(st->children, PcvCgroupCpuChild, 0).throttled_usec, ==, 5);
    g_assert_cmpuint(g_array_index(st->children, PcvCgroupCpuChild, 1).throttled_usec, ==, 100);
    g_assert_cmpuint(g_array_index(st->children, PcvCgroupCpuChild, 2).throttled_usec, ==, 300);
                                                    
    for (gint r = 0; r < PCV_CGROUP_PSI_RES_COUNT; r++) g_assert_true(st->have_psi[r]);
    g_assert_cmpfloat(st->psi[PCV_CGROUP_PSI_RES_CPU].some_avg10, ==, 1.25);
    g_assert_cmpfloat(st->psi[PCV_CGROUP_PSI_RES_MEMORY].full_avg10, ==, 1.50);
    g_assert_cmpfloat(st->psi[PCV_CGROUP_PSI_RES_IO].full_avg10, ==, 1.00);

    const PcvCgroupVmStat *ns = _find_vm_stat(arr, "nostat");
    g_assert_nonnull(ns);
    g_assert_cmpint(ns->ref.domain_id, ==, 10);
    g_assert_false(ns->sum.have);
    g_assert_nonnull(ns->children);
    g_assert_cmpuint(ns->children->len, ==, 0);

    g_ptr_array_unref(arr);
    g_free(c0); g_free(c1); g_free(ce); g_free(lv);
    _rm_rf(root);
    g_free(root);
}

                                                                   
static void
test_cap_equals_diag_cap(void)
{
    g_assert_cmpint(PCV_CGROUP_PSI_MAX_ENTRIES, ==, 256);

    gchar *root = g_dir_make_tmp("pcv-cgpsi-cap256-XXXXXX", NULL);
    g_assert_nonnull(root);
    for (int i = 0; i < 270; i++) {
        gchar *rel = g_strdup_printf(
            "machine.slice/machine-qemu\\x2d%d\\x2dvm%03d.scope", i + 1, i);
        _make_guest(root, rel);
        g_free(rel);
    }

    PcvCgroupPsiRef *refs = g_new0(PcvCgroupPsiRef, PCV_CGROUP_PSI_MAX_ENTRIES);
    gboolean truncated = FALSE;
    g_assert_cmpint(pcv_cgroup_psi_scan(root, refs, PCV_CGROUP_PSI_MAX_ENTRIES, &truncated),
                    ==, 256);
    g_assert_true(truncated);
    g_free(refs);

                                                   
    GPtrArray *arr = g_ptr_array_new_with_free_func(pcv_cgroup_vm_stat_free);
    pcv_cgroup_psi_collect(root, arr);
    g_assert_cmpuint(arr->len, ==, 256);
    g_ptr_array_unref(arr);

    _rm_rf(root);
    g_free(root);
}

                                                                  
                                 
static void
test_prom_export_selection(void)
{
    enum { NV = 70, NC = 70, N = NV + NC };
    PcvCgroupPsiRef *refs = g_new0(PcvCgroupPsiRef, N);
                                             
    for (gint i = 0; i < NV; i++) {
        gint k = (i * 37) % NV;
        PcvCgroupPsiRef *r = &refs[2 * i];
        g_snprintf(r->name, sizeof(r->name), "vm%03d", k);
        r->kind = PCV_CGROUP_PSI_KIND_VM;
        PcvCgroupPsiRef *c = &refs[2 * i + 1];
        g_snprintf(c->name, sizeof(c->name), "ct%03d", (k * 3 + 1) % NC);
        c->kind = PCV_CGROUP_PSI_KIND_CONTAINER;
    }

    gboolean *sel = g_new0(gboolean, N);
    g_assert_true(pcv_cgroup_psi_prom_selected(refs, N, PCV_PROM_VM_SERIES_MAX,
                                               PCV_CGROUP_PSI_PROM_CONTAINER_MAX, sel));
    gint nv = 0, nc = 0;
    for (gint i = 0; i < N; i++) {
        gint num = atoi(refs[i].name + 2);
        gboolean want = num < 64;
        g_assert_cmpint(sel[i], ==, want);
        if (sel[i]) {
            if (refs[i].kind == PCV_CGROUP_PSI_KIND_VM) nv++; else nc++;
        }
    }
    g_assert_cmpint(nv, ==, PCV_PROM_VM_SERIES_MAX);
    g_assert_cmpint(nc, ==, PCV_CGROUP_PSI_PROM_CONTAINER_MAX);

                             
    g_assert_false(pcv_cgroup_psi_prom_selected(refs, 3, 64, 64, sel));
    for (gint i = 0; i < 3; i++) g_assert_true(sel[i]);
                                   
    g_assert_true(pcv_cgroup_psi_prom_selected(refs, 4, 0, 64, sel));
    for (gint i = 0; i < 4; i++)
        g_assert_cmpint(sel[i], ==, refs[i].kind == PCV_CGROUP_PSI_KIND_CONTAINER);

    g_free(sel);
    g_free(refs);
}

                                                                           

void
test_cgroup_psi_register(void)
{
    g_test_add_func("/cgroup_psi/unescape_basic", test_unescape_basic);
    g_test_add_func("/cgroup_psi/unescape_rejects_unsafe", test_unescape_rejects_unsafe);
    g_test_add_func("/cgroup_psi/unescape_rejects_truncation", test_unescape_rejects_truncation);
    g_test_add_func("/cgroup_psi/machine_dir_qemu_vm", test_machine_dir_qemu_vm);
    g_test_add_func("/cgroup_psi/machine_dir_containers", test_machine_dir_containers);
    g_test_add_func("/cgroup_psi/machine_dir_rejects_non_guests", test_machine_dir_rejects_non_guests);
    g_test_add_func("/cgroup_psi/lxc_dir_mapping", test_lxc_dir_mapping);
    g_test_add_func("/cgroup_psi/psi_parse_some_and_full", test_psi_parse_some_and_full);
    g_test_add_func("/cgroup_psi/psi_parse_some_only", test_psi_parse_some_only);
    g_test_add_func("/cgroup_psi/psi_parse_tolerates_junk", test_psi_parse_tolerates_junk);
    g_test_add_func("/cgroup_psi/labels_and_metric_names", test_labels_and_metric_names);
    g_test_add_func("/cgroup_psi/labels_are_sweepable", test_labels_are_sweepable);
    g_test_add_func("/cgroup_psi/scan_fixture_tree", test_scan_fixture_tree);
    g_test_add_func("/cgroup_psi/scan_respects_cap", test_scan_respects_cap);
    g_test_add_func("/cgroup_psi/collect_cycle_counts_samples", test_collect_cycle_counts_samples);
    g_test_add_func("/cgroup_psi/real_sysfs_format_gated", test_real_sysfs_format_gated);
                                
    g_test_add_func("/cgroup_psi/domain_id_from_scope", test_domain_id_from_scope);
    g_test_add_func("/cgroup_psi/normalize_vm_name_underscore", test_normalize_vm_name_underscore);
    g_test_add_func("/cgroup_psi/cpu_stat_parse", test_cpu_stat_parse);
    g_test_add_func("/cgroup_psi/scan_reads_cpu_stat_children", test_scan_reads_cpu_stat_children);
    g_test_add_func("/cgroup_psi/cap_equals_diag_cap", test_cap_equals_diag_cap);
    g_test_add_func("/cgroup_psi/prom_export_selection", test_prom_export_selection);
}
