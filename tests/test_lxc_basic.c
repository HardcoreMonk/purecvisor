
#include <glib.h>
#include <string.h>
#include "purecvisor/pcv_validate.h"

typedef enum {
    TEST_LXC_STATE_STOPPED  = 0,
    TEST_LXC_STATE_STARTING = 1,
    TEST_LXC_STATE_RUNNING  = 2,
    TEST_LXC_STATE_STOPPING = 3,
    TEST_LXC_STATE_FROZEN   = 4,
    TEST_LXC_STATE_UNKNOWN  = 99,
} TestLxcState;

static void test_lxc_state_values(void) {
    g_assert_cmpint(TEST_LXC_STATE_STOPPED,  ==, 0);
    g_assert_cmpint(TEST_LXC_STATE_STARTING, ==, 1);
    g_assert_cmpint(TEST_LXC_STATE_RUNNING,  ==, 2);
    g_assert_cmpint(TEST_LXC_STATE_STOPPING, ==, 3);
    g_assert_cmpint(TEST_LXC_STATE_FROZEN,   ==, 4);
    g_assert_cmpint(TEST_LXC_STATE_UNKNOWN,  ==, 99);
}

static const gchar *
_lxc_state_to_str(TestLxcState s)
{
    switch (s) {
    case TEST_LXC_STATE_STOPPED:  return "STOPPED";
    case TEST_LXC_STATE_STARTING: return "STARTING";
    case TEST_LXC_STATE_RUNNING:  return "RUNNING";
    case TEST_LXC_STATE_STOPPING: return "STOPPING";
    case TEST_LXC_STATE_FROZEN:   return "FROZEN";
    case TEST_LXC_STATE_UNKNOWN:  return "UNKNOWN";
    default:                      return "UNKNOWN";
    }
}

static void test_lxc_state_strings(void) {
    g_assert_cmpstr(_lxc_state_to_str(TEST_LXC_STATE_STOPPED),  ==, "STOPPED");
    g_assert_cmpstr(_lxc_state_to_str(TEST_LXC_STATE_RUNNING),  ==, "RUNNING");
    g_assert_cmpstr(_lxc_state_to_str(TEST_LXC_STATE_FROZEN),   ==, "FROZEN");
    g_assert_cmpstr(_lxc_state_to_str(TEST_LXC_STATE_UNKNOWN),  ==, "UNKNOWN");
    g_assert_cmpstr(_lxc_state_to_str((TestLxcState)42),         ==, "UNKNOWN");
}

static void test_lxc_name_valid(void) {
    g_assert_true(pcv_validate_vm_name("nginx-web"));
    g_assert_true(pcv_validate_vm_name("redis01"));
    g_assert_true(pcv_validate_vm_name("app_server"));
    g_assert_true(pcv_validate_vm_name("a"));
}

static void test_lxc_name_invalid(void) {
    g_assert_false(pcv_validate_vm_name(NULL));
    g_assert_false(pcv_validate_vm_name(""));
    g_assert_false(pcv_validate_vm_name("container name"));
    g_assert_false(pcv_validate_vm_name("ctr;echo"));
    g_assert_false(pcv_validate_vm_name("../../../etc"));

}

static void test_lxc_image_distro_release(void) {
    g_assert_true(pcv_validate_container_image("ubuntu:22.04"));
    g_assert_true(pcv_validate_container_image("ubuntu:24.04"));
    g_assert_true(pcv_validate_container_image("debian:12"));
    g_assert_true(pcv_validate_container_image("debian:bookworm"));
    g_assert_true(pcv_validate_container_image("alpine:3.19"));
    g_assert_true(pcv_validate_container_image("centos:9-stream"));
}

static void test_lxc_image_invalid_format(void) {
    g_assert_false(pcv_validate_container_image(NULL));
    g_assert_false(pcv_validate_container_image(""));
    g_assert_false(pcv_validate_container_image("ubuntu"));
    g_assert_false(pcv_validate_container_image(":22.04"));
    g_assert_false(pcv_validate_container_image("Ubuntu:22.04"));
}

static void test_lxc_bridge_name_valid(void) {
    g_assert_true(pcv_validate_bridge_name("virbr0"));
    g_assert_true(pcv_validate_bridge_name("pcvbr0"));
    g_assert_true(pcv_validate_bridge_name("br-lan"));
}

static void test_lxc_bridge_name_invalid(void) {
    g_assert_false(pcv_validate_bridge_name(NULL));
    g_assert_false(pcv_validate_bridge_name(""));
    g_assert_false(pcv_validate_bridge_name("a bridge"));
}

static gboolean
_validate_resource_limits(gint cpu_percent, gint memory_mb,
                          gint cpu_weight, gint pids_max)
{

    if (cpu_percent < 0) return FALSE;

    if (memory_mb < 0) return FALSE;

    if (cpu_weight < 0 || cpu_weight > 10000) return FALSE;

    if (pids_max < 0) return FALSE;
    return TRUE;
}

static void test_lxc_resource_limits_valid(void) {
    g_assert_true(_validate_resource_limits(100, 512, 100, 256));
    g_assert_true(_validate_resource_limits(200, 1024, 10000, 0));
    g_assert_true(_validate_resource_limits(0, 0, 0, 0));
}

static void test_lxc_resource_limits_invalid(void) {
    g_assert_false(_validate_resource_limits(-1, 512, 100, 256));
    g_assert_false(_validate_resource_limits(100, -1, 100, 256));
    g_assert_false(_validate_resource_limits(100, 512, 10001, 256));
    g_assert_false(_validate_resource_limits(100, 512, -1, 256));
    g_assert_false(_validate_resource_limits(100, 512, 100, -1));
}

void test_lxc_basic_register(void) {
    g_test_add_func("/lxc/state/values",              test_lxc_state_values);
    g_test_add_func("/lxc/state/strings",             test_lxc_state_strings);
    g_test_add_func("/lxc/name/valid",                test_lxc_name_valid);
    g_test_add_func("/lxc/name/invalid",              test_lxc_name_invalid);
    g_test_add_func("/lxc/image/distro_release",      test_lxc_image_distro_release);
    g_test_add_func("/lxc/image/invalid_format",      test_lxc_image_invalid_format);
    g_test_add_func("/lxc/bridge/valid",              test_lxc_bridge_name_valid);
    g_test_add_func("/lxc/bridge/invalid",            test_lxc_bridge_name_invalid);
    g_test_add_func("/lxc/resource_limits/valid",     test_lxc_resource_limits_valid);
    g_test_add_func("/lxc/resource_limits/invalid",   test_lxc_resource_limits_invalid);
}
