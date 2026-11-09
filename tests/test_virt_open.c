                                                                                                      
                                                                                           
                                                   
                                                                                           
#include <glib.h>
#include <gio/gio.h>
#include <libvirt/libvirt.h>
#include <string.h>
#include "../src/modules/virt/pcv_virt_open.h"

static char g_slot;
static GMutex g_mu;
static GCond g_cond;
static gboolean g_release, g_hang, g_fail;
static gint g_open_calls, g_close_calls, g_keepalive_calls;
static gint g_ka_interval;
static guint g_ka_count;
static gint g_ka_ret;
static gint g_last_ro = -1;

static virConnectPtr fake_open(const gchar *uri, gboolean ro) {
    (void)uri;
    g_atomic_int_inc(&g_open_calls);
    g_atomic_int_set(&g_last_ro, ro ? 1 : 0);
    g_mutex_lock(&g_mu);
    while (g_hang && !g_release)
        g_cond_wait(&g_cond, &g_mu);
    g_mutex_unlock(&g_mu);
    return g_fail ? NULL : (virConnectPtr)&g_slot;
}
static int fake_keepalive(virConnectPtr c, int interval, unsigned int count) {
    (void)c;
    g_atomic_int_inc(&g_keepalive_calls);
    g_ka_interval = interval;
    g_ka_count = count;
    return g_ka_ret;
}
static void fake_close(virConnectPtr c) {
    (void)c;
    g_atomic_int_inc(&g_close_calls);
}

static void begin(gboolean hang) {
                                                                                                  
                                                               
    g_assert_cmpint(virInitialize(), ==, 0);
    g_release = FALSE;
    g_hang = hang;
    g_fail = FALSE;
    g_ka_ret = 0;
    g_ka_interval = 0;
    g_ka_count = 0;
    g_atomic_int_set(&g_open_calls, 0);
    g_atomic_int_set(&g_close_calls, 0);
    g_atomic_int_set(&g_keepalive_calls, 0);
    g_atomic_int_set(&g_last_ro, -1);
    pcv_virt_open_set_fn_for_test(fake_open, fake_keepalive, fake_close);
}

                                                       
static void finish(void) {
    g_mutex_lock(&g_mu);
    g_release = TRUE;
    g_cond_broadcast(&g_cond);
    g_mutex_unlock(&g_mu);
    for (int i = 0; i < 500 && pcv_virt_open_abandoned_for_test() > 0; i++)
        g_usleep(10 * 1000);
    g_assert_cmpuint(pcv_virt_open_abandoned_for_test(), ==, 0);
    pcv_virt_open_set_fn_for_test(NULL, NULL, NULL);
}

static void test_returns_conn_fast(void) {
    begin(FALSE);
    gint64 t0 = g_get_monotonic_time();
    GError *err = NULL;
    virConnectPtr c = pcv_virt_open_bounded("test:///default", FALSE, 5, &err);
    g_assert_no_error(err);
    g_assert_true(c == (virConnectPtr)&g_slot);
    g_assert_cmpint(g_get_monotonic_time() - t0, <, G_USEC_PER_SEC);
    g_assert_cmpint(g_atomic_int_get(&g_close_calls), ==, 0);
    finish();
}

                                                                   
static void test_sets_keepalive_before_return(void) {
    begin(FALSE);
    virConnectPtr c = pcv_virt_open_bounded("test:///default", FALSE, 5, NULL);
    g_assert_nonnull(c);
    g_assert_cmpint(g_atomic_int_get(&g_keepalive_calls), ==, 1);
    g_assert_cmpint(g_ka_interval, ==, 5);
    g_assert_cmpuint(g_ka_count, ==, 3);

    g_ka_ret = -1;
    c = pcv_virt_open_bounded("test:///default", FALSE, 5, NULL);
    g_assert_nonnull(c);
    g_assert_cmpint(g_atomic_int_get(&g_keepalive_calls), ==, 2);
    finish();
}

                                                                  
static void test_times_out(void) {
    begin(TRUE);
    GError *err = NULL;
    gint64 t0 = g_get_monotonic_time();
    virConnectPtr c = pcv_virt_open_bounded("test:///default", FALSE, 1, &err);
    gint64 took = g_get_monotonic_time() - t0;
    g_assert_null(c);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_error_free(err);
    g_assert_cmpint(took, >=, G_USEC_PER_SEC - G_USEC_PER_SEC / 10);
    g_assert_cmpint(took, <, G_USEC_PER_SEC + G_USEC_PER_SEC / 2);
    g_assert_cmpuint(pcv_virt_open_abandoned_for_test(), ==, 1);
    finish();
}

                                                                                 
static void test_abandoned_conn_closed_later(void) {
    begin(TRUE);
    g_assert_null(pcv_virt_open_bounded("test:///default", FALSE, 1, NULL));
    g_assert_cmpint(g_atomic_int_get(&g_close_calls), ==, 0);
    finish();
    g_assert_cmpint(g_atomic_int_get(&g_close_calls), ==, 1);
    g_assert_cmpint(g_atomic_int_get(&g_keepalive_calls), ==, 1);
}

                                                             
static void test_abandoned_cap_refuses(void) {
    begin(TRUE);
    for (int i = 0; i < 4; i++) {
        GError *err = NULL;
        g_assert_null(pcv_virt_open_bounded("test:///default", FALSE, 0, &err));
        g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
        g_error_free(err);
    }
    for (int i = 0; i < 500 && g_atomic_int_get(&g_open_calls) < 4; i++)
        g_usleep(10 * 1000);
    g_assert_cmpint(g_atomic_int_get(&g_open_calls), ==, 4);
    g_assert_cmpuint(pcv_virt_open_abandoned_for_test(), ==, 4);

    GError *err = NULL;
    gint64 t0 = g_get_monotonic_time();
    g_assert_null(pcv_virt_open_bounded("test:///default", FALSE, 5, &err));
    g_assert_cmpint(g_get_monotonic_time() - t0, <, G_USEC_PER_SEC / 2);                    
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_error_free(err);
    g_usleep(100 * 1000);
    g_assert_cmpint(g_atomic_int_get(&g_open_calls), ==, 4);

    g_mutex_lock(&g_mu);
    g_release = TRUE;
    g_cond_broadcast(&g_cond);
    g_mutex_unlock(&g_mu);
    for (int i = 0; i < 500 && pcv_virt_open_abandoned_for_test() > 0; i++)
        g_usleep(10 * 1000);
    g_assert_cmpuint(pcv_virt_open_abandoned_for_test(), ==, 0);
    g_assert_nonnull(pcv_virt_open_bounded("test:///default", FALSE, 5, NULL));
    finish();
}

                                                                                   
static void test_error_text_propagated(void) {
    pcv_virt_open_set_fn_for_test(NULL, NULL, NULL);
    GError *err = NULL;
    virConnectPtr c = pcv_virt_open_bounded("test:///pcv-no-such-file.xml", FALSE, 5, &err);
    g_assert_null(c);
    g_assert_error(err, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(err->message, "pcv-no-such-file"));
    g_error_free(err);
}

                                        
static void test_read_only_flag_passed(void) {
    begin(FALSE);
    g_assert_nonnull(pcv_virt_open_bounded("test:///default", TRUE, 5, NULL));
    g_assert_cmpint(g_atomic_int_get(&g_last_ro), ==, 1);
    g_assert_nonnull(pcv_virt_open_bounded("test:///default", FALSE, 5, NULL));
    g_assert_cmpint(g_atomic_int_get(&g_last_ro), ==, 0);
    finish();
}

                             
typedef struct {
    gboolean hang;                                  
    gint    *done;                     
    gchar   *owned;                                              
} CleanupArg;

                                                 
static gpointer cleanup_fn(gpointer data) {
    CleanupArg *a = data;
    g_mutex_lock(&g_mu);
    while (a->hang && !g_release)
        g_cond_wait(&g_cond, &g_mu);
    g_mutex_unlock(&g_mu);
    gint *done = a->done;
    g_free(a->owned);
    g_free(a);
    g_atomic_int_set(done, 1);
    return NULL;
}

static CleanupArg *cleanup_arg(gboolean hang, gint *done) {
    CleanupArg *a = g_new0(CleanupArg, 1);
    a->hang = hang;
    a->done = done;
    a->owned = g_strdup("owned-by-fn");
    return a;
}

static void test_cleanup_completes_fast(void) {
    static gint done;
    g_atomic_int_set(&done, 0);
    g_release = FALSE;
    g_test_expect_message("virt_open", G_LOG_LEVEL_MESSAGE, "*cleanup bounded: fast done*");
    gint64 t0 = g_get_monotonic_time();
    g_assert_true(pcv_virt_cleanup_bounded("fast", cleanup_fn, cleanup_arg(FALSE, &done), 5));
    g_assert_cmpint(g_get_monotonic_time() - t0, <, G_USEC_PER_SEC);
    g_test_assert_expected_messages();
    g_assert_cmpint(g_atomic_int_get(&done), ==, 1);
}

                                                               
static void test_cleanup_gives_up_after_deadline(void) {
    static gint done;
    g_atomic_int_set(&done, 0);
    g_release = FALSE;
    g_test_expect_message("virt_open", G_LOG_LEVEL_WARNING, "*cleanup bounded: stuck gave up after 1 s*");
    gint64 t0 = g_get_monotonic_time();
    g_assert_false(pcv_virt_cleanup_bounded("stuck", cleanup_fn, cleanup_arg(TRUE, &done), 1));
    gint64 took = g_get_monotonic_time() - t0;
    g_test_assert_expected_messages();
    g_assert_cmpint(took, >=, G_USEC_PER_SEC - G_USEC_PER_SEC / 10);
    g_assert_cmpint(took, <, G_USEC_PER_SEC + G_USEC_PER_SEC / 2);
    g_assert_cmpint(g_atomic_int_get(&done), ==, 0);
    g_mutex_lock(&g_mu);
    g_release = TRUE;
    g_cond_broadcast(&g_cond);
    g_mutex_unlock(&g_mu);
    for (int i = 0; i < 500 && !g_atomic_int_get(&done); i++)
        g_usleep(10 * 1000);
    g_assert_cmpint(g_atomic_int_get(&done), ==, 1);
    g_usleep(20 * 1000);                                                   
}

                                                                                 
static void test_cleanup_late_finish_safe(void) {
    static gint late, other;
    g_atomic_int_set(&late, 0);
    g_atomic_int_set(&other, 0);
    g_release = FALSE;
    g_test_expect_message("virt_open", G_LOG_LEVEL_WARNING, "*cleanup bounded: late gave up after 1 s*");
    g_assert_false(pcv_virt_cleanup_bounded("late", cleanup_fn, cleanup_arg(TRUE, &late), 1));
    g_test_expect_message("virt_open", G_LOG_LEVEL_MESSAGE, "*cleanup bounded: other done*");
    g_assert_true(pcv_virt_cleanup_bounded("other", cleanup_fn, cleanup_arg(FALSE, &other), 5));
    g_test_assert_expected_messages();
    g_assert_cmpint(g_atomic_int_get(&late), ==, 0);
    g_mutex_lock(&g_mu);
    g_release = TRUE;
    g_cond_broadcast(&g_cond);
    g_mutex_unlock(&g_mu);
    for (int i = 0; i < 500 && !g_atomic_int_get(&late); i++)
        g_usleep(10 * 1000);
    g_assert_cmpint(g_atomic_int_get(&late), ==, 1);
    g_usleep(20 * 1000);
}

void test_virt_open_register(void) {
    g_test_add_func("/virt_open/returns_conn_fast", test_returns_conn_fast);
    g_test_add_func("/virt_open/sets_keepalive_before_return", test_sets_keepalive_before_return);
    g_test_add_func("/virt_open/times_out", test_times_out);
    g_test_add_func("/virt_open/abandoned_conn_closed_later", test_abandoned_conn_closed_later);
    g_test_add_func("/virt_open/abandoned_cap_refuses", test_abandoned_cap_refuses);
    g_test_add_func("/virt_open/error_text_propagated", test_error_text_propagated);
    g_test_add_func("/virt_open/read_only_flag_passed", test_read_only_flag_passed);
    g_test_add_func("/virt_cleanup/completes_fast", test_cleanup_completes_fast);
    g_test_add_func("/virt_cleanup/gives_up_after_deadline", test_cleanup_gives_up_after_deadline);
    g_test_add_func("/virt_cleanup/late_finish_safe", test_cleanup_late_finish_safe);
}
