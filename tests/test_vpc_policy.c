                                                                                               
                                                                                               
                                                             
                                                             
                      
   
                          
                                                        
   
#include <glib.h>

#include "modules/network/vpc/vpc_policy_nft.h"

static PcvVpcPolicySnapshot *
sample_snapshot(void)
{
    PcvVpcPolicySnapshot *snapshot = pcv_vpc_policy_snapshot_new();
    PcvVpcPolicyVpc *nat = pcv_vpc_policy_vpc_new("vpc-a", "nat");
    PcvVpcPolicySubnet *web = pcv_vpc_policy_subnet_new(
        "subnet-web", "pcvsweb", "10.20.0.0/24", "10.20.0.1");
    PcvVpcPolicySubnet *db = pcv_vpc_policy_subnet_new(
        "subnet-db", "pcvsdb", "10.20.1.0/24", "10.20.1.1");
    g_ptr_array_add(web->attachments,
                    pcv_vpc_policy_attachment_new("10.20.0.10", "02:00:00:00:00:10"));
    g_ptr_array_add(nat->subnets, web);
    g_ptr_array_add(nat->subnets, db);
    g_ptr_array_add(snapshot->vpcs, nat);

    PcvVpcPolicyVpc *isolated = pcv_vpc_policy_vpc_new("vpc-b", "isolated");
    g_ptr_array_add(isolated->subnets, pcv_vpc_policy_subnet_new(
        "subnet-private", "pcvsprivate", "10.30.0.0/24", "10.30.0.1"));
    g_ptr_array_add(snapshot->vpcs, isolated);

    PcvVpcPolicyPublish *publish = pcv_vpc_policy_publish_new(
        "tcp", "192.0.2.10", 8443, "10.20.0.10", 443, "pcvsweb");
    g_ptr_array_add(publish->allowed_sources, g_strdup("198.51.100.0/24"));
    g_ptr_array_add(snapshot->publishes, publish);
    return snapshot;
}

static void
test_vpc_policy_cross_drop_precedes_nat_allow(void)
{
    g_autoptr(PcvVpcPolicySnapshot) snapshot = sample_snapshot();
    g_autofree gchar *script = pcv_vpc_policy_build_script(snapshot);
    const gchar *same = strstr(script,
        "iifname \"pcvsweb\" oifname \"pcvsdb\" accept");
    const gchar *cross = strstr(script,
        "iifname @managed_ifaces oifname @managed_ifaces drop");
    const gchar *egress = strstr(script, "iifname \"pcvsweb\" accept");
    g_assert_nonnull(same); g_assert_nonnull(cross); g_assert_nonnull(egress);
    g_assert_true(same < cross);
    g_assert_true(cross < egress);
    g_assert_nonnull(strstr(script,
        "iifname \"pcvsprivate\" drop"));
}

static void
test_vpc_policy_source_guard_is_pair_bound(void)
{
    g_autoptr(PcvVpcPolicySnapshot) snapshot = sample_snapshot();
    g_autofree gchar *script = pcv_vpc_policy_build_script(snapshot);
    g_assert_nonnull(strstr(script,
        "ibrname \"pcvsweb\" ether saddr 02:00:00:00:00:10 ip saddr 10.20.0.10 accept"));
    g_assert_nonnull(strstr(script,
        "ibrname \"pcvsweb\" ether saddr 02:00:00:00:00:10 ether type arp "
        "arp saddr ether 02:00:00:00:00:10 arp saddr ip 10.20.0.10 accept"));
    g_assert_nonnull(strstr(script,
        "ibrname \"pcvsweb\" ether type ip drop"));
    g_assert_nonnull(strstr(script,
        "ibrname \"pcvsweb\" ether type arp drop"));
    g_assert_nonnull(strstr(script,
        "ibrname \"pcvsweb\" ether type ip6 drop"));
}

static void
test_vpc_policy_publish_has_source_dnat_and_forward(void)
{
    g_autoptr(PcvVpcPolicySnapshot) snapshot = sample_snapshot();
    g_autofree gchar *script = pcv_vpc_policy_build_script(snapshot);
    g_assert_nonnull(strstr(script,
        "ip daddr 192.0.2.10 ip saddr 198.51.100.0/24 tcp dport 8443 "
        "dnat ip to 10.20.0.10:443"));
    g_assert_nonnull(strstr(script,
        "oifname \"pcvsweb\" ip saddr 198.51.100.0/24 ip daddr 10.20.0.10 "
        "tcp dport 443 ct status dnat accept"));
}

static void
test_vpc_policy_nat_rejects_unsolicited_inbound(void)
{
    g_autoptr(PcvVpcPolicySnapshot) snapshot = sample_snapshot();
    g_autofree gchar *script = pcv_vpc_policy_build_script(snapshot);
    const gchar *published = strstr(script,
        "oifname \"pcvsweb\" ip saddr 198.51.100.0/24");
    const gchar *established = strstr(script,
        "oifname \"pcvsweb\" ct state established,related accept");
    const gchar *drop = strstr(script,
        "oifname \"pcvsweb\" drop");
    g_assert_nonnull(published);
    g_assert_nonnull(established);
    g_assert_nonnull(drop);
    g_assert_true(published < drop);
    g_assert_true(established < drop);
}

static void
test_vpc_policy_ovn_uses_host_edge_without_linux_bridge_guard(void)
{
    g_autoptr(PcvVpcPolicySnapshot) snapshot = pcv_vpc_policy_snapshot_new();
    PcvVpcPolicyVpc *vpc = pcv_vpc_policy_vpc_new("vpc-ovn", "nat");
    g_free(vpc->backend);
    vpc->backend = g_strdup("ovn");
    vpc->edge_interface = g_strdup("pcve12345678901");
    PcvVpcPolicySubnet *subnet = pcv_vpc_policy_subnet_new(
        "subnet-ovn", NULL, "10.90.0.0/24", "10.90.0.1");
    g_free(subnet->backend);
    subnet->backend = g_strdup("ovn");
    g_ptr_array_add(vpc->subnets, subnet);
    g_ptr_array_add(snapshot->vpcs, vpc);

    g_autofree gchar *script = pcv_vpc_policy_build_script(snapshot);
    g_assert_nonnull(strstr(script,
        "add element inet pcv_vpc managed_ifaces { \"pcve12345678901\" }"));
    g_assert_nonnull(strstr(script,
        "add rule inet pcv_vpc input iifname \"pcve12345678901\" drop"));
    g_assert_nonnull(strstr(script,
        "iifname \"pcve12345678901\" accept"));
    g_assert_nonnull(strstr(script,
        "ip saddr 10.90.0.0/24 oifname != @managed_ifaces masquerade"));
    g_assert_null(strstr(script, "ibrname \"pcve12345678901\""));
    g_assert_null(strstr(script, "iifname \"(null)\""));
}

static void
test_vpc_quarantine_drops_both_directions(void)
{
    g_autoptr(GPtrArray) bridges = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(bridges, g_strdup("pcvsdead"));
    g_autofree gchar *script = pcv_vpc_policy_build_quarantine_script(bridges);
    g_assert_nonnull(strstr(script, "input iifname \"pcvsdead\" drop"));
    g_assert_nonnull(strstr(script, "forward iifname \"pcvsdead\" drop"));
    g_assert_nonnull(strstr(script, "forward oifname \"pcvsdead\" drop"));
}

                                                                    
                                         
static void
test_vpc_policy_uses_legacy_compatible_table_replace(void)
{
    g_autoptr(PcvVpcPolicySnapshot) snapshot = sample_snapshot();
    g_autofree gchar *full = pcv_vpc_policy_build_script(snapshot);
    g_assert_null(strstr(full, "destroy table"));
    const gchar *first_add = strstr(full, "add table inet pcv_vpc\n");
    const gchar *delete = first_add
        ? strstr(first_add, "delete table inet pcv_vpc\n") : NULL;
    const gchar *replacement_add = delete
        ? strstr(delete + 1, "add table inet pcv_vpc\n") : NULL;
    g_assert_nonnull(first_add);
    g_assert_nonnull(delete);
    g_assert_nonnull(replacement_add);
    g_assert_true(first_add < delete && delete < replacement_add);

    g_autoptr(GPtrArray) bridges = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(bridges, g_strdup("pcvsdead"));
    g_autofree gchar *quarantine = pcv_vpc_policy_build_quarantine_script(bridges);
    g_assert_null(strstr(quarantine, "destroy table"));
    g_assert_true(g_str_has_prefix(quarantine,
        "add table inet pcv_vpc_quarantine\n"
        "delete table inet pcv_vpc_quarantine\n"
        "add table inet pcv_vpc_quarantine\n"));
}

void
test_vpc_policy_register(void)
{
    g_test_add_func("/vpc/policy/cross_drop_order", test_vpc_policy_cross_drop_precedes_nat_allow);
    g_test_add_func("/vpc/policy/source_guard", test_vpc_policy_source_guard_is_pair_bound);
    g_test_add_func("/vpc/policy/service_publish", test_vpc_policy_publish_has_source_dnat_and_forward);
    g_test_add_func("/vpc/policy/nat_inbound_default_drop", test_vpc_policy_nat_rejects_unsolicited_inbound);
    g_test_add_func("/vpc/policy/ovn_host_edge",
                    test_vpc_policy_ovn_uses_host_edge_without_linux_bridge_guard);
    g_test_add_func("/vpc/policy/quarantine", test_vpc_quarantine_drops_both_directions);
    g_test_add_func("/vpc/policy/legacy_table_replace",
                    test_vpc_policy_uses_legacy_compatible_table_replace);
}
