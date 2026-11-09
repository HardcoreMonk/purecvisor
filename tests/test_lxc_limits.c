                                                                                            
                                                                                                  
                                                                                                   
                                                              
                                                                                      
                                                                                                               
                                                                                                
                                                                                             
                                                           
                                                                
                                                                               
                                                                                 
  
                                   
   
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modules/lxc/lxc_limits.h"

                                                        
static void
_rm_tree(const gchar *path)
{
    GStatBuf st;
    if (g_lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        g_chmod(path, 0700);
        GDir *d = g_dir_open(path, 0, NULL);
        const gchar *e;
        while (d && (e = g_dir_read_name(d))) {
            g_autofree gchar *child = g_build_filename(path, e, NULL);
            _rm_tree(child);
        }
        if (d)
            g_dir_close(d);
        g_rmdir(path);
    } else {
        g_unlink(path);
    }
}

                                                 
static gchar *
_tmp_dir(void)
{
    GError *e = NULL;
    gchar *d = g_dir_make_tmp("pcv-lxc-limits-XXXXXX", &e);
    g_assert_no_error(e);
    g_assert_nonnull(d);
    return d;
}

                                  
static gchar *
_slurp(const gchar *path)
{
    gchar *t = NULL;
    return g_file_get_contents(path, &t, NULL, NULL) ? t : NULL;
}

static void
test_cgroup_write_without_temp_file(void)
{
    g_autofree gchar *dir = _tmp_dir();
    g_autofree gchar *file = g_build_filename(dir, "cpu.max", NULL);
    g_assert_true(g_file_set_contents(file, "max 100000", -1, NULL));
    g_assert_cmpint(g_chmod(file, 0644), ==, 0);
    g_assert_cmpint(g_chmod(dir, 0555), ==, 0);                                                    

    GError *err = NULL;
    g_assert_true(pcv_lxc_cgroup_write(file, "200000 100000", &err));
    g_assert_no_error(err);
    g_autofree gchar *got = _slurp(file);
    g_assert_cmpstr(got, ==, "200000 100000");

    if (geteuid() == 0) {
        g_test_skip("root는 CAP_DAC_OVERRIDE로 임시 파일을 만들 수 있어 반사실을 보일 수 없다");
    } else {
                                                     
        g_assert_false(g_file_set_contents(file, "300000 100000", -1, NULL));
    }
    _rm_tree(dir);
}

static void
test_cgroup_write_missing_is_error(void)
{
    g_autofree gchar *dir = _tmp_dir();
    g_autofree gchar *file = g_build_filename(dir, "nope", "cpu.max", NULL);
    GError *err = NULL;
    g_assert_false(pcv_lxc_cgroup_write(file, "1 100000", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_assert_nonnull(strstr(err->message, file));
    g_clear_error(&err);
    _rm_tree(dir);
}

static void
test_cpu_max_rule(void)
{
    struct { const gchar *cpu, *shares, *want; } t[] = {
        { "150000 100000", "4096", "150000 100000" },                          
        { NULL, "4096", "400000 100000" },                                     
        { NULL, "512", "100000 100000" },                           
        { "max 100000", NULL, "max 100000" },
        { "bad", "2048", "200000 100000" },                                 
        { NULL, "0", NULL },
        { NULL, NULL, NULL },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(t); i++) {
        g_autofree gchar *got = pcv_lxc_limits_cpu_max(t[i].cpu, t[i].shares);
        g_assert_cmpstr(got, ==, t[i].want);
    }
}

static void
test_memory_bytes_units(void)
{
    guint64 b = 0;
    g_assert_true(pcv_lxc_limits_memory_bytes("512M", &b));
    g_assert_cmpuint(b, ==, 536870912);
    g_assert_true(pcv_lxc_limits_memory_bytes("536870912", &b));
    g_assert_cmpuint(b, ==, 536870912);
    g_assert_true(pcv_lxc_limits_memory_bytes("1G", &b));
    g_assert_cmpuint(b, ==, 1073741824);
    g_assert_true(pcv_lxc_limits_memory_bytes("64K", &b));
    g_assert_cmpuint(b, ==, 65536);
    for (const gchar *const *bad = (const gchar *const[]){ "max", "12x", "", NULL }; *bad; bad++)
        g_assert_false(pcv_lxc_limits_memory_bytes(*bad, &b));
}

                                                                                                    
static gchar *
_fixture(const gchar *config_text, gchar **cg_root, gboolean with_cpu_max)
{
    gchar *base = _tmp_dir();
    g_autofree gchar *cdir = g_build_filename(base, "lxc", "c1", NULL);
    *cg_root = g_build_filename(base, "cg", NULL);
    g_autofree gchar *pay = g_build_filename(*cg_root, "lxc.payload.c1", NULL);
    g_assert_cmpint(g_mkdir_with_parents(cdir, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(pay, 0700), ==, 0);
    g_autofree gchar *cfg = g_build_filename(cdir, "config", NULL);
    g_assert_true(g_file_set_contents(cfg, config_text, -1, NULL));
    g_autofree gchar *mem = g_build_filename(pay, "memory.max", NULL);
    g_assert_true(g_file_set_contents(mem, "max", -1, NULL));
    if (with_cpu_max) {
        g_autofree gchar *cpu = g_build_filename(pay, "cpu.max", NULL);
        g_assert_true(g_file_set_contents(cpu, "max 100000", -1, NULL));
    }
    return base;
}

static void
test_reapply_shares_not_clamped(void)
{
    g_autofree gchar *cg = NULL;
    g_autofree gchar *base = _fixture("lxc.cgroup.cpu.shares = 4096\nlxc.cgroup2.memory.max = 512M\n", &cg, TRUE);
    g_autofree gchar *lxc = g_build_filename(base, "lxc", NULL);
    GError *err = NULL;
    g_assert_true(pcv_lxc_limits_reapply(lxc, "c1", cg, &err));
    g_assert_no_error(err);
    g_autofree gchar *cpu = g_build_filename(cg, "lxc.payload.c1", "cpu.max", NULL);
    g_autofree gchar *mem = g_build_filename(cg, "lxc.payload.c1", "memory.max", NULL);
    g_autofree gchar *c = _slurp(cpu), *m = _slurp(mem);
    g_assert_cmpstr(c, ==, "400000 100000");
    g_assert_cmpstr(m, ==, "536870912");
    _rm_tree(base);
}

static void
test_reapply_prefers_config_cpu_max(void)
{
    g_autofree gchar *cg = NULL;
    g_autofree gchar *base = _fixture("lxc.cgroup.cpu.shares = 4096\nlxc.cgroup2.cpu.max = 150000 100000\n", &cg, TRUE);
    g_autofree gchar *lxc = g_build_filename(base, "lxc", NULL);
    GError *err = NULL;
    g_assert_true(pcv_lxc_limits_reapply(lxc, "c1", cg, &err));
    g_assert_no_error(err);
    g_autofree gchar *cpu = g_build_filename(cg, "lxc.payload.c1", "cpu.max", NULL);
    g_autofree gchar *c = _slurp(cpu);
    g_assert_cmpstr(c, ==, "150000 100000");
    _rm_tree(base);
}

static void
test_reapply_write_failure_is_reported(void)
{
    g_autofree gchar *cg = NULL;
    g_autofree gchar *base = _fixture("lxc.cgroup2.cpu.max = 150000 100000\n", &cg, FALSE);                   
    g_autofree gchar *lxc = g_build_filename(base, "lxc", NULL);
    g_autofree gchar *cpu = g_build_filename(cg, "lxc.payload.c1", "cpu.max", NULL);
    GError *err = NULL;
    g_assert_false(pcv_lxc_limits_reapply(lxc, "c1", cg, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, cpu));
    g_clear_error(&err);
    _rm_tree(base);
}

                                                                                              
                                             
static void
test_cgroup_read_payload_path(void)
{
    g_autofree gchar *base = _tmp_dir();
    g_autofree gchar *pay = g_build_filename(base, "lxc.payload.c1", NULL);
    g_autofree gchar *old = g_build_filename(base, "lxc", "c2", NULL);
    g_assert_cmpint(g_mkdir_with_parents(pay, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(old, 0700), ==, 0);
    g_autofree gchar *f1 = g_build_filename(pay, "memory.current", NULL);
    g_autofree gchar *f2 = g_build_filename(old, "memory.current", NULL);
    g_assert_true(g_file_set_contents(f1, "4411392\n", -1, NULL));
    g_assert_true(g_file_set_contents(f2, "999\n", -1, NULL));
    g_assert_cmpuint(pcv_lxc_cgroup_read_u64(base, "c1", "memory.current"), ==, 4411392);
    g_assert_cmpuint(pcv_lxc_cgroup_read_u64(base, "c2", "memory.current"), ==, 0);                     
    g_assert_cmpuint(pcv_lxc_cgroup_read_u64(base, "c1", "memory.max"), ==, 0);                  
    _rm_tree(base);
}

                                                                          
static void
test_cgroup_cpu_usage_ns(void)
{
    g_autofree gchar *base = _tmp_dir();
    g_autofree gchar *pay = g_build_filename(base, "lxc.payload.c1", NULL);
    g_autofree gchar *pay2 = g_build_filename(base, "lxc.payload.c2", NULL);
    g_assert_cmpint(g_mkdir_with_parents(pay, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(pay2, 0700), ==, 0);
    g_autofree gchar *f1 = g_build_filename(pay, "cpu.stat", NULL);
    g_autofree gchar *f2 = g_build_filename(pay2, "cpu.stat", NULL);
    g_assert_true(g_file_set_contents(f1, "usage_usec 61104\nuser_usec 40000\nsystem_usec 21104\n", -1, NULL));
    g_assert_true(g_file_set_contents(f2, "user_usec 40000\nsystem_usec 21104\n", -1, NULL));
    g_assert_cmpuint(pcv_lxc_cgroup_cpu_usage_ns(base, "c1"), ==, 61104000);
    g_assert_cmpuint(pcv_lxc_cgroup_cpu_usage_ns(base, "c2"), ==, 0);                        
    g_assert_cmpuint(pcv_lxc_cgroup_cpu_usage_ns(base, "c3"), ==, 0);              
    _rm_tree(base);
}

                                                                                                           
                                                                                                     
static void
test_cgroup_cpu_time_ns(void)
{
    g_autofree gchar *base = _tmp_dir();
    g_autofree gchar *v1 = g_build_filename(base, "lxc.payload.pcvt-v1", NULL);
    g_autofree gchar *v2 = g_build_filename(base, "lxc.payload.pcvt-v2", NULL);
    g_assert_cmpint(g_mkdir_with_parents(v1, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(v2, 0700), ==, 0);
    g_autofree gchar *acct = g_build_filename(v1, "cpuacct.usage", NULL);
    g_autofree gchar *stat1 = g_build_filename(v1, "cpu.stat", NULL);
    g_autofree gchar *stat2 = g_build_filename(v2, "cpu.stat", NULL);
    g_assert_true(g_file_set_contents(acct, "7000\n", -1, NULL));
    g_assert_true(g_file_set_contents(stat1, "usage_usec 99\n", -1, NULL));                         
    g_assert_true(g_file_set_contents(stat2, "usage_usec 1929288\nuser_usec 1\n", -1, NULL));
    g_assert_cmpuint(pcv_lxc_cgroup_cpu_time_ns(base, "pcvt-v1"), ==, 7000);
    g_assert_cmpuint(pcv_lxc_cgroup_cpu_time_ns(base, "pcvt-v2"), ==, G_GUINT64_CONSTANT(1929288000));
    g_assert_cmpuint(pcv_lxc_cgroup_cpu_time_ns(base, "pcvt-none"), ==, 0);
    _rm_tree(base);
}

                                                                                                           
                                                                                  
static void
test_cpu_percent_between(void)
{
    const gint64 S = G_GINT64_CONSTANT(1000000000);               
    const PcvLxcCpuSample p = { 4 * (guint64)S, 10 * S, 4242 };
    const struct { PcvLxcCpuSample cur; gdouble want; const gchar *why; } rows[] = {
        { { 4 * (guint64)S + (guint64)S / 2, 11 * S, 4242 }, 50.0,  "0.5초 CPU / 1초 = 반 코어" },
        { { 5 * (guint64)S, 11 * S, 4242 },                  100.0, "한 코어를 다 씀 = 100" },
        { { 7 * (guint64)S, 11 * S, 4242 },                  300.0, "세 코어 = 300(100을 넘는다 — set_limits cpu_percent와 같은 눈금)" },
        { { 4 * (guint64)S, 12 * S, 4242 },                  0.0,   "쉬는 컨테이너 = 0" },
        { { 3 * (guint64)S, 11 * S, 4242 },                  0.0,   "카운터 리셋(지금 < 직전 — 재시작) = 0" },
        { { 5 * (guint64)S, 10 * S, 4242 },                  0.0,   "Δt 0 = 0" },
        { { 5 * (guint64)S, 9 * S, 4242 },                   0.0,   "Δt < 0 = 0" },
        { { 5 * (guint64)S, 11 * S, 5151 },                  0.0,   "init pid가 다름(빠른 재시작 — 새 카운터가 옛 값을 넘었다) = 0" },
        { { 5 * (guint64)S, 11 * S, -1 },                    0.0,   "init 없음(-1)도 다른 pid = 0" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(rows); i++) {
        g_test_message("row %" G_GSIZE_FORMAT ": %s", i, rows[i].why);
        g_assert_cmpfloat_with_epsilon(pcv_lxc_cpu_percent_between(&p, &rows[i].cur), rows[i].want, 1e-9);
    }
    const PcvLxcCpuSample cur = { 5 * (guint64)S, 11 * S, 4242 };
    g_assert_cmpfloat(pcv_lxc_cpu_percent_between(NULL, &cur), ==, 0.0);             
    g_assert_cmpfloat(pcv_lxc_cpu_percent_between(&p, NULL), ==, 0.0);
}

                                                                                          
                                                         
                                                                                               
static void
test_cpu_sample_record_table(void)
{
    const gint64 S = G_GINT64_CONSTANT(1000000000);
    const guint64 U = (guint64)S;
    pcv_lxc_cpu_sample_forget("pcvt-a");
    pcv_lxc_cpu_sample_forget("pcvt-b");
    g_assert_cmpfloat(pcv_lxc_cpu_sample_record("pcvt-a", 100, 10 * U, 100 * S), ==, 0.0);                              
    g_assert_cmpfloat_with_epsilon(pcv_lxc_cpu_sample_record("pcvt-a", 100, 11 * U, 102 * S), 50.0, 1e-9);                 
    g_assert_cmpfloat(pcv_lxc_cpu_sample_record("pcvt-b", 200, 30 * U, 102 * S), ==, 0.0);                                          
    g_assert_cmpfloat_with_epsilon(pcv_lxc_cpu_sample_record("pcvt-a", 100, 13 * U, 103 * S), 200.0, 1e-9);                       
    g_assert_cmpfloat(pcv_lxc_cpu_sample_record("pcvt-a", 100, 1 * U, 104 * S), ==, 0.0);                                 
    g_assert_cmpfloat_with_epsilon(pcv_lxc_cpu_sample_record("pcvt-a", 100, 2 * U, 105 * S), 100.0, 1e-9);                    
    g_assert_cmpfloat(pcv_lxc_cpu_sample_record("pcvt-a", 101, 9 * U, 108 * S), ==, 0.0);                                                   
    g_assert_cmpfloat_with_epsilon(pcv_lxc_cpu_sample_record("pcvt-a", 101, 10 * U, 110 * S), 50.0, 1e-9);                       
    g_assert_cmpfloat(pcv_lxc_cpu_sample_record("pcvt-a", 101, 0, 111 * S), ==, 0.0);                                            
    g_assert_cmpfloat(pcv_lxc_cpu_sample_record("pcvt-a", 101, 50 * U, 112 * S), ==, 0.0);                                     
    g_assert_cmpfloat_with_epsilon(pcv_lxc_cpu_sample_record("pcvt-b", 200, 31 * U, 104 * S), 50.0, 1e-9);                       
    pcv_lxc_cpu_sample_forget("pcvt-b");
    g_assert_cmpfloat(pcv_lxc_cpu_sample_record("pcvt-b", 200, 40 * U, 105 * S), ==, 0.0);                                       
    pcv_lxc_cpu_sample_forget("pcvt-missing");                                                                               
    g_assert_cmpfloat(pcv_lxc_cpu_sample_record(NULL, 1, 1 * U, 1 * S), ==, 0.0);
    pcv_lxc_cpu_sample_forget(NULL);
    pcv_lxc_cpu_sample_forget("pcvt-a");
    pcv_lxc_cpu_sample_forget("pcvt-b");
}

                                                                              
void test_lxc_limits_register(void);
void
test_lxc_limits_register(void)
{
    g_test_add_func("/lxc_limits/cgroup_write_without_temp_file", test_cgroup_write_without_temp_file);
    g_test_add_func("/lxc_limits/cgroup_write_missing_is_error", test_cgroup_write_missing_is_error);
    g_test_add_func("/lxc_limits/cpu_max_rule", test_cpu_max_rule);
    g_test_add_func("/lxc_limits/memory_bytes_units", test_memory_bytes_units);
    g_test_add_func("/lxc_limits/reapply_shares_not_clamped", test_reapply_shares_not_clamped);
    g_test_add_func("/lxc_limits/reapply_prefers_config_cpu_max", test_reapply_prefers_config_cpu_max);
    g_test_add_func("/lxc_limits/reapply_write_failure_is_reported", test_reapply_write_failure_is_reported);
    g_test_add_func("/lxc_limits/cgroup_read_payload_path", test_cgroup_read_payload_path);
    g_test_add_func("/lxc_limits/cgroup_cpu_usage_ns", test_cgroup_cpu_usage_ns);
    g_test_add_func("/lxc_limits/cgroup_cpu_time_ns", test_cgroup_cpu_time_ns);
    g_test_add_func("/lxc_limits/cpu_percent_between", test_cpu_percent_between);
    g_test_add_func("/lxc_limits/cpu_sample_record_table", test_cpu_sample_record_table);
}
