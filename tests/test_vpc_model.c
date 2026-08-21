                                                                                               
                                                                                         
                                               
                                                                    
                              
   
                         
                                             
   
#include <glib.h>
#include <string.h>

#include "modules/network/vpc/vpc_model.h"

static void
test_vpc_cidr_canonical_and_overlap(void)
{
    PcvVpcIpv4Cidr a = {0}, b = {0}, c = {0};
    g_autofree gchar *canonical = NULL;
    g_assert_true(pcv_vpc_subnet_cidr_parse("10.20.0.0/24", &a, &canonical, NULL));
    g_assert_cmpstr(canonical, ==, "10.20.0.0/24");
    g_assert_true(pcv_vpc_subnet_cidr_parse("10.20.0.128/25", &b, NULL, NULL));
    g_assert_true(pcv_vpc_subnet_cidr_parse("10.20.1.0/24", &c, NULL, NULL));
    g_assert_true(pcv_vpc_cidr_overlaps(&a, &b));
    g_assert_false(pcv_vpc_cidr_overlaps(&a, &c));
}

static void
test_vpc_subnet_rejects_host_address_and_prefix(void)
{
    PcvVpcIpv4Cidr cidr = {0};
    g_autoptr(GError) error = NULL;
    g_assert_false(pcv_vpc_subnet_cidr_parse("10.20.0.7/24", &cidr, NULL, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_vpc_subnet_cidr_parse("10.20.0.0/15", &cidr, NULL, &error));
    g_assert_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(pcv_vpc_subnet_cidr_parse("10.20.0.0/31", &cidr, NULL, &error));
}

static void
test_vpc_usable_range_and_contains(void)
{
    PcvVpcIpv4Cidr cidr = {0}; guint32 first = 0, last = 0;
    g_assert_true(pcv_vpc_subnet_cidr_parse("172.30.9.0/30", &cidr, NULL, NULL));
    g_assert_true(pcv_vpc_cidr_usable_range(&cidr, &first, &last));
    g_autofree gchar *first_text = pcv_vpc_ipv4_to_string(first);
    g_autofree gchar *last_text = pcv_vpc_ipv4_to_string(last);
    g_assert_cmpstr(first_text, ==, "172.30.9.1");
    g_assert_cmpstr(last_text, ==, "172.30.9.2");
    g_assert_true(pcv_vpc_cidr_contains_ip(&cidr, "172.30.9.2"));
    g_assert_false(pcv_vpc_cidr_contains_ip(&cidr, "172.30.9.4"));
}

static void
test_vpc_kernel_identifiers_are_deterministic(void)
{
    g_autofree gchar *bridge_a = pcv_vpc_bridge_name_from_id("subnet-alpha");
    g_autofree gchar *bridge_b = pcv_vpc_bridge_name_from_id("subnet-alpha");
    g_autofree gchar *mac_a = pcv_vpc_mac_from_id("attachment-alpha");
    g_autofree gchar *mac_b = pcv_vpc_mac_from_id("attachment-alpha");
    g_assert_cmpstr(bridge_a, ==, bridge_b);
    g_assert_cmpuint(strlen(bridge_a), <=, 15);
    g_assert_true(g_str_has_prefix(bridge_a, "pcvs"));
    g_assert_cmpstr(mac_a, ==, mac_b);
    g_assert_true(g_str_has_prefix(mac_a, "02:"));
}

static void
test_vpc_names_modes_and_ports(void)
{
    g_assert_true(pcv_vpc_name_is_valid("tenant-a.prod"));
    g_assert_false(pcv_vpc_name_is_valid("../tenant"));
    g_assert_false(pcv_vpc_name_is_valid("tenant a"));
    g_assert_true(pcv_vpc_egress_mode_is_valid("nat"));
    g_assert_true(pcv_vpc_egress_mode_is_valid("isolated"));
    g_assert_false(pcv_vpc_egress_mode_is_valid("routed"));
    g_assert_true(pcv_vpc_backend_is_valid("linux"));
    g_assert_true(pcv_vpc_backend_is_valid("ovn"));
    g_assert_false(pcv_vpc_backend_is_valid("ovs"));
    g_assert_true(pcv_vpc_protocol_is_valid("tcp"));
    g_assert_false(pcv_vpc_protocol_is_valid("icmp"));
    g_assert_true(pcv_vpc_port_is_valid(65535));
    g_assert_false(pcv_vpc_port_is_valid(0));
}

static void
test_vpc_ovn_identifiers_and_interface_xml(void)
{
    const gchar *attachment_id = "7d91398a-5dd7-4f59-93b9-f0fd92f806cf";
    g_autofree gchar *router = pcv_vpc_ovn_router_name_from_id("vpc-alpha");
    g_autofree gchar *sw = pcv_vpc_ovn_switch_name_from_id("subnet-alpha");
    g_autofree gchar *port = pcv_vpc_ovn_port_name_from_id(attachment_id);
    g_autofree gchar *legacy_port = pcv_vpc_ovn_port_name_from_id("attachment-alpha");
    g_autofree gchar *edge_iface = pcv_vpc_ovn_edge_iface_name_from_id("vpc-alpha");
    g_autofree gchar *xml = pcv_vpc_ovn_interface_xml(
        "02:aa:bb:cc:dd:ee", port, 1450);

    g_assert_true(g_str_has_prefix(router, "pcvv_lr_"));
    g_assert_true(g_str_has_prefix(sw, "pcvv_ls_"));
    g_assert_cmpstr(port, ==, attachment_id);
    g_assert_true(g_uuid_string_is_valid(legacy_port));
    g_assert_cmpuint(strlen(edge_iface), <=, 15);
    g_assert_nonnull(strstr(xml, "<source bridge='br-int'/>") );
    g_assert_nonnull(strstr(xml, "virtualport type='openvswitch'"));
    g_assert_nonnull(strstr(xml, port));
    g_assert_nonnull(strstr(xml, "<mtu size='1450'/>") );
}

static void
test_vpc_source_union_detects_full_public_access(void)
{
    g_autoptr(GPtrArray) sources = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(sources, g_strdup("0.0.0.0/1"));
    g_ptr_array_add(sources, g_strdup("128.0.0.0/1"));
    g_assert_true(pcv_vpc_sources_cover_all_ipv4(sources));

    g_ptr_array_set_size(sources, 0);
    g_ptr_array_add(sources, g_strdup("0.0.0.0/2"));
    g_ptr_array_add(sources, g_strdup("192.0.0.0/2"));
    g_assert_false(pcv_vpc_sources_cover_all_ipv4(sources));

    g_ptr_array_set_size(sources, 0);
    g_ptr_array_add(sources, g_strdup("0.0.0.0/0"));
    g_assert_true(pcv_vpc_sources_cover_all_ipv4(sources));
}

                                                                  
                                                          
static void
test_vpc_dnsmasq_conf_arg_is_single_equals_option(void)
{
    g_autofree gchar *arg = pcv_vpc_dnsmasq_conf_arg(
        "/run/purecvisor/network/dnsmasq-pcvs123.conf");
    g_assert_cmpstr(arg, ==,
        "--conf-file=/run/purecvisor/network/dnsmasq-pcvs123.conf");
    g_assert_null(strchr(arg, ' '));
}

                                                                      
                                                                
static void
test_vpc_libvirt_metadata_payload_has_no_self_namespace(void)
{
    const gchar *stored =
        "<pcv:vpc xmlns:pcv='urn:purecvisor:vpc' id='vpc-1'>"
        "<pcv:attachment id='att-1' subnet='subnet-1' ip='10.0.0.2'/>"
        "</pcv:vpc>";
    g_autoptr(GError) error = NULL;
    g_autofree gchar *payload = pcv_vpc_libvirt_metadata_payload(stored, &error);

    g_assert_no_error(error);
    g_assert_nonnull(payload);
    g_assert_null(strstr(payload, "xmlns"));
    g_assert_null(strstr(payload, "pcv:"));
    g_assert_nonnull(strstr(payload, "<vpc id=\"vpc-1\">"));
    g_assert_nonnull(strstr(payload,
        "<attachment id=\"att-1\" subnet=\"subnet-1\" ip=\"10.0.0.2\"/>"));
}

                                                                   
                                                         
static void
test_vpc_interface_xml_is_valid_for_attach_and_detach(void)
{
    g_autofree gchar *xml = pcv_vpc_interface_xml(
        "02:aa:bb:cc:dd:ee", "pcvs-test", 1500);

    g_assert_nonnull(strstr(xml, "<interface type='bridge'>"));
    g_assert_nonnull(strstr(xml, "<mac address='02:aa:bb:cc:dd:ee'/>"));
    g_assert_nonnull(strstr(xml, "<source bridge='pcvs-test'/>"));
    g_assert_nonnull(strstr(xml, "<model type='virtio'/>"));
    g_assert_nonnull(strstr(xml, "<driver name='vhost'/>"));
    g_assert_nonnull(strstr(xml, "<mtu size='1500'/>"));
}

void
test_vpc_model_register(void)
{
    g_test_add_func("/vpc/model/cidr_overlap", test_vpc_cidr_canonical_and_overlap);
    g_test_add_func("/vpc/model/subnet_reject", test_vpc_subnet_rejects_host_address_and_prefix);
    g_test_add_func("/vpc/model/usable_range", test_vpc_usable_range_and_contains);
    g_test_add_func("/vpc/model/kernel_identifiers", test_vpc_kernel_identifiers_are_deterministic);
    g_test_add_func("/vpc/model/ovn_identifiers", test_vpc_ovn_identifiers_and_interface_xml);
    g_test_add_func("/vpc/model/enums", test_vpc_names_modes_and_ports);
    g_test_add_func("/vpc/model/source_union", test_vpc_source_union_detects_full_public_access);
    g_test_add_func("/vpc/model/dnsmasq_conf_arg",
                    test_vpc_dnsmasq_conf_arg_is_single_equals_option);
    g_test_add_func("/vpc/model/libvirt_metadata_payload",
                    test_vpc_libvirt_metadata_payload_has_no_self_namespace);
    g_test_add_func("/vpc/model/interface_xml",
                    test_vpc_interface_xml_is_valid_for_attach_and_detach);
}
