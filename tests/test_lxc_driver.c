   
                          
                                                                     
  
                           
                                                                  
                                               
                                                                                           
                                                               
                                                                    
                                                                                 
                                                                    
                                                      
                                                            
                                                                                   
                                                                           
                                                                       
                                                                                                                  
                                                                                            
                                                                                          
                                                                                       
                                                                                                                             
                                                                                                       
                                                                                     
                                                                                                                 
                                                                                                          
                                                                                       
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          
  
                       
                                                                       
                                                                    
                                                            
                                                                  
                                                                              
   
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "api/drain.h"                                                                     
#include "modules/dispatcher/rpc_completion.h"                                                     
#include "modules/lxc/lxc_clone.h"                                                                
#include "modules/lxc/lxc_driver.h"
#include "modules/lxc/lxc_limits.h"                                                            
#include "modules/lxc/lxc_unit.h"
#include "modules/lxc/lxc_volume_mount.h"
#include "utils/pcv_config.h"

                                                                   

static void
test_bw_net_index_prefers_exact_name(void)
{
                                                   
    const gchar *names[] = {"eth0", "eth1", "mgmt0"};
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 3, "eth0"), ==, 0);
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 3, "mgmt0"), ==, 2);
}

static void
test_bw_net_index_unnamed_slot_uses_kernel_default(void)
{
                                                                         
    const gchar *names[] = {"", NULL, "data"};
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 3, "eth0"), ==, 0);
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 3, "eth1"), ==, 1);
                                                
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 3, "eth2"), ==, -1);
}

static void
test_bw_net_index_rejects_unknown(void)
{
    const gchar *names[] = {"eth0"};
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 1, "eth1"), ==, -1);             
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 1, "eth"), ==, -1);
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 1, "eth01x"), ==, -1);
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 1, "wlan0"), ==, -1);
    g_assert_cmpint(pcv_lxc_bw_net_index(names, 1, NULL), ==, -1);
    g_assert_cmpint(pcv_lxc_bw_net_index(NULL, 0, "eth0"), ==, -1);
}

                                                                        

static void
test_bw_normalize_veth_accepts_lxc_names(void)
{
    gchar *v = pcv_lxc_bw_normalize_veth("vethAB12cd");
    g_assert_cmpstr(v, ==, "vethAB12cd");
    g_free(v);
    v = pcv_lxc_bw_normalize_veth("  vethQ9x2Lm\n");                        
    g_assert_cmpstr(v, ==, "vethQ9x2Lm");
    g_free(v);
}

static void
test_bw_normalize_veth_rejects_unsafe_values(void)
{
                                                                                    
    const gchar *bad[] = {
        "", "   ", "7:vethAB12cd@if6:<BROADCAST,MULTICAST,UP>mtu1500",
        "veth;reboot", "-vethX", "veth0123456789abc", NULL,
    };
    for (gsize i = 0; bad[i]; i++)
        g_assert_null(pcv_lxc_bw_normalize_veth(bad[i]));
    g_assert_null(pcv_lxc_bw_normalize_veth(NULL));
}

                                                         

static void
test_set_bandwidth_missing_container_is_error(void)
{
    GError *error = NULL;
                                                                    
    g_assert_false(pcv_lxc_set_bandwidth("zbc-z16-no-such-ctr", NULL, 0, 3000, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    g_assert_false(pcv_lxc_set_bandwidth("zbc-z16-no-such-ctr", "eth0", 5000, 0, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
}

static void
test_set_bandwidth_rejects_empty_or_invalid_request(void)
{
    GError *error = NULL;
                                                     
    g_assert_false(pcv_lxc_set_bandwidth("zbc-z16-no-such-ctr", NULL, 0, 0, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_lxc_set_bandwidth(NULL, NULL, 1, 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_lxc_set_bandwidth("bad name;x", NULL, 1, 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_lxc_set_bandwidth("zbc-z16-no-such-ctr", "eth0;x", 1, 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
}

                                                                                                      

                                                                      
static void
_tree_remove(const gchar *path)
{
    GStatBuf st;
    if (g_lstat(path, &st) != 0) {
        g_assert_cmpint(errno, ==, ENOENT);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        GDir *dir = g_dir_open(path, 0, NULL);
        g_assert_nonnull(dir);
        for (const gchar *n; (n = g_dir_read_name(dir)) != NULL;) {
            gchar *child = g_build_filename(path, n, NULL);
            _tree_remove(child);
            g_free(child);
        }
        g_dir_close(dir);
        g_assert_cmpint(g_rmdir(path), ==, 0);
    } else {
        g_assert_cmpint(g_unlink(path), ==, 0);
    }
}

                                                                                     
static void
_nic_rmw_sleep(const gchar *name)
{
    (void)name;
    g_usleep(100 * G_TIME_SPAN_MILLISECOND);
}

                                                                                        
static gpointer
_nic_attach_thread(gpointer name)
{
    GError *error = NULL;
    gboolean ok = pcv_lxc_nic_attach(name, "br0", NULL, &error);
    if (error)
        g_test_message("pcv_lxc_nic_attach: %s", error->message);
    g_clear_error(&error);
    return GINT_TO_POINTER(ok);
}

                                                                                     
static void
test_nic_attach_concurrent_no_lost_nic(void)
{
    gchar *root = g_dir_make_tmp("pcv-ctr-nic-XXXXXX", NULL);
    g_assert_nonnull(root);
    gchar *ctr_dir = g_build_filename(root, "ctr", NULL);
    g_assert_cmpint(g_mkdir(ctr_dir, 0750), ==, 0);
    gchar *cfg = g_build_filename(ctr_dir, "config", NULL);
    g_assert_true(g_file_set_contents(cfg, "lxc.uts.name = ctr\n"
                                           "lxc.idmap = u 0 100000 65536\nlxc.idmap = g 0 100000 65536\n"
                                           "lxc.net.0.type = veth\nlxc.net.0.link = br0\nlxc.net.0.flags = up\n", -1, NULL));
    gchar *conf = g_build_filename(root, "daemon.conf", NULL);
    gchar *conf_body = g_strdup_printf("[container]\nlxc_path=%s\n", root);
    g_assert_true(g_file_set_contents(conf, conf_body, -1, NULL));
    g_setenv("PCV_CONFIG_PATH", conf, TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_container_path(), ==, root);
                                                                                            
    gchar *old_xdg = g_strdup(g_getenv("XDG_RUNTIME_DIR"));
    gchar *rundir = g_build_filename(root, "run", NULL);
    g_setenv("XDG_RUNTIME_DIR", rundir, TRUE);

    pcv_lxc_nic_attach_set_rmw_hook_for_test(_nic_rmw_sleep);
    GThread *a = g_thread_new("nic-a", _nic_attach_thread, (gpointer)"ctr");
    GThread *b = g_thread_new("nic-b", _nic_attach_thread, (gpointer)"ctr");
    gboolean ok_a = GPOINTER_TO_INT(g_thread_join(a)), ok_b = GPOINTER_TO_INT(g_thread_join(b));
    pcv_lxc_nic_attach_set_rmw_hook_for_test(NULL);
    if (old_xdg)
        g_setenv("XDG_RUNTIME_DIR", old_xdg, TRUE);
    else
        g_unsetenv("XDG_RUNTIME_DIR");

    gchar *text = NULL;
    g_assert_true(g_file_get_contents(cfg, &text, NULL, NULL));
    g_test_message("config after two attaches:\n%s", text);
    g_assert_true(ok_a);
    g_assert_true(ok_b);
    g_assert_nonnull(strstr(text, "lxc.net.0.type = veth\n"));
    for (gint idx = 1; idx <= 2; idx++) {                                  
        gchar *type = g_strdup_printf("lxc.net.%d.type = veth\n", idx);
        gchar *link = g_strdup_printf("lxc.net.%d.link = br0\n", idx);
        gchar *nm = g_strdup_printf("lxc.net.%d.name = eth%d\n", idx, idx);
        g_assert_nonnull(strstr(text, type));
        g_assert_nonnull(strstr(text, link));
        g_assert_nonnull(strstr(text, nm));
        g_free(type); g_free(link); g_free(nm);
    }
    g_assert_null(strstr(text, "lxc.net.3."));

    pcv_config_shutdown();
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);                               
    if (geteuid() == 0) {                                                                                    
        gchar *lockdir = g_strconcat("/run/lxc/lock", root, NULL);
        gchar *lockfile = g_build_filename(lockdir, ".ctr", NULL);
        gchar *parent = g_path_get_dirname(lockdir);
        (void)g_unlink(lockfile);
        (void)g_rmdir(lockdir);
        (void)g_rmdir(parent);
        g_free(parent); g_free(lockfile); g_free(lockdir);
    }
    _tree_remove(root);
    g_free(text); g_free(rundir); g_free(old_xdg); g_free(conf_body); g_free(conf); g_free(cfg); g_free(ctr_dir); g_free(root);
}

                                                                                           
                                                                                            
                                                                                                 
#define DESTROY_POOL "crrpool/ctr"
#define DESTROY_NAME "crr-d1"
#define DESTROY_DS   DESTROY_POOL "/" DESTROY_NAME
#define ZFS_BUSY     "echo \"cannot destroy '" DESTROY_DS "': dataset is busy\" >&2\nexit 1\n"

                                                                                               
static guint G_destroy_reset_calls;
static guint G_destroy_other_calls;                                      

static gboolean
_destroy_fake_run(const gchar * const *argv, guint timeout_sec, gchar **stdout_out, gchar **stderr_out,
                  gint *exit_status, GError **error)
{
    (void)timeout_sec; (void)error;
    if (g_strcmp0(argv[0], "systemctl") == 0 && g_strcmp0(argv[1], "reset-failed") == 0 &&
        g_strcmp0(argv[2], "purecvisor-lxc@" DESTROY_NAME ".service") == 0)
        G_destroy_reset_calls++;
    else
        G_destroy_other_calls++;
    if (stdout_out) *stdout_out = g_strdup("");
    if (stderr_out) *stderr_out = g_strdup("");
    if (exit_status) *exit_status = 0;
    return TRUE;
}

static gboolean _destroy_fake_running(const gchar *p, const gchar *n) { (void)p; (void)n; return FALSE; }
static void     _destroy_fake_sleep(guint ms) { (void)ms; }
static gint64   _destroy_fake_now(void) { return 0; }

static const PcvLxcUnitOps G_destroy_fake_ops = {
    .run = _destroy_fake_run, .is_running = _destroy_fake_running,
    .sleep_ms = _destroy_fake_sleep, .now_us = _destroy_fake_now,
};

typedef struct {
    gchar *root;                                                                            
    gchar *bin, *log, *ctr_dir, *cfg;
    gchar *saved_path, *saved_xdg;
} DestroyEnv;

static void
_destroy_env_begin(DestroyEnv *e)
{
    e->root = g_dir_make_tmp("pcv-ctr-destroy-XXXXXX", NULL);
    g_assert_nonnull(e->root);
    e->bin = g_build_filename(e->root, "bin", NULL);
    e->log = g_build_filename(e->root, "calls.log", NULL);
    e->ctr_dir = g_build_filename(e->root, DESTROY_NAME, NULL);
    e->cfg = g_build_filename(e->ctr_dir, "config", NULL);
    g_autofree gchar *rootfs = g_build_filename(e->ctr_dir, "rootfs", NULL);
    g_autofree gchar *keep = g_build_filename(rootfs, "keep", NULL);
    g_assert_cmpint(g_mkdir_with_parents(e->bin, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(rootfs, 0750), ==, 0);
    g_assert_true(g_file_set_contents(e->cfg, "lxc.uts.name = " DESTROY_NAME "\n", -1, NULL));
    g_assert_true(g_file_set_contents(keep, "data\n", -1, NULL));                            
    g_autofree gchar *conf = g_build_filename(e->root, "daemon.conf", NULL);
    g_autofree gchar *body = g_strdup_printf("[container]\nlxc_path=%s\n[storage]\ncontainer_pool=" DESTROY_POOL "\n", e->root);
    g_assert_true(g_file_set_contents(conf, body, -1, NULL));
    g_setenv("PCV_CONFIG_PATH", conf, TRUE);
    pcv_config_init();
    g_assert_cmpstr(pcv_config_get_container_path(), ==, e->root);
    g_assert_cmpstr(pcv_config_get_container_pool(), ==, DESTROY_POOL);
    e->saved_path = g_strdup(g_getenv("PATH"));
    g_autofree gchar *path = g_strdup_printf("%s:%s", e->bin, e->saved_path ? e->saved_path : "");
    g_setenv("PATH", path, TRUE);
    e->saved_xdg = g_strdup(g_getenv("XDG_RUNTIME_DIR"));                                      
    g_autofree gchar *rundir = g_build_filename(e->root, "run", NULL);
    g_setenv("XDG_RUNTIME_DIR", rundir, TRUE);
    pcv_lxc_zfs_destroy_set_interval_for_test(10);
    G_destroy_reset_calls = G_destroy_other_calls = 0;
    pcv_lxc_unit_set_ops_for_test(&G_destroy_fake_ops);                                          
                                                                                         
    g_autofree gchar *vs = g_build_filename(e->root, "volstage", NULL), *rt = g_build_filename(e->root, "rt", NULL);
    pcv_lxc_volume_set_roots_for_test(vs, rt);
}

                                                                             
static void
_destroy_mock(const DestroyEnv *e, const gchar *tool, const gchar *body)
{
    g_autofree gchar *file = g_build_filename(e->bin, tool, NULL);
    g_autofree gchar *qlog = g_shell_quote(e->log);
    g_autofree gchar *script = g_strdup_printf("#!/bin/sh\nprintf '%%s %%s\\n' \"${0##*/}\" \"$*\" >> %s\n%s", qlog, body);
    g_assert_true(g_file_set_contents(file, script, -1, NULL));
    g_assert_cmpint(g_chmod(file, 0755), ==, 0);
}

                               
static guint
_destroy_calls(const DestroyEnv *e, const gchar *tool)
{
    g_autofree gchar *text = NULL;
    if (!g_file_get_contents(e->log, &text, NULL, NULL))
        return 0;
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    g_autofree gchar *prefix = g_strconcat(tool, " ", NULL);
    guint n = 0;
    for (guint i = 0; lines[i]; i++)
        n += g_str_has_prefix(lines[i], prefix) ? 1 : 0;
    return n;
}

static void
_destroy_env_end(DestroyEnv *e)
{
    pcv_lxc_zfs_destroy_set_interval_for_test(0);
    pcv_lxc_unit_set_ops_for_test(NULL);
    pcv_lxc_volume_set_roots_for_test(NULL, NULL);
    g_assert_cmpuint(G_destroy_other_calls, ==, 0);                                            
    if (e->saved_path) g_setenv("PATH", e->saved_path, TRUE); else g_unsetenv("PATH");
    if (e->saved_xdg) g_setenv("XDG_RUNTIME_DIR", e->saved_xdg, TRUE); else g_unsetenv("XDG_RUNTIME_DIR");
    pcv_config_shutdown();
    g_setenv("PCV_CONFIG_PATH", "/nonexistent/pcv-test-isolated.conf", TRUE);                               
    if (geteuid() == 0) {                                                                                                    
        gchar *lockdir = g_strconcat("/run/lxc/lock", e->root, NULL);
        GDir *dir = g_dir_open(lockdir, 0, NULL);
        if (dir) {
            const gchar *ent;
            while ((ent = g_dir_read_name(dir))) {
                gchar *f = g_build_filename(lockdir, ent, NULL);
                (void)g_unlink(f);
                g_free(f);
            }
            g_dir_close(dir);
        }
        gchar *parent = g_path_get_dirname(lockdir);
        (void)g_rmdir(lockdir);
        (void)g_rmdir(parent);
        g_free(parent); g_free(lockdir);
    }
    _tree_remove(e->root);
    g_free(e->saved_xdg); g_free(e->saved_path); g_free(e->cfg); g_free(e->ctr_dir); g_free(e->log); g_free(e->bin); g_free(e->root);
}

                                                                       
static void
test_destroy_zfs_busy_retries_then_fails(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    _destroy_mock(&e, "zfs", ZFS_BUSY);
    GError *err = NULL;
    g_assert_false(pcv_lxc_zfs_destroy_retry(DESTROY_DS, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "dataset is busy"));
    g_assert_cmpuint(_destroy_calls(&e, "zfs"), ==, 11);
    g_assert_cmpuint(G_destroy_reset_calls, ==, 0);                                
    g_assert_cmpuint(PCV_LXC_ZFS_DESTROY_RETRIES, ==, 10);
    g_assert_cmpuint(PCV_LXC_ZFS_DESTROY_INTERVAL_MS, ==, 1000);
    g_clear_error(&err);
    _destroy_env_end(&e);
}

                              
static void
test_destroy_zfs_busy_then_succeeds(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    g_autofree gchar *qlog = g_shell_quote(e.log);
    g_autofree gchar *body = g_strdup_printf("if [ \"$(grep -c '^zfs ' %s)\" -le 2 ]; then\n" ZFS_BUSY "fi\nexit 0\n", qlog);
    _destroy_mock(&e, "zfs", body);
    GError *err = NULL;
    g_assert_true(pcv_lxc_zfs_destroy_retry(DESTROY_DS, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(_destroy_calls(&e, "zfs"), ==, 3);
    _destroy_env_end(&e);
}

                                              
static void
test_destroy_zfs_missing_is_success(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    _destroy_mock(&e, "zfs", "echo \"cannot open '" DESTROY_DS "': dataset does not exist\" >&2\nexit 1\n");
    GError *err = NULL;
    g_assert_true(pcv_lxc_zfs_destroy_retry(DESTROY_DS, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(_destroy_calls(&e, "zfs"), ==, 1);
    _destroy_env_end(&e);
}

                                                            
                                                                        
static gpointer
_destroy_pool_thread(gpointer dataset)
{
    return GINT_TO_POINTER(pcv_lxc_zfs_destroy_retry(dataset, NULL));
}

static void
test_destroy_zfs_same_pool_serialized(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    g_autofree gchar *seq = g_build_filename(e.root, "seq", NULL);
    g_autofree gchar *qseq = g_shell_quote(seq);
    g_autofree gchar *body = g_strdup_printf("echo S >> %s\nsleep 0.2\necho E >> %s\nexit 0\n", qseq, qseq);
    _destroy_mock(&e, "zfs", body);
    GThread *a = g_thread_new("destroy-a", _destroy_pool_thread, (gpointer)DESTROY_POOL "/a");
    GThread *b = g_thread_new("destroy-b", _destroy_pool_thread, (gpointer)DESTROY_POOL "/b");
    g_assert_true(GPOINTER_TO_INT(g_thread_join(a)));
    g_assert_true(GPOINTER_TO_INT(g_thread_join(b)));
    g_autofree gchar *text = NULL;
    g_assert_true(g_file_get_contents(seq, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "S\nE\nS\nE\n");
    _destroy_env_end(&e);
}

typedef struct { GMainLoop *loop; gboolean ok; GError *error; } DestroyWait;

static void
_destroy_done(GObject *source, GAsyncResult *res, gpointer data)
{
    (void)source;
    DestroyWait *w = data;
    w->ok = pcv_lxc_destroy_finish(res, &w->error);
    g_main_loop_quit(w->loop);
}

                                                               
static gboolean
_destroy_run(const gchar *name, GError **error)
{
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    DestroyWait w = { g_main_loop_new(ctx, FALSE), FALSE, NULL };
    pcv_lxc_destroy_async(name, NULL, _destroy_done, &w);
    g_main_loop_run(w.loop);
    g_main_loop_unref(w.loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    if (w.error)
        g_propagate_error(error, w.error);
    return w.ok;
}

                                                                                             
                                                                       
static void
test_destroy_recall_finishes_cleanup(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    g_autofree gchar *busy_flag = g_build_filename(e.root, "zfs-busy", NULL);
    g_autofree gchar *qflag = g_shell_quote(busy_flag), *qcfg = g_shell_quote(e.cfg);
    g_autofree gchar *zfs = g_strdup_printf("if [ -e %s ]; then\n" ZFS_BUSY "fi\nexit 0\n", qflag);
    g_autofree gchar *lxcd = g_strdup_printf("if [ -e %s ]; then rm -f %s; exit 0; fi\n"
                                             "echo \"Container is not defined\" >&2\nexit 1\n", qcfg, qcfg);
    _destroy_mock(&e, "zfs", zfs);
    _destroy_mock(&e, "lxc-destroy", lxcd);
    g_assert_true(g_file_set_contents(busy_flag, "", -1, NULL));

    GError *err = NULL;
    g_assert_false(_destroy_run(DESTROY_NAME, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "container '" DESTROY_NAME "' removed but ZFS dataset '" DESTROY_DS "' remains: "
                                      "zfs: cannot destroy '" DESTROY_DS "': dataset is busy; "
                                      "run container.destroy again to finish cleanup");
    g_clear_error(&err);
    g_assert_false(g_file_test(e.cfg, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(e.ctr_dir, G_FILE_TEST_IS_DIR));                                  
    g_autofree gchar *keep = g_build_filename(e.ctr_dir, "rootfs", "keep", NULL);
    g_assert_true(g_file_test(keep, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(_destroy_calls(&e, "lxc-destroy"), ==, 1);
    g_assert_cmpuint(_destroy_calls(&e, "zfs"), ==, 11);
    g_assert_cmpuint(G_destroy_reset_calls, ==, 0);                                

    g_assert_cmpint(g_unlink(busy_flag), ==, 0);
    g_assert_true(_destroy_run(DESTROY_NAME, &err));
    g_assert_no_error(err);
    g_assert_false(g_file_test(e.ctr_dir, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(_destroy_calls(&e, "lxc-destroy"), ==, 2);
    g_assert_cmpuint(_destroy_calls(&e, "zfs"), ==, 12);
    g_assert_cmpuint(G_destroy_reset_calls, ==, 1);                                    
    _destroy_env_end(&e);
}

                                                                                                     
                                                                                                   
                                                                                                                        
static guint    G_res_calls;
static gboolean G_res_fail;
static gchar   *G_res_log;

static int
_res_open_dir(const gchar *path, GError **error)
{
    if (g_str_has_suffix(path, "/rootfs")) {
        G_res_calls++;
        FILE *f = fopen(G_res_log, "a");
        g_assert_nonnull(f);
        fputs("residue-clear\n", f);
        fclose(f);
        if (G_res_fail) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "denied");
            return -1;
        }
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "not found");
    return -1;
}

static const PcvLxcMountOps G_res_ops = { .open_dir = _res_open_dir };

static void
_res_begin(const DestroyEnv *e, gboolean fail)
{
    G_res_calls = 0;
    G_res_fail = fail;
    g_free(G_res_log);
    G_res_log = g_strdup(e->log);
    pcv_lxc_volume_set_ops_for_test(&G_res_ops);
}

static void
_res_end(void)
{
    pcv_lxc_volume_set_ops_for_test(NULL);
    pcv_lxc_destroy_set_running_probe_for_test(NULL);
    g_clear_pointer(&G_res_log, g_free);
}

                                                   
static gint
_destroy_line_of(const DestroyEnv *e, const gchar *needle)
{
    g_autofree gchar *text = NULL;
    if (!g_file_get_contents(e->log, &text, NULL, NULL))
        return -1;
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    for (guint i = 0; lines[i]; i++)
        if (g_str_has_prefix(lines[i], needle))
            return (gint)i;
    return -1;
}

                                         
static void
test_destroy_clears_residue_first(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    _res_begin(&e, FALSE);
    _destroy_mock(&e, "zfs", "exit 0\n");
    _destroy_mock(&e, "lxc-destroy", "exit 0\n");
    GError *err = NULL;
    g_assert_true(_destroy_run(DESTROY_NAME, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(G_res_calls, ==, 1);
    gint res = _destroy_line_of(&e, "residue-clear"), des = _destroy_line_of(&e, "lxc-destroy");
    g_assert_cmpint(res, >=, 0);
    g_assert_cmpint(des, >, res);
    _res_end();
    _destroy_env_end(&e);
}

static gboolean _probe_running(const gchar *name) { (void)name; return TRUE; }

                                                                   
static void
test_destroy_skips_clear_when_still_running(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    _res_begin(&e, TRUE);                              
    pcv_lxc_destroy_set_running_probe_for_test(_probe_running);
    _destroy_mock(&e, "zfs", "exit 0\n");
    _destroy_mock(&e, "lxc-destroy", "exit 0\n");
    GError *err = NULL;
    g_assert_true(_destroy_run(DESTROY_NAME, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(G_res_calls, ==, 0);
    g_assert_cmpuint(_destroy_calls(&e, "lxc-destroy"), ==, 1);
    _res_end();
    _destroy_env_end(&e);
}

                                                                                                           
                                                                                                                   
                                                                                                       
static gchar *
_metrics_cg_begin(const DestroyEnv *e)
{
    gchar *cg = g_build_filename(e->root, "cg", NULL);
    g_autofree gchar *pay = g_build_filename(cg, "lxc.payload." DESTROY_NAME, NULL);
    g_assert_cmpint(g_mkdir_with_parents(pay, 0700), ==, 0);
    const gchar *files[][2] = { { "memory.current", "7626752\n" }, { "memory.max", "536870912\n" }, { "cpuacct.usage", "0\n" } };
    for (gsize i = 0; i < G_N_ELEMENTS(files); i++) {
        g_autofree gchar *f = g_build_filename(pay, files[i][0], NULL);
        g_assert_true(g_file_set_contents(f, files[i][1], -1, NULL));
    }
    pcv_lxc_metrics_set_cgroup_root_for_test(cg);
    pcv_lxc_cpu_sample_forget(DESTROY_NAME);                          
    return cg;
}

                                                              
static void
_metrics_usage(const gchar *cg, guint64 usec)
{
    g_autofree gchar *f = g_build_filename(cg, "lxc.payload." DESTROY_NAME, "cpu.stat", NULL);
    g_autofree gchar *body = g_strdup_printf("usage_usec %" G_GUINT64_FORMAT "\nuser_usec 1\nsystem_usec 1\n", usec);
    g_assert_true(g_file_set_contents(f, body, -1, NULL));
}

                                                                             
static PcvLxcMetrics *
_metrics_get(void)
{
    GError *err = NULL;
    PcvLxcMetrics *m = pcv_lxc_get_metrics(DESTROY_NAME, &err);
    g_assert_no_error(err);
    g_assert_nonnull(m);
    return m;
}

static void
_metrics_cg_end(void)
{
    pcv_lxc_metrics_set_cgroup_root_for_test(NULL);
    pcv_lxc_cpu_sample_forget(DESTROY_NAME);
}

                                                                                           
                                                                                      
static void
test_metrics_cpu_percent_two_samples(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    g_autofree gchar *cg = _metrics_cg_begin(&e);
    _metrics_usage(cg, 1000000);
    PcvLxcMetrics *m1 = _metrics_get();
    g_assert_cmpuint(m1->mem_used_bytes, ==, 7626752);
    g_assert_cmpuint(m1->mem_limit_bytes, ==, 536870912);
    g_assert_cmpuint(m1->cpu_time_ns, ==, G_GUINT64_CONSTANT(1000000000));
    g_assert_cmpfloat(m1->cpu_percent, ==, 0.0);             
    g_usleep(20000);
    _metrics_usage(cg, 1500000);
    PcvLxcMetrics *m2 = _metrics_get();
    g_assert_cmpuint(m2->cpu_time_ns, ==, G_GUINT64_CONSTANT(1500000000));
    g_test_message("second cpu_percent = %.3f", m2->cpu_percent);
    g_assert_cmpfloat(m2->cpu_percent, >, 0.0);
    g_assert_cmpfloat(m2->cpu_percent, <=, 2500.0 + 1e-6);
    pcv_lxc_metrics_free(m1);
    pcv_lxc_metrics_free(m2);
    _metrics_cg_end();
    _destroy_env_end(&e);
}

                                                                                             
                                                               
static void
test_destroy_forgets_cpu_sample(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    _res_begin(&e, FALSE);
    g_autofree gchar *cg = _metrics_cg_begin(&e);
    _destroy_mock(&e, "zfs", "exit 0\n");
    _destroy_mock(&e, "lxc-destroy", "exit 0\n");
    _metrics_usage(cg, 1000000);
    PcvLxcMetrics *m = _metrics_get();
    g_assert_cmpfloat(m->cpu_percent, ==, 0.0);
    pcv_lxc_metrics_free(m);
    g_usleep(2000);
    _metrics_usage(cg, 2000000);
    m = _metrics_get();
    g_assert_cmpfloat(m->cpu_percent, >, 0.0);                       
    pcv_lxc_metrics_free(m);

    GError *err = NULL;
    g_assert_true(_destroy_run(DESTROY_NAME, &err));
    g_assert_no_error(err);
    g_assert_false(g_file_test(e.ctr_dir, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_mkdir_with_parents(e.ctr_dir, 0750), ==, 0);                      
    g_assert_true(g_file_set_contents(e.cfg, "lxc.uts.name = " DESTROY_NAME "\n", -1, NULL));
    g_usleep(2000);
    _metrics_usage(cg, 3000000);
    m = _metrics_get();
    g_test_message("after destroy: cpu_percent = %.3f", m->cpu_percent);
    g_assert_cmpfloat(m->cpu_percent, ==, 0.0);                     
    pcv_lxc_metrics_free(m);
    _metrics_cg_end();
    _res_end();
    _destroy_env_end(&e);
}

                                                                                 
static void
test_destroy_residue_failure_keeps_container(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    _res_begin(&e, TRUE);
    _destroy_mock(&e, "zfs", "exit 0\n");
    _destroy_mock(&e, "lxc-destroy", "exit 0\n");
    GError *err = NULL;
    g_assert_false(_destroy_run(DESTROY_NAME, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_true(g_str_has_prefix(err->message, "container '" DESTROY_NAME "' has leftover volume mounts under its rootfs that could not be removed: cannot open "));
    g_assert_true(g_str_has_suffix(err->message, "; nothing was destroyed"));
    g_clear_error(&err);
    g_assert_cmpuint(_destroy_calls(&e, "lxc-destroy"), ==, 0);
    g_assert_cmpuint(_destroy_calls(&e, "zfs"), ==, 0);
    g_assert_true(g_file_test(e.ctr_dir, G_FILE_TEST_IS_DIR));

                                                                            
    g_assert_false(_destroy_run(DESTROY_NAME, &err));
    g_assert_nonnull(err);
    g_assert_false(g_error_matches(err, G_IO_ERROR, G_IO_ERROR_BUSY));
    g_assert_true(g_str_has_suffix(err->message, "; nothing was destroyed"));
    g_clear_error(&err);
    g_assert_cmpuint(G_res_calls, ==, 2);

                                   
    G_res_fail = FALSE;
    g_assert_true(_destroy_run(DESTROY_NAME, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(_destroy_calls(&e, "lxc-destroy"), ==, 1);
    _res_end();
    _destroy_env_end(&e);
}

static guint G_start_systemctl_start;

static gboolean
_start_fake_run(const gchar * const *argv, guint timeout_sec, gchar **stdout_out, gchar **stderr_out,
                gint *exit_status, GError **error)
{
    (void)timeout_sec; (void)error;
    if (g_strcmp0(argv[0], "systemctl") == 0 && g_strcmp0(argv[1], "start") == 0)
        G_start_systemctl_start++;
    if (stdout_out) *stdout_out = g_strdup("");
    if (stderr_out) *stderr_out = g_strdup("");
    if (exit_status) *exit_status = 0;
    return TRUE;
}

static const PcvLxcUnitOps G_start_fake_ops = {
    .run = _start_fake_run, .is_running = _destroy_fake_running,
    .sleep_ms = _destroy_fake_sleep, .now_us = _destroy_fake_now,
};

                                                                   
static void
_start_config(const DestroyEnv *e, const gchar *body)
{
    g_autofree gchar *dir = g_build_filename(e->root, "c1", NULL), *cfg = g_build_filename(dir, "config", NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(cfg, body, -1, NULL));
}

                                                                                                    
static void
test_start_sync_residue_failure_no_unit_start(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    _start_config(&e, "lxc.uts.name = c1\n");
    _res_begin(&e, TRUE);
    G_start_systemctl_start = 0;
    pcv_lxc_unit_set_ops_for_test(&G_start_fake_ops);
    GError *err = NULL;
    g_assert_false(pcv_lxc_start_sync("c1", &err));
    g_assert_nonnull(err);
    g_assert_true(g_str_has_prefix(err->message, "container 'c1' has leftover volume mounts under its rootfs that could not be removed: cannot open "));
    g_clear_error(&err);
    g_assert_cmpuint(G_res_calls, ==, 1);
    g_assert_cmpuint(G_start_systemctl_start, ==, 0);
    pcv_lxc_unit_set_ops_for_test(&G_destroy_fake_ops);
    _res_end();
    _destroy_env_end(&e);
}

                                                                             
static gboolean G_stop_running, G_stop_result;
static guint    G_stop_calls;

static gboolean _stop_fake_running(const gchar *name) { (void)name; return G_stop_running; }

static gboolean
_stop_fake_stop(const gchar *name, gboolean force)
{
    (void)name; (void)force;
    G_stop_calls++;
    FILE *f = fopen(G_res_log, "a");
    g_assert_nonnull(f);
    fputs("stop\n", f);
    fclose(f);
    return G_stop_result;
}

static const PcvLxcStopOps G_stop_fake_ops = { .is_running = _stop_fake_running, .stop = _stop_fake_stop };

static void
_stop_done(GObject *source, GAsyncResult *res, gpointer data)
{
    (void)source;
    DestroyWait *w = data;
    w->ok = pcv_lxc_stop_finish(res, &w->error);
    g_main_loop_quit(w->loop);
}

                                                            
static gboolean
_stop_run(const gchar *name, GError **error)
{
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    DestroyWait w = { g_main_loop_new(ctx, FALSE), FALSE, NULL };
    pcv_lxc_stop_async(name, FALSE, NULL, _stop_done, &w);
    g_main_loop_run(w.loop);
    g_main_loop_unref(w.loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    if (w.error)
        g_propagate_error(error, w.error);
    return w.ok;
}

                                                                          
                                                                           
static void
test_stop_clears_residue_after_stop(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    _res_begin(&e, FALSE);
    pcv_lxc_stop_set_ops_for_test(&G_stop_fake_ops);
    g_autofree gchar *rtdir = g_build_filename(e.root, "rt", DESTROY_NAME, NULL), *rtcfg = g_build_filename(rtdir, "config", NULL);
    g_assert_cmpint(g_mkdir_with_parents(rtdir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(rtcfg, "lxc.uts.name = " DESTROY_NAME "\n", -1, NULL));
    G_stop_calls = 0;
    G_stop_running = TRUE;
    G_stop_result = TRUE;
    GError *err = NULL;
    g_assert_true(_stop_run(DESTROY_NAME, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(G_stop_calls, ==, 1);
    g_assert_cmpuint(G_res_calls, ==, 1);
    gint stp = _destroy_line_of(&e, "stop"), res = _destroy_line_of(&e, "residue-clear");
    g_assert_cmpint(stp, >=, 0);
    g_assert_cmpint(res, >, stp);                                                  
    g_assert_false(g_file_test(rtdir, G_FILE_TEST_EXISTS));                          

                                                              
    g_assert_cmpint(g_mkdir_with_parents(rtdir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(rtcfg, "x\n", -1, NULL));
    G_stop_result = FALSE;
    g_assert_false(_stop_run(DESTROY_NAME, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&err);
    g_assert_cmpuint(G_stop_calls, ==, 2);
    g_assert_cmpuint(G_res_calls, ==, 1);
    g_assert_true(g_file_test(rtcfg, G_FILE_TEST_EXISTS));

                                
    G_stop_running = FALSE;
    g_assert_true(_stop_run(DESTROY_NAME, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(G_stop_calls, ==, 2);
    g_assert_cmpuint(G_res_calls, ==, 1);

    pcv_lxc_stop_set_ops_for_test(NULL);
    _res_end();
    _destroy_env_end(&e);
}

                                                                                                  
static void
test_destroy_removes_runtime_config(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    _res_begin(&e, FALSE);
    _destroy_mock(&e, "zfs", "exit 0\n");
    _destroy_mock(&e, "lxc-destroy", "exit 0\n");
    g_autofree gchar *rtdir = g_build_filename(e.root, "rt", DESTROY_NAME, NULL), *rtcfg = g_build_filename(rtdir, "config", NULL);
    g_assert_cmpint(g_mkdir_with_parents(rtdir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(rtcfg, "lxc.uts.name = " DESTROY_NAME "\n", -1, NULL));
    GError *err = NULL;
    g_assert_true(_destroy_run(DESTROY_NAME, &err));
    g_assert_no_error(err);
    g_assert_false(g_file_test(rtdir, G_FILE_TEST_EXISTS));

    g_assert_cmpint(g_mkdir_with_parents(rtdir, 0700), ==, 0);                               
    g_assert_true(g_file_set_contents(rtcfg, "x\n", -1, NULL));
    g_assert_cmpint(g_chmod(rtdir, 0500), ==, 0);
    g_assert_true(_destroy_run(DESTROY_NAME, &err));
    g_assert_no_error(err);
    if (geteuid() != 0)
        g_assert_true(g_file_test(rtcfg, G_FILE_TEST_EXISTS));                               
    if (g_file_test(rtdir, G_FILE_TEST_EXISTS))
        g_assert_cmpint(g_chmod(rtdir, 0700), ==, 0);                              
    _res_end();
    _destroy_env_end(&e);
}

                                                                                                    
                                                                                                              
                                                            
static PcvLxcMountOps G_ss_ops;
static gboolean       G_ss_unit_fail, G_ss_running;
static gchar         *G_ss_rtcfg;

static void
_ss_log(const gchar *line)
{
    FILE *f = fopen(G_res_log, "a");
    g_assert_nonnull(f);
    fprintf(f, "%s\n", line);
    fclose(f);
}

static int
_ss_open_dir(const gchar *path, GError **error)
{
    g_autofree gchar *dir = g_path_get_dirname(path);
    if (g_str_has_suffix(dir, "/volstage")) {                                                       
        G_res_calls++;
        _ss_log("residue-clear");
    }
    return pcv_lxc_volume_real_ops_for_test()->open_dir(path, error);
}

static gchar *_ss_mountinfo(GError **error) { (void)error; return g_strdup(""); }

static int
_ss_source_open(const gchar *host_path, gchar **canon_out, GError **error)
{
    (void)canon_out; (void)error;
    _ss_log("staging-source");
    int fd = open(host_path, O_PATH | O_CLOEXEC);
    g_assert_cmpint(fd, >=, 0);
    return fd;
}

static gboolean
_ss_mount_clone(int src_fd, int target_fd, guint flags, guint64 *mnt_id_out, GError **error)
{
    (void)src_fd; (void)target_fd; (void)error;
    g_assert_null(mnt_id_out);                                   
    g_assert_cmpuint(flags, ==, PCV_LXC_MOUNT_RO);
    _ss_log(g_file_test(G_ss_rtcfg, G_FILE_TEST_EXISTS) ? "staging-mount rt" : "staging-mount no-rt");
    return TRUE;
}

static gboolean
_ss_run(const gchar * const *argv, guint timeout_sec, gchar **stdout_out, gchar **stderr_out, gint *exit_status, GError **error)
{
    (void)timeout_sec; (void)error;
    gboolean start = g_strcmp0(argv[0], "systemctl") == 0 && g_strcmp0(argv[1], "start") == 0;
    if (start) {
        G_start_systemctl_start++;
        _ss_log(g_file_test(G_ss_rtcfg, G_FILE_TEST_EXISTS) ? "unit-start rt" : "unit-start no-rt");
        G_ss_running = !G_ss_unit_fail;
    }
    if (stdout_out) *stdout_out = g_strdup(start ? "" : "LoadState=loaded\nActiveState=inactive\n");
    if (stderr_out) *stderr_out = g_strdup(start && G_ss_unit_fail ? "Job failed\n" : "");
    if (exit_status) *exit_status = start && G_ss_unit_fail ? 1 : 0;
    return TRUE;
}

static gboolean _ss_is_running(const gchar *p, const gchar *n) { (void)p; (void)n; return G_ss_running; }

static const PcvLxcUnitOps G_ss_unit_ops = {
    .run = _ss_run, .is_running = _ss_is_running, .sleep_ms = _destroy_fake_sleep, .now_us = _destroy_fake_now,
};

                                                                                                          
                                                        
static void
test_start_sync_staging_order(void)
{
    for (guint fail = 0; fail < 2; fail++) {
        DestroyEnv e;
        _destroy_env_begin(&e);
        g_autofree gchar *src = g_build_filename(e.root, "src", NULL);
        g_assert_cmpint(g_mkdir(src, 0700), ==, 0);
        g_autofree gchar *body = g_strdup_printf("lxc.uts.name = c1\nlxc.mount.entry = %s mnt/a none bind,ro 0 0\n", src);
        _start_config(&e, body);
        g_autofree gchar *rootfs = g_build_filename(e.root, "c1", "rootfs", NULL);
        g_assert_cmpint(g_mkdir(rootfs, 0755), ==, 0);                                          
        _res_begin(&e, FALSE);
        G_ss_ops = *pcv_lxc_volume_real_ops_for_test();
        G_ss_ops.open_dir = _ss_open_dir;
        G_ss_ops.read_mountinfo = _ss_mountinfo;
        G_ss_ops.source_open = _ss_source_open;
        G_ss_ops.mount_clone = _ss_mount_clone;
        pcv_lxc_volume_set_ops_for_test(&G_ss_ops);
        g_autofree gchar *env = g_build_filename(e.root, "lxc-unit.env", NULL);
        pcv_lxc_unit_set_env_path_for_test(env);
        G_ss_rtcfg = g_build_filename(e.root, "rt", "c1", "config", NULL);
        G_ss_unit_fail = fail;
        G_ss_running = FALSE;
        G_start_systemctl_start = 0;
        pcv_lxc_unit_set_ops_for_test(&G_ss_unit_ops);
        GError *err = NULL;
        gboolean ok = pcv_lxc_start_sync("c1", &err);
        gint res = _destroy_line_of(&e, "residue-clear"), src_l = _destroy_line_of(&e, "staging-source"),
             mnt = _destroy_line_of(&e, "staging-mount no-rt"), sys = _destroy_line_of(&e, "unit-start rt");
        g_assert_true(0 <= res && res < src_l && src_l < mnt && mnt < sys);
        g_assert_cmpuint(G_start_systemctl_start, ==, 1);
        g_autofree gchar *rtdir = g_build_filename(e.root, "rt", "c1", NULL);
        if (!fail) {
            g_assert_true(ok);
            g_assert_no_error(err);
            g_assert_cmpuint(G_res_calls, ==, 1);
            g_assert_true(g_file_test(G_ss_rtcfg, G_FILE_TEST_IS_REGULAR));
        } else {
            g_assert_false(ok);
            g_assert_nonnull(err);
            g_assert_true(g_str_has_prefix(err->message, "lxc-start failed for 'c1': "));
            g_clear_error(&err);
            g_assert_cmpuint(G_res_calls, ==, 2);                                                  
            g_assert_false(g_file_test(rtdir, G_FILE_TEST_EXISTS));
        }
        pcv_lxc_unit_set_env_path_for_test(NULL);
        pcv_lxc_unit_set_ops_for_test(&G_destroy_fake_ops);
        g_clear_pointer(&G_ss_rtcfg, g_free);
        _res_end();
        _destroy_env_end(&e);
    }
}

                                                       
                                                                                 
                                                                                                
                                                     
typedef struct { GMainLoop *loop; gchar *actor; gboolean ok; GError *error; } StartActorWait;

static void
_start_actor_done(GObject *source, GAsyncResult *res, gpointer data)
{
    (void)source;
    StartActorWait *w = data;
    w->actor = g_strdup(pcv_drain_task_actor(res));
    w->ok = pcv_lxc_start_finish(res, &w->error);
    g_main_loop_quit(w->loop);
}

                                                                                                   
                                                                                           
static gchar *
_start_actor_run(const gchar *scope_actor, const gchar *actor)
{
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    StartActorWait w = { g_main_loop_new(ctx, FALSE), NULL, FALSE, NULL };
    if (scope_actor) {
        pcv_rpc_completion_begin("container.start");
        pcv_rpc_completion_set_actor(scope_actor);
    }
    pcv_lxc_start_async("nope", actor, NULL, _start_actor_done, &w);
    if (scope_actor)
        (void)pcv_rpc_completion_finish();
    g_main_loop_run(w.loop);
    g_main_loop_unref(w.loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    g_assert_false(w.ok);
    g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);                                 
    g_clear_error(&w.error);
    return w.actor;
}

                                                                                                         
                                                                        
static void
test_start_async_actor(void)
{
    DestroyEnv e;
    _destroy_env_begin(&e);
    g_autofree gchar *outside = _start_actor_run(NULL, "alice");
    g_assert_cmpstr(outside, ==, "alice");
    g_autofree gchar *scoped = _start_actor_run("bob", NULL);
    g_assert_cmpstr(scoped, ==, "bob");
    g_autofree gchar *over = _start_actor_run("bob", "alice");
    g_assert_cmpstr(over, ==, "alice");
    g_autofree gchar *none = _start_actor_run(NULL, NULL);
    g_assert_null(none);
    _destroy_env_end(&e);
}

                                                                                                      
                                                                                               
                                                                                              
                                                                                   
#define RB_SNAP    "s1"
#define RB_TARGET  DESTROY_DS "@" RB_SNAP
#define RB_EXISTS  "exists " RB_TARGET "\n"
#define RB_STATE(s) "state " DESTROY_NAME " " s "\n"
#define RB_HALT    "halt " DESTROY_NAME " 30\n"
#define RB_RESIDUE "residue " DESTROY_NAME "\n"
#define RB_ZFS     "zfs_rollback " RB_TARGET "\n"
#define RB_FORGET  "forget " DESTROY_NAME "\n"

typedef struct { PcvLxcState first, after; gboolean exists, halt_ok, residue_ok, zfs_ok, nest; } RbCase;
typedef struct { gchar *log; gboolean ok, restart; GError *error; } RbResult;

static GString     *G_rb_log;
static RbCase       G_rb;                                   
static guint        G_rb_state_calls;
static RbResult     G_rb_nest;                                                        

static gboolean _rb_run(gboolean *restart, GError **error);

static const gchar *
_rb_state_str(PcvLxcState s)
{
    switch (s) {
        case PCV_LXC_STATE_STOPPED:  return "STOPPED";
        case PCV_LXC_STATE_STARTING: return "STARTING";
        case PCV_LXC_STATE_RUNNING:  return "RUNNING";
        case PCV_LXC_STATE_STOPPING: return "STOPPING";
        case PCV_LXC_STATE_FROZEN:   return "FROZEN";
        default:                     return "UNKNOWN";
    }
}

                                                                                     
static gboolean
_rb_fake_exists(const gchar *snapshot)
{
    g_string_append_printf(G_rb_log, "exists %s\n", snapshot);
    if (G_rb.nest) {
        G_rb.nest = FALSE;
        const gsize before = G_rb_log->len;
        G_rb_nest.ok = _rb_run(&G_rb_nest.restart, &G_rb_nest.error);
        G_rb_nest.log = g_strdup(G_rb_log->str + before);
    }
    return G_rb.exists;
}

                                                    
static PcvLxcState
_rb_fake_state(const gchar *name)
{
    const PcvLxcState s = G_rb_state_calls++ == 0 ? G_rb.first : G_rb.after;
    g_string_append_printf(G_rb_log, "state %s %s\n", name, _rb_state_str(s));
    return s;
}

static gboolean
_rb_fake_halt(const gchar *name, guint grace_sec, GError **error)
{
    g_string_append_printf(G_rb_log, "halt %s %u\n", name, grace_sec);
    if (!G_rb.halt_ok)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fake halt failed");
    return G_rb.halt_ok;
}

static gboolean
_rb_fake_residue(const gchar *lxc_path, const gchar *name, GError **error)
{
    g_assert_cmpstr(lxc_path, ==, pcv_config_get_container_path());
    g_string_append_printf(G_rb_log, "residue %s\n", name);
    if (!G_rb.residue_ok)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "1 volume mount(s) left under the container rootfs could not be removed");
    return G_rb.residue_ok;
}

static gboolean
_rb_fake_zfs(const gchar *snapshot, GError **error)
{
    g_string_append_printf(G_rb_log, "zfs_rollback %s\n", snapshot);
    if (!G_rb.zfs_ok)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fake zfs failed");
    return G_rb.zfs_ok;
}

static gboolean
_rb_fake_forget(const gchar *lxc_path, const gchar *name)
{
    g_assert_cmpstr(lxc_path, ==, pcv_config_get_container_path());
    g_string_append_printf(G_rb_log, "forget %s\n", name);
    return TRUE;
}

static const PcvLxcRollbackOps G_rb_fake_ops = {
    .snapshot_exists = _rb_fake_exists, .state = _rb_fake_state, .halt = _rb_fake_halt,
    .residue_clear = _rb_fake_residue, .zfs_rollback = _rb_fake_zfs, .forget = _rb_fake_forget,
};

typedef struct { GMainLoop *loop; gboolean ok, restart; GError *error; } RbWait;

static void
_rb_done(GObject *source, GAsyncResult *res, gpointer data)
{
    (void)source;
    RbWait *w = data;
    w->ok = pcv_lxc_snapshot_rollback_finish(res, &w->restart, &w->error);
    g_main_loop_quit(w->loop);
}

                                                                                                             
                                                         
static gboolean
_rb_run(gboolean *restart, GError **error)
{
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    RbWait w = { g_main_loop_new(ctx, FALSE), FALSE, TRUE, NULL };
    pcv_lxc_snapshot_rollback_async(DESTROY_NAME, RB_SNAP, NULL, _rb_done, &w);
    g_main_loop_run(w.loop);
    g_main_loop_unref(w.loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    *restart = w.restart;
    if (w.error)
        g_propagate_error(error, w.error);
    return w.ok;
}

                                                                
static void
_rb_case(const RbCase *c, RbResult *r)
{
    G_rb = *c;
    G_rb_state_calls = 0;
    G_rb_log = g_string_new(NULL);
    pcv_lxc_snapshot_set_rollback_ops_for_test(&G_rb_fake_ops);
    r->error = NULL;
    r->ok = _rb_run(&r->restart, &r->error);
    pcv_lxc_snapshot_set_rollback_ops_for_test(NULL);
    r->log = g_string_free(g_steal_pointer(&G_rb_log), FALSE);
}

static void
_rb_result_clear(RbResult *r)
{
    g_clear_pointer(&r->log, g_free);
    g_clear_error(&r->error);
}

                                                                                             
static void
_rb_env_begin(DestroyEnv *e)
{
    _destroy_env_begin(e);
    _destroy_mock(e, "zfs", "exit 1\n");
}

static void
_rb_env_end(DestroyEnv *e)
{
    g_assert_cmpuint(_destroy_calls(e, "zfs"), ==, 0);
    _destroy_env_end(e);
}

                                                                                                       
                                                                                                 
                                                                                                  
static void
test_rollback_stops_before_zfs_rollback(void)
{
    DestroyEnv e;
    _rb_env_begin(&e);
    g_assert_cmpuint(PCV_LXC_STOP_GRACE_SEC, ==, 30);
    RbResult r;
    _rb_case(&(RbCase){ PCV_LXC_STATE_RUNNING, PCV_LXC_STATE_STOPPED, TRUE, TRUE, TRUE, TRUE, FALSE }, &r);
    g_assert_cmpstr(r.log, ==, RB_EXISTS RB_STATE("RUNNING") RB_HALT RB_STATE("STOPPED") RB_RESIDUE RB_ZFS RB_FORGET);
    g_assert_no_error(r.error);
    g_assert_true(r.ok);
    g_assert_true(r.restart);
    _rb_result_clear(&r);
    _rb_case(&(RbCase){ PCV_LXC_STATE_STOPPING, PCV_LXC_STATE_STOPPED, TRUE, TRUE, TRUE, TRUE, FALSE }, &r);
    g_assert_null(strstr(r.log, "zfs_rollback"));
    g_assert_false(r.ok);
    g_assert_false(r.restart);
    _rb_result_clear(&r);
    _rb_env_end(&e);
}

                                                
static void
test_rollback_frozen_stops_first(void)
{
    DestroyEnv e;
    _rb_env_begin(&e);
    RbResult r;
    _rb_case(&(RbCase){ PCV_LXC_STATE_FROZEN, PCV_LXC_STATE_STOPPED, TRUE, TRUE, TRUE, TRUE, FALSE }, &r);
    g_assert_cmpstr(r.log, ==, RB_EXISTS RB_STATE("FROZEN") RB_HALT RB_STATE("STOPPED") RB_RESIDUE RB_ZFS RB_FORGET);
    g_assert_true(r.ok);
    g_assert_true(r.restart);
    _rb_result_clear(&r);
    _rb_env_end(&e);
}

                                                                   
static void
test_rollback_missing_snapshot_untouched(void)
{
    DestroyEnv e;
    _rb_env_begin(&e);
    RbResult r;
    _rb_case(&(RbCase){ PCV_LXC_STATE_RUNNING, PCV_LXC_STATE_STOPPED, FALSE, TRUE, TRUE, TRUE, FALSE }, &r);
    g_assert_cmpstr(r.log, ==, RB_EXISTS);
    g_assert_false(r.ok);
    g_assert_false(r.restart);
    g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(r.error->message, ==, "Snapshot '" RB_SNAP "' not found for container '" DESTROY_NAME "' (container left untouched)");
    _rb_result_clear(&r);
    _rb_env_end(&e);
}

                                                               
static void
test_rollback_halt_failure_no_rollback(void)
{
    DestroyEnv e;
    _rb_env_begin(&e);
    RbResult r;
    _rb_case(&(RbCase){ PCV_LXC_STATE_RUNNING, PCV_LXC_STATE_RUNNING, TRUE, FALSE, TRUE, TRUE, FALSE }, &r);
    g_assert_cmpstr(r.log, ==, RB_EXISTS RB_STATE("RUNNING") RB_HALT);
    g_assert_false(r.ok);
    g_assert_false(r.restart);
    g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(r.error->message, ==,
                    "container '" DESTROY_NAME "' could not be stopped for the rollback: fake halt failed; nothing was rolled back");
    _rb_result_clear(&r);
    _rb_env_end(&e);
}

                                                                               
static void
test_rollback_still_running_after_halt(void)
{
    DestroyEnv e;
    _rb_env_begin(&e);
    RbResult r;
    _rb_case(&(RbCase){ PCV_LXC_STATE_RUNNING, PCV_LXC_STATE_RUNNING, TRUE, TRUE, TRUE, TRUE, FALSE }, &r);
    g_assert_cmpstr(r.log, ==, RB_EXISTS RB_STATE("RUNNING") RB_HALT RB_STATE("RUNNING"));
    g_assert_false(r.ok);
    g_assert_false(r.restart);
    g_assert_cmpstr(r.error->message, ==, "container '" DESTROY_NAME
                    "' could not be stopped for the rollback: it is still RUNNING after the stop; nothing was rolled back");
    _rb_result_clear(&r);
    _rb_env_end(&e);
}

                                                                                     
static void
test_rollback_residue_failure_restarts(void)
{
    DestroyEnv e;
    _rb_env_begin(&e);
    RbResult r;
    _rb_case(&(RbCase){ PCV_LXC_STATE_RUNNING, PCV_LXC_STATE_STOPPED, TRUE, TRUE, FALSE, TRUE, FALSE }, &r);
    g_assert_cmpstr(r.log, ==, RB_EXISTS RB_STATE("RUNNING") RB_HALT RB_STATE("STOPPED") RB_RESIDUE);
    g_assert_false(r.ok);
    g_assert_true(r.restart);
    g_assert_cmpstr(r.error->message, ==, "container '" DESTROY_NAME "' has leftover volume mounts under its rootfs that could not be "
                    "removed: 1 volume mount(s) left under the container rootfs could not be removed; nothing was rolled back");
    _rb_result_clear(&r);
    _rb_env_end(&e);
}

                                                              
static void
test_rollback_zfs_failure_restarts(void)
{
    DestroyEnv e;
    _rb_env_begin(&e);
    RbResult r;
    _rb_case(&(RbCase){ PCV_LXC_STATE_RUNNING, PCV_LXC_STATE_STOPPED, TRUE, TRUE, TRUE, FALSE, FALSE }, &r);
    g_assert_cmpstr(r.log, ==, RB_EXISTS RB_STATE("RUNNING") RB_HALT RB_STATE("STOPPED") RB_RESIDUE RB_ZFS);
    g_assert_false(r.ok);
    g_assert_true(r.restart);
    g_assert_cmpstr(r.error->message, ==, "snapshot rollback failed: fake zfs failed");
    _rb_result_clear(&r);
    _rb_env_end(&e);
}

                                                                                                   
                                                                    
static void
test_rollback_transitional_state_refused(void)
{
    static const PcvLxcState states[] = { PCV_LXC_STATE_STARTING, PCV_LXC_STATE_STOPPING, PCV_LXC_STATE_UNKNOWN };
    DestroyEnv e;
    _rb_env_begin(&e);
    for (guint i = 0; i < G_N_ELEMENTS(states); i++) {
        RbResult r;
        _rb_case(&(RbCase){ states[i], PCV_LXC_STATE_STOPPED, TRUE, TRUE, TRUE, TRUE, FALSE }, &r);
        const gchar *name = _rb_state_str(states[i]);
        g_autofree gchar *want_log = g_strdup_printf(RB_EXISTS "state " DESTROY_NAME " %s\n", name);
        g_autofree gchar *want_msg = g_strdup_printf("container '" DESTROY_NAME "' is %s; nothing was rolled back "
                                                     "(retry when it is running or stopped)", name);
        g_assert_cmpstr(r.log, ==, want_log);
        g_assert_false(r.ok);
        g_assert_false(r.restart);
        g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_FAILED);
        g_assert_cmpstr(r.error->message, ==, want_msg);
        _rb_result_clear(&r);
    }
    _rb_env_end(&e);
}

                                                                        
static void
test_rollback_stopped_no_restart(void)
{
    DestroyEnv e;
    _rb_env_begin(&e);
    RbResult r;
    _rb_case(&(RbCase){ PCV_LXC_STATE_STOPPED, PCV_LXC_STATE_STOPPED, TRUE, TRUE, TRUE, TRUE, FALSE }, &r);
    g_assert_cmpstr(r.log, ==, RB_EXISTS RB_STATE("STOPPED") RB_RESIDUE RB_ZFS RB_FORGET);
    g_assert_true(r.ok);
    g_assert_false(r.restart);
    _rb_result_clear(&r);
    _rb_env_end(&e);
}

                                                                                                   
                                            
static void
test_rollback_busy(void)
{
    DestroyEnv e;
    _rb_env_begin(&e);
    G_rb_nest = (RbResult){ NULL, TRUE, TRUE, NULL };
    RbResult r;
    _rb_case(&(RbCase){ PCV_LXC_STATE_STOPPED, PCV_LXC_STATE_STOPPED, TRUE, TRUE, TRUE, TRUE, TRUE }, &r);
    g_assert_nonnull(G_rb_nest.log);
    g_assert_cmpstr(G_rb_nest.log, ==, "");
    g_assert_false(G_rb_nest.ok);
    g_assert_false(G_rb_nest.restart);
    g_assert_error(G_rb_nest.error, G_IO_ERROR, G_IO_ERROR_BUSY);
    _rb_result_clear(&G_rb_nest);
    g_assert_true(r.ok);
    g_assert_cmpstr(r.log, ==, RB_EXISTS RB_STATE("STOPPED") RB_RESIDUE RB_ZFS RB_FORGET);
    _rb_result_clear(&r);
    _rb_case(&(RbCase){ PCV_LXC_STATE_STOPPED, PCV_LXC_STATE_STOPPED, TRUE, TRUE, TRUE, TRUE, FALSE }, &r);
    g_assert_no_error(r.error);
    g_assert_true(r.ok);
    _rb_result_clear(&r);
    _rb_env_end(&e);
}

                                                                                                    
                                                                                            
                                                                                  
                                                                                                      
                                                                                     
                                                                           
#define CLH_DST "crr-clh2"

typedef struct {
    gboolean running;                                                              
    gboolean snapshot_ok, pipe_ok, rewrite_ok;                                              
} ClhCase;

static ClhCase  G_clh;
static gint     G_clh_hook_calls;                             
static gint     G_clh_hook_bad;                                                  
static GThread *G_clh_test_thread;
static gint     G_clh_marker;                                     

static gboolean
_clh_fail(GError **error, const gchar *what)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fake %s failed", what);
    return FALSE;
}

static gboolean
_clh_run(const gchar *const *argv, gchar **out, GError **error)
{
    const gchar *verb = argv[0] ? argv[1] : NULL;
    if (out)
        *out = NULL;
    if (g_strcmp0(verb, "list") == 0) {                                      
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "zfs: cannot open '" DESTROY_POOL "/" CLH_DST "': dataset does not exist");
        return FALSE;
    }
    if (g_strcmp0(verb, "snapshot") == 0)
        return G_clh.snapshot_ok ? TRUE : _clh_fail(error, "snapshot");
    if (g_strcmp0(verb, "get") == 0) {                                                    
        if (out)
            *out = g_strdup("yes\n");
        return TRUE;
    }
    return g_strcmp0(verb, "set") == 0 ? TRUE : _clh_fail(error, "unexpected zfs verb");
}

static gboolean
_clh_pipe(const gchar *const *producer, const gchar *const *consumer, GError **error)
{
    (void)producer; (void)consumer;
    return G_clh.pipe_ok ? TRUE : _clh_fail(error, "send/recv");
}

static gboolean _clh_is_running(const gchar *name) { (void)name; return G_clh.running; }
static gboolean _clh_dir_exists(const gchar *path) { (void)path; return FALSE; }
static gboolean _clh_hostname(const gchar *lxc_path, const gchar *dst) { (void)lxc_path; (void)dst; return TRUE; }
static gboolean _clh_stamp(const gchar *dst, const gchar *subject) { (void)dst; (void)subject; return TRUE; }

static gboolean
_clh_name_ok(const gchar *lxc_path, const gchar *name, GError **error)                                    
{
    (void)lxc_path; (void)name; (void)error;
    return TRUE;
}

static gboolean
_clh_rewrite(const gchar *lxc_path, const gchar *dst, const gchar *src, GError **error)
{
    (void)lxc_path; (void)dst; (void)src;
    return G_clh.rewrite_ok ? TRUE : _clh_fail(error, "rewrite");
}

static gboolean
_clh_product(const gchar *lxc_path, const gchar *src, const gchar *dst, GError **error)
{
    (void)lxc_path; (void)src; (void)dst; (void)error;
    return TRUE;
}

static gboolean
_clh_one_ok(const gchar *what, GError **error)                                                                
{
    (void)what; (void)error;
    return TRUE;
}

static const PcvLxcCloneOps G_clh_ops = {
    .run = _clh_run, .pipe = _clh_pipe, .is_running = _clh_is_running, .dir_exists = _clh_dir_exists,
    .residue_clear = _clh_name_ok, .rewrite_config = _clh_rewrite, .write_hostname = _clh_hostname, .settle = _clh_name_ok,
    .product_files = _clh_product, .stamp_owner = _clh_stamp, .destroy_snapshot = _clh_one_ok, .destroy_dataset = _clh_one_ok,
    .remove_dir = _clh_one_ok,
};

                                                                                        
static void
_clh_hook(gpointer data)
{
    if (data != &G_clh_marker || g_thread_self() == G_clh_test_thread)
        g_atomic_int_set(&G_clh_hook_bad, 1);
    g_usleep(30 * 1000);                                        
    g_atomic_int_inc(&G_clh_hook_calls);
}

typedef struct { GMainLoop *loop; gboolean ok; GError *error; gint hook_calls_at_done; } ClhWait;

static void
_clh_done(GObject *source, GAsyncResult *res, gpointer data)
{
    (void)source;
    ClhWait *w = data;
    w->hook_calls_at_done = g_atomic_int_get(&G_clh_hook_calls);                           
    w->ok = pcv_lxc_clone_finish(res, &w->error);
    g_main_loop_quit(w->loop);
}

                                                                                                  
                                                                   
static gboolean
_clh_clone(const ClhCase *c, const gchar *src, const gchar *dst, gboolean hook, GError **error)
{
    G_clh = *c;
    g_atomic_int_set(&G_clh_hook_calls, 0);
    g_atomic_int_set(&G_clh_hook_bad, 0);
    G_clh_test_thread = g_thread_self();
    pcv_lxc_clone_set_ops_for_test(&G_clh_ops);
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    ClhWait w = { g_main_loop_new(ctx, FALSE), FALSE, NULL, -1 };
    pcv_lxc_clone_async(src, dst, NULL, hook ? _clh_hook : NULL, &G_clh_marker, NULL, _clh_done, &w);
    g_main_loop_run(w.loop);
    g_main_loop_unref(w.loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    g_usleep(60 * 1000);                                      
    pcv_lxc_clone_set_ops_for_test(NULL);
    const gint want = hook ? 1 : 0;
    g_assert_cmpint(w.hook_calls_at_done, ==, want);                                          
    g_assert_cmpint(g_atomic_int_get(&G_clh_hook_calls), ==, want);                 
    g_assert_cmpint(g_atomic_int_get(&G_clh_hook_bad), ==, 0);                             
    if (w.error)
        g_propagate_error(error, w.error);
    return w.ok;
}

                                                                                        
static void
_clh_env_begin(DestroyEnv *e)
{
    _destroy_env_begin(e);
    _destroy_mock(e, "zfs", "exit 1\n");
}

static void
_clh_env_end(DestroyEnv *e)
{
    g_assert_cmpuint(_destroy_calls(e, "zfs"), ==, 0);
    _destroy_env_end(e);
}

static const ClhCase CLH_OK = { .snapshot_ok = TRUE, .pipe_ok = TRUE, .rewrite_ok = TRUE };

static void
_clh_expect_fail(const ClhCase *c, const gchar *want_prefix)
{
    GError *err = NULL;
    g_assert_false(_clh_clone(c, DESTROY_NAME, CLH_DST, TRUE, &err));
    g_assert_nonnull(err);
    g_autofree gchar *head = g_strndup(err->message, strlen(want_prefix));                          
    g_assert_cmpstr(head, ==, want_prefix);
    g_error_free(err);
}

static void
test_clone_release_hook_success(void)
{
    DestroyEnv e;
    _clh_env_begin(&e);
    GError *err = NULL;
    g_assert_true(_clh_clone(&CLH_OK, DESTROY_NAME, CLH_DST, TRUE, &err));
    g_assert_no_error(err);
    _clh_env_end(&e);
}

static void
test_clone_release_hook_precheck_refusal(void)
{
    DestroyEnv e;
    _clh_env_begin(&e);
    ClhCase c = CLH_OK;
    c.running = TRUE;
    _clh_expect_fail(&c, "stop the source container before cloning");
    _clh_env_end(&e);
}

static void
test_clone_release_hook_snapshot_failure(void)
{
    DestroyEnv e;
    _clh_env_begin(&e);
    ClhCase c = CLH_OK;
    c.snapshot_ok = FALSE;
    _clh_expect_fail(&c, "clone '" CLH_DST "' failed at snapshot: fake snapshot failed; nothing was created");
    _clh_env_end(&e);
}

static void
test_clone_release_hook_send_recv_failure(void)
{
    DestroyEnv e;
    _clh_env_begin(&e);
    ClhCase c = CLH_OK;
    c.pipe_ok = FALSE;
    _clh_expect_fail(&c, "clone '" CLH_DST "' failed at send/recv: fake send/recv failed; clone removed");
    _clh_env_end(&e);
}

static void
test_clone_release_hook_post_mount_failure(void)
{
    DestroyEnv e;
    _clh_env_begin(&e);
    ClhCase c = CLH_OK;
    c.rewrite_ok = FALSE;
    _clh_expect_fail(&c, "clone '" CLH_DST "' failed at config rewrite: fake rewrite failed; clone removed");
    _clh_env_end(&e);
}

                                                               
static void
test_clone_release_hook_driver_lock_busy(void)
{
    DestroyEnv e;
    _clh_env_begin(&e);
    const gchar *held[] = { DESTROY_NAME, CLH_DST };
    const gchar *want[] = { "source container has a concurrent operation in progress",
                            "target container has a concurrent operation in progress" };
    for (guint i = 0; i < G_N_ELEMENTS(held); i++) {
        g_assert_true(pcv_lxc_op_lock(held[i]));
        GError *err = NULL;
        g_assert_false(_clh_clone(&CLH_OK, DESTROY_NAME, CLH_DST, TRUE, &err));
        g_assert_error(err, G_IO_ERROR, G_IO_ERROR_BUSY);
        g_assert_cmpstr(err->message, ==, want[i]);
        g_error_free(err);
        pcv_lxc_op_unlock(held[i]);
        g_assert_true(pcv_lxc_op_lock(DESTROY_NAME));                           
        pcv_lxc_op_unlock(DESTROY_NAME);
    }
    _clh_env_end(&e);
}

                                                                                     
static void
test_clone_release_hook_invalid_args(void)
{
    DestroyEnv e;
    _clh_env_begin(&e);
    const gchar *srcs[] = { NULL, DESTROY_NAME }, *dsts[] = { CLH_DST, "" };
    for (guint i = 0; i < G_N_ELEMENTS(srcs); i++) {
        GError *err = NULL;
        g_assert_false(_clh_clone(&CLH_OK, srcs[i], dsts[i], TRUE, &err));
        g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_assert_cmpstr(err->message, ==, "source and target are required");
        g_error_free(err);
    }
    _clh_env_end(&e);
}

                                                           
static void
test_clone_release_hook_null_hook(void)
{
    DestroyEnv e;
    _clh_env_begin(&e);
    GError *err = NULL;
    g_assert_true(_clh_clone(&CLH_OK, DESTROY_NAME, CLH_DST, FALSE, &err));
    g_assert_no_error(err);
    ClhCase c = CLH_OK;
    c.pipe_ok = FALSE;
    g_assert_false(_clh_clone(&c, DESTROY_NAME, CLH_DST, FALSE, &err));
    g_clear_error(&err);
    g_assert_true(pcv_lxc_op_lock(DESTROY_NAME));
    g_assert_false(_clh_clone(&CLH_OK, DESTROY_NAME, CLH_DST, FALSE, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_BUSY);
    g_clear_error(&err);
    pcv_lxc_op_unlock(DESTROY_NAME);
    _clh_env_end(&e);
}

void
test_lxc_driver_register(void)
{
    g_test_add_func("/lxc_driver/clone_release_hook/success", test_clone_release_hook_success);
    g_test_add_func("/lxc_driver/clone_release_hook/precheck_refusal", test_clone_release_hook_precheck_refusal);
    g_test_add_func("/lxc_driver/clone_release_hook/snapshot_failure", test_clone_release_hook_snapshot_failure);
    g_test_add_func("/lxc_driver/clone_release_hook/send_recv_failure", test_clone_release_hook_send_recv_failure);
    g_test_add_func("/lxc_driver/clone_release_hook/post_mount_failure", test_clone_release_hook_post_mount_failure);
    g_test_add_func("/lxc_driver/clone_release_hook/driver_lock_busy", test_clone_release_hook_driver_lock_busy);
    g_test_add_func("/lxc_driver/clone_release_hook/invalid_args", test_clone_release_hook_invalid_args);
    g_test_add_func("/lxc_driver/clone_release_hook/null_hook", test_clone_release_hook_null_hook);
    g_test_add_func("/lxc_driver/destroy_zfs_same_pool_serialized", test_destroy_zfs_same_pool_serialized);
    g_test_add_func("/lxc_driver/destroy/zfs_busy_retries_then_fails", test_destroy_zfs_busy_retries_then_fails);
    g_test_add_func("/lxc_driver/destroy/zfs_busy_then_succeeds", test_destroy_zfs_busy_then_succeeds);
    g_test_add_func("/lxc_driver/destroy/zfs_missing_is_success", test_destroy_zfs_missing_is_success);
    g_test_add_func("/lxc_driver/destroy/recall_finishes_cleanup", test_destroy_recall_finishes_cleanup);
    g_test_add_func("/lxc_driver/destroy_clears_residue_first", test_destroy_clears_residue_first);
    g_test_add_func("/lxc_driver/destroy_skips_clear_when_still_running", test_destroy_skips_clear_when_still_running);
    g_test_add_func("/lxc_driver/metrics_cpu_percent_two_samples", test_metrics_cpu_percent_two_samples);
    g_test_add_func("/lxc_driver/destroy_forgets_cpu_sample", test_destroy_forgets_cpu_sample);
    g_test_add_func("/lxc_driver/destroy_residue_failure_keeps_container", test_destroy_residue_failure_keeps_container);
    g_test_add_func("/lxc_driver/stop_clears_residue_after_stop", test_stop_clears_residue_after_stop);
    g_test_add_func("/lxc_driver/start_sync_residue_failure_no_unit_start", test_start_sync_residue_failure_no_unit_start);
    g_test_add_func("/lxc_driver/destroy_removes_runtime_config", test_destroy_removes_runtime_config);
    g_test_add_func("/lxc_driver/start_sync_staging_order", test_start_sync_staging_order);
    g_test_add_func("/lxc_driver/start_async_actor", test_start_async_actor);
    g_test_add_func("/lxc_driver/rollback_stops_before_zfs_rollback", test_rollback_stops_before_zfs_rollback);
    g_test_add_func("/lxc_driver/rollback_frozen_stops_first", test_rollback_frozen_stops_first);
    g_test_add_func("/lxc_driver/rollback_missing_snapshot_untouched", test_rollback_missing_snapshot_untouched);
    g_test_add_func("/lxc_driver/rollback_halt_failure_no_rollback", test_rollback_halt_failure_no_rollback);
    g_test_add_func("/lxc_driver/rollback_still_running_after_halt", test_rollback_still_running_after_halt);
    g_test_add_func("/lxc_driver/rollback_residue_failure_restarts", test_rollback_residue_failure_restarts);
    g_test_add_func("/lxc_driver/rollback_zfs_failure_restarts", test_rollback_zfs_failure_restarts);
    g_test_add_func("/lxc_driver/rollback_stopped_no_restart", test_rollback_stopped_no_restart);
    g_test_add_func("/lxc_driver/rollback_transitional_state_refused", test_rollback_transitional_state_refused);
    g_test_add_func("/lxc_driver/rollback_busy", test_rollback_busy);
    g_test_add_func("/lxc_driver/nic_attach_concurrent_no_lost_nic", test_nic_attach_concurrent_no_lost_nic);
    g_test_add_func("/lxc_driver/bandwidth/net_index_prefers_exact_name",
                    test_bw_net_index_prefers_exact_name);
    g_test_add_func("/lxc_driver/bandwidth/net_index_unnamed_slot_uses_kernel_default",
                    test_bw_net_index_unnamed_slot_uses_kernel_default);
    g_test_add_func("/lxc_driver/bandwidth/net_index_rejects_unknown",
                    test_bw_net_index_rejects_unknown);
    g_test_add_func("/lxc_driver/bandwidth/normalize_veth_accepts_lxc_names",
                    test_bw_normalize_veth_accepts_lxc_names);
    g_test_add_func("/lxc_driver/bandwidth/normalize_veth_rejects_unsafe_values",
                    test_bw_normalize_veth_rejects_unsafe_values);
    g_test_add_func("/lxc_driver/bandwidth/missing_container_is_error",
                    test_set_bandwidth_missing_container_is_error);
    g_test_add_func("/lxc_driver/bandwidth/rejects_empty_or_invalid_request",
                    test_set_bandwidth_rejects_empty_or_invalid_request);
}
