













#include <glib.h>
#include "purecvisor/pcv_validate.h"



static void test_bridge_name_valid(void) {
    g_assert_true(pcv_validate_bridge_name("pcvbr0"));
    g_assert_true(pcv_validate_bridge_name("br-lan"));
    g_assert_true(pcv_validate_bridge_name("virbr0"));
    g_assert_true(pcv_validate_bridge_name("a"));
}

static void test_bridge_name_invalid(void) {
    g_assert_false(pcv_validate_bridge_name(NULL));
    g_assert_false(pcv_validate_bridge_name(""));
    g_assert_false(pcv_validate_bridge_name("br name"));
    g_assert_false(pcv_validate_bridge_name("br;inject"));
    g_assert_false(pcv_validate_bridge_name("../etc"));
}

static void test_bridge_name_boundary(void) {

    gchar buf[32];
    memset(buf, 'a', PCV_MAX_BRIDGE_NAME);
    buf[PCV_MAX_BRIDGE_NAME] = '\0';
    g_assert_true(pcv_validate_bridge_name(buf));


    buf[PCV_MAX_BRIDGE_NAME] = 'x';
    buf[PCV_MAX_BRIDGE_NAME + 1] = '\0';
    g_assert_false(pcv_validate_bridge_name(buf));
}



static void test_network_mode_strings(void) {

    g_assert_true(pcv_validate_bridge_name("nat"));
    g_assert_true(pcv_validate_bridge_name("isolated"));
    g_assert_true(pcv_validate_bridge_name("routed"));
    g_assert_true(pcv_validate_bridge_name("bridge"));
}



void test_network_register(void) {
    g_test_add_func("/network/bridge_name/valid",    test_bridge_name_valid);
    g_test_add_func("/network/bridge_name/invalid",  test_bridge_name_invalid);
    g_test_add_func("/network/bridge_name/boundary", test_bridge_name_boundary);
    g_test_add_func("/network/mode_strings",         test_network_mode_strings);
}
