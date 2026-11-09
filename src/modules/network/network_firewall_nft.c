   
                               
                                                         
  
                           
                                                                                      
                                                                       
                                                                                                
                                                                                    
                                                             
                                                                 
  
                                                                      
   
#include "modules/network/network_firewall_nft.h"

#include <gio/gio.h>
#include <string.h>

#define GFW_T "inet purecvisor"

                                            
PcvGuestFwNet *
pcv_guest_fw_net_new(const gchar *bridge, const gchar *mode, const gchar *cidr)
{
    PcvGuestFwNet *net = g_new0(PcvGuestFwNet, 1);
    net->bridge = g_strdup(bridge);
    net->mode = g_strdup(mode);
    net->cidr = g_strdup(cidr);
    return net;
}

                                                
void
pcv_guest_fw_net_free(gpointer data)
{
    PcvGuestFwNet *net = data;
    if (!net)
        return;
    g_free(net->bridge);
    g_free(net->mode);
    g_free(net->cidr);
    g_free(net);
}

                                  
PcvGuestFwSnapshot *
pcv_guest_fw_snapshot_new(void)
{
    PcvGuestFwSnapshot *snap = g_new0(PcvGuestFwSnapshot, 1);
    snap->nets = g_ptr_array_new_with_free_func(pcv_guest_fw_net_free);
    return snap;
}

                                           
void
pcv_guest_fw_snapshot_free(PcvGuestFwSnapshot *snap)
{
    if (!snap)
        return;
    g_clear_pointer(&snap->nets, g_ptr_array_unref);
    g_free(snap);
}

                                         
void
pcv_guest_fw_snapshot_add(PcvGuestFwSnapshot *snap, const gchar *bridge, const gchar *mode, const gchar *cidr)
{
    g_ptr_array_add(snap->nets, pcv_guest_fw_net_new(bridge, mode, cidr));
}

                                                                                  
gchar *
pcv_guest_fw_cidr_to_subnet(const gchar *cidr, GError **error)
{
    if (!cidr) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "CIDR is NULL");
        return NULL;
    }
    gchar **parts = g_strsplit(cidr, ".", 4);                                              
    if (!parts || g_strv_length(parts) != 4) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid CIDR: %s", cidr);
        g_strfreev(parts);
        return NULL;
    }
    gchar **last = g_strsplit(parts[3], "/", 2);
    if (!last || g_strv_length(last) < 2 || !last[1]) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid CIDR format (missing /prefix): %s", cidr);
        g_strfreev(last);
        g_strfreev(parts);
        return NULL;
    }
    gchar *subnet = g_strdup_printf("%s.%s.%s.0/%s", parts[0], parts[1], parts[2], last[1]);
    g_strfreev(parts);
    g_strfreev(last);
    return subnet;
}

                              
static gint
_net_cmp(gconstpointer a, gconstpointer b)
{
    const PcvGuestFwNet *x = *(PcvGuestFwNet *const *)a;
    const PcvGuestFwNet *y = *(PcvGuestFwNet *const *)b;
    return g_strcmp0(x->bridge, y->bridge);
}

                   
static gboolean
_known_mode(const gchar *mode)
{
    return g_strcmp0(mode, "nat") == 0 || g_strcmp0(mode, "isolated") == 0 || g_strcmp0(mode, "routed") == 0;
}

                                                                                   
static gboolean
_append_names(GString *s, GPtrArray *sorted, const gchar *mode)
{
    guint n = 0;
    for (guint i = 0; i < sorted->len; i++) {
        const PcvGuestFwNet *net = g_ptr_array_index(sorted, i);
        if (mode && g_strcmp0(net->mode, mode) != 0)
            continue;
        g_string_append_printf(s, "%s\"%s\"", n++ ? ", " : "", net->bridge);
    }
    return n > 0;
}

                                                      
gchar *
pcv_guest_fw_build_script(const PcvGuestFwSnapshot *snap)
{
    g_autoptr(GPtrArray) sorted = g_ptr_array_new();                          
    for (guint i = 0; snap && snap->nets && i < snap->nets->len; i++) {
        PcvGuestFwNet *net = g_ptr_array_index(snap->nets, i);
        if (net && net->bridge && _known_mode(net->mode))
            g_ptr_array_add(sorted, net);
    }
    g_ptr_array_sort(sorted, _net_cmp);

    GString *s = g_string_new(
        "add table " GFW_T "\n"
        "add set " GFW_T " guest_nat_ifaces { type ifname; }\n"
        "add set " GFW_T " guest_routed_ifaces { type ifname; }\n"
        "add set " GFW_T " guest_isolated_ifaces { type ifname; }\n"
        "add set " GFW_T " guest_ifaces { type ifname; }\n"
        "add chain " GFW_T " postrouting { type nat hook postrouting priority srcnat; }\n"
        "add chain " GFW_T " forward { type filter hook forward priority filter; }\n"
        "add chain " GFW_T " guest_input { type filter hook input priority filter; policy accept; }\n"
        "flush chain " GFW_T " postrouting\n"
        "flush chain " GFW_T " forward\n"
        "flush chain " GFW_T " guest_input\n"
        "flush set " GFW_T " guest_nat_ifaces\n"
        "flush set " GFW_T " guest_routed_ifaces\n"
        "flush set " GFW_T " guest_isolated_ifaces\n"
        "flush set " GFW_T " guest_ifaces\n");

    static const struct { const gchar *set; const gchar *mode; } sets[] = {
        {"guest_nat_ifaces", "nat"}, {"guest_routed_ifaces", "routed"},
        {"guest_isolated_ifaces", "isolated"}, {"guest_ifaces", NULL},
    };
    for (gsize k = 0; k < G_N_ELEMENTS(sets); k++) {
        GString *names = g_string_new(NULL);
        if (_append_names(names, sorted, sets[k].mode))
            g_string_append_printf(s, "add element " GFW_T " %s { %s }\n", sets[k].set, names->str);
        g_string_free(names, TRUE);
    }

                                                                              
    static const gchar *const input_rules[] = {
        "ct state established,related accept",
        "udp dport 67 accept",
        "udp dport 547 accept",
        "icmpv6 type { nd-router-solicit, nd-neighbor-solicit, nd-neighbor-advert } accept",
        "fib daddr . iif type local udp dport 53 accept",
        "fib daddr . iif type local tcp dport 53 accept",
        "fib daddr . iif type local icmp type echo-request accept",
        "fib daddr . iif type local icmpv6 type echo-request accept",
        "drop",
    };
    for (gsize k = 0; k < G_N_ELEMENTS(input_rules); k++)
        g_string_append_printf(s, "add rule " GFW_T " guest_input iifname @guest_ifaces %s\n", input_rules[k]);

                                                             
    for (guint i = 0; i < sorted->len; i++) {
        const PcvGuestFwNet *net = g_ptr_array_index(sorted, i);
        g_string_append_printf(s, "add rule " GFW_T " forward iifname \"%s\" oifname \"%s\" accept\n",
                               net->bridge, net->bridge);
    }
                                                                               
    g_string_append(s,
        "add rule " GFW_T " forward iifname @guest_isolated_ifaces drop\n"
        "add rule " GFW_T " forward oifname @guest_isolated_ifaces drop\n"
        "add rule " GFW_T " forward iifname @guest_ifaces oifname @guest_ifaces drop\n"
        "add rule " GFW_T " forward ct state established,related accept\n"
        "add rule " GFW_T " forward iifname != @guest_ifaces oifname @guest_nat_ifaces ct status dnat accept\n"
        "add rule " GFW_T " forward oifname @guest_nat_ifaces drop\n");

                                                                
    for (guint i = 0; i < sorted->len; i++) {
        const PcvGuestFwNet *net = g_ptr_array_index(sorted, i);
        if (g_strcmp0(net->mode, "nat") != 0)
            continue;
        g_autofree gchar *subnet = pcv_guest_fw_cidr_to_subnet(net->cidr, NULL);
        if (!subnet)
            continue;                            
        g_string_append_printf(s, "add rule " GFW_T " postrouting oifname != \"%s\" ip saddr %s masquerade\n",
                               net->bridge, subnet);
    }
    return g_string_free(s, FALSE);
}
