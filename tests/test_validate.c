































#include <glib.h>
#include "purecvisor/pcv_validate.h"



static void test_vm_name_valid(void) {
    g_assert_true(pcv_validate_vm_name("myvm"));
    g_assert_true(pcv_validate_vm_name("vm-01"));
    g_assert_true(pcv_validate_vm_name("VM_test"));
    g_assert_true(pcv_validate_vm_name("a"));
}

static void test_vm_name_invalid(void) {
    g_assert_false(pcv_validate_vm_name(NULL));
    g_assert_false(pcv_validate_vm_name(""));
    g_assert_false(pcv_validate_vm_name("vm name"));
    g_assert_false(pcv_validate_vm_name("vm/name"));
    g_assert_false(pcv_validate_vm_name("vm;name"));
    g_assert_false(pcv_validate_vm_name("../etc"));
}

static void test_vm_name_boundary(void) {

    gchar *maxname = g_strnfill(PCV_MAX_VM_NAME, 'a');
    g_assert_true(pcv_validate_vm_name(maxname));
    g_free(maxname);


    gchar *overmax = g_strnfill(PCV_MAX_VM_NAME + 1, 'a');
    g_assert_false(pcv_validate_vm_name(overmax));
    g_free(overmax);
}



static void test_snap_name_valid(void) {
    g_assert_true(pcv_validate_snap_name("snap-2025"));
    g_assert_true(pcv_validate_snap_name("SNAP_01"));
}

static void test_snap_name_invalid(void) {
    g_assert_false(pcv_validate_snap_name(NULL));
    g_assert_false(pcv_validate_snap_name("snap name"));
}



static void test_bridge_name_valid(void) {
    g_assert_true(pcv_validate_bridge_name("virbr0"));
    g_assert_true(pcv_validate_bridge_name("br-eth0"));
}

static void test_bridge_name_invalid(void) {
    g_assert_false(pcv_validate_bridge_name(NULL));

    gchar *over = g_strnfill(PCV_MAX_BRIDGE_NAME + 1, 'x');
    g_assert_false(pcv_validate_bridge_name(over));
    g_free(over);
}



static void test_remote_host_valid(void) {
    g_assert_true(pcv_validate_remote_host("192.0.2.20"));
    g_assert_true(pcv_validate_remote_host("pcv-node-1"));
    g_assert_true(pcv_validate_remote_host("node-1.example.internal"));
}

static void test_remote_host_invalid(void) {
    g_assert_false(pcv_validate_remote_host(NULL));
    g_assert_false(pcv_validate_remote_host(""));
    g_assert_false(pcv_validate_remote_host("host name"));
    g_assert_false(pcv_validate_remote_host("host;id"));
    g_assert_false(pcv_validate_remote_host("host$(id)"));
    g_assert_false(pcv_validate_remote_host("-leadingdash"));
    g_assert_false(pcv_validate_remote_host(".leadingdot"));
    g_assert_false(pcv_validate_remote_host("trailingdot."));
}

static void test_ssh_user_valid(void) {
    g_assert_true(pcv_validate_ssh_user("pcvdev"));
    g_assert_true(pcv_validate_ssh_user("pcv-dev_1"));
    g_assert_true(pcv_validate_ssh_user("user.name"));
}

static void test_ssh_user_invalid(void) {
    g_assert_false(pcv_validate_ssh_user(NULL));
    g_assert_false(pcv_validate_ssh_user(""));
    g_assert_false(pcv_validate_ssh_user("-oProxyCommand"));
    g_assert_false(pcv_validate_ssh_user("user@host"));
    g_assert_false(pcv_validate_ssh_user("../root"));
    g_assert_false(pcv_validate_ssh_user("user;id"));
}



static void test_iso_path_valid(void) {
    g_assert_true(pcv_validate_iso_path("/var/lib/images/ubuntu.iso"));
    g_assert_true(pcv_validate_iso_path("/tmp/test.iso"));
}

static void test_iso_path_invalid(void) {
    g_assert_false(pcv_validate_iso_path(NULL));
    g_assert_false(pcv_validate_iso_path("relative/path.iso"));
    g_assert_false(pcv_validate_iso_path("/var/../../etc/passwd"));
    g_assert_false(pcv_validate_iso_path(""));
}



static void test_memory_mb(void) {
    g_assert_true(pcv_validate_memory_mb(PCV_MIN_MEMORY_MB));
    g_assert_true(pcv_validate_memory_mb(4096));
    g_assert_true(pcv_validate_memory_mb(PCV_MAX_MEMORY_MB));
    g_assert_false(pcv_validate_memory_mb(PCV_MIN_MEMORY_MB - 1));
    g_assert_false(pcv_validate_memory_mb(PCV_MAX_MEMORY_MB + 1));
    g_assert_false(pcv_validate_memory_mb(0));
    g_assert_false(pcv_validate_memory_mb(-1));
}

static void test_vcpu(void) {
    g_assert_true(pcv_validate_vcpu(PCV_MIN_VCPU));
    g_assert_true(pcv_validate_vcpu(4));
    g_assert_true(pcv_validate_vcpu(PCV_MAX_VCPU));
    g_assert_false(pcv_validate_vcpu(0));
    g_assert_false(pcv_validate_vcpu(PCV_MAX_VCPU + 1));
    g_assert_false(pcv_validate_vcpu(-1));
}

static void test_disk_gb(void) {
    g_assert_true(pcv_validate_disk_gb(PCV_MIN_DISK_GB));
    g_assert_true(pcv_validate_disk_gb(100));
    g_assert_true(pcv_validate_disk_gb(PCV_MAX_DISK_GB));
    g_assert_false(pcv_validate_disk_gb(0));
    g_assert_false(pcv_validate_disk_gb(PCV_MAX_DISK_GB + 1));
}



static void test_create_params_valid(void) {
    GError *err = NULL;
    gboolean ok = pcv_validate_vm_create_params(
        "myvm", 2, 2048, 20, NULL, NULL, &err);
    g_assert_true(ok);
    g_assert_null(err);
}

static void test_create_params_bad_name(void) {
    GError *err = NULL;
    gboolean ok = pcv_validate_vm_create_params(
        "bad name!", 2, 2048, 20, NULL, NULL, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_create_params_bad_vcpu(void) {
    GError *err = NULL;
    gboolean ok = pcv_validate_vm_create_params(
        "myvm", 0, 2048, 20, NULL, NULL, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_create_params_bad_mem(void) {
    GError *err = NULL;
    gboolean ok = pcv_validate_vm_create_params(
        "myvm", 2, 64, 20, NULL, NULL, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_error_free(err);
}



static void test_container_image_valid(void) {
    g_assert_true(pcv_validate_container_image("ubuntu:22.04"));
    g_assert_true(pcv_validate_container_image("alpine:3.18"));
    g_assert_true(pcv_validate_container_image("debian:bookworm"));
}

static void test_container_image_invalid(void) {
    g_assert_false(pcv_validate_container_image(NULL));
    g_assert_false(pcv_validate_container_image("ubuntu"));
    g_assert_false(pcv_validate_container_image(":22.04"));
    g_assert_false(pcv_validate_container_image("Ubuntu:22.04"));
}



static void test_exec_cmd_valid(void) {
    g_assert_true(pcv_validate_exec_cmd("ls -la"));
    g_assert_true(pcv_validate_exec_cmd("/usr/bin/python3 script.py"));
}

static void test_exec_cmd_invalid(void) {
    g_assert_false(pcv_validate_exec_cmd(NULL));
    g_assert_false(pcv_validate_exec_cmd(""));
}



void test_validate_register(void) {

    g_test_add_func("/validate/vm_name/valid",    test_vm_name_valid);
    g_test_add_func("/validate/vm_name/invalid",  test_vm_name_invalid);
    g_test_add_func("/validate/vm_name/boundary", test_vm_name_boundary);

    g_test_add_func("/validate/snap_name/valid",   test_snap_name_valid);
    g_test_add_func("/validate/snap_name/invalid", test_snap_name_invalid);

    g_test_add_func("/validate/bridge/valid",   test_bridge_name_valid);
    g_test_add_func("/validate/bridge/invalid", test_bridge_name_invalid);

    g_test_add_func("/validate/remote_host/valid", test_remote_host_valid);
    g_test_add_func("/validate/remote_host/invalid", test_remote_host_invalid);
    g_test_add_func("/validate/ssh_user/valid", test_ssh_user_valid);
    g_test_add_func("/validate/ssh_user/invalid", test_ssh_user_invalid);

    g_test_add_func("/validate/iso_path/valid",   test_iso_path_valid);
    g_test_add_func("/validate/iso_path/invalid", test_iso_path_invalid);

    g_test_add_func("/validate/memory_mb", test_memory_mb);
    g_test_add_func("/validate/vcpu",      test_vcpu);
    g_test_add_func("/validate/disk_gb",   test_disk_gb);

    g_test_add_func("/validate/create_params/valid",    test_create_params_valid);
    g_test_add_func("/validate/create_params/bad_name", test_create_params_bad_name);
    g_test_add_func("/validate/create_params/bad_vcpu", test_create_params_bad_vcpu);
    g_test_add_func("/validate/create_params/bad_mem",  test_create_params_bad_mem);

    g_test_add_func("/validate/container_image/valid",   test_container_image_valid);
    g_test_add_func("/validate/container_image/invalid", test_container_image_invalid);

    g_test_add_func("/validate/exec_cmd/valid",   test_exec_cmd_valid);
    g_test_add_func("/validate/exec_cmd/invalid", test_exec_cmd_invalid);
}
