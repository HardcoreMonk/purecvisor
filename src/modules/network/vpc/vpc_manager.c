   
                      
                                                                                     
  
                       
                                                             
                                                             
  
              
                                                                               
                                                                          
                                                                                
                                                           
  
                 
                                                               
                                                                  
                                                                     
                                                                   
                                                              
  
                                         
                                                                               
                                                               
                                                                        
                                                           
                                                                  
                                                                   
                        
  
                               
                                                                      
                                                                         
                                                                         
                                                                        
                                                           
                                
  
                              
                                                              
                                                                   
                                                                 
                                                            
                               
  
                              
                                                                         
                                                                
                                                                
                                                            
                                                                      
  
                                  
                                                                        
                                                                         
                                                                     
                                                             
                                                            
  
                                  
                                                                          
                                                             
                                                           
                                                                             
                                                
  
                                    
                                                                           
                                                                               
                                                                                    
                                                                     
                                                               
                                 
  
                                    
                                                                                    
                                                                             
                                                                             
                                                               
                                                     
  
                                 
                                                                       
                                                                  
                                                                        
                                                                       
                                                                       
                
  
                          
                                                               
                                                                  
                                                                
                                                          
                                                            
  
                
                                                                   
                                                                      
                                                           
                                                                
                                            
  
                 
                                                     
                                                                     
                                                                  
                                                              
                              
  
         
                                                                     
                                                                                     
                                                                        
                                                     
                                                            
                                                                  
  
                       
                                                          
                                                   
                                               
   
#include "vpc_manager.h"

#include "vpc_model.h"
#include "vpc_backend_ovn.h"
#include "vpc_policy_nft.h"
#include "vpc_store.h"
#include "modules/network/network_dhcp.h"
#include "modules/network/network_manager.h"
#include "modules/network/security_group.h"
#include "modules/network/tenant_overlay.h"
#include "modules/virt/virt_conn_pool.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_config.h"
#include "utils/pcv_validate.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <glib/gstdio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <net/if.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

#define PCV_VPC_METADATA_URI "urn:purecvisor:vpc"

static PcvVpcStore *g_store;
static GMutex g_lifecycle_mu;
static GMutex g_mutation_mu;

static const gchar *
_ovn_transit_pool(void)
{
    return pcv_config_get_string("ovn", "edge_transit_pool", "100.64.0.0/16");
}

static PcvVpcStore *
_store_ref(GError **error)
{
    g_mutex_lock(&g_lifecycle_mu);
    PcvVpcStore *store = g_store;
    if (!store)
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                    "VPC subsystem이 초기화되지 않았습니다");
    g_mutex_unlock(&g_lifecycle_mu);
    return store;
}

static gboolean
_nft_apply(const gchar *script, GError **error)
{
    const gchar *check_argv[] = { "nft", "-c", "-f", "-", NULL };
    g_autofree gchar *stderr_text = NULL;
    if (!pcv_spawn_sync_stdin(check_argv, script, -1, NULL, &stderr_text, error)) {
        if (error && !*error)
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                        "nft policy validation 실패: %s", stderr_text ? stderr_text : "unknown");
        return FALSE;
    }
    const gchar *apply_argv[] = { "nft", "-f", "-", NULL };
    g_clear_pointer(&stderr_text, g_free);
    if (!pcv_spawn_sync_stdin(apply_argv, script, -1, NULL, &stderr_text, error)) {
        if (error && !*error)
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                        "nft policy apply 실패: %s", stderr_text ? stderr_text : "unknown");
        return FALSE;
    }
    return TRUE;
}

static gboolean
_apply_quarantine(PcvVpcStore *store, GError **error)
{
    g_autoptr(GPtrArray) bridges = pcv_vpc_store_list_managed_bridges(store, error);
    if (!bridges) return FALSE;
    if (bridges->len > 0) {
        g_autofree gchar *script = pcv_vpc_policy_build_quarantine_script(bridges);
        if (!_nft_apply(script, error)) return FALSE;
    }
    g_autoptr(JsonArray) vpcs = pcv_vpc_store_list_vpcs(store, NULL, error);
    if (!vpcs) return FALSE;
    for (guint i = 0; i < json_array_get_length(vpcs); i++) {
        JsonObject *vpc = json_array_get_object_element(vpcs, i);
        if (g_strcmp0(json_object_get_string_member(vpc, "backend"), "ovn") == 0 &&
            g_strcmp0(json_object_get_string_member(vpc, "state"), "CREATING") != 0 &&
            !pcv_vpc_ovn_set_quarantine(vpc, TRUE, error))
            return FALSE;
    }
    return TRUE;
}

static gboolean
_apply_full_policy(PcvVpcStore *store, GError **error)
{
    g_autoptr(PcvVpcPolicySnapshot) snapshot = pcv_vpc_store_policy_snapshot(store, error);
    if (!snapshot) return FALSE;
    g_autofree gchar *script = pcv_vpc_policy_build_script(snapshot);
    return _nft_apply(script, error);
}

static gboolean
_clear_ovn_quarantine(PcvVpcStore *store, GError **error)
{
    g_autoptr(JsonArray) vpcs = pcv_vpc_store_list_vpcs(store, NULL, error);
    if (!vpcs) return FALSE;
    for (guint i = 0; i < json_array_get_length(vpcs); i++) {
        JsonObject *vpc = json_array_get_object_element(vpcs, i);
        if (g_strcmp0(json_object_get_string_member(vpc, "backend"), "ovn") == 0) {
            g_autoptr(JsonArray) attachments = pcv_vpc_store_list_attachments(
                store, json_object_get_string_member(vpc, "id"), NULL, error);
            if (!attachments || !pcv_vpc_ovn_sync_policy(vpc, attachments, FALSE, error))
                return FALSE;
        }
    }
    return TRUE;
}

static void
_managed_bridges_down(PcvVpcStore *store)
{
    g_autoptr(GPtrArray) bridges = pcv_vpc_store_list_managed_bridges(store, NULL);
    for (guint i = 0; bridges && i < bridges->len; i++) {
        const gchar *bridge = g_ptr_array_index(bridges, i);
        const gchar *argv[] = { "ip", "link", "set", "dev", bridge, "down", NULL };
        pcv_spawn_sync(argv, NULL, NULL, NULL);
    }
}

static gboolean
_bridge_exists(const gchar *bridge)
{
    return bridge && if_nametoindex(bridge) != 0;
}

static gboolean
_host_cidr_overlaps_output(const gchar *output, const PcvVpcIpv4Cidr *candidate)
{
    if (!output) return FALSE;
    g_auto(GStrv) tokens = g_strsplit_set(output, " \t\r\n", -1);
    for (guint i = 0; tokens[i]; i++) {
        if (!strchr(tokens[i], '/') || g_strcmp0(tokens[i], "0.0.0.0/0") == 0)
            continue;
        PcvVpcIpv4Cidr actual = {0};
        if (pcv_vpc_cidr_parse(tokens[i], &actual, NULL, NULL) &&
            pcv_vpc_cidr_overlaps(&actual, candidate))
            return TRUE;
    }
    return FALSE;
}

static gboolean
_host_cidr_available(const gchar *cidr, GError **error)
{
    if (!pcv_validate_private_cidr(cidr)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "subnet CIDR은 RFC1918 사설 IPv4 대역이어야 합니다");
        return FALSE;
    }
    PcvVpcIpv4Cidr candidate = {0};
    if (!pcv_vpc_subnet_cidr_parse(cidr, &candidate, NULL, error))
        return FALSE;
    const gchar *addr_argv[] = { "ip", "-4", "-o", "addr", "show", NULL };
    const gchar *route_argv[] = { "ip", "-4", "route", "show", "table", "all", NULL };
    g_autofree gchar *addresses = NULL;
    g_autofree gchar *routes = NULL;
    if (!pcv_spawn_sync(addr_argv, &addresses, NULL, error) ||
        !pcv_spawn_sync(route_argv, &routes, NULL, error))
        return FALSE;
    if (_host_cidr_overlaps_output(addresses, &candidate) ||
        _host_cidr_overlaps_output(routes, &candidate)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "host connected/on-link CIDR과 중첩됩니다: %s", cidr);
        return FALSE;
    }
    return TRUE;
}

static gchar *
_gateway_cidr(JsonObject *subnet)
{
    const gchar *cidr = json_object_get_string_member(subnet, "cidr");
    const gchar *slash = strrchr(cidr, '/');
    return g_strdup_printf("%s%s", json_object_get_string_member(subnet, "gateway"), slash);
}

static gboolean
_restart_dhcp(PcvVpcStore *store, JsonObject *subnet, GError **error)
{
                                                                             
                                                                    
                                                                          
    if (g_strcmp0(json_object_get_string_member_with_default(
            subnet, "backend", "linux"), "ovn") == 0)
        return TRUE;
    const gchar *bridge = json_object_get_string_member(subnet, "bridge_name");
    const gchar *vpc_id = json_object_get_string_member(subnet, "vpc_id");
    const gchar *subnet_id = json_object_get_string_member(subnet, "id");
    const gchar *cidr = json_object_get_string_member(subnet, "cidr");
    const gchar *slash = strrchr(cidr, '/');
    if (!slash) return FALSE;
    gint prefix = (gint)g_ascii_strtoll(slash + 1, NULL, 10);
    guint32 mask = prefix == 0 ? 0 : G_MAXUINT32 << (32 - prefix);
    g_autofree gchar *netmask = pcv_vpc_ipv4_to_string(mask);
    g_autofree gchar *conf_path = g_strdup_printf(
        PCV_NETWORK_RUNDIR "/dnsmasq-%s.conf", bridge);
    g_autofree gchar *pid_path = g_strdup_printf(
        PCV_NETWORK_RUNDIR "/dnsmasq-%s.pid", bridge);
    g_autofree gchar *lease_path = g_strdup_printf(
        PCV_NETWORK_RUNDIR "/dnsmasq-%s.leases", bridge);

    GString *conf = g_string_new(NULL);
    g_string_append_printf(conf,
        "bind-interfaces\ninterface=%s\nexcept-interface=lo\n"
        "pid-file=%s\ndhcp-leasefile=%s\n"
        "dhcp-range=%s,%s,%s,12h\n"
        "dhcp-option=option:router,%s\n"
        "dhcp-option=option:dns-server,%s\n",
        bridge, pid_path, lease_path,
        json_object_get_string_member(subnet, "allocation_start"),
        json_object_get_string_member(subnet, "allocation_end"), netmask,
        json_object_get_string_member(subnet, "gateway"),
        json_object_get_string_member(subnet, "gateway"));
    g_autoptr(JsonArray) attachments = pcv_vpc_store_list_attachments(store, vpc_id, NULL, error);
    if (!attachments) { g_string_free(conf, TRUE); return FALSE; }
    for (guint i = 0; i < json_array_get_length(attachments); i++) {
        JsonObject *a = json_array_get_object_element(attachments, i);
        if (g_strcmp0(json_object_get_string_member(a, "subnet_id"), subnet_id) != 0 ||
            g_strcmp0(json_object_get_string_member(a, "state"), "ACTIVE") != 0)
            continue;
        g_string_append_printf(conf, "dhcp-host=%s,%s,infinite\n",
            json_object_get_string_member(a, "mac_address"),
            json_object_get_string_member(a, "ip_address"));
    }
    gboolean ok = network_dhcp_stop(bridge, error);
    if (ok) ok = g_file_set_contents(conf_path, conf->str, -1, error);
    if (ok && g_chmod(conf_path, 0600) != 0) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "dnsmasq 설정 권한 변경 실패: %s", g_strerror(errno));
        ok = FALSE;
    }
    if (ok) {
                          
                                                                   
                                                                 
                                                                    
                                               
          
                         
                                                                 
                                                                   
           
        g_autofree gchar *conf_arg = pcv_vpc_dnsmasq_conf_arg(conf_path);
        const gchar *argv[] = { "dnsmasq", conf_arg, NULL };
        ok = pcv_spawn_sync(argv, NULL, NULL, error);
    }
    g_string_free(conf, TRUE);
    return ok;
}

static gboolean
_listener_address_is_local(const gchar *address)
{
    if (g_strcmp0(address, "0.0.0.0") == 0) return TRUE;
    struct ifaddrs *ifaces = NULL;
    if (getifaddrs(&ifaces) != 0) return FALSE;
    gboolean found = FALSE;
    for (struct ifaddrs *it = ifaces; it && !found; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        gchar text[INET_ADDRSTRLEN] = {0};
        struct sockaddr_in *sin = (struct sockaddr_in *)it->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) &&
            g_strcmp0(text, address) == 0) found = TRUE;
    }
    freeifaddrs(ifaces);
    return found;
}

static gboolean
_listener_port_is_free(const gchar *protocol,
                       const gchar *address,
                       gint port,
                       GError **error)
{
    gint type = g_strcmp0(protocol, "tcp") == 0 ? SOCK_STREAM : SOCK_DGRAM;
    gint fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "port probe socket 실패: %s", g_strerror(errno)); return FALSE;
    }
    struct sockaddr_in bind_addr = { .sin_family = AF_INET, .sin_port = htons((guint16)port) };
    inet_pton(AF_INET, address, &bind_addr.sin_addr);
    gboolean free_port = bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0;
    if (!free_port)
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "host listener와 충돌합니다: %s/%s:%d", protocol, address, port);
    close(fd);
    return free_port;
}

static gchar *
_xml_owner(const gchar *xml)
{
    xmlDocPtr doc = xmlReadMemory(xml, (gint)strlen(xml), "vpc-owner.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return NULL;
    gchar *owner = NULL;
    GQueue queue = G_QUEUE_INIT;
    g_queue_push_tail(&queue, xmlDocGetRootElement(doc));
    while (!g_queue_is_empty(&queue) && !owner) {
        xmlNodePtr node = g_queue_pop_head(&queue);
        for (xmlNodePtr cur = node; cur; cur = cur->next) {
            if (cur->type == XML_ELEMENT_NODE && xmlStrcmp(cur->name, BAD_CAST "owner") == 0) {
                xmlChar *content = xmlNodeGetContent(cur);
                owner = content ? g_strdup((const gchar *)content) : NULL;
                if (content) xmlFree(content);
                break;
            }
            if (cur->children) g_queue_push_tail(&queue, cur->children);
        }
    }
    xmlFreeDoc(doc);
    if (owner) g_strstrip(owner);
    return owner;
}

static gboolean
_metadata_add_attachment(virDomainPtr dom,
                         const gchar *vpc_id,
                         const gchar *subnet_id,
                         const gchar *attachment_id,
                         const gchar *ip,
                         GError **error)
{
    char *old = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                     PCV_VPC_METADATA_URI, VIR_DOMAIN_AFFECT_CONFIG);
    xmlDocPtr doc = old ? xmlReadMemory(old, (gint)strlen(old), "vpc.xml", NULL,
                                        XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING)
                        : xmlNewDoc(BAD_CAST "1.0");
    if (old) free(old);
    xmlNodePtr root = doc ? xmlDocGetRootElement(doc) : NULL;
    if (!root && doc) {
        root = xmlNewNode(NULL, BAD_CAST "vpc");
        xmlNsPtr ns = xmlNewNs(root, BAD_CAST PCV_VPC_METADATA_URI, BAD_CAST "pcv");
        xmlSetNs(root, ns); xmlDocSetRootElement(doc, root);
        xmlNewProp(root, BAD_CAST "id", BAD_CAST vpc_id);
    }
    xmlChar *current_vpc = root ? xmlGetProp(root, BAD_CAST "id") : NULL;
    if (!doc || !root || (current_vpc && xmlStrcmp(current_vpc, BAD_CAST vpc_id) != 0)) {
        if (current_vpc)
            xmlFree(current_vpc);
        if (doc)
            xmlFreeDoc(doc);
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "VM의 기존 VPC metadata가 다른 VPC를 가리킵니다"); return FALSE;
    }
    if (current_vpc) xmlFree(current_vpc);
    for (xmlNodePtr cur = root->children; cur; cur = cur->next) {
        if (cur->type != XML_ELEMENT_NODE ||
            xmlStrcmp(cur->name, BAD_CAST "attachment") != 0)
            continue;
        xmlChar *existing_id = xmlGetProp(cur, BAD_CAST "id");
        gboolean same_id = existing_id &&
            xmlStrcmp(existing_id, BAD_CAST attachment_id) == 0;
        if (existing_id) xmlFree(existing_id);
        if (!same_id) continue;
        xmlChar *existing_subnet = xmlGetProp(cur, BAD_CAST "subnet");
        xmlChar *existing_ip = xmlGetProp(cur, BAD_CAST "ip");
        gboolean exact = existing_subnet && existing_ip &&
            xmlStrcmp(existing_subnet, BAD_CAST subnet_id) == 0 &&
            xmlStrcmp(existing_ip, BAD_CAST ip) == 0;
        if (existing_subnet) xmlFree(existing_subnet);
        if (existing_ip) xmlFree(existing_ip);
        xmlFreeDoc(doc);
        if (exact) return TRUE;
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "같은 attachment ID의 libvirt metadata가 DB와 다릅니다");
        return FALSE;
    }
    xmlNodePtr child = xmlNewChild(root, root->ns, BAD_CAST "attachment", NULL);
    xmlNewProp(child, BAD_CAST "id", BAD_CAST attachment_id);
    xmlNewProp(child, BAD_CAST "subnet", BAD_CAST subnet_id);
    xmlNewProp(child, BAD_CAST "ip", BAD_CAST ip);
    xmlBufferPtr buffer = xmlBufferCreate();
    xmlNodeDump(buffer, doc, root, 0, 0);
                      
                                                                     
                                                                   
                                                             
                                                                     
      
                     
                                                                
                                                                           
       
    g_autofree gchar *payload = pcv_vpc_libvirt_metadata_payload(
        (const gchar *)xmlBufferContent(buffer), error);
    if (!payload) {
        xmlBufferFree(buffer);
        xmlFreeDoc(doc);
        return FALSE;
    }
    gint rc = virDomainSetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                   payload,
                                   "pcv", PCV_VPC_METADATA_URI,
                                   VIR_DOMAIN_AFFECT_CONFIG);
    xmlBufferFree(buffer); xmlFreeDoc(doc);
    if (rc < 0) {
        virErrorPtr e = virGetLastError();
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "VPC metadata 기록 실패: %s", e ? e->message : "unknown"); return FALSE;
    }
    return TRUE;
}

typedef enum {
    PCV_VPC_NIC_ERROR = -2,
    PCV_VPC_NIC_MISMATCH = -1,
    PCV_VPC_NIC_MISSING = 0,
    PCV_VPC_NIC_MATCH = 1,
} PcvVpcNicMatch;

                                                          
                                                               
static PcvVpcNicMatch
_domain_nic_match(virDomainPtr dom,
                  guint xml_flags,
                  const gchar *mac_address,
                  const gchar *bridge_name,
                  const gchar *interface_id,
                  GError **error)
{
    char *xml = virDomainGetXMLDesc(dom, xml_flags);
    if (!xml) {
        virErrorPtr e = virGetLastError();
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "VM NIC XML 조회 실패: %s", e ? e->message : "unknown");
        return PCV_VPC_NIC_ERROR;
    }
    xmlDocPtr doc = xmlReadMemory(xml, (gint)strlen(xml), "vpc-domain.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    free(xml);
    if (!doc) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO, "VM NIC XML 파싱 실패");
        return PCV_VPC_NIC_ERROR;
    }
    PcvVpcNicMatch result = PCV_VPC_NIC_MISSING;
    GQueue queue = G_QUEUE_INIT;
    g_queue_push_tail(&queue, xmlDocGetRootElement(doc));
    while (!g_queue_is_empty(&queue) && result == PCV_VPC_NIC_MISSING) {
        xmlNodePtr node = g_queue_pop_head(&queue);
        for (xmlNodePtr cur = node; cur; cur = cur->next) {
            if (cur->type == XML_ELEMENT_NODE &&
                xmlStrcmp(cur->name, BAD_CAST "interface") == 0) {
                xmlChar *mac = NULL; xmlChar *bridge = NULL; xmlChar *actual_interface_id = NULL;
                for (xmlNodePtr child = cur->children; child; child = child->next) {
                    if (child->type != XML_ELEMENT_NODE) continue;
                    if (xmlStrcmp(child->name, BAD_CAST "mac") == 0)
                        mac = xmlGetProp(child, BAD_CAST "address");
                    else if (xmlStrcmp(child->name, BAD_CAST "source") == 0)
                        bridge = xmlGetProp(child, BAD_CAST "bridge");
                    else if (xmlStrcmp(child->name, BAD_CAST "virtualport") == 0)
                        for (xmlNodePtr parameter = child->children; parameter;
                             parameter = parameter->next)
                            if (parameter->type == XML_ELEMENT_NODE &&
                                xmlStrcmp(parameter->name, BAD_CAST "parameters") == 0) {
                                actual_interface_id = xmlGetProp(
                                    parameter, BAD_CAST "interfaceid");
                                break;
                            }
                }
                if (mac && g_ascii_strcasecmp((const gchar *)mac, mac_address) == 0) {
                    gboolean bridge_match = bridge &&
                        g_strcmp0((const gchar *)bridge, bridge_name) == 0;
                    gboolean interface_match = !interface_id ||
                        (actual_interface_id && g_strcmp0(
                            (const gchar *)actual_interface_id, interface_id) == 0);
                    result = bridge_match && interface_match
                        ? PCV_VPC_NIC_MATCH : PCV_VPC_NIC_MISMATCH;
                }
                if (mac) xmlFree(mac);
                if (bridge) xmlFree(bridge);
                if (actual_interface_id) xmlFree(actual_interface_id);
                if (result != PCV_VPC_NIC_MISSING) break;
            }
            if (cur->children) g_queue_push_tail(&queue, cur->children);
        }
    }
    xmlFreeDoc(doc);
    return result;
}

static gchar *
_attachment_interface_xml(JsonObject *attachment)
{
    if (g_strcmp0(json_object_get_string_member_with_default(
            attachment, "backend", "linux"), "ovn") == 0) {
        g_autofree gchar *iface_id = pcv_vpc_ovn_port_name_from_id(
            json_object_get_string_member(attachment, "id"));
        return pcv_vpc_ovn_interface_xml(
            json_object_get_string_member(attachment, "mac_address"), iface_id, 0);
    }
    const gchar *bridge = json_object_get_string_member(attachment, "bridge_name");
    gint bridge_mtu = pcv_bridge_mtu_read(bridge, NULL);
    return pcv_vpc_interface_xml(
        json_object_get_string_member(attachment, "mac_address"),
        bridge, bridge_mtu);
}

static const gchar *
_attachment_bridge(JsonObject *attachment)
{
    return g_strcmp0(json_object_get_string_member_with_default(
        attachment, "backend", "linux"), "ovn") == 0
        ? "br-int" : json_object_get_string_member(attachment, "bridge_name");
}

static gboolean
_metadata_remove_attachment(virDomainPtr dom, const gchar *attachment_id, GError **error)
{
    char *old = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                     PCV_VPC_METADATA_URI, VIR_DOMAIN_AFFECT_CONFIG);
    if (!old) { virResetLastError(); return TRUE; }
    xmlDocPtr doc = xmlReadMemory(old, (gint)strlen(old), "vpc.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    free(old);
    if (!doc) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO, "VPC metadata 파싱 실패"); return FALSE;
    }
    xmlNodePtr root = xmlDocGetRootElement(doc);
    for (xmlNodePtr cur = root ? root->children : NULL; cur; cur = cur->next) {
        xmlChar *id = xmlGetProp(cur, BAD_CAST "id");
        gboolean match = id && xmlStrcmp(id, BAD_CAST attachment_id) == 0;
        if (id) xmlFree(id);
        if (match) { xmlUnlinkNode(cur); xmlFreeNode(cur); break; }
    }
    gboolean has_attachment = FALSE;
    for (xmlNodePtr cur = root ? root->children : NULL; cur; cur = cur->next)
        if (cur->type == XML_ELEMENT_NODE) { has_attachment = TRUE; break; }
    gint rc;
    if (!has_attachment) {
        rc = virDomainSetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT, NULL,
                                  "pcv", PCV_VPC_METADATA_URI,
                                  VIR_DOMAIN_AFFECT_CONFIG);
    } else {
        xmlBufferPtr buffer = xmlBufferCreate();
        xmlNodeDump(buffer, doc, root, 0, 0);
        g_autofree gchar *payload = pcv_vpc_libvirt_metadata_payload(
            (const gchar *)xmlBufferContent(buffer), error);
        if (payload) {
            rc = virDomainSetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                      payload, "pcv", PCV_VPC_METADATA_URI,
                                      VIR_DOMAIN_AFFECT_CONFIG);
        } else {
            rc = -1;
        }
        xmlBufferFree(buffer);
    }
    xmlFreeDoc(doc);
    if (rc < 0 && (!error || !*error)) {
        virErrorPtr e = virGetLastError();
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "VPC metadata 제거 실패: %s", e ? e->message : "unknown"); return FALSE;
    }
    return TRUE;
}

gboolean
pcv_vpc_init(const gchar *db_path, GError **error)
{
    g_mutex_lock(&g_lifecycle_mu);
    if (g_store) { g_mutex_unlock(&g_lifecycle_mu); return TRUE; }
    const gchar *path = db_path && *db_path ? db_path : PCV_VPC_DEFAULT_DB_PATH;
    g_autofree gchar *parent = g_path_get_dirname(path);
    if (g_mkdir_with_parents(parent, 0750) != 0 && errno != EEXIST) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "VPC DB 디렉터리 생성 실패: %s", g_strerror(errno));
        g_mutex_unlock(&g_lifecycle_mu); return FALSE;
    }
    g_store = pcv_vpc_store_open(path, error);
    g_mutex_unlock(&g_lifecycle_mu);
    return g_store != NULL;
}

void
pcv_vpc_shutdown(void)
{
    g_mutex_lock(&g_lifecycle_mu);
    PcvVpcStore *store = g_store; g_store = NULL;
    g_mutex_unlock(&g_lifecycle_mu);
    pcv_vpc_store_free(store);
}

gboolean
pcv_vpc_bridge_is_managed(const gchar *bridge_name)
{
    PcvVpcStore *store = _store_ref(NULL);
    return store && pcv_vpc_store_bridge_is_managed(store, bridge_name);
}

gboolean
pcv_vpc_mac_is_managed(const gchar *mac_address)
{
    PcvVpcStore *store = _store_ref(NULL);
    return store && pcv_vpc_store_mac_is_managed(store, mac_address);
}

gboolean
pcv_vpc_vm_is_attached(const gchar *vm_identifier)
{
    PcvVpcStore *store = _store_ref(NULL);
    return store && pcv_vpc_store_vm_is_attached(store, vm_identifier);
}

gboolean
pcv_vpc_vm_has_publish(const gchar *vm_identifier)
{
    PcvVpcStore *store = _store_ref(NULL);
    return store && pcv_vpc_store_vm_has_publish(store, vm_identifier);
}

gboolean
pcv_vpc_security_group_sync_vm(const gchar *vm_name, GError **error)
{
    if (!vm_name || !*vm_name) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "Security Group을 동기화할 VM 이름이 필요합니다");
        return FALSE;
    }
    PcvVpcStore *store = _store_ref(error);
    if (!store)
        return FALSE;

    g_mutex_lock(&g_mutation_mu);
    g_autoptr(JsonArray) all = pcv_vpc_store_list_attachments(store, NULL, NULL, error);
    gboolean ok = all != NULL;
    g_autofree gchar *ovn_vpc_id = NULL;
    for (guint i = 0; ok && i < json_array_get_length(all); i++) {
        JsonObject *attachment = json_array_get_object_element(all, i);
        if (g_strcmp0(json_object_get_string_member(attachment, "vm_name"), vm_name) == 0 &&
            g_strcmp0(json_object_get_string_member_with_default(
                attachment, "backend", "linux"), "ovn") == 0) {
            ovn_vpc_id = g_strdup(json_object_get_string_member(attachment, "vpc_id"));
            break;
        }
    }
    if (ok && ovn_vpc_id) {
        g_autoptr(JsonObject) vpc = pcv_vpc_store_get_vpc(
            store, ovn_vpc_id, NULL, error);
        g_autoptr(JsonArray) attachments = vpc
            ? pcv_vpc_store_list_attachments(store, ovn_vpc_id, NULL, error) : NULL;
        ok = vpc && attachments && pcv_vpc_ovn_set_quarantine(vpc, TRUE, error) &&
             pcv_vpc_ovn_sync_policy(vpc, attachments, FALSE, error);
        if (!ok && vpc)
            (void)pcv_vpc_store_set_resource_state(
                store, "vpcs", ovn_vpc_id, "DEGRADED",
                error && *error ? (*error)->message : "OVN Security Group ACL sync failed",
                NULL);
    }
    g_mutex_unlock(&g_mutation_mu);
    return ok;
}

JsonArray *pcv_vpc_list(const gchar *tenant, GError **error) {
    PcvVpcStore *s = _store_ref(error); return s ? pcv_vpc_store_list_vpcs(s, tenant, error) : NULL;
}
JsonObject *pcv_vpc_get(const gchar *id, const gchar *tenant, GError **error) {
    PcvVpcStore *s = _store_ref(error); return s ? pcv_vpc_store_get_vpc(s, id, tenant, error) : NULL;
}
JsonArray *pcv_vpc_subnet_list(const gchar *id, const gchar *tenant, GError **error) {
    PcvVpcStore *s = _store_ref(error); return s ? pcv_vpc_store_list_subnets(s, id, tenant, error) : NULL;
}
JsonArray *pcv_vpc_attachment_list(const gchar *id, const gchar *tenant, GError **error) {
    PcvVpcStore *s = _store_ref(error); return s ? pcv_vpc_store_list_attachments(s, id, tenant, error) : NULL;
}
JsonArray *pcv_vpc_service_list(const gchar *id, const gchar *tenant, GError **error) {
    PcvVpcStore *s = _store_ref(error); return s ? pcv_vpc_store_list_publishes(s, id, tenant, error) : NULL;
}

JsonArray *
pcv_vpc_backend_list(GError **error)
{
    PcvVpcStore *s = _store_ref(error);
    if (!s) return NULL;
    g_autoptr(JsonArray) vpcs = pcv_vpc_store_list_vpcs(s, NULL, error);
    if (!vpcs) return NULL;
    guint linux_count = 0, ovn_count = 0;
    for (guint i = 0; i < json_array_get_length(vpcs); i++) {
        JsonObject *vpc = json_array_get_object_element(vpcs, i);
        if (g_strcmp0(json_object_get_string_member(vpc, "backend"), "ovn") == 0)
            ovn_count++;
        else
            linux_count++;
    }
    JsonArray *result = json_array_new();
    JsonObject *linux_backend = json_object_new();
    json_object_set_string_member(linux_backend, "id", "linux");
    json_object_set_string_member(linux_backend, "label", "Linux bridge");
    json_object_set_boolean_member(linux_backend, "ready", TRUE);
    json_object_set_int_member(linux_backend, "current_vpcs", linux_count);
    json_object_set_null_member(linux_backend, "product_limit");
    json_object_set_null_member(linux_backend, "allocatable_vpcs");
    json_array_add_object_element(result, linux_backend);

    JsonObject *ovn = pcv_vpc_ovn_backend_capability(_ovn_transit_pool());
    json_object_set_int_member(ovn, "current_vpcs", ovn_count);
    json_object_set_null_member(ovn, "product_limit");
    gint64 total = json_object_get_int_member(ovn, "allocatable_edge_links");
    json_object_set_int_member(ovn, "allocatable_vpcs", MAX((gint64)0, total - ovn_count));
    json_array_add_object_element(result, ovn);
    return result;
}

static JsonObject *
_resource_state_counts(JsonArray *resources)
{
    JsonObject *counts = json_object_new();
    for (guint i = 0; resources && i < json_array_get_length(resources); i++) {
        JsonObject *resource = json_array_get_object_element(resources, i);
        const gchar *state = json_object_get_string_member(resource, "state");
        gint64 count = json_object_has_member(counts, state)
            ? json_object_get_int_member(counts, state) : 0;
        json_object_set_int_member(counts, state, count + 1);
    }
    return counts;
}

static gboolean
_all_resources_active(JsonArray *resources)
{
    for (guint i = 0; resources && i < json_array_get_length(resources); i++)
        if (g_strcmp0(json_object_get_string_member(
                json_array_get_object_element(resources, i), "state"), "ACTIVE") != 0)
            return FALSE;
    return TRUE;
}

JsonObject *
pcv_vpc_status(const gchar *tenant, GError **error)
{
    PcvVpcStore *s = _store_ref(error); if (!s) return NULL;
    g_autoptr(JsonArray) vpcs = pcv_vpc_store_list_vpcs(s, tenant, error);
    g_autoptr(JsonArray) subnets = pcv_vpc_store_list_subnets(s, NULL, tenant, error);
    g_autoptr(JsonArray) attachments = pcv_vpc_store_list_attachments(s, NULL, tenant, error);
    g_autoptr(JsonArray) publishes = pcv_vpc_store_list_publishes(s, NULL, tenant, error);
    if (!vpcs || !subnets || !attachments || !publishes) return NULL;
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "vpc_count", json_array_get_length(vpcs));
    json_object_set_int_member(o, "subnet_count", json_array_get_length(subnets));
    json_object_set_int_member(o, "attachment_count", json_array_get_length(attachments));
    json_object_set_int_member(o, "service_publish_count", json_array_get_length(publishes));








    JsonArray *subnet_cidrs = json_array_new();
    for (guint i = 0; i < json_array_get_length(subnets); i++) {
        JsonObject *subnet = json_array_get_object_element(subnets, i);
        const gchar *vpc_id = json_object_get_string_member(subnet, "vpc_id");
        const gchar *vpc_name = NULL;
        for (guint j = 0; j < json_array_get_length(vpcs); j++) {
            JsonObject *vpc = json_array_get_object_element(vpcs, j);
            if (g_strcmp0(json_object_get_string_member(vpc, "id"), vpc_id) == 0) {
                vpc_name = json_object_get_string_member(vpc, "name");
                break;
            }
        }
        JsonObject *entry = json_object_new();
        json_object_set_string_member(entry, "vpc_id", vpc_id);
        if (vpc_name)
            json_object_set_string_member(entry, "vpc_name", vpc_name);
        json_object_set_string_member(entry, "backend",
            json_object_get_string_member_with_default(subnet, "backend", "linux"));
        json_object_set_string_member(entry, "cidr",
            json_object_get_string_member(subnet, "cidr"));
        json_object_set_string_member(entry, "state",
            json_object_get_string_member(subnet, "state"));
        json_array_add_object_element(subnet_cidrs, entry);
    }
    json_object_set_array_member(o, "subnet_cidrs", subnet_cidrs);
    json_object_set_object_member(o, "vpc_states", _resource_state_counts(vpcs));
    json_object_set_object_member(o, "subnet_states", _resource_state_counts(subnets));
    json_object_set_object_member(o, "attachment_states", _resource_state_counts(attachments));
    json_object_set_object_member(o, "service_publish_states", _resource_state_counts(publishes));
    gboolean healthy = _all_resources_active(vpcs) && _all_resources_active(subnets) &&
        _all_resources_active(attachments) && _all_resources_active(publishes);
    json_object_set_boolean_member(o, "healthy", healthy);
    json_object_set_boolean_member(o, "reconcile_required", !healthy);
    JsonArray *backends = pcv_vpc_backend_list(error);
    if (!backends) { json_object_unref(o); return NULL; }
    json_object_set_array_member(o, "backends", backends);
    return o;
}

                           
                                                                           
                                                                       
                                                                       
                                                                       
                    
  
                       
                                                            
                                                       
static gboolean
_subnet_create_locked(PcvVpcStore *s, const gchar *vpc_id, const gchar *tenant,
                      const gchar *name, const gchar *cidr, gint mtu,
                      gint64 expected_revision, gboolean cidr_prechecked,
                      gboolean *cleanup_verified_out,
                      JsonObject **result_out, GError **error)
{
    if (result_out) *result_out = NULL;
    if (cleanup_verified_out) *cleanup_verified_out = TRUE;
    gboolean ok = cidr_prechecked || _host_cidr_available(cidr, error);
    g_autofree gchar *id = NULL;
    g_autofree gchar *backend_ref = NULL;
    gint64 revision = 0;
    g_autoptr(JsonObject) vpc = ok ? pcv_vpc_store_get_vpc(s, vpc_id, tenant, error) : NULL;
    if (ok && !vpc) ok = FALSE;
    g_autofree gchar *backend = g_strdup(vpc
        ? json_object_get_string_member_with_default(vpc, "backend", "linux") : "linux");
    if (ok) ok = pcv_vpc_store_create_subnet(s, vpc_id, tenant, name, cidr, mtu,
                                              expected_revision, &id, &backend_ref,
                                              &revision, error);
    if (ok) {
        g_clear_pointer(&vpc, json_object_unref);
        vpc = pcv_vpc_store_get_vpc(s, vpc_id, tenant, error);
        ok = vpc != NULL;
    }
    if (ok) ok = _apply_quarantine(s, error);
    g_autoptr(JsonObject) subnet = ok
        ? pcv_vpc_store_get_subnet(s, id, tenant, error) : NULL;
    if (ok && !subnet)
        ok = FALSE;
    g_autofree gchar *gateway_cidr = subnet ? _gateway_cidr(subnet) : NULL;
    g_autoptr(JsonObject) binding = ok && g_strcmp0(backend, "ovn") == 0
        ? pcv_vpc_store_get_backend_binding(s, vpc_id, tenant, error) : NULL;
    if (ok && g_strcmp0(backend, "ovn") == 0 && !binding)
        ok = FALSE;
    gboolean actual_attempted = FALSE;
    if (ok && g_strcmp0(backend, "linux") == 0) {
        actual_attempted = TRUE;
        ok = network_bridge_create(backend_ref, gateway_cidr, mtu, error);
    } else if (ok) {
        actual_attempted = TRUE;
        ok = binding && pcv_vpc_ovn_ensure_subnet(vpc, binding, subnet, error);
    }
    if (ok) ok = _restart_dhcp(s, subnet, error);
    if (ok) ok = _apply_full_policy(s, error);
    if (ok && g_strcmp0(backend, "ovn") == 0)
        ok = _clear_ovn_quarantine(s, error);
    if (ok) ok = pcv_vpc_store_set_resource_state(
        s, "subnets", id, "ACTIVE", NULL, error);
    if (!ok && id) {
        const gchar *failure = error && *error ? (*error)->message : "subnet create failed";
                                                           
                                                             
        gboolean cleaned = !actual_attempted;
        if (actual_attempted && g_strcmp0(backend, "linux") == 0)
            cleaned = network_bridge_delete(backend_ref, NULL);
        else if (actual_attempted)
            cleaned = pcv_vpc_ovn_remove_subnet(vpc, binding, subnet, NULL);
        if (cleaned)
            cleaned = pcv_vpc_store_delete_subnet(s, id, tenant, NULL);
        if (cleaned)
            cleaned = _apply_full_policy(s, NULL);
        if (!cleaned) {
            if (cleanup_verified_out) *cleanup_verified_out = FALSE;
            (void)pcv_vpc_store_set_resource_state(
                s, "subnets", id, "ERROR", failure, NULL);
            (void)pcv_vpc_store_set_resource_state(
                s, "vpcs", vpc_id, "DEGRADED", failure, NULL);
        }
    }
    if (ok && result_out) {
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "id", id);
        json_object_set_string_member(o, "vpc_id", vpc_id);
        json_object_set_string_member(o, "name", name);
        json_object_set_string_member(o, "cidr", cidr);
        json_object_set_string_member(o, "backend", backend);
        json_object_set_string_member(o, "backend_ref", backend_ref);
        if (g_strcmp0(backend, "linux") == 0)
            json_object_set_string_member(o, "bridge_name", backend_ref);
        else
            json_object_set_null_member(o, "bridge_name");
        json_object_set_int_member(o, "revision", revision);
        json_object_set_string_member(o, "state", "ACTIVE");
        *result_out = o;
    }
    return ok;
}

gboolean
pcv_vpc_create(const gchar *name, const gchar *tenant, const gchar *mode,
               const gchar *backend,
               const gchar *initial_subnet_name, const gchar *initial_subnet_cidr,
               gint initial_subnet_mtu, JsonObject **result_out, GError **error)
{
    if (result_out) *result_out = NULL;
    gboolean has_initial_subnet = initial_subnet_name || initial_subnet_cidr;
    if (!pcv_vpc_backend_is_valid(backend) ||
        (initial_subnet_name == NULL) != (initial_subnet_cidr == NULL) ||
        (has_initial_subnet && (initial_subnet_mtu < 68 || initial_subnet_mtu > 9216))) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_INVALID_ARGUMENT,
                    "backend 또는 첫 subnet name/cidr/mtu 묶음이 유효하지 않습니다");
        return FALSE;
    }
    PcvVpcStore *s = _store_ref(error); if (!s) return FALSE;
    if (g_strcmp0(backend, "ovn") == 0 &&
        !pcv_vpc_ovn_require_ready(_ovn_transit_pool(), error))
        return FALSE;
    if (has_initial_subnet && !_host_cidr_available(initial_subnet_cidr, error))
        return FALSE;

    g_mutex_lock(&g_mutation_mu);
    g_autofree gchar *id = NULL;
    gint64 revision = 0;
    gboolean ok = pcv_vpc_store_create_vpc(
        s, name, tenant, mode, backend, &id, &revision, error);
    g_autoptr(JsonObject) vpc = ok ? pcv_vpc_store_get_vpc(s, id, tenant, error) : NULL;
    g_autoptr(JsonObject) binding = NULL;
    gboolean ovn_actual_attempted = FALSE;
    if (ok && g_strcmp0(backend, "ovn") == 0) {
        binding = pcv_vpc_store_ensure_ovn_binding(
            s, id, tenant, _ovn_transit_pool(), error);
        if (binding) {
            ovn_actual_attempted = TRUE;
            ok = pcv_vpc_ovn_ensure_vpc(vpc, binding, error);
        } else {
            ok = FALSE;
        }
    }
    g_autoptr(JsonObject) subnet_result = NULL;
    gboolean subnet_cleanup_verified = TRUE;
    if (ok && has_initial_subnet) {
        ok = _subnet_create_locked(
            s, id, tenant, initial_subnet_name, initial_subnet_cidr,
            initial_subnet_mtu, revision, TRUE, &subnet_cleanup_verified,
            &subnet_result, error);
    }
    if (ok && !has_initial_subnet)
        ok = _apply_full_policy(s, error);
    if (ok && g_strcmp0(backend, "ovn") == 0)
        ok = _clear_ovn_quarantine(s, error);
    if (ok)
        ok = pcv_vpc_store_set_resource_state(s, "vpcs", id, "ACTIVE", NULL, error);
    if (!ok && id) {
        g_autoptr(GError) operation_error =
            error && *error ? g_steal_pointer(error) : NULL;
        g_autoptr(GError) rollback_error = NULL;
        gboolean actual_cleaned = !ovn_actual_attempted ||
            pcv_vpc_ovn_remove_vpc(vpc, binding, NULL);
        gboolean rolled_back = subnet_cleanup_verified && actual_cleaned &&
            pcv_vpc_store_delete_vpc(s, id, tenant, &rollback_error);
        if (!subnet_cleanup_verified)
            g_set_error(&rollback_error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                        "첫 subnet cleanup을 확인할 수 없습니다");
        if (!rolled_back) {
            const gchar *failure = operation_error ? operation_error->message
                                                   : "initial subnet create failed";
            (void)pcv_vpc_store_set_resource_state(
                s, "vpcs", id, "DEGRADED", failure, NULL);
            if (operation_error)
                g_prefix_error(&operation_error,
                    "첫 subnet 생성 뒤 VPC rollback 실패(%s): ",
                    rollback_error ? rollback_error->message : "unknown");
        }
        if (error) {
            if (operation_error)
                g_propagate_error(error, g_steal_pointer(&operation_error));
            else if (rollback_error)
                g_propagate_error(error, g_steal_pointer(&rollback_error));
            else
                g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                            "VPC와 첫 subnet 생성에 실패했습니다");
        }
    }
    g_mutex_unlock(&g_mutation_mu);

    if (ok && result_out) {
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "id", id);
        json_object_set_string_member(o, "backend", backend);
        json_object_set_int_member(o, "revision",
            subnet_result ? json_object_get_int_member(subnet_result, "revision") : revision);
        json_object_set_string_member(o, "state", "ACTIVE");
        if (subnet_result)
            json_object_set_object_member(o, "subnet", json_object_ref(subnet_result));
        *result_out = o;
    }
    return ok;
}

gboolean
pcv_vpc_delete(const gchar *id, const gchar *tenant, GError **error)
{
    PcvVpcStore *s = _store_ref(error); if (!s) return FALSE;
    g_mutex_lock(&g_mutation_mu);
    g_autoptr(JsonObject) vpc = pcv_vpc_store_get_vpc(s, id, tenant, error);
    g_autoptr(JsonArray) subnets = vpc
        ? pcv_vpc_store_list_subnets(s, id, tenant, error) : NULL;
    gboolean ok = vpc && subnets;
    if (ok && json_array_get_length(subnets) != 0) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "subnet이 있는 VPC는 삭제할 수 없습니다");
        ok = FALSE;
    }
    const gchar *backend = vpc
        ? json_object_get_string_member_with_default(vpc, "backend", "linux") : "linux";
    g_autoptr(JsonObject) binding = ok && g_strcmp0(backend, "ovn") == 0
        ? pcv_vpc_store_get_backend_binding(s, id, tenant, error) : NULL;
    if (ok) ok = _apply_quarantine(s, error);
    if (ok && g_strcmp0(backend, "ovn") == 0)
        ok = binding && pcv_vpc_ovn_remove_vpc(vpc, binding, error);
    if (ok) ok = pcv_vpc_store_delete_vpc(s, id, tenant, error);
    if (ok) ok = _apply_full_policy(s, error);
    if (ok) ok = _clear_ovn_quarantine(s, error);
    g_mutex_unlock(&g_mutation_mu);
    return ok;
}

gboolean
pcv_vpc_egress_set(const gchar *id, const gchar *tenant, const gchar *mode,
                   gint64 expected_revision, JsonObject **result_out, GError **error)
{
    if (result_out) *result_out = NULL;
    PcvVpcStore *s = _store_ref(error); if (!s) return FALSE;
    g_mutex_lock(&g_mutation_mu);
    g_autoptr(JsonObject) vpc = pcv_vpc_store_get_vpc(s, id, tenant, error);
    gboolean ok = vpc != NULL;
    const gchar *backend = vpc
        ? json_object_get_string_member_with_default(vpc, "backend", "linux") : "linux";
    g_autoptr(JsonObject) binding = ok && g_strcmp0(backend, "ovn") == 0
        ? pcv_vpc_store_get_backend_binding(s, id, tenant, error) : NULL;
    if (ok && g_strcmp0(backend, "ovn") == 0 && !binding) ok = FALSE;
    if (g_strcmp0(mode, "isolated") == 0) {
        g_autoptr(JsonArray) publishes = pcv_vpc_store_list_publishes(s, id, tenant, error);
        if (!publishes) ok = FALSE;
        else if (json_array_get_length(publishes) > 0) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                        "Service Publish가 있는 VPC는 isolated로 전환할 수 없습니다");
            ok = FALSE;
        }
    }
    gboolean quarantined = FALSE;
    if (ok) {
        ok = _apply_quarantine(s, error);
        quarantined = ok;
    }
    gint64 revision = 0;
    gboolean desired_changed = FALSE;
    if (ok) {
        ok = pcv_vpc_store_set_egress(
            s, id, tenant, mode, expected_revision, &revision, error);
        desired_changed = ok;
    }
    if (ok && g_strcmp0(backend, "ovn") == 0) {
        g_clear_pointer(&vpc, json_object_unref);
        vpc = pcv_vpc_store_get_vpc(s, id, tenant, error);
        ok = vpc && pcv_vpc_ovn_set_egress(vpc, binding, error);
    }
    if (ok) ok = _apply_full_policy(s, error);
    if (ok) ok = _clear_ovn_quarantine(s, error);
    if (!ok && quarantined && !desired_changed)
        (void)_apply_full_policy(s, NULL);
    if (!ok && desired_changed)
        pcv_vpc_store_set_resource_state(s, "vpcs", id, "DEGRADED",
            error && *error ? (*error)->message : "policy apply failed", NULL);
    g_mutex_unlock(&g_mutation_mu);
    if (ok && result_out) { JsonObject *o = json_object_new();
        json_object_set_string_member(o, "id", id); json_object_set_string_member(o, "egress_mode", mode);
        json_object_set_int_member(o, "revision", revision); *result_out = o; }
    return ok;
}

gboolean
pcv_vpc_subnet_create(const gchar *vpc_id, const gchar *tenant, const gchar *name,
                      const gchar *cidr, gint mtu, gint64 expected_revision,
                      JsonObject **result_out, GError **error)
{
    if (result_out) *result_out = NULL;
    PcvVpcStore *s = _store_ref(error); if (!s) return FALSE;
    g_mutex_lock(&g_mutation_mu);
    gboolean ok = _subnet_create_locked(
        s, vpc_id, tenant, name, cidr, mtu, expected_revision, FALSE,
        NULL, result_out, error);
    g_mutex_unlock(&g_mutation_mu);
    return ok;
}

gboolean
pcv_vpc_subnet_delete(const gchar *id, const gchar *tenant, GError **error)
{
    PcvVpcStore *s = _store_ref(error); if (!s) return FALSE;
    g_mutex_lock(&g_mutation_mu);
    g_autoptr(JsonObject) subnet = pcv_vpc_store_get_subnet(s, id, tenant, error);
    const gchar *vpc_id = subnet
        ? json_object_get_string_member(subnet, "vpc_id") : NULL;
    g_autoptr(JsonArray) attachments = subnet
        ? pcv_vpc_store_list_attachments(s, vpc_id, tenant, error) : NULL;
    gboolean ok = subnet != NULL && attachments != NULL;








    for (guint i = 0; ok && i < json_array_get_length(attachments); i++) {
        JsonObject *attachment = json_array_get_object_element(attachments, i);
        if (g_strcmp0(json_object_get_string_member(attachment, "subnet_id"), id) == 0) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                        "attachment가 있는 subnet은 삭제할 수 없습니다");
            ok = FALSE;
        }
    }
    const gchar *backend = subnet
        ? json_object_get_string_member_with_default(subnet, "backend", "linux") : "linux";
    g_autoptr(JsonObject) vpc = ok ? pcv_vpc_store_get_vpc(
        s, vpc_id, tenant, error) : NULL;
    g_autoptr(JsonObject) binding = vpc && g_strcmp0(backend, "ovn") == 0
        ? pcv_vpc_store_get_backend_binding(s,
            json_object_get_string_member(vpc, "id"), tenant, error) : NULL;
    gboolean mutation_started = FALSE;
    if (ok) {
        mutation_started = TRUE;
        ok = _apply_quarantine(s, error);
    }
    const gchar *bridge = subnet && g_strcmp0(backend, "linux") == 0
        ? json_object_get_string_member(subnet, "bridge_name") : NULL;
    if (ok) ok = pcv_vpc_store_set_resource_state(s, "subnets", id, "DELETING", NULL, error);
    if (ok && g_strcmp0(backend, "linux") == 0) ok = network_dhcp_stop(bridge, error);
    if (ok && g_strcmp0(backend, "linux") == 0) ok = network_bridge_delete(bridge, error);
    if (ok && g_strcmp0(backend, "ovn") == 0)
        ok = binding && pcv_vpc_ovn_remove_subnet(vpc, binding, subnet, error);
    if (ok) ok = pcv_vpc_store_delete_subnet(s, id, tenant, error);
    if (ok) ok = _apply_full_policy(s, error);
    if (ok) ok = _clear_ovn_quarantine(s, error);
    if (!ok && subnet && mutation_started) pcv_vpc_store_set_resource_state(s, "subnets", id, "ERROR",
        error && *error ? (*error)->message : "subnet delete failed", NULL);
    g_mutex_unlock(&g_mutation_mu); return ok;
}

gboolean
pcv_vpc_attachment_create(const gchar *subnet_id, const gchar *tenant, const gchar *vm,
                          const gchar *actor, gboolean actor_is_admin, const gchar *requested_ip,
                          JsonObject **result_out, GError **error)
{
    if (result_out) *result_out = NULL;
    PcvVpcStore *s = _store_ref(error); if (!s) return FALSE;
    g_mutex_lock(&g_mutation_mu);
    virConnectPtr conn = virt_conn_pool_acquire(); virDomainPtr dom = NULL;
    gboolean ok = conn != NULL;
    if (!ok) g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO, "libvirt connection unavailable");
    if (ok) { dom = virDomainLookupByName(conn, vm); if (!dom) { virResetLastError(); dom = virDomainLookupByUUIDString(conn, vm); } }
    if (ok && !dom) { g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND, "VM을 찾을 수 없습니다"); ok = FALSE; }
    if (ok && virDomainIsActive(dom) != 0) { g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
        "첫 릴리스 VPC attachment는 정지 VM에만 만들 수 있습니다"); ok = FALSE; }
    char uuid[VIR_UUID_STRING_BUFLEN] = {0}; const char *domain_name = dom ? virDomainGetName(dom) : NULL;
    if (ok && virDomainGetUUIDString(dom, uuid) < 0) { g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
        "VM UUID 조회 실패"); ok = FALSE; }
    if (ok && !actor_is_admin) {
        char *xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
        g_autofree gchar *owner = xml ? _xml_owner(xml) : NULL; if (xml) free(xml);
        if (!owner || g_strcmp0(owner, actor) != 0) { g_set_error(error, PCV_VPC_ERROR,
            PCV_VPC_ERROR_CONFLICT, "operator는 자기 VM만 VPC에 연결할 수 있습니다"); ok = FALSE; }
    }
    if (ok && pcv_tenant_overlay_vm_in_any_tenant(domain_name)) { g_set_error(error, PCV_VPC_ERROR,
        PCV_VPC_ERROR_CONFLICT, "tenant-overlay VM은 Local VPC에 연결할 수 없습니다"); ok = FALSE; }
    g_autoptr(JsonObject) attachment = NULL;
    if (ok) ok = pcv_vpc_store_allocate_attachment(s, subnet_id, tenant, uuid, domain_name,
                                                     actor ? actor : "admin", requested_ip,
                                                     &attachment, error);
    const gchar *aid = attachment ? json_object_get_string_member(attachment, "id") : NULL;
    g_autoptr(JsonObject) subnet = ok ? pcv_vpc_store_get_subnet(s, subnet_id, tenant, error) : NULL;
    g_autoptr(JsonObject) vpc = subnet ? pcv_vpc_store_get_vpc(
        s, json_object_get_string_member(subnet, "vpc_id"), tenant, error) : NULL;
    const gchar *backend = attachment
        ? json_object_get_string_member_with_default(attachment, "backend", "linux") : "linux";
    if (ok) ok = _apply_quarantine(s, error);
    gboolean ovn_port_attempted = FALSE;
    if (ok && g_strcmp0(backend, "ovn") == 0) {
        ovn_port_attempted = TRUE;
        ok = vpc && subnet && pcv_vpc_ovn_ensure_attachment(vpc, subnet, attachment, error);
    }
    g_autofree gchar *xml = attachment ? _attachment_interface_xml(attachment) : NULL;
    gboolean nic_attached = FALSE;
    gboolean metadata_added = FALSE;
    if (ok) { ok = virDomainAttachDeviceFlags(dom, xml, VIR_DOMAIN_AFFECT_CONFIG) == 0; nic_attached = ok;
        if (!ok) { virErrorPtr e = virGetLastError(); g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
            "VPC NIC attach 실패: %s", e ? e->message : "unknown"); } }
    if (ok) {
        ok = _metadata_add_attachment(dom,
            json_object_get_string_member(attachment, "vpc_id"), subnet_id, aid,
            json_object_get_string_member(attachment, "ip_address"), error);
        metadata_added = ok;
    }
    if (ok) ok = pcv_vpc_store_set_resource_state(s, "attachments", aid, "ACTIVE", NULL, error);
    if (ok) ok = _restart_dhcp(s, subnet, error);
    if (ok) ok = _apply_full_policy(s, error);
    if (ok) ok = _clear_ovn_quarantine(s, error);
    if (!ok && attachment) {
        gboolean nic_rolled_back = !nic_attached;
        if (nic_attached)
            nic_rolled_back = virDomainDetachDeviceFlags(
                dom, xml, VIR_DOMAIN_AFFECT_CONFIG) == 0;
        gboolean metadata_rolled_back = !metadata_added;
        if (metadata_added)
            metadata_rolled_back = _metadata_remove_attachment(dom, aid, NULL);
                                                                            
                                                              
        gboolean ovn_rolled_back = !ovn_port_attempted ||
            pcv_vpc_ovn_remove_attachment(vpc, subnet, attachment, NULL);
        if (nic_rolled_back && metadata_rolled_back && ovn_rolled_back)
            pcv_vpc_store_delete_attachment(s, aid, tenant, NULL);
        else pcv_vpc_store_set_resource_state(s, "attachments", aid, "QUARANTINED",
            error && *error ? (*error)->message : "attachment create failed", NULL);
    }
    if (dom)
        virDomainFree(dom);
    if (conn)
        virt_conn_pool_release(conn);
    g_mutex_unlock(&g_mutation_mu);
    if (ok && result_out) *result_out = json_object_ref(attachment);
    return ok;
}

gboolean
pcv_vpc_attachment_delete(const gchar *id, const gchar *tenant, const gchar *actor,
                          gboolean actor_is_admin, GError **error)
{
    PcvVpcStore *s = _store_ref(error); if (!s) return FALSE;
    g_mutex_lock(&g_mutation_mu);
    g_autoptr(JsonObject) a = pcv_vpc_store_get_attachment(s, id, tenant, error);
    gboolean ok = a != NULL;
    if (ok && !actor_is_admin && g_strcmp0(actor,
            json_object_get_string_member(a, "owner_subject")) != 0) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "operator는 자기 VM attachment만 삭제할 수 있습니다"); ok = FALSE;
    }
    g_autoptr(JsonArray) publishes = ok ? pcv_vpc_store_list_publishes(
        s, json_object_get_string_member(a, "vpc_id"), tenant, error) : NULL;
    for (guint i = 0; ok && publishes && i < json_array_get_length(publishes); i++) {
        JsonObject *publish = json_array_get_object_element(publishes, i);
        if (g_strcmp0(json_object_get_string_member(publish, "attachment_id"), id) == 0) {
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                        "Service Publish가 참조하는 attachment는 삭제할 수 없습니다");
            ok = FALSE;
        }
    }
    if (ok && !publishes) ok = FALSE;
    virConnectPtr conn = ok ? virt_conn_pool_acquire() : NULL; virDomainPtr dom = NULL;
    if (ok && !conn) { g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                                   "libvirt connection unavailable"); ok = FALSE; }
    if (ok) dom = virDomainLookupByUUIDString(conn, json_object_get_string_member(a, "vm_uuid"));
    if (ok && !dom) { g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND, "VM을 찾을 수 없습니다"); ok = FALSE; }
    if (ok && virDomainIsActive(dom) != 0) { g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
        "첫 릴리스 VPC detach는 정지 VM에만 허용됩니다"); ok = FALSE; }
    if (ok) ok = _apply_quarantine(s, error);
    gboolean detaching_marked = FALSE;
    if (ok) {
        ok = pcv_vpc_store_set_resource_state(
            s, "attachments", id, "DETACHING", NULL, error);
        detaching_marked = ok;
    }
                                                                               
                                                                        
                                                                  
                                                            
    g_autofree gchar *xml = a ? _attachment_interface_xml(a) : NULL;
    g_autofree gchar *interface_id = a && g_strcmp0(
        json_object_get_string_member_with_default(a, "backend", "linux"), "ovn") == 0
        ? pcv_vpc_ovn_port_name_from_id(id) : NULL;
    PcvVpcNicMatch nic = ok ? _domain_nic_match(
        dom, VIR_DOMAIN_XML_INACTIVE,
        json_object_get_string_member(a, "mac_address"),
        _attachment_bridge(a), interface_id,
        error) : PCV_VPC_NIC_ERROR;
    if (ok && nic == PCV_VPC_NIC_MISMATCH) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "VPC NIC MAC이 다른 bridge를 가리킵니다");
        ok = FALSE;
    } else if (ok && nic == PCV_VPC_NIC_ERROR) {
        ok = FALSE;
    } else if (ok && nic == PCV_VPC_NIC_MATCH &&
               virDomainDetachDeviceFlags(dom, xml, VIR_DOMAIN_AFFECT_CONFIG) < 0) {
        virErrorPtr e = virGetLastError(); g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
            "VPC NIC detach 실패: %s", e ? e->message : "unknown"); ok = FALSE;
    }
    if (ok) ok = _metadata_remove_attachment(dom, id, error);
    g_autofree gchar *subnet_id = a ? g_strdup(json_object_get_string_member(a, "subnet_id")) : NULL;
    g_autoptr(JsonObject) subnet = ok ? pcv_vpc_store_get_subnet(s, subnet_id, tenant, error) : NULL;
    g_autoptr(JsonObject) vpc = subnet ? pcv_vpc_store_get_vpc(
        s, json_object_get_string_member(subnet, "vpc_id"), tenant, error) : NULL;
    if (ok && g_strcmp0(json_object_get_string_member_with_default(
            a, "backend", "linux"), "ovn") == 0)
        ok = vpc && pcv_vpc_ovn_remove_attachment(vpc, subnet, a, error);
    if (ok) ok = _restart_dhcp(s, subnet, error);
    if (ok) ok = _apply_full_policy(s, error);
    if (ok) ok = _clear_ovn_quarantine(s, error);
                                                                 
    if (ok) ok = pcv_vpc_store_delete_attachment(s, id, tenant, error);
    if (!ok && detaching_marked)
        pcv_vpc_store_set_resource_state(s, "attachments", id, "DETACHING",
            error && *error ? (*error)->message : "attachment detach failed", NULL);
    if (dom)
        virDomainFree(dom);
    if (conn)
        virt_conn_pool_release(conn);
    g_mutex_unlock(&g_mutation_mu); return ok;
}

gboolean
pcv_vpc_service_publish(const gchar *attachment_id, const gchar *tenant, const gchar *protocol,
                        const gchar *listen_address, gint listen_port, gint target_port,
                        GPtrArray *sources, gboolean actor_is_admin,
                        JsonObject **result_out, GError **error)
{
    if (result_out) *result_out = NULL;
    PcvVpcStore *s = _store_ref(error); if (!s) return FALSE;
    if (!_listener_address_is_local(listen_address)) { g_set_error(error, PCV_VPC_ERROR,
        PCV_VPC_ERROR_INVALID_ARGUMENT, "listen_address가 host local IPv4가 아닙니다"); return FALSE; }
    if (!actor_is_admin && pcv_vpc_sources_cover_all_ipv4(sources)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "전체 IPv4 공개 Service Publish는 admin만 만들 수 있습니다");
        return FALSE;
    }
    if (!_listener_port_is_free(protocol, listen_address, listen_port, error)) return FALSE;
    g_mutex_lock(&g_mutation_mu);
    g_autoptr(JsonObject) attachment = pcv_vpc_store_get_attachment(
        s, attachment_id, tenant, error);
    g_autofree gchar *id = NULL;
    gboolean ok = attachment != NULL;
    if (ok && !pcv_security_group_vm_is_bound(
            json_object_get_string_member(attachment, "vm_name"))) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "Service Publish 대상 VM에는 Security Group이 필요합니다");
        ok = FALSE;
    }
    if (ok) ok = pcv_vpc_store_create_publish(s, attachment_id, tenant, protocol,
                                                listen_address, listen_port, target_port,
                                                sources, &id, error);
    if (ok) ok = pcv_vpc_store_set_resource_state(s, "service_publishes", id, "ACTIVE", NULL, error);
    if (ok) ok = _apply_full_policy(s, error);
    if (!ok && id) pcv_vpc_store_set_resource_state(s, "service_publishes", id, "ERROR",
        error && *error ? (*error)->message : "publish apply failed", NULL);
    g_mutex_unlock(&g_mutation_mu);
    if (ok && result_out) { JsonObject *o = json_object_new(); json_object_set_string_member(o, "id", id);
        json_object_set_string_member(o, "state", "ACTIVE"); *result_out = o; }
    return ok;
}

gboolean
pcv_vpc_service_unpublish(const gchar *id, const gchar *tenant, GError **error)
{
    PcvVpcStore *s = _store_ref(error); if (!s) return FALSE;
    g_mutex_lock(&g_mutation_mu);
    g_autoptr(JsonObject) p = pcv_vpc_store_get_publish(s, id, tenant, error);
    gboolean ok = p != NULL && _apply_quarantine(s, error);
    if (ok) ok = pcv_vpc_store_set_resource_state(s, "service_publishes", id, "DELETING", NULL, error);
    if (ok) ok = _apply_full_policy(s, error);
    if (ok) ok = pcv_vpc_store_delete_publish(s, id, tenant, error);
    if (!ok && p) pcv_vpc_store_set_resource_state(s, "service_publishes", id, "DELETING",
        error && *error ? (*error)->message : "unpublish failed", NULL);
    g_mutex_unlock(&g_mutation_mu); return ok;
}

static gboolean
_reconcile_attachment_actual(PcvVpcStore *store, JsonObject *attachment, GError **error)
{
    g_autoptr(JsonObject) subnet = pcv_vpc_store_get_subnet(
        store, json_object_get_string_member(attachment, "subnet_id"), NULL, error);
    g_autoptr(JsonObject) vpc = subnet ? pcv_vpc_store_get_vpc(
        store, json_object_get_string_member(attachment, "vpc_id"), NULL, error) : NULL;
    if (!subnet || !vpc)
        return FALSE;
    if (g_strcmp0(json_object_get_string_member_with_default(
            attachment, "backend", "linux"), "ovn") == 0 &&
        !pcv_vpc_ovn_ensure_attachment(vpc, subnet, attachment, error))
        return FALSE;
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "libvirt connection unavailable");
        return FALSE;
    }
    virDomainPtr dom = virDomainLookupByUUIDString(
        conn, json_object_get_string_member(attachment, "vm_uuid"));
    if (!dom) {
        virResetLastError();
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_NOT_FOUND,
                    "attachment VM을 찾을 수 없습니다: %s",
                    json_object_get_string_member(attachment, "vm_name"));
        virt_conn_pool_release(conn);
        return FALSE;
    }

    const gchar *mac = json_object_get_string_member(attachment, "mac_address");
    const gchar *bridge = _attachment_bridge(attachment);
    g_autofree gchar *interface_id = g_strcmp0(
        json_object_get_string_member_with_default(attachment, "backend", "linux"), "ovn") == 0
        ? pcv_vpc_ovn_port_name_from_id(
            json_object_get_string_member(attachment, "id")) : NULL;
    PcvVpcNicMatch config_match = _domain_nic_match(
        dom, VIR_DOMAIN_XML_INACTIVE, mac, bridge, interface_id, error);
    gboolean ok = config_match != PCV_VPC_NIC_ERROR &&
                  config_match != PCV_VPC_NIC_MISMATCH;
    if (config_match == PCV_VPC_NIC_MISMATCH) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "attachment MAC이 다른 persistent bridge를 가리킵니다");
    }
    if (ok && config_match == PCV_VPC_NIC_MISSING) {
        g_autofree gchar *xml = _attachment_interface_xml(attachment);
        if (virDomainAttachDeviceFlags(dom, xml, VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            virErrorPtr e = virGetLastError();
            g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                        "persistent VPC NIC 복구 실패: %s", e ? e->message : "unknown");
            ok = FALSE;
        }
    }
    if (ok) {
        ok = _metadata_add_attachment(
            dom,
            json_object_get_string_member(attachment, "vpc_id"),
            json_object_get_string_member(attachment, "subnet_id"),
            json_object_get_string_member(attachment, "id"),
            json_object_get_string_member(attachment, "ip_address"), error);
    }

    gint active = ok ? virDomainIsActive(dom) : 0;
    if (ok && active < 0) {
        virErrorPtr e = virGetLastError();
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_IO,
                    "VM 실행 상태 조회 실패: %s", e ? e->message : "unknown");
        ok = FALSE;
    }
    if (ok && active == 1) {
        PcvVpcNicMatch live_match = _domain_nic_match(
            dom, 0, mac, bridge, interface_id, error);
        if (live_match != PCV_VPC_NIC_MATCH) {
            if (live_match == PCV_VPC_NIC_MISSING)
                g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                            "실행 VM의 VPC NIC가 없어 VM 정지·재시작이 필요합니다");
            else if (live_match == PCV_VPC_NIC_MISMATCH)
                g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                            "실행 VM의 VPC NIC MAC이 다른 bridge를 가리킵니다");
            ok = FALSE;
        }
    }
    virDomainFree(dom);
    virt_conn_pool_release(conn);
    return ok;
}

static gboolean
_reconcile_subnet_actual(PcvVpcStore *store, JsonObject *subnet, GError **error)
{
    if (g_strcmp0(json_object_get_string_member_with_default(
            subnet, "backend", "linux"), "ovn") == 0) {
        const gchar *vpc_id = json_object_get_string_member(subnet, "vpc_id");
        g_autoptr(JsonObject) vpc = pcv_vpc_store_get_vpc(store, vpc_id, NULL, error);
        g_autoptr(JsonObject) binding = vpc ? pcv_vpc_store_get_backend_binding(
            store, vpc_id, NULL, error) : NULL;
        return vpc && binding &&
            pcv_vpc_ovn_ensure_vpc(vpc, binding, error) &&
            pcv_vpc_ovn_ensure_subnet(vpc, binding, subnet, error);
    }
    const gchar *bridge = json_object_get_string_member(subnet, "bridge_name");
    g_autofree gchar *gateway_cidr = _gateway_cidr(subnet);
    gint mtu = (gint)json_object_get_int_member(subnet, "mtu");
    if (!_bridge_exists(bridge))
        return network_bridge_create(bridge, gateway_cidr, mtu, error);

    g_autofree gchar *bridge_marker = g_build_filename(
        "/sys/class/net", bridge, "bridge", NULL);
    if (!g_file_test(bridge_marker, G_FILE_TEST_IS_DIR)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "managed bridge 이름을 다른 interface가 사용 중입니다: %s", bridge);
        return FALSE;
    }
    const gchar *flush_argv[] = { "ip", "-4", "addr", "flush", "dev", bridge, NULL };
    const gchar *addr_argv[] = { "ip", "addr", "add", gateway_cidr, "dev", bridge, NULL };
    gchar mtu_text[16]; g_snprintf(mtu_text, sizeof(mtu_text), "%d", mtu);
    const gchar *mtu_argv[] = { "ip", "link", "set", "dev", bridge, "mtu", mtu_text, NULL };
    const gchar *up_argv[] = { "ip", "link", "set", "dev", bridge, "up", NULL };
    return pcv_spawn_sync(flush_argv, NULL, NULL, error) &&
           pcv_spawn_sync(addr_argv, NULL, NULL, error) &&
           pcv_spawn_sync(mtu_argv, NULL, NULL, error) &&
           pcv_spawn_sync(up_argv, NULL, NULL, error);
}

static gboolean
_reconcile_publish_actual(PcvVpcStore *store, JsonObject *publish, GError **error)
{
    g_autoptr(JsonObject) attachment = pcv_vpc_store_get_attachment(
        store, json_object_get_string_member(publish, "attachment_id"), NULL, error);
    g_autoptr(JsonObject) vpc = attachment ? pcv_vpc_store_get_vpc(
        store, json_object_get_string_member(publish, "vpc_id"), NULL, error) : NULL;
    if (!attachment || !vpc) return FALSE;
    if (g_strcmp0(json_object_get_string_member(attachment, "state"), "ACTIVE") != 0 ||
        g_strcmp0(json_object_get_string_member(vpc, "egress_mode"), "nat") != 0) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                    "Service Publish 대상은 ACTIVE attachment가 있는 NAT VPC여야 합니다");
        return FALSE;
    }
    if (!pcv_security_group_vm_is_bound(
            json_object_get_string_member(attachment, "vm_name"))) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_CONFLICT,
                    "Service Publish 대상 VM의 Security Group이 없습니다");
        return FALSE;
    }
    const gchar *address = json_object_get_string_member(publish, "listen_address");
    if (!_listener_address_is_local(address)) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                    "Service Publish listen address가 더 이상 host에 없습니다: %s", address);
        return FALSE;
    }
    return _listener_port_is_free(
        json_object_get_string_member(publish, "protocol"), address,
        (gint)json_object_get_int_member(publish, "listen_port"), error);
}

gboolean
pcv_vpc_reconcile(GError **error)
{
    PcvVpcStore *s = _store_ref(error); if (!s) return FALSE;
    g_mutex_lock(&g_mutation_mu);
    g_autoptr(JsonArray) vpcs = pcv_vpc_store_list_vpcs(s, NULL, error);
    if (!vpcs) { g_mutex_unlock(&g_mutation_mu); return FALSE; }
    g_autoptr(GPtrArray) bridges = pcv_vpc_store_list_managed_bridges(s, error);
    if (!bridges) { g_mutex_unlock(&g_mutation_mu); return FALSE; }
    if (bridges->len == 0) {
                                                              
        gboolean empty_ok = _apply_full_policy(s, error);
        for (guint i = 0; empty_ok && i < json_array_get_length(vpcs); i++)
            empty_ok = pcv_vpc_store_set_resource_state(
                s, "vpcs", json_object_get_string_member(
                    json_array_get_object_element(vpcs, i), "id"),
                "ACTIVE", NULL, error);
        g_mutex_unlock(&g_mutation_mu);
        return empty_ok;
    }
    gboolean ok = _apply_quarantine(s, error);
    if (!ok) { _managed_bridges_down(s); g_mutex_unlock(&g_mutation_mu); return FALSE; }
    for (guint i = 0; ok && i < json_array_get_length(vpcs); i++) {
        JsonObject *vpc = json_array_get_object_element(vpcs, i);
        if (g_strcmp0(json_object_get_string_member_with_default(
                vpc, "backend", "linux"), "ovn") != 0)
            continue;
        g_autoptr(JsonObject) binding = pcv_vpc_store_get_backend_binding(
            s, json_object_get_string_member(vpc, "id"), NULL, error);
        ok = binding && pcv_vpc_ovn_require_ready(_ovn_transit_pool(), error) &&
             pcv_vpc_ovn_ensure_vpc(vpc, binding, error);
    }
    g_autoptr(JsonArray) subnets = pcv_vpc_store_list_subnets(s, NULL, NULL, error);
    if (!subnets) ok = FALSE;
    for (guint i = 0; ok && subnets && i < json_array_get_length(subnets); i++) {
        JsonObject *subnet = json_array_get_object_element(subnets, i);
        const gchar *state = json_object_get_string_member(subnet, "state");
        if (g_strcmp0(state, "DELETING") == 0) continue;
        ok = _reconcile_subnet_actual(s, subnet, error);
    }
    gboolean degraded = FALSE;
    g_autofree gchar *degraded_message = NULL;
    g_autoptr(JsonArray) attachments = ok
        ? pcv_vpc_store_list_attachments(s, NULL, NULL, error) : NULL;
    if (ok && !attachments) ok = FALSE;
    for (guint i = 0; ok && attachments && i < json_array_get_length(attachments); i++) {
        JsonObject *attachment = json_array_get_object_element(attachments, i);
        const gchar *state = json_object_get_string_member(attachment, "state");
        if (g_strcmp0(state, "DETACHING") == 0) {
            degraded = TRUE;
            if (!degraded_message)
                degraded_message = g_strdup("DETACHING attachment는 명시 재시도가 필요합니다");
            ok = pcv_vpc_store_set_resource_state(
                s, "vpcs", json_object_get_string_member(attachment, "vpc_id"),
                "DEGRADED", "DETACHING attachment는 명시 재시도가 필요합니다", error);
            continue;
        }
        g_autoptr(GError) attachment_error = NULL;
        if (_reconcile_attachment_actual(s, attachment, &attachment_error)) {
            ok = pcv_vpc_store_set_resource_state(
                s, "attachments", json_object_get_string_member(attachment, "id"),
                "ACTIVE", NULL, error);
        } else {
            const gchar *attachment_message = attachment_error && attachment_error->message
                ? attachment_error->message : "attachment reconcile 실패";
            degraded = TRUE;
            if (!degraded_message)
                degraded_message = g_strdup(attachment_message);
            if (!pcv_vpc_store_set_resource_state(
                    s, "attachments", json_object_get_string_member(attachment, "id"),
                    "QUARANTINED", attachment_message, error))
                ok = FALSE;
            if (ok && !pcv_vpc_store_set_resource_state(
                    s, "vpcs", json_object_get_string_member(attachment, "vpc_id"),
                    "DEGRADED", attachment_message, error))
                ok = FALSE;
        }
    }
                                                             
    for (guint i = 0; ok && subnets && i < json_array_get_length(subnets); i++) {
        JsonObject *subnet = json_array_get_object_element(subnets, i);
        if (g_strcmp0(json_object_get_string_member(subnet, "state"), "DELETING") != 0)
            ok = _restart_dhcp(s, subnet, error);
    }
    g_autoptr(JsonArray) publishes = ok
        ? pcv_vpc_store_list_publishes(s, NULL, NULL, error) : NULL;
    if (ok && !publishes) ok = FALSE;
    for (guint i = 0; ok && publishes && i < json_array_get_length(publishes); i++) {
        JsonObject *publish = json_array_get_object_element(publishes, i);
        if (g_strcmp0(json_object_get_string_member(publish, "state"), "DELETING") == 0) {
            ok = pcv_vpc_store_delete_publish(
                s, json_object_get_string_member(publish, "id"), NULL, error);
            continue;
        }
        g_autoptr(GError) publish_error = NULL;
        if (_reconcile_publish_actual(s, publish, &publish_error)) {
            ok = pcv_vpc_store_set_resource_state(
                s, "service_publishes", json_object_get_string_member(publish, "id"),
                "ACTIVE", NULL, error);
        } else {
            const gchar *publish_message = publish_error && publish_error->message
                ? publish_error->message : "Service Publish reconcile 실패";
            degraded = TRUE;
            if (!degraded_message) degraded_message = g_strdup(publish_message);
            if (!pcv_vpc_store_set_resource_state(
                    s, "service_publishes", json_object_get_string_member(publish, "id"),
                    "ERROR", publish_message, error))
                ok = FALSE;
            if (ok && !pcv_vpc_store_set_resource_state(
                    s, "vpcs", json_object_get_string_member(publish, "vpc_id"),
                    "DEGRADED", publish_message, error))
                ok = FALSE;
        }
    }
    if (ok) ok = _apply_full_policy(s, error);
    if (ok) ok = _clear_ovn_quarantine(s, error);
    for (guint i = 0; ok && subnets && i < json_array_get_length(subnets); i++) {
        JsonObject *subnet = json_array_get_object_element(subnets, i);
        if (g_strcmp0(json_object_get_string_member(subnet, "state"), "DELETING") != 0)
            ok = pcv_vpc_store_set_resource_state(
                s, "subnets", json_object_get_string_member(subnet, "id"),
                "ACTIVE", NULL, error);
    }
                                                             
    for (guint i = 0; ok && !degraded && i < json_array_get_length(vpcs); i++)
        ok = pcv_vpc_store_set_resource_state(
            s, "vpcs", json_object_get_string_member(
                json_array_get_object_element(vpcs, i), "id"),
            "ACTIVE", NULL, error);
    if (!ok) _managed_bridges_down(s);
    if (ok && degraded) {
        g_set_error(error, PCV_VPC_ERROR, PCV_VPC_ERROR_STATE,
                    "VPC는 fail-closed로 수렴했지만 일부 attachment가 quarantine 상태입니다: %s",
                    degraded_message ? degraded_message : "unknown");
        ok = FALSE;
    }
    g_mutex_unlock(&g_mutation_mu); return ok;
}
