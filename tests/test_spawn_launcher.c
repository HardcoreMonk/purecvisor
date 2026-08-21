                                                                                        
                                                                                                           
                                                                         
                                                                
                             
                              
  
                                                               
  
                 
                                               
                                                                     
                       
                                                          
  
                                            
  
                                                                     
                                     
   

#include <glib.h>
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
}
