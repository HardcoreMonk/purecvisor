
#include <glib.h>
#include <string.h>
#include "purecvisor/pcv_validate.h"

static void test_container_image_valid(void) {
    g_assert_true(pcv_validate_container_image("ubuntu:22.04"));
    g_assert_true(pcv_validate_container_image("debian:bookworm"));
    g_assert_true(pcv_validate_container_image("alpine:3.18"));
    g_assert_true(pcv_validate_container_image("centos:9-stream"));
}

static void test_container_image_invalid(void) {
    g_assert_false(pcv_validate_container_image(NULL));
    g_assert_false(pcv_validate_container_image(""));
    g_assert_false(pcv_validate_container_image("ubuntu"));
    g_assert_false(pcv_validate_container_image(":22.04"));
    g_assert_false(pcv_validate_container_image("Ubuntu:22.04"));
    g_assert_false(pcv_validate_container_image("ubu ntu:22.04"));
}

static void test_container_image_boundary(void) {

    gchar buf[256];
    memset(buf, 'a', 60);
    buf[60] = ':';
    memset(buf + 61, '1', PCV_MAX_CONTAINER_IMAGE - 61);
    buf[PCV_MAX_CONTAINER_IMAGE] = '\0';
    g_assert_true(pcv_validate_container_image(buf));

    buf[PCV_MAX_CONTAINER_IMAGE] = 'x';
    buf[PCV_MAX_CONTAINER_IMAGE + 1] = '\0';
    g_assert_false(pcv_validate_container_image(buf));
}

static void test_exec_cmd_valid(void) {
    g_assert_true(pcv_validate_exec_cmd("ls -la"));
    g_assert_true(pcv_validate_exec_cmd("echo hello"));
    g_assert_true(pcv_validate_exec_cmd("cat /etc/hostname"));
    g_assert_true(pcv_validate_exec_cmd("a"));
}

static void test_exec_cmd_invalid(void) {
    g_assert_false(pcv_validate_exec_cmd(NULL));
    g_assert_false(pcv_validate_exec_cmd(""));

    gchar *long_cmd = g_malloc0(PCV_MAX_EXEC_CMD + 2);
    memset(long_cmd, 'a', PCV_MAX_EXEC_CMD + 1);
    g_assert_false(pcv_validate_exec_cmd(long_cmd));
    g_free(long_cmd);
}

static void test_container_name_valid(void) {
    g_assert_true(pcv_validate_vm_name("web-ctr-1"));
    g_assert_true(pcv_validate_vm_name("my_container"));
    g_assert_true(pcv_validate_vm_name("ctr01"));
}

static void test_container_name_invalid(void) {
    g_assert_false(pcv_validate_vm_name("web ctr"));
    g_assert_false(pcv_validate_vm_name("ctr;rm"));
    g_assert_false(pcv_validate_vm_name("../../etc"));
}

void test_container_register(void) {
    g_test_add_func("/container/image/valid",      test_container_image_valid);
    g_test_add_func("/container/image/invalid",    test_container_image_invalid);
    g_test_add_func("/container/image/boundary",   test_container_image_boundary);
    g_test_add_func("/container/exec_cmd/valid",   test_exec_cmd_valid);
    g_test_add_func("/container/exec_cmd/invalid", test_exec_cmd_invalid);
    g_test_add_func("/container/name/valid",       test_container_name_valid);
    g_test_add_func("/container/name/invalid",     test_container_name_invalid);
}
