
#include <glib.h>
#include "../src/modules/network/vm_vnet_cache.h"

static GPtrArray *_arr(const gchar *a, const gchar *b) {
    GPtrArray *p = g_ptr_array_new_with_free_func(g_free);
    if (a) g_ptr_array_add(p, g_strdup(a));
    if (b) g_ptr_array_add(p, g_strdup(b));
    return p;
}

static void test_put_get(void) {
    GPtrArray *in = _arr("vnet3", "vnet4");
    pcv_vm_vnet_cache_put("web-01", in);
    g_ptr_array_unref(in);

    GPtrArray *out = pcv_vm_vnet_cache_get("web-01");
    g_assert_nonnull(out);
    g_assert_cmpuint(out->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(out, 0), ==, "vnet3");
    g_assert_cmpstr(g_ptr_array_index(out, 1), ==, "vnet4");
    g_ptr_array_unref(out);

    g_assert_null(pcv_vm_vnet_cache_get("nonexist-vm"));
}

static void test_overwrite_evict(void) {
    GPtrArray *a = _arr("vnet0", NULL);
    pcv_vm_vnet_cache_put("db-01", a);
    g_ptr_array_unref(a);
    GPtrArray *b = _arr("vnet9", NULL);
    pcv_vm_vnet_cache_put("db-01", b);
    g_ptr_array_unref(b);

    GPtrArray *out = pcv_vm_vnet_cache_get("db-01");
    g_assert_cmpuint(out->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(out, 0), ==, "vnet9");
    g_ptr_array_unref(out);

    pcv_vm_vnet_cache_evict("db-01");
    g_assert_null(pcv_vm_vnet_cache_get("db-01"));
    pcv_vm_vnet_cache_evict("db-01");
}

static void test_snapshot_independent(void) {
    GPtrArray *in = _arr("vnet1", NULL);
    pcv_vm_vnet_cache_put("iso-vm", in);
    g_ptr_array_unref(in);

    GPtrArray *o1 = pcv_vm_vnet_cache_get("iso-vm");
    g_ptr_array_unref(o1);
    GPtrArray *o2 = pcv_vm_vnet_cache_get("iso-vm");
    g_assert_nonnull(o2);
    g_assert_cmpstr(g_ptr_array_index(o2, 0), ==, "vnet1");
    g_ptr_array_unref(o2);
}

void test_vm_vnet_cache_register(void) {
    g_test_add_func("/sg_vnet_cache/put_get",            test_put_get);
    g_test_add_func("/sg_vnet_cache/overwrite_evict",    test_overwrite_evict);
    g_test_add_func("/sg_vnet_cache/snapshot_independent", test_snapshot_independent);
}
