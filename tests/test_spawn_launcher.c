                                                                                        
                                                                                                           
                                                                         
                                                                
                             
                              
  
                                                               
  
                 
                                               
                                                                     
                       
                                                          
  
                                            
  
                                                                     
                                     
   

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <linux/capability.h>
#include "../src/utils/pcv_spawn.h"

                                                                         
guint64 pcv_privdrop_daemon_effective_mask(void);
guint64 pcv_privdrop_spawn_ceiling_mask(void);

                                                     

                                              
static void
test_launcher_init_shutdown(void)
{
    pcv_spawn_launcher_init();
    pcv_spawn_launcher_shutdown();
                      
    pcv_spawn_launcher_init();
    pcv_spawn_launcher_shutdown();
}

                                         
static void
test_launcher_double_init(void)
{
    pcv_spawn_launcher_init();
    pcv_spawn_launcher_init();                       
    pcv_spawn_launcher_shutdown();
}

                                            
static void
test_sync_true(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/true", NULL };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);

    g_assert_true(ok);
    g_assert_null(err);

    pcv_spawn_launcher_shutdown();
}

                                                       
static void
test_sync_false(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/false", NULL };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);

    g_assert_false(ok);
    g_assert_nonnull(err);
    g_error_free(err);

    pcv_spawn_launcher_shutdown();
}

                                                    
static void
test_sync_stdout_capture(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/echo", "hello-purecvisor", NULL };
    GError *err   = NULL;
    gchar  *out   = NULL;
    gboolean ok = pcv_spawn_sync(argv, &out, NULL, &err);

    g_assert_true(ok);
    g_assert_nonnull(out);
                         
    g_assert_true(g_str_has_prefix(out, "hello-purecvisor"));

    g_free(out);
    pcv_spawn_launcher_shutdown();
}

                                                 
static void
test_sync_stderr_capture(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/ls", "/nonexistent-path-xyz", NULL };
    GError *err   = NULL;
    gchar  *serr  = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &serr, &err);

    g_assert_false(ok);
                                                
    g_assert_true(serr != NULL || err != NULL);

    g_free(serr);
    if (err) g_error_free(err);
    pcv_spawn_launcher_shutdown();
}

                                                   
static void
test_fire_ok(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/bin/true", NULL };
    pcv_spawn_fire(argv);                            

                                                    
    g_usleep(50000);            

    pcv_spawn_launcher_shutdown();
}

                                                           
static void
test_fire_nonexistent(void)
{
    pcv_spawn_launcher_init();

    const gchar *argv[] = { "/nonexistent/binary", NULL };
    pcv_spawn_fire(argv);                                 

    pcv_spawn_launcher_shutdown();
}

                                              
static void
test_sync_without_launcher(void)
{
                                                
    const gchar *argv[] = { "/bin/true", NULL };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, NULL, &err);

    g_assert_true(ok);
    g_assert_null(err);
}

                                                               
static void
test_pipe_sync_counts_streamed_bytes(void)
{
    pcv_spawn_launcher_init();

    const gchar *producer[] = { "/usr/bin/printf", "abcde", NULL };
    const gchar *consumer[] = { "/usr/bin/wc", "-c", NULL };
    GError *err = NULL;
    gchar *out = NULL;
    gchar *stderr_buf = NULL;

    gboolean ok = pcv_spawn_pipe_sync(producer, consumer, &out, &stderr_buf, &err);

    g_assert_true(ok);
    g_assert_null(err);
    g_assert_nonnull(out);
    g_assert_cmpint((gint)g_ascii_strtoll(out, NULL, 10), ==, 5);
    g_assert_nonnull(stderr_buf);
    g_assert_cmpstr(stderr_buf, ==, "");

    g_free(out);
    g_free(stderr_buf);
    pcv_spawn_launcher_shutdown();
}

                                                           
static void
test_pipe_sync_consumer_failure(void)
{
    pcv_spawn_launcher_init();

    const gchar *producer[] = { "/usr/bin/printf", "abcde", NULL };
    const gchar *consumer[] = { "/bin/false", NULL };
    GError *err = NULL;
    gchar *stderr_buf = NULL;

    gboolean ok = pcv_spawn_pipe_sync(producer, consumer, NULL, &stderr_buf, &err);

    g_assert_false(ok);
    g_assert_nonnull(err);
    g_assert_nonnull(stderr_buf);

    g_error_free(err);
    g_free(stderr_buf);
    pcv_spawn_launcher_shutdown();
}

                                                                  
static void
test_sync_timeout_fires(void)
{
    pcv_spawn_launcher_init();
    const gchar *argv[] = { "/bin/sleep", "60", NULL };
    GError *err = NULL;
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, NULL, 1, &err);
    gdouble elapsed = (g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC;
    g_assert_false(ok);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_cmpfloat(elapsed, <, 10.0);                               
    g_clear_error(&err);
    pcv_spawn_launcher_shutdown();
}

                                            
static void
test_sync_timeout_normal(void)
{
    pcv_spawn_launcher_init();
    const gchar *argv[] = { "/bin/true", NULL };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, NULL, 5, &err);
    g_assert_true(ok);
    g_assert_no_error(err);
    pcv_spawn_launcher_shutdown();
}

                                      
static void
test_sync_timeout_zero_unbounded(void)
{
    pcv_spawn_launcher_init();
    const gchar *argv[] = { "/bin/true", NULL };
    GError *err = NULL;
    gboolean ok = pcv_spawn_sync_timeout(argv, NULL, NULL, 0, &err);
    g_assert_true(ok);
    g_assert_no_error(err);
    pcv_spawn_launcher_shutdown();
}

                                                             
static void
test_sync_env_rejects_execution_boundary_override(void)
{
    const gchar *argv[] = { "/usr/bin/printenv", "PCV_SPAWN_TEST_TOKEN", NULL };
    const gchar *safe_env[] = { "PCV_SPAWN_TEST_TOKEN=accepted", NULL };
    gchar *out = NULL;
    gchar *stderr_buf = NULL;
    GError *error = NULL;
    g_assert_true(pcv_spawn_sync_env(argv, safe_env, &out, &stderr_buf, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(out, ==, "accepted\n");
    g_assert_cmpstr(stderr_buf, ==, "");
    g_free(out);
    g_free(stderr_buf);

    const gchar *path_override[] = { "PATH=/tmp", NULL };
    g_assert_false(pcv_spawn_sync_env(argv, path_override, NULL, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    const gchar *loader_override[] = { "LD_PRELOAD=/tmp/evil.so", NULL };
    g_assert_false(pcv_spawn_sync_env(argv, loader_override, NULL, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    const gchar *malformed[] = { "NOT_AN_ASSIGNMENT", NULL };
    g_assert_false(pcv_spawn_sync_env(argv, malformed, NULL, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
}

                                                           
                                                                          
static void
test_capability_profile_classification(void)
{
    const gchar *empty[] = { NULL };
    const gchar *dns[] = { "dnsmasq", NULL };
    const gchar *dns_abs[] = { "/usr/sbin/dnsmasq", NULL };
    const gchar *dns_untrusted[] = { "/tmp/dnsmasq", NULL };
    const gchar *dns_dotdot[] = { "/usr/bin/../sbin/dnsmasq", NULL };
    const gchar *dns_adjacent[] = { "dnsmasq-helper", NULL };
    const gchar *netns_dns[] = { "ip", "netns", "exec", "pcv-ep0", "dnsmasq", NULL };
    const gchar *netns_untrusted[] = {
        "ip", "netns", "exec", "pcv-ep0", "/tmp/dnsmasq", NULL
    };
    const gchar *signal[] = { "/usr/bin/pkill", "-F", "/run/x.pid", NULL };
    const gchar *signal_kill[] = { "/usr/bin/kill", "-TERM", "123", NULL };
    const gchar *signal_fuser[] = { "/usr/bin/fuser", "-k", "/dev/x", NULL };
    const gchar *signal_untrusted[] = { "/tmp/kill", "-TERM", "123", NULL };
    const gchar *signal_adjacent[] = { "kill-helper", "-TERM", "123", NULL };
    const gchar *storage[] = { "zfs", "list", NULL };
    const gchar *storage_cp[] = { "/bin/cp", "a", "b", NULL };
    const gchar *runtime_lxc[] = { "lxc-start", "-n", "c1", NULL };
    const gchar *runtime_virt[] = { "/usr/bin/virt-customize", "-a", "x", NULL };
    const gchar *runtime_qemu[] = { "qemu-system-x86_64", NULL };
    const gchar *runtime_adjacent[] = { "lxc-start-helper", NULL };
    const gchar *runtime_untrusted[] = { "/usr/local/bin/lxc-start", NULL };
    const gchar *shell_lxc[] = { "/bin/sh", "-c", "lxc-start -n c1", NULL };
    const gchar *ordinary[] = { "/usr/bin/rsync", "a", "b", NULL };

    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(empty), ==, PCV_CHILD_CAP_BASE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(dns), ==, PCV_CHILD_CAP_DHCP);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(dns_abs), ==, PCV_CHILD_CAP_DHCP);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(dns_untrusted), ==, PCV_CHILD_CAP_BASE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(dns_dotdot), ==, PCV_CHILD_CAP_BASE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(dns_adjacent), ==, PCV_CHILD_CAP_BASE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(netns_dns), ==, PCV_CHILD_CAP_DHCP);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(netns_untrusted), ==, PCV_CHILD_CAP_BASE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(signal), ==, PCV_CHILD_CAP_SIGNAL);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(signal_kill), ==,
                    PCV_CHILD_CAP_SIGNAL);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(signal_fuser), ==,
                    PCV_CHILD_CAP_SIGNAL);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(signal_untrusted), ==,
                    PCV_CHILD_CAP_BASE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(signal_adjacent), ==,
                    PCV_CHILD_CAP_BASE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(storage), ==, PCV_CHILD_CAP_STORAGE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(storage_cp), ==, PCV_CHILD_CAP_STORAGE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(runtime_lxc), ==, PCV_CHILD_CAP_RUNTIME);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(runtime_virt), ==, PCV_CHILD_CAP_RUNTIME);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(runtime_qemu), ==, PCV_CHILD_CAP_RUNTIME);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(runtime_adjacent), ==, PCV_CHILD_CAP_BASE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(runtime_untrusted), ==, PCV_CHILD_CAP_BASE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(shell_lxc), ==, PCV_CHILD_CAP_BASE);
    g_assert_cmpint(pcv_spawn_capability_profile_for_argv(ordinary), ==, PCV_CHILD_CAP_BASE);

    const guint64 bit_chown = G_GUINT64_CONSTANT(1) << CAP_CHOWN;
    const guint64 bit_fowner = G_GUINT64_CONSTANT(1) << CAP_FOWNER;
    const guint64 bit_fsetid = G_GUINT64_CONSTANT(1) << CAP_FSETID;
    const guint64 bit_kill = G_GUINT64_CONSTANT(1) << CAP_KILL;
    const guint64 bit_setgid = G_GUINT64_CONSTANT(1) << CAP_SETGID;
    const guint64 bit_setpcap = G_GUINT64_CONSTANT(1) << CAP_SETPCAP;
    const guint64 bit_net_raw = G_GUINT64_CONSTANT(1) << CAP_NET_RAW;
    guint64 keep = pcv_privdrop_daemon_effective_mask();
    guint64 ceiling = pcv_privdrop_spawn_ceiling_mask();
    g_assert_cmpuint(pcv_privdrop_child_profile_mask(PCV_CHILD_CAP_BASE), ==, keep);
    g_assert_cmpuint(pcv_privdrop_child_profile_mask(PCV_CHILD_CAP_STORAGE), ==,
                     keep | bit_chown | bit_fowner | bit_fsetid);
    g_assert_cmpuint(pcv_privdrop_child_profile_mask(PCV_CHILD_CAP_SIGNAL), ==,
                     keep | bit_kill);
    g_assert_cmpuint(pcv_privdrop_child_profile_mask(PCV_CHILD_CAP_DHCP), ==,
                     keep | bit_chown | bit_setgid | bit_setpcap | bit_net_raw);
    g_assert_cmpuint(pcv_privdrop_child_profile_mask(PCV_CHILD_CAP_RUNTIME), ==, ceiling);
    for (gint p = PCV_CHILD_CAP_BASE; p < PCV_CHILD_CAP_N_PROFILES; p++)
        g_assert_cmpuint(pcv_privdrop_child_profile_mask((PcvChildCapabilityProfile)p) & ~ceiling,
                         ==, 0);
}

                                                       

static guint
_count_self_fds(void)
{
    GDir *d = g_dir_open("/proc/self/fd", 0, NULL);
    g_assert_nonnull(d);
    guint n = 0;
    while (g_dir_read_name(d))
        n++;
    g_dir_close(d);
    return n;
}

static void
test_bounded_exit_status(void)
{
    pcv_spawn_launcher_init();
    const gchar *argv[] = { "/bin/sh", "-c", "echo out; echo err >&2; exit 3", NULL };
    PcvSpawnBound b = { .profile = PCV_CHILD_CAP_BASE, .timeout_sec = 5 };
    gchar *out = NULL, *errs = NULL;
    gint status = -7;
    GError *err = NULL;
    g_assert_true(pcv_spawn_sync_bounded(argv, &b, &out, &errs, &status, &err));
    g_assert_no_error(err);
    g_assert_cmpint(status, ==, 3);
    g_assert_cmpstr(out, ==, "out\n");
    g_assert_cmpstr(errs, ==, "err\n");
    g_free(out);
    g_free(errs);
    pcv_spawn_launcher_shutdown();
}

static void
test_bounded_timeout_kills_group(void)
{
    pcv_spawn_launcher_init();

    const gchar *warm[] = { "/bin/true", NULL };
    PcvSpawnBound wb = { .profile = PCV_CHILD_CAP_BASE, .timeout_sec = 5,
                         .new_process_group = TRUE };
    gint status = -7;
    g_assert_true(pcv_spawn_sync_bounded(warm, &wb, NULL, NULL, &status, NULL));
    g_assert_cmpint(status, ==, 0);
    guint fds_before = _count_self_fds();

    g_autofree gchar *dir = g_dir_make_tmp("pcv-bounded-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree gchar *pidfile = g_build_filename(dir, "grandchild", NULL);
    g_autofree gchar *leaderfile = g_build_filename(dir, "leader", NULL);
    g_autofree gchar *script = g_strdup_printf("echo $$ > '%s'; sleep 60 & echo $! > '%s'; sleep 60",
                                               leaderfile, pidfile);
    const gchar *argv[] = { "/bin/sh", "-c", script, NULL };
    PcvSpawnBound b = { .profile = PCV_CHILD_CAP_BASE, .timeout_sec = 1,
                        .new_process_group = TRUE };
    gchar *out = NULL, *errs = NULL;
    GError *err = NULL;
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = pcv_spawn_sync_bounded(argv, &b, &out, &errs, &status, &err);
    gdouble elapsed = (g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC;
    g_assert_false(ok);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_cmpfloat(elapsed, <, 5.0);
    g_assert_null(out);
    g_assert_null(errs);
    g_assert_cmpint(status, ==, -1);
    g_clear_error(&err);

    g_autofree gchar *leadertxt = NULL;
    g_assert_true(g_file_get_contents(leaderfile, &leadertxt, NULL, NULL));
    g_autofree gchar *leader_path = g_strdup_printf("/proc/%s", g_strstrip(leadertxt));
    g_assert_false(g_file_test(leader_path, G_FILE_TEST_EXISTS));

    g_autofree gchar *pidtxt = NULL;
    g_assert_true(g_file_get_contents(pidfile, &pidtxt, NULL, NULL));
    gint64 gpid = g_ascii_strtoll(g_strstrip(pidtxt), NULL, 10);
    g_assert_cmpint(gpid, >, 1);

    g_autofree gchar *proc_path = g_strdup_printf("/proc/%" G_GINT64_FORMAT, gpid);
    gboolean gone = FALSE;
    for (int i = 0; i < 40 && !(gone = !g_file_test(proc_path, G_FILE_TEST_EXISTS)); i++)
        g_usleep(50 * 1000);
    g_assert_true(gone);
    guint fds_after = _count_self_fds();
    for (int i = 0; i < 40 && fds_after != fds_before; i++) {
        g_usleep(50 * 1000);
        fds_after = _count_self_fds();
    }
    g_assert_cmpuint(fds_after, ==, fds_before);
    g_unlink(pidfile);
    g_unlink(leaderfile);
    g_rmdir(dir);
    pcv_spawn_launcher_shutdown();
}

static void
test_bounded_leader_exits_grandchild_holds_pipe(void)
{
    pcv_spawn_launcher_init();
    g_autofree gchar *dir = g_dir_make_tmp("pcv-bounded-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_autofree gchar *pidfile = g_build_filename(dir, "gc", NULL);
    g_autofree gchar *script = g_strdup_printf("sleep 60 & echo $! > '%s'; exit 0", pidfile);
    const gchar *argv[] = { "/bin/sh", "-c", script, NULL };
    PcvSpawnBound b = { .profile = PCV_CHILD_CAP_BASE, .timeout_sec = 1,
                        .new_process_group = TRUE };
    gchar *out = NULL;
    gint status = -7;
    GError *err = NULL;
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = pcv_spawn_sync_bounded(argv, &b, &out, NULL, &status, &err);
    gdouble elapsed = (g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC;
    g_assert_false(ok);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_cmpfloat(elapsed, <, 5.0);
    g_assert_null(out);
    g_clear_error(&err);

    g_autofree gchar *pidtxt = NULL;
    g_assert_true(g_file_get_contents(pidfile, &pidtxt, NULL, NULL));
    gint64 gpid = g_ascii_strtoll(g_strstrip(pidtxt), NULL, 10);
    g_assert_cmpint(gpid, >, 1);
    g_autofree gchar *proc_path = g_strdup_printf("/proc/%" G_GINT64_FORMAT, gpid);
    gboolean gone = FALSE;
    for (int i = 0; i < 40 && !(gone = !g_file_test(proc_path, G_FILE_TEST_EXISTS)); i++)
        g_usleep(50 * 1000);
    g_assert_true(gone);
    g_unlink(pidfile);
    g_rmdir(dir);
    pcv_spawn_launcher_shutdown();
}

static gint g_bounded_abort_flag;

static gboolean
_bounded_should_abort(void)
{
    return g_atomic_int_get(&g_bounded_abort_flag) != 0;
}

static gpointer
_bounded_raise_abort(gpointer data)
{
    (void)data;
    g_usleep(500 * 1000);
    g_atomic_int_set(&g_bounded_abort_flag, 1);
    return NULL;
}

static void
test_bounded_should_abort(void)
{
    pcv_spawn_launcher_init();
    g_atomic_int_set(&g_bounded_abort_flag, 0);
    const gchar *argv[] = { "/bin/sleep", "60", NULL };
    PcvSpawnBound b = { .profile = PCV_CHILD_CAP_BASE, .timeout_sec = 30,
                        .should_abort = _bounded_should_abort };
    gint status = -7;
    GError *err = NULL;
    GThread *t = g_thread_new("bounded-abort", _bounded_raise_abort, NULL);
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = pcv_spawn_sync_bounded(argv, &b, NULL, NULL, &status, &err);
    gdouble elapsed = (g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC;
    g_thread_join(t);
    g_assert_false(ok);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpfloat(elapsed, <, 3.0);
    g_assert_cmpint(status, ==, -1);
    g_clear_error(&err);
    g_atomic_int_set(&g_bounded_abort_flag, 0);
    pcv_spawn_launcher_shutdown();
}

static void
test_bounded_output_cap(void)
{
    pcv_spawn_launcher_init();
    const gchar *argv[] = { "/usr/bin/yes", NULL };
    PcvSpawnBound b = { .profile = PCV_CHILD_CAP_BASE, .timeout_sec = 10,
                        .max_output_bytes = 65536 };
    gchar *out = NULL, *errs = NULL;
    gint status = -7;
    GError *err = NULL;
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = pcv_spawn_sync_bounded(argv, &b, &out, &errs, &status, &err);
    gdouble elapsed = (g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC;
    g_assert_false(ok);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
    g_assert_cmpstr(err->message, ==, "/usr/bin/yes: output exceeds 65536 bytes");
    g_assert_cmpfloat(elapsed, <, 5.0);
    g_assert_null(out);
    g_assert_null(errs);
    g_assert_cmpint(status, ==, -1);
    g_clear_error(&err);
    pcv_spawn_launcher_shutdown();
}

static gpointer
_bounded_cancel_later(gpointer data)
{
    g_usleep(500 * 1000);
    g_cancellable_cancel(G_CANCELLABLE(data));
    return NULL;
}

static void
test_bounded_cancel_kills(void)
{
    pcv_spawn_launcher_init();
    GCancellable *cancel = g_cancellable_new();
    const gchar *argv[] = { "/bin/sleep", "60", NULL };
    PcvSpawnBound b = { .profile = PCV_CHILD_CAP_BASE, .timeout_sec = 30,
                        .cancellable = cancel };
    gint status = -7;
    GError *err = NULL;
    GThread *t = g_thread_new("bounded-cancel", _bounded_cancel_later, cancel);
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = pcv_spawn_sync_bounded(argv, &b, NULL, NULL, &status, &err);
    gdouble elapsed = (g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC;
    g_thread_join(t);
    g_assert_false(ok);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpfloat(elapsed, <, 5.0);
    g_assert_cmpint(status, ==, -1);
    g_clear_error(&err);
    g_object_unref(cancel);
    pcv_spawn_launcher_shutdown();
}

static void
test_sync_timeout_message_unchanged(void)
{
    pcv_spawn_launcher_init();
    GError *err = NULL;
    const gchar *argv[] = { "/bin/sleep", "60", NULL };
    gint64 t0 = g_get_monotonic_time();
    g_assert_false(pcv_spawn_sync_timeout(argv, NULL, NULL, 1, &err));
    g_assert_cmpfloat((g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC, <, 3.0);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_cmpstr(err->message, ==, "/bin/sleep: timed out after 1s");
    g_clear_error(&err);

    const gchar *fail[] = { "/bin/sh", "-c", "echo partial; echo bad >&2; exit 2", NULL };
    gchar *out = NULL, *errs = NULL;
    g_assert_false(pcv_spawn_sync_timeout(fail, &out, &errs, 5, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "/bin/sh: bad");
    g_assert_cmpstr(out, ==, "partial\n");
    g_clear_error(&err);
    g_free(out);
    g_free(errs);

    const gchar *missing[] = { "/nonexistent/pcv-timeout-probe", NULL };
    g_assert_false(pcv_spawn_sync_timeout(missing, NULL, NULL, 5, &err));
    g_assert_error(err, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT);
    g_clear_error(&err);
    pcv_spawn_launcher_shutdown();
}

static const gchar * const k_env_forbidden[] = {
    "PCV_SECRET_SPAWN_TEST", "PURECVISOR_JWT_SECRET", "NOTIFY_SOCKET",
    "CREDENTIALS_DIRECTORY", "LC_ALL", NULL
};

static void
env_probe_set(void)
{
    g_setenv("PCV_SECRET_SPAWN_TEST", "leak", TRUE);
    g_setenv("PURECVISOR_JWT_SECRET", "leak", TRUE);
    g_setenv("NOTIFY_SOCKET", "/run/systemd/notify", TRUE);
    g_setenv("CREDENTIALS_DIRECTORY", "/run/credentials/x", TRUE);
    g_setenv("LC_ALL", "de_DE.UTF-8", TRUE);
    g_setenv("LIBVIRT_DEFAULT_URI", "test:///default", TRUE);
    g_setenv("ZFS_MODULE_TIMEOUT", "0", TRUE);
}

static void
env_probe_unset(void)
{
    for (const gchar * const *k = k_env_forbidden; *k; k++)
        g_unsetenv(*k);
    g_unsetenv("LIBVIRT_DEFAULT_URI");
    g_unsetenv("ZFS_MODULE_TIMEOUT");
}

static gboolean
env_has_line(const gchar *out, const gchar *line)
{
    gchar **v = g_strsplit(out, "\n", -1);
    gboolean found = FALSE;
    for (gchar **p = v; *p && !found; p++)
        found = g_strcmp0(*p, line) == 0;
    g_strfreev(v);
    return found;
}

static gboolean
env_has_key(const gchar *out, const gchar *key)
{
    gchar *pfx = g_strconcat(key, "=", NULL);
    gchar **v = g_strsplit(out, "\n", -1);
    gboolean found = FALSE;
    for (gchar **p = v; *p && !found; p++)
        found = g_str_has_prefix(*p, pfx);
    g_strfreev(v);
    g_free(pfx);
    return found;
}

static void
env_assert_filtered(const gchar *out)
{
    g_assert_nonnull(out);
    for (const gchar * const *k = k_env_forbidden; *k; k++)
        g_assert_false(env_has_key(out, *k));
    g_assert_true(env_has_line(out, "PATH=/usr/sbin:/usr/bin:/sbin:/bin"));
    g_assert_true(env_has_line(out, "HOME=/root"));
    g_assert_true(env_has_line(out, "LANG=C.UTF-8"));
}

static void
test_env_allowlist_shared(void)
{
    env_probe_set();
    pcv_spawn_launcher_init();
    const gchar *argv[] = { "/usr/bin/env", NULL };
    gchar *out = NULL;
    GError *err = NULL;
    g_assert_true(pcv_spawn_sync(argv, &out, NULL, &err));
    g_assert_no_error(err);
    env_assert_filtered(out);
    g_free(out);
    pcv_spawn_launcher_shutdown();
    env_probe_unset();
}

static void
test_env_allowlist_fallback(void)
{
    env_probe_set();
    pcv_spawn_launcher_shutdown();
    const gchar *argv[] = { "/usr/bin/env", NULL };
    gchar *out = NULL;
    GError *err = NULL;
    g_assert_true(pcv_spawn_sync(argv, &out, NULL, &err));
    g_assert_no_error(err);
    env_assert_filtered(out);
    g_free(out);
    env_probe_unset();
}

static void
test_env_allowlist_env_launcher(void)
{
    env_probe_set();
    const gchar *argv[] = { "/usr/bin/env", NULL };
    const gchar *envp[] = { "PCV_SPAWN_TEST_TOKEN=accepted", NULL };
    gchar *out = NULL;
    GError *err = NULL;
    g_assert_true(pcv_spawn_sync_env(argv, envp, &out, NULL, &err));
    g_assert_no_error(err);
    env_assert_filtered(out);
    g_assert_true(env_has_line(out, "PCV_SPAWN_TEST_TOKEN=accepted"));
    g_free(out);
    env_probe_unset();
}

static void
test_env_allowlist_keeps_allowed_keys(void)
{
    env_probe_set();
    pcv_spawn_launcher_init();
    const gchar *argv[] = { "/usr/bin/env", NULL };
    gchar *out = NULL;
    GError *err = NULL;
    g_assert_true(pcv_spawn_sync(argv, &out, NULL, &err));
    g_assert_no_error(err);
    g_assert_true(env_has_line(out, "LIBVIRT_DEFAULT_URI=test:///default"));
    g_assert_true(env_has_line(out, "ZFS_MODULE_TIMEOUT=0"));
    g_free(out);
    pcv_spawn_launcher_shutdown();
    env_probe_unset();
}

static void
test_env_allowlist_bounded(void)
{
    env_probe_set();
    pcv_spawn_launcher_init();
    const gchar *argv[] = { "/usr/bin/env", NULL };
    PcvSpawnBound b = { .profile = PCV_CHILD_CAP_BASE, .timeout_sec = 5 };
    gchar *out = NULL;
    gint st = -1;
    GError *err = NULL;
    g_assert_true(pcv_spawn_sync_bounded(argv, &b, &out, NULL, &st, &err));
    g_assert_no_error(err);
    g_assert_cmpint(st, ==, 0);
    env_assert_filtered(out);
    g_free(out);
    pcv_spawn_launcher_shutdown();
    env_probe_unset();
}

void
test_spawn_launcher_register(void)
{
    g_test_add_func("/spawn_launcher/init_shutdown",       test_launcher_init_shutdown);
    g_test_add_func("/spawn_launcher/double_init",         test_launcher_double_init);
    g_test_add_func("/spawn_launcher/sync_true",           test_sync_true);
    g_test_add_func("/spawn_launcher/sync_false",          test_sync_false);
    g_test_add_func("/spawn_launcher/sync_stdout_capture", test_sync_stdout_capture);
    g_test_add_func("/spawn_launcher/sync_stderr_capture", test_sync_stderr_capture);
    g_test_add_func("/spawn_launcher/fire_ok",             test_fire_ok);
    g_test_add_func("/spawn_launcher/fire_nonexistent",    test_fire_nonexistent);
    g_test_add_func("/spawn_launcher/sync_no_launcher",    test_sync_without_launcher);
    g_test_add_func("/spawn_launcher/pipe_sync_counts_bytes",
                    test_pipe_sync_counts_streamed_bytes);
    g_test_add_func("/spawn_launcher/pipe_sync_consumer_failure",
                    test_pipe_sync_consumer_failure);
    g_test_add_func("/spawn_launcher/sync_timeout_fires",          test_sync_timeout_fires);
    g_test_add_func("/spawn_launcher/sync_timeout_normal",         test_sync_timeout_normal);
    g_test_add_func("/spawn_launcher/sync_timeout_zero_unbounded", test_sync_timeout_zero_unbounded);
    g_test_add_func("/spawn_launcher/sync_env_boundary_override",
                    test_sync_env_rejects_execution_boundary_override);
    g_test_add_func("/spawn_launcher/capability_profile_classification",
                    test_capability_profile_classification);
    g_test_add_func("/spawn_launcher/bounded_exit_status",         test_bounded_exit_status);
    g_test_add_func("/spawn_launcher/bounded_timeout_kills_group", test_bounded_timeout_kills_group);
    g_test_add_func("/spawn_launcher/bounded_leader_exits_grandchild_holds_pipe",
                    test_bounded_leader_exits_grandchild_holds_pipe);
    g_test_add_func("/spawn_launcher/bounded_should_abort",        test_bounded_should_abort);
    g_test_add_func("/spawn_launcher/bounded_output_cap",          test_bounded_output_cap);
    g_test_add_func("/spawn_launcher/bounded_cancel_kills",        test_bounded_cancel_kills);
    g_test_add_func("/spawn_launcher/sync_timeout_message_unchanged",
                    test_sync_timeout_message_unchanged);
    g_test_add_func("/spawn_launcher/env_allowlist_shared",        test_env_allowlist_shared);
    g_test_add_func("/spawn_launcher/env_allowlist_fallback",      test_env_allowlist_fallback);
    g_test_add_func("/spawn_launcher/env_allowlist_env_launcher",  test_env_allowlist_env_launcher);
    g_test_add_func("/spawn_launcher/env_allowlist_keeps_allowed_keys",
                    test_env_allowlist_keeps_allowed_keys);
    g_test_add_func("/spawn_launcher/env_allowlist_bounded",       test_env_allowlist_bounded);
}
