                                                                                            
                                                                                      
                               
                                                                        
                                
   
                     
                                                        
  
                                                                               
                 
                                                                               
                                                     
                           
  
                                              
                                         
  
          
                                          
                                                           
                                         
                                                                         
                                                            
                                                         
  
                                                       
                                          
                                                                               
   
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>
#include "../src/api/drain.h"
#include "../src/modules/dispatcher/rpc_completion.h"

static void test_init_inflight_zero(void) {
    pcv_drain_init();
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 0);
    g_assert_false(pcv_drain_is_shutdown());
}

static void test_inc_dec_basic(void) {
    pcv_drain_init();
    g_assert_true(pcv_drain_inc());
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 1);
    g_assert_true(pcv_drain_inc());
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 2);
    pcv_drain_dec();
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 1);
    pcv_drain_dec();
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 0);
}

static void test_inc_many(void) {
    pcv_drain_init();
    for (int i = 0; i < 100; i++)
        g_assert_true(pcv_drain_inc());
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 100);
    for (int i = 0; i < 100; i++)
        pcv_drain_dec();
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 0);
}

static void test_notify_ready_no_socket(void) {
    pcv_drain_init();
                                            
    g_unsetenv("NOTIFY_SOCKET");
    pcv_drain_notify_ready();
    pcv_drain_notify_stopping();
    pcv_drain_notify_watchdog();
}

static void test_watchdog_usec(void) {
    pcv_drain_init();
                               
    g_unsetenv("WATCHDOG_USEC");
    g_assert_cmpuint(pcv_drain_get_watchdog_usec(), ==, 0);
}

static void test_watchdog_usec_from_env(void) {
    pcv_drain_init();
    g_setenv("WATCHDOG_USEC", "30000000", TRUE);          
    guint64 v = pcv_drain_get_watchdog_usec();
                                                       
    g_assert_cmpuint(v, >, 0);
    g_unsetenv("WATCHDOG_USEC");
}

static gpointer drain_inc_dec_worker(gpointer u) {
    (void)u;
    for (int j = 0; j < 100; j++) {
        pcv_drain_inc();
        g_usleep(100);
        pcv_drain_dec();
    }
    return NULL;
}

static void test_concurrent_inc_dec(void) {
    pcv_drain_init();
    GThread *t[10];
    for (int i = 0; i < 10; i++)
        t[i] = g_thread_new("drain", drain_inc_dec_worker, NULL);
    for (int i = 0; i < 10; i++)
        g_thread_join(t[i]);
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 0);
}

                                                       

static void test_drain_cancel(void) {
    pcv_drain_init();
                                                     
    g_assert_false(pcv_drain_is_shutdown());
    g_assert_true(pcv_drain_inc());
    pcv_drain_dec();

                                               
    pcv_drain_cancel();
    g_assert_false(pcv_drain_is_shutdown());

                                  
    g_assert_true(pcv_drain_inc());
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 1);
    pcv_drain_dec();
    g_assert_cmpint(pcv_drain_get_inflight(), ==, 0);
    g_assert_false(pcv_drain_is_shutdown());
}

                                                       

static void test_drain_begin_shutdown(void) {
    pcv_drain_init();

                                                               

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    pcv_drain_begin(loop, 1              );

                                
    g_assert_true(pcv_drain_is_shutdown());

                                     
    g_assert_false(pcv_drain_inc());

    g_main_loop_run(loop);
    g_assert_cmpint(pcv_drain_get_work(), ==, 0);
    pcv_drain_shutdown();
    g_main_loop_unref(loop);
    pcv_drain_init();
}

                                                        

static void test_drain_notify_ready_with_socket(void) {
                                                            
    gchar *tmpdir = g_dir_make_tmp("drain-notify-XXXXXX", NULL);
    gchar *sock_path = g_build_filename(tmpdir, "notify.sock", NULL);

                         
    int recv_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    g_assert_cmpint(recv_fd, >=, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    int rc = bind(recv_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
                                       
        close(recv_fd);
        g_unlink(sock_path);
        g_rmdir(tmpdir);
        g_free(sock_path);
        g_free(tmpdir);
        g_test_skip("bind() failed — skipping socket notify test");
        return;
    }

    pcv_drain_init();
    g_setenv("NOTIFY_SOCKET", sock_path, TRUE);

                    
    pcv_drain_notify_ready();

                                        
    char buf[64] = { 0 };
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(recv_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(recv_fd, buf, sizeof(buf) - 1, 0);

    if (n > 0) {
        g_assert_true(strstr(buf, "READY=1") != NULL);
    }
                                          

                            
    pcv_drain_notify_stopping();
    memset(buf, 0, sizeof(buf));
    n = recv(recv_fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        g_assert_true(strstr(buf, "STOPPING=1") != NULL);
    }

            
    g_unsetenv("NOTIFY_SOCKET");
    close(recv_fd);
    g_unlink(sock_path);
    g_rmdir(tmpdir);
    g_free(sock_path);
    g_free(tmpdir);
}

static void
_spin_default_context_ms(guint ms)
{
    gint64 until = g_get_monotonic_time() + (gint64)ms * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < until) {
        while (g_main_context_iteration(NULL, FALSE))
            ;
        g_usleep(10 * G_TIME_SPAN_MILLISECOND);
    }
}

static guint g_manual_outcome_logs;

static void
_count_manual_outcome(const gchar *domain, GLogLevelFlags level,
                      const gchar *message, gpointer unused)
{
    (void)domain; (void)level; (void)unused;
    if (message && (strstr(message, "Manual drain complete") ||
                    strstr(message, "Manual drain timeout")))
        g_manual_outcome_logs++;
}

static void test_manual_drain_honors_timeout(void) {
    pcv_drain_init();
    g_assert_true(pcv_drain_inc());
    g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*Manual drain*(timeout: 1s)*");
    pcv_drain_begin(NULL, 1);
    g_test_assert_expected_messages();
    g_assert_true(pcv_drain_is_shutdown());
    g_assert_false(pcv_drain_is_terminating());

    g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*Manual drain timeout after 1s*requests=1*");
    _spin_default_context_ms(1500);
    g_test_assert_expected_messages();
    g_assert_true(pcv_drain_is_shutdown());

    pcv_drain_dec();
    pcv_drain_cancel();
    g_assert_false(pcv_drain_is_shutdown());
    pcv_drain_shutdown();
    pcv_drain_init();
}

static void test_manual_drain_reports_completion(void) {
    pcv_drain_init();
    g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*Manual drain*(timeout: 57s)*");
    pcv_drain_begin(NULL, 57);
    g_test_assert_expected_messages();
    g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*Manual drain complete*");
    _spin_default_context_ms(400);
    g_test_assert_expected_messages();
    pcv_drain_cancel();
    pcv_drain_shutdown();
    pcv_drain_init();
}

static void test_manual_drain_resume_stops_watch(void) {
    pcv_drain_init();
    g_assert_true(pcv_drain_inc());
    g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*Manual drain*(timeout: 1s)*");
    pcv_drain_begin(NULL, 1);
    g_test_assert_expected_messages();
    pcv_drain_cancel();

    g_manual_outcome_logs = 0;
    guint h_msg = g_log_set_handler(NULL, G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_WARNING,
                                    _count_manual_outcome, NULL);
    _spin_default_context_ms(1500);
    g_log_remove_handler(NULL, h_msg);
    g_assert_cmpuint(g_manual_outcome_logs, ==, 0);
    g_assert_true(pcv_drain_inc());
    pcv_drain_dec();
    pcv_drain_dec();
    pcv_drain_shutdown();
    pcv_drain_init();
}

static void test_manual_drain_promotes_to_termination(void) {
    pcv_drain_init();
    g_assert_true(pcv_drain_inc());
    g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*Manual drain*(timeout: 30s)*");
    pcv_drain_begin(NULL, 30);
    g_test_assert_expected_messages();

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    pcv_drain_begin(loop, 5);
    g_assert_true(pcv_drain_is_terminating());
    pcv_drain_dec();
    g_manual_outcome_logs = 0;
    guint h_msg = g_log_set_handler(NULL, G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_WARNING,
                                    _count_manual_outcome, NULL);
    g_main_loop_run(loop);
    g_log_remove_handler(NULL, h_msg);
    g_assert_cmpuint(g_manual_outcome_logs, ==, 0);
    pcv_drain_shutdown();
    g_main_loop_unref(loop);
    pcv_drain_init();
}

static void test_task_actor_captured_in_scope(void)
{
    pcv_drain_init();
    pcv_rpc_completion_begin("vm.start");
    pcv_rpc_completion_set_actor("alice");
    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    (void)pcv_rpc_completion_finish();
    g_assert_cmpstr(pcv_drain_task_actor(task), ==, "alice");
    g_assert_cmpstr(pcv_drain_task_actor(G_ASYNC_RESULT(task)), ==, "alice");
    g_object_unref(task);
    GTask *outside = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_assert_null(pcv_drain_task_actor(outside));
    g_object_unref(outside);
}

static void test_task_actor_override_clear_and_non_task(void)
{
    pcv_drain_init();
    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    pcv_drain_task_set_actor(task, "system");
    g_assert_cmpstr(pcv_drain_task_actor(task), ==, "system");
    pcv_drain_task_set_actor(task, "bob");
    g_assert_cmpstr(pcv_drain_task_actor(task), ==, "bob");
    pcv_drain_task_set_actor(task, NULL);
    g_assert_null(pcv_drain_task_actor(task));
    g_object_unref(task);
    GObject *plain = g_object_new(G_TYPE_OBJECT, NULL);
    g_assert_null(pcv_drain_task_actor_of(plain));
    g_assert_null(pcv_drain_task_actor_of(NULL));
    g_object_unref(plain);
}

typedef struct {
    gint     ran;
    gint     destroyed;
    GThread *ran_on;
} TryInvokeProbe;

static gboolean
_try_invoke_cb(gpointer data)
{
    TryInvokeProbe *p = data;
    p->ran_on = g_thread_self();
    g_atomic_int_inc(&p->ran);
    return G_SOURCE_REMOVE;
}

static void
_try_invoke_destroy(gpointer data)
{
    TryInvokeProbe *p = data;
    g_atomic_int_inc(&p->destroyed);
}

static gpointer
_try_invoke_from_thread(gpointer data)
{
    return GINT_TO_POINTER(pcv_drain_try_invoke(NULL, G_PRIORITY_DEFAULT, _try_invoke_cb,
                                                data, _try_invoke_destroy));
}

static void test_try_invoke_refused_after_seal(void) {
    pcv_drain_init();
    g_assert_cmpint(pcv_drain_get_work(), ==, 0);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    pcv_drain_begin(loop, 5);
    g_main_loop_run(loop);

    TryInvokeProbe p = {0};
    g_assert_false(pcv_drain_try_invoke(NULL, G_PRIORITY_DEFAULT, _try_invoke_cb, &p,
                                        _try_invoke_destroy));
    for (gint i = 0; i < 10 && g_main_context_iteration(NULL, FALSE); i++)
        ;
    g_assert_cmpint(g_atomic_int_get(&p.ran), ==, 0);
    g_assert_cmpint(g_atomic_int_get(&p.destroyed), ==, 0);
    g_assert_cmpint(pcv_drain_get_work(), ==, 0);
    pcv_drain_shutdown();
    g_main_loop_unref(loop);
    pcv_drain_init();
}

static void test_try_invoke_never_runs_inline(void) {
    pcv_drain_init();
    gint base = pcv_drain_get_work();

    TryInvokeProbe other = {0};
    GThread *t = g_thread_new("try-invoke", _try_invoke_from_thread, &other);
    g_assert_true(GPOINTER_TO_INT(g_thread_join(t)));
    g_assert_cmpint(g_atomic_int_get(&other.ran), ==, 0);
    g_assert_cmpint(pcv_drain_get_work(), ==, base + 1);

    g_assert_true(g_main_context_acquire(NULL));
    TryInvokeProbe own = {0};
    g_assert_true(pcv_drain_try_invoke(NULL, G_PRIORITY_DEFAULT, _try_invoke_cb, &own,
                                       _try_invoke_destroy));
    g_assert_cmpint(g_atomic_int_get(&own.ran), ==, 0);
    g_assert_cmpint(pcv_drain_get_work(), ==, base + 2);

    gint64 until = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while ((!g_atomic_int_get(&other.ran) || !g_atomic_int_get(&own.ran)) &&
           g_get_monotonic_time() < until)
        g_main_context_iteration(NULL, FALSE);
    g_main_context_release(NULL);
    g_assert_cmpint(g_atomic_int_get(&other.ran), ==, 1);
    g_assert_cmpint(g_atomic_int_get(&own.ran), ==, 1);
    g_assert_true(other.ran_on == g_thread_self());
    g_assert_true(own.ran_on == g_thread_self());
    g_assert_cmpint(g_atomic_int_get(&other.destroyed), ==, 1);
    g_assert_cmpint(g_atomic_int_get(&own.destroyed), ==, 1);
    g_assert_cmpint(pcv_drain_get_work(), ==, base);
}

void test_drain_register(void) {
    g_test_add_func("/drain/try_invoke_refused_after_seal", test_try_invoke_refused_after_seal);
    g_test_add_func("/drain/try_invoke_never_runs_inline", test_try_invoke_never_runs_inline);
    g_test_add_func("/drain/manual_drain_honors_timeout", test_manual_drain_honors_timeout);
    g_test_add_func("/drain/manual_drain_reports_completion", test_manual_drain_reports_completion);
    g_test_add_func("/drain/manual_drain_resume_stops_watch", test_manual_drain_resume_stops_watch);
    g_test_add_func("/drain/manual_drain_promotes_to_termination", test_manual_drain_promotes_to_termination);
    g_test_add_func("/drain/init_inflight_zero", test_init_inflight_zero);
    g_test_add_func("/drain/inc_dec_basic", test_inc_dec_basic);
    g_test_add_func("/drain/inc_many", test_inc_many);
    g_test_add_func("/drain/notify_ready_no_socket", test_notify_ready_no_socket);
    g_test_add_func("/drain/watchdog_usec", test_watchdog_usec);
    g_test_add_func("/drain/watchdog_usec_from_env", test_watchdog_usec_from_env);
    g_test_add_func("/drain/concurrent_inc_dec", test_concurrent_inc_dec);
    g_test_add_func("/drain/drain_cancel", test_drain_cancel);
    g_test_add_func("/drain/drain_begin_shutdown", test_drain_begin_shutdown);
    g_test_add_func("/drain/task_actor_captured_in_scope", test_task_actor_captured_in_scope);
    g_test_add_func("/drain/task_actor_override_clear_and_non_task", test_task_actor_override_clear_and_non_task);
    g_test_add_func("/drain/notify_ready_with_socket", test_drain_notify_ready_with_socket);
}
