

















#include <glib.h>
#include <string.h>

#include "modules/dispatcher/handler_monitor.h"

static const gchar *ARP_FIXTURE =
    "IP address       HW type     Flags       HW address            Mask     Device\n"
    "192.0.2.1        0x1         0x2         aa:bb:cc:dd:ee:01     *        br0\n"
    "192.0.2.77       0x1         0x0         52:54:00:00:0d:19     *        br0\n"
    "192.0.2.53       0x1         0x2         52:54:00:00:0D:19     *        br0\n"
    "192.0.2.90       0x1         0x6         52:54:00:aa:00:02     *        br0\n"
    "garbage-line-without-columns\n"
    "10.0.0.9         0x1         0x2         52:54:00:aa:00:03\n";

static void
test_arp_lookup_matches_complete_entry_case_insensitive(void)
{
    gchar ip[64] = "N/A";

    g_assert_true(pcv_monitor_arp_lookup_ipv4(ARP_FIXTURE, "52:54:00:00:0d:19",
                                              ip, sizeof(ip)));
    g_assert_cmpstr(ip, ==, "192.0.2.53");
}

static void
test_arp_lookup_accepts_permanent_entry(void)
{
    gchar ip[64] = "N/A";

    g_assert_true(pcv_monitor_arp_lookup_ipv4(ARP_FIXTURE, "52:54:00:AA:00:02",
                                              ip, sizeof(ip)));
    g_assert_cmpstr(ip, ==, "192.0.2.90");
}

static void
test_arp_lookup_miss_leaves_output_untouched(void)
{
    gchar ip[64] = "N/A";
    g_assert_false(pcv_monitor_arp_lookup_ipv4(ARP_FIXTURE, "52:54:00:ff:ff:ff",
                                               ip, sizeof(ip)));
    g_assert_cmpstr(ip, ==, "N/A");

    const gchar *only_incomplete =
        "IP address       HW type     Flags       HW address            Mask     Device\n"
        "192.0.2.77       0x1         0x0         52:54:00:00:0d:19     *        br0\n";
    g_assert_false(pcv_monitor_arp_lookup_ipv4(only_incomplete, "52:54:00:00:0d:19",
                                               ip, sizeof(ip)));
    g_assert_cmpstr(ip, ==, "N/A");
}

static void
test_arp_lookup_rejects_short_lines_and_bad_inputs(void)
{
    gchar ip[64] = "N/A";

    g_assert_false(pcv_monitor_arp_lookup_ipv4(ARP_FIXTURE, "52:54:00:aa:00:03",
                                               ip, sizeof(ip)));
    g_assert_false(pcv_monitor_arp_lookup_ipv4(NULL, "52:54:00:00:0d:19", ip, sizeof(ip)));
    g_assert_false(pcv_monitor_arp_lookup_ipv4(ARP_FIXTURE, NULL, ip, sizeof(ip)));
    g_assert_false(pcv_monitor_arp_lookup_ipv4(ARP_FIXTURE, "", ip, sizeof(ip)));
    g_assert_false(pcv_monitor_arp_lookup_ipv4(ARP_FIXTURE, "52:54:00:00:0d:19", NULL, 0));
    g_assert_cmpstr(ip, ==, "N/A");
}

static void
test_arp_lookup_never_overflows_output(void)
{
    gchar ip[8] = "N/A";
    g_assert_false(pcv_monitor_arp_lookup_ipv4(ARP_FIXTURE, "52:54:00:00:0d:19",
                                               ip, sizeof(ip)));
    g_assert_cmpstr(ip, ==, "N/A");
}

void
test_monitor_arp_register(void)
{
    g_test_add_func("/monitor/arp/matches_complete_entry_case_insensitive",
                    test_arp_lookup_matches_complete_entry_case_insensitive);
    g_test_add_func("/monitor/arp/accepts_permanent_entry",
                    test_arp_lookup_accepts_permanent_entry);
    g_test_add_func("/monitor/arp/miss_leaves_output_untouched",
                    test_arp_lookup_miss_leaves_output_untouched);
    g_test_add_func("/monitor/arp/rejects_short_lines_and_bad_inputs",
                    test_arp_lookup_rejects_short_lines_and_bad_inputs);
    g_test_add_func("/monitor/arp/never_overflows_output",
                    test_arp_lookup_never_overflows_output);
}
