
#include <glib.h>
#include <string.h>
#include <stdarg.h>
#include "../src/modules/network/security_group_nft.h"

static void test_ensure_script_shape(void) {
    gchar *s = pcv_sg_nft_build_ensure_script();
    g_assert_nonnull(strstr(s, "add table bridge pcv_sg\n"));

    g_assert_nonnull(strstr(s, "add chain bridge pcv_sg ingress-dispatch "
        "{ type filter hook postrouting priority filter ; policy accept ; }\n"));
    g_assert_nonnull(strstr(s, "add chain bridge pcv_sg egress-dispatch "
        "{ type filter hook prerouting priority filter ; policy accept ; }\n"));

    g_assert_nonnull(strstr(s, "flush chain bridge pcv_sg baseline-in\n"));
    g_assert_nonnull(strstr(s, "baseline-in ether type arp accept\n"));
    g_assert_nonnull(strstr(s, "baseline-in udp sport 67 udp dport 68 accept\n"));
    g_assert_nonnull(strstr(s, "baseline-in ct state established,related accept\n"));
    g_assert_nonnull(strstr(s, "flush chain bridge pcv_sg baseline-out\n"));
    g_assert_nonnull(strstr(s, "baseline-out udp sport 68 udp dport 67 accept\n"));
    g_free(s);
}

static void test_group_script_rules(void) {
    GPtrArray *rules = g_ptr_array_new();
    SgNftRule r1 = { "ingress", "tcp", 80, 0, "10.0.0.0/24" };
    SgNftRule r2 = { "ingress", "udp", 5000, 5100, "0.0.0.0/0" };
    SgNftRule r3 = { "ingress", "icmp", 0, 0, "192.0.2.10/16" };
    SgNftRule r4 = { "egress",  "tcp", 443, 0, "1.1.1.1/32" };
    g_ptr_array_add(rules, &r1); g_ptr_array_add(rules, &r2);
    g_ptr_array_add(rules, &r3); g_ptr_array_add(rules, &r4);

    gchar *s = pcv_sg_nft_build_group_script("web", rules);

    g_assert_nonnull(strstr(s, "add chain bridge pcv_sg sg-web-in\n"));
    g_assert_nonnull(strstr(s, "add chain bridge pcv_sg sg-web-out\n"));
    g_assert_nonnull(strstr(s, "flush chain bridge pcv_sg sg-web-in\n"));
    g_assert_nonnull(strstr(s, "flush chain bridge pcv_sg sg-web-out\n"));
    g_assert_nonnull(strstr(s,
        "add rule bridge pcv_sg sg-web-in ip saddr 10.0.0.0/24 tcp dport 80 accept\n"));
    g_assert_nonnull(strstr(s,
        "add rule bridge pcv_sg sg-web-in udp dport 5000-5100 accept\n"));
    g_assert_nonnull(strstr(s,
        "add rule bridge pcv_sg sg-web-in ip saddr 192.0.2.10/16 ip protocol icmp accept\n"));
    g_assert_nonnull(strstr(s,
        "add rule bridge pcv_sg sg-web-out ip daddr 1.1.1.1/32 tcp dport 443 accept\n"));

    g_assert_null(strstr(s, "drop"));
    g_free(s);
    g_ptr_array_unref(rules);
}

static void test_group_script_empty(void) {
    gchar *s = pcv_sg_nft_build_group_script("db", NULL);
    g_assert_nonnull(strstr(s, "add chain bridge pcv_sg sg-db-in\n"));
    g_assert_nonnull(strstr(s, "flush chain bridge pcv_sg sg-db-out\n"));
    g_assert_null(strstr(s, "add rule"));
    g_free(s);
}

static void test_group_delete_script(void) {
    gchar *s = pcv_sg_nft_build_group_delete_script("web");

    g_assert_cmpstr(s, ==,
        "destroy chain bridge pcv_sg sg-web-in\n"
        "destroy chain bridge pcv_sg sg-web-out\n");
    g_free(s);
}

static SgNftBinding *
_mk_binding(const gchar *vnet, gboolean egress, ...)
{
    va_list ap;
    SgNftBinding *b = g_new0(SgNftBinding, 1);
    b->vnet = vnet;
    b->egress_enforced = egress;
    b->groups = g_ptr_array_new();
    va_start(ap, egress);
    const gchar *g;
    while ((g = va_arg(ap, const gchar *)) != NULL)
        g_ptr_array_add(b->groups, (gpointer)g);
    va_end(ap);
    return b;
}

static void _binding_free(SgNftBinding *b) {
    g_ptr_array_unref(b->groups);
    g_free(b);
}

static void test_dispatch_single_binding(void) {
    GPtrArray *bindings = g_ptr_array_new();
    SgNftBinding *b = _mk_binding("vnet3", FALSE, "web", NULL);
    g_ptr_array_add(bindings, b);

    gchar *s = pcv_sg_nft_build_dispatch_script(bindings);

    const gchar *flush_in = strstr(s, "flush chain bridge pcv_sg ingress-dispatch");
    const gchar *flush_out = strstr(s, "flush chain bridge pcv_sg egress-dispatch");
    const gchar *first_add = strstr(s, "add rule bridge pcv_sg");
    g_assert_nonnull(flush_in); g_assert_nonnull(flush_out);
    g_assert_true(flush_in < first_add);
    g_assert_true(flush_out < first_add);

    const gchar *base = strstr(s, "ingress-dispatch oifname \"vnet3\" jump baseline-in");
    const gchar *grp  = strstr(s, "ingress-dispatch oifname \"vnet3\" jump sg-web-in");
    const gchar *drop = strstr(s, "ingress-dispatch oifname \"vnet3\" drop");
    g_assert_nonnull(base); g_assert_nonnull(grp); g_assert_nonnull(drop);
    g_assert_true(base < grp && grp < drop);

    g_assert_null(strstr(s, "iifname \"vnet3\""));
    g_free(s);
    _binding_free(b); g_ptr_array_unref(bindings);
}

static void test_dispatch_multi_group_and_egress(void) {
    GPtrArray *bindings = g_ptr_array_new();
    SgNftBinding *b = _mk_binding("vnet7", TRUE, "web", "db", NULL);
    g_ptr_array_add(bindings, b);

    gchar *s = pcv_sg_nft_build_dispatch_script(bindings);

    const gchar *flush_in = strstr(s, "flush chain bridge pcv_sg ingress-dispatch");
    const gchar *flush_out = strstr(s, "flush chain bridge pcv_sg egress-dispatch");
    const gchar *first_add = strstr(s, "add rule bridge pcv_sg");
    g_assert_nonnull(flush_in); g_assert_nonnull(flush_out);
    g_assert_true(flush_in < first_add);
    g_assert_true(flush_out < first_add);

    const gchar *ing_base = strstr(s, "ingress-dispatch oifname \"vnet7\" jump baseline-in");
    const gchar *ing_web = strstr(s, "ingress-dispatch oifname \"vnet7\" jump sg-web-in");
    const gchar *ing_db = strstr(s, "ingress-dispatch oifname \"vnet7\" jump sg-db-in");
    const gchar *ing_drop = strstr(s, "ingress-dispatch oifname \"vnet7\" drop");
    g_assert_nonnull(ing_base); g_assert_nonnull(ing_web);
    g_assert_nonnull(ing_db); g_assert_nonnull(ing_drop);
    g_assert_true(ing_base < ing_web && ing_web < ing_db && ing_db < ing_drop);

    const gchar *eg_base = strstr(s, "egress-dispatch iifname \"vnet7\" jump baseline-out");
    const gchar *eg_web = strstr(s, "egress-dispatch iifname \"vnet7\" jump sg-web-out");
    const gchar *eg_db = strstr(s, "egress-dispatch iifname \"vnet7\" jump sg-db-out");
    const gchar *eg_drop = strstr(s, "egress-dispatch iifname \"vnet7\" drop");
    g_assert_nonnull(eg_base); g_assert_nonnull(eg_web);
    g_assert_nonnull(eg_db); g_assert_nonnull(eg_drop);
    g_assert_true(eg_base < eg_web && eg_web < eg_db && eg_db < eg_drop);

    g_free(s);
    _binding_free(b); g_ptr_array_unref(bindings);
}

static void test_dispatch_empty(void) {
    gchar *s = pcv_sg_nft_build_dispatch_script(NULL);
    g_assert_cmpstr(s, ==,
        "flush chain bridge pcv_sg ingress-dispatch\n"
        "flush chain bridge pcv_sg egress-dispatch\n");
    g_free(s);
}

static void test_no_host_hooks_anywhere(void) {
    GPtrArray *rules = g_ptr_array_new();
    SgNftRule r = { "ingress", "tcp", 80, 0, "10.0.0.0/24" };
    g_ptr_array_add(rules, &r);
    GPtrArray *bindings = g_ptr_array_new();
    SgNftBinding *b = _mk_binding("vnet0", TRUE, "web", NULL);
    g_ptr_array_add(bindings, b);

    gchar *scripts[4];
    scripts[0] = pcv_sg_nft_build_ensure_script();
    scripts[1] = pcv_sg_nft_build_group_script("web", rules);
    scripts[2] = pcv_sg_nft_build_group_delete_script("web");
    scripts[3] = pcv_sg_nft_build_dispatch_script(bindings);
    for (gsize i = 0; i < G_N_ELEMENTS(scripts); i++) {
        g_assert_null(strstr(scripts[i], "hook input"));
        g_assert_null(strstr(scripts[i], "hook output"));
        g_assert_null(strstr(scripts[i], "inet purecvisor"));
        g_free(scripts[i]);
    }
    g_ptr_array_unref(rules);
    _binding_free(b); g_ptr_array_unref(bindings);
}

void test_sg_nft_builder_register(void) {
    g_test_add_func("/sg_nft/ensure/shape", test_ensure_script_shape);
    g_test_add_func("/sg_nft/group/rules",  test_group_script_rules);
    g_test_add_func("/sg_nft/group/empty",  test_group_script_empty);
    g_test_add_func("/sg_nft/group/delete", test_group_delete_script);
    g_test_add_func("/sg_nft/dispatch/single",       test_dispatch_single_binding);
    g_test_add_func("/sg_nft/dispatch/multi_egress", test_dispatch_multi_group_and_egress);
    g_test_add_func("/sg_nft/dispatch/empty",        test_dispatch_empty);
    g_test_add_func("/sg_nft/guard/no_host_hooks",   test_no_host_hooks_anywhere);
}
