                                                                                              
                                                                                                  
                                                                                
                                                                        
                                                                                                      
                                                                                                  
                                                                      
                                                                                                        
                                                                        
                                                                                             
                                                                                  
                                                                         
                                                                        
  
                                 
   
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "modules/lxc/lxc_unit.h"

                                                              

                                             
static void
_rm_tree(const gchar *path)
{
    GStatBuf st;
    if (g_lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
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

                                 
static goffset
_file_size(const gchar *path)
{
    GStatBuf st;
    return g_stat(path, &st) == 0 ? (goffset)st.st_size : -1;
}

                                               
static void
_write_bytes(const gchar *path, gsize n)
{
    g_autofree gchar *buf = g_malloc(n);
    memset(buf, 'x', n);
    g_assert_true(g_file_set_contents(path, buf, (gssize)n, NULL));
}

                                                          
static void
_append(const gchar *path, const gchar *text)
{
    FILE *f = fopen(path, "a");
    g_assert_nonnull(f);
    fputs(text, f);
    fclose(f);
}

                                           
#define REAL_ERR_LINE "lxc-start c1 20261025085512.403 ERROR    network - ../src/lxc/network.c:" \
                      "netdev_configure_server_veth:719 - No such file or directory - Failed to attach \"vethX\" " \
                      "to bridge \"nosuchbr0\", bridge interface doesn't exist\n"
#define REAL_ERR_CAUSE "No such file or directory - Failed to attach \"vethX\" to bridge \"nosuchbr0\", " \
                       "bridge interface doesn't exist"

                                                                     

                                                                  
typedef struct {
    gchar       *tmp;                         
    gchar       *env_path;                                                            
    gchar       *lxc_path;                        
    gchar       *log_path;                                  
    GString     *calls;                                                    
    GString     *sysctl;                                                           
    GString     *argv_all;                                       
    const gchar * const *states;                                                       
    guint        n_states;
    guint        n_show;
    gint         show_err;                                                              
    gint         start_err;                                                              
    gint         start_rc;
    gint         reset_err;                                                                     
    guint        reset_timeout;                                
    gint         reset_rc;                                 
    const gchar *start_stderr;
    const gchar *append_on_start;                           
    gboolean     running_after_start;
    gboolean     started;
    gchar      **start_argv;
    guint        start_timeout;
    gboolean     env_at_start;
    gchar       *env_text_at_start;
    gboolean     log1_at_start;
    gint64       prep_elapsed_at_start;
    guint        sleeps;
    gboolean     sleeps_all_poll;                                           
    gboolean     sleep_advances;                             
    gint64       sleep_extra_us;                                          
    gint64       now;                                
    gint64       t0;
    guint        prep_bad;                                           
} FakeUnit;

static FakeUnit G_f;

                                  
static void
_fake_call(GString *rec, const gchar *what)
{
    if (rec->len)
        g_string_append_c(rec, ',');
    g_string_append(rec, what);
}

                                                                                          
                                                                         
static gboolean
_fake_run(const gchar * const *argv, guint timeout_sec, gchar **stdout_out, gchar **stderr_out,
          gint *exit_status, GError **error)
{
    g_autofree gchar *joined = g_strjoinv(" ", (gchar **)argv);
    g_string_append_printf(G_f.argv_all, "%s|", joined);
    g_assert_cmpstr(argv[0], ==, "systemctl");
    const gchar *verb = argv[1];
    _fake_call(G_f.calls, verb);
    _fake_call(G_f.sysctl, verb);
    if (stdout_out) *stdout_out = NULL;
    if (stderr_out) *stderr_out = NULL;
    if (exit_status) *exit_status = -1;

    if (g_strcmp0(verb, "start") == 0) {
        G_f.start_argv = g_strdupv((gchar **)argv);
        G_f.start_timeout = timeout_sec;
        G_f.prep_elapsed_at_start = G_f.now - G_f.t0;
        G_f.env_at_start = g_file_get_contents(G_f.env_path, &G_f.env_text_at_start, NULL, NULL);
        g_autofree gchar *log1 = g_strconcat(G_f.log_path, ".1", NULL);
        G_f.log1_at_start = g_file_test(log1, G_FILE_TEST_EXISTS);
        if (G_f.start_err >= 0) {
            g_set_error(error, G_IO_ERROR, G_f.start_err, "fake spawn failure");
            return FALSE;
        }
        if (G_f.append_on_start)
            _append(G_f.log_path, G_f.append_on_start);
        G_f.started = TRUE;
        if (stdout_out) *stdout_out = g_strdup("");
        if (stderr_out) *stderr_out = g_strdup(G_f.start_stderr ? G_f.start_stderr : "");
        if (exit_status) *exit_status = G_f.start_rc;
        return TRUE;
    }

    if (g_strcmp0(verb, "reset-failed") == 0) {
        G_f.reset_timeout = timeout_sec;
        if (G_f.reset_err >= 0) {
            g_set_error(error, G_IO_ERROR, G_f.reset_err, "fake spawn failure");
            return FALSE;
        }
        if (G_f.reset_rc != 0) {
            if (stdout_out) *stdout_out = g_strdup("");
            if (stderr_out) *stderr_out = g_strdup("");
            if (exit_status) *exit_status = G_f.reset_rc;
            return TRUE;
        }
    }

    gint64 left = (G_f.t0 + (gint64)PCV_LXC_UNIT_PREP_BUDGET_SEC * G_USEC_PER_SEC - G_f.now) / G_USEC_PER_SEC;
    if (left < 1)
        left = 1;
    if (timeout_sec < 1 || (gint64)timeout_sec > left)
        G_f.prep_bad++;

    if (g_strcmp0(verb, "show") == 0) {
        if (G_f.show_err >= 0) {
            g_set_error(error, G_IO_ERROR, G_f.show_err, "fake spawn failure");
            return FALSE;
        }
        const gchar *st = G_f.states[MIN(G_f.n_show, G_f.n_states - 1)];
        G_f.n_show++;
        g_auto(GStrv) kv = g_strsplit(st, " ", 2);
        if (stdout_out) *stdout_out = g_strdup_printf("ActiveState=%s\nLoadState=%s\n", kv[1], kv[0]);
    } else if (stdout_out) {
        *stdout_out = g_strdup("");
    }
    if (stderr_out) *stderr_out = g_strdup("");
    if (exit_status) *exit_status = 0;
    return TRUE;
}

                                                       
static gboolean
_fake_is_running(const gchar *lxc_path, const gchar *name)
{
    g_assert_cmpstr(lxc_path, ==, G_f.lxc_path);
    g_assert_cmpstr(name, ==, "c1");
    _fake_call(G_f.calls, "is_running");
    return G_f.started ? G_f.running_after_start : FALSE;
}

                                                  
static void
_fake_sleep(guint ms)
{
    G_f.sleeps++;
    if (ms != PCV_LXC_UNIT_POLL_MS)
        G_f.sleeps_all_poll = FALSE;
    if (G_f.sleep_advances)
        G_f.now += (gint64)ms * 1000 + G_f.sleep_extra_us;
}

                           
static gint64
_fake_now(void)
{
    return G_f.now;
}

static const PcvLxcUnitOps G_fake_ops = {
    .run        = _fake_run,
    .is_running = _fake_is_running,
    .sleep_ms   = _fake_sleep,
    .now_us     = _fake_now,
};

                                                                                    
static gboolean
_fake_setup(const gchar * const *states, guint n_states)
{
    memset(&G_f, 0, sizeof(G_f));
    G_f.tmp = g_dir_make_tmp("pcv-lxc-unit-XXXXXX", NULL);
    g_assert_nonnull(G_f.tmp);
    G_f.env_path = g_build_filename(G_f.tmp, "run", "purecvisor", "lxc-unit.env", NULL);
    G_f.lxc_path = g_build_filename(G_f.tmp, "lxc", NULL);
    g_autofree gchar *cdir = g_build_filename(G_f.lxc_path, "c1", NULL);
    g_assert_cmpint(g_mkdir_with_parents(cdir, 0700), ==, 0);
    G_f.log_path = g_build_filename(cdir, "c1.log", NULL);
    G_f.calls = g_string_new(NULL);
    G_f.sysctl = g_string_new(NULL);
    G_f.argv_all = g_string_new(NULL);
    G_f.states = states;
    G_f.n_states = n_states;
    G_f.show_err = -1;
    G_f.start_err = -1;
    G_f.reset_err = -1;
    G_f.running_after_start = TRUE;
    G_f.sleeps_all_poll = TRUE;
    G_f.now = G_f.t0 = (gint64)1000 * G_USEC_PER_SEC;
    pcv_lxc_unit_set_ops_for_test(&G_fake_ops);
    pcv_lxc_unit_set_env_path_for_test(G_f.env_path);
    if (!pcv_lxc_unit_path_valid(G_f.lxc_path)) {
        g_test_skip("임시 디렉터리 경로가 유닛 경로 규칙([A-Za-z0-9/._-])에 맞지 않는다");
        return FALSE;
    }
    return TRUE;
}

                                             
static void
_fake_teardown(void)
{
    pcv_lxc_unit_set_ops_for_test(NULL);
    pcv_lxc_unit_set_env_path_for_test(NULL);
    _rm_tree(G_f.tmp);
    g_free(G_f.tmp);
    g_free(G_f.env_path);
    g_free(G_f.lxc_path);
    g_free(G_f.log_path);
    g_string_free(G_f.calls, TRUE);
    g_string_free(G_f.sysctl, TRUE);
    g_string_free(G_f.argv_all, TRUE);
    g_strfreev(G_f.start_argv);
    g_free(G_f.env_text_at_start);
    memset(&G_f, 0, sizeof(G_f));
}

                                                                  

                                                                 
                                                                                   
static void
test_env_text_and_path_check(void)
{
    g_autofree gchar *t = pcv_lxc_unit_env_text("/var/lib/pcv-test/lxc");
    g_assert_cmpstr(t, ==, "PCV_LXC_PATH=/var/lib/pcv-test/lxc\n");
    g_assert_true(pcv_lxc_unit_path_valid("/var/lib/pcv-test/lxc"));
    g_assert_true(pcv_lxc_unit_path_valid("/srv/pcv-lxc_2.d"));
    const gchar *bad[] = { "relative/x", "/a b", "/a\nb", "/a;b", "/a$b", "", NULL };
    for (guint i = 0; bad[i]; i++) {
        g_assert_false(pcv_lxc_unit_path_valid(bad[i]));
        g_assert_null(pcv_lxc_unit_env_text(bad[i]));
    }
    g_assert_false(pcv_lxc_unit_path_valid(NULL));
    g_assert_null(pcv_lxc_unit_env_text(NULL));
}

                                                                                         
static void
test_env_write_atomic(void)
{
    g_autofree gchar *tmp = g_dir_make_tmp("pcv-lxc-env-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree gchar *dir = g_build_filename(tmp, "run", "purecvisor", NULL);
    g_autofree gchar *env = g_build_filename(dir, "lxc-unit.env", NULL);
    GError *err = NULL;

    g_assert_true(pcv_lxc_unit_env_write(env, "/var/lib/pcv-test/lxc", &err));
    g_assert_no_error(err);
    g_autofree gchar *c1 = NULL;
    g_assert_true(g_file_get_contents(env, &c1, NULL, NULL));
    g_assert_cmpstr(c1, ==, "PCV_LXC_PATH=/var/lib/pcv-test/lxc\n");

    struct utimbuf old = { .actime = 1000, .modtime = 1000 };
    g_assert_cmpint(g_utime(env, &old), ==, 0);
    GStatBuf s1, s2, s3;
    g_assert_cmpint(g_stat(env, &s1), ==, 0);
    g_assert_true(pcv_lxc_unit_env_write(env, "/var/lib/pcv-test/lxc", &err));              
    g_assert_no_error(err);
    g_assert_cmpint(g_stat(env, &s2), ==, 0);
    g_assert_cmpuint(s2.st_ino, ==, s1.st_ino);
    g_assert_cmpint(s2.st_mtime, ==, 1000);

    g_assert_true(pcv_lxc_unit_env_write(env, "/srv/lxc", &err));                             
    g_assert_no_error(err);
    g_assert_cmpint(g_stat(env, &s3), ==, 0);
    g_assert_cmpuint(s3.st_ino, !=, s1.st_ino);
    g_autofree gchar *c3 = NULL;
    g_assert_true(g_file_get_contents(env, &c3, NULL, NULL));
    g_assert_cmpstr(c3, ==, "PCV_LXC_PATH=/srv/lxc\n");

    g_assert_false(pcv_lxc_unit_env_write(env, "/srv/bad path", &err));                            
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_cmpstr(err->message, ==, "container path '/srv/bad path' must be absolute and use only [A-Za-z0-9/._-]");
    g_clear_error(&err);
    g_autofree gchar *c4 = NULL;
    g_assert_true(g_file_get_contents(env, &c4, NULL, NULL));
    g_assert_cmpstr(c4, ==, "PCV_LXC_PATH=/srv/lxc\n");

    GDir *d = g_dir_open(dir, 0, NULL);                                                             
    g_assert_nonnull(d);
    guint n = 0;
    const gchar *e;
    while ((e = g_dir_read_name(d))) {
        g_assert_cmpstr(e, ==, "lxc-unit.env");
        n++;
    }
    g_dir_close(d);
    g_assert_cmpuint(n, ==, 1);
    _rm_tree(tmp);
}

                                                                        
static void
test_log_rotate_over_limit(void)
{
    g_autofree gchar *tmp = g_dir_make_tmp("pcv-lxc-log-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree gchar *log = g_build_filename(tmp, "c1.log", NULL);
    g_autofree gchar *log1 = g_build_filename(tmp, "c1.log.1", NULL);
    GError *err = NULL;

    g_assert_true(pcv_lxc_unit_log_rotate(log, PCV_LXC_LOG_ROTATE_BYTES, &err));            
    g_assert_no_error(err);
    g_assert_false(g_file_test(log1, G_FILE_TEST_EXISTS));

    _write_bytes(log, 1048577);
    g_assert_true(g_file_set_contents(log1, "old", -1, NULL));
    g_assert_true(pcv_lxc_unit_log_rotate(log, PCV_LXC_LOG_ROTATE_BYTES, &err));
    g_assert_no_error(err);
    g_assert_false(g_file_test(log, G_FILE_TEST_EXISTS));
    g_assert_cmpint(_file_size(log1), ==, 1048577);

    _write_bytes(log, 1048576);
    g_assert_true(pcv_lxc_unit_log_rotate(log, PCV_LXC_LOG_ROTATE_BYTES, &err));
    g_assert_no_error(err);
    g_assert_cmpint(_file_size(log), ==, 1048576);
    g_assert_cmpint(_file_size(log1), ==, 1048577);
    _rm_tree(tmp);
}

                                                                            
static void
test_log_cause_extracts_error_lines(void)
{
    const gchar *one =
        "lxc-start c1 20261025085512.400 INFO     conf - ../src/lxc/conf.c:run_script:340 - ERROR word in info\n"
        REAL_ERR_LINE
        "lxc-start c1 20261025085512.404 WARN     start - ../src/lxc/start.c:lxc_abort:1037 - warn only\n";
    g_autofree gchar *c1 = pcv_lxc_unit_log_cause(one);
    g_assert_cmpstr(c1, ==, REAL_ERR_CAUSE);

    g_assert_null(pcv_lxc_unit_log_cause(
        "lxc-start c1 20261025085512.400 INFO     conf - ../src/lxc/conf.c:run_script:340 - ERROR word in info\n"));
    g_assert_null(pcv_lxc_unit_log_cause(                                                                     
        "lxc-start ERROR 20261025085512.400 INFO     conf - ../src/lxc/conf.c:run_script:340 - fine\n"));
    g_assert_null(pcv_lxc_unit_log_cause(""));
    g_assert_null(pcv_lxc_unit_log_cause(NULL));

    g_autofree gchar *nofile = pcv_lxc_unit_log_cause("lxc-start c1 20261025085512.400 ERROR    odd line\n");
    g_assert_cmpstr(nofile, ==, "odd line");                                                                  

    GString *five = g_string_new(NULL);
    for (gint i = 0; i < 5; i++)
        g_string_append_printf(five, "lxc-start c1 20261025085512.40%d ERROR    start - ../src/lxc/start.c:"
                                     "__lxc_start:20%d - cause %c\n", i, i, 'A' + i);
    g_autofree gchar *c5 = pcv_lxc_unit_log_cause(five->str);
    g_string_free(five, TRUE);
    g_assert_cmpstr(c5, ==, "cause A; cause B; cause C");

    GString *big = g_string_new("lxc-start c1 20261025085512.403 ERROR    conf - ../src/lxc/conf.c:f:1 - ");
    for (guint i = 0; i < 200; i++)
        g_string_append(big, "가");                                                                         
    g_string_append_c(big, '\n');
    g_autofree gchar *cbig = pcv_lxc_unit_log_cause(big->str);
    g_string_free(big, TRUE);
    g_assert_nonnull(cbig);
    g_assert_cmpuint(strlen(cbig), <=, PCV_LXC_UNIT_CAUSE_MAX);
    g_assert_cmpuint(strlen(cbig), >=, PCV_LXC_UNIT_CAUSE_MAX - 3);
    g_assert_true(g_utf8_validate(cbig, -1, NULL));
}

                                                                  

                                                                                             
static void
test_start_order_fresh_unit(void)
{
    static const gchar * const st[] = { "loaded inactive" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    GError *err = NULL;
    g_assert_true(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_no_error(err);
    g_assert_cmpstr(G_f.calls->str, ==, "show,start,is_running");
    g_assert_nonnull(G_f.start_argv);                                                                          
    g_assert_cmpuint(g_strv_length(G_f.start_argv), ==, 3);
    g_assert_cmpstr(G_f.start_argv[0], ==, "systemctl");
    g_assert_cmpstr(G_f.start_argv[1], ==, "start");
    g_assert_cmpstr(G_f.start_argv[2], ==, "purecvisor-lxc@c1.service");
    g_assert_true(G_f.env_at_start);
    g_autofree gchar *want_env = g_strdup_printf("PCV_LXC_PATH=%s\n", G_f.lxc_path);
    g_assert_cmpstr(G_f.env_text_at_start, ==, want_env);
    g_assert_true(g_str_has_prefix(G_f.argv_all->str,
                                   "systemctl show --property=LoadState --property=ActiveState purecvisor-lxc@c1.service|"));
    _fake_teardown();
}

                                                    
static void
test_start_resets_failed(void)
{
    static const gchar * const st[] = { "loaded failed" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    GError *err = NULL;
    g_assert_true(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_no_error(err);
    g_assert_cmpstr(G_f.calls->str, ==, "show,reset-failed,start,is_running");
    g_assert_true(g_str_has_prefix(G_f.argv_all->str, "systemctl show --property=LoadState --property=ActiveState "
                                                      "purecvisor-lxc@c1.service|"));
    g_assert_nonnull(strstr(G_f.argv_all->str, " reset-failed purecvisor-lxc@c1.service|"));                            
    _fake_teardown();
}

                                                                             
                                
static void
test_start_env_write_failure_no_start(void)
{
    static const gchar * const st[] = { "loaded inactive" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    g_autofree gchar *blocker = g_build_filename(G_f.tmp, "run", NULL);
    g_assert_true(g_file_set_contents(blocker, "x", -1, NULL));                                 
    GError *err = NULL;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(err->message, "cannot write"));
    g_assert_null(strstr(G_f.calls->str, "start"));
    g_assert_cmpstr(G_f.calls->str, ==, "show");
    g_assert_false(G_f.started);
    g_clear_error(&err);
    _fake_teardown();
}

                                                                          
static void
test_start_waits_for_inactive_after_stop(void)
{
    static const gchar * const st[] = { "loaded active", "loaded active", "loaded active", "loaded inactive" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    GError *err = NULL;
    g_assert_true(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_no_error(err);
    g_assert_cmpstr(G_f.sysctl->str, ==, "show,show,show,show,start");
    g_assert_cmpuint(G_f.sleeps, ==, 3);
    g_assert_true(G_f.sleeps_all_poll);
    g_assert_true(g_str_has_suffix(G_f.calls->str, "start,is_running"));
    _fake_teardown();
}

                                                                             
static void
test_start_gives_up_when_unit_stays_active(void)
{
    static const gchar * const st[] = { "loaded deactivating" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    GError *err = NULL;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "lxc-start failed for 'c1': purecvisor-lxc@c1.service is still deactivating "
                                      "while the container is stopped");
    g_clear_error(&err);
    g_assert_null(strstr(G_f.sysctl->str, "start"));
    g_assert_cmpuint(G_f.n_show, <=, 41);
    g_assert_cmpuint(G_f.n_show, >=, 2);
    _fake_teardown();
}

                                                                                        
                                    
static void
test_start_not_installed_fails_closed(void)
{
    static const gchar * const st[] = { "not-found inactive" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    GError *err = NULL;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_assert_cmpstr(err->message, ==, PCV_LXC_UNIT_NOT_INSTALLED);
    g_clear_error(&err);
    g_assert_cmpstr(G_f.sysctl->str, ==, "show");
    g_assert_null(strstr(G_f.argv_all->str, "lxc-start"));
    g_assert_false(G_f.started);
    _fake_teardown();
}

                                                                  
static void
test_start_not_found_on_start_stderr(void)
{
    static const gchar * const st[] = { "loaded inactive" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    G_f.start_rc = 5;
    G_f.start_stderr = "Failed to start purecvisor-lxc@c1.service: Unit purecvisor-lxc@c1.service not found.\n";
    G_f.running_after_start = FALSE;
    GError *err = NULL;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_assert_cmpstr(err->message, ==, PCV_LXC_UNIT_NOT_INSTALLED);
    g_clear_error(&err);
    g_assert_null(strstr(G_f.argv_all->str, "lxc-start"));
    _fake_teardown();
}

                                                                      
static void
test_start_failure_reports_log_cause(void)
{
    static const gchar * const st[] = { "loaded inactive" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    g_assert_true(g_file_set_contents(G_f.log_path,
        "lxc-start c1 20261024010101.000 ERROR    conf - ../src/lxc/conf.c:old:1 - stale old cause\n", -1, NULL));
    G_f.append_on_start = REAL_ERR_LINE;
    G_f.start_rc = 1;
    G_f.start_stderr = "Job for purecvisor-lxc@c1.service failed because the control process exited with error code.\n";
    G_f.running_after_start = FALSE;
    GError *err = NULL;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "lxc-start failed for 'c1': " REAL_ERR_CAUSE);
    g_assert_null(strstr(err->message, "stale old cause"));
    g_clear_error(&err);
    _fake_teardown();
}

                                                                          
static void
test_start_failure_without_log_cause_uses_stderr(void)
{
    static const gchar * const st[] = { "loaded inactive" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    G_f.start_rc = 1;
    G_f.start_stderr = "Job for purecvisor-lxc@c1.service failed.\nSee \"systemctl status\" for details.\n";
    G_f.running_after_start = FALSE;
    GError *err = NULL;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "lxc-start failed for 'c1': systemctl start exit 1: "
                                      "Job for purecvisor-lxc@c1.service failed.");
    g_clear_error(&err);
    _fake_teardown();

    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    G_f.start_rc = 1;
    G_f.running_after_start = FALSE;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_cmpstr(err->message, ==, "lxc-start failed for 'c1': systemctl start exit 1: (no stderr)");
    g_clear_error(&err);
    _fake_teardown();
}

                                                         
static void
test_start_ok_but_not_running(void)
{
    static const gchar * const st[] = { "loaded inactive" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    G_f.start_rc = 0;
    G_f.running_after_start = FALSE;
    GError *err = NULL;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "lxc-start failed for 'c1': container is not RUNNING after "
                                      "purecvisor-lxc@c1.service started");
    g_clear_error(&err);
    g_assert_cmpstr(G_f.calls->str, ==, "show,start,is_running");
    _fake_teardown();
}

                                                 
static void
test_start_invalid_path_refused(void)
{
    static const gchar * const st[] = { "loaded inactive" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    GError *err = NULL;
    g_assert_false(pcv_lxc_unit_start("/var/lib/pcv lxc", "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_cmpstr(err->message, ==, "container path '/var/lib/pcv lxc' must be absolute and use only [A-Za-z0-9/._-]");
    g_clear_error(&err);
    g_assert_cmpstr(G_f.calls->str, ==, "");
    g_assert_false(g_file_test(G_f.env_path, G_FILE_TEST_EXISTS));
    _fake_teardown();
}

                                                                               
static void
test_start_timeouts_fit_budget(void)
{
    const gint64 budget = (gint64)PCV_LXC_UNIT_PREP_BUDGET_SEC * G_USEC_PER_SEC;
    GError *err = NULL;
    g_assert_cmpint(PCV_LXC_UNIT_PREP_BUDGET_SEC + PCV_LXC_UNIT_START_TIMEOUT_SEC, <=, PCV_LXC_UNIT_START_BUDGET_SEC);

                                          
    static const gchar * const fa[] = { "loaded failed" };
    if (!_fake_setup(fa, G_N_ELEMENTS(fa))) { _fake_teardown(); return; }
    g_assert_true(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_no_error(err);
    g_assert_cmpuint(G_f.start_timeout, ==, PCV_LXC_UNIT_START_TIMEOUT_SEC);
    g_assert_cmpuint(G_f.start_timeout, ==, 26);
    g_assert_cmpuint(G_f.prep_bad, ==, 0);
    _fake_teardown();

                                                                                  
    static const gchar *slow[21];
    for (guint i = 0; i < 20; i++)
        slow[i] = "loaded active";
    slow[20] = "loaded failed";
    if (!_fake_setup(slow, G_N_ELEMENTS(slow))) { _fake_teardown(); return; }
    G_f.sleep_advances = TRUE;
    G_f.sleep_extra_us = 50 * 1000;
    g_assert_true(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_no_error(err);
    g_assert_cmpuint(G_f.start_timeout, ==, 26);
    g_assert_cmpint(G_f.prep_elapsed_at_start, <=, budget);
    g_assert_cmpint(G_f.prep_elapsed_at_start, >=, 20 * 150 * 1000);
    g_assert_cmpuint(G_f.prep_bad, ==, 0);
    g_assert_true(g_str_has_suffix(G_f.sysctl->str, "show,reset-failed,start"));
    _fake_teardown();

                                                                       
    static const gchar * const stuck[] = { "loaded deactivating" };
    if (!_fake_setup(stuck, G_N_ELEMENTS(stuck))) { _fake_teardown(); return; }
    G_f.sleep_advances = TRUE;
    G_f.sleep_extra_us = 50 * 1000;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_true(g_str_has_suffix(err->message, "is still deactivating while the container is stopped"));
    g_clear_error(&err);
    g_assert_cmpint(G_f.now - G_f.t0, <=, budget);
    g_assert_cmpuint(G_f.n_show, <, 41);
    g_assert_cmpuint(G_f.prep_bad, ==, 0);
    g_assert_null(strstr(G_f.sysctl->str, "start"));
    _fake_teardown();
}

                                                            
static void
test_start_timeout_and_cancel_messages(void)
{
    static const gchar * const st[] = { "loaded inactive" };
    GError *err = NULL;

    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    G_f.start_err = G_IO_ERROR_TIMED_OUT;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_cmpstr(err->message, ==, "lxc-start failed for 'c1': systemctl start timed out after 26s");
    g_clear_error(&err);
    _fake_teardown();

    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    G_f.start_err = G_IO_ERROR_CANCELLED;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpstr(err->message, ==, "lxc-start failed for 'c1': cancelled (daemon shutting down)");
    g_clear_error(&err);
    _fake_teardown();

    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    G_f.start_err = G_IO_ERROR_FAILED;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpstr(err->message, ==, "lxc-start failed for 'c1': fake spawn failure");
    g_clear_error(&err);
    _fake_teardown();

    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    G_f.show_err = G_IO_ERROR_TIMED_OUT;
    g_assert_false(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_cmpstr(err->message, ==, "lxc-start failed for 'c1': systemctl show timed out");
    g_clear_error(&err);
    g_assert_cmpstr(G_f.sysctl->str, ==, "show");
    _fake_teardown();
}

                                                
static void
test_start_rotates_log_first(void)
{
    static const gchar * const st[] = { "loaded inactive" };
    if (!_fake_setup(st, G_N_ELEMENTS(st))) { _fake_teardown(); return; }
    _write_bytes(G_f.log_path, (gsize)PCV_LXC_LOG_ROTATE_BYTES + 1);
    GError *err = NULL;
    g_assert_true(pcv_lxc_unit_start(G_f.lxc_path, "c1", &err));
    g_assert_no_error(err);
    g_assert_true(G_f.log1_at_start);
    g_autofree gchar *log1 = g_strconcat(G_f.log_path, ".1", NULL);
    g_assert_cmpint(_file_size(log1), ==, PCV_LXC_LOG_ROTATE_BYTES + 1);
    _fake_teardown();
}

                                                                    

                                                                                    
static gchar *
_log_tail_fixture(gboolean with_config, gchar **fallback_out)
{
    gchar *tmp = g_dir_make_tmp("pcv-lxc-logs-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree gchar *ctr = g_build_filename(tmp, "c1", NULL);
    *fallback_out = g_build_filename(tmp, "varlog", NULL);
    g_assert_cmpint(g_mkdir(ctr, 0750), ==, 0);
    g_assert_cmpint(g_mkdir(*fallback_out, 0750), ==, 0);
    if (with_config) {
        g_autofree gchar *cfg = g_build_filename(ctr, "config", NULL);
        g_assert_true(g_file_set_contents(cfg, "lxc.uts.name = c1\n", -1, NULL));
    }
    return tmp;
}

                                            
static void
_assert_lines(GPtrArray *lines, const gchar * const *want)
{
    g_assert_nonnull(lines);
    guint n = 0;
    for (; want[n]; n++) {
        g_assert_cmpuint(n, <, lines->len);
        g_assert_cmpstr(g_ptr_array_index(lines, n), ==, want[n]);
    }
    g_assert_cmpuint(lines->len, ==, n);
}

                                                                           
static void
test_log_tail_empty_when_container_has_no_log(void)
{
    g_autofree gchar *fb = NULL;
    g_autofree gchar *tmp = _log_tail_fixture(TRUE, &fb);
    gint total = -1;
    GError *err = NULL;
    GPtrArray *lines = pcv_lxc_unit_log_tail(tmp, "c1", fb, 50, &total, &err);
    g_assert_no_error(err);
    _assert_lines(lines, (const gchar * const[]){ NULL });
    g_assert_cmpint(total, ==, 0);
    g_ptr_array_unref(lines);
    _rm_tree(tmp);
}

                                                                                                             
static void
test_log_tail_reads_last_lines(void)
{
    g_autofree gchar *fb = NULL;
    g_autofree gchar *tmp = _log_tail_fixture(TRUE, &fb);
    g_autofree gchar *log = g_build_filename(tmp, "c1", "c1.log", NULL);
    g_autofree gchar *fb_log = g_build_filename(fb, "c1.log", NULL);
    g_assert_true(g_file_set_contents(log, "l1\nl2\nl3\nl4\nl5\n", -1, NULL));
    g_assert_true(g_file_set_contents(fb_log, "other\n", -1, NULL));                                   
    gint total = -1;
    GError *err = NULL;
    GPtrArray *lines = pcv_lxc_unit_log_tail(tmp, "c1", fb, 2, &total, &err);
    g_assert_no_error(err);
    _assert_lines(lines, (const gchar * const[]){ "l4", "l5", NULL });
    g_assert_cmpint(total, ==, 5);
    g_ptr_array_unref(lines);

    g_assert_true(g_file_set_contents(log, "a\nb", -1, NULL));                     
    lines = pcv_lxc_unit_log_tail(tmp, "c1", fb, 50, &total, &err);
    g_assert_no_error(err);
    _assert_lines(lines, (const gchar * const[]){ "a", "b", NULL });
    g_assert_cmpint(total, ==, 2);
    g_ptr_array_unref(lines);

    _write_bytes(log, (gsize)PCV_LXC_LOG_ROTATE_BYTES);                                                          
    _append(log, "\nlast1\nlast2\n");
    lines = pcv_lxc_unit_log_tail(tmp, "c1", fb, 1, &total, &err);
    g_assert_no_error(err);
    _assert_lines(lines, (const gchar * const[]){ "last2", NULL });
    g_assert_cmpint(total, ==, 2);
    g_ptr_array_unref(lines);
    _rm_tree(tmp);
}

                                                             
static void
test_log_tail_missing_container_is_error(void)
{
    g_autofree gchar *tmp = g_dir_make_tmp("pcv-lxc-logs-XXXXXX", NULL);
    g_assert_nonnull(tmp);
    gint total = 7;
    GError *err = NULL;
    g_assert_null(pcv_lxc_unit_log_tail(tmp, "nosuch", tmp, 50, &total, &err));
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&err);
    _rm_tree(tmp);
}

                                                                                     
static void
test_log_tail_fallback_dir(void)
{
    g_autofree gchar *fb = NULL;
    g_autofree gchar *tmp = _log_tail_fixture(FALSE, &fb);
    g_autofree gchar *fb_log = g_build_filename(fb, "c1.log", NULL);
    g_assert_true(g_file_set_contents(fb_log, "f1\nf2\n", -1, NULL));
    gint total = -1;
    GError *err = NULL;
    GPtrArray *lines = pcv_lxc_unit_log_tail(tmp, "c1", fb, 50, &total, &err);
    g_assert_no_error(err);
    _assert_lines(lines, (const gchar * const[]){ "f1", "f2", NULL });
    g_assert_cmpint(total, ==, 2);
    g_ptr_array_unref(lines);

    g_autofree gchar *secret = g_build_filename(tmp, "secret", NULL);
    g_autofree gchar *log = g_build_filename(tmp, "c1", "c1.log", NULL);
    g_assert_true(g_file_set_contents(secret, "s1\n", -1, NULL));
    g_assert_cmpint(symlink(secret, log), ==, 0);
    lines = pcv_lxc_unit_log_tail(tmp, "c1", fb, 50, &total, &err);
    g_assert_no_error(err);
    _assert_lines(lines, (const gchar * const[]){ "f1", "f2", NULL });
    g_ptr_array_unref(lines);
    _rm_tree(tmp);
}

                                                                          
void test_lxc_unit_register(void);
                                                                       

                                                                                           
static void
test_reset_failed_after_destroy_argv(void)
{
    const gchar *st[] = { "loaded inactive" };
    if (!_fake_setup(st, 1)) { _fake_teardown(); return; }
    pcv_lxc_unit_reset_failed_best_effort("ct1");
    g_assert_cmpstr(G_f.argv_all->str, ==, "systemctl reset-failed purecvisor-lxc@ct1.service|");
    g_assert_cmpstr(G_f.sysctl->str, ==, "reset-failed");
    g_assert_cmpuint(G_f.reset_timeout, ==, PCV_LXC_UNIT_PREP_BUDGET_SEC);
    _fake_teardown();
}

                                        
static void
test_reset_failed_nonzero_ignored(void)
{
    const gchar *st[] = { "loaded inactive" };
    if (!_fake_setup(st, 1)) { _fake_teardown(); return; }
    G_f.reset_rc = 1;
    pcv_lxc_unit_reset_failed_best_effort("ct1");
    g_assert_cmpstr(G_f.sysctl->str, ==, "reset-failed");
    _fake_teardown();
}

                                                          
static void
test_reset_failed_run_error_warns(void)
{
    const gchar *st[] = { "loaded inactive" };
    if (!_fake_setup(st, 1)) { _fake_teardown(); return; }
    G_f.reset_err = G_IO_ERROR_TIMED_OUT;
    g_test_expect_message("lxc_unit", G_LOG_LEVEL_WARNING,
                          "*purecvisor-lxc@ct1.service: reset-failed after destroy failed: fake spawn failure*");
    pcv_lxc_unit_reset_failed_best_effort("ct1");
    g_test_assert_expected_messages();
    g_assert_cmpstr(G_f.sysctl->str, ==, "reset-failed");
    _fake_teardown();
}

void
test_lxc_unit_register(void)
{
    g_test_add_func("/lxc_unit/env_text_and_path_check", test_env_text_and_path_check);
    g_test_add_func("/lxc_unit/env_write_atomic", test_env_write_atomic);
    g_test_add_func("/lxc_unit/log_rotate_over_limit", test_log_rotate_over_limit);
    g_test_add_func("/lxc_unit/log_cause_extracts_error_lines", test_log_cause_extracts_error_lines);
    g_test_add_func("/lxc_unit/start_order_fresh_unit", test_start_order_fresh_unit);
    g_test_add_func("/lxc_unit/start_resets_failed", test_start_resets_failed);
    g_test_add_func("/lxc_unit/start_env_write_failure_no_start", test_start_env_write_failure_no_start);
    g_test_add_func("/lxc_unit/start_waits_for_inactive_after_stop", test_start_waits_for_inactive_after_stop);
    g_test_add_func("/lxc_unit/start_gives_up_when_unit_stays_active", test_start_gives_up_when_unit_stays_active);
    g_test_add_func("/lxc_unit/start_not_installed_fails_closed", test_start_not_installed_fails_closed);
    g_test_add_func("/lxc_unit/start_not_found_on_start_stderr", test_start_not_found_on_start_stderr);
    g_test_add_func("/lxc_unit/start_failure_reports_log_cause", test_start_failure_reports_log_cause);
    g_test_add_func("/lxc_unit/start_failure_without_log_cause_uses_stderr",
                    test_start_failure_without_log_cause_uses_stderr);
    g_test_add_func("/lxc_unit/start_ok_but_not_running", test_start_ok_but_not_running);
    g_test_add_func("/lxc_unit/start_invalid_path_refused", test_start_invalid_path_refused);
    g_test_add_func("/lxc_unit/start_timeouts_fit_budget", test_start_timeouts_fit_budget);
    g_test_add_func("/lxc_unit/start_timeout_and_cancel_messages", test_start_timeout_and_cancel_messages);
    g_test_add_func("/lxc_unit/start_rotates_log_first", test_start_rotates_log_first);
    g_test_add_func("/lxc_unit/reset_failed_after_destroy_argv", test_reset_failed_after_destroy_argv);
    g_test_add_func("/lxc_unit/reset_failed_nonzero_ignored", test_reset_failed_nonzero_ignored);
    g_test_add_func("/lxc_unit/reset_failed_run_error_warns", test_reset_failed_run_error_warns);
    g_test_add_func("/lxc_unit/log_tail_empty_when_container_has_no_log", test_log_tail_empty_when_container_has_no_log);
    g_test_add_func("/lxc_unit/log_tail_reads_last_lines", test_log_tail_reads_last_lines);
    g_test_add_func("/lxc_unit/log_tail_missing_container_is_error", test_log_tail_missing_container_is_error);
    g_test_add_func("/lxc_unit/log_tail_fallback_dir", test_log_tail_fallback_dir);
}
