                                                                             
                                    
                                                                            
  
                           
                                                                     
                                                                        
                                                                              
                                                                        
                                                                                                                                        
  
                                                                
                                            
                                                                                
#include "guest_ip.h"

#include <stdio.h>
#include <string.h>

#include <json-glib/json-glib.h>

                                                                        
#define GUEST_IP_ARP_FLAG_COMPLETE 0x2UL

                                                             
                                                
void
pcv_guest_iface_free(gpointer iface)
{
    PcvGuestIface *f = iface;
    if (!f)
        return;
    g_free(f->mac);
    if (f->ipv4)
        g_ptr_array_unref(f->ipv4);
    if (f->ipv6)
        g_ptr_array_unref(f->ipv6);
    g_free(f);
}

                                                                         
                                                       
static PcvGuestIface *
_parse_one_iface(JsonObject *obj)
{
    const gchar *name = json_object_has_member(obj, "name")
                            ? json_object_get_string_member_with_default(obj, "name", "") : "";
    if (g_strcmp0(name, "lo") == 0)
        return NULL;
    const gchar *mac = json_object_has_member(obj, "hardware-address")
                           ? json_object_get_string_member_with_default(obj, "hardware-address", NULL)
                           : NULL;
    if (!mac || !*mac)
        return NULL;

    PcvGuestIface *f = g_new0(PcvGuestIface, 1);
    f->mac  = g_ascii_strdown(mac, -1);
    f->ipv4 = g_ptr_array_new_with_free_func(g_free);
    f->ipv6 = g_ptr_array_new_with_free_func(g_free);

    JsonNode *addrs = json_object_get_member(obj, "ip-addresses");
    if (addrs && JSON_NODE_HOLDS_ARRAY(addrs)) {
        JsonArray *arr = json_node_get_array(addrs);
        for (guint i = 0; i < json_array_get_length(arr); i++) {
            JsonNode *n = json_array_get_element(arr, i);
            if (!n || !JSON_NODE_HOLDS_OBJECT(n))
                continue;
            JsonObject *a = json_node_get_object(n);
            const gchar *ip = json_object_get_string_member_with_default(a, "ip-address", NULL);
            const gchar *type = json_object_get_string_member_with_default(a, "ip-address-type", NULL);
            if (!ip || !*ip || !type)
                continue;
            if (g_strcmp0(type, "ipv4") == 0)
                g_ptr_array_add(f->ipv4, g_strdup(ip));
            else if (g_strcmp0(type, "ipv6") == 0)
                g_ptr_array_add(f->ipv6, g_strdup(ip));
        }
    }
    return f;
}

                                                                                 
                                                     
GPtrArray *
pcv_guest_ip_parse_qga_interfaces(const gchar *qga_json, GError **error)
{
    if (!qga_json || !*qga_json) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "empty QGA response");
        return NULL;
    }
                                                
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, qga_json, -1, error)) {
        g_object_unref(parser);
        return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    JsonNode *ret = NULL;
    if (root && JSON_NODE_HOLDS_OBJECT(root))
        ret = json_object_get_member(json_node_get_object(root), "return");
    if (!ret || !JSON_NODE_HOLDS_ARRAY(ret)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "QGA response has no \"return\" array");
        g_object_unref(parser);
        return NULL;
    }

    GPtrArray *out = g_ptr_array_new_with_free_func(pcv_guest_iface_free);
    JsonArray *arr = json_node_get_array(ret);
    for (guint i = 0; i < json_array_get_length(arr); i++) {
        JsonNode *n = json_array_get_element(arr, i);
        if (!n || !JSON_NODE_HOLDS_OBJECT(n))
            continue;
        PcvGuestIface *f = _parse_one_iface(json_node_get_object(n));
        if (f)
            g_ptr_array_add(out, f);
    }
    g_object_unref(parser);
    return out;
}

                                                                                                      
                                                                         
                                                                            
                                                                          
                                                                     
                                                                               
GPtrArray *
pcv_guest_ip_from_virt_ifaces(virDomainInterfacePtr *ifaces, int count)
{
    if (!ifaces || count <= 0)
        return NULL;
    GPtrArray *out = g_ptr_array_new_with_free_func(pcv_guest_iface_free);
    for (int i = 0; i < count; i++) {
        const virDomainInterface *vi = ifaces[i];
        if (!vi || g_strcmp0(vi->name, "lo") == 0)
            continue;
        PcvGuestIface *f = g_new0(PcvGuestIface, 1);
        f->mac  = (vi->hwaddr && *vi->hwaddr) ? g_ascii_strdown(vi->hwaddr, -1) : NULL;
        f->ipv4 = g_ptr_array_new_with_free_func(g_free);
        f->ipv6 = g_ptr_array_new_with_free_func(g_free);
        for (unsigned int a = 0; vi->addrs && a < vi->naddrs; a++) {
            const virDomainIPAddress *ad = &vi->addrs[a];
            if (!ad->addr || !*ad->addr)
                continue;                      
            g_ptr_array_add(ad->type == VIR_IP_ADDR_TYPE_IPV4 ? f->ipv4 : f->ipv6, g_strdup(ad->addr));
        }
        g_ptr_array_add(out, f);
    }
    if (out->len == 0) {
        g_ptr_array_unref(out);
        return NULL;
    }
    return out;
}

                                                              
                                                                    
                                   
static gchar *
_ifaces_pick(const GPtrArray *ifaces, const gchar *mac, PcvGuestIpRule rule)
{
    if (!ifaces)
        return NULL;
    for (guint i = 0; i < ifaces->len; i++) {
        const PcvGuestIface *f = g_ptr_array_index((GPtrArray *)ifaces, i);
        if (!f)
            continue;
        if (rule == PCV_GUEST_IP_ANY_FIRST_IPV4) {
            if (f->ipv4 && f->ipv4->len > 0)
                return g_strdup(g_ptr_array_index(f->ipv4, 0));
            continue;
        }
        if (!mac || !f->mac || g_ascii_strcasecmp(f->mac, mac) != 0)
            continue;
        if (f->ipv4 && f->ipv4->len > 0)
            return g_strdup(g_ptr_array_index(f->ipv4, 0));
        if (f->ipv6 && f->ipv6->len > 0)
            return g_strdup(g_ptr_array_index(f->ipv6, 0));
    }
    return NULL;
}

                                                                              
                                                               
gchar *
pcv_guest_ip_lease_file_lookup(const gchar *lease_text, const gchar *mac)
{
    if (!lease_text || !mac || !*mac)
        return NULL;
    gchar *result = NULL;
    gchar **lines = g_strsplit(lease_text, "\n", -1);
    for (guint i = 0; lines[i] && !result; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!*line)
            continue;
        gchar expiry[32] = {0}, lease_mac[32] = {0}, ip[64] = {0};
                                                          
        if (sscanf(line, "%31s %31s %63s", expiry, lease_mac, ip) == 3 &&
            g_ascii_strcasecmp(lease_mac, mac) == 0)
            result = g_strdup(ip);
    }
    g_strfreev(lines);
    return result;
}

                                                                      
                                                                        
                                                                 
gboolean
pcv_guest_ip_arp_lookup(const gchar *proc_net_arp, const gchar *mac,
                        gboolean require_complete, gchar *ip_out, gsize ip_out_len)
{
    if (!proc_net_arp || !mac || !*mac || !ip_out || ip_out_len == 0)
        return FALSE;

    gboolean found = FALSE;
    gchar **lines = g_strsplit(proc_net_arp, "\n", -1);
    for (gsize i = 0; lines[i] && !found; i++) {
        char ip[64] = {0}, hwtype[16] = {0}, flags[16] = {0};
        char hwaddr[32] = {0}, mask[32] = {0}, dev[32] = {0};
                                                      
        if (sscanf(lines[i], "%63s %15s %15s %31s %31s %31s",
                   ip, hwtype, flags, hwaddr, mask, dev) != 6)
            continue;
        if (!g_hostname_is_ip_address(ip) || strchr(ip, ':'))
            continue;                                         
        if (require_complete) {
            gchar *end = NULL;
            guint64 bits = g_ascii_strtoull(flags, &end, 16);
            if (!end || end == flags || (bits & GUEST_IP_ARP_FLAG_COMPLETE) == 0)
                continue;                                           
        }
        if (g_ascii_strcasecmp(hwaddr, mac) != 0)
            continue;
        if (strlen(ip) >= ip_out_len)
            break;                                    
        g_strlcpy(ip_out, ip, ip_out_len);
        found = TRUE;
    }
    g_strfreev(lines);
    return found;
}

                                                                                
                                                                    
                                                                         
                                                               
gchar *
pcv_guest_ip_resolve(const PcvGuestIpInputs *in, const gchar *mac,
                     PcvGuestIpRule rule, const gchar **source_out)
{
    const gchar *src = "";
    gchar *ip = NULL;

    if (in) {
        if ((ip = _ifaces_pick(in->lease_ifaces, mac, rule))) {
            src = "lease";
        } else if ((ip = _ifaces_pick(in->agent_ifaces, mac, rule))) {
            src = "guest-agent";
        } else if ((ip = pcv_guest_ip_lease_file_lookup(in->lease_file_text, mac))) {
            src = "lease-file";
        } else if (in->proc_net_arp_text && mac && *mac) {
            gchar buf[64] = {0};
            if (pcv_guest_ip_arp_lookup(in->proc_net_arp_text, mac, in->arp_require_complete,
                                        buf, sizeof(buf))) {
                ip = g_strdup(buf);
                src = "arp";
            }
        }
    }
    if (source_out)
        *source_out = src;
    return ip;
}

                                                                  
                                             
gboolean
pcv_guest_agent_slot_fresh(const PcvGuestAgentSlot *s, gint64 now_us)
{
    return s && s->state == PCV_GUEST_AGENT_OK && s->fetched_us > 0 &&
           now_us < s->fetched_us + PCV_GUEST_AGENT_TTL_US;
}

                                                        
                                                  
gboolean
pcv_guest_agent_slot_should_probe(const PcvGuestAgentSlot *s, gint64 now_us)
{
    if (!s || s->inflight)
        return FALSE;
    if (pcv_guest_agent_slot_fresh(s, now_us))
        return FALSE;
    return now_us >= s->next_try_us;
}

                                                      
                                               
void
pcv_guest_agent_slot_on_success(PcvGuestAgentSlot *s, gint64 now_us)
{
    if (!s)
        return;
    s->fetched_us  = now_us;
    s->next_try_us = 0;
    s->backoff_s   = 0;
    s->state       = PCV_GUEST_AGENT_OK;
    s->inflight    = FALSE;
}

                                                                           
                                                     
gint
pcv_guest_agent_slot_on_failure(PcvGuestAgentSlot *s, gint64 now_us)
{
    if (!s)
        return 0;
    gint next = s->backoff_s <= 0 ? PCV_GUEST_AGENT_BACKOFF_MIN_S
                                  : s->backoff_s * 2;
    if (next > PCV_GUEST_AGENT_BACKOFF_MAX_S)
        next = PCV_GUEST_AGENT_BACKOFF_MAX_S;
    s->backoff_s   = next;
    s->next_try_us = now_us + (gint64)next * G_USEC_PER_SEC;
    s->state       = PCV_GUEST_AGENT_UNRESPONSIVE;
    s->inflight    = FALSE;
    return next;
}
