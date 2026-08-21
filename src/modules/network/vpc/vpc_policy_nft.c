   
                         
                                            
  
                           
                                                                           
                                                                   
                                                                           
                                                                           
                                                           
                                                               
                                                                         
                                                                            
                                               
  
                       
                                                         
                                                       
                                                                     
  
              
                                                                     
                                                                                
                                                                     
                                                                         
                                                                             
                                                                             
                                          
  
                  
                                                                           
                                                                      
                                                             
                                                                   
  
                 
                                                               
                                                             
                                                           
                                                    
   
#include "vpc_policy_nft.h"

static void
_attachment_free(gpointer data)
{
    PcvVpcPolicyAttachment *a = data;
    if (!a) return;
    g_free(a->ip_address);
    g_free(a->mac_address);
    g_free(a);
}

static void
_subnet_free(gpointer data)
{
    PcvVpcPolicySubnet *s = data;
    if (!s) return;
    g_free(s->id);
    g_free(s->backend);
    g_free(s->bridge_name);
    g_free(s->cidr);
    g_free(s->gateway);
    g_clear_pointer(&s->attachments, g_ptr_array_unref);
    g_free(s);
}

static void
_vpc_free(gpointer data)
{
    PcvVpcPolicyVpc *v = data;
    if (!v) return;
    g_free(v->id);
    g_free(v->backend);
    g_free(v->edge_interface);
    g_free(v->egress_mode);
    g_clear_pointer(&v->subnets, g_ptr_array_unref);
    g_free(v);
}

static void
_publish_free(gpointer data)
{
    PcvVpcPolicyPublish *p = data;
    if (!p) return;
    g_free(p->protocol);
    g_free(p->listen_address);
    g_free(p->target_ip);
    g_free(p->target_bridge);
    g_clear_pointer(&p->allowed_sources, g_ptr_array_unref);
    g_free(p);
}

                           
                                                                        
                                                               
                                                                         
                                                                             
                                 
  
                       
                                                      
                                                        
                   
PcvVpcPolicySnapshot *
pcv_vpc_policy_snapshot_new(void)
{
    PcvVpcPolicySnapshot *s = g_new0(PcvVpcPolicySnapshot, 1);
    s->vpcs = g_ptr_array_new_with_free_func(_vpc_free);
    s->publishes = g_ptr_array_new_with_free_func(_publish_free);
    return s;
}

void
pcv_vpc_policy_snapshot_free(PcvVpcPolicySnapshot *snapshot)
{
    if (!snapshot) return;
    g_clear_pointer(&snapshot->vpcs, g_ptr_array_unref);
    g_clear_pointer(&snapshot->publishes, g_ptr_array_unref);
    g_free(snapshot);
}

                           
                                                                    
                                                               
                                                                          
                                             
  
                       
                                                        
                                                             
PcvVpcPolicyVpc *
pcv_vpc_policy_vpc_new(const gchar *id, const gchar *egress_mode)
{
    PcvVpcPolicyVpc *v = g_new0(PcvVpcPolicyVpc, 1);
    v->id = g_strdup(id);
    v->backend = g_strdup("linux");
    v->egress_mode = g_strdup(egress_mode);
    v->subnets = g_ptr_array_new_with_free_func(_subnet_free);
    return v;
}

                           
                                                                                 
                                                            
                                                                       
                                               
  
                       
                                                            
                                                              
PcvVpcPolicySubnet *
pcv_vpc_policy_subnet_new(const gchar *id,
                          const gchar *bridge_name,
                          const gchar *cidr,
                          const gchar *gateway)
{
    PcvVpcPolicySubnet *s = g_new0(PcvVpcPolicySubnet, 1);
    s->id = g_strdup(id);
    s->backend = g_strdup("linux");
    s->bridge_name = g_strdup(bridge_name);
    s->cidr = g_strdup(cidr);
    s->gateway = g_strdup(gateway);
    s->attachments = g_ptr_array_new_with_free_func(_attachment_free);
    return s;
}

                           
                                                                       
                                                                
                                                                   
  
                       
                                                         
                                                           
PcvVpcPolicyAttachment *
pcv_vpc_policy_attachment_new(const gchar *ip, const gchar *mac)
{
    PcvVpcPolicyAttachment *a = g_new0(PcvVpcPolicyAttachment, 1);
    a->ip_address = g_strdup(ip);
    a->mac_address = g_strdup(mac);
    return a;
}

                           
                                                                                     
                                                                         
                                                                            
                                                          
  
                       
                                                     
                                                         
PcvVpcPolicyPublish *
pcv_vpc_policy_publish_new(const gchar *protocol,
                           const gchar *listen_address,
                           guint16 listen_port,
                           const gchar *target_ip,
                           guint16 target_port,
                           const gchar *target_bridge)
{
    PcvVpcPolicyPublish *p = g_new0(PcvVpcPolicyPublish, 1);
    p->protocol = g_strdup(protocol);
    p->listen_address = g_strdup(listen_address);
    p->listen_port = listen_port;
    p->target_ip = g_strdup(target_ip);
    p->target_port = target_port;
    p->target_bridge = g_strdup(target_bridge);
    p->allowed_sources = g_ptr_array_new_with_free_func(g_free);
    return p;
}

                           
                                                                  
                                                                           
                              
static GPtrArray *
_all_subnets(const PcvVpcPolicySnapshot *snapshot)
{
    GPtrArray *all = g_ptr_array_new();
    for (guint i = 0; snapshot && snapshot->vpcs && i < snapshot->vpcs->len; i++) {
        PcvVpcPolicyVpc *v = g_ptr_array_index(snapshot->vpcs, i);
        for (guint j = 0; v && v->subnets && j < v->subnets->len; j++) {
            PcvVpcPolicySubnet *subnet = g_ptr_array_index(v->subnets, j);
            if (g_strcmp0(subnet->backend, "linux") == 0)
                g_ptr_array_add(all, subnet);
        }
    }
    return all;
}

static void
_append_base(GString *s, GPtrArray *subnets, const PcvVpcPolicySnapshot *snapshot)
{
                                                                      
                                                               
                                                   
    g_string_append(s,
        "add table inet pcv_vpc_quarantine\n"
        "delete table inet pcv_vpc_quarantine\n"
        "add table inet pcv_vpc\n"
        "delete table inet pcv_vpc\n"
        "add table bridge pcv_vpc_guard\n"
        "delete table bridge pcv_vpc_guard\n"
        "add table inet pcv_vpc\n"
        "add set inet pcv_vpc managed_ifaces { type ifname; }\n"
        "add chain inet pcv_vpc input { type filter hook input priority -20; policy accept; }\n"
        "add chain inet pcv_vpc forward { type filter hook forward priority -20; policy accept; }\n"
        "add chain inet pcv_vpc prerouting { type nat hook prerouting priority dstnat; policy accept; }\n"
        "add chain inet pcv_vpc postrouting { type nat hook postrouting priority srcnat; policy accept; }\n"
        "add table bridge pcv_vpc_guard\n"
        "add chain bridge pcv_vpc_guard source_guard "
        "{ type filter hook prerouting priority -300; policy accept; }\n");

    guint ovn_edges = 0;
    for (guint i = 0; snapshot && i < snapshot->vpcs->len; i++) {
        PcvVpcPolicyVpc *v = g_ptr_array_index(snapshot->vpcs, i);
        if (g_strcmp0(v->backend, "ovn") == 0 && v->edge_interface)
            ovn_edges++;
    }
    if (subnets->len == 0 && ovn_edges == 0)
        return;
    g_string_append(s, "add element inet pcv_vpc managed_ifaces { ");
    guint emitted = 0;
    for (guint i = 0; i < subnets->len; i++) {
        PcvVpcPolicySubnet *subnet = g_ptr_array_index(subnets, i);
        g_string_append_printf(s, "%s\"%s\"", emitted++ ? ", " : "", subnet->bridge_name);
    }
    for (guint i = 0; snapshot && i < snapshot->vpcs->len; i++) {
        PcvVpcPolicyVpc *v = g_ptr_array_index(snapshot->vpcs, i);
        if (g_strcmp0(v->backend, "ovn") == 0 && v->edge_interface)
            g_string_append_printf(s, "%s\"%s\"", emitted++ ? ", " : "", v->edge_interface);
    }
    g_string_append(s, " }\n");
}

static void
_append_host_and_guard(GString *s, GPtrArray *subnets,
                       const PcvVpcPolicySnapshot *snapshot)
{
    for (guint i = 0; i < subnets->len; i++) {
        PcvVpcPolicySubnet *subnet = g_ptr_array_index(subnets, i);
        g_string_append_printf(s,
            "add rule inet pcv_vpc input iifname \"%s\" udp dport { 53, 67 } accept\n"
            "add rule inet pcv_vpc input iifname \"%s\" tcp dport 53 accept\n"
            "add rule inet pcv_vpc input iifname \"%s\" ip protocol icmp "
            "icmp type { echo-request, destination-unreachable, time-exceeded } accept\n"
            "add rule inet pcv_vpc input iifname \"%s\" drop\n",
            subnet->bridge_name, subnet->bridge_name,
            subnet->bridge_name, subnet->bridge_name);

                                                                             
        g_string_append_printf(s,
            "add rule bridge pcv_vpc_guard source_guard ibrname \"%s\" "
            "ip saddr 0.0.0.0 udp sport 68 udp dport 67 accept\n",
            subnet->bridge_name);
        for (guint j = 0; subnet->attachments && j < subnet->attachments->len; j++) {
            PcvVpcPolicyAttachment *a = g_ptr_array_index(subnet->attachments, j);
            g_string_append_printf(s,
                "add rule bridge pcv_vpc_guard source_guard ibrname \"%s\" "
                "ether saddr %s ip saddr %s accept\n"
                "add rule bridge pcv_vpc_guard source_guard ibrname \"%s\" "
                "ether saddr %s ether type arp arp saddr ether %s "
                "arp saddr ip %s accept\n",
                subnet->bridge_name, a->mac_address, a->ip_address,
                subnet->bridge_name, a->mac_address, a->mac_address, a->ip_address);
        }
        g_string_append_printf(s,
            "add rule bridge pcv_vpc_guard source_guard ibrname \"%s\" ether type ip drop\n"
            "add rule bridge pcv_vpc_guard source_guard ibrname \"%s\" ether type arp drop\n"
            "add rule bridge pcv_vpc_guard source_guard ibrname \"%s\" ether type ip6 drop\n",
            subnet->bridge_name, subnet->bridge_name, subnet->bridge_name);
    }

                                                                               
                                                                         
                                                                
                                                                 
    for (guint i = 0; snapshot && i < snapshot->vpcs->len; i++) {
        PcvVpcPolicyVpc *vpc = g_ptr_array_index(snapshot->vpcs, i);
        if (g_strcmp0(vpc->backend, "ovn") == 0 && vpc->edge_interface)
            g_string_append_printf(s,
                "add rule inet pcv_vpc input iifname \"%s\" drop\n",
                vpc->edge_interface);
    }
}

static void
_append_publishes(GString *s, const PcvVpcPolicySnapshot *snapshot)
{
    for (guint i = 0; snapshot && snapshot->publishes && i < snapshot->publishes->len; i++) {
        PcvVpcPolicyPublish *p = g_ptr_array_index(snapshot->publishes, i);
        for (guint j = 0; p->allowed_sources && j < p->allowed_sources->len; j++) {
            const gchar *source = g_ptr_array_index(p->allowed_sources, j);
            g_string_append(s, "add rule inet pcv_vpc prerouting ");
            if (g_strcmp0(p->listen_address, "0.0.0.0") != 0)
                g_string_append_printf(s, "ip daddr %s ", p->listen_address);
            g_string_append_printf(s,
                "ip saddr %s %s dport %u dnat ip to %s:%u\n",
                source, p->protocol, p->listen_port, p->target_ip, p->target_port);
            g_string_append_printf(s,
                "add rule inet pcv_vpc forward oifname \"%s\" ip saddr %s "
                "ip daddr %s %s dport %u ct status dnat accept\n",
                p->target_bridge, source, p->target_ip, p->protocol, p->target_port);
        }
    }
}

static void
_append_forward_and_nat(GString *s, const PcvVpcPolicySnapshot *snapshot)
{
                                                                       
    for (guint i = 0; snapshot && snapshot->vpcs && i < snapshot->vpcs->len; i++) {
        PcvVpcPolicyVpc *v = g_ptr_array_index(snapshot->vpcs, i);
        if (g_strcmp0(v->backend, "linux") != 0)
            continue;
        for (guint a = 0; v->subnets && a < v->subnets->len; a++) {
            PcvVpcPolicySubnet *left = g_ptr_array_index(v->subnets, a);
            for (guint b = 0; b < v->subnets->len; b++) {
                if (a == b) continue;
                PcvVpcPolicySubnet *right = g_ptr_array_index(v->subnets, b);
                g_string_append_printf(s,
                    "add rule inet pcv_vpc forward iifname \"%s\" oifname \"%s\" accept\n",
                    left->bridge_name, right->bridge_name);
            }
        }
    }
    g_string_append(s,
        "add rule inet pcv_vpc forward iifname @managed_ifaces "
        "oifname @managed_ifaces drop\n");

    for (guint i = 0; snapshot && snapshot->vpcs && i < snapshot->vpcs->len; i++) {
        PcvVpcPolicyVpc *v = g_ptr_array_index(snapshot->vpcs, i);
        if (g_strcmp0(v->backend, "ovn") == 0) {
            if (!v->edge_interface)
                continue;
            if (g_strcmp0(v->egress_mode, "nat") == 0) {
                g_string_append_printf(s,
                    "add rule inet pcv_vpc forward iifname \"%s\" accept\n"
                    "add rule inet pcv_vpc forward oifname \"%s\" ct state established,related accept\n"
                    "add rule inet pcv_vpc forward oifname \"%s\" drop\n",
                    v->edge_interface, v->edge_interface, v->edge_interface);
                for (guint j = 0; v->subnets && j < v->subnets->len; j++) {
                    PcvVpcPolicySubnet *subnet = g_ptr_array_index(v->subnets, j);
                    g_string_append_printf(s,
                        "add rule inet pcv_vpc postrouting ip saddr %s "
                        "oifname != @managed_ifaces masquerade\n", subnet->cidr);
                }
            } else {
                g_string_append_printf(s,
                    "add rule inet pcv_vpc forward iifname \"%s\" drop\n"
                    "add rule inet pcv_vpc forward oifname \"%s\" drop\n",
                    v->edge_interface, v->edge_interface);
            }
            continue;
        }
        for (guint j = 0; v->subnets && j < v->subnets->len; j++) {
            PcvVpcPolicySubnet *subnet = g_ptr_array_index(v->subnets, j);
            if (g_strcmp0(v->egress_mode, "nat") == 0) {
                g_string_append_printf(s,
                    "add rule inet pcv_vpc forward iifname \"%s\" accept\n"
                    "add rule inet pcv_vpc forward oifname \"%s\" "
                    "ct state established,related accept\n"
                    "add rule inet pcv_vpc forward oifname \"%s\" drop\n"
                    "add rule inet pcv_vpc postrouting ip saddr %s "
                    "oifname != @managed_ifaces masquerade\n",
                    subnet->bridge_name, subnet->bridge_name,
                    subnet->bridge_name, subnet->cidr);
            } else {
                g_string_append_printf(s,
                    "add rule inet pcv_vpc forward iifname \"%s\" drop\n"
                    "add rule inet pcv_vpc forward oifname \"%s\" drop\n",
                    subnet->bridge_name, subnet->bridge_name);
            }
        }
    }
}

                           
                                                                   
                                                                                  
                                                                       
                                                                     
                                                                
                                                              
  
                       
                                                          
                                                            
                       
gchar *
pcv_vpc_policy_build_script(const PcvVpcPolicySnapshot *snapshot)
{
    g_autoptr(GPtrArray) subnets = _all_subnets(snapshot);
    GString *s = g_string_new(NULL);
    _append_base(s, subnets, snapshot);
    _append_host_and_guard(s, subnets, snapshot);
    _append_publishes(s, snapshot);
    _append_forward_and_nat(s, snapshot);
    return g_string_free(s, FALSE);
}

                           
                                                                    
                                                                          
                                                                   
                                                                       
                                                 
  
                       
                                                         
                                                          
                                             
gchar *
pcv_vpc_policy_build_quarantine_script(GPtrArray *managed_bridges)
{
    GString *s = g_string_new(
        "add table inet pcv_vpc_quarantine\n"
        "delete table inet pcv_vpc_quarantine\n"
        "add table inet pcv_vpc_quarantine\n"
        "add chain inet pcv_vpc_quarantine input "
        "{ type filter hook input priority -30; policy accept; }\n"
        "add chain inet pcv_vpc_quarantine forward "
        "{ type filter hook forward priority -30; policy accept; }\n");
    for (guint i = 0; managed_bridges && i < managed_bridges->len; i++) {
        const gchar *bridge = g_ptr_array_index(managed_bridges, i);
        g_string_append_printf(s,
            "add rule inet pcv_vpc_quarantine input iifname \"%s\" drop\n"
            "add rule inet pcv_vpc_quarantine forward iifname \"%s\" drop\n"
            "add rule inet pcv_vpc_quarantine forward oifname \"%s\" drop\n",
            bridge, bridge, bridge);
    }
    return g_string_free(s, FALSE);
}
