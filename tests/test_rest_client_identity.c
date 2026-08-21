                                                                                            
                                                                                  
                                                                           
                                                                            
                     
  
                                         
  
                                                
                                              
   

#include <glib.h>

#include "../src/api/rest_client_identity.h"

static void
assert_client_ip(const gchar *expected,
                 const gchar *peer_ip,
                 const gchar *x_real_ip,
                 const gchar *x_forwarded_for)
{
    g_autofree gchar *actual = pcv_rest_resolve_client_ip(peer_ip,
                                                          x_real_ip,
                                                          x_forwarded_for);
    g_assert_cmpstr(actual, ==, expected);
}

static void
test_untrusted_peer_ignores_forwarding_headers(void)
{
    assert_client_ip("198.51.100.7",
                     "198.51.100.7",
                     "203.0.113.10",
                     "192.0.2.1, 203.0.113.11");
    g_assert_false(pcv_rest_resolve_external_https("198.51.100.7", "https"));
}

static void
test_invalid_peer_fails_closed(void)
{
    g_autofree gchar *null_peer = pcv_rest_resolve_client_ip(NULL,
                                                             "203.0.113.30",
                                                             "203.0.113.31");

    g_assert_null(null_peer);
    g_assert_false(pcv_rest_resolve_external_https(NULL, "https"));

    assert_client_ip("not-an-ip",
                     "not-an-ip",
                     "203.0.113.32",
                     "203.0.113.33");
    g_assert_false(pcv_rest_resolve_external_https("not-an-ip", "https"));

                                                             
    assert_client_ip("fe80::1%lo",
                     "fe80::1%lo",
                     "203.0.113.34",
                     "203.0.113.35");
    g_assert_false(pcv_rest_resolve_external_https("fe80::1%lo", "https"));
}

static void
test_ipv4_loopback_trust_boundaries(void)
{
    assert_client_ip("203.0.113.1", "127.0.0.1", "203.0.113.1", NULL);
    assert_client_ip("203.0.113.2", "127.255.255.254", "203.0.113.2", NULL);
    assert_client_ip("126.255.255.255",
                     "126.255.255.255",
                     "203.0.113.3",
                     NULL);
    assert_client_ip("128.0.0.0", "128.0.0.0", "203.0.113.4", NULL);
}

static void
test_ipv6_only_exact_loopback_is_trusted(void)
{
    assert_client_ip("2001:db8::1", "::1", "2001:0db8:0:0:0:0:0:1", NULL);
    assert_client_ip("::2", "::2", "203.0.113.5", NULL);
    assert_client_ip("::ffff:127.0.0.1",
                     "::ffff:127.0.0.1",
                     "203.0.113.6",
                     NULL);
}

static void
test_valid_real_ip_wins_and_is_canonical(void)
{
    assert_client_ip("2001:db8::2",
                     "127.1.2.3",
                     "  2001:0db8:0:0:0:0:0:2  ",
                     "192.0.2.10, 192.0.2.11");
}

static void
test_missing_real_ip_uses_rightmost_valid_xff(void)
{
    assert_client_ip("2001:db8::9",
                     "::1",
                     NULL,
                     " 192.0.2.10 , 2001:0db8:0:0:0:0:0:9 ");
    assert_client_ip("203.0.113.9",
                     "127.0.0.1",
                     "",
                     "192.0.2.10, 203.0.113.9");
}

static void
test_invalid_xff_discards_entire_chain(void)
{
    assert_client_ip("127.0.0.1", "127.0.0.1", NULL, "192.0.2.1, host.test");
    assert_client_ip("127.0.0.1", "127.0.0.1", NULL, "192.0.2.1, ");
    assert_client_ip("127.0.0.1", "127.0.0.1", NULL, ",192.0.2.1");
    assert_client_ip("127.0.0.1", "127.0.0.1", NULL, "192.0.2.1,,203.0.113.1");
    assert_client_ip("127.0.0.1", "127.0.0.1", NULL, "   ");
}

static void
test_invalid_real_ip_is_ignored(void)
{
    assert_client_ip("203.0.113.20",
                     "127.0.0.1",
                     "not-an-ip",
                     "192.0.2.20, 203.0.113.20");
    assert_client_ip("203.0.113.21",
                     "127.0.0.1",
                     " ",
                     "203.0.113.21");
    assert_client_ip("203.0.113.22",
                     "127.0.0.1",
                     "192.0.2.22, 198.51.100.22",
                     "203.0.113.22");
}

static void
test_external_https_normalization_and_rejection(void)
{
    g_assert_true(pcv_rest_resolve_external_https("127.0.0.1", "https"));
    g_assert_true(pcv_rest_resolve_external_https("127.255.255.254", " HTTPS "));
    g_assert_true(pcv_rest_resolve_external_https("::1", "\thTtPs\n"));

    g_assert_false(pcv_rest_resolve_external_https("::2", "https"));
    g_assert_false(pcv_rest_resolve_external_https("127.0.0.1", "http"));
    g_assert_false(pcv_rest_resolve_external_https("127.0.0.1", "https,http"));
    g_assert_false(pcv_rest_resolve_external_https("127.0.0.1", "https, https"));
    g_assert_false(pcv_rest_resolve_external_https("127.0.0.1", ""));
    g_assert_false(pcv_rest_resolve_external_https("127.0.0.1", NULL));
}

void
test_rest_client_identity_register(void)
{
    g_test_add_func("/rest_client_identity/untrusted_peer",
                    test_untrusted_peer_ignores_forwarding_headers);
    g_test_add_func("/rest_client_identity/invalid_peer",
                    test_invalid_peer_fails_closed);
    g_test_add_func("/rest_client_identity/ipv4_loopback_boundaries",
                    test_ipv4_loopback_trust_boundaries);
    g_test_add_func("/rest_client_identity/ipv6_exact_loopback",
                    test_ipv6_only_exact_loopback_is_trusted);
    g_test_add_func("/rest_client_identity/real_ip_precedence",
                    test_valid_real_ip_wins_and_is_canonical);
    g_test_add_func("/rest_client_identity/xff_rightmost",
                    test_missing_real_ip_uses_rightmost_valid_xff);
    g_test_add_func("/rest_client_identity/xff_invalid_chain",
                    test_invalid_xff_discards_entire_chain);
    g_test_add_func("/rest_client_identity/real_ip_invalid",
                    test_invalid_real_ip_is_ignored);
    g_test_add_func("/rest_client_identity/external_https",
                    test_external_https_normalization_and_rejection);
}
