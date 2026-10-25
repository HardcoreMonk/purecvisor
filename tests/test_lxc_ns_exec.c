#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>

#include "modules/lxc/lxc_ns_exec.h"
#include "utils/pcv_spawn.h"

static void
test_attach_argv_shape(void)
{
    const gchar *cmd[] = { "ip", "-4", "-o", "addr", "show", NULL };
    const gchar *want[] = { "lxc-attach", "--clear-env", "--set-var",
                            "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "-P", "/var/lib/lxc",
                            "-n", "c1", "--", "ip", "-4", "-o", "addr", "show", NULL };
    gchar **argv = pcv_lxc_attach_argv("/var/lib/lxc", "c1", cmd);
    g_assert_nonnull(argv);
    g_assert_cmpuint(g_strv_length(argv), ==, G_N_ELEMENTS(want) - 1);
    for (guint i = 0; want[i]; i++)
        g_assert_cmpstr(argv[i], ==, want[i]);
    for (guint i = 0; argv[i]; i++)
        g_assert_cmpstr(argv[i], !=, "/bin/sh");
    g_strfreev(argv);
}

static void
test_attach_argv_keeps_metachars_literal(void)
{
    const gchar *cmd[] = { "echo", "a;b $(x)", NULL };
    gchar **argv = pcv_lxc_attach_argv("/var/lib/lxc", "c1", cmd);
    g_assert_nonnull(argv);
    guint n = g_strv_length(argv);
    g_assert_cmpuint(n, ==, 11);
    g_assert_cmpstr(argv[n - 3], ==, "--");
    g_assert_cmpstr(argv[n - 2], ==, "echo");
    g_assert_cmpstr(argv[n - 1], ==, "a;b $(x)");
    g_strfreev(argv);
}

static gint g_ns_abort_flag;

static gboolean
_ns_abort_after_flag(void)
{
    return g_atomic_int_get(&g_ns_abort_flag) != 0;
}

static gpointer
_ns_raise_abort(gpointer data)
{
    (void)data;
    g_usleep(500 * 1000);
    g_atomic_int_set(&g_ns_abort_flag, 1);
    return NULL;
}

static void
test_ns_run_aborts_when_terminating(void)
{
    pcv_spawn_launcher_init();
    g_atomic_int_set(&g_ns_abort_flag, 0);
    pcv_lxc_ns_set_abort_fn_for_test(_ns_abort_after_flag);

    const gchar *argv[] = { "/bin/sleep", "60", NULL };
    gchar *out = NULL, *err_out = NULL;
    gint status = -7;
    GError *err = NULL;
    GThread *t = g_thread_new("ns-abort", _ns_raise_abort, NULL);
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = pcv_lxc_ns_run(argv, 30, PCV_LXC_NS_OUTPUT_MAX, &out, &err_out, &status, &err);
    gdouble elapsed = (g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC;
    g_thread_join(t);
    g_assert_false(ok);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpfloat(elapsed, <, 3.0);
    g_assert_null(out);
    g_assert_null(err_out);
    g_assert_cmpint(status, ==, -1);
    g_clear_error(&err);

    pcv_lxc_ns_set_abort_fn_for_test(NULL);
    g_atomic_int_set(&g_ns_abort_flag, 0);
    const gchar *ok_argv[] = { "/bin/true", NULL };
    status = -7;
    g_assert_true(pcv_lxc_ns_run(ok_argv, 5, PCV_LXC_NS_OUTPUT_MAX, &out, &err_out, &status, &err));
    g_assert_no_error(err);
    g_assert_cmpint(status, ==, 0);
    g_assert_nonnull(out);
    g_assert_nonnull(err_out);
    g_free(out);
    g_free(err_out);
    pcv_spawn_launcher_shutdown();
}

typedef struct {
    gchar *base;
    gchar *bin;
    gchar *log;
    gchar *saved_path;
} NsMock;

static void
_ns_mock_begin(NsMock *m)
{
    m->base = g_dir_make_tmp("pcv-ns-exec-XXXXXX", NULL);
    g_assert_nonnull(m->base);
    m->bin = g_build_filename(m->base, "bin", NULL);
    m->log = g_build_filename(m->base, "calls.log", NULL);
    g_assert_cmpint(g_mkdir_with_parents(m->bin, 0700), ==, 0);
    m->saved_path = g_strdup(g_getenv("PATH"));
    gchar *path = g_strdup_printf("%s:%s", m->bin, m->saved_path ? m->saved_path : "");
    g_setenv("PATH", path, TRUE);
    g_free(path);
}

static void
_ns_mock_tool(const NsMock *m, const gchar *name, const gchar *body)
{
    gchar *file = g_build_filename(m->bin, name, NULL);
    gchar *qlog = g_shell_quote(m->log);
    gchar *script = g_strdup_printf("#!/bin/sh\nprintf '%%s %%s\\n' \"${0##*/}\" \"$*\" >> %s\n%s",
                                    qlog, body);
    g_assert_true(g_file_set_contents(file, script, -1, NULL));
    g_assert_cmpint(g_chmod(file, 0755), ==, 0);
    g_free(script);
    g_free(qlog);
    g_free(file);
}

static gchar *
_ns_mock_calls(const NsMock *m)
{
    gchar *text = NULL;
    if (!g_file_get_contents(m->log, &text, NULL, NULL))
        text = g_strdup("");
    return text;
}

static void
_ns_rm_tree(const gchar *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    if (d) {
        const gchar *e;
        while ((e = g_dir_read_name(d))) {
            gchar *child = g_build_filename(path, e, NULL);
            _ns_rm_tree(child);
            g_free(child);
        }
        g_dir_close(d);
        g_rmdir(path);
    } else {
        g_unlink(path);
    }
}

static void
_ns_mock_end(NsMock *m)
{
    if (m->saved_path) g_setenv("PATH", m->saved_path, TRUE); else g_unsetenv("PATH");
    _ns_rm_tree(m->base);
    g_free(m->saved_path);
    g_free(m->log);
    g_free(m->bin);
    g_free(m->base);
}

static gchar *
_ns_step_line(const gchar *step)
{
    gchar **argv = g_strsplit(step, "\x1f", -1);
    gchar *line = g_strjoinv(" ", argv);
    g_strfreev(argv);
    return line;
}

static void
test_wire_steps_use_init_pid(void)
{
    gchar **steps = pcv_lxc_ns_nic_wire_steps("eth1-host", "eth1", "br0", 4242);
    g_assert_nonnull(steps);
    g_assert_cmpuint(g_strv_length(steps), ==, 4);
    const gchar *want[] = {
        "ip link add eth1-host type veth peer name eth1 netns 4242",
        "ip link set eth1-host master br0",
        "ip link set eth1-host up",
        "nsenter -t 4242 -n -- ip link set eth1 up",
    };
    for (guint i = 0; i < G_N_ELEMENTS(want); i++) {
        gchar *line = _ns_step_line(steps[i]);
        g_assert_cmpstr(line, ==, want[i]);
        g_assert_null(strstr(line, "cgroup.procs"));
        g_free(line);
        gchar **argv = g_strsplit(steps[i], "\x1f", -1);
        for (guint j = 0; argv[j]; j++) {
            g_assert_cmpstr(argv[j], !=, "/bin/sh");
            g_assert_cmpstr(argv[j], !=, "sh");
            g_assert_cmpstr(argv[j], !=, "-c");
        }
        g_strfreev(argv);
    }
    g_strfreev(steps);
}

static void
test_wire_rejects_bad_pid(void)
{
    pcv_spawn_launcher_init();
    NsMock m;
    _ns_mock_begin(&m);
    _ns_mock_tool(&m, "ip", "exit 0\n");
    _ns_mock_tool(&m, "nsenter", "exit 0\n");
    const pid_t bad[] = { 1, 0, -1 };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        GError *err = NULL;
        g_assert_false(pcv_lxc_ns_nic_wire("pcvzt9-host", "pcvzt9", "br0", bad[i], &err));
        g_assert_nonnull(err);
        g_assert_cmpstr(err->message, ==, "config saved; live wiring not confirmed: no init pid");
        g_clear_error(&err);
    }
    gchar *calls = _ns_mock_calls(&m);
    g_assert_cmpstr(calls, ==, "");
    g_free(calls);
    _ns_mock_end(&m);
    pcv_spawn_launcher_shutdown();
}

static void
test_wire_failure_cleans_up(void)
{
    pcv_spawn_launcher_init();
    NsMock m;
    _ns_mock_begin(&m);
    gchar *flag = g_build_filename(m.base, "fail-master", NULL);
    gchar *qflag = g_shell_quote(flag);
    gchar *ip_body = g_strdup_printf(
        "case \"$*\" in *master*) [ -e %s ] && { echo 'boom master' >&2; exit 1; } ;; esac\nexit 0\n", qflag);
    _ns_mock_tool(&m, "ip", ip_body);
    _ns_mock_tool(&m, "nsenter", "exit 0\n");

    g_assert_true(g_file_set_contents(flag, "", -1, NULL));
    GError *err = NULL;
    g_assert_false(pcv_lxc_ns_nic_wire("pcvzt9-host", "pcvzt9", "br0", 4242, &err));
    g_assert_nonnull(err);
    g_assert_true(g_str_has_prefix(err->message,
        "config saved; live wiring not confirmed: ip link set pcvzt9-host master br0: "));
    g_assert_nonnull(strstr(err->message, "boom master"));
    g_clear_error(&err);
    gchar *calls = _ns_mock_calls(&m);
    g_assert_cmpstr(calls, ==,
        "ip link add pcvzt9-host type veth peer name pcvzt9 netns 4242\n"
        "ip link set pcvzt9-host master br0\n"
        "ip link del pcvzt9-host\n");
    g_free(calls);

    g_assert_cmpint(g_unlink(flag), ==, 0);
    g_assert_cmpint(g_unlink(m.log), ==, 0);
    g_assert_true(pcv_lxc_ns_nic_wire("pcvzt9-host", "pcvzt9", "br0", 4242, &err));
    g_assert_no_error(err);
    calls = _ns_mock_calls(&m);
    g_assert_cmpstr(calls, ==,
        "ip link add pcvzt9-host type veth peer name pcvzt9 netns 4242\n"
        "ip link set pcvzt9-host master br0\n"
        "ip link set pcvzt9-host up\n"
        "nsenter -t 4242 -n -- ip link set pcvzt9 up\n");
    g_free(calls);

    g_free(ip_body);
    g_free(qflag);
    g_free(flag);
    _ns_mock_end(&m);
    pcv_spawn_launcher_shutdown();
}

static GString *g_ns_seen;

static void
_ns_observe(const gchar * const *argv, PcvChildCapabilityProfile profile)
{
    gchar *line = g_strjoinv(" ", (gchar **)argv);
    g_string_append_printf(g_ns_seen, "%s %s\n",
                           profile == PCV_CHILD_CAP_RUNTIME ? "RUNTIME" :
                           profile == PCV_CHILD_CAP_BASE ? "BASE" : "OTHER", line);
    g_free(line);
}

static void
test_wire_step_profiles(void)
{
    pcv_spawn_launcher_init();
    NsMock m;
    _ns_mock_begin(&m);
    gchar *flag = g_build_filename(m.base, "fail-master", NULL);
    gchar *qflag = g_shell_quote(flag);
    gchar *ip_body = g_strdup_printf(
        "case \"$*\" in *master*) [ -e %s ] && exit 1 ;; esac\nexit 0\n", qflag);
    _ns_mock_tool(&m, "ip", ip_body);
    _ns_mock_tool(&m, "nsenter", "exit 0\n");
    g_ns_seen = g_string_new(NULL);
    pcv_lxc_ns_set_spawn_observer_for_test(_ns_observe);

    GError *err = NULL;
    g_assert_true(pcv_lxc_ns_nic_wire("pcvzt9-host", "pcvzt9", "br0", 4242, &err));
    g_assert_no_error(err);
    g_assert_cmpstr(g_ns_seen->str, ==,
        "RUNTIME ip link add pcvzt9-host type veth peer name pcvzt9 netns 4242\n"
        "BASE ip link set pcvzt9-host master br0\n"
        "BASE ip link set pcvzt9-host up\n"
        "RUNTIME nsenter -t 4242 -n -- ip link set pcvzt9 up\n");

    g_string_truncate(g_ns_seen, 0);
    g_assert_true(g_file_set_contents(flag, "", -1, NULL));
    g_assert_false(pcv_lxc_ns_nic_wire("pcvzt9-host", "pcvzt9", "br0", 4242, &err));
    g_assert_nonnull(err);
    g_clear_error(&err);
    g_assert_cmpstr(g_ns_seen->str, ==,
        "RUNTIME ip link add pcvzt9-host type veth peer name pcvzt9 netns 4242\n"
        "BASE ip link set pcvzt9-host master br0\n"
        "BASE ip link del pcvzt9-host\n");

    pcv_lxc_ns_set_spawn_observer_for_test(NULL);
    g_string_free(g_ns_seen, TRUE);
    g_ns_seen = NULL;
    g_free(ip_body);
    g_free(qflag);
    g_free(flag);
    _ns_mock_end(&m);
    pcv_spawn_launcher_shutdown();
}

static void
test_wire_step1_exit_skips_cleanup(void)
{
    pcv_spawn_launcher_init();
    NsMock m;
    _ns_mock_begin(&m);
    gchar *fail = g_build_filename(m.base, "add-fail", NULL);
    gchar *hang = g_build_filename(m.base, "add-hang", NULL);
    gchar *qfail = g_shell_quote(fail);
    gchar *qhang = g_shell_quote(hang);
    gchar *ip_body = g_strdup_printf(
        "case \"$*\" in 'link add'*)\n"
        "  [ -e %s ] && { echo 'RTNETLINK answers: File exists' >&2; exit 2; }\n"
        "  [ -e %s ] && sleep 60 ;;\n"
        "esac\nexit 0\n", qfail, qhang);
    _ns_mock_tool(&m, "ip", ip_body);
    _ns_mock_tool(&m, "nsenter", "exit 0\n");

    g_assert_true(g_file_set_contents(fail, "", -1, NULL));
    GError *err = NULL;
    g_assert_false(pcv_lxc_ns_nic_wire("pcvzt9-host", "pcvzt9", "br0", 4242, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==,
        "config saved; live wiring not confirmed: "
        "ip link add pcvzt9-host type veth peer name pcvzt9 netns 4242: exit 2: RTNETLINK answers: File exists");
    g_clear_error(&err);
    gchar *calls = _ns_mock_calls(&m);
    g_assert_cmpstr(calls, ==, "ip link add pcvzt9-host type veth peer name pcvzt9 netns 4242\n");
    g_free(calls);

    g_assert_cmpint(g_unlink(fail), ==, 0);
    g_assert_cmpint(g_unlink(m.log), ==, 0);
    g_assert_true(g_file_set_contents(hang, "", -1, NULL));
    gint64 t0 = g_get_monotonic_time();
    g_assert_false(pcv_lxc_ns_nic_wire("pcvzt9-host", "pcvzt9", "br0", 4242, &err));
    gdouble elapsed = (g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC;
    g_assert_cmpfloat(elapsed, <, 10.0);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_true(g_str_has_prefix(err->message,
        "config saved; live wiring not confirmed: ip link add pcvzt9-host "));
    g_clear_error(&err);
    calls = _ns_mock_calls(&m);
    g_assert_cmpstr(calls, ==,
        "ip link add pcvzt9-host type veth peer name pcvzt9 netns 4242\n"
        "ip link del pcvzt9-host\n");
    g_free(calls);

    g_free(ip_body);
    g_free(qhang);
    g_free(qfail);
    g_free(hang);
    g_free(fail);
    _ns_mock_end(&m);
    pcv_spawn_launcher_shutdown();
}

static void
test_wire_refuses_existing_host_if(void)
{
    pcv_spawn_launcher_init();
    NsMock m;
    _ns_mock_begin(&m);
    _ns_mock_tool(&m, "ip", "exit 0\n");
    _ns_mock_tool(&m, "nsenter", "exit 0\n");
    GError *err = NULL;
    g_assert_false(pcv_lxc_ns_nic_wire("lo", "pcvzt9", "br0", 4242, &err));
    g_assert_nonnull(err);
    g_assert_true(g_str_has_prefix(err->message, "config saved; live wiring not confirmed: "));
    g_assert_nonnull(strstr(err->message, "already exists"));
    g_clear_error(&err);
    gchar *calls = _ns_mock_calls(&m);
    g_assert_cmpstr(calls, ==, "");
    g_free(calls);
    _ns_mock_end(&m);
    pcv_spawn_launcher_shutdown();
}

static void
test_remove_warning_on_nonzero(void)
{
    pcv_spawn_launcher_init();
    NsMock m;
    _ns_mock_begin(&m);
    gchar *warning = NULL;
    GError *err = NULL;

    _ns_mock_tool(&m, "lxc-attach", "exit 0\n");
    g_assert_true(pcv_lxc_ns_nic_remove("/var/lib/lxc", "c1", "eth1", &warning, &err));
    g_assert_no_error(err);
    g_assert_null(warning);
    gchar *calls = _ns_mock_calls(&m);
    g_assert_nonnull(strstr(calls, "-n c1 -- ip link del eth1\n"));
    g_assert_nonnull(strstr(calls, "lxc-attach --clear-env "));
    g_free(calls);

    _ns_mock_tool(&m, "lxc-attach", "echo 'Cannot find device \"eth1\"' >&2\nexit 2\n");
    g_assert_true(pcv_lxc_ns_nic_remove("/var/lib/lxc", "c1", "eth1", &warning, &err));
    g_assert_no_error(err);
    g_assert_nonnull(warning);
    g_assert_cmpstr(warning, !=, "");
    g_free(warning);
    warning = NULL;

    _ns_mock_tool(&m, "lxc-attach", "sleep 60\n");
    gint64 t0 = g_get_monotonic_time();
    g_assert_false(pcv_lxc_ns_nic_remove("/var/lib/lxc", "c1", "eth1", &warning, &err));
    gdouble elapsed = (g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC;
    g_assert_cmpfloat(elapsed, <, 8.0);
    g_assert_nonnull(err);
    g_assert_true(g_str_has_prefix(err->message,
                                   "config removed; live interface removal not confirmed:"));
    g_assert_null(warning);
    g_clear_error(&err);

    _ns_mock_end(&m);
    pcv_spawn_launcher_shutdown();
}

static void
test_ip4_query_timeout(void)
{
    pcv_spawn_launcher_init();
    NsMock m;
    _ns_mock_begin(&m);
    gchar *out = NULL;
    GError *err = NULL;

    _ns_mock_tool(&m, "lxc-attach",
        "case \"$*\" in *'-- ip -4 -o addr show') echo '2: eth0    inet 192.0.2.42/24 brd 192.0.2.255' ;;"
        " *) exit 9 ;; esac\n");
    g_assert_true(pcv_lxc_ns_ip4_query("/var/lib/lxc", "c1", &out, &err));
    g_assert_no_error(err);
    g_assert_nonnull(strstr(out, "inet 192.0.2.42/24"));
    g_free(out);
    out = NULL;

    _ns_mock_tool(&m, "lxc-attach", "sleep 60\n");
    gint64 t0 = g_get_monotonic_time();
    g_assert_false(pcv_lxc_ns_ip4_query("/var/lib/lxc", "c1", &out, &err));
    gdouble elapsed = (g_get_monotonic_time() - t0) / (gdouble)G_USEC_PER_SEC;
    g_assert_cmpfloat(elapsed, <, 6.0);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_null(out);
    g_clear_error(&err);

    _ns_mock_end(&m);
    pcv_spawn_launcher_shutdown();
}

void
test_lxc_ns_exec_register(void)
{
    g_test_add_func("/lxc_ns_exec/attach_argv_shape", test_attach_argv_shape);
    g_test_add_func("/lxc_ns_exec/attach_argv_keeps_metachars_literal",
                    test_attach_argv_keeps_metachars_literal);
    g_test_add_func("/lxc_ns_exec/ns_run_aborts_when_terminating",
                    test_ns_run_aborts_when_terminating);
    g_test_add_func("/lxc_ns_exec/wire_steps_use_init_pid", test_wire_steps_use_init_pid);
    g_test_add_func("/lxc_ns_exec/wire_rejects_bad_pid", test_wire_rejects_bad_pid);
    g_test_add_func("/lxc_ns_exec/wire_failure_cleans_up", test_wire_failure_cleans_up);
    g_test_add_func("/lxc_ns_exec/wire_step_profiles", test_wire_step_profiles);
    g_test_add_func("/lxc_ns_exec/wire_step1_exit_skips_cleanup", test_wire_step1_exit_skips_cleanup);
    g_test_add_func("/lxc_ns_exec/wire_refuses_existing_host_if", test_wire_refuses_existing_host_if);
    g_test_add_func("/lxc_ns_exec/remove_warning_on_nonzero", test_remove_warning_on_nonzero);
    g_test_add_func("/lxc_ns_exec/ip4_query_timeout", test_ip4_query_timeout);
}
