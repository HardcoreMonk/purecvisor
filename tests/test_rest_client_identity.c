                                                                                            
                                                                                  
                                                                           
                                                                            
                     
  
                                         
  
                                                
                                              
   

#include <glib.h>

#include "../src/api/rest_client_identity.h"

static gboolean
trusted_by(const gchar *list, const gchar *secret, const gchar *peer, const gchar *hdr)
{
    g_autoptr(PcvRestProxyTrust) trust = pcv_rest_proxy_trust_new(list, secret, NULL);
    return pcv_rest_proxy_trusted(trust, peer, hdr);
}

static void
assert_client_ip(const gchar *expected, gboolean trusted, const gchar *peer_ip,
                 const gchar *x_real_ip, const gchar *x_forwarded_for)
{
    g_autofree gchar *actual = pcv_rest_resolve_client_ip(trusted, peer_ip, x_real_ip, x_forwarded_for);
    g_assert_cmpstr(actual, ==, expected);
}

static void
test_default_list_trusts_only_127_0_0_1(void)
{
    g_assert_true(trusted_by("127.0.0.1", NULL, "127.0.0.1", NULL));
    g_assert_false(trusted_by("127.0.0.1", NULL, "127.0.0.2", NULL));
    g_assert_false(trusted_by("127.0.0.1", NULL, "127.255.255.254", NULL));
    g_assert_false(trusted_by("127.0.0.1", NULL, "::1", NULL));
    g_assert_false(trusted_by("127.0.0.1", NULL, "::ffff:127.0.0.1", NULL));
    g_assert_false(trusted_by("127.0.0.1", NULL, "192.0.2.1", NULL));
    g_assert_false(trusted_by("127.0.0.1", NULL, NULL, NULL));
    g_assert_false(trusted_by("127.0.0.1", NULL, "not-an-ip", NULL));
}

static void
test_cidr_and_ipv6_entries(void)
{
    const gchar *list = "10.0.0.0/8, ::1 ,2001:db8::/32";
    g_assert_true(trusted_by(list, NULL, "10.1.2.3", NULL));
    g_assert_false(trusted_by(list, NULL, "11.0.0.1", NULL));
    g_assert_true(trusted_by(list, NULL, "::1", NULL));
    g_assert_true(trusted_by(list, NULL, "2001:db8::5", NULL));
    g_assert_false(trusted_by(list, NULL, "2001:db9::1", NULL));
    g_assert_false(trusted_by(list, NULL, "::ffff:10.1.2.3", NULL));
    g_assert_false(trusted_by("fe80::/10", NULL, "fe80::1%lo", NULL));
}

static void
test_bad_entries_rejected(void)
{
    GPtrArray *rejected = NULL;
    g_autoptr(PcvRestProxyTrust) trust = pcv_rest_proxy_trust_new(
        "127.0.0.1, localhost, 10.0.0.1/8, fe80::1%lo, 300.1.1.1, , ::1/129", NULL, &rejected);
    g_assert_cmpuint(rejected->len, ==, 5);
    g_assert_cmpstr(g_ptr_array_index(rejected, 0), ==, "localhost");
    g_assert_cmpstr(g_ptr_array_index(rejected, 1), ==, "10.0.0.1/8");
    g_assert_cmpstr(g_ptr_array_index(rejected, 2), ==, "fe80::1%lo");
    g_assert_true(pcv_rest_proxy_trusted(trust, "127.0.0.1", NULL));
    g_assert_false(pcv_rest_proxy_trusted(trust, "10.0.0.1", NULL));
    g_ptr_array_unref(rejected);
}

static void
test_empty_list_trusts_nobody(void)
{
    g_assert_false(trusted_by("", NULL, "127.0.0.1", NULL));
    g_assert_false(trusted_by(" , ", NULL, "127.0.0.1", NULL));
    g_assert_false(trusted_by(NULL, NULL, "127.0.0.1", NULL));
}

static void
test_secret_gate(void)
{
    const gchar *s = "s3cret-test-only-value";
    g_assert_true(trusted_by("127.0.0.1", s, "127.0.0.1", s));
    g_assert_false(trusted_by("127.0.0.1", s, "127.0.0.1", NULL));
    g_assert_false(trusted_by("127.0.0.1", s, "127.0.0.1", "wrong"));
    g_assert_false(trusted_by("127.0.0.1", s, "127.0.0.1", ""));
    g_assert_false(trusted_by("127.0.0.1", s, "127.0.0.1", "s3cret-test-only-valu"));
    g_assert_false(trusted_by("127.0.0.1", s, "192.0.2.1", s));
    g_assert_true(trusted_by("127.0.0.1", "", "127.0.0.1", NULL));
}

static void
test_direct_local(void)
{
    g_assert_true(pcv_rest_is_direct_local("127.0.0.1", FALSE));
    g_assert_true(pcv_rest_is_direct_local("127.9.9.9", FALSE));
    g_assert_true(pcv_rest_is_direct_local("::1", FALSE));
    g_assert_false(pcv_rest_is_direct_local("127.0.0.1", TRUE));
    g_assert_false(pcv_rest_is_direct_local("::ffff:127.0.0.1", FALSE));
    g_assert_false(pcv_rest_is_direct_local("::1%lo", FALSE));
    g_assert_false(pcv_rest_is_direct_local("192.0.2.1", FALSE));
    g_assert_false(pcv_rest_is_direct_local("not-an-ip", FALSE));
    g_assert_false(pcv_rest_is_direct_local(NULL, FALSE));
}

static void
test_untrusted_peer_ignores_forwarding_headers(void)
{
    assert_client_ip("198.51.100.7", FALSE, "198.51.100.7", "203.0.113.10", "192.0.2.1, 203.0.113.11");
    g_assert_false(pcv_rest_resolve_external_https(FALSE, "https"));
}

static void
test_invalid_peer_fails_closed(void)
{
    g_autofree gchar *null_peer = pcv_rest_resolve_client_ip(FALSE, NULL, "203.0.113.30", "203.0.113.31");
    g_assert_null(null_peer);
    assert_client_ip("not-an-ip", FALSE, "not-an-ip", "203.0.113.32", "203.0.113.33");
    assert_client_ip("fe80::1%lo", FALSE, "fe80::1%lo", "203.0.113.34", "203.0.113.35");
}

static void
test_valid_real_ip_wins_and_is_canonical(void)
{
    assert_client_ip("2001:db8::2", TRUE, "127.0.0.1", "  2001:0db8:0:0:0:0:0:2  ", "192.0.2.10, 192.0.2.11");
}

static void
test_missing_real_ip_uses_rightmost_valid_xff(void)
{
    assert_client_ip("2001:db8::9", TRUE, "::1", NULL, " 192.0.2.10 , 2001:0db8:0:0:0:0:0:9 ");
    assert_client_ip("203.0.113.9", TRUE, "127.0.0.1", "", "192.0.2.10, 203.0.113.9");
}

static void
test_invalid_xff_discards_entire_chain(void)
{
    assert_client_ip("127.0.0.1", TRUE, "127.0.0.1", NULL, "192.0.2.1, host.test");
    assert_client_ip("127.0.0.1", TRUE, "127.0.0.1", NULL, "192.0.2.1, ");
    assert_client_ip("127.0.0.1", TRUE, "127.0.0.1", NULL, ",192.0.2.1");
    assert_client_ip("127.0.0.1", TRUE, "127.0.0.1", NULL, "192.0.2.1,,203.0.113.1");
    assert_client_ip("127.0.0.1", TRUE, "127.0.0.1", NULL, "   ");
}

static void
test_invalid_real_ip_is_ignored(void)
{
    assert_client_ip("203.0.113.20", TRUE, "127.0.0.1", "not-an-ip", "192.0.2.20, 203.0.113.20");
    assert_client_ip("203.0.113.21", TRUE, "127.0.0.1", " ", "203.0.113.21");
    assert_client_ip("203.0.113.22", TRUE, "127.0.0.1", "192.0.2.22, 198.51.100.22", "203.0.113.22");
}

static void
test_external_https_normalization_and_rejection(void)
{
    g_assert_true(pcv_rest_resolve_external_https(TRUE, "https"));
    g_assert_true(pcv_rest_resolve_external_https(TRUE, " HTTPS "));
    g_assert_true(pcv_rest_resolve_external_https(TRUE, "\thTtPs\n"));
    g_assert_false(pcv_rest_resolve_external_https(FALSE, "https"));
    g_assert_false(pcv_rest_resolve_external_https(TRUE, "http"));
    g_assert_false(pcv_rest_resolve_external_https(TRUE, "https,http"));
    g_assert_false(pcv_rest_resolve_external_https(TRUE, "https, https"));
    g_assert_false(pcv_rest_resolve_external_https(TRUE, ""));
    g_assert_false(pcv_rest_resolve_external_https(TRUE, NULL));
}

void
test_rest_client_identity_register(void)
{
    g_test_add_func("/rest_client_identity/proxy_trust/default_list",  test_default_list_trusts_only_127_0_0_1);
    g_test_add_func("/rest_client_identity/proxy_trust/cidr_and_ipv6", test_cidr_and_ipv6_entries);
    g_test_add_func("/rest_client_identity/proxy_trust/bad_entries",   test_bad_entries_rejected);
    g_test_add_func("/rest_client_identity/proxy_trust/empty_list",    test_empty_list_trusts_nobody);
    g_test_add_func("/rest_client_identity/proxy_trust/secret",        test_secret_gate);
    g_test_add_func("/rest_client_identity/direct_local",              test_direct_local);
    g_test_add_func("/rest_client_identity/untrusted_peer",            test_untrusted_peer_ignores_forwarding_headers);
    g_test_add_func("/rest_client_identity/invalid_peer",              test_invalid_peer_fails_closed);
    g_test_add_func("/rest_client_identity/real_ip_precedence",        test_valid_real_ip_wins_and_is_canonical);
    g_test_add_func("/rest_client_identity/xff_rightmost",             test_missing_real_ip_uses_rightmost_valid_xff);
    g_test_add_func("/rest_client_identity/xff_invalid_chain",         test_invalid_xff_discards_entire_chain);
    g_test_add_func("/rest_client_identity/real_ip_invalid",           test_invalid_real_ip_is_ignored);
    g_test_add_func("/rest_client_identity/external_https",            test_external_https_normalization_and_rejection);
}
