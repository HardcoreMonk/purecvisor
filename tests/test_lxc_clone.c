                                                                             
                         
                                                                                                                                                       
  
                           
                                                                                                         
                                                                                                  
                                                                                       
                                                                                                        
                                                                                    
                                                                                     
  
                                                                           
                                    
                      
                                                                                
#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "modules/lxc/lxc_clone.h"

#define LP  "/v/lxc"                                          
#define LPC "/real/v/lxc"                                     

                            
static void
_rand_112233(guint8 *buf, gsize n)
{
    static const guint8 v[3] = { 0x11, 0x22, 0x33 };
    for (gsize i = 0; i < n; i++)
        buf[i] = v[i % 3];
}

                                                             
static gchar *
_rewrite(const gchar *text)
{
    GError *e = NULL;
    gchar *out = pcv_lxc_clone_config_text(text, LP, LPC, "a", "b", _rand_112233, &e);
    g_assert_no_error(e);
    g_assert_nonnull(out);
    return out;
}

static const gchar *RF = "lxc.rootfs.path = dir:" LP "/a/rootfs\n";

static void
test_config_rootfs_path(void)
{
    g_autofree gchar *o1 = _rewrite("lxc.rootfs.path = dir:/v/lxc/a/rootfs\n");
    g_assert_cmpstr(o1, ==, "lxc.rootfs.path = dir:/v/lxc/b/rootfs\nlxc.uts.name = b\n");

                                 
    g_autofree gchar *o2 = _rewrite("lxc.rootfs.path = /v/lxc/a/rootfs\n");
    g_assert_cmpstr(o2, ==, "lxc.rootfs.path = /v/lxc/b/rootfs\nlxc.uts.name = b\n");

                                
    g_autofree gchar *o3 = _rewrite("lxc.rootfs.path = dir:/real/v/lxc/a/rootfs\n");
    g_assert_cmpstr(o3, ==, "lxc.rootfs.path = dir:/real/v/lxc/b/rootfs\nlxc.uts.name = b\n");

                                    
    g_autofree gchar *o4 = _rewrite("lxc.rootfs.path = dir:/v//lxc/./a/rootfs\n");
    g_assert_cmpstr(o4, ==, "lxc.rootfs.path = dir:/v/lxc/b/rootfs\nlxc.uts.name = b\n");

                                                                      
    const gchar *bad[] = {
        "lxc.rootfs.path = dir:/other/rootfs\n",
        "lxc.rootfs.path = /other/rootfs\n",
        "lxc.rootfs.path = overlay:/v/lxc/a/rootfs:/x\n",
        "lxc.rootfs.path = dir:/v/lxc/c/rootfs\n",
        "lxc.rootfs.path = dir:/v/lxc/a/x/../rootfs\n",
        "lxc.rootfs.path = dir:/v/lxc/a/rootfs/sub\n",
        "lxc.uts.name = a\n",
        NULL
    };
    for (gsize i = 0; bad[i]; i++) {
        GError *e = NULL;
        gchar *out = pcv_lxc_clone_config_text(bad[i], LP, LPC, "a", "b", _rand_112233, &e);
        g_assert_null(out);
        g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&e);
    }
}

static void
test_config_utsname(void)
{
    g_autofree gchar *in = g_strdup_printf("%slxc.uts.name = a\nlxc.net.0.type = veth\nlxc.utsname = a\nlxc.uts.name=zz\n", RF);
    g_autofree gchar *o = _rewrite(in);
    g_assert_cmpstr(o, ==, "lxc.rootfs.path = dir:/v/lxc/b/rootfs\nlxc.net.0.type = veth\nlxc.uts.name = b\n");
                               
    g_autofree gchar *o2 = _rewrite("lxc.rootfs.path = dir:/v/lxc/a/rootfs");
    g_assert_cmpstr(o2, ==, "lxc.rootfs.path = dir:/v/lxc/b/rootfs\nlxc.uts.name = b\n");
}

static void
test_config_hwaddr(void)
{
    const gchar *vals[] = { "00:16:3e:xx:xx:xx", "00:16:3e:aa:bb:cc", "52:54:00:01:02:03", NULL };
    for (gsize i = 0; vals[i]; i++) {
        g_autofree gchar *in = g_strdup_printf("%slxc.net.0.hwaddr = %s\n", RF, vals[i]);
        g_autofree gchar *o = _rewrite(in);
        g_assert_cmpstr(o, ==, "lxc.rootfs.path = dir:/v/lxc/b/rootfs\nlxc.net.0.hwaddr = 00:16:3e:11:22:33\nlxc.uts.name = b\n");
    }
                                
    g_autofree gchar *o2 = _rewrite("lxc.rootfs.path = dir:/v/lxc/a/rootfs\nlxc.network.hwaddr = 00:16:3e:xx:xx:xx\n"
                                    "lxc.net.12.hwaddr=00:16:3e:aa:bb:cc\n");
    g_assert_cmpstr(o2, ==, "lxc.rootfs.path = dir:/v/lxc/b/rootfs\nlxc.network.hwaddr = 00:16:3e:11:22:33\n"
                            "lxc.net.12.hwaddr=00:16:3e:11:22:33\nlxc.uts.name = b\n");                         
                        
    g_autofree gchar *in3 = g_strdup_printf("%slxc.net.0.hwaddr = 00:16:3e:aa:bb\nlxc.net.1.hwaddr = \n", RF);
    g_autofree gchar *o3 = _rewrite(in3);
    g_assert_cmpstr(o3, ==, "lxc.rootfs.path = dir:/v/lxc/b/rootfs\nlxc.net.0.hwaddr = 00:16:3e:aa:bb\nlxc.net.1.hwaddr = \nlxc.uts.name = b\n");
                                                  
    GError *e = NULL;
    g_autofree gchar *in4 = g_strdup_printf("%slxc.net.0.hwaddr = 00:16:3e:xx:xx:xx\n", RF);
    g_autofree gchar *o4 = pcv_lxc_clone_config_text(in4, LP, LPC, "a", "b", NULL, &e);
    g_assert_no_error(e);
    g_assert_nonnull(o4);
    g_assert_true(g_str_has_prefix(o4, "lxc.rootfs.path = dir:/v/lxc/b/rootfs\nlxc.net.0.hwaddr = 00:16:3e:"));
    g_assert_true(g_regex_match_simple("hwaddr = 00:16:3e:[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}\n", o4, 0, 0));
}

static void
test_config_hook_fstab_paths(void)
{
    g_autofree gchar *in = g_strdup_printf("%slxc.hook.pre-start = /v/lxc/a/hooks/x\nlxc.hook.start-host = /real/v/lxc/a/hooks/y\n"
                                           "lxc.mount.fstab = /v/lxc/a/fstab\n"
                                           "lxc.hook.pre-mount = /usr/local/bin/pcvctl container volume-guard-hook\n"
                                           "lxc.hook.mount = /v/lxc/ab/x\nlxc.hook.post-stop = /v/lxc/a\n", RF);
    g_autofree gchar *o = _rewrite(in);
    g_assert_cmpstr(o, ==,
        "lxc.rootfs.path = dir:/v/lxc/b/rootfs\nlxc.hook.pre-start = /v/lxc/b/hooks/x\nlxc.hook.start-host = /real/v/lxc/b/hooks/y\n"
        "lxc.mount.fstab = /v/lxc/b/fstab\n"
        "lxc.hook.pre-mount = /usr/local/bin/pcvctl container volume-guard-hook\n"
        "lxc.hook.mount = /v/lxc/ab/x\nlxc.hook.post-stop = /v/lxc/a\nlxc.uts.name = b\n");
}

                                                                
static void
test_config_untouched_lines(void)
{
    const gchar *keep[] = {
        "lxc.mount.entry = /v/lxc/a/data mnt/data none bind,create=dir 0 0",                                          
        "lxc.idmap = u 0 1000000 65536",
        "lxc.idmap = g 0 1000000 65536",
        "lxc.net.0.link = br0",
        "lxc.net.0.type = veth",
        "lxc.net.0.name = eth0",
        "lxc.cgroup2.memory.max = 536870912",
        "lxc.cgroup2.pids.max = 512",
        "lxc.environment = HOSTNAME=a",
        "lxc.init.uid = 0",
        "lxc.apparmor.profile = unchanged",
        "# lxc.uts.name = a",
        "#lxc.net.0.hwaddr = 00:16:3e:aa:bb:cc",
        "  # lxc.hook.clone = /v/lxc/a/h",
        "",
        "   ",
        "lxc.start.auto = 1\r",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(keep); i++) {
                                                        
        g_autofree gchar *in = g_strdup_printf("%s%s\nlxc.tty.max = 4\n", RF, keep[i]);
        g_autofree gchar *o = _rewrite(in);
        g_autofree gchar *want = g_strdup_printf("lxc.rootfs.path = dir:/v/lxc/b/rootfs\n%s\nlxc.tty.max = 4\nlxc.uts.name = b\n", keep[i]);
        g_assert_cmpstr(o, ==, want);
    }
                                                  
    GString *in = g_string_new(RF);
    GString *want = g_string_new("lxc.rootfs.path = dir:/v/lxc/b/rootfs\n");
    for (gsize i = 0; i < G_N_ELEMENTS(keep); i++) {
        g_string_append_printf(in, "%s\n", keep[i]);
        g_string_append_printf(want, "%s\n", keep[i]);
    }
    g_string_append(want, "lxc.uts.name = b\n");
    g_autofree gchar *o = _rewrite(in->str);
    g_assert_cmpstr(o, ==, want->str);
    g_string_free(in, TRUE);
    g_string_free(want, TRUE);
}

static void
test_has_clone_hook(void)
{
    g_assert_true(pcv_lxc_clone_has_clone_hook("lxc.hook.clone = /v/lxc/a/h\n"));
    g_assert_true(pcv_lxc_clone_has_clone_hook("lxc.uts.name = a\n\t lxc.hook.clone=/x\nlxc.tty.max = 4"));
    g_assert_false(pcv_lxc_clone_has_clone_hook("# lxc.hook.clone = /x\n"));
    g_assert_false(pcv_lxc_clone_has_clone_hook("  #lxc.hook.clone = /x\nlxc.hook.pre-start = /y\n"));
    g_assert_false(pcv_lxc_clone_has_clone_hook("lxc.hook.clone-extra = /x\nlxc.hook.cloned = /x\n"));
    g_assert_false(pcv_lxc_clone_has_clone_hook(""));
    g_assert_false(pcv_lxc_clone_has_clone_hook(NULL));
}

                                                                                 

typedef struct { gchar *root; int fd; } Rfs;

static Rfs
_rfs_new(gboolean mk_etc)
{
    Rfs r;
    r.root = g_dir_make_tmp("pcv-clone-XXXXXX", NULL);
    g_assert_nonnull(r.root);
    if (mk_etc) {
        g_autofree gchar *etc = g_build_filename(r.root, "etc", NULL);
        g_assert_cmpint(g_mkdir(etc, 0755), ==, 0);
    }
    r.fd = open(r.root, O_PATH | O_DIRECTORY | O_CLOEXEC);
    g_assert_cmpint(r.fd, >=, 0);
    return r;
}

static void
_rfs_free(Rfs *r)
{
    close(r->fd);
    g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", r->root);
    g_assert_cmpint(system(cmd), ==, 0);
    g_free(r->root);
}

static gchar *
_rpath(const Rfs *r, const gchar *rel)
{
    return g_build_filename(r->root, rel, NULL);
}

static void
test_hostname_regular_file(void)
{
    Rfs r = _rfs_new(TRUE);
    g_autofree gchar *p = _rpath(&r, "etc/hostname");
    g_assert_true(g_file_set_contents(p, "a\n", -1, NULL));
    g_assert_cmpint(g_chmod(p, 0640), ==, 0);
    struct stat before;
    g_assert_cmpint(stat(p, &before), ==, 0);
    g_assert_true(pcv_lxc_clone_write_hostname(r.fd, "b"));
    g_autofree gchar *c = NULL;
    gsize len = 0;
    g_assert_true(g_file_get_contents(p, &c, &len, NULL));
    g_assert_cmpuint(len, ==, 1);                              
    g_assert_cmpstr(c, ==, "b");
    struct stat after;
    g_assert_cmpint(stat(p, &after), ==, 0);
    g_assert_cmpuint(after.st_uid, ==, before.st_uid);
    g_assert_cmpuint(after.st_gid, ==, before.st_gid);
    g_assert_cmpuint(after.st_mode, ==, before.st_mode);
    g_assert_cmpuint(after.st_ino, ==, before.st_ino);                            
                        
    g_assert_true(g_file_set_contents(p, "a-very-long-old-name\n", -1, NULL));
    g_assert_true(pcv_lxc_clone_write_hostname(r.fd, "bb"));
    g_autofree gchar *c2 = NULL;
    g_assert_true(g_file_get_contents(p, &c2, NULL, NULL));
    g_assert_cmpstr(c2, ==, "bb");
    _rfs_free(&r);
}

static void
test_hostname_fifo_no_block(void)
{
    Rfs r = _rfs_new(TRUE);
    g_autofree gchar *p = _rpath(&r, "etc/hostname");
    g_assert_cmpint(mkfifo(p, 0644), ==, 0);
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = pcv_lxc_clone_write_hostname(r.fd, "b");
    gint64 dt = g_get_monotonic_time() - t0;
    g_assert_false(ok);
    g_assert_cmpint(dt, <, 1 * G_USEC_PER_SEC);                                               
                                  
    int rd = open(p, O_RDONLY | O_NONBLOCK);
    g_assert_cmpint(rd, >=, 0);
    char buf[8];
    g_assert_cmpint(read(rd, buf, sizeof buf), <=, 0);
    close(rd);
    _rfs_free(&r);
}

                                                  
static void
_assert_false_and_content(const Rfs *r, const gchar *rel, const gchar *want)
{
    g_assert_false(pcv_lxc_clone_write_hostname(r->fd, "b"));
    if (want) {
        g_autofree gchar *p = _rpath(r, rel);
        g_autofree gchar *c = NULL;
        g_assert_true(g_file_get_contents(p, &c, NULL, NULL));
        g_assert_cmpstr(c, ==, want);
    }
}

static void
test_hostname_not_regular(void)
{
                                
    {
        Rfs r = _rfs_new(TRUE);
        g_autofree gchar *tgt = _rpath(&r, "target");
        g_autofree gchar *p = _rpath(&r, "etc/hostname");
        g_assert_true(g_file_set_contents(tgt, "orig\n", -1, NULL));
        g_assert_cmpint(symlink(tgt, p), ==, 0);
        _assert_false_and_content(&r, "target", "orig\n");
        _rfs_free(&r);
    }
                
    {
        Rfs r = _rfs_new(TRUE);
        g_autofree gchar *p = _rpath(&r, "etc/hostname");
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        g_assert_cmpint(s, >=, 0);
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        g_assert_cmpuint(strlen(p), <, sizeof sa.sun_path);
        strcpy(sa.sun_path, p);
        g_assert_cmpint(bind(s, (struct sockaddr *)&sa, sizeof sa), ==, 0);
        _assert_false_and_content(&r, "etc/hostname", NULL);
        close(s);
        _rfs_free(&r);
    }
              
    {
        Rfs r = _rfs_new(TRUE);
        g_autofree gchar *p = _rpath(&r, "etc/hostname");
        g_assert_cmpint(g_mkdir(p, 0755), ==, 0);
        _assert_false_and_content(&r, "etc/hostname", NULL);
        _rfs_free(&r);
    }
                                                  
    {
        Rfs r = _rfs_new(TRUE);
        g_autofree gchar *p = _rpath(&r, "etc/hostname");
        g_autofree gchar *other = _rpath(&r, "other");
        g_assert_true(g_file_set_contents(p, "a\n", -1, NULL));
        g_assert_cmpint(link(p, other), ==, 0);
        _assert_false_and_content(&r, "etc/hostname", "a\n");
        _assert_false_and_content(&r, "other", "a\n");
        _rfs_free(&r);
    }
                       
    {
        Rfs r = _rfs_new(TRUE);
        g_autofree gchar *p = _rpath(&r, "etc/hostname");
        g_assert_false(pcv_lxc_clone_write_hostname(r.fd, "b"));
        g_assert_false(g_file_test(p, G_FILE_TEST_EXISTS));
        _rfs_free(&r);
    }
                         
    {
        Rfs r = _rfs_new(FALSE);
        g_assert_false(pcv_lxc_clone_write_hostname(r.fd, "b"));
        _rfs_free(&r);
    }
}

                                                           
static void
test_hostname_etc_symlink(void)
{
    Rfs r = _rfs_new(FALSE);
    g_autofree gchar *outside = g_dir_make_tmp("pcv-clone-out-XXXXXX", NULL);
    g_assert_nonnull(outside);
    g_autofree gchar *oh = g_build_filename(outside, "hostname", NULL);
    g_assert_true(g_file_set_contents(oh, "host\n", -1, NULL));
    g_autofree gchar *etc = _rpath(&r, "etc");
    g_assert_cmpint(symlink(outside, etc), ==, 0);
    g_assert_false(pcv_lxc_clone_write_hostname(r.fd, "b"));
    g_autofree gchar *c = NULL;
    g_assert_true(g_file_get_contents(oh, &c, NULL, NULL));
    g_assert_cmpstr(c, ==, "host\n");
    g_unlink(oh);
    g_rmdir(outside);
    _rfs_free(&r);
}

                                                                                             
                                                                                           
                                                                                                          
                                                                                   
                                                                                   
                                       
#define WB        "tank/containers"
#define WDST      WB "/b"
#define WSRC_SNAP WB "/a@pcv-clone-b"
#define WDST_SNAP WB "/b@pcv-clone-b"

typedef struct {
    gboolean     running, dir_exists, list_ok;                                                           
    gboolean     residue_ok, snapshot_ok, pipe_ok, set_ok, mount_ok;
    const gchar *mounted;                                                           
    gboolean     dst_snap_ok, src_snap_ok, rewrite_ok, settle_ok, product_ok, stamp_ok, destroy_ok, rmdir_ok;
    const gchar *list_err;                                                                                        
    int          set_mkdir;                                                                                                           
} CloneCase;

static CloneCase  G_cc;
static GString   *G_clog;
static gchar     *G_clxc;                       

static CloneCase
_cc_ok(void)
{
    return (CloneCase){ .residue_ok = TRUE, .snapshot_ok = TRUE, .pipe_ok = TRUE, .set_ok = TRUE, .mount_ok = TRUE,
                        .mounted = "no\n", .dst_snap_ok = TRUE, .src_snap_ok = TRUE, .rewrite_ok = TRUE, .settle_ok = TRUE,
                        .product_ok = TRUE, .stamp_ok = TRUE, .destroy_ok = TRUE, .rmdir_ok = TRUE };
}

static gboolean
_cfail(GError **error, const gchar *what)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fake %s failed", what);
    return FALSE;
}

static void
_clog_argv(const gchar *tag, const gchar *const *argv)
{
    g_string_append(G_clog, tag);
    for (gsize i = 0; argv[i]; i++)
        g_string_append_printf(G_clog, " %s", argv[i]);
}

static gboolean
_f_run(const gchar *const *argv, gchar **out, GError **error)
{
    _clog_argv("run", argv);
    g_string_append_c(G_clog, '\n');
    const gchar *verb = argv[0] ? argv[1] : NULL;
    if (out)
        *out = NULL;
    if (g_strcmp0(verb, "list") == 0) {
        if (G_cc.list_ok)
            return TRUE;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "zfs: %s",
                    G_cc.list_err ? G_cc.list_err : "cannot open '" WDST "': dataset does not exist");
        return FALSE;
    }
    if (g_strcmp0(verb, "snapshot") == 0)
        return G_cc.snapshot_ok ? TRUE : _cfail(error, "snapshot");
    if (g_strcmp0(verb, "set") == 0) {
        if (G_cc.set_ok && G_cc.set_mkdir) {                                                                       
            g_autofree gchar *d = g_build_filename(G_clxc, "b", NULL);
            g_assert_cmpint(g_mkdir(d, 0711), ==, 0);
            if (G_cc.set_mkdir == 2) {
                g_autofree gchar *f = g_build_filename(d, "leftover", NULL);
                g_assert_true(g_file_set_contents(f, "x", -1, NULL));
            }
        }
        return G_cc.set_ok ? TRUE : _cfail(error, "set");
    }
    if (g_strcmp0(verb, "mount") == 0)
        return G_cc.mount_ok ? TRUE : _cfail(error, "mount");
    if (g_strcmp0(verb, "get") == 0) {
        if (!G_cc.mounted)
            return _cfail(error, "get");
        if (out)
            *out = g_strdup(G_cc.mounted);
        return TRUE;
    }
    return _cfail(error, "unexpected run");
}

static gboolean
_f_pipe(const gchar *const *producer, const gchar *const *consumer, GError **error)
{
    _clog_argv("pipe", producer);
    _clog_argv(" |", consumer);
    g_string_append_c(G_clog, '\n');
    return G_cc.pipe_ok ? TRUE : _cfail(error, "send/recv");
}

static gboolean
_f_is_running(const gchar *name)
{
    g_string_append_printf(G_clog, "is_running %s\n", name);
    return G_cc.running;
}

static gboolean
_f_dir_exists(const gchar *path)
{
    g_string_append_printf(G_clog, "dir_exists %s\n", path);
    return G_cc.dir_exists;
}

static gboolean
_f_residue(const gchar *lxc_path, const gchar *name, GError **error)
{
    g_string_append_printf(G_clog, "residue %s %s\n", lxc_path, name);
    return G_cc.residue_ok ? TRUE : _cfail(error, "residue");
}

static gboolean
_f_rewrite(const gchar *lxc_path, const gchar *dst, const gchar *src, GError **error)
{
    g_string_append_printf(G_clog, "rewrite %s %s %s\n", lxc_path, dst, src);
    return G_cc.rewrite_ok ? TRUE : _cfail(error, "rewrite");
}

static gboolean
_f_hostname(const gchar *lxc_path, const gchar *dst)
{
    g_string_append_printf(G_clog, "hostname %s %s\n", lxc_path, dst);
    return FALSE;                                            
}

static gboolean
_f_settle(const gchar *lxc_path, const gchar *dst, GError **error)
{
    g_string_append_printf(G_clog, "settle %s %s\n", lxc_path, dst);
    return G_cc.settle_ok ? TRUE : _cfail(error, "settle");
}

static gboolean
_f_product(const gchar *lxc_path, const gchar *src, const gchar *dst, GError **error)
{
    g_string_append_printf(G_clog, "product_files %s %s %s\n", lxc_path, src, dst);
    return G_cc.product_ok ? TRUE : _cfail(error, "product files");
}

static gboolean
_f_stamp(const gchar *dst, const gchar *subject)
{
    g_string_append_printf(G_clog, "stamp %s %s\n", dst, subject);
    return G_cc.stamp_ok;
}

static gboolean
_f_destroy_snap(const gchar *snapshot, GError **error)
{
    g_string_append_printf(G_clog, "destroy_snapshot %s\n", snapshot);
    gboolean ok = g_str_has_prefix(snapshot, WB "/a@") ? G_cc.src_snap_ok : G_cc.dst_snap_ok;
    return ok ? TRUE : _cfail(error, "destroy snapshot");
}

static gboolean
_f_destroy_ds(const gchar *dataset, GError **error)
{
    g_string_append_printf(G_clog, "destroy_dataset %s\n", dataset);
    return G_cc.destroy_ok ? TRUE : _cfail(error, "destroy dataset");
}

static gboolean
_f_remove_dir(const gchar *path, GError **error)
{
    g_string_append_printf(G_clog, "remove_dir %s\n", path);
    return G_cc.rmdir_ok ? TRUE : _cfail(error, "rmdir");
}

static const PcvLxcCloneOps G_cfake = {
    .run = _f_run, .pipe = _f_pipe, .is_running = _f_is_running, .dir_exists = _f_dir_exists,
    .residue_clear = _f_residue, .rewrite_config = _f_rewrite, .write_hostname = _f_hostname, .settle = _f_settle,
    .product_files = _f_product, .stamp_owner = _f_stamp, .destroy_snapshot = _f_destroy_snap, .destroy_dataset = _f_destroy_ds,
    .remove_dir = _f_remove_dir,
};

                                                                                        
static void
_cenv_new(const gchar *src_config)
{
    G_clxc = g_dir_make_tmp("pcv-clonew-XXXXXX", NULL);
    g_assert_nonnull(G_clxc);
    g_autofree gchar *a = g_build_filename(G_clxc, "a", NULL);
    g_assert_cmpint(g_mkdir(a, 0700), ==, 0);
    if (src_config) {
        g_autofree gchar *cfg = g_build_filename(a, "config", NULL);
        g_assert_true(g_file_set_contents(cfg, src_config, -1, NULL));
    }
}

static void
_cenv_free(void)
{
    g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", G_clxc);
    g_assert_cmpint(system(cmd), ==, 0);
    g_clear_pointer(&G_clxc, g_free);
}

                                                                      
static gboolean
_crun_ops(const CloneCase *cc, const PcvLxcCloneOps *ops, const gchar *subject, gboolean unpriv, gchar **log_out, GError **error)
{
    G_cc = *cc;
    G_clog = g_string_new(NULL);
    pcv_lxc_clone_set_ops_for_test(ops);
    gboolean ok = pcv_lxc_clone_run(G_clxc, WB, "a", "b", subject, unpriv, error);
    pcv_lxc_clone_set_ops_for_test(NULL);
    *log_out = g_string_free(g_steal_pointer(&G_clog), FALSE);
    g_assert_null(strstr(*log_out, "lxc-copy"));                                      
    return ok;
}

static gboolean
_crun(const CloneCase *cc, const gchar *subject, gboolean unpriv, gchar **log_out, GError **error)
{
    return _crun_ops(cc, &G_cfake, subject, unpriv, log_out, error);
}

                                                                         
static gchar *
_clog_head(void)
{
    return g_strdup_printf("is_running a\n"
                           "dir_exists %s/b\n"
                           "run zfs list -H -o name " WDST "\n"
                           "residue %s a\n"
                           "run zfs snapshot " WSRC_SNAP "\n"
                           "pipe zfs send " WSRC_SNAP " | zfs recv -u " WDST "\n"
                           "run zfs set mountpoint=%s/b " WDST "\n"
                           "run zfs get -H -o value mounted " WDST "\n", G_clxc, G_clxc, G_clxc);
}

#define WSRC_CONFIG_FMT "lxc.rootfs.path = dir:%s/a/rootfs\nlxc.uts.name = a\n"

                                                          
static void
_cenv_default(void)
{
    _cenv_new("lxc.uts.name = a\n");
}

static void
_assert_msg(const GError *e, const gchar *want)
{
    g_assert_nonnull(e);
    g_assert_cmpstr(e->message, ==, want);
}

static void
test_run_order(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_true(_crun(&cc, "sub", TRUE, &log, &e));
    g_assert_no_error(e);
    g_autofree gchar *head = _clog_head();
    g_autofree gchar *want = g_strdup_printf("%s"
                                             "run zfs mount " WDST "\n"
                                             "destroy_snapshot " WDST_SNAP "\n"
                                             "destroy_snapshot " WSRC_SNAP "\n"
                                             "rewrite %s b a\n"
                                             "hostname %s b\n"
                                             "settle %s b\n"
                                             "product_files %s a b\n"
                                             "stamp b sub\n", head, G_clxc, G_clxc, G_clxc, G_clxc);
    g_assert_cmpstr(log, ==, want);
    _cenv_free();
}

static void
test_mounted_yes_skips_mount(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.mounted = "yes\n";
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_true(_crun(&cc, "sub", TRUE, &log, &e));
    g_assert_no_error(e);
    g_assert_null(strstr(log, "run zfs mount"));                                                
    g_assert_nonnull(strstr(log, "destroy_snapshot " WDST_SNAP "\n"));
    _cenv_free();
}

                                                                                              
static void
_assert_rmdir(const gchar *log)
{
    g_autofree gchar *line = g_strdup_printf("remove_dir %s/b\n", G_clxc);
    const gchar *ds = strstr(log, "destroy_dataset " WDST "\n");
    const gchar *rm = strstr(log, line);
    if (!G_cc.destroy_ok) {
        g_assert_null(strstr(log, "remove_dir "));
        return;
    }
    g_assert_nonnull(ds);
    g_assert_nonnull(rm);
    g_assert_true(rm > ds);
    g_assert_null(strstr(rm + 1, "remove_dir "));
}

                                                               
static void
_assert_cleaned(const gchar *log, const GError *e, const gchar *want)
{
    _assert_msg(e, want);
    g_assert_nonnull(strstr(log, "destroy_dataset " WDST "\n"));
    g_assert_nonnull(strstr(log, "destroy_snapshot " WSRC_SNAP "\n"));
    _assert_rmdir(log);
}

static void
test_mounted_unreadable_fails(void)
{
    _cenv_default();
    const gchar *bad[] = { NULL, "", "maybe\n", NULL };                          
    for (gsize i = 0; i < 3; i++) {
        CloneCase cc = _cc_ok();
        cc.mounted = bad[i];
        g_autofree gchar *log = NULL;
        GError *e = NULL;
        g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
        g_assert_nonnull(e);
        g_assert_true(g_str_has_prefix(e->message, "clone 'b' failed at mount: "));
        g_assert_true(g_str_has_suffix(e->message, "; clone removed"));
        _assert_cleaned(log, e, e->message);
        g_assert_null(strstr(log, "run zfs mount"));
        g_assert_null(strstr(log, "rewrite "));
        g_clear_error(&e);
    }
                                                
    CloneCase cc = _cc_ok();
    cc.mount_ok = FALSE;
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    _assert_cleaned(log, e, "clone 'b' failed at mount: fake mount failed; clone removed");
    g_clear_error(&e);
    _cenv_free();
}

                                     
static void
_assert_nothing_created(const gchar *log)
{
    g_assert_null(strstr(log, "zfs snapshot"));
    g_assert_null(strstr(log, "destroy_"));
    g_assert_null(strstr(log, "pipe "));
}

static void
test_source_running_refused(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.running = TRUE;
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    _assert_msg(e, "stop the source container before cloning");
    g_assert_cmpstr(log, ==, "is_running a\n");
    g_clear_error(&e);
    _cenv_free();
}

static void
test_target_exists_refused(void)
{
    _cenv_default();
    for (int k = 0; k < 2; k++) {                                                    
        CloneCase cc = _cc_ok();
        if (k == 0)
            cc.dir_exists = TRUE;
        else
            cc.list_ok = TRUE;
        g_autofree gchar *log = NULL;
        GError *e = NULL;
        g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
        g_assert_error(e, G_IO_ERROR, G_IO_ERROR_EXISTS);
        _assert_msg(e, "clone target 'b' already exists");
        _assert_nothing_created(log);
        g_assert_null(strstr(log, "residue "));
        g_clear_error(&e);
    }
    _cenv_free();
}

static void
test_clone_hook_refused(void)
{
    _cenv_new("lxc.uts.name = a\nlxc.hook.clone = /v/lxc/a/h\n");
    CloneCase cc = _cc_ok();
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    _assert_msg(e, "source config has lxc.hook.clone, which purecvisor does not run; remove it before cloning");
    _assert_nothing_created(log);
    g_assert_null(strstr(log, "residue "));
    g_assert_nonnull(strstr(log, "run zfs list"));                                
    g_clear_error(&e);
    _cenv_free();
                            
    _cenv_new("# lxc.hook.clone = /v/lxc/a/h\n");
    g_autofree gchar *log2 = NULL;
    g_assert_true(_crun(&cc, "sub", TRUE, &log2, &e));
    g_assert_no_error(e);
    _cenv_free();
                                                   
    _cenv_new(NULL);
    g_autofree gchar *log3 = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log3, &e));
    g_assert_nonnull(e);
    g_assert_true(g_str_has_prefix(e->message, "cannot read the config of source container 'a': "));
    _assert_nothing_created(log3);
    g_assert_null(strstr(log3, "residue "));
    g_clear_error(&e);
    _cenv_free();
}

static void
test_residue_failure_creates_nothing(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.residue_ok = FALSE;
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    _assert_msg(e, "container 'a' has leftover volume mounts under its rootfs that could not be removed: fake residue failed; "
                   "nothing was cloned");
    _assert_nothing_created(log);
    g_clear_error(&e);
    _cenv_free();
}

static void
test_snapshot_exists_fails_without_cleanup(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.snapshot_ok = FALSE;                                                                         
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    _assert_msg(e, "clone 'b' failed at snapshot: fake snapshot failed; nothing was created");
    g_assert_nonnull(strstr(log, "run zfs snapshot " WSRC_SNAP "\n"));
    g_assert_null(strstr(log, "destroy_"));
    g_assert_null(strstr(log, "pipe "));
    g_clear_error(&e);
    _cenv_free();
}

static void
test_send_recv_failure_cleans(void)
{
    _cenv_default();
    struct { gboolean destroy_ok, src_snap_ok; const gchar *want; } rows[] = {
        { TRUE,  TRUE,  "clone 'b' failed at send/recv: fake send/recv failed; clone removed" },
        { FALSE, TRUE,  "clone 'b' failed at send/recv: fake send/recv failed; could not remove " WDST "; remove it manually" },
        { TRUE,  FALSE, "clone 'b' failed at send/recv: fake send/recv failed; could not remove " WSRC_SNAP "; remove it manually" },
        { FALSE, FALSE, "clone 'b' failed at send/recv: fake send/recv failed; could not remove " WDST " and " WSRC_SNAP
                        "; remove it manually" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(rows); i++) {
        CloneCase cc = _cc_ok();
        cc.pipe_ok = FALSE;
        cc.destroy_ok = rows[i].destroy_ok;
        cc.src_snap_ok = rows[i].src_snap_ok;
        g_autofree gchar *log = NULL;
        GError *e = NULL;
        g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
        _assert_cleaned(log, e, rows[i].want);
        g_assert_null(strstr(log, "zfs set"));
        g_clear_error(&e);
    }
                                                                
    CloneCase cc = _cc_ok();
    cc.pipe_ok = FALSE;
    cc.rmdir_ok = FALSE;
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    g_autofree gchar *want = g_strdup_printf("clone 'b' failed at send/recv: fake send/recv failed; could not remove %s/b: "
                                             "fake rmdir failed; remove it manually", G_clxc);
    _assert_cleaned(log, e, want);
    g_clear_error(&e);
    _cenv_free();
}

static void
test_source_snapshot_cleanup_warns(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.src_snap_ok = FALSE;                                                             
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_true(_crun(&cc, "sub", TRUE, &log, &e));
    g_assert_no_error(e);
    g_assert_nonnull(strstr(log, "stamp b sub\n"));
    g_assert_null(strstr(log, "destroy_dataset"));
                                                          
    cc.rewrite_ok = FALSE;
    g_autofree gchar *log2 = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log2, &e));
    _assert_msg(e, "clone 'b' failed at config rewrite: fake rewrite failed; could not remove " WSRC_SNAP "; remove it manually");
    const gchar *first = strstr(log2, "destroy_snapshot " WSRC_SNAP "\n");
    g_assert_nonnull(first);
    g_assert_nonnull(strstr(first + 1, "destroy_snapshot " WSRC_SNAP "\n"));
    g_clear_error(&e);
    _cenv_free();
}

static void
test_snapshot_cleanup_failure_cleans(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.dst_snap_ok = FALSE;                                                        
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    _assert_cleaned(log, e, "clone 'b' failed at snapshot cleanup: fake destroy snapshot failed; clone removed");
    g_assert_null(strstr(log, "rewrite "));
    g_clear_error(&e);
    _cenv_free();
}

                                                                 
static void
_assert_late_cleaned(const gchar *log, const GError *e, const gchar *want)
{
    _assert_msg(e, want);
    g_assert_nonnull(strstr(log, "destroy_dataset " WDST "\n"));
    const gchar *first = strstr(log, "destroy_snapshot " WSRC_SNAP "\n");
    g_assert_nonnull(first);
    g_assert_null(strstr(first + 1, "destroy_snapshot " WSRC_SNAP "\n"));
    _assert_rmdir(log);
}

static void
test_rewrite_failure_cleans(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.rewrite_ok = FALSE;
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    _assert_late_cleaned(log, e, "clone 'b' failed at config rewrite: fake rewrite failed; clone removed");
    g_assert_null(strstr(log, "hostname "));
    g_clear_error(&e);
    _cenv_free();
}

static void
test_settle_failure_cleans(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.settle_ok = FALSE;
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    _assert_late_cleaned(log, e, "clone 'b' failed at directory setup: fake settle failed; clone removed");
    g_assert_null(strstr(log, "product_files "));
    g_clear_error(&e);
                                                      
    g_autofree gchar *log2 = NULL;
    g_assert_true(_crun(&cc, "sub", FALSE, &log2, &e));
    g_assert_no_error(e);
    g_assert_null(strstr(log2, "settle "));
    _cenv_free();
}

static void
test_product_files_failure_cleans(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.product_ok = FALSE;                                                                   
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    _assert_late_cleaned(log, e, "clone 'b' failed at product files: fake product files failed; clone removed");
    g_assert_null(strstr(log, "stamp "));
    g_clear_error(&e);
    _cenv_free();
}

static void
test_owner_written(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_true(_crun(&cc, "user-42", TRUE, &log, &e));
    g_assert_no_error(e);
    g_autofree gchar *tail = g_strdup_printf("product_files %s a b\nstamp b user-42\n", G_clxc);
    g_assert_true(g_str_has_suffix(log, tail));                                            
    _cenv_free();
}

static void
test_owner_skipped_without_subject(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.stamp_ok = FALSE;                                                  
    const gchar *subjects[] = { NULL, "" };
    for (gsize i = 0; i < G_N_ELEMENTS(subjects); i++) {
        g_autofree gchar *log = NULL;
        GError *e = NULL;
        g_assert_true(_crun(&cc, subjects[i], TRUE, &log, &e));
        g_assert_no_error(e);
        g_assert_null(strstr(log, "stamp "));
        g_assert_nonnull(strstr(log, "product_files "));
    }
    _cenv_free();
}

static void
test_owner_failure_rolls_back(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.stamp_ok = FALSE;
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    _assert_late_cleaned(log, e, "clone 'b' failed at owner: cannot write the owner file; clone removed");
    g_clear_error(&e);
    _cenv_free();
}

                                                                       
static void
test_invalid_args_refused(void)
{
    _cenv_default();
    const gchar *pairs[][2] = { { "a", "a" }, { "a", "x/y" }, { "a", "b@s" }, { "", "b" }, { "a", "" }, { "a/..", "b" } };
    for (gsize i = 0; i < G_N_ELEMENTS(pairs); i++) {
        G_clog = g_string_new(NULL);
        pcv_lxc_clone_set_ops_for_test(&G_cfake);
        GError *e = NULL;
        g_assert_false(pcv_lxc_clone_run(G_clxc, WB, pairs[i][0], pairs[i][1], "sub", TRUE, &e));
        pcv_lxc_clone_set_ops_for_test(NULL);
        g_assert_error(e, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_assert_cmpuint(G_clog->len, ==, 0);
        g_string_free(G_clog, TRUE);
        G_clog = NULL;
        g_clear_error(&e);
    }
    _cenv_free();
}

                                                                                                    
                                   
static void
test_target_check_failure_refused(void)
{
    _cenv_default();
    CloneCase cc = _cc_ok();
    cc.list_err = "failed to execute child process \"zfs\" (Resource temporarily unavailable)";
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun(&cc, "sub", TRUE, &log, &e));
    _assert_msg(e, "cannot check whether clone target 'b' exists: zfs: failed to execute child process \"zfs\" "
                   "(Resource temporarily unavailable)");
    g_autofree gchar *want = g_strdup_printf("is_running a\ndir_exists %s/b\nrun zfs list -H -o name " WDST "\n", G_clxc);
    g_assert_cmpstr(log, ==, want);                                           
    g_clear_error(&e);
                                                      
    CloneCase ok = _cc_ok();
    g_autofree gchar *log2 = NULL;
    g_assert_true(_crun(&ok, "sub", TRUE, &log2, &e));
    g_assert_no_error(e);
    g_assert_nonnull(strstr(log2, "run zfs snapshot " WSRC_SNAP "\n"));
    _cenv_free();
}

                                                                                              
                                                                                     
static void
test_cleanup_removes_target_dir(void)
{
    _cenv_default();
    g_autofree gchar *b = g_build_filename(G_clxc, "b", NULL);
    PcvLxcCloneOps ops = G_cfake;
    ops.remove_dir = NULL;
    ops.dir_exists = NULL;
    CloneCase cc = _cc_ok();
    cc.set_mkdir = 1;                                                                        
    cc.rewrite_ok = FALSE;
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun_ops(&cc, &ops, "sub", TRUE, &log, &e));
    _assert_msg(e, "clone 'b' failed at config rewrite: fake rewrite failed; clone removed");
    g_assert_false(g_file_test(b, G_FILE_TEST_EXISTS));
    g_clear_error(&e);
                                         
    cc.rewrite_ok = TRUE;
    g_autofree gchar *log2 = NULL;
    g_assert_true(_crun_ops(&cc, &ops, "sub", TRUE, &log2, &e));
    g_assert_no_error(e);
    g_assert_true(g_file_test(b, G_FILE_TEST_IS_DIR));
    g_assert_cmpint(g_rmdir(b), ==, 0);
                                                                            
    cc.pipe_ok = FALSE;
    g_autofree gchar *log3 = NULL;
    g_assert_false(_crun_ops(&cc, &ops, "sub", TRUE, &log3, &e));
    _assert_msg(e, "clone 'b' failed at send/recv: fake send/recv failed; clone removed");
    g_clear_error(&e);
    _cenv_free();
}

                                                                          
static void
test_cleanup_dir_not_empty(void)
{
    _cenv_default();
    g_autofree gchar *b = g_build_filename(G_clxc, "b", NULL);
    g_autofree gchar *left = g_build_filename(b, "leftover", NULL);
    PcvLxcCloneOps ops = G_cfake;
    ops.remove_dir = NULL;
    CloneCase cc = _cc_ok();
    cc.set_mkdir = 2;
    cc.rewrite_ok = FALSE;
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_false(_crun_ops(&cc, &ops, "sub", TRUE, &log, &e));
    g_autofree gchar *want = g_strdup_printf("clone 'b' failed at config rewrite: fake rewrite failed; could not remove %s/b: "
                                             "%s; remove it manually", G_clxc, g_strerror(ENOTEMPTY));
    _assert_msg(e, want);
    g_assert_true(g_file_test(left, G_FILE_TEST_IS_REGULAR));
    g_clear_error(&e);
    _cenv_free();
}

                                                                                                               
                              
static void
_hn_tree(const gchar *name, const gchar *content)
{
    g_autofree gchar *etc = g_build_filename(G_clxc, name, "rootfs", "etc", NULL);
    g_assert_cmpint(g_mkdir_with_parents(etc, 0755), ==, 0);
    if (content) {
        g_autofree gchar *h = g_build_filename(etc, "hostname", NULL);
        g_assert_true(g_file_set_contents(h, content, -1, NULL));
    }
}

static gchar *
_hn_read(const gchar *name)
{
    g_autofree gchar *h = g_build_filename(G_clxc, name, "rootfs", "etc", "hostname", NULL);
    gchar *c = NULL;
    g_assert_true(g_file_get_contents(h, &c, NULL, NULL));
    return c;
}

static void
test_write_hostname_real(void)
{
    PcvLxcCloneOps ops = G_cfake;
    ops.write_hostname = NULL;                                     
    CloneCase cc = _cc_ok();
    for (int k = 0; k < 4; k++) {
        _cenv_default();
        _hn_tree("a", "a\n");
        g_autofree gchar *a_host = g_build_filename(G_clxc, "a", "rootfs", "etc", "hostname", NULL);
        if (k == 0) {
            _hn_tree("b", "a\n");                                            
        } else if (k == 1) {
            _hn_tree("b", NULL);                                                              
            g_autofree gchar *l = g_build_filename(G_clxc, "b", "rootfs", "etc", "hostname", NULL);
            g_assert_cmpint(symlink(a_host, l), ==, 0);
        } else if (k == 2) {
            g_autofree gchar *bdir = g_build_filename(G_clxc, "b", NULL);                                      
            g_assert_cmpint(g_mkdir(bdir, 0711), ==, 0);
            g_autofree gchar *ar = g_build_filename(G_clxc, "a", "rootfs", NULL);
            g_autofree gchar *br = g_build_filename(bdir, "rootfs", NULL);
            g_assert_cmpint(symlink(ar, br), ==, 0);
        } else {
            g_autofree gchar *ad = g_build_filename(G_clxc, "a", NULL);                                   
            g_autofree gchar *bd = g_build_filename(G_clxc, "b", NULL);
            g_assert_cmpint(symlink(ad, bd), ==, 0);
        }
        g_autofree gchar *log = NULL;
        GError *e = NULL;
        g_assert_true(_crun_ops(&cc, &ops, "sub", TRUE, &log, &e));                             
        g_assert_no_error(e);
        g_autofree gchar *a_after = _hn_read("a");
        g_assert_cmpstr(a_after, ==, "a\n");                                  
        if (k == 0) {
            g_autofree gchar *b_after = _hn_read("b");
            g_assert_cmpstr(b_after, ==, "b");
        }
        _cenv_free();
    }
}

                                                                                                           
static void
test_product_files_real(void)
{
    _cenv_default();
    g_autofree gchar *b = g_build_filename(G_clxc, "b", NULL);
    g_assert_cmpint(g_mkdir(b, 0711), ==, 0);
    const gchar *gone[] = { "purecvisor.owner", "purecvisor.volgen", "a.log", "a.log.1" };
    const gchar *kept[] = { "purecvisor.meta", "b.log", "config" };
    for (gsize i = 0; i < G_N_ELEMENTS(gone); i++) {
        g_autofree gchar *p = g_build_filename(b, gone[i], NULL);
        g_assert_true(g_file_set_contents(p, "x", -1, NULL));
    }
    for (gsize i = 0; i < G_N_ELEMENTS(kept); i++) {
        g_autofree gchar *p = g_build_filename(b, kept[i], NULL);
        g_assert_true(g_file_set_contents(p, "keep", -1, NULL));
    }
    PcvLxcCloneOps ops = G_cfake;
    ops.product_files = NULL;                                      
    CloneCase cc = _cc_ok();
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_true(_crun_ops(&cc, &ops, "sub", TRUE, &log, &e));
    g_assert_no_error(e);
    for (gsize i = 0; i < G_N_ELEMENTS(gone); i++) {
        g_autofree gchar *p = g_build_filename(b, gone[i], NULL);
        g_assert_false(g_file_test(p, G_FILE_TEST_EXISTS));
    }
    for (gsize i = 0; i < G_N_ELEMENTS(kept); i++) {
        g_autofree gchar *p = g_build_filename(b, kept[i], NULL);
        g_autofree gchar *c = NULL;
        g_assert_true(g_file_get_contents(p, &c, NULL, NULL));
        g_assert_cmpstr(c, ==, "keep");
    }
                               
    g_autofree gchar *log2 = NULL;
    g_assert_true(_crun_ops(&cc, &ops, "sub", TRUE, &log2, &e));
    g_assert_no_error(e);
                                                  
    g_autofree gchar *real = g_build_filename(G_clxc, "elsewhere", NULL);
    g_assert_cmpint(g_rename(b, real), ==, 0);
    g_autofree gchar *owner = g_build_filename(real, "purecvisor.owner", NULL);
    g_assert_true(g_file_set_contents(owner, "x", -1, NULL));
    g_assert_cmpint(symlink(real, b), ==, 0);
    g_autofree gchar *log3 = NULL;
    g_assert_false(_crun_ops(&cc, &ops, "sub", TRUE, &log3, &e));
    g_assert_nonnull(e);
    g_assert_true(g_str_has_prefix(e->message, "clone 'b' failed at product files: "));
    g_assert_true(g_file_test(owner, G_FILE_TEST_EXISTS));
    g_clear_error(&e);
    _cenv_free();
}

                                                                                         
static void
test_rewrite_real(void)
{
    _cenv_default();
    g_autofree gchar *b = g_build_filename(G_clxc, "b", NULL);
    g_assert_cmpint(g_mkdir(b, 0711), ==, 0);
    g_autofree gchar *cfg = g_build_filename(b, "config", NULL);
    g_autofree gchar *src_text = g_strdup_printf(WSRC_CONFIG_FMT "lxc.mount.entry = /srv/d mnt/d none bind,create=dir 0 0\n", G_clxc);
    g_assert_true(g_file_set_contents(cfg, src_text, -1, NULL));
    g_assert_cmpint(g_chmod(cfg, 0640), ==, 0);
    PcvLxcCloneOps ops = G_cfake;
    ops.rewrite_config = NULL;                                     
    CloneCase cc = _cc_ok();
    g_autofree gchar *log = NULL;
    GError *e = NULL;
    g_assert_true(_crun_ops(&cc, &ops, "sub", TRUE, &log, &e));
    g_assert_no_error(e);
    g_autofree gchar *got = NULL;
    g_assert_true(g_file_get_contents(cfg, &got, NULL, NULL));
    g_autofree gchar *want = g_strdup_printf("lxc.rootfs.path = dir:%s/b/rootfs\n"
                                             "lxc.mount.entry = /srv/d mnt/d none bind,create=dir 0 0\nlxc.uts.name = b\n", G_clxc);
    g_assert_cmpstr(got, ==, want);
    struct stat st;
    g_assert_cmpint(stat(cfg, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 0777, ==, 0640);                         
                                                                 
    const gchar *bad[] = { "lxc.rootfs.path = dir:/other/rootfs\n", "lxc.uts.name = a\n" };
    for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_assert_true(g_file_set_contents(cfg, bad[i], -1, NULL));
        g_autofree gchar *log2 = NULL;
        g_assert_false(_crun_ops(&cc, &ops, "sub", TRUE, &log2, &e));
        g_assert_nonnull(e);
        g_assert_true(g_str_has_prefix(e->message, "clone 'b' failed at config rewrite: "));
        g_assert_true(g_str_has_suffix(e->message, "; clone removed"));
        _assert_late_cleaned(log2, e, e->message);
        g_autofree gchar *after = NULL;
        g_assert_true(g_file_get_contents(cfg, &after, NULL, NULL));
        g_assert_cmpstr(after, ==, bad[i]);
        g_clear_error(&e);
    }
                                                                                    
    char *rp = realpath(G_clxc, NULL);
    g_assert_nonnull(rp);
    g_autofree gchar *canon = g_strdup(rp);
    free(rp);
    g_autofree gchar *link = g_strdup_printf("%s-link", G_clxc);
    g_assert_cmpint(symlink(G_clxc, link), ==, 0);
    g_autofree gchar *canon_text = g_strdup_printf("lxc.rootfs.path = dir:%s/a/rootfs\n", canon);
    g_assert_true(g_file_set_contents(cfg, canon_text, -1, NULL));
    gchar *saved = G_clxc;
    G_clxc = link;                                                                     
    g_autofree gchar *log3 = NULL;
    gboolean ok3 = _crun_ops(&cc, &ops, "sub", TRUE, &log3, &e);
    G_clxc = saved;
    g_assert_no_error(e);
    g_assert_true(ok3);
    g_autofree gchar *got3 = NULL;
    g_assert_true(g_file_get_contents(cfg, &got3, NULL, NULL));
    g_autofree gchar *want3 = g_strdup_printf("lxc.rootfs.path = dir:%s/b/rootfs\nlxc.uts.name = b\n", canon);
    g_assert_cmpstr(got3, ==, want3);
    g_assert_cmpint(unlink(link), ==, 0);
    _cenv_free();
}

void
test_lxc_clone_register(void)
{
    g_test_add_func("/lxc_clone/config_rootfs_path", test_config_rootfs_path);
    g_test_add_func("/lxc_clone/config_utsname", test_config_utsname);
    g_test_add_func("/lxc_clone/config_hwaddr", test_config_hwaddr);
    g_test_add_func("/lxc_clone/config_hook_fstab_paths", test_config_hook_fstab_paths);
    g_test_add_func("/lxc_clone/config_untouched_lines", test_config_untouched_lines);
    g_test_add_func("/lxc_clone/has_clone_hook", test_has_clone_hook);
    g_test_add_func("/lxc_clone/hostname_regular_file", test_hostname_regular_file);
    g_test_add_func("/lxc_clone/hostname_fifo_no_block", test_hostname_fifo_no_block);
    g_test_add_func("/lxc_clone/hostname_not_regular", test_hostname_not_regular);
    g_test_add_func("/lxc_clone/hostname_etc_symlink", test_hostname_etc_symlink);
    g_test_add_func("/lxc_clone/run_order", test_run_order);
    g_test_add_func("/lxc_clone/mounted_yes_skips_mount", test_mounted_yes_skips_mount);
    g_test_add_func("/lxc_clone/mounted_unreadable_fails", test_mounted_unreadable_fails);
    g_test_add_func("/lxc_clone/source_running_refused", test_source_running_refused);
    g_test_add_func("/lxc_clone/target_exists_refused", test_target_exists_refused);
    g_test_add_func("/lxc_clone/clone_hook_refused", test_clone_hook_refused);
    g_test_add_func("/lxc_clone/residue_failure_creates_nothing", test_residue_failure_creates_nothing);
    g_test_add_func("/lxc_clone/snapshot_exists_fails_without_cleanup", test_snapshot_exists_fails_without_cleanup);
    g_test_add_func("/lxc_clone/send_recv_failure_cleans", test_send_recv_failure_cleans);
    g_test_add_func("/lxc_clone/source_snapshot_cleanup_warns", test_source_snapshot_cleanup_warns);
    g_test_add_func("/lxc_clone/snapshot_cleanup_failure_cleans", test_snapshot_cleanup_failure_cleans);
    g_test_add_func("/lxc_clone/rewrite_failure_cleans", test_rewrite_failure_cleans);
    g_test_add_func("/lxc_clone/settle_failure_cleans", test_settle_failure_cleans);
    g_test_add_func("/lxc_clone/product_files_failure_cleans", test_product_files_failure_cleans);
    g_test_add_func("/lxc_clone/owner_written", test_owner_written);
    g_test_add_func("/lxc_clone/owner_skipped_without_subject", test_owner_skipped_without_subject);
    g_test_add_func("/lxc_clone/owner_failure_rolls_back", test_owner_failure_rolls_back);
    g_test_add_func("/lxc_clone/invalid_args_refused", test_invalid_args_refused);
    g_test_add_func("/lxc_clone/target_check_failure_refused", test_target_check_failure_refused);
    g_test_add_func("/lxc_clone/cleanup_removes_target_dir", test_cleanup_removes_target_dir);
    g_test_add_func("/lxc_clone/cleanup_dir_not_empty", test_cleanup_dir_not_empty);
    g_test_add_func("/lxc_clone/write_hostname_real", test_write_hostname_real);
    g_test_add_func("/lxc_clone/product_files_real", test_product_files_real);
    g_test_add_func("/lxc_clone/rewrite_real", test_rewrite_real);
}
