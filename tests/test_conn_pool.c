                                                                                                    
                                                                               
                                                       
                                                                
                             
   
                         
                                                                 
  
                                                                               
                 
                                                                               
                                                              
               
  
                                                         
                                                          
  
          
                                              
                                              
                                  
                                               
                                                        
                                   
                                                  
                           
                                                                               
   
#include <glib.h>
#include <string.h>
#include <libvirt/libvirt.h>
#include "../src/modules/virt/virt_conn_pool.h"
#include "../src/modules/virt/pcv_virt_open.h"

static void test_init_shutdown(void) {
    virt_conn_pool_init(4);
    virt_conn_pool_shutdown();
}

static void test_stats_after_init(void) {
    virt_conn_pool_init(8);
    guint idle = 99, total = 99, max = 0;
    virt_conn_pool_stats(&idle, &total, &max);
    g_assert_cmpuint(max, ==, 8);
                                                           
    g_assert_cmpuint(total, <=, max);
    g_assert_cmpuint(idle, <=, total);
    virt_conn_pool_shutdown();
}

static void test_stats_null_safe(void) {
    virt_conn_pool_init(2);
    virt_conn_pool_stats(NULL, NULL, NULL);                   
    virt_conn_pool_shutdown();
}

static void test_wait_avg_zero_initial(void) {
    virt_conn_pool_init(4);
                        
    g_assert_cmpfloat(virt_conn_pool_wait_avg_seconds(), ==, 0.0);
    virt_conn_pool_shutdown();
}

static void test_init_min_size(void) {
    virt_conn_pool_init(1);
    guint max = 0;
    virt_conn_pool_stats(NULL, NULL, &max);
    g_assert_cmpuint(max, >=, 1);
    virt_conn_pool_shutdown();
}

static void test_shutdown_idempotent(void) {
    virt_conn_pool_shutdown();               
    virt_conn_pool_init(2);
    virt_conn_pool_shutdown();
    virt_conn_pool_shutdown();           
}

                                                             

static void test_acquire_release_test_driver(void) {
    g_setenv("PCV_LIBVIRT_URI", "test:///default", TRUE);
    virt_conn_pool_init(4);

    virConnectPtr c1 = virt_conn_pool_acquire();
    if (c1) {
        guint idle, total, max;
        virt_conn_pool_stats(&idle, &total, &max);
        g_assert_cmpuint(total, >=, 1);
        virt_conn_pool_release(c1);

                           
        virConnectPtr c2 = virt_conn_pool_acquire();
        g_assert_nonnull(c2);
        virt_conn_pool_release(c2);
    }

    virt_conn_pool_shutdown();
    g_unsetenv("PCV_LIBVIRT_URI");
}

static void test_acquire_multiple_then_release(void) {
    g_setenv("PCV_LIBVIRT_URI", "test:///default", TRUE);
    virt_conn_pool_init(4);

    virConnectPtr conns[3] = {NULL, NULL, NULL};
    for (int i = 0; i < 3; i++) conns[i] = virt_conn_pool_acquire();
                    
    int got = 0;
    for (int i = 0; i < 3; i++) if (conns[i]) got++;
    if (got > 0) {
        guint total = 0;
        virt_conn_pool_stats(NULL, &total, NULL);
        g_assert_cmpuint(total, >=, (guint)got);
    }
    for (int i = 0; i < 3; i++) if (conns[i]) virt_conn_pool_release(conns[i]);

    virt_conn_pool_shutdown();
    g_unsetenv("PCV_LIBVIRT_URI");
}

static void test_release_null_safe(void) {
    virt_conn_pool_init(2);
    virt_conn_pool_release(NULL);               
    virt_conn_pool_shutdown();
}

static void test_try_acquire_no_wait_when_exhausted(void) {
    g_setenv("PCV_LIBVIRT_URI", "test:///default", TRUE);
    virt_conn_pool_init(1);

    virConnectPtr held = virt_conn_pool_acquire();
    if (!held) {
        virt_conn_pool_shutdown();
        g_unsetenv("PCV_LIBVIRT_URI");
        g_test_skip("test:///default 연결 불가");
        return;
    }

    gint64 t0 = g_get_monotonic_time();
    virConnectPtr none = virt_conn_pool_try_acquire();
    gint64 waited_us = g_get_monotonic_time() - t0;
    g_assert_null(none);
    g_assert_cmpint(waited_us, <, 2 * G_USEC_PER_SEC);

    virt_conn_pool_release(held);
    virConnectPtr again = virt_conn_pool_try_acquire();
    g_assert_true(again == held);
    virt_conn_pool_release(again);

    virt_conn_pool_shutdown();
    g_unsetenv("PCV_LIBVIRT_URI");
}

static void test_try_acquire_opens_under_limit(void) {
    g_setenv("PCV_LIBVIRT_URI", "test:///default", TRUE);
    virt_conn_pool_init(2);

    virConnectPtr first = virt_conn_pool_acquire();
    if (!first) {
        virt_conn_pool_shutdown();
        g_unsetenv("PCV_LIBVIRT_URI");
        g_test_skip("test:///default 연결 불가");
        return;
    }
    virConnectPtr second = virt_conn_pool_try_acquire();
    g_assert_nonnull(second);
    g_assert_true(second != first);
    guint total = 0;
    virt_conn_pool_stats(NULL, &total, NULL);
    g_assert_cmpuint(total, ==, 2);

    virt_conn_pool_release(second);
    virt_conn_pool_release(first);
    virt_conn_pool_shutdown();
    g_unsetenv("PCV_LIBVIRT_URI");
}

static char g_fake_slots[16];
typedef struct {
    int      next_slot;
    int      open_calls, close_calls, alive_calls, version_calls;
    int      open_fail;
    int      alive_ret;
    int      version_ret;
    guint    last_open_timeout;
    int      not_free_calls;
    int      alive_locked_calls;
} FakeState;
static FakeState g_fake;

static virConnectPtr fake_open(const gchar *uri, guint timeout_sec, GError **error) {
    (void)uri;
    (void)error;
    g_fake.open_calls++;
    g_fake.last_open_timeout = timeout_sec;
    if (!virt_conn_pool_lock_is_free_for_test()) g_fake.not_free_calls++;
    if (g_fake.open_fail) return NULL;
    return (virConnectPtr)&g_fake_slots[g_fake.next_slot++ % 16];
}
static int fake_close(virConnectPtr c) {
    (void)c;
    g_fake.close_calls++;
    if (!virt_conn_pool_lock_is_free_for_test()) g_fake.not_free_calls++;
    return 0;
}
static int fake_is_alive(virConnectPtr c) {
    (void)c;
    g_fake.alive_calls++;
    if (!virt_conn_pool_lock_is_free_for_test()) g_fake.alive_locked_calls++;
    return g_fake.alive_ret;
}
static int fake_get_version(virConnectPtr c, unsigned long *v) {
    (void)c;
    g_fake.version_calls++;
    if (!virt_conn_pool_lock_is_free_for_test()) g_fake.not_free_calls++;
    *v = 1;
    return g_fake.version_ret;
}
static const PcvConnPoolOps G_fake_ops = {
    .open = fake_open, .close = fake_close, .is_alive = fake_is_alive,
    .get_version = fake_get_version,
};

static void fake_begin(guint max_size) {
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.alive_ret = 1;
    g_fake.version_ret = 0;
    virt_conn_pool_set_ops_for_test(&G_fake_ops);
    virt_conn_pool_init(max_size);
}
static void fake_end(void) {
    virt_conn_pool_shutdown();
    virt_conn_pool_set_ops_for_test(NULL);
    virt_conn_pool_set_terminating_fn_for_test(NULL);
}

static void test_acquire_health_check_outside_lock(void) {
    fake_begin(2);
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    g_assert_cmpint(g_fake.version_calls, ==, 1);
    g_assert_cmpint(g_fake.alive_calls, >=, 1);
    g_assert_cmpint(g_fake.not_free_calls, ==, 0);
    g_assert_cmpint(g_fake.alive_locked_calls, ==, 0);
    virt_conn_pool_release(c);
    fake_end();
}

static void test_reconnect_outside_lock(void) {
    fake_begin(2);
    int opens_before = g_fake.open_calls;
    g_fake.alive_ret = 0;
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    g_assert_cmpint(g_fake.close_calls, ==, 1);
    g_assert_cmpint(g_fake.open_calls, ==, opens_before + 1);
    g_assert_cmpint(g_fake.not_free_calls, ==, 0);
    g_assert_cmpint(g_fake.alive_locked_calls, ==, 0);
    guint total = 0;
    virt_conn_pool_stats(NULL, &total, NULL);
    g_assert_cmpuint(total, ==, 1);
    virt_conn_pool_release(c);
    fake_end();
}

static void test_reconnect_failure_keeps_total_consistent(void) {
    fake_begin(2);
    g_fake.alive_ret = 0;
    g_fake.open_fail = 1;
    gint64 t0 = g_get_monotonic_time();
    virConnectPtr c = virt_conn_pool_acquire();
    gint64 waited_us = g_get_monotonic_time() - t0;
    g_assert_null(c);
    g_assert_cmpint(waited_us, <, 2 * G_USEC_PER_SEC);
    guint idle = 99, total = 99;
    virt_conn_pool_stats(&idle, &total, NULL);
    g_assert_cmpuint(idle, ==, 0);
    g_assert_cmpuint(total, ==, 0);
    g_assert_cmpint(g_fake.not_free_calls, ==, 0);

    g_fake.open_fail = 0;
    g_fake.alive_ret = 1;
    c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    virt_conn_pool_stats(NULL, &total, NULL);
    g_assert_cmpuint(total, ==, 1);
    virt_conn_pool_release(c);
    fake_end();
}

static void test_reaper_does_not_reconnect(void) {
    fake_begin(2);
    int opens_before = g_fake.open_calls;
    g_fake.alive_ret = 0;
    virt_conn_pool_reap_for_test();
    g_assert_cmpint(g_fake.close_calls, ==, 1);
    g_assert_cmpint(g_fake.open_calls, ==, opens_before);
    g_assert_cmpint(g_fake.not_free_calls, ==, 0);
    guint idle = 99, total = 99;
    virt_conn_pool_stats(&idle, &total, NULL);
    g_assert_cmpuint(idle, ==, 0);
    g_assert_cmpuint(total, ==, 0);

    g_fake.alive_ret = 1;
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    g_assert_cmpint(g_fake.open_calls, ==, opens_before + 1);
    virt_conn_pool_release(c);
    fake_end();
}

static gboolean terminating_true(void) { return TRUE; }

static void test_normal_open_10s(void) {
    fake_begin(2);
    g_assert_cmpuint(g_fake.last_open_timeout, ==, PCV_VIRT_OPEN_TIMEOUT_SEC);
    g_assert_cmpuint(PCV_VIRT_OPEN_TIMEOUT_SEC, ==, 10);
    g_fake.alive_ret = 0;
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    g_assert_cmpuint(g_fake.last_open_timeout, ==, 10);
    virt_conn_pool_release(c);
    fake_end();
}

static void test_terminating_open_succeeds(void) {
    fake_begin(2);
    virt_conn_pool_set_terminating_fn_for_test(terminating_true);
    g_fake.alive_ret = 0;
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    g_assert_cmpuint(g_fake.last_open_timeout, ==, 3);
    g_fake.last_open_timeout = 0;
    virConnectPtr d = virt_conn_pool_acquire();
    g_assert_nonnull(d);
    g_assert_cmpuint(g_fake.last_open_timeout, ==, 3);
    virt_conn_pool_release(c);
    virt_conn_pool_release(d);
    fake_end();
}

static GMutex g_hang_mu;
static GCond  g_hang_cond;
static gboolean g_hang_release;
static gint g_hang_opens, g_hang_closes;
static gboolean g_hang_stuck;
static virConnectPtr hang_open(const gchar *uri, gboolean ro) {
    (void)uri; (void)ro;
    if (!g_hang_stuck) return NULL;
    g_atomic_int_inc(&g_hang_opens);
    g_mutex_lock(&g_hang_mu);
    while (!g_hang_release)
        g_cond_wait(&g_hang_cond, &g_hang_mu);
    g_mutex_unlock(&g_hang_mu);
    return (virConnectPtr)&g_fake_slots[0];
}
static int hang_keepalive(virConnectPtr c, int i, unsigned int n) { (void)c; (void)i; (void)n; return 0; }
static void hang_close(virConnectPtr c) { (void)c; g_atomic_int_inc(&g_hang_closes); }

static void test_terminating_open_short_bound(void) {
    g_assert_cmpint(virInitialize(), ==, 0);
    g_hang_release = FALSE;
    g_hang_stuck = FALSE;
    g_atomic_int_set(&g_hang_opens, 0);
    g_atomic_int_set(&g_hang_closes, 0);
    pcv_virt_open_set_fn_for_test(hang_open, hang_keepalive, hang_close);
    virt_conn_pool_set_ops_for_test(NULL);
    virt_conn_pool_init(2);
    virt_conn_pool_set_terminating_fn_for_test(terminating_true);
    g_hang_stuck = TRUE;

    gint64 t0 = g_get_monotonic_time();
    virConnectPtr c = virt_conn_pool_acquire();
    gint64 took = g_get_monotonic_time() - t0;
    g_assert_null(c);
    g_assert_cmpint(took, >=, 2 * G_USEC_PER_SEC + G_USEC_PER_SEC / 2);
    g_assert_cmpint(took, <, 3 * G_USEC_PER_SEC + G_USEC_PER_SEC / 2);
    guint total = 99;
    virt_conn_pool_stats(NULL, &total, NULL);
    g_assert_cmpuint(total, ==, 0);

    g_mutex_lock(&g_hang_mu);
    g_hang_release = TRUE;
    g_cond_broadcast(&g_hang_cond);
    g_mutex_unlock(&g_hang_mu);
    for (int i = 0; i < 500 && pcv_virt_open_abandoned_for_test() > 0; i++)
        g_usleep(10 * 1000);
    g_assert_cmpuint(pcv_virt_open_abandoned_for_test(), ==, 0);
    g_assert_cmpint(g_atomic_int_get(&g_hang_closes), ==, 1);

    virt_conn_pool_set_terminating_fn_for_test(NULL);
    virt_conn_pool_shutdown();
    pcv_virt_open_set_fn_for_test(NULL, NULL, NULL);
}

static void test_terminating_exhaust_wait_short(void) {
    fake_begin(1);
    virConnectPtr held = virt_conn_pool_acquire();
    g_assert_nonnull(held);
    virt_conn_pool_set_terminating_fn_for_test(terminating_true);
    gint64 t0 = g_get_monotonic_time();
    virConnectPtr c = virt_conn_pool_acquire();
    gint64 took = g_get_monotonic_time() - t0;
    g_assert_null(c);
    g_assert_cmpint(took, >=, 2 * G_USEC_PER_SEC + G_USEC_PER_SEC / 2);
    g_assert_cmpint(took, <, 3 * G_USEC_PER_SEC + G_USEC_PER_SEC / 2);
    virt_conn_pool_release(held);
    fake_end();
}

static gpointer release_later(gpointer data) {
    g_usleep(300 * 1000);
    virt_conn_pool_release(data);
    return NULL;
}

static void test_terminating_exhaust_wakes_on_release(void) {
    fake_begin(1);
    virConnectPtr held = virt_conn_pool_acquire();
    g_assert_nonnull(held);
    virt_conn_pool_set_terminating_fn_for_test(terminating_true);
    GThread *th = g_thread_new("release-later", release_later, held);
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_true(c == held);
    g_thread_join(th);
    virt_conn_pool_release(c);
    fake_end();
}

static gboolean g_stall_release;
static gint g_stall_entered, g_stall_done;
static int stall_close(virConnectPtr c) {
    (void)c;
    g_atomic_int_inc(&g_stall_entered);
    g_mutex_lock(&g_hang_mu);
    while (!g_stall_release)
        g_cond_wait(&g_hang_cond, &g_hang_mu);
    g_mutex_unlock(&g_hang_mu);
    g_atomic_int_inc(&g_stall_done);
    return 0;
}
static const PcvConnPoolOps G_stall_ops = {
    .open = fake_open, .close = stall_close, .is_alive = fake_is_alive,
    .get_version = fake_get_version,
};

static void test_shutdown_close_bounded(void) {
    g_stall_release = FALSE;
    g_atomic_int_set(&g_stall_entered, 0);
    g_atomic_int_set(&g_stall_done, 0);
    fake_begin(2);
    virConnectPtr a = virt_conn_pool_acquire();
    virConnectPtr b = virt_conn_pool_acquire();
    virt_conn_pool_release(a);
    virt_conn_pool_release(b);
    virt_conn_pool_set_ops_for_test(&G_stall_ops);

    gint64 t0 = g_get_monotonic_time();
    virt_conn_pool_shutdown();
    gint64 took = g_get_monotonic_time() - t0;
    g_assert_cmpint(took, <, 3 * G_USEC_PER_SEC + G_USEC_PER_SEC / 2);
    g_assert_cmpint(took, >=, 2 * G_USEC_PER_SEC + G_USEC_PER_SEC / 2);
    g_assert_cmpint(g_atomic_int_get(&g_stall_entered), ==, 1);

    virt_conn_pool_init(3);
    guint idle = 99, total = 99, max = 0;
    virt_conn_pool_stats(&idle, &total, &max);
    g_assert_cmpuint(idle, ==, 1);
    g_assert_cmpuint(total, ==, 1);

    g_mutex_lock(&g_hang_mu);
    g_stall_release = TRUE;
    g_cond_broadcast(&g_hang_cond);
    g_mutex_unlock(&g_hang_mu);
    for (int i = 0; i < 500 && g_atomic_int_get(&g_stall_done) < 2; i++)
        g_usleep(10 * 1000);
    g_assert_cmpint(g_atomic_int_get(&g_stall_done), ==, 2);

    virt_conn_pool_stats(&idle, &total, &max);
    g_assert_cmpuint(idle, ==, 1);
    g_assert_cmpuint(total, ==, 1);
    g_assert_cmpuint(max, ==, 3);
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    virt_conn_pool_release(c);
    fake_end();
}

static void test_shutdown_close_fast_normal(void) {
    fake_begin(2);
    virConnectPtr a = virt_conn_pool_acquire();
    virConnectPtr b = virt_conn_pool_acquire();
    virt_conn_pool_release(a);
    virt_conn_pool_release(b);
    gint64 t0 = g_get_monotonic_time();
    virt_conn_pool_shutdown();
    g_assert_cmpint(g_get_monotonic_time() - t0, <, G_USEC_PER_SEC);
    g_assert_cmpint(g_fake.close_calls, ==, 2);
    virt_conn_pool_set_ops_for_test(NULL);
    virt_conn_pool_set_terminating_fn_for_test(NULL);
}

static gint64 g_door_clock = 1000 * G_USEC_PER_SEC;
static gint64 door_fake_now(void) { return g_door_clock; }
#define DOOR_SEC(n) ((gint64)(n) * G_USEC_PER_SEC)

static void door_begin(void) {
    fake_begin(2);
    g_door_clock = DOOR_SEC(1000);
    virt_conn_pool_set_door_now_for_test(door_fake_now);
    g_assert_true(g_main_context_acquire(NULL));
    g_fake.open_calls = g_fake.alive_calls = g_fake.version_calls = 0;
}
static void door_end(void) {
    g_main_context_release(NULL);
    virt_conn_pool_set_door_now_for_test(NULL);
    fake_end();
}
static void door_assert_no_calls(void) {
    g_assert_cmpint(g_fake.open_calls, ==, 0);
    g_assert_cmpint(g_fake.alive_calls, ==, 0);
    g_assert_cmpint(g_fake.version_calls, ==, 0);
}

static void test_door_blocks_main_when_stale(void) {
    door_begin();
    virt_conn_pool_note_libvirtd_ok();
    g_door_clock += DOOR_SEC(11);
    g_assert_null(virt_conn_pool_acquire());
    g_assert_null(virt_conn_pool_try_acquire());
    door_assert_no_calls();
    door_end();
}

static void test_door_open_when_fresh(void) {
    door_begin();
    virt_conn_pool_note_libvirtd_ok();
    g_door_clock += DOOR_SEC(9);
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    virt_conn_pool_release(c);
    door_end();
}

static gpointer door_worker(gpointer unused) {
    (void)unused;
    return virt_conn_pool_acquire();
}

static void test_door_ignores_worker_thread(void) {
    door_begin();
    virt_conn_pool_note_libvirtd_ok();
    g_door_clock += DOOR_SEC(60);
    GThread *th = g_thread_new("door-worker", door_worker, NULL);
    virConnectPtr c = g_thread_join(th);
    g_assert_nonnull(c);
    g_assert_cmpint(g_fake.version_calls, >=, 1);
    virt_conn_pool_release(c);
    door_end();
}

static void test_door_inert_without_coverage(void) {
    door_begin();
    g_door_clock += DOOR_SEC(3600);
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    virt_conn_pool_release(c);
    door_end();
}

static void test_door_restart_boundary(void) {
    door_begin();
    virt_conn_pool_note_libvirtd_ok();
    g_door_clock += DOOR_SEC(7);
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    virt_conn_pool_release(c);
    virt_conn_pool_note_libvirtd_ok();
    g_door_clock += DOOR_SEC(9);
    c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    virt_conn_pool_release(c);
    door_end();
}

static void test_door_cleared_on_stop(void) {
    door_begin();
    virt_conn_pool_note_libvirtd_ok();
    g_door_clock += DOOR_SEC(1);
    virt_conn_pool_note_coverage_end();
    g_door_clock += DOOR_SEC(20);
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    virt_conn_pool_release(c);
    door_end();
}

static void test_door_stays_closed_if_stale_at_coverage_end(void) {
    door_begin();
    virt_conn_pool_note_libvirtd_ok();
    g_door_clock += DOOR_SEC(11);
    virt_conn_pool_note_coverage_end();
    g_door_clock += DOOR_SEC(20);
    g_assert_null(virt_conn_pool_acquire());
    door_assert_no_calls();
    door_end();
}

static void test_door_transition_logged_once(void) {
    door_begin();
    virt_conn_pool_note_libvirtd_ok();
    g_door_clock += DOOR_SEC(11);
    g_test_expect_message("conn_pool", G_LOG_LEVEL_WARNING, "*libvirtd not answering for 11 s; refusing libvirt calls on the main loop*");
    g_assert_null(virt_conn_pool_acquire());
    g_test_assert_expected_messages();
    g_assert_null(virt_conn_pool_acquire());
    g_test_expect_message("conn_pool", G_LOG_LEVEL_MESSAGE, "*libvirtd answering again*");
    virt_conn_pool_note_libvirtd_ok();
    g_test_assert_expected_messages();
    virConnectPtr c = virt_conn_pool_acquire();
    g_assert_nonnull(c);
    virt_conn_pool_release(c);
    door_end();
}

void test_conn_pool_register(void) {
    g_test_add_func("/conn_pool/init_shutdown", test_init_shutdown);
    g_test_add_func("/conn_pool/stats_after_init", test_stats_after_init);
    g_test_add_func("/conn_pool/stats_null_safe", test_stats_null_safe);
    g_test_add_func("/conn_pool/wait_avg_zero_initial", test_wait_avg_zero_initial);
    g_test_add_func("/conn_pool/init_min_size", test_init_min_size);
    g_test_add_func("/conn_pool/shutdown_idempotent", test_shutdown_idempotent);
    g_test_add_func("/conn_pool/acquire_release_test_driver", test_acquire_release_test_driver);
    g_test_add_func("/conn_pool/acquire_multiple_then_release", test_acquire_multiple_then_release);
    g_test_add_func("/conn_pool/release_null_safe", test_release_null_safe);
    g_test_add_func("/conn_pool/try_acquire_no_wait_when_exhausted", test_try_acquire_no_wait_when_exhausted);
    g_test_add_func("/conn_pool/try_acquire_opens_under_limit", test_try_acquire_opens_under_limit);
    g_test_add_func("/conn_pool/acquire_health_check_outside_lock", test_acquire_health_check_outside_lock);
    g_test_add_func("/conn_pool/reconnect_outside_lock", test_reconnect_outside_lock);
    g_test_add_func("/conn_pool/reconnect_failure_keeps_total_consistent", test_reconnect_failure_keeps_total_consistent);
    g_test_add_func("/conn_pool/reaper_does_not_reconnect", test_reaper_does_not_reconnect);
    g_test_add_func("/conn_pool/door_blocks_main_when_stale", test_door_blocks_main_when_stale);
    g_test_add_func("/conn_pool/door_open_when_fresh", test_door_open_when_fresh);
    g_test_add_func("/conn_pool/door_ignores_worker_thread", test_door_ignores_worker_thread);
    g_test_add_func("/conn_pool/door_inert_without_coverage", test_door_inert_without_coverage);
    g_test_add_func("/conn_pool/door_restart_boundary", test_door_restart_boundary);
    g_test_add_func("/conn_pool/door_cleared_on_stop", test_door_cleared_on_stop);
    g_test_add_func("/conn_pool/door_stays_closed_if_stale_at_coverage_end", test_door_stays_closed_if_stale_at_coverage_end);
    g_test_add_func("/conn_pool/door_transition_logged_once", test_door_transition_logged_once);
    g_test_add_func("/conn_pool/shutdown_close_bounded", test_shutdown_close_bounded);
    g_test_add_func("/conn_pool/shutdown_close_fast_normal", test_shutdown_close_fast_normal);
    g_test_add_func("/conn_pool/normal_open_10s", test_normal_open_10s);
    g_test_add_func("/conn_pool/terminating_open_succeeds", test_terminating_open_succeeds);
    g_test_add_func("/conn_pool/terminating_open_short_bound", test_terminating_open_short_bound);
    g_test_add_func("/conn_pool/terminating_exhaust_wait_short", test_terminating_exhaust_wait_short);
    g_test_add_func("/conn_pool/terminating_exhaust_wakes_on_release", test_terminating_exhaust_wakes_on_release);
}
