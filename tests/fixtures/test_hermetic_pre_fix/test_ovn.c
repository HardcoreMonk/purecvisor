#include <glib.h>

static void test_overlay_list_empty_single(void)
{
    JsonArray *array = pcv_overlay_list(NULL);
    g_assert_nonnull(array);
    g_assert_cmpuint(json_array_get_length(array), ==, 0);
    json_array_unref(array);
}

void test_ovn_pre_fix_register(void) {
    g_test_add_func("/overlay/list/empty_single",  test_overlay_list_empty_single);
}
