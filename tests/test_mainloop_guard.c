   
                              
                                                                                    
  
                           
                                                               
                                                                                  
                                                                          
                                                                                    
                                                                                  
                                                                     
                                                             
                                                                       
                                                                                  
                                                                            
                                                  
                                                              
                                          
  
                       
                                                                
                                                            
                                                             
   
#include <errno.h>
#include <glib.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils/pcv_config.h"
#include "utils/pcv_mainloop_guard.h"

#define GUARD_DOM "mainloop_guard"
#define SEC(x) ((gint64)(x) * G_USEC_PER_SEC)
                                               
#define FAKE_BASE_US SEC(1000)

                                                                  
static gint64   s_now_us;
static gint     s_exit_calls;
static gint     s_exit_code;
static gint     s_on_signal_calls;
static GThread *s_on_signal_thread;

                                 
                                        
static gint64
_fake_now(void)
{
    return s_now_us;
}

                                    
                                     
static void
_fake_exit(gint code)
{
    s_exit_calls++;
    s_exit_code = code;
}

static const PcvMainloopGuardOps k_fake_ops = { _fake_now, _fake_exit };

                                                                       
                                    
static gboolean
_fake_on_signal(gpointer unused)
{
    (void)unused;
    s_on_signal_calls++;
    s_on_signal_thread = g_thread_self();
    return G_SOURCE_REMOVE;
}

                                    
typedef struct {
    GMainContext *ctx;
    GMainLoop    *loop;
    GPtrArray    *logs;                                                  
    guint         log_handler;
} GuardFx;

                         
                                                     
static void
_capture_log(const gchar *dom, GLogLevelFlags lvl, const gchar *msg, gpointer data)
{
    (void)dom;
    GPtrArray *logs = data;
    const gchar *tag = (lvl & G_LOG_LEVEL_CRITICAL) ? "C"
                     : (lvl & G_LOG_LEVEL_WARNING) ? "W" : "I";
    g_ptr_array_add(logs, g_strdup_printf("%s|%s", tag, msg));
}

                            
static gboolean
_has_log(GPtrArray *logs, const gchar *exact)
{
    for (guint i = 0; i < logs->len; i++)
        if (g_strcmp0(g_ptr_array_index(logs, i), exact) == 0) return TRUE;
    return FALSE;
}

                            
static guint
_count_logs(GPtrArray *logs, const gchar *prefix)
{
    guint n = 0;
    for (guint i = 0; i < logs->len; i++)
        if (g_str_has_prefix(g_ptr_array_index(logs, i), prefix)) n++;
    return n;
}

                                                                          
                                                
static void
fx_setup(GuardFx *fx, gconstpointer unused)
{
    (void)unused;
    s_now_us = FAKE_BASE_US;
    s_exit_calls = 0;
    s_exit_code = -1;
    s_on_signal_calls = 0;
    s_on_signal_thread = NULL;
    pcv_mainloop_guard_set_ops_for_test(&k_fake_ops);
    fx->ctx = g_main_context_new();
    fx->loop = g_main_loop_new(fx->ctx, FALSE);
    fx->logs = g_ptr_array_new_with_free_func(g_free);
    fx->log_handler = g_log_set_handler(GUARD_DOM,
        G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_FATAL,
        _capture_log, fx->logs);
    pcv_mainloop_guard_start_for_test(fx->loop, _fake_on_signal);
}

                                                                                 
static void
fx_teardown(GuardFx *fx, gconstpointer unused)
{
    (void)unused;
    g_log_remove_handler(GUARD_DOM, fx->log_handler);
    pcv_mainloop_guard_set_ops_for_test(NULL);
    g_ptr_array_unref(fx->logs);
    g_main_loop_unref(fx->loop);
    g_main_context_unref(fx->ctx);
}

                                                                  

                                                                          
                                   
static void
test_stall_warn_at_10s(void)
{
    PcvStallState st = {0};
    const gint64 beat = FAKE_BASE_US;
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat + SEC(9), beat), ==, PCV_STALL_NONE);
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat + SEC(10) - 1, beat), ==, PCV_STALL_NONE);
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat + SEC(10), beat), ==, PCV_STALL_WARN);
    g_assert_cmpint(st.stall_start_us, ==, beat);
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat + SEC(11), beat), ==, PCV_STALL_NONE);
}

                                                                             
                                                  
static void
test_stall_crit_at_60s(void)
{
    PcvStallState st = {0};
    const gint64 beat = FAKE_BASE_US;
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat + SEC(10), beat), ==, PCV_STALL_WARN);
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat + SEC(59), beat), ==, PCV_STALL_NONE);
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat + SEC(60), beat), ==, PCV_STALL_CRIT);
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat + SEC(61), beat), ==, PCV_STALL_NONE);
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat + SEC(600), beat), ==, PCV_STALL_NONE);

    PcvStallState jump = {0};
    g_assert_cmpint(pcv_mainloop_stall_classify(&jump, beat + SEC(60), beat), ==, PCV_STALL_CRIT);
    g_assert_cmpint(pcv_mainloop_stall_classify(&jump, beat + SEC(61), beat), ==, PCV_STALL_NONE);
}

                                                                   
                                                     
                                                               
static void
test_recovered_once(void)
{
    PcvStallState st = {0};
    const gint64 beat = FAKE_BASE_US;
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat + SEC(10), beat), ==, PCV_STALL_WARN);
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat + SEC(14), beat), ==, PCV_STALL_NONE);
    const gint64 beat2 = beat + SEC(15);
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat2 + SEC(1), beat2), ==, PCV_STALL_RECOVERED);
    g_assert_cmpint(beat2 - st.stall_start_us, ==, SEC(15));
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat2 + SEC(2), beat2), ==, PCV_STALL_NONE);
    g_assert_cmpint(pcv_mainloop_stall_classify(&st, beat2 + SEC(10), beat2), ==, PCV_STALL_WARN);
    g_assert_cmpint(st.stall_start_us, ==, beat2);

    PcvStallState quiet = {0};
    g_assert_cmpint(pcv_mainloop_stall_classify(&quiet, beat + SEC(5), beat), ==, PCV_STALL_NONE);
    g_assert_cmpint(pcv_mainloop_stall_classify(&quiet, beat + SEC(6), beat + SEC(6)), ==, PCV_STALL_NONE);
}

                                                                 

                                                                             
                                                                      
                              
                                                           
static void
test_deadline_fires_when_not_picked_up(GuardFx *fx, gconstpointer unused)
{
    (void)unused;
    const gint timeout_s = pcv_config_get_drain_timeout();
    g_assert_false(pcv_mainloop_deadline_expired(0, SEC(1)));
    g_assert_false(pcv_mainloop_deadline_expired(SEC(10), SEC(10) - 1));
    g_assert_true(pcv_mainloop_deadline_expired(SEC(10), SEC(10)));

    pcv_mainloop_guard_raise_for_test(SIGTERM);
    pcv_mainloop_guard_tick_for_test();
    g_assert_cmpint(s_exit_calls, ==, 0);

    s_now_us = FAKE_BASE_US + SEC(timeout_s) - 1;
    pcv_mainloop_guard_tick_for_test();
    g_assert_cmpint(s_exit_calls, ==, 0);

    s_now_us = FAKE_BASE_US + SEC(timeout_s);
    pcv_mainloop_guard_tick_for_test();
    g_assert_cmpint(s_exit_calls, ==, 1);
    g_assert_cmpint(s_exit_code, ==, 1);
    g_autofree gchar *want = g_strdup_printf(
        "C|stop signal not handled by main loop within %d s (current RPC: none); exiting unsuccessfully",
        timeout_s);
    g_assert_true(_has_log(fx->logs, want));
    g_assert_cmpuint(_count_logs(fx->logs, "C|stop signal not handled"), ==, 1);
}

                                                   
                                               
static void
test_picked_up_disarms(GuardFx *fx, gconstpointer unused)
{
    (void)unused;
    const gint timeout_s = pcv_config_get_drain_timeout();
    pcv_mainloop_guard_raise_for_test(SIGTERM);
    pcv_mainloop_guard_tick_for_test();
    pcv_mainloop_guard_signal_picked_up();
    s_now_us = FAKE_BASE_US + SEC(timeout_s + 1);
    pcv_mainloop_guard_tick_for_test();
    g_assert_cmpint(s_exit_calls, ==, 0);
    g_assert_cmpuint(_count_logs(fx->logs, "C|stop signal not handled"), ==, 0);
}

                                                              
                                                             
static void
test_second_signal_ignored(GuardFx *fx, gconstpointer unused)
{
    (void)unused;
    const gint timeout_s = pcv_config_get_drain_timeout();
    pcv_mainloop_guard_raise_for_test(SIGTERM);
    pcv_mainloop_guard_tick_for_test();

    s_now_us = FAKE_BASE_US + SEC(5);
    pcv_mainloop_guard_raise_for_test(SIGINT);
    pcv_mainloop_guard_tick_for_test();
    g_autofree gchar *again = g_strdup_printf("I|signal %d received again during shutdown; ignored", SIGINT);
    g_assert_true(_has_log(fx->logs, again));
    g_assert_cmpint(s_exit_calls, ==, 0);

    s_now_us = FAKE_BASE_US + SEC(timeout_s);                                
    pcv_mainloop_guard_tick_for_test();
    g_assert_cmpint(s_exit_calls, ==, 1);
    g_assert_cmpint(s_exit_code, ==, 1);

    while (g_main_context_iteration(fx->ctx, FALSE))
        ;
    g_assert_cmpint(s_on_signal_calls, ==, 1);
}

                                                                   

                                                               
                                                             
static void
test_current_rpc_in_message(GuardFx *fx, gconstpointer unused)
{
    (void)unused;
    s_now_us = FAKE_BASE_US + SEC(10);
    pcv_mainloop_guard_tick_for_test();
    g_assert_true(_has_log(fx->logs, "W|main loop stalled for 10 s (current RPC: none)"));

    pcv_mainloop_guard_rpc_enter("health.deep");
    s_now_us = FAKE_BASE_US + SEC(70);
    pcv_mainloop_guard_tick_for_test();
    g_assert_true(_has_log(fx->logs,
        "C|main loop unresponsive for 70 s (current RPC: health.deep, running 60 s)"));
    pcv_mainloop_guard_rpc_leave();
    g_assert_cmpuint(_count_logs(fx->logs, "W|"), ==, 1);
    g_assert_cmpuint(_count_logs(fx->logs, "C|"), ==, 1);
}

                                                                
                                             
static void
test_no_stall_report_after_pickup(GuardFx *fx, gconstpointer unused)
{
    (void)unused;
    pcv_mainloop_guard_raise_for_test(SIGTERM);
    pcv_mainloop_guard_tick_for_test();
    pcv_mainloop_guard_signal_picked_up();
    s_now_us = FAKE_BASE_US + SEC(70);
    pcv_mainloop_guard_tick_for_test();
    g_assert_cmpuint(_count_logs(fx->logs, "W|"), ==, 0);
    g_assert_cmpuint(_count_logs(fx->logs, "C|"), ==, 0);
    g_assert_cmpint(s_exit_calls, ==, 0);
}

                                                                
                                                                            
                                                                             
              
                                                           
static void
test_signal_dispatch_never_inline(GuardFx *fx, gconstpointer unused)
{
    (void)unused;
    pcv_mainloop_guard_raise_for_test(SIGTERM);
    g_main_context_push_thread_default(fx->ctx);
    pcv_mainloop_guard_tick_for_test();
    g_main_context_pop_thread_default(fx->ctx);
    g_assert_cmpint(s_on_signal_calls, ==, 0);

    GSource *src = g_main_context_find_source_by_funcs_user_data(fx->ctx, &g_idle_funcs, NULL);
    g_assert_nonnull(src);
    g_assert_cmpint(g_source_get_priority(src), ==, G_PRIORITY_HIGH);

    g_assert_true(g_main_context_iteration(fx->ctx, FALSE));
    g_assert_cmpint(s_on_signal_calls, ==, 1);
    g_assert_true(s_on_signal_thread == g_thread_self());
}

                                                                  
static gboolean
_safety_timeout(gpointer flag)
{
    *(gboolean *)flag = TRUE;
    return G_SOURCE_REMOVE;
}

                                                                  
                                                         
static void
test_heartbeat_recovery_logged(GuardFx *fx, gconstpointer unused)
{
    (void)unused;
    s_now_us = FAKE_BASE_US + SEC(15);
    pcv_mainloop_guard_tick_for_test();
    g_assert_true(_has_log(fx->logs, "W|main loop stalled for 15 s (current RPC: none)"));

    gboolean timed_out = FALSE;
    GSource *safety = g_timeout_source_new(5000);
    g_source_set_callback(safety, _safety_timeout, &timed_out, NULL);
    g_source_attach(safety, fx->ctx);
    g_main_context_iteration(fx->ctx, TRUE);                                      
    g_source_destroy(safety);
    g_source_unref(safety);
    g_assert_false(timed_out);

    s_now_us = FAKE_BASE_US + SEC(16);
    pcv_mainloop_guard_tick_for_test();
    g_assert_true(_has_log(fx->logs, "I|main loop recovered after 15 s stall"));
}

                                                        

                                               
static _Atomic gint64 s_real_offset_us;

                                      
                                         
static gint64
_offset_now(void)
{
    return g_get_monotonic_time() + atomic_load(&s_real_offset_us);
}

                                                              
                                                     
static void
_print_and_exit(gint code)
{
    fprintf(stdout, "exit_now(%d)\n", code);
    fflush(stdout);
    _exit(code);
}

                                                                                          
                                                                      
                                                      
                                                                   
static void
test_real_signal_path_exits_unsuccessfully(void)
{
    if (g_test_subprocess()) {
        static const PcvMainloopGuardOps ops = { _offset_now, _print_and_exit };
        pcv_mainloop_guard_set_ops_for_test(&ops);
        GMainContext *ctx = g_main_context_new();
        GMainLoop *loop = g_main_loop_new(ctx, FALSE);
        pcv_mainloop_guard_start(loop, _fake_on_signal);

        raise(SIGTERM);
        gint64 until = g_get_monotonic_time() + SEC(5);
        while (!g_main_context_find_source_by_funcs_user_data(ctx, &g_idle_funcs, NULL) &&
               g_get_monotonic_time() < until)
            g_usleep(10000);
        g_assert_nonnull(g_main_context_find_source_by_funcs_user_data(ctx, &g_idle_funcs, NULL));
        g_assert_cmpint(s_on_signal_calls, ==, 0);

        raise(SIGINT);
        atomic_store(&s_real_offset_us, SEC(3600));
        g_usleep(SEC(5));
        g_assert_not_reached();                                     
    }
    g_test_trap_subprocess(NULL, SEC(15), G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed();
    g_test_trap_assert_stdout("*exit_now(1)*");
    g_autofree gchar *again = g_strdup_printf("*signal %d received again during shutdown; ignored*", SIGINT);
    g_test_trap_assert_stderr(again);
    g_autofree gchar *deadline = g_strdup_printf(
        "*stop signal not handled by main loop within %d s (current RPC: none); exiting unsuccessfully*",
        pcv_config_get_drain_timeout());
    g_test_trap_assert_stderr(deadline);
}

                                                                                     
                                                                      
                                                                                
                                                                              
                  
                                                                      
static void
test_forked_child_signal_does_not_reach_parent(void)
{
    if (g_test_subprocess()) {
        static const PcvMainloopGuardOps ops = { _offset_now, _print_and_exit };
        pcv_mainloop_guard_set_ops_for_test(&ops);
        GMainContext *ctx = g_main_context_new();
        GMainLoop *loop = g_main_loop_new(ctx, FALSE);
        pcv_mainloop_guard_start(loop, _fake_on_signal);

        pid_t pid = fork();
        g_assert_cmpint(pid, >=, 0);
        if (pid == 0) {
            raise(SIGTERM);                                                          
            _exit(0);                                          
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0)
            g_assert_cmpint(errno, ==, EINTR);
        g_assert_true(WIFSIGNALED(status));
        g_assert_cmpint(WTERMSIG(status), ==, SIGTERM);

        g_usleep(G_USEC_PER_SEC / 2);                                          
        g_assert_null(g_main_context_find_source_by_funcs_user_data(ctx, &g_idle_funcs, NULL));
        atomic_store(&s_real_offset_us, SEC(3600));                                   
        g_usleep(G_USEC_PER_SEC * 3 / 2);
        g_assert_cmpint(s_on_signal_calls, ==, 0);
        return;
    }
    g_test_trap_subprocess(NULL, SEC(15), G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
    g_test_trap_assert_stdout_unmatched("*exit_now*");
    g_test_trap_assert_stderr_unmatched("*stop signal*");
}

                                                                                 
                                   
                                                                        
                                               
                                                                                                            
guint pcv_test_prom_inc_count(const gchar *key);
static void
test_stall_counters(GuardFx *fx, gconstpointer unused)
{
    (void)fx; (void)unused;
    const gchar *warn = "purecvisor_mainloop_stalls_total{level=warn}";
    const gchar *crit = "purecvisor_mainloop_stalls_total{level=crit}";
    guint w0 = pcv_test_prom_inc_count(warn), c0 = pcv_test_prom_inc_count(crit);

    s_now_us += SEC(10);
    pcv_mainloop_guard_tick_for_test();
    g_assert_cmpuint(pcv_test_prom_inc_count(warn), ==, w0 + 1);
    g_assert_cmpuint(pcv_test_prom_inc_count(crit), ==, c0);

    s_now_us += SEC(5);
    pcv_mainloop_guard_tick_for_test();
    g_assert_cmpuint(pcv_test_prom_inc_count(warn), ==, w0 + 1);

    s_now_us += SEC(45);
    pcv_mainloop_guard_tick_for_test();
    g_assert_cmpuint(pcv_test_prom_inc_count(warn), ==, w0 + 1);
    g_assert_cmpuint(pcv_test_prom_inc_count(crit), ==, c0 + 1);
}

void
test_mainloop_guard_register(void)
{
    g_test_add_func("/mainloop_guard/stall_warn_at_10s", test_stall_warn_at_10s);
    g_test_add_func("/mainloop_guard/stall_crit_at_60s", test_stall_crit_at_60s);
    g_test_add_func("/mainloop_guard/recovered_once", test_recovered_once);
    g_test_add("/mainloop_guard/deadline_fires_when_not_picked_up", GuardFx, NULL,
               fx_setup, test_deadline_fires_when_not_picked_up, fx_teardown);
    g_test_add("/mainloop_guard/picked_up_disarms", GuardFx, NULL,
               fx_setup, test_picked_up_disarms, fx_teardown);
    g_test_add("/mainloop_guard/second_signal_ignored", GuardFx, NULL,
               fx_setup, test_second_signal_ignored, fx_teardown);
    g_test_add("/mainloop_guard/current_rpc_in_message", GuardFx, NULL,
               fx_setup, test_current_rpc_in_message, fx_teardown);
    g_test_add("/mainloop_guard/no_stall_report_after_pickup", GuardFx, NULL,
               fx_setup, test_no_stall_report_after_pickup, fx_teardown);
    g_test_add("/mainloop_guard/signal_dispatch_never_inline", GuardFx, NULL,
               fx_setup, test_signal_dispatch_never_inline, fx_teardown);
    g_test_add("/mainloop_guard/heartbeat_recovery_logged", GuardFx, NULL,
               fx_setup, test_heartbeat_recovery_logged, fx_teardown);
    g_test_add("/mainloop_guard/stall_counters", GuardFx, NULL,
               fx_setup, test_stall_counters, fx_teardown);
    g_test_add_func("/mainloop_guard/real_signal_path_exits_unsuccessfully",
                    test_real_signal_path_exits_unsuccessfully);
    g_test_add_func("/mainloop_guard/forked_child_signal_does_not_reach_parent",
                    test_forked_child_signal_does_not_reach_parent);
}
