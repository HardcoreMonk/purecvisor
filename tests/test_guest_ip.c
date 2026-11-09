   
                        
                                                     
  
                           
                                                                              
                                                                      
                                                                                        
                                                                                 
                                                                                         
                                    
  
                       
                                                            
                         
   
#include <glib.h>
#include <string.h>

#include "modules/dispatcher/guest_ip.h"

static const gchar *QGA_BASIC =
    "{\"return\":["
    "{\"name\":\"lo\",\"hardware-address\":\"00:00:00:00:00:00\","
     "\"ip-addresses\":[{\"ip-address\":\"127.0.0.1\",\"ip-address-type\":\"ipv4\",\"prefix\":8}]},"
    "{\"name\":\"eth0\",\"hardware-address\":\"52:54:00:AA:BB:CC\",\"ip-addresses\":["
     "{\"ip-address\":\"10.168.3.10\",\"ip-address-type\":\"ipv4\",\"prefix\":24},"
     "{\"ip-address\":\"10.0.0.5\",\"ip-address-type\":\"ipv4\",\"prefix\":8},"
     "{\"ip-address\":\"fe80::1\",\"ip-address-type\":\"ipv6\",\"prefix\":64}]}"
    "]}";

                                                             
static GPtrArray *
mk_ifaces(const gchar *mac, const gchar *v4, const gchar *v6)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(pcv_guest_iface_free);
    PcvGuestIface *f = g_new0(PcvGuestIface, 1);
    f->mac  = g_strdup(mac);
    f->ipv4 = g_ptr_array_new_with_free_func(g_free);
    f->ipv6 = g_ptr_array_new_with_free_func(g_free);
    if (v4)
        g_ptr_array_add(f->ipv4, g_strdup(v4));
    if (v6)
        g_ptr_array_add(f->ipv6, g_strdup(v6));
    g_ptr_array_add(a, f);
    return a;
}

static void
test_qga_parse_basic(void)
{
    GError *err = NULL;
    GPtrArray *a = pcv_guest_ip_parse_qga_interfaces(QGA_BASIC, &err);
    g_assert_no_error(err);
    g_assert_nonnull(a);
    g_assert_cmpuint(a->len, ==, 1);              
    PcvGuestIface *f = g_ptr_array_index(a, 0);
    g_assert_cmpstr(f->mac, ==, "52:54:00:aa:bb:cc");             
    g_assert_cmpuint(f->ipv4->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(f->ipv4, 0), ==, "10.168.3.10");
    g_assert_cmpuint(f->ipv6->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(f->ipv6, 0), ==, "fe80::1");
    g_ptr_array_unref(a);
}

static void
test_qga_parse_optional_members(void)
{
    GError *err = NULL;
    const gchar *json =
        "{\"return\":["
        "{\"name\":\"nomac\",\"ip-addresses\":[{\"ip-address\":\"1.2.3.4\",\"ip-address-type\":\"ipv4\"}]},"
        "{\"name\":\"noip\",\"hardware-address\":\"52:54:00:00:00:01\"}]}";
    GPtrArray *a = pcv_guest_ip_parse_qga_interfaces(json, &err);
    g_assert_no_error(err);
    g_assert_nonnull(a);
    g_assert_cmpuint(a->len, ==, 1);                                               
    PcvGuestIface *f = g_ptr_array_index(a, 0);
    g_assert_cmpuint(f->ipv4->len, ==, 0);
    g_assert_cmpuint(f->ipv6->len, ==, 0);
    g_ptr_array_unref(a);
}

static void
test_qga_parse_rejects_bad_json(void)
{
    const gchar *bad[] = {
        "{\"error\":{\"class\":\"CommandNotFound\",\"desc\":\"x\"}}",
        "[1,2,3]",
        "{\"return\":{\"a\":1}}",
        "not json at all",
        "",
    };
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        GError *err = NULL;
        GPtrArray *a = pcv_guest_ip_parse_qga_interfaces(bad[i], &err);
        g_assert_null(a);
        g_assert_nonnull(err);
        g_clear_error(&err);
    }
}

static void
test_qga_parse_skips_lo(void)
{
    GError *err = NULL;
    const gchar *json =
        "{\"return\":[{\"name\":\"lo\",\"hardware-address\":\"00:00:00:00:00:00\"}]}";
    GPtrArray *a = pcv_guest_ip_parse_qga_interfaces(json, &err);
    g_assert_no_error(err);
    g_assert_nonnull(a);
    g_assert_cmpuint(a->len, ==, 0);
    g_ptr_array_unref(a);
}

static void
test_resolve_order_lease_first(void)
{
    GPtrArray *lease = mk_ifaces("52:54:00:aa:bb:cc", "10.1.1.1", NULL);
    GPtrArray *agent = mk_ifaces("52:54:00:aa:bb:cc", "10.2.2.2", NULL);
    PcvGuestIpInputs in = {
        .lease_ifaces = lease, .agent_ifaces = agent,
        .lease_file_text = "0 52:54:00:aa:bb:cc 10.3.3.3 h *\n",
        .proc_net_arp_text = NULL, .arp_require_complete = TRUE,
    };
    const gchar *src = NULL;
    gchar *ip = pcv_guest_ip_resolve(&in, "52:54:00:aa:bb:cc", PCV_GUEST_IP_ANY_FIRST_IPV4, &src);
    g_assert_cmpstr(ip, ==, "10.1.1.1");
    g_assert_cmpstr(src, ==, "lease");
    g_free(ip);
    g_ptr_array_unref(lease);
    g_ptr_array_unref(agent);
}

static void
test_resolve_agent_before_lease_file(void)
{
    GPtrArray *agent = mk_ifaces("52:54:00:aa:bb:cc", "10.2.2.2", NULL);
    PcvGuestIpInputs in = {
        .agent_ifaces = agent,
        .lease_file_text = "0 52:54:00:aa:bb:cc 10.3.3.3 h *\n",
        .arp_require_complete = TRUE,
    };
    const gchar *src = NULL;
    gchar *ip = pcv_guest_ip_resolve(&in, "52:54:00:aa:bb:cc", PCV_GUEST_IP_ANY_FIRST_IPV4, &src);
    g_assert_cmpstr(ip, ==, "10.2.2.2");
    g_assert_cmpstr(src, ==, "guest-agent");
    g_free(ip);

                                              
    in.agent_ifaces = NULL;
    ip = pcv_guest_ip_resolve(&in, "52:54:00:aa:bb:cc", PCV_GUEST_IP_ANY_FIRST_IPV4, &src);
    g_assert_cmpstr(ip, ==, "10.3.3.3");
    g_assert_cmpstr(src, ==, "lease-file");
    g_free(ip);

    in.lease_file_text = NULL;
    in.proc_net_arp_text =
        "IP address HW type Flags HW address Mask Device\n"
        "10.4.4.4 0x1 0x2 52:54:00:aa:bb:cc * br0\n";
    ip = pcv_guest_ip_resolve(&in, "52:54:00:aa:bb:cc", PCV_GUEST_IP_ANY_FIRST_IPV4, &src);
    g_assert_cmpstr(ip, ==, "10.4.4.4");
    g_assert_cmpstr(src, ==, "arp");
    g_free(ip);

    in.proc_net_arp_text = NULL;
    ip = pcv_guest_ip_resolve(&in, "52:54:00:aa:bb:cc", PCV_GUEST_IP_ANY_FIRST_IPV4, &src);
    g_assert_null(ip);
    g_assert_cmpstr(src, ==, "");
    g_ptr_array_unref(agent);
}

static void
test_resolve_fleet_any_first_ipv4(void)
{
                                                                           
    GPtrArray *lease = mk_ifaces("52:54:00:00:00:99", "10.9.9.9", NULL);
    PcvGuestIpInputs in = { .lease_ifaces = lease, .arp_require_complete = TRUE };
    const gchar *src = NULL;
    gchar *ip = pcv_guest_ip_resolve(&in, "52:54:00:aa:bb:cc", PCV_GUEST_IP_ANY_FIRST_IPV4, &src);
    g_assert_cmpstr(ip, ==, "10.9.9.9");
    g_assert_cmpstr(src, ==, "lease");
    g_free(ip);
    g_ptr_array_unref(lease);

    GPtrArray *v6only = mk_ifaces("52:54:00:aa:bb:cc", NULL, "fd00::5");
    in.lease_ifaces = v6only;
    ip = pcv_guest_ip_resolve(&in, "52:54:00:aa:bb:cc", PCV_GUEST_IP_ANY_FIRST_IPV4, &src);
    g_assert_null(ip);
    g_assert_cmpstr(src, ==, "");
    g_ptr_array_unref(v6only);
}

static void
test_resolve_niclist_mac_ipv6_fallback(void)
{
                                                 
    GPtrArray *lease = mk_ifaces("52:54:00:aa:bb:cc", NULL, "fd00::5");
    PcvGuestIpInputs in = { .lease_ifaces = lease };
    const gchar *src = NULL;
    gchar *ip = pcv_guest_ip_resolve(&in, "52:54:00:AA:BB:CC", PCV_GUEST_IP_MAC_IPV4_THEN_ANY, &src);
    g_assert_cmpstr(ip, ==, "fd00::5");
    g_assert_cmpstr(src, ==, "lease");
    g_free(ip);

                            
    ip = pcv_guest_ip_resolve(&in, "52:54:00:00:00:01", PCV_GUEST_IP_MAC_IPV4_THEN_ANY, &src);
    g_assert_null(ip);
    g_assert_cmpstr(src, ==, "");
    g_ptr_array_unref(lease);

                               
    GPtrArray *both = mk_ifaces("52:54:00:aa:bb:cc", "10.0.0.7", "fd00::5");
    in.lease_ifaces = both;
    ip = pcv_guest_ip_resolve(&in, "52:54:00:aa:bb:cc", PCV_GUEST_IP_MAC_IPV4_THEN_ANY, &src);
    g_assert_cmpstr(ip, ==, "10.0.0.7");
    g_free(ip);
    g_ptr_array_unref(both);
}

static void
test_arp_complete_flag_rule(void)
{
    const gchar *arp =
        "IP address       HW type     Flags       HW address            Mask     Device\n"
        "10.168.3.77     0x1         0x0         52:54:00:00:0d:19     *        br0\n";
    gchar ip[64] = "keep";
    g_assert_false(pcv_guest_ip_arp_lookup(arp, "52:54:00:00:0d:19", TRUE, ip, sizeof(ip)));
    g_assert_cmpstr(ip, ==, "keep");                   
    g_assert_true(pcv_guest_ip_arp_lookup(arp, "52:54:00:00:0D:19", FALSE, ip, sizeof(ip)));
    g_assert_cmpstr(ip, ==, "10.168.3.77");
}

static void
test_lease_file_lookup(void)
{
    const gchar *lease =
        "1700000000 52:54:00:00:00:01 10.0.0.11 vm1 *\n"
        "\n"
        "1700000001 52:54:00:AA:BB:CC 10.0.0.12 vm2 *\n"
        "garbage\n";
    gchar *ip = pcv_guest_ip_lease_file_lookup(lease, "52:54:00:aa:bb:cc");
    g_assert_cmpstr(ip, ==, "10.0.0.12");
    g_free(ip);
    g_assert_null(pcv_guest_ip_lease_file_lookup(lease, "52:54:00:ff:ff:ff"));
    g_assert_null(pcv_guest_ip_lease_file_lookup(NULL, "52:54:00:aa:bb:cc"));
    g_assert_null(pcv_guest_ip_lease_file_lookup(lease, NULL));
}

static void
test_backoff_sequence(void)
{
    PcvGuestAgentSlot s = {0};
    const gint expect[] = {60, 120, 240, 480, 600, 600};
    gint64 now = 1000 * G_USEC_PER_SEC;
    for (guint i = 0; i < G_N_ELEMENTS(expect); i++) {
        s.inflight = TRUE;
        g_assert_cmpint(pcv_guest_agent_slot_on_failure(&s, now), ==, expect[i]);
        g_assert_cmpint(s.next_try_us, ==, now + (gint64)expect[i] * G_USEC_PER_SEC);
        g_assert_cmpint(s.state, ==, PCV_GUEST_AGENT_UNRESPONSIVE);
        g_assert_false(s.inflight);
    }
                       
    pcv_guest_agent_slot_on_success(&s, now);
    g_assert_cmpint(s.backoff_s, ==, 0);
    g_assert_cmpint(s.state, ==, PCV_GUEST_AGENT_OK);
    g_assert_cmpint(pcv_guest_agent_slot_on_failure(&s, now), ==, 60);
}

static void
test_backoff_caps_at_600(void)
{
    PcvGuestAgentSlot s = {0};
    gint last = 0;
    for (gint i = 0; i < 20; i++)
        last = pcv_guest_agent_slot_on_failure(&s, 0);
    g_assert_cmpint(last, ==, 600);
    g_assert_cmpint(s.backoff_s, ==, 600);
}

static void
test_cache_ttl_expires(void)
{
    PcvGuestAgentSlot s = {0};
    gint64 t0 = (gint64)5000 * G_USEC_PER_SEC;
    g_assert_false(pcv_guest_agent_slot_fresh(&s, t0));                     
    pcv_guest_agent_slot_on_success(&s, t0);
    g_assert_true(pcv_guest_agent_slot_fresh(&s, t0));
    g_assert_true(pcv_guest_agent_slot_fresh(&s, t0 + 60 * G_USEC_PER_SEC - 1));
    g_assert_false(pcv_guest_agent_slot_fresh(&s, t0 + 60 * G_USEC_PER_SEC));
    g_assert_false(pcv_guest_agent_slot_fresh(&s, t0 + 61 * G_USEC_PER_SEC));
}

static void
test_should_probe_respects_inflight_and_backoff(void)
{
    PcvGuestAgentSlot s = {0};
    gint64 now = 100 * G_USEC_PER_SEC;
    g_assert_true(pcv_guest_agent_slot_should_probe(&s, now));               
    s.inflight = TRUE;
    g_assert_false(pcv_guest_agent_slot_should_probe(&s, now));                    
    s.inflight = FALSE;

    pcv_guest_agent_slot_on_failure(&s, now);                                  
    g_assert_false(pcv_guest_agent_slot_should_probe(&s, now + 59 * G_USEC_PER_SEC));
    g_assert_true(pcv_guest_agent_slot_should_probe(&s, now + 60 * G_USEC_PER_SEC));

    pcv_guest_agent_slot_on_success(&s, now);                                     
    g_assert_false(pcv_guest_agent_slot_should_probe(&s, now + 30 * G_USEC_PER_SEC));
    g_assert_true(pcv_guest_agent_slot_should_probe(&s, now + 60 * G_USEC_PER_SEC));
}

static void
test_resolve_niclist_arp_without_flag_check(void)
{
                                                                                     
                                                                    
    const gchar *arp =
        "IP address       HW type     Flags       HW address            Mask     Device\n"
        "10.168.3.77     0x1         0x0         52:54:00:00:0d:19     *        br0\n";
    PcvGuestIpInputs in = { .proc_net_arp_text = arp, .arp_require_complete = FALSE };
    const gchar *src = NULL;
    gchar *ip = pcv_guest_ip_resolve(&in, "52:54:00:00:0D:19", PCV_GUEST_IP_MAC_IPV4_THEN_ANY, &src);
    g_assert_cmpstr(ip, ==, "10.168.3.77");
    g_assert_cmpstr(src, ==, "arp");
    g_free(ip);

    in.arp_require_complete = TRUE;
    ip = pcv_guest_ip_resolve(&in, "52:54:00:00:0d:19", PCV_GUEST_IP_ANY_FIRST_IPV4, &src);
    g_assert_null(ip);
    g_assert_cmpstr(src, ==, "");
}

static void
test_from_virt_ifaces_drops_lo(void)
{
                                                                              
                                                                             
    char lo_name[] = "lo", lo_mac[] = "00:00:00:00:00:00", lo_ip[] = "127.0.0.1";
    char eth_name[] = "eth0", eth_mac[] = "52:54:00:AA:BB:CC";
    char eth_v6[] = "fd00::5", eth_empty[] = "", eth_v4[] = "10.0.0.7";
    char nomac_name[] = "vnet9", nomac_v4[] = "10.9.9.9";
    virDomainIPAddress lo_addr[] = { { .type = VIR_IP_ADDR_TYPE_IPV4, .addr = lo_ip, .prefix = 8 } };
    virDomainIPAddress eth_addrs[] = {
        { .type = VIR_IP_ADDR_TYPE_IPV6, .addr = eth_v6,    .prefix = 64 },
        { .type = VIR_IP_ADDR_TYPE_IPV4, .addr = eth_empty, .prefix = 24 },
        { .type = VIR_IP_ADDR_TYPE_IPV4, .addr = eth_v4,    .prefix = 24 },
    };
    virDomainIPAddress nomac_addr[] = { { .type = VIR_IP_ADDR_TYPE_IPV4, .addr = nomac_v4, .prefix = 24 } };
    virDomainInterface lo  = { .name = lo_name,  .hwaddr = lo_mac,  .naddrs = 1, .addrs = lo_addr };
    virDomainInterface eth = { .name = eth_name, .hwaddr = eth_mac, .naddrs = 3, .addrs = eth_addrs };
    virDomainInterface nomac = { .name = nomac_name, .hwaddr = NULL, .naddrs = 1, .addrs = nomac_addr };
    virDomainInterfacePtr arr[] = { &lo, NULL, &eth, &nomac };

    GPtrArray *a = pcv_guest_ip_from_virt_ifaces(arr, (int)G_N_ELEMENTS(arr));
    g_assert_nonnull(a);
    g_assert_cmpuint(a->len, ==, 2);                   
    PcvGuestIface *f = g_ptr_array_index(a, 0);
    g_assert_cmpstr(f->mac, ==, "52:54:00:aa:bb:cc");
    g_assert_cmpuint(f->ipv4->len, ==, 1);                
    g_assert_cmpstr(g_ptr_array_index(f->ipv4, 0), ==, "10.0.0.7");
    g_assert_cmpuint(f->ipv6->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(f->ipv6, 0), ==, "fd00::5");
    f = g_ptr_array_index(a, 1);
    g_assert_null(f->mac);
    g_assert_cmpstr(g_ptr_array_index(f->ipv4, 0), ==, "10.9.9.9");

                                                               
    PcvGuestIpInputs in = { .lease_ifaces = a, .arp_require_complete = TRUE };
    const gchar *src = NULL;
    gchar *ip = pcv_guest_ip_resolve(&in, NULL, PCV_GUEST_IP_ANY_FIRST_IPV4, &src);
    g_assert_cmpstr(ip, ==, "10.0.0.7");
    g_assert_cmpstr(src, ==, "lease");
    g_free(ip);
    g_ptr_array_unref(a);

                                             
    g_assert_null(pcv_guest_ip_from_virt_ifaces(NULL, 0));
    g_assert_null(pcv_guest_ip_from_virt_ifaces(arr, -1));
}

void
test_guest_ip_register(void)
{
    g_test_add_func("/guest_ip/qga_parse_basic", test_qga_parse_basic);
    g_test_add_func("/guest_ip/qga_parse_optional_members", test_qga_parse_optional_members);
    g_test_add_func("/guest_ip/qga_parse_rejects_bad_json", test_qga_parse_rejects_bad_json);
    g_test_add_func("/guest_ip/qga_parse_skips_lo", test_qga_parse_skips_lo);
    g_test_add_func("/guest_ip/resolve_order_lease_first", test_resolve_order_lease_first);
    g_test_add_func("/guest_ip/resolve_agent_before_lease_file", test_resolve_agent_before_lease_file);
    g_test_add_func("/guest_ip/resolve_fleet_any_first_ipv4", test_resolve_fleet_any_first_ipv4);
    g_test_add_func("/guest_ip/resolve_niclist_mac_ipv6_fallback", test_resolve_niclist_mac_ipv6_fallback);
    g_test_add_func("/guest_ip/arp_complete_flag_rule", test_arp_complete_flag_rule);
    g_test_add_func("/guest_ip/lease_file_lookup", test_lease_file_lookup);
    g_test_add_func("/guest_ip/backoff_sequence", test_backoff_sequence);
    g_test_add_func("/guest_ip/backoff_caps_at_600", test_backoff_caps_at_600);
    g_test_add_func("/guest_ip/cache_ttl_expires", test_cache_ttl_expires);
    g_test_add_func("/guest_ip/should_probe_respects_inflight_and_backoff",
                    test_should_probe_respects_inflight_and_backoff);
    g_test_add_func("/guest_ip/resolve_niclist_arp_without_flag_check",
                    test_resolve_niclist_arp_without_flag_check);
    g_test_add_func("/guest_ip/from_virt_ifaces_drops_lo", test_from_virt_ifaces_drops_lo);
}
