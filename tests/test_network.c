                                                                                       
                                                                                       
                                                      
                                                         
                                
                       
  
                                                  
  
                 
                                                                
                                             
                                         
  
                                     
  
                                      
   

#include <glib.h>
#include <glib/gstdio.h>                                                    
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "purecvisor/pcv_validate.h"
#include "modules/network/network_firewall_host.h"
#include "modules/network/network_firewall.h"                                     
#include "modules/network/network_manager.h"                     
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
    g_assert_true(pcv_validate_private_cidr("192.168.0.1/24"));
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

                                                     
    g_assert_true(pcv_validate_ip_literal("192.168.1.1"));
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

                                                           

   
                           
                                                           
                                                
   
static void test_n8_bridge_mtu_read(void) {
    gchar *base = g_dir_make_tmp("pcv_mtu_XXXXXX", NULL);
    g_assert_nonnull(base);
    gchar *brdir = g_build_filename(base, "pcvbr0", NULL);
    g_assert_cmpint(g_mkdir_with_parents(brdir, 0700), ==, 0);
    gchar *mtu_path = g_build_filename(brdir, "mtu", NULL);

                     
    g_assert_true(g_file_set_contents(mtu_path, "9000\n", -1, NULL));
    g_assert_cmpint(pcv_bridge_mtu_read("pcvbr0", base), ==, 9000);

                                                    
    g_assert_true(g_file_set_contents(mtu_path, "1500\n", -1, NULL));
    g_assert_cmpint(pcv_bridge_mtu_read("pcvbr0", base), ==, 1500);

                               
    g_assert_true(g_file_set_contents(mtu_path, "67\n", -1, NULL));
    g_assert_cmpint(pcv_bridge_mtu_read("pcvbr0", base), ==, 0);
    g_assert_true(g_file_set_contents(mtu_path, "9217\n", -1, NULL));
    g_assert_cmpint(pcv_bridge_mtu_read("pcvbr0", base), ==, 0);

                  
    g_assert_true(g_file_set_contents(mtu_path, "garbage\n", -1, NULL));
    g_assert_cmpint(pcv_bridge_mtu_read("pcvbr0", base), ==, 0);

                       
    g_assert_cmpint(pcv_bridge_mtu_read("nosuchbr", base), ==, 0);

                                              
    g_assert_cmpint(pcv_bridge_mtu_read(NULL, base), ==, 0);
    g_assert_cmpint(pcv_bridge_mtu_read("", base), ==, 0);
    g_assert_cmpint(pcv_bridge_mtu_read("../etc", base), ==, 0);

    g_unlink(mtu_path); g_rmdir(brdir); g_rmdir(base);
    g_free(mtu_path); g_free(brdir); g_free(base);
}

                                                                        

static void _pb_write(const gchar *path, const gchar *value)
{
    g_assert_true(g_file_set_contents(path, value, -1, NULL));
}

static void _pb_fixture_create(const gchar *sysroot,
                               const gchar *procroot,
                               const gchar *iface)
{
    gchar *ifdir = g_build_filename(sysroot, iface, NULL);
    gchar *device = g_build_filename(ifdir, "device", NULL);
    gchar *netdir = g_build_filename(procroot, "net", NULL);
    g_assert_cmpint(g_mkdir_with_parents(device, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(netdir, 0700), ==, 0);
    gchar *type = g_build_filename(ifdir, "type", NULL);
    gchar *flags = g_build_filename(ifdir, "flags", NULL);
    gchar *mtu = g_build_filename(ifdir, "mtu", NULL);
    gchar *address = g_build_filename(ifdir, "address", NULL);
    gchar *route4 = g_build_filename(netdir, "route", NULL);
    gchar *route6 = g_build_filename(netdir, "ipv6_route", NULL);
    _pb_write(type, "1\n");
    _pb_write(flags, "0x1\n");
    _pb_write(mtu, "1500\n");
    _pb_write(address, "02:44:00:00:00:01\n");
    _pb_write(route4, "Iface\tDestination\tGateway\tFlags\n");
    _pb_write(route6, "");
    g_free(route6); g_free(route4); g_free(address); g_free(mtu); g_free(flags); g_free(type);
    g_free(netdir); g_free(device); g_free(ifdir);
}

static void _pb_remove_tree(const gchar *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const gchar *name = NULL;
        while ((name = g_dir_read_name(dir)) != NULL) {
            gchar *child = g_build_filename(path, name, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR)) _pb_remove_tree(child);
            else g_remove(child);
            g_free(child);
        }
        g_dir_close(dir);
    }
    g_rmdir(path);
}

static void test_physical_preflight_safe_and_default_route(void)
{
    gchar *base = g_dir_make_tmp("pcv-physical-XXXXXX", NULL);
    gchar *sysroot = g_build_filename(base, "sys", NULL);
    gchar *procroot = g_build_filename(base, "proc", NULL);
    _pb_fixture_create(sysroot, procroot, "enptest0");

    GError *error = NULL;
    gboolean was_up = FALSE;
    g_assert_true(pcv_network_iface_preflight_dedicated(
        "enptest0", sysroot, procroot, &was_up, &error));
    g_assert_no_error(error);
    g_assert_true(was_up);

    gchar *route = g_build_filename(procroot, "net", "route", NULL);
    _pb_write(route,
        "Iface\tDestination\tGateway\tFlags\n"
        "enptest0\t00000000\t0100000A\t0003\n");
    g_assert_false(pcv_network_iface_preflight_dedicated(
        "enptest0", sysroot, procroot, &was_up, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "default route"));
    g_clear_error(&error);

    g_free(route);
    _pb_remove_tree(base);
    g_free(procroot); g_free(sysroot); g_free(base);
}

static void test_physical_preflight_rejects_master_and_uncertain(void)
{
    gchar *base = g_dir_make_tmp("pcv-physical-XXXXXX", NULL);
    gchar *sysroot = g_build_filename(base, "sys", NULL);
    gchar *procroot = g_build_filename(base, "proc", NULL);
    _pb_fixture_create(sysroot, procroot, "enptest0");
    gchar *master = g_build_filename(sysroot, "enptest0", "master", NULL);
    g_assert_cmpint(symlink("../../pcvbr9", master), ==, 0);

    GError *error = NULL;
    g_assert_false(pcv_network_iface_preflight_dedicated(
        "enptest0", sysroot, procroot, NULL, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "master"));
    g_clear_error(&error);
    g_remove(master);

    gchar *route6 = g_build_filename(procroot, "net", "ipv6_route", NULL);
    g_remove(route6);                                
    g_assert_false(pcv_network_iface_preflight_dedicated(
        "enptest0", sysroot, procroot, NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);

    g_free(route6); g_free(master);
    _pb_remove_tree(base);
    g_free(procroot); g_free(sysroot); g_free(base);
}

static void test_physical_preflight_rejects_wireless_ipv6_and_loopback(void)
{
    gchar *base = g_dir_make_tmp("pcv-physical-XXXXXX", NULL);
    gchar *sysroot = g_build_filename(base, "sys", NULL);
    gchar *procroot = g_build_filename(base, "proc", NULL);
    _pb_fixture_create(sysroot, procroot, "enptest0");
    gchar *wireless = g_build_filename(sysroot, "enptest0", "wireless", NULL);
    g_assert_cmpint(g_mkdir(wireless, 0700), ==, 0);

    GError *error = NULL;
    g_assert_false(pcv_network_iface_preflight_dedicated(
        "enptest0", sysroot, procroot, NULL, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "wireless"));
    g_clear_error(&error);
    g_rmdir(wireless);

    gchar *bonding = g_build_filename(sysroot, "enptest0", "bonding", NULL);
    g_assert_cmpint(g_mkdir(bonding, 0700), ==, 0);
    g_assert_false(pcv_network_iface_preflight_dedicated(
        "enptest0", sysroot, procroot, NULL, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "bonded"));
    g_clear_error(&error);
    g_rmdir(bonding);

    gchar *vlan_dir = g_build_filename(procroot, "net", "vlan", NULL);
    g_assert_cmpint(g_mkdir_with_parents(vlan_dir, 0700), ==, 0);
    gchar *vlan = g_build_filename(vlan_dir, "enptest0", NULL);
    _pb_write(vlan, "enptest0 VID: 100\n");
    g_assert_false(pcv_network_iface_preflight_dedicated(
        "enptest0", sysroot, procroot, NULL, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "VLAN"));
    g_clear_error(&error);
    g_remove(vlan);

    gchar *route6 = g_build_filename(procroot, "net", "ipv6_route", NULL);
    _pb_write(route6,
        "00000000000000000000000000000000 00 "
        "00000000000000000000000000000000 00 "
        "00000000000000000000000000000000 00000000 00000000 00000000 "
        "00000000 enptest0\n");
    g_assert_false(pcv_network_iface_preflight_dedicated(
        "enptest0", sysroot, procroot, NULL, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "IPv6 default route"));
    g_clear_error(&error);

    g_assert_false(pcv_network_iface_preflight_dedicated(
        "lo", sysroot, procroot, NULL, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "non-loopback"));
    g_clear_error(&error);

    g_free(vlan); g_free(vlan_dir); g_free(bonding); g_free(route6); g_free(wireless);
    _pb_remove_tree(base);
    g_free(procroot); g_free(sysroot); g_free(base);
}

static void test_physical_address_facts_fail_closed(void)
{
    GError *error = NULL;
    g_assert_true(pcv_network_iface_address_facts_dedicated(
        "enptest0", TRUE, FALSE, FALSE, &error));
    g_assert_no_error(error);

    g_assert_false(pcv_network_iface_address_facts_dedicated(
        "enptest0", TRUE, TRUE, FALSE, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "IPv4"));
    g_clear_error(&error);

    g_assert_false(pcv_network_iface_address_facts_dedicated(
        "enptest0", TRUE, FALSE, TRUE, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "IPv6"));
    g_clear_error(&error);

    g_assert_false(pcv_network_iface_address_facts_dedicated(
        "enptest0", FALSE, FALSE, FALSE, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "cannot inspect"));
    g_clear_error(&error);
}

static void test_shared_preflight_and_address_facts(void)
{
    gchar *base = g_dir_make_tmp("pcv-shared-preflight-XXXXXX", NULL);
    gchar *sysroot = g_build_filename(base, "sys", NULL);
    gchar *procroot = g_build_filename(base, "proc", NULL);
    _pb_fixture_create(sysroot, procroot, "enptest0");
    PcvSharedIfaceFacts facts = {0};
    GError *error = NULL;
    g_assert_true(pcv_network_iface_preflight_shared(
        "enptest0", sysroot, procroot, &facts, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(facts.mac, ==, "02:44:00:00:00:01");
    g_assert_cmpint(facts.mtu, ==, 1500);
    g_assert_true(facts.was_up);
    g_assert_false(facts.promisc_was_on);

    g_assert_true(pcv_network_iface_address_facts_shared(
        "enptest0", TRUE, TRUE, FALSE, &error));
    g_assert_no_error(error);
    g_assert_false(pcv_network_iface_address_facts_shared(
        "enptest0", TRUE, FALSE, FALSE, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(pcv_network_iface_address_facts_shared(
        "enptest0", FALSE, FALSE, FALSE, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);

    _pb_remove_tree(base);
    g_free(procroot); g_free(sysroot); g_free(base);
}

static void test_physical_state_atomic_file(void)
{
    gchar *base = g_dir_make_tmp("pcv-physical-state-XXXXXX", NULL);
    GError *error = NULL;
    g_assert_true(pcv_network_physical_state_save_at(
        base, "pcvbr0", "enptest0", 9000, TRUE, &error));
    g_assert_no_error(error);
    gchar *path = g_build_filename(base, "pcvbr0.json", NULL);
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(contents, "\"physical_if\":\"enptest0\""));
    g_assert_nonnull(strstr(contents, "\"schema_version\":2"));
    g_assert_nonnull(strstr(contents, "\"uplink_mode\":\"dedicated\""));
    g_assert_nonnull(strstr(contents, "\"mtu\":9000"));
    struct stat st = {0};
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    g_free(contents);
    g_remove(path); g_rmdir(base);
    g_free(path); g_free(base);
}

static gchar *_pb_mock_ip(const gchar *base, const gchar *log_path)
{
    gchar *bin = g_build_filename(base, "bin", NULL);
    g_assert_cmpint(g_mkdir_with_parents(bin, 0700), ==, 0);
    gchar *ip = g_build_filename(bin, "ip", NULL);
    gchar *quoted_log = g_shell_quote(log_path);
    gchar *body = g_strdup_printf(
        "#!/bin/sh\nprintf '%%s\\n' \"$*\" >> %s\nexit 0\n", quoted_log);
    _pb_write(ip, body);
    g_assert_cmpint(g_chmod(ip, 0755), ==, 0);
    g_free(body); g_free(quoted_log); g_free(ip);
    return bin;
}

static gchar *_pb_mock_ip_blocks_state_commit(const gchar *base,
                                               const gchar *log_path,
                                               const gchar *blocked_state)
{
    gchar *bin = g_build_filename(base, "bin", NULL);
    g_assert_cmpint(g_mkdir_with_parents(bin, 0700), ==, 0);
    gchar *ip = g_build_filename(bin, "ip", NULL);
    gchar *quoted_log = g_shell_quote(log_path);
    gchar *quoted_state = g_shell_quote(blocked_state);
    gchar *body = g_strdup_printf(
        "#!/bin/sh\nprintf '%%s\\n' \"$*\" >> %s\n"
        "if [ \"$*\" = 'link set dev enptest0 up' ]; then "
        "printf 'blocked\\n' > %s; fi\nexit 0\n",
        quoted_log, quoted_state);
    _pb_write(ip, body);
    g_assert_cmpint(g_chmod(ip, 0755), ==, 0);
    g_free(body); g_free(quoted_state); g_free(quoted_log); g_free(ip);
    return bin;
}

static gchar *_pb_mock_ip_fail_mtu(const gchar *base, const gchar *log_path)
{
    gchar *bin = g_build_filename(base, "bin", NULL);
    g_assert_cmpint(g_mkdir_with_parents(bin, 0700), ==, 0);
    gchar *ip = g_build_filename(bin, "ip", NULL);
    gchar *quoted_log = g_shell_quote(log_path);
    gchar *body = g_strdup_printf(
        "#!/bin/sh\nprintf '%%s\\n' \"$*\" >> %s\n"
        "case \"$*\" in\n"
        "  'link set dev pcvbr0 mtu 1500') exit 1 ;;\n"
        "  *) exit 0 ;;\n"
        "esac\n", quoted_log);
    _pb_write(ip, body);
    g_assert_cmpint(g_chmod(ip, 0755), ==, 0);
    g_free(body); g_free(quoted_log); g_free(ip);
    return bin;
}

static void test_bridge_create_rolls_back_synchronously(void)
{
    pcv_spawn_launcher_shutdown();
    gchar *base = g_dir_make_tmp("pcv-bridge-rollback-XXXXXX", NULL);
    gchar *log = g_build_filename(base, "ip.log", NULL);
    gchar *bin = _pb_mock_ip_fail_mtu(base, log);
    gchar *saved_path = g_strdup(g_getenv("PATH"));
    gchar *mock_path = g_strdup_printf("%s:%s", bin, saved_path ? saved_path : "");
    g_setenv("PATH", mock_path, TRUE);

    GError *error = NULL;
    g_assert_false(network_bridge_create("pcvbr0", NULL, 1500, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    gchar *commands = NULL;
    g_assert_true(g_file_get_contents(log, &commands, NULL, NULL));
    const gchar *created = strstr(commands, "link add name pcvbr0 type bridge");
    const gchar *failed = strstr(commands, "link set dev pcvbr0 mtu 1500");
    const gchar *deleted = strstr(commands, "link delete pcvbr0 type bridge");
    g_assert_nonnull(created);
    g_assert_nonnull(failed);
    g_assert_nonnull(deleted);
    g_assert_true(created < failed && failed < deleted);

    if (saved_path) g_setenv("PATH", saved_path, TRUE); else g_unsetenv("PATH");
    g_free(commands); g_free(mock_path); g_free(saved_path); g_free(bin);
    _pb_remove_tree(base);
    g_free(log); g_free(base);
}

static void test_generic_delete_refuses_unmanaged_host_uplink(void)
{
    pcv_spawn_launcher_shutdown();
    gchar *base = g_dir_make_tmp("pcv-bridge-delete-guard-XXXXXX", NULL);
    gchar *sysroot = g_build_filename(base, "sys", NULL);
    gchar *brif = g_build_filename(sysroot, "pcvbr0", "brif", NULL);
    g_assert_cmpint(g_mkdir_with_parents(brif, 0700), ==, 0);
    gchar *uplink_entry = g_build_filename(brif, "enptest0", NULL);
    _pb_write(uplink_entry, "");
    gchar *log = g_build_filename(base, "ip.log", NULL);
    gchar *bin = _pb_mock_ip(base, log);
    gchar *saved_path = g_strdup(g_getenv("PATH"));
    gchar *mock_path = g_strdup_printf("%s:%s", bin, saved_path ? saved_path : "");
    g_setenv("PATH", mock_path, TRUE);

    GError *error = NULL;
    gchar *detected = NULL;
    g_assert_true(pcv_network_bridge_has_host_uplink_at(
        "pcvbr0", sysroot, &detected, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(detected, ==, "enptest0");
    g_free(detected);

    g_assert_false(pcv_network_bridge_delete_at("pcvbr0", sysroot, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "unmanaged host uplink"));
    g_clear_error(&error);
    g_assert_false(g_file_test(log, G_FILE_TEST_EXISTS));

    g_remove(uplink_entry);
    gchar *guest_entry = g_build_filename(brif, "vnet7", NULL);
    _pb_write(guest_entry, "");
    g_assert_true(pcv_network_bridge_has_host_uplink_at(
        "pcvbr0", sysroot, &detected, &error));
    g_assert_no_error(error);
    g_assert_null(detected);

    if (saved_path) g_setenv("PATH", saved_path, TRUE); else g_unsetenv("PATH");
    g_free(guest_entry); g_free(mock_path); g_free(saved_path); g_free(bin);
    g_free(log); g_free(uplink_entry); g_free(brif); g_free(sysroot);
    _pb_remove_tree(base);
    g_free(base);
}

static void test_live_l3_mutation_guard_blocks_physical_state_and_uplink(void)
{
    gchar *base = g_dir_make_tmp("pcv-live-l3-guard-XXXXXX", NULL);
    gchar *sysroot = g_build_filename(base, "sys", NULL);
    gchar *state = g_build_filename(base, "state", NULL);
    gchar *bridge_kind = g_build_filename(sysroot, "pcvbr0", "bridge", NULL);
    gchar *brif = g_build_filename(sysroot, "pcvbr0", "brif", NULL);
    g_assert_cmpint(g_mkdir_with_parents(bridge_kind, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(brif, 0700), ==, 0);

    gchar *reason = NULL;
    g_assert_true(pcv_network_live_l3_mutation_allowed_at(
        "pcvbr0", state, sysroot, &reason));
    g_assert_null(reason);

    gchar *uplink = g_build_filename(brif, "enptest0", NULL);
    _pb_write(uplink, "");
    g_assert_false(pcv_network_live_l3_mutation_allowed_at(
        "pcvbr0", state, sysroot, &reason));
    g_assert_nonnull(reason);
    g_assert_nonnull(strstr(reason, "host uplink"));
    g_clear_pointer(&reason, g_free);
    g_remove(uplink);

    GError *error = NULL;
    g_assert_true(pcv_network_physical_state_save_at(
        state, "pcvbr0", "enptest0", 1500, TRUE, &error));
    g_assert_no_error(error);
    g_assert_false(pcv_network_live_l3_mutation_allowed_at(
        "pcvbr0", state, sysroot, &reason));
    g_assert_nonnull(reason);
    g_assert_nonnull(strstr(reason, "physical bridge"));
    g_free(reason);

    g_free(uplink); g_free(brif); g_free(bridge_kind);
    _pb_remove_tree(base);
    g_free(state); g_free(sysroot); g_free(base);
}

static void test_physical_transaction_commits_state(void)
{
    pcv_spawn_launcher_shutdown();
    gchar *base = g_dir_make_tmp("pcv-physical-txn-XXXXXX", NULL);
    gchar *sysroot = g_build_filename(base, "sys", NULL);
    gchar *procroot = g_build_filename(base, "proc", NULL);
    gchar *state = g_build_filename(base, "state", NULL);
    gchar *log = g_build_filename(base, "ip.log", NULL);
    _pb_fixture_create(sysroot, procroot, "enptest0");
    gchar *bin = _pb_mock_ip(base, log);
    gchar *saved_path = g_strdup(g_getenv("PATH"));
    gchar *mock_path = g_strdup_printf("%s:%s", bin, saved_path ? saved_path : "");
    g_setenv("PATH", mock_path, TRUE);

    GError *error = NULL;
    g_assert_true(pcv_network_physical_bridge_create_at(
        "pcvbr0", "enptest0", 1500, sysroot, procroot, state, &error));
    g_assert_no_error(error);
    gchar *record = g_build_filename(state, "pcvbr0.json", NULL);
    g_assert_true(g_file_test(record, G_FILE_TEST_IS_REGULAR));
    gchar *commands = NULL;
    g_assert_true(g_file_get_contents(log, &commands, NULL, NULL));
    g_assert_nonnull(strstr(commands, "link add name pcvbr0 type bridge"));
    g_assert_nonnull(strstr(commands, "link set enptest0 master pcvbr0"));
    g_assert_nonnull(strstr(commands, "link set dev enptest0 up"));
    g_assert_null(strstr(commands, "nomaster"));

                                                           
    gsize first_commands_len = strlen(commands);
    g_assert_false(pcv_network_physical_bridge_create_at(
        "pcvbr0", "enptest0", 1500, sysroot, procroot, state, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "already has desired state"));
    g_clear_error(&error);
    gchar *commands_after_retry = NULL;
    g_assert_true(g_file_get_contents(log, &commands_after_retry, NULL, NULL));
    g_assert_cmpuint(strlen(commands_after_retry), ==, first_commands_len);

    if (saved_path) g_setenv("PATH", saved_path, TRUE); else g_unsetenv("PATH");
    g_free(commands_after_retry); g_free(commands); g_free(record);
    g_free(mock_path); g_free(saved_path); g_free(bin);
    _pb_remove_tree(base);
    g_free(log); g_free(state); g_free(procroot); g_free(sysroot); g_free(base);
}

static void test_physical_transaction_rolls_back_state_failure(void)
{
    pcv_spawn_launcher_shutdown();
    gchar *base = g_dir_make_tmp("pcv-physical-txn-XXXXXX", NULL);
    gchar *sysroot = g_build_filename(base, "sys", NULL);
    gchar *procroot = g_build_filename(base, "proc", NULL);
    gchar *blocked_state = g_build_filename(base, "not-a-directory", NULL);
    gchar *log = g_build_filename(base, "ip.log", NULL);
    _pb_fixture_create(sysroot, procroot, "enptest0");
    gchar *bin = _pb_mock_ip_blocks_state_commit(base, log, blocked_state);
    gchar *saved_path = g_strdup(g_getenv("PATH"));
    gchar *mock_path = g_strdup_printf("%s:%s", bin, saved_path ? saved_path : "");
    g_setenv("PATH", mock_path, TRUE);

    GError *error = NULL;
    g_assert_false(pcv_network_physical_bridge_create_at(
        "pcvbr0", "enptest0", 1500, sysroot, procroot, blocked_state, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    gchar *commands = NULL;
    g_assert_true(g_file_get_contents(log, &commands, NULL, NULL));
    const gchar *bound = strstr(commands, "link set enptest0 master pcvbr0");
    const gchar *detached = strstr(commands, "link set enptest0 nomaster");
    const gchar *deleted = strstr(commands, "link delete pcvbr0 type bridge");
    g_assert_nonnull(bound);
    g_assert_nonnull(detached);
    g_assert_nonnull(deleted);
    g_assert_true(bound < detached && detached < deleted);

    if (saved_path) g_setenv("PATH", saved_path, TRUE); else g_unsetenv("PATH");
    g_free(commands); g_free(mock_path); g_free(saved_path); g_free(bin);
    _pb_remove_tree(base);
    g_free(log); g_free(blocked_state); g_free(procroot); g_free(sysroot); g_free(base);
}

static void test_physical_delete_restores_admin_and_removes_state(void)
{
    pcv_spawn_launcher_shutdown();
    gchar *base = g_dir_make_tmp("pcv-physical-delete-XXXXXX", NULL);
    gchar *sysroot = g_build_filename(base, "sys", NULL);
    gchar *procroot = g_build_filename(base, "proc", NULL);
    gchar *state = g_build_filename(base, "state", NULL);
    gchar *log = g_build_filename(base, "ip.log", NULL);
    _pb_fixture_create(sysroot, procroot, "enptest0");
    gchar *master = g_build_filename(sysroot, "enptest0", "master", NULL);
    g_assert_cmpint(symlink("../../pcvbr0", master), ==, 0);
    GError *error = NULL;
    g_assert_true(pcv_network_physical_state_save_at(
        state, "pcvbr0", "enptest0", 1500, FALSE, &error));
    g_assert_no_error(error);
    gchar *record = g_build_filename(state, "pcvbr0.json", NULL);
    gchar *bin = _pb_mock_ip(base, log);
    gchar *saved_path = g_strdup(g_getenv("PATH"));
    gchar *mock_path = g_strdup_printf("%s:%s", bin, saved_path ? saved_path : "");
    g_setenv("PATH", mock_path, TRUE);

    g_assert_true(pcv_network_physical_bridge_delete_at(
        "pcvbr0", sysroot, state, &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(record, G_FILE_TEST_EXISTS));
    gchar *commands = NULL;
    g_assert_true(g_file_get_contents(log, &commands, NULL, NULL));
    const gchar *detached = strstr(commands, "link set enptest0 nomaster");
    const gchar *restored = strstr(commands, "link set dev enptest0 down");
    const gchar *deleted = strstr(commands, "link delete pcvbr0 type bridge");
    g_assert_nonnull(detached);
    g_assert_nonnull(restored);
    g_assert_nonnull(deleted);
    g_assert_true(detached < restored && restored < deleted);

    if (saved_path) g_setenv("PATH", saved_path, TRUE); else g_unsetenv("PATH");
    g_free(commands); g_free(mock_path); g_free(saved_path); g_free(bin);
    g_free(record); g_free(master);
    _pb_remove_tree(base);
    g_free(log); g_free(state); g_free(procroot); g_free(sysroot); g_free(base);
}

static void test_physical_reconcile_idempotent_and_drift_closed(void)
{
    pcv_spawn_launcher_shutdown();
    gchar *base = g_dir_make_tmp("pcv-physical-reconcile-XXXXXX", NULL);
    gchar *sysroot = g_build_filename(base, "sys", NULL);
    gchar *procroot = g_build_filename(base, "proc", NULL);
    gchar *state = g_build_filename(base, "state", NULL);
    gchar *log = g_build_filename(base, "ip.log", NULL);
    _pb_fixture_create(sysroot, procroot, "enptest0");
    GError *error = NULL;
    g_assert_true(pcv_network_physical_state_save_at(
        state, "pcvbr0", "enptest0", 1500, TRUE, &error));
    g_assert_no_error(error);

    gchar *bridge_kind = g_build_filename(sysroot, "pcvbr0", "bridge", NULL);
    g_assert_cmpint(g_mkdir_with_parents(bridge_kind, 0700), ==, 0);
    gchar *master = g_build_filename(sysroot, "enptest0", "master", NULL);
    g_assert_cmpint(symlink("../../pcvbr0", master), ==, 0);
    gchar *bin = _pb_mock_ip(base, log);
    gchar *saved_path = g_strdup(g_getenv("PATH"));
    gchar *mock_path = g_strdup_printf("%s:%s", bin, saved_path ? saved_path : "");
    g_setenv("PATH", mock_path, TRUE);

    g_assert_true(pcv_network_reconcile_physical_bridges_at(
        state, sysroot, procroot, &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(log, G_FILE_TEST_EXISTS));                           

                                                                
                                                        
    gchar *route = g_build_filename(procroot, "net", "route", NULL);
    _pb_write(route,
        "Iface\tDestination\tGateway\tFlags\n"
        "enptest0\t00000000\t0100000A\t0003\n");
    g_assert_false(pcv_network_reconcile_physical_bridges_at(
        state, sysroot, procroot, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "default route"));
    g_clear_error(&error);
    g_assert_false(g_file_test(log, G_FILE_TEST_EXISTS));

                                                                
    g_remove(master);
    gchar *bridge_dir = g_build_filename(sysroot, "pcvbr0", NULL);
    _pb_remove_tree(bridge_dir);
    g_assert_false(pcv_network_reconcile_physical_bridges_at(
        state, sysroot, procroot, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(g_file_test(log, G_FILE_TEST_EXISTS));

    if (saved_path) g_setenv("PATH", saved_path, TRUE); else g_unsetenv("PATH");
    g_free(route); g_free(bridge_dir); g_free(mock_path); g_free(saved_path); g_free(bin);
    g_free(master); g_free(bridge_kind);
    _pb_remove_tree(base);
    g_free(log); g_free(state); g_free(procroot); g_free(sysroot); g_free(base);
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
                            
    g_test_add_func("/network/n8/bridge_mtu_read", test_n8_bridge_mtu_read);
    g_test_add_func("/network/physical/preflight_safe_default_route",
                    test_physical_preflight_safe_and_default_route);
    g_test_add_func("/network/physical/preflight_master_uncertain",
                    test_physical_preflight_rejects_master_and_uncertain);
    g_test_add_func("/network/physical/preflight_wireless_ipv6_loopback",
                    test_physical_preflight_rejects_wireless_ipv6_and_loopback);
    g_test_add_func("/network/physical/address_facts_fail_closed",
                    test_physical_address_facts_fail_closed);
    g_test_add_func("/network/shared/preflight_address_facts",
                    test_shared_preflight_and_address_facts);
    g_test_add_func("/network/physical/bridge_create_sync_rollback",
                    test_bridge_create_rolls_back_synchronously);
    g_test_add_func("/network/physical/generic_delete_refuses_host_uplink",
                    test_generic_delete_refuses_unmanaged_host_uplink);
    g_test_add_func("/network/physical/live_l3_mutation_guard",
                    test_live_l3_mutation_guard_blocks_physical_state_and_uplink);
    g_test_add_func("/network/physical/state_atomic_file",
                    test_physical_state_atomic_file);
    g_test_add_func("/network/physical/transaction_commits_state",
                    test_physical_transaction_commits_state);
    g_test_add_func("/network/physical/transaction_rolls_back_state_failure",
                    test_physical_transaction_rolls_back_state_failure);
    g_test_add_func("/network/physical/delete_restores_admin_removes_state",
                    test_physical_delete_restores_admin_and_removes_state);
    g_test_add_func("/network/physical/reconcile_idempotent_drift_closed",
                    test_physical_reconcile_idempotent_and_drift_closed);
}
