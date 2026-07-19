
#include <glib.h>
#include "../src/modules/network/vm_iface.h"

static void test_parse_domiflist_multi_nic(void) {
    const gchar *out =
        " Interface   Type     Source   Model    MAC\n"
        "---------------------------------------------------------\n"
        " vnet3       bridge   pcvbr0   virtio   52:54:00:aa:bb:01\n"
        " vnet4       bridge   pcvbr1   virtio   52:54:00:aa:bb:02\n"
        " tap9        ethernet -        virtio   52:54:00:aa:bb:03\n"
        "\n";
    GPtrArray *arr = pcv_vm_iface_parse_domiflist(out);
    g_assert_cmpuint(arr->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(arr, 0), ==, "vnet3");
    g_assert_cmpstr(g_ptr_array_index(arr, 1), ==, "vnet4");
    g_assert_cmpstr(g_ptr_array_index(arr, 2), ==, "tap9");
    g_ptr_array_unref(arr);
}

static void test_parse_domiflist_empty_and_null(void) {
    GPtrArray *arr = pcv_vm_iface_parse_domiflist(NULL);
    g_assert_nonnull(arr);
    g_assert_cmpuint(arr->len, ==, 0);
    g_ptr_array_unref(arr);

    arr = pcv_vm_iface_parse_domiflist(" Interface   Type   Source   Model   MAC\n---\n\n");
    g_assert_cmpuint(arr->len, ==, 0);
    g_ptr_array_unref(arr);
}

void test_vm_iface_register(void) {
    g_test_add_func("/vm_iface/parse/multi_nic", test_parse_domiflist_multi_nic);
    g_test_add_func("/vm_iface/parse/empty",     test_parse_domiflist_empty_and_null);
}
