                                                                                                                  
                                                                                                          
                                                                                                 
                                                                                                 
                                                                                                                    
                                                                                              
                                                                                                          
                                                                                                           
                                                                                                            
                                                                                             
                                                                                                                            
                                                                                                       
                                                                                             
                                                                                                                
                                      
  
                                                                                
                                                                              
                                                                      
#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <ftw.h>
#include <sys/mman.h>                                                           
#include <sys/stat.h>
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>                                                                                    
#else
#define RUNNING_ON_VALGRIND 0
#endif
#include "modules/lxc/lxc_volume_detach.h"
#include "modules/lxc/lxc_limits.h"                                                                     
#include "modules/lxc/lxc_volume_mount.h"
#include "modules/lxc/lxc_volume_guard.h"                                                            

#define VS_ROOT "/vstage"                                                              
#define CG_ROOT "/cg"
#define CG      CG_ROOT "/lxc.payload.c1"
#define INIT    100

                                       
#define CLEAN "20 1 0:40 / / rw,relatime - zfs pool/c1 rw\n" \
              "21 20 0:5 / /proc rw,nosuid - proc proc rw\n" \
              "22 20 0:41 / /tmp rw - tmpfs tmpfs rw\n"
                                                
#define COPY_AT_A "90 20 0:50 /vol /mnt/a rw,relatime - zfs pool/v rw\n"

typedef struct {
    GHashTable     *text;                 
    GHashTable     *dirs;                                    
    GHashTable     *err;                                    
    GPtrArray      *log;                                    
    PcvLxcVerifyOps ops;
    PcvLxcMountOps  mops;                                       
    guint           source_opens;
    int             fd_probe;
    guint           calls;                                                
    gboolean        fake_clock;                                                             
    gint64          clock_base, clock_step, clock_extra;
    gint64          clock_div;                                                                   
    const gchar    *slow_path;                                               
} VFake;

static VFake V;

                                                                                        
                                                                                           
                                                                                
static gint64
vf_now_us(void)
{
    if (V.fake_clock)
        return V.clock_base + (gint64)V.calls * V.clock_step + V.clock_extra;
    return V.clock_div > 1 ? g_get_monotonic_time() / V.clock_div : g_get_monotonic_time();
}

static gchar *
vf_read_text(const gchar *path, gint64 deadline_us, gint *errno_out)
{
    V.calls++;
    g_ptr_array_add(V.log, g_strdup_printf("read %s", path));
    if (V.slow_path && g_str_equal(path, V.slow_path)) {
        V.clock_extra += PCV_LXC_VERIFY_BUDGET_US;                                              
        if (vf_now_us() >= deadline_us) {
            *errno_out = ETIMEDOUT;
            return NULL;
        }
    }
    gpointer e;
    if (g_hash_table_lookup_extended(V.err, path, NULL, &e)) {
        *errno_out = GPOINTER_TO_INT(e);
        return NULL;
    }
    const gchar *t = g_hash_table_lookup(V.text, path);
    if (!t) {
        *errno_out = ENOENT;                                       
        return NULL;
    }
    return g_strdup(t);
}

static GPtrArray *
vf_list_dirs(const gchar *path, gint64 deadline_us, gint *errno_out)
{
    (void)deadline_us;
    V.calls++;
    g_ptr_array_add(V.log, g_strdup_printf("list %s", path));
    gpointer e;
    if (g_hash_table_lookup_extended(V.err, path, NULL, &e)) {
        *errno_out = GPOINTER_TO_INT(e);
        return NULL;
    }
    GPtrArray *names = g_hash_table_lookup(V.dirs, path);
    if (!names) {
        *errno_out = ENOENT;
        return NULL;
    }
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < names->len; i++)
        g_ptr_array_add(out, g_strdup(names->pdata[i]));
    return out;
}

static int
vf_source_open(const gchar *host_path, gchar **canon_out, GError **error)
{
    (void)host_path; (void)canon_out;
    V.source_opens++;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "fake: no host source");
    return -1;
}

static void
vf_begin(void)
{
    memset(&V, 0, sizeof V);
    V.fd_probe = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(V.fd_probe, >=, 0);
    close(V.fd_probe);
    V.text = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    V.dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    V.err = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    V.log = g_ptr_array_new_with_free_func(g_free);
    V.ops.read_text = vf_read_text;
    V.ops.list_dirs = vf_list_dirs;
    V.ops.now_us = vf_now_us;
    pcv_lxc_verify_set_ops_for_test(&V.ops);
    V.mops = *pcv_lxc_volume_real_ops_for_test();
    V.mops.source_open = vf_source_open;
    pcv_lxc_volume_set_ops_for_test(&V.mops);
    pcv_lxc_volume_set_roots_for_test(VS_ROOT, "/rt-unused");
}

static void
vf_end(void)
{
    pcv_lxc_verify_set_ops_for_test(NULL);
    pcv_lxc_volume_set_ops_for_test(NULL);
    pcv_lxc_volume_set_roots_for_test(NULL, NULL);
    int probe = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(probe, ==, V.fd_probe);                      
    close(probe);
    g_hash_table_unref(V.text);
    g_hash_table_unref(V.dirs);
    g_hash_table_unref(V.err);
    g_ptr_array_unref(V.log);
}

static void
vf_text(const gchar *path, const gchar *body)
{
    g_hash_table_replace(V.text, g_strdup(path), g_strdup(body));
}

static void
vf_err(const gchar *path, gint e)
{
    g_hash_table_replace(V.err, g_strdup(path), GINT_TO_POINTER(e));
}

                                                        
static void
vf_dirs(const gchar *path, const gchar *names)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_auto(GStrv) parts = g_strsplit(names, " ", -1);
    for (gchar **p = parts; *p; p++)
        if (**p)
            g_ptr_array_add(a, g_strdup(*p));
    g_hash_table_replace(V.dirs, g_strdup(path), a);
}

                                   
static void
vf_thread(guint pid, guint tid, const gchar *body)
{
    g_autofree gchar *p = g_strdup_printf("/proc/%u/task/%u/mountinfo", pid, tid);
    vf_text(p, body);
}

                                                                                   
static gchar *
vf_slot_line(guint id, const gchar *dest, guint ord, const gchar *dev, const gchar *root)
{
    g_autofree gchar *slot = pcv_lxc_volstage_slot(VS_ROOT, "c1", dest, ord);
    return g_strdup_printf("%u 30 %s %s %s rw,relatime master:9 - zfs pool/v rw\n", id, dev, root, slot);
}

                                                                                                            
static void
vf_std_at(const gchar *cg)
{
    g_autofree gchar *procs = g_strdup_printf("%s/cgroup.procs", cg), *sub = g_strdup_printf("%s/sub", cg);
    g_autofree gchar *sub_procs = g_strdup_printf("%s/sub/cgroup.procs", cg);
    vf_text(procs, "100\n200\n");
    vf_dirs(cg, "sub");
    vf_text(sub_procs, "300\n");
    vf_dirs(sub, "");
    vf_text("/proc/100/mountinfo", CLEAN);
    vf_dirs("/proc/100/task", "100");
    vf_thread(100, 100, CLEAN);
    vf_dirs("/proc/200/task", "200 201");
    vf_thread(200, 200, CLEAN);
    vf_thread(200, 201, CLEAN);
    vf_dirs("/proc/300/task", "300");
    vf_thread(300, 300, CLEAN);
}

                                                     
static void
vf_std(void)
{
    vf_std_at(CG);
}

                                                                       
static PcvLxcVolumeSet *
vf_set_a(void)
{
    g_autofree gchar *slot = vf_slot_line(61, "mnt/a", 0, "0:50", "/vol");
    g_autofree gchar *host = g_strconcat("1 0 8:1 / / rw - ext4 /dev/sda1 rw\n", slot, NULL);
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load("lxc.mount.entry = /data mnt/a none bind,create=dir 0 0\n", "c1", host);
    g_assert_nonnull(set);
    return set;
}

static gboolean
vf_check(const PcvLxcVolumeSet *set, const gchar *dest, GError **err)
{
    return pcv_lxc_volume_verify_gone(CG_ROOT, "c1", INIT, set, dest, err);
}

static gboolean
vf_logged(const gchar *line)
{
    for (guint i = 0; i < V.log->len; i++)
        if (g_str_equal(g_ptr_array_index(V.log, i), line))
            return TRUE;
    return FALSE;
}

static void
vf_assert_still(GError *err, const gchar *container_path)
{
    g_autofree gchar *want = g_strdup_printf("volume is still mounted inside the container: %s; stop the container, then detach again",
                                             container_path);
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT);
    g_assert_cmpstr(err->message, ==, want);
}

static void
vf_assert_cannot(GError *err, const gchar *why)
{
    g_autofree gchar *want = g_strdup_printf("cannot verify that the volume is gone from the container: %s; stop the container, then "
                                             "detach again", why);
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT);
    g_assert_cmpstr(err->message, ==, want);
}

                                                                                           
static void
test_verify_clean_passes(void)
{
    vf_begin();
    vf_std();
    vf_thread(200, 201, CLEAN "91 20 0:50 /other /srv/other rw - zfs pool/v rw\n"
                              "92 20 0:77 /vol /mnt/zzz rw - zfs pool/w rw\n");
    PcvLxcVolumeSet *set = vf_set_a();
    GError *err = NULL;
    g_assert_true(vf_check(set, "mnt/a", &err));
    g_assert_no_error(err);
    g_assert_cmpuint(V.source_opens, ==, 0);                                        
    const gchar *seen[] = { "read /proc/100/mountinfo", "read " CG "/cgroup.procs", "list " CG, "list /proc/100/task",
                            "read /proc/100/task/100/mountinfo", "list /proc/200/task", "read /proc/200/task/200/mountinfo",
                            "read /proc/200/task/201/mountinfo", "read " CG "/sub/cgroup.procs", "list " CG "/sub",
                            "list /proc/300/task", "read /proc/300/task/300/mountinfo" };
    for (guint i = 0; i < G_N_ELEMENTS(seen); i++)
        g_assert_true(vf_logged(seen[i]));
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                                      
                                                                                  
static void
test_verify_other_pid_ns_copy_fails(void)
{
    vf_begin();
    vf_std();
    vf_thread(200, 200, CLEAN COPY_AT_A);
    PcvLxcVolumeSet *set = vf_set_a();
    GError *err = NULL;
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                            
static void
test_verify_other_thread_ns_copy_fails(void)
{
    vf_begin();
    vf_std();
    vf_thread(200, 201, CLEAN "93 20 0:50 /vol/sub /srv/rebind rw - zfs pool/v rw\n");                                
    PcvLxcVolumeSet *set = vf_set_a();
    GError *err = NULL;
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                             
static void
test_verify_rename_mount_at_path_fails(void)
{
    vf_begin();
    vf_std();
    vf_text("/proc/100/mountinfo", CLEAN "94 20 0:77 /renamed /mnt/a rw - zfs pool/w rw\n");
    PcvLxcVolumeSet *set = vf_set_a();
    GError *err = NULL;
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    vf_text("/proc/100/mountinfo", CLEAN "94 20 0:77 /renamed /mnt/zzz rw - zfs pool/w rw\n");
    g_assert_true(vf_check(set, "mnt/a", &err));
    g_assert_no_error(err);
    vf_text("/proc/100/mountinfo", CLEAN "94 20 0:77 /renamed /mnt//a/ rw - zfs pool/w rw\n");                        
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                                    
                                                                              
static void
test_verify_ambiguous_counts_as_target(void)
{
    vf_begin();
    vf_std();
    g_autofree gchar *sa = vf_slot_line(61, "mnt/a", 0, "0:50", "/vol"), *sb = vf_slot_line(62, "mnt/b", 0, "0:50", "/vol");
    g_autofree gchar *host = g_strconcat(sa, sb, NULL);
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load("lxc.mount.entry = /data mnt/a none bind 0 0\n"
                                                   "lxc.mount.entry = /data mnt/b none bind 0 0\n", "c1", host);
    g_assert_nonnull(set);
    vf_thread(200, 200, CLEAN "95 20 0:50 /vol /mnt/c rw - zfs pool/v rw\n");
    GError *err = NULL;
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    vf_thread(200, 200, CLEAN "95 20 0:50 /vol /mnt/b rw - zfs pool/v rw\n");
    g_assert_true(vf_check(set, "mnt/a", &err));
    g_assert_no_error(err);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                                           
                                                                             
static void
test_verify_overlap_keeps_other_line(void)
{
    vf_begin();
    vf_std();
    g_autofree gchar *sa = vf_slot_line(61, "mnt/a", 0, "0:50", "/"), *sb = vf_slot_line(62, "mnt/b", 0, "0:50", "/db");
    g_autofree gchar *host = g_strconcat(sa, sb, NULL);
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load("lxc.mount.entry = /data mnt/a none bind 0 0\n"
                                                   "lxc.mount.entry = /data/db mnt/b none bind 0 0\n", "c1", host);
    g_assert_nonnull(set);
    vf_text("/proc/100/mountinfo", CLEAN "96 20 0:50 /db /mnt/b rw - zfs pool/v rw\n");                        
    vf_thread(100, 100, CLEAN "96 20 0:50 /db /mnt/b rw - zfs pool/v rw\n");
    vf_thread(200, 200, CLEAN "97 20 0:50 /db /srv/db rw - zfs pool/v rw\n");                              
    GError *err = NULL;
    g_assert_true(vf_check(set, "mnt/a", &err));
    g_assert_no_error(err);
    g_assert_false(vf_check(set, "mnt/b", &err));
    vf_assert_still(err, "/mnt/b");
    g_clear_error(&err);
    vf_thread(300, 300, CLEAN "98 20 0:50 / /srv/all rw - zfs pool/v rw\n");                              
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                                          
                                                                                                                      
                                                                                             
                                                     
static void
test_verify_skip_errno_table(void)
{
    static const struct { const gchar *path; gint err; gboolean ok; const gchar *after; } rows[] = {
        { "/proc/200/task", ENOENT, TRUE, "read /proc/300/task/300/mountinfo" },
        { "/proc/200/task", ESRCH, TRUE, "read /proc/300/task/300/mountinfo" },
        { "/proc/200/task", EINVAL, TRUE, "read /proc/300/task/300/mountinfo" },
        { "/proc/200/task", EACCES, FALSE, NULL },
        { "/proc/200/task", EPERM, FALSE, NULL },
        { "/proc/200/task", EIO, FALSE, NULL },
        { "/proc/200/task", EOPNOTSUPP, FALSE, NULL },
        { "/proc/200/task/201/mountinfo", ENOENT, TRUE, "read /proc/300/task/300/mountinfo" },
        { "/proc/200/task/201/mountinfo", ESRCH, TRUE, "read /proc/300/task/300/mountinfo" },
        { "/proc/200/task/201/mountinfo", EINVAL, TRUE, "read /proc/300/task/300/mountinfo" },
        { "/proc/200/task/201/mountinfo", EACCES, FALSE, NULL },
        { "/proc/200/task/201/mountinfo", EPERM, FALSE, NULL },
        { "/proc/200/task/201/mountinfo", EIO, FALSE, NULL },
        { "/proc/200/task/201/mountinfo", EOPNOTSUPP, FALSE, NULL },
        { CG "/sub/cgroup.procs", ENOENT, TRUE, "list " CG "/sub" },
        { CG "/sub/cgroup.procs", EOPNOTSUPP, TRUE, "list " CG "/sub" },
        { CG "/sub/cgroup.procs", EACCES, FALSE, NULL },
        { CG "/sub/cgroup.procs", EIO, FALSE, NULL },
        { CG "/sub/cgroup.procs", ESRCH, FALSE, NULL },
        { "/proc/100/mountinfo", ENOENT, FALSE, NULL },
        { CG, ENOENT, FALSE, NULL },
        { CG "/sub", ENOENT, TRUE, "read /proc/300/task/300/mountinfo" },
        { CG "/sub", EACCES, FALSE, NULL },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        vf_begin();
        vf_std();
        vf_err(rows[i].path, rows[i].err);
        PcvLxcVolumeSet *set = vf_set_a();
        GError *err = NULL;
        gboolean ok = vf_check(set, "mnt/a", &err);
        g_test_message("row %u: %s errno %d", i, rows[i].path, rows[i].err);
        g_assert_cmpint(ok, ==, rows[i].ok);
        if (rows[i].ok) {
            g_assert_no_error(err);
            g_assert_true(vf_logged(rows[i].after));
        } else {
            g_autofree gchar *why = g_strdup_printf("cannot read %s: %s", rows[i].path, g_strerror(rows[i].err));
            vf_assert_cannot(err, why);
            g_clear_error(&err);
        }
        pcv_lxc_volume_set_free(set);
        vf_end();
    }
}

                                                                                                
static void
vf_many_tids(guint count)
{
    GString *names = g_string_new(NULL);
    for (guint i = 0; i < count; i++)
        g_string_append_printf(names, "%s%u", i ? " " : "", 200 + i);
    vf_dirs("/proc/200/task", names->str);
    g_string_free(names, TRUE);
}

                                                                                            
static void
test_verify_task_limit_fails(void)
{
    vf_begin();
    vf_std();
    vf_dirs(CG, "");                                         
    vf_many_tids(PCV_LXC_VERIFY_MAX_TASKS - 1);                                  
    PcvLxcVolumeSet *set = vf_set_a();
    GError *err = NULL;
    g_assert_true(vf_check(set, "mnt/a", &err));
    g_assert_no_error(err);
    vf_many_tids(PCV_LXC_VERIFY_MAX_TASKS);                            
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_cannot(err, "more than 4096 tasks");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                          
static void
vf_many_cgroups(const gchar *parent, guint count)
{
    GString *names = g_string_new(NULL);
    for (guint i = 0; i < count; i++)
        g_string_append_printf(names, "%ss%u", i ? " " : "", i);
    vf_dirs(parent, names->str);
    g_string_free(names, TRUE);
}

                                                                                                                       
static void
test_verify_cgroup_limit_fails(void)
{
    vf_begin();
    vf_std();
    vf_many_cgroups(CG, PCV_LXC_VERIFY_MAX_CGROUPS);
    PcvLxcVolumeSet *set = vf_set_a();
    GError *err = NULL;
    g_assert_true(vf_check(set, "mnt/a", &err));
    g_assert_no_error(err);
    vf_many_cgroups(CG, PCV_LXC_VERIFY_MAX_CGROUPS + 1);
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_cannot(err, "more than 1024 cgroups");
    g_clear_error(&err);
    vf_dirs(CG, "a");
    vf_many_cgroups(CG "/a", PCV_LXC_VERIFY_MAX_CGROUPS);
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_cannot(err, "more than 1024 cgroups");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                                             
                                                  
                                                                                                           
static GString *
vf_large_body(void)
{
    g_autofree gchar *pad = g_strnfill(900, 'p');
    GString *body = g_string_new(CLEAN);
    for (guint i = 0; i < 4999; i++)
        g_string_append_printf(body, "%u 20 0:%u /layer%u /var/lib/docker/overlay2/%s%u/merged rw - overlay overlay rw\n",
                               1000 + i, 60 + i % 7, i, pad, i);
    g_assert_cmpuint(body->len, >, 4u * 1024 * 1024);
    return body;
}

static void
test_verify_large_mountinfo(void)
{
    vf_begin();
    vf_std();
    GString *body = vf_large_body();
    vf_thread(200, 200, body->str);
    PcvLxcVolumeSet *set = vf_set_a();
    GError *err = NULL;
    g_assert_true(vf_check(set, "mnt/a", &err));
    g_assert_no_error(err);
    g_string_append(body, "7000 20 0:50 /vol /var/lib/docker/volumes/x/_data rw - zfs pool/v rw\n");                         
    vf_thread(200, 200, body->str);
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    g_string_free(body, TRUE);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                     
static gchar *
vf_long(const gchar *prefix)
{
    GString *s = g_string_new(prefix);
    for (guint i = 0; i < 21; i++) {
        g_string_append_c(s, '/');
        for (guint k = 0; k < 200; k++)
            g_string_append_c(s, 'd');
    }
    return g_string_free(s, FALSE);
}

                                                                                                    
                                                                                            
                                                    
static void
test_verify_long_copy_root_fails(void)
{
    vf_begin();
    vf_std();
    g_autofree gchar *deep = vf_long("/vol"), *deep_mp = vf_long("/srv");
    g_assert_cmpuint(strlen(deep), >, 4096);
    g_autofree gchar *copy = g_strdup_printf(CLEAN "99 20 0:50 %s /srv/deep rw - zfs pool/v rw\n", deep);
    vf_thread(200, 200, copy);
    PcvLxcVolumeSet *set = vf_set_a();
    GError *err = NULL;
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    g_autofree gchar *copy2 = g_strdup_printf(CLEAN "99 20 0:50 %s %s rw - zfs pool/v rw\n", deep, deep_mp);
    vf_thread(200, 200, copy2);
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    vf_thread(200, 200, CLEAN);
    g_autofree gchar *slashes = g_strnfill(5000, '/');
    g_autofree gchar *init = g_strdup_printf(CLEAN "98 20 0:77 /renamed /mnt%sa rw - zfs pool/w rw\n", slashes);
    vf_text("/proc/100/mountinfo", init);
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                                           
static void
test_verify_long_ident_root_fails(void)
{
    vf_begin();
    vf_std();
    g_autofree gchar *deep = vf_long("/vol");
    g_autofree gchar *slot = vf_slot_line(61, "mnt/a", 0, "0:50", deep);
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load("lxc.mount.entry = /data mnt/a none bind 0 0\n", "c1", slot);
    g_assert_nonnull(set);
    const PcvLxcVolumeLine *l = set->lines->pdata[0];
    g_assert_true(l->ident_ok);
    g_assert_cmpstr(l->ident.root, ==, deep);
    g_assert_cmpuint(V.source_opens, ==, 0);
    g_autofree gchar *copy = g_strdup_printf(CLEAN "99 20 0:50 %s/sub /srv/x rw - zfs pool/v rw\n", deep);
    vf_thread(200, 201, copy);
    GError *err = NULL;
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                                    
                                                                                                          
                                                         
static void
test_verify_target_without_ident_fails(void)
{
    vf_begin();
    vf_std();
    vf_thread(200, 200, CLEAN COPY_AT_A);
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load("lxc.mount.entry = /data mnt/a none bind 0 0\n", "c1",
                                                   "1 0 8:1 / / rw - ext4 /dev/sda1 rw\n");                                        
    g_assert_nonnull(set);
    g_assert_false(((const PcvLxcVolumeLine *)set->lines->pdata[0])->ident_ok);
    GError *err = NULL;
    g_ptr_array_set_size(V.log, 0);
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_cannot(err, "cannot identify the volume source on the host");
    g_clear_error(&err);
    g_assert_cmpuint(V.log->len, ==, 0);
    g_assert_false(vf_check(set, "/mnt//zzz/", &err));                                 
    vf_assert_cannot(err, "cannot identify the volume source on the host");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    g_autofree gchar *sa = vf_slot_line(61, "mnt/a", 0, "0:50", "/vol");                                           
    set = pcv_lxc_volume_set_load("lxc.mount.entry = /data mnt/a none bind 0 0\n"
                                  "lxc.mount.entry = /data2 mnt/a none bind 0 0\n"
                                  "lxc.mount.entry = /data3 mnt/b none bind 0 0\n", "c1", sa);
    g_assert_nonnull(set);
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_cannot(err, "cannot identify the volume source on the host");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    set = pcv_lxc_volume_set_load("lxc.mount.entry = /data mnt/a none bind 0 0\n"
                                  "lxc.mount.entry = /data3 mnt/b none bind 0 0\n", "c1", sa);
    g_assert_false(vf_check(set, "mnt/a", &err));                                                      
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                               
                                                                                 
static void
test_verify_budget_exceeded_after_files(void)
{
    vf_begin();
    vf_std();
    vf_thread(200, 200, CLEAN COPY_AT_A);
    PcvLxcVolumeSet *set = vf_set_a();
    V.fake_clock = TRUE;
    V.clock_base = 1000000;
    V.clock_step = PCV_LXC_VERIFY_BUDGET_US / 3 + 1;                                          
    V.calls = 0;
    g_ptr_array_set_size(V.log, 0);
    GError *err = NULL;
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_cannot(err, "verification took longer than 5 s");
    g_clear_error(&err);
    g_assert_cmpuint(V.log->len, ==, 3);                                                                              
    g_assert_false(vf_logged("read /proc/200/task/200/mountinfo"));
    V.clock_step = PCV_LXC_VERIFY_BUDGET_US / 2 + 1;                                                 
    V.calls = 0;
    g_ptr_array_set_size(V.log, 0);
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_cannot(err, "verification took longer than 5 s");
    g_clear_error(&err);
    g_assert_cmpuint(V.log->len, ==, 2);
    V.clock_step = PCV_LXC_VERIFY_BUDGET_US / 100;
    V.calls = 0;
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                           
             
static void
test_verify_budget_exceeded_mid_file(void)
{
    vf_begin();
    vf_std();
    PcvLxcVolumeSet *set = vf_set_a();
    V.fake_clock = TRUE;
    V.clock_base = 1000000;
    V.clock_step = 0;
    V.slow_path = "/proc/200/task/201/mountinfo";
    GError *err = NULL;
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_cannot(err, "verification took longer than 5 s");
    g_clear_error(&err);
    g_assert_false(vf_logged("read /proc/300/task/300/mountinfo"));                           
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                                                                                                 
                                                                                             
                                                                                                    
                                                                                               
                                                                                                        
                                                                                     
                                                                                    
                                                 
static void
test_verify_budget_large_mountinfo_normal(void)
{
    const gint64 div = RUNNING_ON_VALGRIND != 0 ? 10 : 1;
    const gint64 limit = RUNNING_ON_VALGRIND != 0 ? PCV_LXC_VERIFY_BUDGET_US * div + 10 * G_USEC_PER_SEC : PCV_LXC_VERIFY_BUDGET_US;
    vf_begin();
    V.clock_div = div;                                                                                 
    vf_std();
    GString *body = vf_large_body();
    vf_thread(200, 200, body->str);
    vf_thread(200, 201, body->str);
    vf_thread(300, 300, body->str);
    PcvLxcVolumeSet *set = vf_set_a();
    GError *err = NULL;
    gint64 t0 = g_get_monotonic_time();
    g_assert_true(vf_check(set, "mnt/a", &err));
    g_assert_no_error(err);
    g_assert_cmpint(g_get_monotonic_time() - t0, <, limit);
    g_string_append(body, "7000 20 0:50 /vol /var/lib/docker/volumes/x/_data rw - zfs pool/v rw\n");
    vf_thread(300, 300, body->str);
    t0 = g_get_monotonic_time();
    g_assert_false(vf_check(set, "mnt/a", &err));
    vf_assert_still(err, "/mnt/a");
    g_clear_error(&err);
    g_assert_cmpint(g_get_monotonic_time() - t0, <, limit);
    g_string_free(body, TRUE);
    pcv_lxc_volume_set_free(set);
    vf_end();
}

static int
rt_rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

                                                                                                        
                                                                           
static void
test_real_ops_tmpdir(void)
{
    const PcvLxcVerifyOps *ops = pcv_lxc_verify_real_ops_for_test();
    g_assert_nonnull(ops);
    g_autofree gchar *base = g_dir_make_tmp("pcv-verify-XXXXXX", NULL);
    g_assert_nonnull(base);
    g_autofree gchar *a = g_build_filename(base, "a", NULL), *b = g_build_filename(base, "b", NULL);
    g_autofree gchar *f = g_build_filename(base, "f", NULL), *l = g_build_filename(base, "l", NULL);
    g_autofree gchar *missing = g_build_filename(base, "missing", NULL);
    g_assert_cmpint(g_mkdir(a, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(b, 0700), ==, 0);
    g_assert_cmpint(symlink(a, l), ==, 0);
    GString *big = g_string_new(NULL);
    for (guint i = 0; i < 4000; i++)
        g_string_append_printf(big, "%u 1 0:1 / /m%u rw - tmpfs t rw\n", i, i);
    g_assert_cmpuint(big->len, >, 64u * 1024);
    g_assert_true(g_file_set_contents(f, big->str, (gssize)big->len, NULL));

    gint e = 0;
    g_autofree gchar *got = ops->read_text(f, G_MAXINT64, &e);
    g_assert_cmpstr(got, ==, big->str);
    e = 0;
    g_assert_null(ops->read_text(missing, G_MAXINT64, &e));
    g_assert_cmpint(e, ==, ENOENT);
    e = 0;
    g_assert_null(ops->read_text(a, G_MAXINT64, &e));
    g_assert_cmpint(e, ==, EISDIR);
    e = 0;
    g_assert_null(ops->read_text(f, ops->now_us() - 1, &e));                                        
    g_assert_cmpint(e, ==, ETIMEDOUT);
    e = 0;
    g_assert_null(ops->list_dirs(base, ops->now_us() - 1, &e));
    g_assert_cmpint(e, ==, ETIMEDOUT);
    gint64 t1 = ops->now_us(), t2 = ops->now_us();
    g_assert_cmpint(t1, >, 0);
    g_assert_cmpint(t2, >=, t1);                                            

    e = 0;
    GPtrArray *names = ops->list_dirs(base, G_MAXINT64, &e);
    g_assert_nonnull(names);
    gboolean saw_a = FALSE, saw_b = FALSE;
    for (guint i = 0; i < names->len; i++) {
        const gchar *n = names->pdata[i];
        saw_a |= g_str_equal(n, "a");
        saw_b |= g_str_equal(n, "b");
        g_assert_true(g_str_equal(n, "a") || g_str_equal(n, "b"));                                
    }
    g_assert_true(saw_a && saw_b);
    g_assert_cmpuint(names->len, ==, 2);
    g_ptr_array_unref(names);
    e = 0;
    g_assert_null(ops->list_dirs(missing, G_MAXINT64, &e));
    g_assert_cmpint(e, ==, ENOENT);
    e = 0;
    g_assert_null(ops->list_dirs(f, G_MAXINT64, &e));
    g_assert_cmpint(e, ==, ENOTDIR);

    g_string_free(big, TRUE);
    g_assert_cmpint(nftw(base, rt_rm_cb, 16, FTW_DEPTH | FTW_PHYS), ==, 0);
}

                                                                                                   
                                                                                                                    
                                                

                                                              
static gchar *
vf_join(const gchar *const *argv)
{
    return argv ? g_strjoinv(" ", (gchar **)argv) : NULL;
}

                                                                                                                                                                                      
                                                                               
static void
test_ns_detach_argv(void)
{
    vf_begin();
    g_autofree gchar *sb = vf_slot_line(62, "mnt/b", 0, "0:62", "/data/db"), *sa = vf_slot_line(61, "mnt/a", 0, "0:62", "/data");
    g_autofree gchar *host = g_strconcat("1 0 8:1 / / rw - ext4 /dev/sda1 rw\n", sb, sa, NULL);
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load("lxc.mount.entry = /data/db mnt/b none bind 0 0\n"
                                                   "lxc.mount.entry = /data mnt/a none bind,create=dir 0 0\n", "c1", host);
    g_assert_nonnull(set);
    const gchar *want = "pcvctl container volume-ns-detach 4242 /lxc.payload.c1 0:62 /data mnt/a 0:62 /data/db mnt/b";
    g_auto(GStrv) argv = pcv_lxc_volume_ns_detach_argv("pcvctl", "c1", 4242, set, "mnt/a");
    g_assert_nonnull(argv);
    g_assert_cmpuint(g_strv_length(argv), ==, 11);                                                            
    g_autofree gchar *got = vf_join((const gchar *const *)argv);
    g_assert_cmpstr(got, ==, want);
    g_auto(GStrv) argv2 = pcv_lxc_volume_ns_detach_argv("pcvctl", "c1", 4242, set, "//mnt//a/");
    g_autofree gchar *got2 = vf_join((const gchar *const *)argv2);
    g_assert_cmpstr(got2, ==, want);
    g_auto(GStrv) argv_b = pcv_lxc_volume_ns_detach_argv("pcvctl", "c1", 4242, set, "mnt/b");
    g_autofree gchar *got_b = vf_join((const gchar *const *)argv_b);
    g_assert_cmpstr(got_b, ==, "pcvctl container volume-ns-detach 4242 /lxc.payload.c1 0:62 /data/db mnt/b 0:62 /data mnt/a");
    g_assert_null(pcv_lxc_volume_ns_detach_argv("pcvctl", "c1", 4242, set, "mnt/zzz"));                       
    g_assert_null(pcv_lxc_volume_ns_detach_argv("pcvctl", "c1", 0, set, "mnt/a"));                         
    g_assert_null(pcv_lxc_volume_ns_detach_argv(NULL, "c1", 4242, set, "mnt/a"));
    g_assert_null(pcv_lxc_volume_ns_detach_argv("pcvctl", "c1", 4242, NULL, "mnt/a"));
    g_assert_null(pcv_lxc_volume_ns_detach_argv("pcvctl", "c1", 4242, set, NULL));
                                                                                                 
                           
    for (const gchar *const *bad = (const gchar *const[]){ "", "a/b", ".", "..", NULL }; *bad; bad++)
        g_assert_null(pcv_lxc_volume_ns_detach_argv("pcvctl", *bad, 4242, set, "mnt/a"));
    g_assert_null(pcv_lxc_volume_ns_detach_argv("pcvctl", NULL, 4242, set, "mnt/a"));
    pcv_lxc_volume_set_free(set);

                                                          
    g_autofree gchar *s0 = vf_slot_line(61, "mnt/a", 0, "0:50", "/vol"), *s1 = vf_slot_line(62, "mnt/a", 1, "0:51", "/vol2");
    g_autofree gchar *s2 = vf_slot_line(63, "mnt/b", 0, "0:52", "/v3");
    g_autofree gchar *host2 = g_strconcat(s2, s0, s1, NULL);
    set = pcv_lxc_volume_set_load("lxc.mount.entry = /data3 mnt/b none bind 0 0\n"
                                  "lxc.mount.entry = /data mnt/a none bind 0 0\n"
                                  "lxc.mount.entry = /data2 /mnt/a/ none bind 0 0\n", "c1", host2);
    g_assert_nonnull(set);
    g_auto(GStrv) argv3 = pcv_lxc_volume_ns_detach_argv("pcvctl", "c1", 4242, set, "mnt/a");
    g_autofree gchar *got3 = vf_join((const gchar *const *)argv3);
    g_assert_cmpstr(got3, ==, "pcvctl container volume-ns-detach 4242 /lxc.payload.c1 0:50 /vol mnt/a 0:51 /vol2 mnt/a 0:52 /v3 mnt/b");
    pcv_lxc_volume_set_free(set);

                                                         
    set = pcv_lxc_volume_set_load("lxc.mount.entry = /data mnt/a none bind 0 0\n"
                                  "lxc.mount.entry = /data3 mnt/b none bind 0 0\n", "c1", s0);
    g_assert_nonnull(set);
    g_auto(GStrv) argv4 = pcv_lxc_volume_ns_detach_argv("pcvctl", "c1", 4242, set, "mnt/a");
    g_autofree gchar *got4 = vf_join((const gchar *const *)argv4);
    g_assert_cmpstr(got4, ==, "pcvctl container volume-ns-detach 4242 /lxc.payload.c1 0:50 /vol mnt/a");
    g_assert_null(pcv_lxc_volume_ns_detach_argv("pcvctl", "c1", 4242, set, "mnt/b"));                       
    pcv_lxc_volume_set_free(set);
    vf_end();
}

                              
static struct {
    guint     calls;
    gchar    *joined;
    gboolean  result;
    const gchar *out;                                   
} NSD;

static gboolean
nsd_fake(const gchar *const *argv, gchar **out, GError **error)
{
    NSD.calls++;
    if (out)
        *out = g_strdup(NSD.out ? NSD.out : "");
    g_free(NSD.joined);
    NSD.joined = vf_join(argv);
    if (!NSD.result)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "fake: timed out after 5s");
    return NSD.result;
}

                                                                                                    
                                                                                                
static void
test_ns_detach_seam(void)
{
    vf_begin();
    memset(&NSD, 0, sizeof NSD);
    pcv_lxc_volume_set_ns_detach_for_test(nsd_fake);
    PcvLxcVolumeSet *set = vf_set_a();
    GError *err = NULL;
    NSD.result = TRUE;
    g_assert_true(pcv_lxc_volume_ns_detach("c1", 4242, set, "mnt/a", &err));
    g_assert_no_error(err);
    g_assert_cmpuint(NSD.calls, ==, 1);
    g_assert_cmpstr(NSD.joined, ==, PCV_LXC_VOLUME_GUARD_HOOK " container volume-ns-detach 4242 /lxc.payload.c1 0:50 /vol mnt/a");
    NSD.result = FALSE;
    g_assert_false(pcv_lxc_volume_ns_detach("c1", 4242, set, "/mnt/a", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);                                            
    g_clear_error(&err);
    g_assert_cmpuint(NSD.calls, ==, 2);
    g_assert_false(pcv_lxc_volume_ns_detach("c1", 4242, set, "mnt/zzz", &err));
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT);
    g_assert_cmpstr(err->message, ==, "cannot identify the volume source on the host; stop the container, then detach again");
    g_clear_error(&err);
    g_assert_cmpuint(NSD.calls, ==, 2);                                                   
                                                                                               
                                                                                                           
                                         
    NSD.result = TRUE;
    NSD.out = "detached 12\nlocked 34\nlocked 56\n";
    g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*ns detach*container 'c1'*pid 4242*mnt/a*locked mount id 34*final check*");
    g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*ns detach*container 'c1'*pid 4242*mnt/a*locked mount id 56*final check*");
    g_assert_true(pcv_lxc_volume_ns_detach("c1", 4242, set, "mnt/a", &err));
    g_assert_no_error(err);
    g_test_assert_expected_messages();
    NSD.out = "detached 12\n";                                                  
    g_assert_true(pcv_lxc_volume_ns_detach("c1", 4242, set, "mnt/a", &err));
    NSD.out = NULL;
    pcv_lxc_volume_set_free(set);
    pcv_lxc_volume_set_ns_detach_for_test(NULL);
    g_clear_pointer(&NSD.joined, g_free);
    vf_end();
}

                                                                                                           
                                                                                                  
                                                                                                                           
                                                                                                                  
                                                                                                         
                                                                                            
                                                                                 

#define DCG PCV_LXC_CGROUP_ROOT "/lxc.payload.c1"

typedef struct { guint64 id; gchar *dev; gchar *root; gchar *mp; gboolean alive; } DMount;

static struct {
    GArray         *mounts;                                     
    GHashTable     *dirs;                                                      
    GHashTable     *files;                                                         
    GHashTable     *fd_path;                      
    GArray         *made;                       
    int             base_fd;
    struct stat     base_st;
    gchar          *tmp, *lxc, *rootfs, *stage;                                              
    PcvLxcState     state;
    pid_t           init;
    gboolean        nsd_ok;
    guint           nsd_calls;
    guint64         private_fail_id, umount_fail_id;
    gboolean        reboot_copy;                                                               
    PcvLxcMountOps  ops;
} D;

                                                
static gchar *
df_norm(const gchar *p)
{
    GString *s = g_string_new(NULL);
    for (const gchar *c = p; *c; c++)
        if (!(*c == '/' && s->len && s->str[s->len - 1] == '/'))
            g_string_append_c(s, *c);
    while (s->len > 1 && s->str[s->len - 1] == '/')
        g_string_truncate(s, s->len - 1);
    return g_string_free(s, FALSE);
}

static void
df_log(gchar *entry)
{
    g_ptr_array_add(V.log, entry);
}

static int
df_new_fd(const gchar *path)
{
    int fd = fcntl(D.base_fd, F_DUPFD_CLOEXEC, 0);
    g_assert_cmpint(fd, >=, 0);
    g_hash_table_insert(D.fd_path, GINT_TO_POINTER(fd), g_strdup(path));
    g_array_append_val(D.made, fd);
    return fd;
}

static const gchar *
df_path_of(int fd)
{
    const gchar *p = g_hash_table_lookup(D.fd_path, GINT_TO_POINTER(fd));
    g_assert_nonnull(p);
    return p;
}

                                                               
static guint64
df_top(const gchar *path)
{
    guint64 id = 0;
    for (guint i = 0; i < D.mounts->len; i++) {
        DMount *m = &g_array_index(D.mounts, DMount, i);
        if (m->alive && g_str_equal(m->mp, path))
            id = m->id;
    }
    return id;
}

static gchar *
df_read_mountinfo(GError **error)
{
    (void)error;
    df_log(g_strdup("host-read"));
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < D.mounts->len; i++) {
        DMount *m = &g_array_index(D.mounts, DMount, i);
        if (m->alive)
            g_string_append_printf(s, "%" G_GUINT64_FORMAT " 1 %s %s %s rw,relatime shared:1 - zfs pool/x rw\n", m->id, m->dev, m->root,
                                   m->mp);
    }
    return g_string_free(s, FALSE);
}

static int
df_open_dir(const gchar *path, GError **error)
{
    g_autofree gchar *n = df_norm(path);
    df_log(g_strdup_printf("open %s", n));
    if (!g_hash_table_contains(D.dirs, n)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No such file or directory");
        return -1;
    }
    return df_new_fd(n);
}

static gchar *
df_fd_path(int fd)
{
    return g_strdup(df_path_of(fd));
}

                                                                       
static gboolean
df_parents_are_dirs(const gchar *full)
{
    for (const gchar *c = strchr(full + 1, '/'); c; c = strchr(c + 1, '/')) {
        g_autofree gchar *pre = g_strndup(full, (gsize)(c - full));
        if (g_hash_table_contains(D.files, pre))
            return FALSE;
    }
    return TRUE;
}

                                                                                      
static int
df_walk(int root_fd, const gchar *rel, gboolean create, GError **error)
{
    g_assert_false(create);
    g_autofree gchar *raw = g_strconcat(df_path_of(root_fd), rel, NULL);
    g_autofree gchar *full = df_norm(raw);
    df_log(g_strdup_printf("walk %s", full));
    if (!df_parents_are_dirs(full) || g_hash_table_contains(D.files, full)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "Not a directory");
        return -1;
    }
    return df_new_fd(full);
}

                                                                                  
static int
df_open_leaf(int root_fd, const gchar *rel, GError **error)
{
    g_assert_true(rel && rel[0] == '/');
    g_autofree gchar *raw = g_strconcat(df_path_of(root_fd), rel, NULL);
    g_autofree gchar *full = df_norm(raw);
    df_log(g_strdup_printf("leaf %s", full));
    if (!df_parents_are_dirs(full)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "Not a directory");
        return -1;
    }
    return df_new_fd(full);
}

static gboolean
df_mnt_id(int fd, guint64 *id_out)
{
    guint64 id = df_top(df_path_of(fd));
    *id_out = id ? id : 1;                           
    return TRUE;
}

static gboolean
df_make_private(int fd, GError **error)
{
    const gchar *p = df_path_of(fd);
    df_log(g_strdup_printf("private %s", p));
    if (D.private_fail_id && df_top(p) == D.private_fail_id) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "mount_setattr private: Invalid argument");
        return FALSE;
    }
    return TRUE;
}

static gboolean
df_umount_fd(int fd, GError **error)
{
    const gchar *p = df_path_of(fd);
    df_log(g_strdup_printf("umount %s", p));
    guint64 id = df_top(p);
    if (D.umount_fail_id && id == D.umount_fail_id) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "umount: Device or resource busy");
        return FALSE;
    }
    for (guint i = 0; i < D.mounts->len; i++)
        if (g_array_index(D.mounts, DMount, i).id == id)
            g_array_index(D.mounts, DMount, i).alive = FALSE;
    if (D.reboot_copy && g_str_has_prefix(p, D.stage))
        vf_thread(200, 200, CLEAN COPY_AT_A);                                                      
    return TRUE;
}

static gboolean
df_remove_at(int dir_fd, const gchar *name, GError **error)
{
    (void)error;
    const gchar *dir = df_path_of(dir_fd);
    g_autofree gchar *child = g_strdup_printf("%s/%s", dir, name);
    df_log(g_strdup_printf("remove %s", child));
    GPtrArray *ents = g_hash_table_lookup(D.dirs, dir);
    for (guint i = 0; ents && i < ents->len; i++)
        if (g_str_equal(g_ptr_array_index(ents, i), name)) {
            g_ptr_array_remove_index(ents, i);
            break;
        }
    g_hash_table_remove(D.dirs, child);
    return TRUE;
}

static GPtrArray *
df_list_at(int dir_fd, GError **error)
{
    (void)error;
    const gchar *dir = df_path_of(dir_fd);
    df_log(g_strdup_printf("ls %s", dir));
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *ents = g_hash_table_lookup(D.dirs, dir);
    for (guint i = 0; ents && i < ents->len; i++)
        g_ptr_array_add(out, g_strdup(g_ptr_array_index(ents, i)));
    return out;
}

static PcvLxcState
df_state(const gchar *name)
{
    g_assert_cmpstr(name, ==, "c1");
    return D.state;
}

static pid_t
df_init(const gchar *name)
{
    g_assert_cmpstr(name, ==, "c1");
    return D.init;
}

static gboolean
df_nsd(const gchar *const *argv, gchar **out, GError **error)
{
    g_autofree gchar *joined = vf_join(argv);
    df_log(g_strdup_printf("nsd %s", joined));
    D.nsd_calls++;
    if (out)
        *out = g_strdup("");
    if (!D.nsd_ok)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "fake: timed out after 5s");
    return D.nsd_ok;
}

static void
df_dmount_clear(gpointer p)
{
    DMount *m = p;
    g_free(m->dev);
    g_free(m->root);
    g_free(m->mp);
}

                                                  
static void
df_mount(guint64 id, const gchar *dev, const gchar *root, const gchar *mp)
{
    DMount m = { id, g_strdup(dev), g_strdup(root), g_strdup(mp), TRUE };
    g_array_append_val(D.mounts, m);
}

static gboolean
df_alive(guint64 id)
{
    for (guint i = 0; i < D.mounts->len; i++)
        if (g_array_index(D.mounts, DMount, i).id == id)
            return g_array_index(D.mounts, DMount, i).alive;
    return FALSE;
}

                                                   
static void
df_dir(const gchar *path, const gchar *names)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_auto(GStrv) parts = g_strsplit(names, " ", -1);
    for (gchar **p = parts; *p; p++)
        if (**p)
            g_ptr_array_add(a, g_strdup(*p));
    g_hash_table_replace(D.dirs, g_strdup(path), a);
}

                                                              
static gchar *
df_slot(const gchar *dest, guint ord)
{
    return pcv_lxc_volstage_slot(VS_ROOT, "c1", dest, ord);
}

                                       
static gchar *
df_rp(const gchar *rel)
{
    return g_strconcat(D.rootfs, rel, NULL);
}

static void
df_config(const gchar *text)
{
    g_autofree gchar *cfg = g_build_filename(D.lxc, "c1", "config", NULL);
    g_assert_true(g_file_set_contents(cfg, text, -1, NULL));
}

static void
df_begin(void)
{
    vf_begin();
    memset(&D, 0, sizeof D);
    D.base_fd = memfd_create("pcv-fake-detach-fd", MFD_CLOEXEC);                                                      
    g_assert_cmpint(D.base_fd, >=, 0);
    g_assert_cmpint(fstat(D.base_fd, &D.base_st), ==, 0);
    D.mounts = g_array_new(FALSE, TRUE, sizeof(DMount));
    g_array_set_clear_func(D.mounts, df_dmount_clear);
    D.dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    D.files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    D.fd_path = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    D.made = g_array_new(FALSE, FALSE, sizeof(int));
    D.tmp = g_dir_make_tmp("pcv-detach-XXXXXX", NULL);
    g_assert_nonnull(D.tmp);
    D.lxc = g_build_filename(D.tmp, "lxc", NULL);
    g_autofree gchar *cdir = g_build_filename(D.lxc, "c1", NULL);
    g_assert_cmpint(g_mkdir_with_parents(cdir, 0700), ==, 0);
    D.rootfs = g_build_filename(cdir, "rootfs", NULL);
    D.stage = g_strdup(VS_ROOT "/c1");
    df_mount(1, "8:1", "/", "/");
    df_dir(D.rootfs, "");
    D.state = PCV_LXC_STATE_RUNNING;
    D.init = INIT;
    D.nsd_ok = TRUE;
    D.ops.read_mountinfo = df_read_mountinfo;
    D.ops.open_dir = df_open_dir;
    D.ops.fd_path = df_fd_path;
    D.ops.walk = df_walk;
    D.ops.open_leaf = df_open_leaf;
    D.ops.mnt_id = df_mnt_id;
    D.ops.make_private = df_make_private;
    D.ops.umount_fd = df_umount_fd;
    D.ops.remove_at = df_remove_at;
    D.ops.list_at = df_list_at;
    D.ops.source_open = vf_source_open;                                                           
    pcv_lxc_volume_set_ops_for_test(&D.ops);
    pcv_lxc_volume_set_state_ops_for_test(df_state, df_init);
    pcv_lxc_volume_set_ns_detach_for_test(df_nsd);
    vf_std_at(DCG);
}

static void
df_end(void)
{
    pcv_lxc_volume_set_state_ops_for_test(NULL, NULL);
    pcv_lxc_volume_set_ns_detach_for_test(NULL);
    for (guint i = 0; i < D.made->len; i++) {                                                              
        int fd = g_array_index(D.made, int, i);
        struct stat st;
        if (fcntl(fd, F_GETFD) >= 0 && fstat(fd, &st) == 0)
            g_assert_false(st.st_ino == D.base_st.st_ino && st.st_dev == D.base_st.st_dev);
    }
    close(D.base_fd);
    g_array_unref(D.mounts);
    g_hash_table_unref(D.dirs);
    g_hash_table_unref(D.files);
    g_hash_table_unref(D.fd_path);
    g_array_unref(D.made);
    g_assert_cmpint(nftw(D.tmp, rt_rm_cb, 16, FTW_DEPTH | FTW_PHYS), ==, 0);
    g_free(D.tmp);
    g_free(D.lxc);
    g_free(D.rootfs);
    g_free(D.stage);
    vf_end();
}

static guint
df_count(const gchar *head)
{
    guint n = 0;
    for (guint i = 0; i < V.log->len; i++)
        n += g_str_has_prefix(g_ptr_array_index(V.log, i), head);
    return n;
}

                                                                                                                  
static gchar *
df_seq(void)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < V.log->len; i++) {
        const gchar *e = g_ptr_array_index(V.log, i);
        if (g_str_has_prefix(e, "private ") || g_str_has_prefix(e, "umount ") || g_str_has_prefix(e, "nsd ") ||
            g_str_equal(e, "read /proc/100/mountinfo"))
            g_string_append_printf(s, "%s%s", s->len ? "|" : "", e);
    }
    return g_string_free(s, FALSE);
}

static gboolean
df_run(const gchar *cpath, gboolean *drop, gboolean *was, GError **err)
{
    *drop = *was = 99;                      
    return pcv_lxc_volume_detach_run(D.lxc, "c1", cpath, drop, was, err);
}

                                                                                                                 
                                                                                                              
static void
df_std_two(void)
{
    df_config("lxc.uts.name = c1\nlxc.mount.entry = /data mnt/a none bind,create=dir 0 0\n"
              "lxc.mount.entry = /other mnt/b none bind 0 0\n");
    g_autofree gchar *sa = df_slot("mnt/a", 0), *sb = df_slot("mnt/b", 0);
    g_autofree gchar *ra = df_rp("/mnt/a"), *rx = df_rp("/srv/x"), *rb = df_rp("/mnt/b");
    df_mount(61, "0:50", "/vol", sa);
    df_mount(62, "0:51", "/ob", sb);
    df_mount(70, "0:50", "/vol", ra);
    df_mount(71, "0:50", "/vol/sub", rx);
    df_mount(72, "0:51", "/ob", rb);
    g_autofree gchar *names = g_strdup_printf("%s %s", strrchr(sa, '/') + 1, strrchr(sb, '/') + 1);
    df_dir(VS_ROOT, "c1");
    df_dir(D.stage, names);
}

static void
df_assert_conflict(GError *err, const gchar *msg)
{
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT);
    g_assert_cmpstr(err->message, ==, msg);
}

                                                                                                            
                                                                                                                     
                                
static void
test_run_order(void)
{
    df_begin();
    df_std_two();
    gboolean drop, was;
    GError *err = NULL;
    g_assert_true(df_run("/mnt/a", &drop, &was, &err));
    g_assert_no_error(err);
    g_assert_cmpint(drop, ==, TRUE);
    g_assert_cmpint(was, ==, TRUE);
    g_autofree gchar *sa = df_slot("mnt/a", 0), *ra = df_rp("/mnt/a"), *rx = df_rp("/srv/x");
    g_autofree gchar *want = g_strdup_printf("private %s|private %s|umount %s|umount %s|nsd " PCV_LXC_VOLUME_GUARD_HOOK
                                             " container volume-ns-detach 100 /lxc.payload.c1 0:50 /vol mnt/a 0:51 /ob mnt/b|read /proc/100/mountinfo|"
                                             "private %s|umount %s|read /proc/100/mountinfo", rx, ra, rx, ra, sa, sa);
    g_autofree gchar *seq = df_seq();
    g_assert_cmpstr(seq, ==, want);
    g_assert_false(df_alive(70) || df_alive(71) || df_alive(61));
    g_assert_true(df_alive(72) && df_alive(62));                               
    g_assert_cmpuint(df_count("walk "), ==, 0);                                    
    df_end();
}

                                                                                        
static void
test_run_keeps_empty_slot_dir(void)
{
    df_begin();
    df_std_two();
    gboolean drop, was;
    GError *err = NULL;
    g_assert_true(df_run("/mnt/a", &drop, &was, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(df_count("remove "), ==, 0);
    g_assert_cmpuint(df_count("ls "), ==, 0);
    GPtrArray *ents = g_hash_table_lookup(D.dirs, D.stage);
    g_assert_nonnull(ents);
    g_assert_cmpuint(ents->len, ==, 2);
    df_end();
}

                                                                                                           
                                                               
static void
test_run_changing_state(void)
{
    static const PcvLxcState states[] = { PCV_LXC_STATE_STARTING, PCV_LXC_STATE_STOPPING, PCV_LXC_STATE_FROZEN, PCV_LXC_STATE_UNKNOWN,
                                          PCV_LXC_STATE_RUNNING };
    for (guint i = 0; i < G_N_ELEMENTS(states); i++) {
        df_begin();
        df_std_two();
        D.state = states[i];
        if (states[i] == PCV_LXC_STATE_RUNNING)
            D.init = -1;                                                               
        gboolean drop, was;
        GError *err = NULL;
        g_test_message("state %d", (int)states[i]);
        g_assert_false(df_run("/mnt/a", &drop, &was, &err));
        df_assert_conflict(err, "container is changing state; try again");
        g_clear_error(&err);
        g_assert_cmpint(drop, ==, FALSE);
        g_assert_cmpint(was, ==, FALSE);
        g_assert_cmpuint(df_count("umount "), ==, 0);
        g_assert_cmpuint(df_count("host-read"), ==, 0);
        g_assert_cmpuint(D.nsd_calls, ==, 0);
        df_end();
    }
}

                                                                                                                      
static void
test_run_unidentified_source(void)
{
    df_begin();
    df_config("lxc.mount.entry = /gone mnt/a none bind 0 0\n");
    g_autofree gchar *ra = df_rp("/mnt/a");
    df_mount(70, "0:50", "/vol", ra);
    gboolean drop, was;
    GError *err = NULL;
    g_assert_false(df_run("/mnt/a", &drop, &was, &err));
    df_assert_conflict(err, "cannot identify the volume source on the host; stop the container, then detach again");
    g_clear_error(&err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_cmpuint(df_count("umount "), ==, 0);
    g_assert_cmpuint(D.nsd_calls, ==, 0);
    g_assert_cmpuint(V.source_opens, >=, 1);
    g_assert_true(df_alive(70));
    df_end();
}

                                                                                    
static void
test_run_was_running_output(void)
{
    static const struct { PcvLxcState st; gboolean ok, was; } rows[] = {
        { PCV_LXC_STATE_RUNNING, TRUE, TRUE }, { PCV_LXC_STATE_STOPPED, TRUE, FALSE }, { PCV_LXC_STATE_STARTING, FALSE, FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        df_begin();
        df_std_two();
        D.state = rows[i].st;
        gboolean drop, was;
        GError *err = NULL;
        g_assert_cmpint(df_run("/mnt/a", &drop, &was, &err), ==, rows[i].ok);
        g_assert_cmpint(was, ==, rows[i].was);
        g_clear_error(&err);
        df_end();
    }
}

                                                                                                
static void
test_run_no_line_no_mount_ok(void)
{
    df_begin();
    df_std_two();
    gboolean drop, was;
    GError *err = NULL;
    g_assert_true(df_run("/mnt/x", &drop, &was, &err));
    g_assert_no_error(err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_cmpint(was, ==, TRUE);
    g_assert_cmpuint(df_count("umount "), ==, 0);
    g_assert_cmpuint(D.nsd_calls, ==, 0);
    g_assert_cmpuint(df_count("read /proc/"), ==, 0);
    df_end();
}

                                                                                            
                                                                                                        
static void
test_run_no_line_host_mount_uses_mount_ident(void)
{
    df_begin();
    df_std_two();
    g_autofree gchar *rx = df_rp("/mnt/x");
    df_mount(80, "0:70", "/srv", rx);
    gboolean drop, was;
    GError *err = NULL;
    g_assert_true(df_run("/mnt/x", &drop, &was, &err));
    g_assert_no_error(err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_cmpint(was, ==, TRUE);
    g_assert_false(df_alive(80));
    g_assert_true(df_alive(70) && df_alive(71) && df_alive(72) && df_alive(61) && df_alive(62));
    g_autofree gchar *nsd = g_strdup("nsd " PCV_LXC_VOLUME_GUARD_HOOK
                                     " container volume-ns-detach 100 /lxc.payload.c1 0:70 /srv mnt/x 0:50 /vol mnt/a 0:51 /ob mnt/b");
    g_assert_true(vf_logged(nsd));
    g_assert_cmpuint(df_count("read /proc/100/mountinfo"), ==, 2);
    df_end();

    df_begin();
    df_std_two();
    g_autofree gchar *rx2 = df_rp("/mnt/x");
    df_mount(80, "0:70", "/srv", rx2);
    vf_thread(300, 300, CLEAN "99 20 0:70 /srv/deep /opt/x rw - zfs pool/s rw\n");                     
    g_assert_false(df_run("/mnt/x", &drop, &was, &err));
    df_assert_conflict(err, "volume is still mounted inside the container: /mnt/x; stop the container, then detach again");
    g_clear_error(&err);
    g_assert_cmpint(drop, ==, FALSE);
    df_end();
}

                                                                                     
static void
test_run_verify_fail_keeps_staging(void)
{
    df_begin();
    df_std_two();
    vf_thread(200, 200, CLEAN COPY_AT_A);
    gboolean drop, was;
    GError *err = NULL;
    g_assert_false(df_run("/mnt/a", &drop, &was, &err));
    df_assert_conflict(err, "volume is still mounted inside the container: /mnt/a; stop the container, then detach again");
    g_clear_error(&err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_true(df_alive(61));                                               
    g_autofree gchar *sa = df_slot("mnt/a", 0);
    g_autofree gchar *um = g_strdup_printf("umount %s", sa), *pv = g_strdup_printf("private %s", sa);
    g_assert_false(vf_logged(um));
    g_assert_false(vf_logged(pv));
    g_assert_cmpuint(df_count("read /proc/100/mountinfo"), ==, 1);
    df_end();
}

                                                                                                 
                                  
static void
test_run_reverify_catches_reboot_copy(void)
{
    df_begin();
    df_std_two();
    D.reboot_copy = TRUE;
    gboolean drop, was;
    GError *err = NULL;
    g_assert_false(df_run("/mnt/a", &drop, &was, &err));
    df_assert_conflict(err, "volume is still mounted inside the container: /mnt/a; stop the container, then detach again");
    g_clear_error(&err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_false(df_alive(61));                                                               
    g_assert_cmpuint(df_count("read /proc/100/mountinfo"), ==, 2);
    df_end();
}

                                                                                                                           
                                                  
static void
test_run_staging_umount_fail_internal(void)
{
    df_begin();
    df_std_two();
    D.umount_fail_id = 61;
    gboolean drop, was;
    GError *err = NULL;
    g_assert_false(df_run("/mnt/a", &drop, &was, &err));
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
    g_assert_true(g_str_has_prefix(err->message, "volume detached from the running container, but its staging mount was not removed: "));
    g_assert_true(g_str_has_suffix(err->message, "; run detach again"));
    g_assert_nonnull(strstr(err->message, "1 staging mount(s) could not be removed"));
    g_clear_error(&err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_true(df_alive(61));
    g_assert_cmpuint(df_count("read /proc/100/mountinfo"), ==, 1);
    df_end();
}

                                                                                                 
                                                                                                
static void
test_run_ambiguous_host_mount_conflict(void)
{
    df_begin();
    df_config("lxc.mount.entry = /data mnt/a none bind 0 0\nlxc.mount.entry = /data mnt/b none bind 0 0\n");
    g_autofree gchar *sa = df_slot("mnt/a", 0), *sb = df_slot("mnt/b", 0), *ra = df_rp("/mnt/a"), *rc = df_rp("/mnt/c");
    df_mount(61, "0:50", "/vol", sa);
    df_mount(62, "0:50", "/vol", sb);
    df_mount(70, "0:50", "/vol", ra);
    df_mount(75, "0:50", "/vol", rc);
    gboolean drop, was;
    GError *err = NULL;
    g_assert_false(df_run("/mnt/a", &drop, &was, &err));
    df_assert_conflict(err, "the same host path is attached twice to this container; stop the container, then detach again");
    g_clear_error(&err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_cmpuint(df_count("umount "), ==, 0);
    g_assert_cmpuint(df_count("private "), ==, 0);
    g_assert_true(df_alive(70) && df_alive(75));
    g_assert_cmpuint(D.nsd_calls, ==, 0);
    df_end();
}

                                                                                                       
static void
df_capture(const gchar *dom, GLogLevelFlags lvl, const gchar *msg, gpointer user)
{
    (void)dom;
    (void)lvl;
    g_ptr_array_add(user, g_strdup(msg));
}

                                                                        
static guint
df_warned(GPtrArray *got, const gchar *pattern)
{
    guint n = 0;
    for (guint i = 0; i < got->len; i++)
        n += g_pattern_match_simple(pattern, g_ptr_array_index(got, i));
    return n;
}

                                                                                           
static void
test_run_ns_child_failure_still_verifies(void)
{
    GPtrArray *got = g_ptr_array_new_with_free_func(g_free);
    guint h = g_log_set_handler(NULL, G_LOG_LEVEL_WARNING, df_capture, got);
    df_begin();
    df_std_two();
    D.nsd_ok = FALSE;
    gboolean drop, was;
    GError *err = NULL;
    g_assert_true(df_run("/mnt/a", &drop, &was, &err));
    g_assert_no_error(err);
    g_assert_cmpint(drop, ==, TRUE);
    g_assert_cmpuint(D.nsd_calls, ==, 1);
    g_assert_cmpuint(df_warned(got, "*container 'c1'*namespace detach*fake: timed out*final check*"), ==, 1);
    g_assert_cmpuint(df_count("read /proc/100/mountinfo"), ==, 2);                             
    df_end();

    g_ptr_array_set_size(got, 0);
    df_begin();
    df_std_two();
    D.nsd_ok = FALSE;
    vf_thread(100, 100, CLEAN COPY_AT_A);
    g_assert_false(df_run("/mnt/a", &drop, &was, &err));
    df_assert_conflict(err, "volume is still mounted inside the container: /mnt/a; stop the container, then detach again");
    g_clear_error(&err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_cmpuint(df_warned(got, "*container 'c1'*namespace detach*"), ==, 1);
    df_end();
    g_log_remove_handler(NULL, h);
    g_ptr_array_unref(got);
}

                                                                                                  
                                                                                                       
static void
test_run_nested_volume_conflict(void)
{
    df_begin();
    df_config("lxc.mount.entry = /data mnt/a none bind 0 0\nlxc.mount.entry = /data2 /mnt/a//sub/ none bind 0 0\n");
    g_autofree gchar *sa = df_slot("mnt/a", 0), *ss = df_slot("/mnt/a//sub/", 0), *ra = df_rp("/mnt/a");
    df_mount(61, "0:50", "/vol", sa);
    df_mount(63, "0:52", "/v2", ss);
    df_mount(70, "0:50", "/vol", ra);
    gboolean drop, was;
    GError *err = NULL;
    g_assert_false(df_run("/mnt/a", &drop, &was, &err));
    df_assert_conflict(err, "another attached volume is mounted under this path: /mnt/a/sub; stop the container, then detach again");
    g_clear_error(&err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_cmpuint(df_count("umount "), ==, 0);
    g_assert_cmpuint(D.nsd_calls, ==, 0);
    g_assert_true(df_alive(70));
    df_end();

    df_begin();
    df_config("lxc.mount.entry = /data mnt/a none bind 0 0\nlxc.mount.entry = /data2 mnt/ab none bind 0 0\n");
    g_autofree gchar *sa2 = df_slot("mnt/a", 0), *sab = df_slot("mnt/ab", 0);
    df_mount(61, "0:50", "/vol", sa2);
    df_mount(63, "0:52", "/v2", sab);
    g_assert_true(df_run("/mnt/a", &drop, &was, &err));
    g_assert_no_error(err);
    g_assert_cmpint(drop, ==, TRUE);
    df_end();
}

                                                                                                        
                                                         
static void
test_run_other_unidentified_conflict(void)
{
    df_begin();
    df_config("lxc.mount.entry = /data mnt/a none bind 0 0\nlxc.mount.entry = /gone mnt/b none bind 0 0\n");
    g_autofree gchar *sa = df_slot("mnt/a", 0), *ra = df_rp("/mnt/a");
    df_mount(61, "0:50", "/vol", sa);
    df_mount(70, "0:50", "/vol", ra);
    gboolean drop, was;
    GError *err = NULL;
    g_assert_false(df_run("/mnt/a", &drop, &was, &err));
    df_assert_conflict(err, "cannot identify the host source of another attached volume: /mnt/b; stop the container, then detach again");
    g_clear_error(&err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_cmpuint(df_count("umount "), ==, 0);
    g_assert_cmpuint(D.nsd_calls, ==, 0);
    g_assert_true(df_alive(70));
    df_end();
}

                                                                                                
                                                  
static void
test_unmount_attributed_file_volume(void)
{
    df_begin();
    const gchar *cfg = "lxc.mount.entry = /cfg/app.conf etc/app.conf none bind,create=file 0 0\n";
    g_autofree gchar *sf = df_slot("etc/app.conf", 0), *rf = df_rp("/etc/app.conf");
    df_mount(61, "0:50", "/cfg/app.conf", sf);
    df_mount(70, "0:50", "/cfg/app.conf", rf);
    g_hash_table_add(D.files, g_strdup(sf));
    g_hash_table_add(D.files, g_strdup(rf));
    GError *err = NULL;
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load_running(D.lxc, "c1", cfg, "/etc/app.conf", &err);
    g_assert_no_error(err);
    g_assert_nonnull(set);
    g_assert_true(pcv_lxc_volume_unmount_attributed(D.lxc, "c1", set, "/etc/app.conf", &err));
    g_assert_no_error(err);
    g_assert_false(df_alive(70));
    g_assert_true(df_alive(61));                                                
    g_assert_cmpuint(df_count("walk "), ==, 0);
    pcv_lxc_volume_set_free(set);
    df_end();
}

                                                                                                
                                                         
static void
test_staging_unmount_file_slot(void)
{
    df_begin();
    g_autofree gchar *s0 = df_slot("etc/app.conf", 0), *s1 = df_slot("etc/app.conf", 1), *sb = df_slot("mnt/b", 0);
    df_mount(61, "0:50", "/cfg/app.conf", s0);
    df_mount(62, "0:50", "/cfg/other.conf", s1);
    df_mount(63, "0:51", "/ob", sb);
    g_hash_table_add(D.files, g_strdup(s0));
    g_hash_table_add(D.files, g_strdup(s1));
    g_autofree gchar *names = g_strdup_printf("%s %s %s", strrchr(s0, '/') + 1, strrchr(s1, '/') + 1, strrchr(sb, '/') + 1);
    df_dir(D.stage, names);
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_staging_unmount("c1", "etc/app.conf", &err));
    g_assert_no_error(err);
    g_assert_false(df_alive(61) || df_alive(62));
    g_assert_true(df_alive(63));
    g_assert_cmpuint(df_count("walk "), ==, 0);
    g_assert_cmpuint(df_count("remove "), ==, 0);
    df_end();
}

                                                                                                            
                                                                  
static void
test_stopped_clears_residue_then_drop(void)
{
    df_begin();
    df_std_two();
    D.state = PCV_LXC_STATE_STOPPED;
    gboolean drop, was;
    GError *err = NULL;
    g_assert_true(df_run("/mnt/a", &drop, &was, &err));
    g_assert_no_error(err);
    g_assert_cmpint(drop, ==, TRUE);
    g_assert_cmpint(was, ==, FALSE);
    g_assert_false(df_alive(61) || df_alive(62) || df_alive(70) || df_alive(71) || df_alive(72));
    g_autofree gchar *sa = df_slot("mnt/a", 0), *sb = df_slot("mnt/b", 0);
    g_autofree gchar *rma = g_strdup_printf("remove %s", sa), *rmb = g_strdup_printf("remove %s", sb);
    g_assert_true(vf_logged(rma) && vf_logged(rmb) && vf_logged("remove " VS_ROOT "/c1"));
    g_assert_cmpuint(D.nsd_calls, ==, 0);
    g_assert_cmpuint(df_count("read /proc/"), ==, 0);
    df_end();

    df_begin();
    df_std_two();
    D.state = PCV_LXC_STATE_STOPPED;
    D.private_fail_id = 70;
    g_assert_false(df_run("/mnt/a", &drop, &was, &err));
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
    g_assert_cmpstr(err->message, ==, "1 volume mount(s) left under the container rootfs could not be removed");
    g_clear_error(&err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_cmpint(was, ==, FALSE);
    df_end();
}

                                                                                                           
                                                                                            
                                             
static void
test_config_drop_failure_internal(void)
{
    df_begin();
    const gchar *body = "lxc.uts.name = c1\nlxc.mount.entry = /data mnt/a none bind 0 0\nlxc.mount.entry = /other mnt/b none bind 0 0\n";
    df_config(body);
    g_autofree gchar *cfg = g_build_filename(D.lxc, "c1", "config", NULL), *hl = g_build_filename(D.tmp, "hardlink", NULL);
    g_assert_cmpint(link(cfg, hl), ==, 0);
    static const struct { gboolean running; const gchar *prefix; } rows[] = {
        { TRUE, "volume detached from the running container, but its config line was not removed: " },
        { FALSE, "volume mounts were removed, but the config line was not removed: " },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        GError *err = NULL;
        g_assert_false(pcv_lxc_volume_config_drop(D.lxc, "c1", "/mnt/a", rows[i].running, &err));
        g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
        g_assert_true(g_str_has_prefix(err->message, rows[i].prefix));
        g_assert_true(g_str_has_suffix(err->message, "; run detach again"));
        g_clear_error(&err);
        g_autofree gchar *now = NULL;
        g_assert_true(g_file_get_contents(cfg, &now, NULL, NULL));
        g_assert_cmpstr(now, ==, body);
    }
    g_assert_cmpint(unlink(hl), ==, 0);
    GError *err = NULL;
    g_assert_true(pcv_lxc_volume_config_drop(D.lxc, "c1", "/mnt/a/", TRUE, &err));
    g_assert_no_error(err);
    g_autofree gchar *after = NULL;
    g_assert_true(g_file_get_contents(cfg, &after, NULL, NULL));
    g_assert_cmpstr(after, ==, "lxc.uts.name = c1\nlxc.mount.entry = /other mnt/b none bind 0 0\n");
    struct stat st0, st1;
    g_assert_cmpint(stat(cfg, &st0), ==, 0);
    g_assert_false(pcv_lxc_volume_config_drop(D.lxc, "c1", "/mnt/a", FALSE, &err));                                         
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
    g_clear_error(&err);
    g_assert_cmpint(stat(cfg, &st1), ==, 0);
    g_assert_cmpuint(st0.st_ino, ==, st1.st_ino);
    df_end();
}

                                             
static gchar *
df_cfg_text(void)
{
    g_autofree gchar *cfg = g_build_filename(D.lxc, "c1", "config", NULL);
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(cfg, &text, NULL, NULL));
    return text;
}

                                                                                                                
                                                                                                      
static void
test_config_drop_slash_spellings(void)
{
    static const struct { const gchar *field2, *request; } rows[] = { { "mnt//a", "/mnt/a" }, { "mnt/a", "/mnt//a" }, { "/mnt//a/", "mnt/a" },
                                                                      { "mnt/./a", "/mnt/a" }, { "mnt/a", "/mnt/./a/" } };                                  
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        df_begin();
        g_autofree gchar *body = g_strdup_printf("lxc.uts.name = c1\nlxc.mount.entry = /data %s none bind 0 0\n"
                                                 "lxc.mount.entry = /other mnt/ab none bind 0 0\n", rows[i].field2);
        df_config(body);
        GError *err = NULL;
        g_test_message("field 2 '%s', request '%s'", rows[i].field2, rows[i].request);
        g_assert_true(pcv_lxc_volume_config_drop(D.lxc, "c1", rows[i].request, TRUE, &err));
        g_assert_no_error(err);
        g_autofree gchar *after = df_cfg_text();
        g_assert_cmpstr(after, ==, "lxc.uts.name = c1\nlxc.mount.entry = /other mnt/ab none bind 0 0\n");
        df_end();
    }
}

                                                                                                    
                                                                                
static void
test_config_drop_zero_removed_internal(void)
{
    df_begin();
    const gchar *body = "lxc.uts.name = c1\nlxc.mount.entry = /other mnt/b none bind 0 0\n";
    df_config(body);
    static const struct { gboolean running; const gchar *msg; } rows[] = {
        { TRUE, "volume detached from the running container, but its config line was not removed: the volume line was not found in the "
                "container config; run detach again" },
        { FALSE, "volume mounts were removed, but the config line was not removed: the volume line was not found in the container config; "
                 "run detach again" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        GError *err = NULL;
        g_assert_false(pcv_lxc_volume_config_drop(D.lxc, "c1", "/mnt/a", rows[i].running, &err));
        g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
        g_assert_cmpstr(err->message, ==, rows[i].msg);
        g_clear_error(&err);
        g_autofree gchar *after = df_cfg_text();
        g_assert_cmpstr(after, ==, body);
    }
    df_end();
}

                                                                                                                     
static void
df_slash_scenario(const gchar *field2, gboolean stopped)
{
    g_autofree gchar *body = g_strdup_printf("lxc.uts.name = c1\nlxc.mount.entry = /data %s none bind 0 0\n"
                                             "lxc.mount.entry = /other mnt/b none bind 0 0\n", field2);
    df_config(body);
    g_autofree gchar *sa = df_slot(field2, 0), *sb = df_slot("mnt/b", 0), *ra = df_rp("/mnt/a");
    df_mount(61, "0:50", "/vol", sa);
    df_mount(62, "0:51", "/ob", sb);
    df_mount(70, "0:50", "/vol", ra);
    g_autofree gchar *names = g_strdup_printf("%s %s", strrchr(sa, '/') + 1, strrchr(sb, '/') + 1);
    df_dir(VS_ROOT, "c1");
    df_dir(D.stage, names);
    if (stopped)
        D.state = PCV_LXC_STATE_STOPPED;
}

                                                                                                   
static gboolean
df_detach_and_drop(const gchar *cpath, gboolean *drop, GError **err)
{
    gboolean was = FALSE;
    return df_run(cpath, drop, &was, err) && (!*drop || pcv_lxc_volume_config_drop(D.lxc, "c1", cpath, was, err));
}

                                                                                                 
                                                                                              
                                                                                                                  
static void
test_run_slash_spellings_line_dropped(void)
{
    static const struct { const gchar *field2, *request; } rows[] = { { "mnt//a", "/mnt/a" }, { "mnt/a", "/mnt//a" },
                                                                      { "mnt/./a", "/mnt/a" }, { "mnt/a", "/mnt/./a" } };                                       
    for (guint stopped = 0; stopped < 2; stopped++) {
        for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
            df_begin();
            df_slash_scenario(rows[i].field2, stopped);
            gboolean drop = FALSE;
            GError *err = NULL;
            g_test_message("%s: field 2 '%s', request '%s'", stopped ? "stopped" : "running", rows[i].field2, rows[i].request);
            g_assert_true(df_detach_and_drop(rows[i].request, &drop, &err));
            g_assert_no_error(err);
            g_assert_cmpint(drop, ==, TRUE);
            g_autofree gchar *after = df_cfg_text();
            g_assert_cmpstr(after, ==, "lxc.uts.name = c1\nlxc.mount.entry = /other mnt/b none bind 0 0\n");
            g_assert_false(df_alive(61) || df_alive(70));                                           
            g_assert_cmpint(df_alive(62), ==, stopped ? FALSE : TRUE);                                  
            df_end();
        }
    }
}

                                                                                                      
                                                                         
static void
test_stopped_no_line_idempotent(void)
{
    df_begin();
    df_std_two();
    D.state = PCV_LXC_STATE_STOPPED;
    gboolean drop, was;
    GError *err = NULL;
    g_assert_true(df_run("/mnt/x", &drop, &was, &err));
    g_assert_no_error(err);
    g_assert_cmpint(drop, ==, FALSE);
    g_assert_cmpint(was, ==, FALSE);
    g_assert_false(df_alive(70));                                                 
    df_end();
}

                                                                                                           
                                                                                                             
static void
test_run_make_private_fail_no_umount(void)
{
    df_begin();
    df_std_two();
    D.private_fail_id = 70;
    gboolean drop, was;
    GError *err = NULL;
    g_assert_false(df_run("/mnt/a", &drop, &was, &err));
    df_assert_conflict(err, "volume is still mounted inside the container: /mnt/a; stop the container, then detach again");
    g_clear_error(&err);
    g_autofree gchar *ra = df_rp("/mnt/a");
    g_autofree gchar *pv = g_strdup_printf("private %s", ra), *um = g_strdup_printf("umount %s", ra);
    g_assert_true(vf_logged(pv));
    g_assert_false(vf_logged(um));
    g_assert_true(df_alive(70));
    g_assert_cmpuint(D.nsd_calls, ==, 0);
    df_end();

    df_begin();
    df_std_two();
    D.private_fail_id = 61;
    g_assert_false(df_run("/mnt/a", &drop, &was, &err));
    g_assert_error(err, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL);
    g_assert_nonnull(strstr(err->message, "staging mount was not removed: 1 staging mount(s) could not be removed: mount_setattr private"));
    g_clear_error(&err);
    g_autofree gchar *sa = df_slot("mnt/a", 0);
    g_autofree gchar *pv2 = g_strdup_printf("private %s", sa), *um2 = g_strdup_printf("umount %s", sa);
    g_assert_true(vf_logged(pv2));
    g_assert_false(vf_logged(um2));
    g_assert_true(df_alive(61));
    g_assert_cmpint(drop, ==, FALSE);
    df_end();
}

void test_lxc_volume_detach_register(void);

void
test_lxc_volume_detach_register(void)
{
    g_test_add_func("/lxc_volume_detach/verify_clean_passes", test_verify_clean_passes);
    g_test_add_func("/lxc_volume_detach/verify_other_pid_ns_copy_fails", test_verify_other_pid_ns_copy_fails);
    g_test_add_func("/lxc_volume_detach/verify_other_thread_ns_copy_fails", test_verify_other_thread_ns_copy_fails);
    g_test_add_func("/lxc_volume_detach/verify_rename_mount_at_path_fails", test_verify_rename_mount_at_path_fails);
    g_test_add_func("/lxc_volume_detach/verify_ambiguous_counts_as_target", test_verify_ambiguous_counts_as_target);
    g_test_add_func("/lxc_volume_detach/verify_overlap_keeps_other_line", test_verify_overlap_keeps_other_line);
    g_test_add_func("/lxc_volume_detach/verify_skip_errno_table", test_verify_skip_errno_table);
    g_test_add_func("/lxc_volume_detach/verify_task_limit_fails", test_verify_task_limit_fails);
    g_test_add_func("/lxc_volume_detach/verify_cgroup_limit_fails", test_verify_cgroup_limit_fails);
    g_test_add_func("/lxc_volume_detach/verify_large_mountinfo", test_verify_large_mountinfo);
    g_test_add_func("/lxc_volume_detach/verify_long_copy_root_fails", test_verify_long_copy_root_fails);
    g_test_add_func("/lxc_volume_detach/verify_long_ident_root_fails", test_verify_long_ident_root_fails);
    g_test_add_func("/lxc_volume_detach/verify_target_without_ident_fails", test_verify_target_without_ident_fails);
    g_test_add_func("/lxc_volume_detach/verify_budget_exceeded_after_files", test_verify_budget_exceeded_after_files);
    g_test_add_func("/lxc_volume_detach/verify_budget_exceeded_mid_file", test_verify_budget_exceeded_mid_file);
    g_test_add_func("/lxc_volume_detach/verify_budget_large_mountinfo_normal", test_verify_budget_large_mountinfo_normal);
    g_test_add_func("/lxc_volume_detach/real_ops_tmpdir", test_real_ops_tmpdir);
    g_test_add_func("/lxc_volume_detach/ns_detach_argv", test_ns_detach_argv);
    g_test_add_func("/lxc_volume_detach/ns_detach_seam", test_ns_detach_seam);
    g_test_add_func("/lxc_volume_detach/run_order", test_run_order);
    g_test_add_func("/lxc_volume_detach/run_keeps_empty_slot_dir", test_run_keeps_empty_slot_dir);
    g_test_add_func("/lxc_volume_detach/run_changing_state", test_run_changing_state);
    g_test_add_func("/lxc_volume_detach/run_unidentified_source", test_run_unidentified_source);
    g_test_add_func("/lxc_volume_detach/run_was_running_output", test_run_was_running_output);
    g_test_add_func("/lxc_volume_detach/run_no_line_no_mount_ok", test_run_no_line_no_mount_ok);
    g_test_add_func("/lxc_volume_detach/run_no_line_host_mount_uses_mount_ident", test_run_no_line_host_mount_uses_mount_ident);
    g_test_add_func("/lxc_volume_detach/run_verify_fail_keeps_staging", test_run_verify_fail_keeps_staging);
    g_test_add_func("/lxc_volume_detach/run_reverify_catches_reboot_copy", test_run_reverify_catches_reboot_copy);
    g_test_add_func("/lxc_volume_detach/run_staging_umount_fail_internal", test_run_staging_umount_fail_internal);
    g_test_add_func("/lxc_volume_detach/run_ambiguous_host_mount_conflict", test_run_ambiguous_host_mount_conflict);
    g_test_add_func("/lxc_volume_detach/run_ns_child_failure_still_verifies", test_run_ns_child_failure_still_verifies);
    g_test_add_func("/lxc_volume_detach/run_nested_volume_conflict", test_run_nested_volume_conflict);
    g_test_add_func("/lxc_volume_detach/run_other_unidentified_conflict", test_run_other_unidentified_conflict);
    g_test_add_func("/lxc_volume_detach/unmount_attributed_file_volume", test_unmount_attributed_file_volume);
    g_test_add_func("/lxc_volume_detach/staging_unmount_file_slot", test_staging_unmount_file_slot);
    g_test_add_func("/lxc_volume_detach/stopped_clears_residue_then_drop", test_stopped_clears_residue_then_drop);
    g_test_add_func("/lxc_volume_detach/config_drop_failure_internal", test_config_drop_failure_internal);
    g_test_add_func("/lxc_volume_detach/config_drop_slash_spellings", test_config_drop_slash_spellings);
    g_test_add_func("/lxc_volume_detach/config_drop_zero_removed_internal", test_config_drop_zero_removed_internal);
    g_test_add_func("/lxc_volume_detach/run_slash_spellings_line_dropped", test_run_slash_spellings_line_dropped);
    g_test_add_func("/lxc_volume_detach/stopped_no_line_idempotent", test_stopped_no_line_idempotent);
    g_test_add_func("/lxc_volume_detach/run_make_private_fail_no_umount", test_run_make_private_fail_no_umount);
}
