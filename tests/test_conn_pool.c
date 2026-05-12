























#include <glib.h>
#include <libvirt/libvirt.h>
#include "../src/modules/virt/virt_conn_pool.h"

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
}
