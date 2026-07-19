
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "purecvisor/pcv_validate.h"
#include "modules/network/network_firewall_host.h"
#include "modules/network/network_firewall.h"
#include "utils/pcv_spawn.h"

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

static void test_ipv6_prefix_valid(void) {
    g_assert_true(pcv_validate_ipv6_prefix("fd00::/64"));
    g_assert_true(pcv_validate_ipv6_prefix("fd00:1::/64"));
    g_assert_true(pcv_validate_ipv6_prefix("2001:db8::/48"));
}

static void test_ipv6_prefix_injection(void) {

    g_assert_false(pcv_validate_ipv6_prefix("fd00::/64\ndhcp-script=/etc/x/64"));
    g_assert_false(pcv_validate_ipv6_prefix("fd00::/64\ndhcp-script=/x"));
    g_assert_false(pcv_validate_ipv6_prefix("fd00::/64 dhcp-script=/x"));
    g_assert_false(pcv_validate_ipv6_prefix("fd00::/64;evil"));
    g_assert_false(pcv_validate_ipv6_prefix("gg00::/64"));
    g_assert_false(pcv_validate_ipv6_prefix("fd00::"));
    g_assert_false(pcv_validate_ipv6_prefix(""));
    g_assert_false(pcv_validate_ipv6_prefix(NULL));
}

static void test_mode_set_cidr_validation(void) {
    g_assert_true(pcv_validate_private_cidr("10.10.10.1/24"));
    g_assert_true(pcv_validate_private_cidr("192.0.2.10/24"));
    g_assert_false(pcv_validate_private_cidr("0.0.0.0/0"));
    g_assert_false(pcv_validate_private_cidr("8.8.8.8/24"));
    g_assert_false(pcv_validate_private_cidr("10.0.0.0/24; nft flush ruleset"));
    g_assert_false(pcv_validate_private_cidr(NULL));
}

static void test_bridge_name_pkill_traversal(void) {
    g_assert_false(pcv_validate_bridge_name("../../etc/passwd"));
    g_assert_false(pcv_validate_bridge_name("br/../x"));
    g_assert_false(pcv_validate_bridge_name("br0/../../root"));
    g_assert_false(pcv_validate_bridge_name("br0\nx"));
    g_assert_true(pcv_validate_bridge_name("pcvbr0"));
}

static void test_v11_iface_ip_vm_validation(void) {

    g_assert_true(pcv_validate_iface_name("eth0.100"));
    g_assert_true(pcv_validate_iface_name("vnet0"));
    g_assert_false(pcv_validate_iface_name("-eth0"));
    g_assert_false(pcv_validate_iface_name("eth0;rm"));
    g_assert_false(pcv_validate_iface_name(NULL));

    g_assert_true(pcv_validate_ip_literal("192.0.2.10"));
    g_assert_true(pcv_validate_ip_literal("fd00::1"));
    g_assert_false(pcv_validate_ip_literal("10.0.0.0/24"));
    g_assert_false(pcv_validate_ip_literal("1.2.3.4; evil"));

    g_assert_true(pcv_validate_vm_name("web-01"));
    g_assert_false(pcv_validate_vm_name("vm; rm -rf /"));
    g_assert_false(pcv_validate_vm_name("../vm"));
}

static void test_firewall_teardown_token_match(void) {
    const gchar *bridge = "br0";
    gchar *needle = g_strdup_printf("\"%s\"", bridge);

    const gchar *line_self  = "iifname \"br0\" accept # handle 5";
    const gchar *line_other = "iifname \"br0x\" accept # handle 9";

    g_assert_nonnull(strstr(line_self, needle));
    g_assert_null(strstr(line_other, needle));

    g_assert_nonnull(strstr(line_other, bridge));

    g_free(needle);
}

static gboolean _plan_contains(GPtrArray *cmds, const gchar *needle) {
    for (guint i = 0; i < cmds->len; i++)
        if (strstr((const gchar *)g_ptr_array_index(cmds, i), needle))
            return TRUE;
    return FALSE;
}

static void test_host_fw_plan_ufw_add(void) {
    GPtrArray *cmds = pcv_host_fw_plan(PCV_HOST_FW_UFW, "br0", FALSE);

    g_assert_cmpuint(cmds->len, ==, 3);
    g_assert_true(_plan_contains(cmds, "ufw route allow in on br0"));
    g_assert_true(_plan_contains(cmds, "ufw route allow out on br0"));
    g_assert_true(_plan_contains(cmds, "ufw allow in on br0"));

    g_assert_false(_plan_contains(cmds, "delete"));
    g_ptr_array_unref(cmds);
}

static void test_host_fw_plan_ufw_remove(void) {
    GPtrArray *cmds = pcv_host_fw_plan(PCV_HOST_FW_UFW, "br0", TRUE);
    g_assert_cmpuint(cmds->len, ==, 3);

    g_assert_true(_plan_contains(cmds, "ufw --force delete route allow in on br0"));
    g_assert_true(_plan_contains(cmds, "ufw --force delete route allow out on br0"));
    g_assert_true(_plan_contains(cmds, "ufw --force delete allow in on br0"));
    g_ptr_array_unref(cmds);
}

static void test_host_fw_plan_iptables_add(void) {
    GPtrArray *cmds = pcv_host_fw_plan(PCV_HOST_FW_IPTABLES_DROP, "br0", FALSE);

    g_assert_cmpuint(cmds->len, ==, 5);
    g_assert_true(_plan_contains(cmds, "iptables -I FORWARD -i br0 -j ACCEPT"));

    g_assert_true(_plan_contains(cmds, "conntrack"));
    g_assert_true(_plan_contains(cmds, "--ctstate RELATED,ESTABLISHED"));

    g_assert_true(_plan_contains(cmds, "INPUT -i br0 -p udp --dport 67"));
    g_assert_true(_plan_contains(cmds, "INPUT -i br0 -p udp --dport 53"));
    g_assert_true(_plan_contains(cmds, "INPUT -i br0 -p tcp --dport 53"));

    g_assert_false(_plan_contains(cmds, "-C "));
    g_ptr_array_unref(cmds);
}

static void test_host_fw_plan_iptables_remove(void) {
    GPtrArray *cmds = pcv_host_fw_plan(PCV_HOST_FW_IPTABLES_DROP, "br0", TRUE);
    g_assert_cmpuint(cmds->len, ==, 5);

    g_assert_true(_plan_contains(cmds, "iptables -D FORWARD -i br0 -j ACCEPT"));
    g_assert_false(_plan_contains(cmds, "-I "));
    g_ptr_array_unref(cmds);
}

static void test_host_fw_plan_open_empty(void) {

    GPtrArray *add = pcv_host_fw_plan(PCV_HOST_FW_OPEN, "br0", FALSE);
    GPtrArray *rem = pcv_host_fw_plan(PCV_HOST_FW_OPEN, "br0", TRUE);
    g_assert_cmpuint(add->len, ==, 0);
    g_assert_cmpuint(rem->len, ==, 0);
    g_ptr_array_unref(add);
    g_ptr_array_unref(rem);
}

static void test_host_fw_plan_firewalld_empty(void) {

    GPtrArray *cmds = pcv_host_fw_plan(PCV_HOST_FW_FIREWALLD, "br0", FALSE);
    g_assert_cmpuint(cmds->len, ==, 0);
    g_ptr_array_unref(cmds);
}

static void _net2_write_mock(const gchar *dir, const gchar *name, const gchar *body) {
    gchar *path = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(path, body, -1, NULL));
    g_assert_cmpint(g_chmod(path, 0755), ==, 0);
    g_free(path);
}

static gboolean _net2_run_isolated(int nft_exit, GError **err) {

    pcv_spawn_launcher_shutdown();

    gchar *dir = g_dir_make_tmp("pcvnet2-XXXXXX", NULL);
    g_assert_nonnull(dir);
    gchar *nft_body = g_strdup_printf("#!/bin/sh\nexit %d\n", nft_exit);
    _net2_write_mock(dir, "nft", nft_body);
    _net2_write_mock(dir, "sysctl", "#!/bin/sh\nexit 0\n");
    g_free(nft_body);

    gchar *saved = g_strdup(g_getenv("PATH"));
    gchar *newp  = g_strdup_printf("%s:%s", dir, saved ? saved : "");
    g_setenv("PATH", newp, TRUE);

    gboolean ok = network_firewall_setup_isolated("pcvbrtest0", "10.9.9.1/24", err);

    if (saved) g_setenv("PATH", saved, TRUE); else g_unsetenv("PATH");

    gchar *p_nft = g_build_filename(dir, "nft", NULL);
    gchar *p_sc  = g_build_filename(dir, "sysctl", NULL);
    g_remove(p_nft); g_remove(p_sc); g_rmdir(dir);
    g_free(p_nft); g_free(p_sc);
    g_free(saved); g_free(newp); g_free(dir);
    return ok;
}

static void test_firewall_isolated_nft_failure_propagates(void) {
    GError *err = NULL;
    gboolean ok = _net2_run_isolated(1, &err);
    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
}

static void test_firewall_isolated_nft_success_returns_true(void) {
    GError *err = NULL;
    gboolean ok = _net2_run_isolated(0, &err);
    g_assert_true(ok);
    g_assert_no_error(err);
}

static void test_firewall_isolated_drop_rule_failure_propagates(void) {
    pcv_spawn_launcher_shutdown();

    gchar *dir = g_dir_make_tmp("pcvnet2d-XXXXXX", NULL);
    g_assert_nonnull(dir);
    _net2_write_mock(dir, "nft",
        "#!/bin/sh\nfor a in \"$@\"; do [ \"$a\" = \"drop\" ] && exit 1; done\nexit 0\n");
    _net2_write_mock(dir, "sysctl", "#!/bin/sh\nexit 0\n");

    gchar *saved = g_strdup(g_getenv("PATH"));
    gchar *newp  = g_strdup_printf("%s:%s", dir, saved ? saved : "");
    g_setenv("PATH", newp, TRUE);

    GError *err = NULL;
    gboolean ok = network_firewall_setup_isolated("pcvbrtest0", "10.9.9.1/24", &err);

    if (saved) g_setenv("PATH", saved, TRUE); else g_unsetenv("PATH");

    gchar *p_nft = g_build_filename(dir, "nft", NULL);
    gchar *p_sc  = g_build_filename(dir, "sysctl", NULL);
    g_remove(p_nft); g_remove(p_sc); g_rmdir(dir);
    g_free(p_nft); g_free(p_sc);
    g_free(saved); g_free(newp); g_free(dir);

    g_assert_false(ok);
    g_assert_nonnull(err);
    g_clear_error(&err);
}

void test_network_register(void) {
    g_test_add_func("/network/bridge_name/valid",    test_bridge_name_valid);
    g_test_add_func("/network/bridge_name/invalid",  test_bridge_name_invalid);
    g_test_add_func("/network/bridge_name/boundary", test_bridge_name_boundary);
    g_test_add_func("/network/mode_strings",         test_network_mode_strings);

    g_test_add_func("/network/v4/ipv6_prefix/valid",      test_ipv6_prefix_valid);
    g_test_add_func("/network/v4/ipv6_prefix/injection",  test_ipv6_prefix_injection);
    g_test_add_func("/network/v8/mode_set_cidr",          test_mode_set_cidr_validation);
    g_test_add_func("/network/v10/bridge_pkill_traversal",test_bridge_name_pkill_traversal);
    g_test_add_func("/network/v11/iface_ip_vm",           test_v11_iface_ip_vm_validation);
    g_test_add_func("/network/v15/teardown_token_match",  test_firewall_teardown_token_match);

    g_test_add_func("/network/host_fw_plan_ufw_add",        test_host_fw_plan_ufw_add);
    g_test_add_func("/network/host_fw_plan_ufw_remove",     test_host_fw_plan_ufw_remove);
    g_test_add_func("/network/host_fw_plan_iptables_add",   test_host_fw_plan_iptables_add);
    g_test_add_func("/network/host_fw_plan_iptables_remove",test_host_fw_plan_iptables_remove);
    g_test_add_func("/network/host_fw_plan_open_empty",     test_host_fw_plan_open_empty);
    g_test_add_func("/network/host_fw_plan_firewalld_empty",test_host_fw_plan_firewalld_empty);

    g_test_add_func("/network/net2/isolated_nft_failure_propagates",
                    test_firewall_isolated_nft_failure_propagates);
    g_test_add_func("/network/net2/isolated_nft_success_returns_true",
                    test_firewall_isolated_nft_success_returns_true);
    g_test_add_func("/network/net2/drop_rule_failure_propagates",
                    test_firewall_isolated_drop_rule_failure_propagates);
}
