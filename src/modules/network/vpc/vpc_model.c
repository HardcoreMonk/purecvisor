   
                    
                                 
  
                           
                                                                           
                                                           
                                                                
                                                                    
                                                                      
                                                                     
                                                                     
                                                             
                                    
  
                       
                                                              
                                                            
                                                            
                                                       
                                                  
   
#include "vpc_model.h"

#include <arpa/inet.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdint.h>
#include <string.h>

                                                                       
                                                   
GQuark
pcv_vpc_error_quark(void)
{
    return g_quark_from_static_string("pcv-vpc-error-quark");
}

                           
                                                                  
                                                                   
                                                                       
                                                           
                             
  
                       
                                                   
                                                    
                    
gboolean
pcv_vpc_name_is_valid(const gchar *name)
{
    if (!name || !*name || strlen(name) > 63 || !g_ascii_isalnum(name[0]))
        return FALSE;

    for (const gchar *p = name; *p; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_' && *p != '.')
            return FALSE;
    }
    return TRUE;
}

gboolean
pcv_vpc_egress_mode_is_valid(const gchar *mode)
{
    return g_strcmp0(mode, "nat") == 0 || g_strcmp0(mode, "isolated") == 0;
}

gboolean
pcv_vpc_backend_is_valid(const gchar *backend)
{
                                                                      
                                                                    
                                                            
    return g_strcmp0(backend, "linux") == 0 || g_strcmp0(backend, "ovn") == 0;
}

gboolean
pcv_vpc_protocol_is_valid(const gchar *protocol)
{
    return g_strcmp0(protocol, "tcp") == 0 || g_strcmp0(protocol, "udp") == 0;
}

gboolean
pcv_vpc_port_is_valid(gint port)
{
    return port >= 1 && port <= 65535;
}

gchar *
pcv_vpc_dnsmasq_conf_arg(const gchar *conf_path)
{
                               
                                                                     
                                                       
                                                              
                                                 
      
                           
                                                       
                                                       
    g_return_val_if_fail(conf_path && *conf_path, NULL);
    return g_strdup_printf("--conf-file=%s", conf_path);
}

                           
                                                                                  
                                                               
                                                                  
                                                                 
                                                                
  
                       
                                                           
                                                          
gchar *
pcv_vpc_libvirt_metadata_payload(const gchar *metadata_xml, GError **error)
{
    g_return_val_if_fail(metadata_xml != NULL, NULL);

    xmlDocPtr source = xmlReadMemory(metadata_xml, (gint)strlen(metadata_xml),
                                     "vpc-metadata.xml", NULL,
                                     XML_PARSE_NONET | XML_PARSE_NOERROR |
                                     XML_PARSE_NOWARNING);
    xmlNodePtr source_root = source ? xmlDocGetRootElement(source) : NULL;
    if (!source_root || xmlStrcmp(source_root->name, BAD_CAST "vpc") != 0) {
        if (source)
            xmlFreeDoc(source);
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "VPC metadata root가 유효하지 않습니다");
        return NULL;
    }

    xmlDocPtr payload_doc = xmlNewDoc(BAD_CAST "1.0");
    xmlNodePtr payload_root = payload_doc
        ? xmlNewNode(NULL, BAD_CAST "vpc") : NULL;
    if (!payload_doc || !payload_root) {
        if (payload_doc)
            xmlFreeDoc(payload_doc);
        xmlFreeDoc(source);
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "VPC metadata payload 생성 실패");
        return NULL;
    }
    xmlDocSetRootElement(payload_doc, payload_root);

    xmlChar *vpc_id = xmlGetProp(source_root, BAD_CAST "id");
    if (vpc_id) {
        xmlNewProp(payload_root, BAD_CAST "id", vpc_id);
        xmlFree(vpc_id);
    }
    for (xmlNodePtr source_child = source_root->children;
         source_child; source_child = source_child->next) {
        if (source_child->type != XML_ELEMENT_NODE ||
            xmlStrcmp(source_child->name, BAD_CAST "attachment") != 0)
            continue;

        xmlNodePtr payload_child = xmlNewChild(
            payload_root, NULL, BAD_CAST "attachment", NULL);
        const gchar *attributes[] = { "id", "subnet", "ip", NULL };
        for (guint i = 0; attributes[i]; i++) {
            xmlChar *value = xmlGetProp(
                source_child, BAD_CAST attributes[i]);
            if (value) {
                xmlNewProp(payload_child, BAD_CAST attributes[i], value);
                xmlFree(value);
            }
        }
    }

    xmlBufferPtr buffer = xmlBufferCreate();
    if (buffer)
        xmlNodeDump(buffer, payload_doc, payload_root, 0, 0);
    gchar *payload = buffer
        ? g_strdup((const gchar *)xmlBufferContent(buffer)) : NULL;
    if (buffer)
        xmlBufferFree(buffer);
    xmlFreeDoc(payload_doc);
    xmlFreeDoc(source);
    if (!payload)
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "VPC metadata payload 직렬화 실패");
    return payload;
}

                           
                                                                          
                                                                
                                                                  
                                                                     
                                                          
  
                       
                                                                
                                                            
                
gchar *
pcv_vpc_interface_xml(const gchar *mac_address,
                      const gchar *bridge_name,
                      gint bridge_mtu)
{
    g_return_val_if_fail(mac_address && *mac_address, NULL);
    g_return_val_if_fail(bridge_name && *bridge_name, NULL);

    g_autofree gchar *safe_mac = g_markup_escape_text(mac_address, -1);
    g_autofree gchar *safe_bridge = g_markup_escape_text(bridge_name, -1);
    g_autofree gchar *mtu_xml = bridge_mtu > 0
        ? g_strdup_printf("<mtu size='%d'/>", bridge_mtu)
        : g_strdup("");
    return g_strdup_printf(
        "<interface type='bridge'><mac address='%s'/><source bridge='%s'/>"
        "<model type='virtio'/><driver name='vhost'/>%s</interface>",
        safe_mac, safe_bridge, mtu_xml);
}

gchar *
pcv_vpc_ovn_interface_xml(const gchar *mac_address,
                          const gchar *iface_id,
                          gint bridge_mtu)
{
    g_return_val_if_fail(mac_address && *mac_address, NULL);
    g_return_val_if_fail(iface_id && *iface_id, NULL);

                                                           
                                                                               
                                                            
    g_autofree gchar *safe_mac = g_markup_escape_text(mac_address, -1);
    g_autofree gchar *safe_iface_id = g_markup_escape_text(iface_id, -1);
    g_autofree gchar *mtu_xml = bridge_mtu > 0
        ? g_strdup_printf("<mtu size='%d'/>", bridge_mtu)
        : g_strdup("");
    return g_strdup_printf(
        "<interface type='bridge'><mac address='%s'/><source bridge='br-int'/>"
        "<virtualport type='openvswitch'><parameters interfaceid='%s'/></virtualport>"
        "<model type='virtio'/><driver name='vhost'/>%s</interface>",
        safe_mac, safe_iface_id, mtu_xml);
}

static gboolean
_parse_prefix(const gchar *text, gchar **address_out, guint8 *prefix_out)
{
    const gchar *slash = text ? strrchr(text, '/') : NULL;
    if (!slash || slash == text || !slash[1])
        return FALSE;

    gchar *end = NULL;
    guint64 prefix = g_ascii_strtoull(slash + 1, &end, 10);
    if (!end || *end != '\0' || prefix > 32)
        return FALSE;

    *address_out = g_strndup(text, (gsize)(slash - text));
    *prefix_out = (guint8)prefix;
    return TRUE;
}

                           
                                                                 
                                                                            
                                                                
                                                                    
                                                              
                                              
  
                       
                                                        
                                                             
                          
gboolean
pcv_vpc_cidr_parse(const gchar *text,
                   PcvVpcIpv4Cidr *out,
                   gchar **canonical_out,
                   GError **error)
{
    g_return_val_if_fail(out != NULL, FALSE);
    if (canonical_out)
        *canonical_out = NULL;

    g_autofree gchar *address = NULL;
    guint8 prefix = 0;
    struct in_addr parsed = {0};
    if (!_parse_prefix(text, &address, &prefix) ||
        inet_pton(AF_INET, address, &parsed) != 1) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "유효한 IPv4 CIDR이 아닙니다");
        return FALSE;
    }

    guint32 host_ip = ntohl(parsed.s_addr);
    guint32 mask = prefix == 0 ? 0U : UINT32_MAX << (32 - prefix);
    out->network = host_ip & mask;
    out->broadcast = out->network | ~mask;
    out->prefix = prefix;

    if (canonical_out) {
        g_autofree gchar *network_text = pcv_vpc_ipv4_to_string(out->network);
        *canonical_out = g_strdup_printf("%s/%u", network_text, out->prefix);
    }
    return TRUE;
}

                           
                                                      
                                                                       
                                                            
                                                               
                     
  
                       
                                                            
                                                              
gboolean
pcv_vpc_subnet_cidr_parse(const gchar *text,
                          PcvVpcIpv4Cidr *out,
                          gchar **canonical_out,
                          GError **error)
{
    g_autofree gchar *canonical = NULL;
    if (!pcv_vpc_cidr_parse(text, out, &canonical, error))
        return FALSE;
    if (out->prefix < 16 || out->prefix > 30 || g_strcmp0(text, canonical) != 0) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "subnet CIDR은 canonical IPv4 network /16~30이어야 합니다");
        return FALSE;
    }
    if (canonical_out)
        *canonical_out = g_steal_pointer(&canonical);
    return TRUE;
}

                           
                                                                 
                                                                         
                                                        
  
                       
                                                    
                                             
gboolean
pcv_vpc_cidr_overlaps(const PcvVpcIpv4Cidr *a, const PcvVpcIpv4Cidr *b)
{
    g_return_val_if_fail(a != NULL && b != NULL, FALSE);
    return a->network <= b->broadcast && b->network <= a->broadcast;
}

                           
                                                             
                                                                
                                                          
  
                       
                                                            
                                                     
gboolean
pcv_vpc_cidr_contains_ip(const PcvVpcIpv4Cidr *cidr, const gchar *ip)
{
    g_return_val_if_fail(cidr != NULL, FALSE);
    struct in_addr parsed = {0};
    if (!ip || inet_pton(AF_INET, ip, &parsed) != 1)
        return FALSE;
    guint32 value = ntohl(parsed.s_addr);
    return value >= cidr->network && value <= cidr->broadcast;
}

                           
                                                                   
                                                                         
                                                                 
                                                  
  
                       
                                                        
                                                             
gboolean
pcv_vpc_cidr_usable_range(const PcvVpcIpv4Cidr *cidr,
                          guint32 *first_out,
                          guint32 *last_out)
{
    g_return_val_if_fail(cidr != NULL, FALSE);
    if (cidr->prefix > 30 || cidr->broadcast - cidr->network < 3)
        return FALSE;
    if (first_out)
        *first_out = cidr->network + 1;
    if (last_out)
        *last_out = cidr->broadcast - 1;
    return TRUE;
}

static gint
_cidr_start_compare(gconstpointer left, gconstpointer right)
{
    const PcvVpcIpv4Cidr *a = left;
    const PcvVpcIpv4Cidr *b = right;
    if (a->network != b->network)
        return a->network < b->network ? -1 : 1;
    if (a->broadcast != b->broadcast)
        return a->broadcast < b->broadcast ? -1 : 1;
    return 0;
}

                           
                                                                     
                                                                
                                                             
                                                   
  
                       
                                                             
                                                                 
                                         
gboolean
pcv_vpc_sources_cover_all_ipv4(GPtrArray *sources)
{
    if (!sources || sources->len == 0)
        return FALSE;

    g_autoptr(GArray) ranges = g_array_sized_new(
        FALSE, FALSE, sizeof(PcvVpcIpv4Cidr), sources->len);
    for (guint i = 0; i < sources->len; i++) {
        PcvVpcIpv4Cidr parsed = {0};
        if (!pcv_vpc_cidr_parse(g_ptr_array_index(sources, i), &parsed, NULL, NULL))
            return FALSE;
        g_array_append_val(ranges, parsed);
    }
    g_array_sort(ranges, _cidr_start_compare);

    guint64 next = 0;
    for (guint i = 0; i < ranges->len; i++) {
        const PcvVpcIpv4Cidr *range = &g_array_index(ranges, PcvVpcIpv4Cidr, i);
        if ((guint64)range->network > next)
            return FALSE;
        guint64 after = (guint64)range->broadcast + 1;
        if (after > next)
            next = after;
        if (next > G_MAXUINT32)
            return TRUE;
    }
    return FALSE;
}

                           
                                                                       
                                                                        
                                 
  
                       
                                                             
                                          
gchar *
pcv_vpc_ipv4_to_string(guint32 host_order_ip)
{
    struct in_addr address = { .s_addr = htonl(host_order_ip) };
    gchar buffer[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &address, buffer, sizeof(buffer)))
        return NULL;
    return g_strdup(buffer);
}

                           
                                                                          
                                                               
                                                          
                                                                 
  
                       
                                                           
                                                             
gchar *
pcv_vpc_bridge_name_from_id(const gchar *subnet_id)
{
    if (!subnet_id || !*subnet_id)
        return NULL;
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                                              subnet_id, -1);
    return g_strdup_printf("pcvs%.11s", digest);
}

static gchar *
_vpc_ovn_name(const gchar *prefix, const gchar *id, guint digest_length)
{
    if (!prefix || !id || !*id)
        return NULL;
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id, -1);
    return g_strdup_printf("%s%.*s", prefix, (gint)digest_length, digest);
}

gchar *
pcv_vpc_ovn_router_name_from_id(const gchar *vpc_id)
{
    return _vpc_ovn_name("pcvv_lr_", vpc_id, 16);
}

gchar *
pcv_vpc_ovn_switch_name_from_id(const gchar *subnet_id)
{
    return _vpc_ovn_name("pcvv_ls_", subnet_id, 16);
}

gchar *
pcv_vpc_ovn_port_name_from_id(const gchar *attachment_id)
{
    if (!attachment_id || !*attachment_id)
        return NULL;

                                                                                
                                                                          
                                                               
                                                           
                                                                    
    if (g_uuid_string_is_valid(attachment_id))
        return g_ascii_strdown(attachment_id, -1);

    g_autofree gchar *digest = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256, attachment_id, -1);
    return g_strdup_printf("%.8s-%.4s-4%.3s-a%.3s-%.12s",
                           digest, digest + 8, digest + 12,
                           digest + 15, digest + 18);
}

gchar *
pcv_vpc_ovn_edge_switch_name_from_id(const gchar *vpc_id)
{
    return _vpc_ovn_name("pcvv_edge_", vpc_id, 16);
}

gchar *
pcv_vpc_ovn_edge_iface_name_from_id(const gchar *vpc_id)
{
                                                     
    return _vpc_ovn_name("pcve", vpc_id, 11);
}

                           
                                                                           
                                                                
                                                                       
                 
  
                       
                                                        
                                                           
                
gchar *
pcv_vpc_mac_from_id(const gchar *attachment_id)
{
    if (!attachment_id || !*attachment_id)
        return NULL;
    g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                                              attachment_id, -1);
    guint8 bytes[5] = {0};
    for (guint i = 0; i < G_N_ELEMENTS(bytes); i++) {
        gchar pair[3] = { digest[i * 2], digest[i * 2 + 1], '\0' };
        bytes[i] = (guint8)g_ascii_strtoull(pair, NULL, 16);
    }
                                                                           
    return g_strdup_printf("02:%02x:%02x:%02x:%02x:%02x",
                           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);
}
