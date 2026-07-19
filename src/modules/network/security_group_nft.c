
#include "security_group_nft.h"

gchar *
pcv_sg_nft_build_ensure_script(void)
{
    GString *s = g_string_new(NULL);
    g_string_append(s, "add table bridge pcv_sg\n");
    g_string_append(s, "add chain bridge pcv_sg ingress-dispatch "
        "{ type filter hook postrouting priority filter ; policy accept ; }\n");
    g_string_append(s, "add chain bridge pcv_sg egress-dispatch "
        "{ type filter hook prerouting priority filter ; policy accept ; }\n");
    g_string_append(s, "add chain bridge pcv_sg baseline-in\n");
    g_string_append(s, "add chain bridge pcv_sg baseline-out\n");

    g_string_append(s, "flush chain bridge pcv_sg baseline-in\n");
    g_string_append(s, "add rule bridge pcv_sg baseline-in ether type arp accept\n");
    g_string_append(s, "add rule bridge pcv_sg baseline-in udp sport 67 udp dport 68 accept\n");
    g_string_append(s, "add rule bridge pcv_sg baseline-in icmpv6 type "
        "{ nd-neighbor-solicit, nd-neighbor-advert, nd-router-advert } accept\n");
    g_string_append(s, "add rule bridge pcv_sg baseline-in ct state established,related accept\n");
    g_string_append(s, "flush chain bridge pcv_sg baseline-out\n");
    g_string_append(s, "add rule bridge pcv_sg baseline-out ether type arp accept\n");
    g_string_append(s, "add rule bridge pcv_sg baseline-out udp sport 68 udp dport 67 accept\n");
    g_string_append(s, "add rule bridge pcv_sg baseline-out icmpv6 type "
        "{ nd-neighbor-solicit, nd-neighbor-advert, nd-router-solicit } accept\n");
    g_string_append(s, "add rule bridge pcv_sg baseline-out ct state established,related accept\n");
    return g_string_free(s, FALSE);
}

static void
_append_rule(GString *s, const gchar *group, const SgNftRule *r)
{
    gboolean egress = (g_strcmp0(r->direction, "egress") == 0);
    g_string_append_printf(s, "add rule bridge pcv_sg sg-%s-%s ",
                           group, egress ? "out" : "in");
    if (r->source && g_strcmp0(r->source, "0.0.0.0/0") != 0)
        g_string_append_printf(s, "ip %s %s ", egress ? "daddr" : "saddr", r->source);
    if (g_strcmp0(r->protocol, "icmp") == 0) {
        g_string_append(s, "ip protocol icmp ");
    } else if (r->port_start > 0) {
        if (r->port_end > r->port_start)
            g_string_append_printf(s, "%s dport %d-%d ",
                                   r->protocol, r->port_start, r->port_end);
        else
            g_string_append_printf(s, "%s dport %d ", r->protocol, r->port_start);
    } else {
        g_string_append_printf(s, "ip protocol %s ", r->protocol);
    }
    g_string_append(s, "accept\n");
}

gchar *
pcv_sg_nft_build_group_script(const gchar *group, GPtrArray *rules)
{
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "add chain bridge pcv_sg sg-%s-in\n", group);
    g_string_append_printf(s, "add chain bridge pcv_sg sg-%s-out\n", group);
    g_string_append_printf(s, "flush chain bridge pcv_sg sg-%s-in\n", group);
    g_string_append_printf(s, "flush chain bridge pcv_sg sg-%s-out\n", group);
    for (guint i = 0; rules && i < rules->len; i++)
        _append_rule(s, group, g_ptr_array_index(rules, i));
    return g_string_free(s, FALSE);
}

gchar *
pcv_sg_nft_build_group_delete_script(const gchar *group)
{
    return g_strdup_printf(
        "destroy chain bridge pcv_sg sg-%s-in\n"
        "destroy chain bridge pcv_sg sg-%s-out\n", group, group);
}

gchar *
pcv_sg_nft_build_dispatch_script(GPtrArray *bindings)
{
    GString *s = g_string_new(NULL);
    g_string_append(s, "flush chain bridge pcv_sg ingress-dispatch\n");
    g_string_append(s, "flush chain bridge pcv_sg egress-dispatch\n");
    for (guint i = 0; bindings && i < bindings->len; i++) {
        const SgNftBinding *b = g_ptr_array_index(bindings, i);

        g_string_append_printf(s,
            "add rule bridge pcv_sg ingress-dispatch oifname \"%s\" jump baseline-in\n",
            b->vnet);
        for (guint j = 0; j < b->groups->len; j++)
            g_string_append_printf(s,
                "add rule bridge pcv_sg ingress-dispatch oifname \"%s\" jump sg-%s-in\n",
                b->vnet, (const gchar *)g_ptr_array_index(b->groups, j));
        g_string_append_printf(s,
            "add rule bridge pcv_sg ingress-dispatch oifname \"%s\" drop\n", b->vnet);

        if (!b->egress_enforced)
            continue;
        g_string_append_printf(s,
            "add rule bridge pcv_sg egress-dispatch iifname \"%s\" jump baseline-out\n",
            b->vnet);
        for (guint j = 0; j < b->groups->len; j++)
            g_string_append_printf(s,
                "add rule bridge pcv_sg egress-dispatch iifname \"%s\" jump sg-%s-out\n",
                b->vnet, (const gchar *)g_ptr_array_index(b->groups, j));
        g_string_append_printf(s,
            "add rule bridge pcv_sg egress-dispatch iifname \"%s\" drop\n", b->vnet);
    }
    return g_string_free(s, FALSE);
}
