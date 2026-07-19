
#include <glib.h>
#include "purecvisor/pcv_validate.h"

static void test_cidr_ipv4_valid(void) {
    g_assert_true(pcv_validate_cidr("10.0.0.0/8"));
    g_assert_true(pcv_validate_cidr("192.0.2.10/24"));
    g_assert_true(pcv_validate_cidr("172.16.0.0/12"));
    g_assert_true(pcv_validate_cidr("10.0.0.1/32"));
    g_assert_true(pcv_validate_cidr("0.0.0.0/0"));
}

static void test_cidr_ipv4_invalid(void) {
    g_assert_false(pcv_validate_cidr(NULL));
    g_assert_false(pcv_validate_cidr(""));
    g_assert_false(pcv_validate_cidr("10.0.0.0"));
    g_assert_false(pcv_validate_cidr("/24"));
    g_assert_false(pcv_validate_cidr("10.0.0.0/"));
    g_assert_false(pcv_validate_cidr("10.0.0.0/33"));
    g_assert_false(pcv_validate_cidr("10.0.0.0/-1"));
    g_assert_false(pcv_validate_cidr("999.999.999.999/24"));
    g_assert_false(pcv_validate_cidr("10.0.0/24"));
    g_assert_false(pcv_validate_cidr("10.0.0.0.0/24"));
}

static void test_cidr_ipv6_valid(void) {
    g_assert_true(pcv_validate_cidr("2001:db8::/32"));
    g_assert_true(pcv_validate_cidr("fe80::/10"));
    g_assert_true(pcv_validate_cidr("::1/128"));
    g_assert_true(pcv_validate_cidr("::/0"));
}

static void test_cidr_ipv6_invalid(void) {
    g_assert_false(pcv_validate_cidr("2001:db8::/129"));
    g_assert_false(pcv_validate_cidr("gggg::/32"));
}

static void test_pci_addr_valid(void) {
    g_assert_true(pcv_validate_pci_addr("0000:3b:00.0"));
    g_assert_true(pcv_validate_pci_addr("0000:00:00.0"));
    g_assert_true(pcv_validate_pci_addr("ffff:ff:1f.7"));
    g_assert_true(pcv_validate_pci_addr("0000:00:1f.0"));
}

static void test_pci_addr_invalid(void) {
    g_assert_false(pcv_validate_pci_addr(NULL));
    g_assert_false(pcv_validate_pci_addr(""));
    g_assert_true(pcv_validate_pci_addr("00:3b:00.0"));
    g_assert_false(pcv_validate_pci_addr("0000:3b:20.0"));
    g_assert_false(pcv_validate_pci_addr("0000:3b:00.8"));
    g_assert_false(pcv_validate_pci_addr("../etc/passwd"));
    g_assert_false(pcv_validate_pci_addr("0000:3b:00.0extra"));
    g_assert_false(pcv_validate_pci_addr("not-a-pci-addr"));
}

static void test_net_create_nat_valid(void) {
    GError *err = NULL;
    g_assert_true(pcv_validate_network_create_params(
        "pcvbr0", "nat", "10.0.0.1/24", NULL, &err));
    g_assert_null(err);
}

static void test_net_create_nat_no_cidr(void) {
    GError *err = NULL;
    g_assert_false(pcv_validate_network_create_params(
        "pcvbr0", "nat", NULL, NULL, &err));
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_net_create_bridge_valid(void) {
    GError *err = NULL;
    g_assert_true(pcv_validate_network_create_params(
        "pcvbr0", "bridge", NULL, "enp42s0", &err));
    g_assert_null(err);
}

static void test_net_create_bridge_no_phys(void) {
    GError *err = NULL;
    g_assert_false(pcv_validate_network_create_params(
        "pcvbr0", "bridge", NULL, NULL, &err));
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_net_create_invalid_mode(void) {
    GError *err = NULL;
    g_assert_false(pcv_validate_network_create_params(
        "pcvbr0", "invalid_mode", "10.0.0.1/24", NULL, &err));
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_net_create_invalid_bridge_name(void) {
    GError *err = NULL;
    g_assert_false(pcv_validate_network_create_params(
        NULL, "nat", "10.0.0.1/24", NULL, &err));
    g_assert_nonnull(err);
    g_error_free(err);
}

static void test_net_create_null_mode_defaults_nat(void) {
    GError *err = NULL;

    g_assert_true(pcv_validate_network_create_params(
        "br0", NULL, "10.0.0.1/24", NULL, &err));
    g_assert_null(err);
}

static void test_net_create_isolated_valid(void) {
    GError *err = NULL;
    g_assert_true(pcv_validate_network_create_params(
        "isol0", "isolated", "192.0.2.10/24", NULL, &err));
    g_assert_null(err);
}

static void test_net_create_routed_valid(void) {
    GError *err = NULL;
    g_assert_true(pcv_validate_network_create_params(
        "rout0", "routed", "172.16.0.1/16", NULL, &err));
    g_assert_null(err);
}

static void test_private_cidr_valid(void) {
    g_assert_true(pcv_validate_private_cidr("10.0.0.0/8"));
    g_assert_true(pcv_validate_private_cidr("172.16.0.0/12"));
    g_assert_true(pcv_validate_private_cidr("192.0.2.10/24"));
}

static void test_private_cidr_invalid(void) {

    g_assert_false(pcv_validate_private_cidr("8.8.8.0/24"));
    g_assert_false(pcv_validate_private_cidr("1.1.1.0/24"));

    g_assert_false(pcv_validate_private_cidr(NULL));
    g_assert_false(pcv_validate_private_cidr(""));
}

static void test_port_valid(void) {
    g_assert_true(pcv_validate_port(80));
    g_assert_true(pcv_validate_port(443));
    g_assert_true(pcv_validate_port(1));
    g_assert_true(pcv_validate_port(65535));
}

static void test_port_invalid(void) {
    g_assert_false(pcv_validate_port(0));
    g_assert_false(pcv_validate_port(65536));
    g_assert_false(pcv_validate_port(-1));
}

static void test_disk_size_gb_valid(void) {
    g_assert_true(pcv_validate_disk_size_gb(1));
    g_assert_true(pcv_validate_disk_size_gb(100));
    g_assert_true(pcv_validate_disk_size_gb(2048));
}

static void test_disk_size_gb_invalid(void) {
    g_assert_false(pcv_validate_disk_size_gb(0));
    g_assert_false(pcv_validate_disk_size_gb(-1));
    g_assert_false(pcv_validate_disk_size_gb(2049));
}

static void test_zvol_name_valid(void) {
    g_assert_true(pcv_validate_zvol_name("pcvpool"));
    g_assert_true(pcv_validate_zvol_name("pcvpool.vms.web01"));
    g_assert_true(pcv_validate_zvol_name("a"));
    g_assert_true(pcv_validate_zvol_name("vol-001_test"));
}

static void test_zvol_name_invalid(void) {
    g_assert_false(pcv_validate_zvol_name(NULL));
    g_assert_false(pcv_validate_zvol_name(""));
    g_assert_false(pcv_validate_zvol_name("pool/../escape"));
    g_assert_false(pcv_validate_zvol_name("_leadunder"));
    g_assert_false(pcv_validate_zvol_name(".dotstart"));
}

void test_validate_ext_register(void) {

    g_test_add_func("/validate_ext/cidr/ipv4_valid",   test_cidr_ipv4_valid);
    g_test_add_func("/validate_ext/cidr/ipv4_invalid", test_cidr_ipv4_invalid);
    g_test_add_func("/validate_ext/cidr/ipv6_valid",   test_cidr_ipv6_valid);
    g_test_add_func("/validate_ext/cidr/ipv6_invalid", test_cidr_ipv6_invalid);

    g_test_add_func("/validate_ext/pci_addr/valid",   test_pci_addr_valid);
    g_test_add_func("/validate_ext/pci_addr/invalid", test_pci_addr_invalid);

    g_test_add_func("/validate_ext/net_create/nat_valid",          test_net_create_nat_valid);
    g_test_add_func("/validate_ext/net_create/nat_no_cidr",        test_net_create_nat_no_cidr);
    g_test_add_func("/validate_ext/net_create/bridge_valid",       test_net_create_bridge_valid);
    g_test_add_func("/validate_ext/net_create/bridge_no_phys",     test_net_create_bridge_no_phys);
    g_test_add_func("/validate_ext/net_create/invalid_mode",       test_net_create_invalid_mode);
    g_test_add_func("/validate_ext/net_create/invalid_bridge_name",test_net_create_invalid_bridge_name);
    g_test_add_func("/validate_ext/net_create/null_mode_nat",      test_net_create_null_mode_defaults_nat);
    g_test_add_func("/validate_ext/net_create/isolated_valid",     test_net_create_isolated_valid);
    g_test_add_func("/validate_ext/net_create/routed_valid",       test_net_create_routed_valid);

    g_test_add_func("/validate_ext/private_cidr/valid",   test_private_cidr_valid);
    g_test_add_func("/validate_ext/private_cidr/invalid", test_private_cidr_invalid);

    g_test_add_func("/validate_ext/port/valid",   test_port_valid);
    g_test_add_func("/validate_ext/port/invalid", test_port_invalid);

    g_test_add_func("/validate_ext/disk_size_gb/valid",   test_disk_size_gb_valid);
    g_test_add_func("/validate_ext/disk_size_gb/invalid", test_disk_size_gb_invalid);

    g_test_add_func("/validate_ext/zvol_name/valid",   test_zvol_name_valid);
    g_test_add_func("/validate_ext/zvol_name/invalid", test_zvol_name_invalid);
}
