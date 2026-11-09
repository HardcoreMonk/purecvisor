   
                      
                                                     
  
                           
                                                                                           
                                                                                                                     
                                                                             
  
                                                  
   

#include "modules/virt/vm_manager.h"
#include "modules/virt/virt_conn_pool.h"
#include "utils/pcv_config.h"
#include "modules/network/pcv_qos.h"
#include "modules/network/tenant_overlay.h"
#include "modules/network/vm_iface.h"
#include "modules/network/dpdk_manager.h"
#if PCV_CLUSTER_ENABLED
#include "modules/cluster/cluster_manager.h"
#endif
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "utils/pcv_validate.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "modules/virt/vm_manager_internal.h"


   
                                                                            
  
                                                                       
                                                     
                                 
  
                                                        
                                                    
                                                            
                                  
  
                                                           
                                                 
                                                
                                               
  
                                                     
                                               
  
                                               
                                                    
                                                                   
                                                
   
gchar *
pcv_vm_xml_inject_metadata_child(const gchar *xml, const gchar *child_xml)
{
    if (!xml)
        return NULL;                                              
    if (!child_xml || !*child_xml)
        return g_strdup(xml);                                         

                                                                  
                                                         
                                                   
    const gchar *meta_close = strstr(xml, "</metadata>");
    if (meta_close) {
        gsize prefix_len = (gsize)(meta_close - xml);                            
        return g_strdup_printf("%.*s    %s%s",
                               (gint)prefix_len, xml,
                               child_xml, meta_close);
    }

                                                                
    gchar *metadata = g_strdup_printf(
        "  <metadata>\n"
        "    %s"
        "  </metadata>\n",
        child_xml);

                                                           
                                                         
                                                      
    const gchar *insert = strstr(xml, "</name>");
    gchar *patched = NULL;
    if (insert) {
        insert += strlen("</name>");                                              
        patched = g_strdup_printf("%.*s%s%s",
                                  (gint)(insert - xml), xml,
                                  metadata, insert);
    } else {
        insert = strstr(xml, "<devices>");                    
        if (insert) {
            patched = g_strdup_printf("%.*s%s%s",
                                      (gint)(insert - xml), xml,
                                      metadata, insert);
        } else {
            const gchar *end = strstr(xml, "</domain>");                    
            patched = end
                ? g_strdup_printf("%.*s%s%s", (gint)(end - xml), xml, metadata, end)
                : g_strdup(xml);                                  
        }
    }
    g_free(metadata);
    return patched;
}

   
                                                                              
                                                       
   
gchar *
_dpdk_metadata_xml(const gchar *nic_type, const gchar *bridge_name)
{
    if (g_strcmp0(nic_type, "dpdk") != 0)
        return g_strdup("");
    gchar *safe_bridge = g_markup_escape_text(bridge_name ? bridge_name : "", -1);
    gchar *xml = g_strdup_printf(
        "<pcv:dpdk xmlns:pcv='%s' bridge='%s'/>\n",
        PCV_DPDK_METADATA_URI, safe_bridge);
    g_free(safe_bridge);
    return xml;
}

                                                      
gboolean
_dpdk_metadata_parse(const gchar *metadata_xml, gchar **bridge_out)
{
    if (!metadata_xml || !bridge_out)
        return FALSE;
    *bridge_out = NULL;
    GRegex *bridge_re = g_regex_new(
        "<(?:[\\w.-]+:)?dpdk\\b[^>]*\\bbridge\\s*=\\s*[\"']([^\"']*)[\"']",
        0, 0, NULL);
    GMatchInfo *match = NULL;
    gchar *bridge = NULL;
    if (bridge_re && g_regex_match(bridge_re, metadata_xml, 0, &match))
        bridge = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);
    if (bridge_re)
        g_regex_unref(bridge_re);
    if (!bridge || !pcv_validate_bridge_name(bridge)) {
        g_free(bridge);
        return FALSE;
    }
    *bridge_out = bridge;
    return TRUE;
}

                                                                    
                                                                           
                                                 
static PcvDpdkVhostSourceResult
_dpdk_vhost_source_classify_doc(xmlDocPtr doc, const gchar *vm_name,
                                xmlNodePtr *source_out)
{
    if (source_out)
        *source_out = NULL;
    if (!doc || !pcv_validate_vm_name(vm_name))
        return PCV_DPDK_VHOST_SOURCE_INVALID;

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root || root->type != XML_ELEMENT_NODE ||
        xmlStrcmp(root->name, BAD_CAST "domain") != 0)
        return PCV_DPDK_VHOST_SOURCE_INVALID;

    xmlNodePtr devices = NULL;
    xmlChar *domain_name = NULL;
    guint devices_count = 0;
    for (xmlNodePtr child = root->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE)
            continue;
        if (xmlStrcmp(child->name, BAD_CAST "name") == 0 && !domain_name)
            domain_name = xmlNodeGetContent(child);
        if (xmlStrcmp(child->name, BAD_CAST "devices") == 0) {
            devices = child;
            devices_count++;
        }
    }
    gboolean name_matches = domain_name &&
        g_strcmp0(g_strstrip((gchar *)domain_name), vm_name) == 0;
    if (domain_name)
        xmlFree(domain_name);
    if (!name_matches || devices_count > 1)
        return PCV_DPDK_VHOST_SOURCE_INVALID;
    if (!devices)
        return PCV_DPDK_VHOST_SOURCE_NONE;

    xmlNodePtr vhost_iface = NULL;
    guint vhost_count = 0;
    for (xmlNodePtr child = devices->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE ||
            xmlStrcmp(child->name, BAD_CAST "interface") != 0)
            continue;
        xmlChar *type = xmlGetProp(child, BAD_CAST "type");
        if (type && xmlStrcmp(type, BAD_CAST "vhostuser") == 0) {
            vhost_iface = child;
            vhost_count++;
        }
        if (type)
            xmlFree(type);
    }
    if (vhost_count == 0)
        return PCV_DPDK_VHOST_SOURCE_NONE;
    if (vhost_count != 1)
        return PCV_DPDK_VHOST_SOURCE_INVALID;

    xmlNodePtr source = NULL;
    guint source_count = 0;
    for (xmlNodePtr child = vhost_iface->children; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE &&
            xmlStrcmp(child->name, BAD_CAST "source") == 0) {
            source = child;
            source_count++;
        }
    }
    if (source_count != 1)
        return PCV_DPDK_VHOST_SOURCE_INVALID;

    xmlChar *type = xmlGetProp(source, BAD_CAST "type");
    xmlChar *mode = xmlGetProp(source, BAD_CAST "mode");
    xmlChar *path = xmlGetProp(source, BAD_CAST "path");
    g_autofree gchar *canonical = pcv_dpdk_vhost_socket_path(vm_name);
    g_autofree gchar *legacy = g_strdup_printf(
        "/var/run/purecvisor/vhost-%s.sock", vm_name);
    PcvDpdkVhostSourceResult result = PCV_DPDK_VHOST_SOURCE_INVALID;
    if (type && mode && path && canonical &&
        xmlStrcmp(type, BAD_CAST "unix") == 0 &&
        xmlStrcmp(mode, BAD_CAST "server") == 0) {
        if (g_strcmp0((const gchar *)path, canonical) == 0)
            result = PCV_DPDK_VHOST_SOURCE_CANONICAL;
        else if (g_strcmp0((const gchar *)path, legacy) == 0)
            result = PCV_DPDK_VHOST_SOURCE_LEGACY;
    }
    if (type)
        xmlFree(type);
    if (mode)
        xmlFree(mode);
    if (path)
        xmlFree(path);
    if (source_out && (result == PCV_DPDK_VHOST_SOURCE_CANONICAL ||
                       result == PCV_DPDK_VHOST_SOURCE_LEGACY))
        *source_out = source;
    return result;
}

PcvDpdkVhostSourceResult
pcv_vm_dpdk_vhost_source_classify(const gchar *domain_xml,
                                  const gchar *vm_name)
{
    if (!domain_xml || !pcv_validate_vm_name(vm_name))
        return PCV_DPDK_VHOST_SOURCE_INVALID;
    xmlDocPtr doc = xmlReadMemory(
        domain_xml, (int)strlen(domain_xml), "dpdk-domain.xml", NULL,
        XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc)
        return PCV_DPDK_VHOST_SOURCE_INVALID;
    PcvDpdkVhostSourceResult result =
        _dpdk_vhost_source_classify_doc(doc, vm_name, NULL);
    xmlFreeDoc(doc);
    return result;
}

PcvDpdkVhostStartAction
pcv_vm_dpdk_vhost_start_action(gboolean active,
                               PcvDpdkVhostSourceResult source)
{
    if (source == PCV_DPDK_VHOST_SOURCE_CANONICAL)
        return PCV_DPDK_VHOST_START_CANONICAL;
    if (source == PCV_DPDK_VHOST_SOURCE_LEGACY)
        return active ? PCV_DPDK_VHOST_START_DEFER_LEGACY
                      : PCV_DPDK_VHOST_START_MIGRATE_LEGACY;
    return PCV_DPDK_VHOST_START_INVALID;
}

gchar *
pcv_vm_dpdk_vhost_migrate_legacy_xml(const gchar *domain_xml,
                                     const gchar *vm_name,
                                     GError **error)
{
    if (!domain_xml || !pcv_validate_vm_name(vm_name)) {
        g_set_error_literal(error, g_quark_from_static_string("vm-dpdk-vhost"), 1,
                            "Invalid DPDK domain XML or VM name");
        return NULL;
    }
    xmlDocPtr doc = xmlReadMemory(
        domain_xml, (int)strlen(domain_xml), "dpdk-domain.xml", NULL,
        XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) {
        g_set_error_literal(error, g_quark_from_static_string("vm-dpdk-vhost"), 1,
                            "Invalid DPDK domain XML");
        return NULL;
    }

    xmlNodePtr source = NULL;
    PcvDpdkVhostSourceResult current =
        _dpdk_vhost_source_classify_doc(doc, vm_name, &source);
    if (current == PCV_DPDK_VHOST_SOURCE_CANONICAL) {
        xmlFreeDoc(doc);
        return g_strdup(domain_xml);
    }
    if (current != PCV_DPDK_VHOST_SOURCE_LEGACY || !source) {
        xmlFreeDoc(doc);
        g_set_error_literal(error, g_quark_from_static_string("vm-dpdk-vhost"), 1,
                            "DPDK vhost source is not exact legacy server endpoint");
        return NULL;
    }

    g_autofree gchar *canonical = pcv_dpdk_vhost_socket_path(vm_name);
    if (!canonical || !xmlSetProp(source, BAD_CAST "path", BAD_CAST canonical)) {
        xmlFreeDoc(doc);
        g_set_error_literal(error, g_quark_from_static_string("vm-dpdk-vhost"), 1,
                            "Failed to set canonical DPDK vhost source");
        return NULL;
    }

    xmlChar *buffer = NULL;
    int buffer_size = 0;
    xmlDocDumpMemoryEnc(doc, &buffer, &buffer_size, "UTF-8");
    xmlFreeDoc(doc);
    if (!buffer || buffer_size <= 0) {
        if (buffer)
            xmlFree(buffer);
        g_set_error_literal(error, g_quark_from_static_string("vm-dpdk-vhost"), 1,
                            "Failed to serialize canonical DPDK domain XML");
        return NULL;
    }
    gchar *migrated = g_strndup((const gchar *)buffer, (gsize)buffer_size);
    xmlFree(buffer);
    if (pcv_vm_dpdk_vhost_source_classify(migrated, vm_name) !=
        PCV_DPDK_VHOST_SOURCE_CANONICAL) {
        g_free(migrated);
        g_set_error_literal(error, g_quark_from_static_string("vm-dpdk-vhost"), 1,
                            "Canonical DPDK vhost migration postcondition failed");
        return NULL;
    }
    return migrated;
}

                                                          
                                                                 
                                                                  
static gboolean
_dpdk_vhost_interface_present(const gchar *domain_xml)
{
    return domain_xml && g_regex_match_simple(
        "<interface\\b[^>]*\\btype\\s*=\\s*[\"']vhostuser[\"']",
        domain_xml, 0, 0);
}

PcvDpdkMetaResult
pcv_vm_dpdk_metadata_read(virDomainPtr dom, gchar **bridge_out)
{
    if (!dom || !bridge_out)
        return PCV_DPDK_META_INVALID;
    *bridge_out = NULL;
    char *meta = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                      PCV_DPDK_METADATA_URI, 0);
    if (!meta) {
        virErrorPtr last = virGetLastError();
        gboolean metadata_absent = last && last->code == VIR_ERR_NO_DOMAIN_METADATA;
        virResetLastError();
        if (!metadata_absent)
            return PCV_DPDK_META_INVALID;

                                                                         
                                                                   
                                                           
        char *domain_xml = virDomainGetXMLDesc(dom, 0);
        if (!domain_xml) {
            virResetLastError();
            return PCV_DPDK_META_INVALID;
        }
        gboolean legacy_dpdk = _dpdk_vhost_interface_present(domain_xml);
        free(domain_xml);                      
        return legacy_dpdk ? PCV_DPDK_META_INVALID : PCV_DPDK_META_ABSENT;
    }
    gboolean parsed = _dpdk_metadata_parse(meta, bridge_out);
    free(meta);                      
    return parsed ? PCV_DPDK_META_OK : PCV_DPDK_META_INVALID;
}

                                                           
                                                                      
                                                         
                                                                 
                        

   
                                                                                  
  
                                                                           
                                                             
                                                                         
                                                        
                               
  
                                                             
                                                   
               
  
                                                 
                                                    
                                                  
                                 
  
                                                  
                                                                   
                                                         
                                                            
                                                      
   
gchar *
_overlay_metadata_xml(const gchar *network_mode, const gchar *tenant)
{
    if (g_strcmp0(network_mode, "tenant-overlay") != 0)
        return g_strdup("");

    gchar *safe_mode = g_markup_escape_text(network_mode, -1);
    gchar *safe_tenant = g_markup_escape_text(tenant ? tenant : "", -1);
    gchar *xml = g_strdup_printf(
        "<pcv:overlay xmlns:pcv='%s' network_mode='%s' tenant='%s'/>\n",
        PCV_OVERLAY_METADATA_URI, safe_mode, safe_tenant);
    g_free(safe_mode);
    g_free(safe_tenant);
    return xml;
}

   
                                                                           
  
                                                             
                                                        
                                       
  
                                               
                                                                                
                                                                     
                 
                                                                       
                                                                          
                                                             
                     
                                                              
                                          
  
                                                   
                                                  
                                                 
                                       
  
                                                                 
                                                                     
                                                               
                                                 
   
gboolean
_overlay_metadata_parse(const gchar *metadata_xml, gchar **mode_out, gchar **tenant_out)
{
    if (!metadata_xml || !mode_out || !tenant_out)
        return FALSE;

    gchar *mode = NULL, *tenant = NULL;
                                                                          
                                                                          
    GRegex *mode_re = g_regex_new(
        "<(?:[\\w.-]+:)?overlay\\b[^>]*\\bnetwork_mode\\s*=\\s*[\"']([^\"']*)[\"']", 0, 0, NULL);
    GRegex *tenant_re = g_regex_new(
        "<(?:[\\w.-]+:)?overlay\\b[^>]*\\btenant\\s*=\\s*[\"']([^\"']*)[\"']", 0, 0, NULL);
    GMatchInfo *match = NULL;

                                                                
    if (mode_re && g_regex_match(mode_re, metadata_xml, 0, &match))
        mode = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);                                 

                                                           
    if (tenant_re && g_regex_match(tenant_re, metadata_xml, 0, &match))
        tenant = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);

    if (mode_re) g_regex_unref(mode_re);
    if (tenant_re) g_regex_unref(tenant_re);

                                                                  
    if (!mode || !tenant) {
        g_free(mode);
        g_free(tenant);
        return FALSE;
    }

    *mode_out = mode;
    *tenant_out = tenant;
    return TRUE;
}

   
                                                               
  
                                                    
                                                   
  
                                                                
                                                 
                                                       
  
                                                           
                                                     
                                                                           
                                                                     
                                                           
                                                      
                                                          
                                               
                                                     
                                                          
                                                                    
                                               
                                                                     
                                                                
                                                               
                                                            
                                                      
                                                 
                                                
                                                         
                                                                
                                                         
  
                                             
                                                           
                                                           
   
gchar *
_qos_metadata_xml(const PcvQosSla *sla)
{
    if (!sla)
        return g_strdup("");

    return g_strdup_printf(
        "<qos min_mbps='%u' max_mbps='%u' burst_kb='%u'/>\n",
        sla->min_mbps, sla->max_mbps, sla->burst_kb);
}

   
                                                                                  
  
                                                            
                                                      
                                                       
                                                 
                                                  
                                    
  
                                                     
                                                        
  
                                           
                                                                    
                                                 
   
gboolean
_qos_metadata_parse(const gchar *metadata_xml, PcvQosSla *out)
{
    if (!metadata_xml || !out)
        return FALSE;

    GRegex *min_re = g_regex_new(
        "<(?:[\\w.-]+:)?qos\\b[^>]*\\bmin_mbps\\s*=\\s*[\"']([0-9]+)[\"']", 0, 0, NULL);
    GRegex *max_re = g_regex_new(
        "<(?:[\\w.-]+:)?qos\\b[^>]*\\bmax_mbps\\s*=\\s*[\"']([0-9]+)[\"']", 0, 0, NULL);
    GRegex *burst_re = g_regex_new(
        "<(?:[\\w.-]+:)?qos\\b[^>]*\\bburst_kb\\s*=\\s*[\"']([0-9]+)[\"']", 0, 0, NULL);
    GMatchInfo *match = NULL;
    gchar *min_s = NULL, *max_s = NULL, *burst_s = NULL;

    if (min_re && g_regex_match(min_re, metadata_xml, 0, &match))
        min_s = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);

    if (max_re && g_regex_match(max_re, metadata_xml, 0, &match))
        max_s = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);

    if (burst_re && g_regex_match(burst_re, metadata_xml, 0, &match))
        burst_s = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);

    if (min_re) g_regex_unref(min_re);
    if (max_re) g_regex_unref(max_re);
    if (burst_re) g_regex_unref(burst_re);

                                                                
    if (!min_s || !max_s) {
        g_free(min_s);
        g_free(max_s);
        g_free(burst_s);
        return FALSE;
    }

    memset(out, 0, sizeof *out);                                             
    out->min_mbps = (guint32)g_ascii_strtoull(min_s, NULL, 10);
    out->max_mbps = (guint32)g_ascii_strtoull(max_s, NULL, 10);
    out->burst_kb = burst_s ? (guint32)g_ascii_strtoull(burst_s, NULL, 10) : 256;                         

    g_free(min_s);
    g_free(max_s);
    g_free(burst_s);
    return TRUE;
}

   
                                                                     
  
                                                    
                                             
  
                                                                     
                                                                    
  
                                                        
                                                                    
                                                   
                            
   
gboolean
pcv_vm_qos_metadata_write(virDomainPtr dom, const PcvQosSla *sla, GError **error)
{
    if (!dom || !sla) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "pcv_vm_qos_metadata_write: dom/sla required");
        return FALSE;
    }

                                                              
    int active = virDomainIsActive(dom);
    if (active < 0) {
        virErrorPtr vir_err = virGetLastError();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to read VM active state: %s",
                    vir_err ? vir_err->message : "unknown");
        return FALSE;
    }
                                                         
    unsigned int flags = VIR_DOMAIN_AFFECT_CONFIG;
    if (active == 1)
        flags |= VIR_DOMAIN_AFFECT_LIVE;

    gchar *xml = _qos_metadata_xml(sla);
    int ret = virDomainSetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT, xml,
                                   "pcv", PCV_QOS_METADATA_URI, flags);
    g_free(xml);

    if (ret != 0) {
        virErrorPtr vir_err = virGetLastError();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "virDomainSetMetadata(qos) failed: %s",
                    vir_err ? vir_err->message : "unknown");
        return FALSE;
    }
    return TRUE;
}

   
                                                                   
                                  
  
                                                     
                                                     
                                                         
                                     
  
                                                                       
                             
  
                                                                            
                                              
                                     
  
                                                            
                                                               
                                                    
                                         
                                                     
                                                       
                                           
   
PcvQosMetaResult
pcv_vm_qos_metadata_read(virDomainPtr dom, PcvQosSla *out)
{
    if (!dom || !out)
        return PCV_QOS_META_INVALID;

    char *meta = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                      PCV_QOS_METADATA_URI, 0);
    if (!meta) {
                                                                  
                                                            
        virErrorPtr vir_err = virGetLastError();
        gboolean truly_absent = vir_err && vir_err->code == VIR_ERR_NO_DOMAIN_METADATA;
        virResetLastError();                                            
        return truly_absent ? PCV_QOS_META_ABSENT : PCV_QOS_META_INVALID;
    }

                                                        
    gboolean parsed = _qos_metadata_parse(meta, out);
    free(meta);                                                            
    return parsed ? PCV_QOS_META_OK : PCV_QOS_META_INVALID;
}

   
                                                           
                                                                         
                                               
                   
  
                                                           
                                                
                                 
  
                                                  
                                                        
                                       
  
                                                                       
                         
   
static PcvQosSla
_qos_default_tier_sla(void)
{
    gint uplink_cfg = pcv_config_get_int("qos", "uplink_mbps", 1000);
    guint32 default_max_mbps;
    if (uplink_cfg <= 0) {
        PCV_LOG_WARN("vm_manager",
            "[qos] qos.uplink_mbps 설정값(%d)이 비합리(<=0) — 기본값 1000Mbit 로 폴백",
            uplink_cfg);
        default_max_mbps = 1000;
    } else {
        default_max_mbps = (guint32)uplink_cfg;
    }

    PcvQosSla sla;
    memset(&sla, 0, sizeof sla);
    sla.min_mbps = 0;
    sla.max_mbps = default_max_mbps;
    sla.burst_kb = 256;
    return sla;
}

   
                                                                        
                                                                 
  
                                                      
                                                    
                                                 
  
                                                                           
                                                                                
                                                                  
                                                      
  
                                                          
                                               
                                                    
                                                 
                                                  
   
void
pcv_qos_backfill_existing(void)
{
    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        PCV_LOG_WARN("vm_manager", "[qos-migrate] libvirt 연결 실패 — backfill 건너뜀");
        return;
    }

    virDomainPtr *domains = NULL;
    int n = virConnectListAllDomains(conn, &domains, 0);
    if (n < 0) {
        PCV_LOG_WARN("vm_manager", "[qos-migrate] virConnectListAllDomains 실패");
        virt_conn_pool_release(conn);
        return;
    }

    PcvQosSla default_sla = _qos_default_tier_sla();

    guint backfilled = 0;
    guint skipped_invalid = 0;
                                                               
    for (int i = 0; i < n; i++) {
        PcvQosSla existing;
        PcvQosMetaResult meta_state = pcv_vm_qos_metadata_read(domains[i], &existing);

                                                         
        if (meta_state == PCV_QOS_META_ABSENT) {
            GError *werr = NULL;
            if (pcv_vm_qos_metadata_write(domains[i], &default_sla, &werr)) {
                backfilled++;
            } else {
                const char *name = virDomainGetName(domains[i]);
                PCV_LOG_WARN("vm_manager",
                    "[qos-migrate] VM '%s' 기본 SLA backfill 실패: %s",
                    name ? name : "?", werr ? werr->message : "unknown");
                g_clear_error(&werr);
            }
        } else if (meta_state == PCV_QOS_META_INVALID) {
                                                         
                                                  
                              
            const char *name = virDomainGetName(domains[i]);
                                                                     
                                                               
                                                              
                         
            PCV_LOG_WARN("vm_manager",
                "[qos-migrate] VM '%s' qos 메타데이터 읽기/파싱 실패(부재 아님 — "
                "덮어쓰기 회피) — 기본 티어로 덮어쓰지 않음, 수동 확인 필요", name ? name : "?");
            skipped_invalid++;
        }
                                                    

        virDomainFree(domains[i]);
    }
    free(domains);                                             
    virt_conn_pool_release(conn);

    if (backfilled > 0) {
        PCV_LOG_INFO("vm_manager", "[qos-migrate] backfilled default SLA to %u VMs", backfilled);
    }
    if (skipped_invalid > 0) {
        PCV_LOG_WARN("vm_manager",
            "[qos-migrate] skipped %u VMs with unparseable qos metadata (manual repair needed)",
            skipped_invalid);
    }
}

                                                                             
                                                                    
  
                                                              
                                                   
                                                                
                                                                   
                                                  
                                                      
                                                 
  
                                                    
                                                            
                                                              
                                                     
                                                      
                                                              
                          
                                                                                

                                                        
                                                   
                                                        
                                                          
  
                                                              
                                                              
                    
  
                                                                   
                                                                               
                                                            
                                                  
                                              
                                                                          
                                                                  
                                              
  
                                                                        
                                                                   
static gboolean
_qos_derive_tenant_iface(virDomainPtr dom, const gchar *vm_name,
                          gchar **tenant_out, gchar **iface_out)
{
    *tenant_out = NULL;
    *iface_out  = NULL;

    char *meta = virDomainGetMetadata(dom, VIR_DOMAIN_METADATA_ELEMENT,
                                      PCV_OVERLAY_METADATA_URI, 0);
    gchar *mode = NULL, *tenant = NULL;
    gboolean is_overlay = FALSE;
    if (meta) {
        if (_overlay_metadata_parse(meta, &mode, &tenant) &&
            g_strcmp0(mode, "tenant-overlay") == 0)
            is_overlay = TRUE;
        free(meta);                                                             
    } else {
                                                                      
                                                     
                                                                       
        virResetLastError();
    }

    if (is_overlay) {
        gchar *ep = NULL;
        if (pcv_tenant_overlay_get_member_ep(tenant, vm_name, &ep) && ep && *ep) {
            *tenant_out = tenant;               
            *iface_out  = g_strdup_printf("%s-h", ep);
            g_free(ep);
            g_free(mode);
            return TRUE;
        }
        g_free(ep);
        PCV_LOG_WARN("vm_manager",
            "[qos] VM '%s' overlay metadata 존재하나 mesh 미조인 — iface 발견 불가"
            "(경합 또는 구 도메인, reconcile 이 배선되면 다음 주기가 재시도)", vm_name);
        g_free(tenant);
        g_free(mode);
        return FALSE;
    }
    g_free(mode);
    g_free(tenant);

                                                                   
                                 
    GPtrArray *ifaces = pcv_vm_iface_list(vm_name);
    gchar *vnet = (ifaces->len > 0) ? g_strdup(g_ptr_array_index(ifaces, 0)) : NULL;
    g_ptr_array_unref(ifaces);
    if (!vnet)
        return FALSE;                                          

    *tenant_out = g_strdup("default");                                      
    *iface_out  = vnet;
    return TRUE;
}

   
                                                                                      
  
                                                    
                                                    
                           
  
                                                                                  
                                                                        
                                                                  
                                                                      
   
gboolean
pcv_vm_qos_derive_context(virDomainPtr dom, const gchar *vm_name,
                          gchar **tenant_out, gchar **iface_out,
                          PcvQosSla *sla_out)
{
    if (tenant_out) *tenant_out = NULL;
    if (iface_out)  *iface_out  = NULL;

    if (!dom || !vm_name || !*vm_name || !sla_out)
        return FALSE;

    gchar *tenant = NULL, *iface = NULL;
    if (!_qos_derive_tenant_iface(dom, vm_name, &tenant, &iface)) {
        g_free(tenant);
        g_free(iface);
        return FALSE;
    }

                                                          
                                                    
                                                        
                                         
    PcvQosMetaResult mstate = pcv_vm_qos_metadata_read(dom, sla_out);
    if (mstate != PCV_QOS_META_OK) {
        if (mstate == PCV_QOS_META_INVALID)
            PCV_LOG_WARN("vm_manager",
                "[qos] VM '%s' qos 메타데이터 읽기/파싱 실패(부재 아님 — 덮어쓰기 회피) — "
                "런타임 기본 티어만 적용(영속 미변경)", vm_name);
        *sla_out = _qos_default_tier_sla();
    }
    g_strlcpy(sla_out->tenant, tenant, sizeof sla_out->tenant);
    g_strlcpy(sla_out->vm, vm_name, sizeof sla_out->vm);

    if (tenant_out) *tenant_out = tenant; else g_free(tenant);
    if (iface_out)  *iface_out  = iface;  else g_free(iface);
    return TRUE;
}

   
                                                                                
                                                                      
  
                                                             
                                                    
                                                               
                                                       
                
  
                                                     
                                                       
                               
  
                                                                    
                                                
   
GPtrArray *
pcv_vm_qos_expected_provider(void)
{
    GPtrArray *entries = g_ptr_array_new_with_free_func(g_free);

    virConnectPtr conn = virt_conn_pool_acquire();
    if (!conn) {
        PCV_LOG_WARN("vm_manager", "[qos] expected-provider: libvirt 연결 실패 — 빈 목록 반환");
        return entries;
    }

    virDomainPtr *domains = NULL;
    int n = virConnectListAllDomains(conn, &domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
    if (n < 0) {
        PCV_LOG_WARN("vm_manager", "[qos] expected-provider: virConnectListAllDomains 실패");
        virt_conn_pool_release(conn);
        return entries;
    }

    for (int i = 0; i < n; i++) {
        const char *name = virDomainGetName(domains[i]);
        if (name && *name) {
            gchar *tenant = NULL, *iface = NULL;
            PcvQosSla sla;
            if (pcv_vm_qos_derive_context(domains[i], name, &tenant, &iface, &sla)) {
                PcvQosExpectedEntry *e = g_new0(PcvQosExpectedEntry, 1);
                g_strlcpy(e->tenant, tenant, sizeof e->tenant);
                g_strlcpy(e->vm, name, sizeof e->vm);
                g_strlcpy(e->iface, iface, sizeof e->iface);
                e->sla = sla;                                                                
                g_ptr_array_add(entries, e);
            }
            g_free(tenant);
            g_free(iface);
        }
        virDomainFree(domains[i]);
    }
    free(domains);                                             
    virt_conn_pool_release(conn);

    return entries;
}

   
                                                                             
                                      
  
                                                                        
                                                                          
                                                      
      
  
                                                                       
                                                                  
                                                                       
                                                                
  
                                                             
                                                     
                                                   
                                 
  
                                                             
                                                                    
                                                                
                                                         
   
gboolean
_overlay_live_iface_parse(const gchar *domain_xml, gchar **tap_out, gchar **mac_out)
{
    if (!domain_xml || !tap_out || !mac_out)
        return FALSE;
    *tap_out = NULL;
    *mac_out = NULL;

                                                                      
                                                        
                                                                    
    GRegex *iface_re = g_regex_new(
        "<interface\\b[^>]*\\btype='ethernet'[^>]*>(.*?)</interface>",
        G_REGEX_DOTALL, 0, NULL);
    if (!iface_re)
        return FALSE;

    GMatchInfo *mi = NULL;
    gchar *block = NULL;
    guint eth_count = 0;
    if (g_regex_match(iface_re, domain_xml, 0, &mi)) {
        while (g_match_info_matches(mi)) {
            eth_count++;
            if (!block)
                block = g_match_info_fetch(mi, 1);                          
            g_match_info_next(mi, NULL);
        }
    }
    g_clear_pointer(&mi, g_match_info_free);
    g_regex_unref(iface_re);

    if (!block)
        return FALSE;                                                  
    if (eth_count > 1)
        PCV_LOG_WARN("vm_manager",
                     "tenant-overlay 도메인에 ethernet iface 가 %u 개 — 첫 번째만 배선",
                     eth_count);

    gchar *tap = NULL, *mac = NULL;
    GRegex *tap_re = g_regex_new("<target\\b[^>]*\\bdev='([^']*)'", 0, 0, NULL);
    GRegex *mac_re = g_regex_new("<mac\\b[^>]*\\baddress='([^']*)'", 0, 0, NULL);
    GMatchInfo *m2 = NULL;
    if (tap_re && g_regex_match(tap_re, block, 0, &m2))
        tap = g_match_info_fetch(m2, 1);
    g_clear_pointer(&m2, g_match_info_free);
    if (mac_re && g_regex_match(mac_re, block, 0, &m2))
        mac = g_match_info_fetch(m2, 1);
    g_clear_pointer(&m2, g_match_info_free);
    if (tap_re) g_regex_unref(tap_re);
    if (mac_re) g_regex_unref(mac_re);
    g_free(block);

                                                           
    if (!tap || !*tap || !mac || !*mac) {
        g_free(tap);
        g_free(mac);
        return FALSE;
    }
    *tap_out = tap;
    *mac_out = mac;
    return TRUE;
}

   
                                                                       
                               
  
                                                                      
                                                                
                                                                   
                                                    
  
                                                         
                                                                
  
                                    
                                                           
   
gchar *
_overlay_gw_cidr_from_subnet(const gchar *subnet_cidr)
{
    if (!subnet_cidr)
        return NULL;

    gchar **cidr_parts = g_strsplit(subnet_cidr, "/", 2);                            
                                           
    if (!cidr_parts[0] || !cidr_parts[1] || !*cidr_parts[1]) {
        g_strfreev(cidr_parts);
        return NULL;
    }

    gchar **octets = g_strsplit(cidr_parts[0], ".", -1);                          
    if (g_strv_length(octets) != 4) {                                   
        g_strfreev(octets);
        g_strfreev(cidr_parts);
        return NULL;
    }

    gchar *gw = g_strdup_printf("%s.%s.%s.1/%s",
                                octets[0], octets[1], octets[2], cidr_parts[1]);
    g_strfreev(octets);
    g_strfreev(cidr_parts);
    return gw;
}

   
                                                                                      
  
                                   
                                                            
                                                        
                                                    
                                                             
                                                         
                                                
  
                                                        
                                                 
                             
  
                                                    
                                                      
  
                                                            
   
gchar *
_overlay_ethernet_iface_xml(void)
{
    return g_strdup(
        "    <interface type='ethernet'>\n"
        "      <model type='virtio'/>\n"
        "    </interface>\n");
}
