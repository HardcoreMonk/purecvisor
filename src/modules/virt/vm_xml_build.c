   
                       
                                                      
  
                           
                                                                                           
                                                                                                                     
                                                                             
  
                                                 
   

#include "modules/virt/virt_conn_pool.h"
#include "modules/network/dpdk_manager.h"
#if PCV_CLUSTER_ENABLED
#include "modules/cluster/cluster_manager.h"
#endif
#include "utils/pcv_spawn.h"
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


#define PCV_VM_METADATA_URI "urn:purecvisor:metadata"

   
                                                                                
  
                                                  
                                                           
  
                                                          
                                                                              
                                                                         
   
gchar *
pcv_vm_xml_with_owner_metadata(const gchar *xml, const gchar *owner)
{
                           
                                                    
                                                 
                                                  
      
               
                                           
                                                                                 
                                                                 
                                   
    if (!xml || !owner || !*owner)
        return g_strdup(xml);

    gchar *safe_owner = g_markup_escape_text(owner, -1);
    gchar *owner_child = g_strdup_printf(
        "<pcv:owner xmlns:pcv='%s'>%s</pcv:owner>\n",
        PCV_VM_METADATA_URI, safe_owner);
    g_free(safe_owner);

    gchar *patched = pcv_vm_xml_inject_metadata_child(xml, owner_child);
    g_free(owner_child);

    return patched;
}

   
                                                                                
                                                         
                                                                      
                                                       
                                                                  
                                                         
   

   
                                                                       
  
                                                    
                                          
  
                                                                     
                                                           
                 
  
                            
                             
   
gint64
_hugepage_preflight_need_pages(gint ram_mb)
{
    return ((gint64)ram_mb + 1) / 2;                     
}

   
                                                                             
  
                                                    
                                                     
  
                                                               
                                                                               
                                                                                       
                                                    
                                    
                                                          
                                                           
  
                                         
                                                            
                                                    
   
gchar *
_build_memory_backing_xml(gboolean hugepages, gboolean want_shared)
{
    if (hugepages && want_shared) {
        return g_strdup("  <memoryBacking><hugepages><page size='2048' unit='KiB'/></hugepages>"
                        "<nosharepages/><access mode='shared'/></memoryBacking>\n");
    } else if (want_shared) {
        return g_strdup("  <memoryBacking><nosharepages/><source type='memfd'/>"
                        "<access mode='shared'/></memoryBacking>\n");
    } else if (hugepages) {
        return g_strdup("  <memoryBacking><hugepages><page size='2048' unit='KiB'/>"
                        "</hugepages><nosharepages/></memoryBacking>\n");
    } else {
        return g_strdup("  <memoryBacking><nosharepages/></memoryBacking>\n");
    }
}

   
                                                            
                                                                         
                                                                                   
                                                                            
                                                                       
                                                                           
                                                        
   

   
                                                                              
  
                                                         
                                                  
                          
  
                                                                           
                                                                   
                                                                
                                                           
                                                          
  
                                                              
                                                           
                                                                      
                                                    
                                                        
                                                          
  
                                             
                                                                  
                                                       
                                                                               
                                                                        
   
gchar *
_build_bridge_iface_xml(const gchar *safe_bridge,
                        const gchar *virtualport_xml,
                        const gchar *vlan_xml,
                        gint mtu)
{
    gchar *mtu_xml = (mtu > 0)
        ? g_strdup_printf("      <mtu size='%d'/>\n", mtu)
        : g_strdup("");
    gchar *xml = g_strdup_printf(
        "    <interface type='bridge'>\n"
        "      <source bridge='%s'/>\n"
        "%s"
        "      <model type='virtio'/>\n"
        "      <driver name='vhost'/>\n"
        "%s"
        "%s"
        "    </interface>\n",
        safe_bridge, virtualport_xml, mtu_xml, vlan_xml);
    g_free(mtu_xml);
    return xml;
}

   
                                                        
  
                                                    
                                                               
                                                           
                
  
                  
                                                                
                                                                 
                                                              
                                                                   
                                                      
          
  
                                
                                                             
   
gchar *
_build_dpdk_iface_xml(const gchar *vm_name)
{
    g_autofree gchar *socket_path = pcv_dpdk_vhost_socket_path(vm_name);
    if (!socket_path)
        return NULL;
    return g_strdup_printf(
        "    <interface type='vhostuser'>\n"
        "      <source type='unix' path='%s' mode='server'/>\n"
        "      <model type='virtio'/>\n"
        "      <driver queues='2' rx_queue_size='1024' tx_queue_size='256'/>\n"
        "    </interface>\n",
        socket_path);
}

   
                                                                                
  
                                                          
                                                            
  
                                                                   
                                                                     
                                                                              
                                                              
                                                               
                                                                  
                                              
                                                           
  
                                         
                                                          
   
gchar *
_vm_xml_ensure_memballoon_stats(const gchar *xml)
{
    if (!xml) return NULL;
    if (strstr(xml, "<stats period=")) return g_strdup(xml);                  

    const gchar *mb = strstr(xml, "<memballoon");
    if (mb) {
        const gchar *gt = strchr(mb, '>');
        if (!gt) return g_strdup(xml);                        
        if (*(gt - 1) == '/') {
                                                                            
            gsize head = (gsize)(gt - 1 - xml);
            return g_strdup_printf("%.*s><stats period='10'/></memballoon>%s",
                                   (gint)head, xml, gt + 1);
        }
                                                                    
        gsize head = (gsize)(gt + 1 - xml);
        return g_strdup_printf("%.*s<stats period='10'/>%s", (gint)head, xml, gt + 1);
    }

                                                        
    const gchar *dev = strstr(xml, "</devices>");
    if (!dev) return g_strdup(xml);
    gsize head = (gsize)(dev - xml);
    return g_strdup_printf("%.*s    <memballoon model='virtio'>\n"
                           "      <stats period='10'/>\n"
                           "    </memballoon>\n%s",
                           (gint)head, xml, dev);
}

   
                                                             
  
                                                       
                                          
  
                                                           
                                                                  
                                                           
                                   
  
                           
                                                          
   
gchar *
_vm_xml_insert_virtio_rng(const gchar *xml)
{
    if (!xml) return NULL;
    if (strstr(xml, "<rng")) return g_strdup(xml);                  
    const gchar *dev = strstr(xml, "</devices>");
    if (!dev) return g_strdup(xml);
    gsize head = (gsize)(dev - xml);
    return g_strdup_printf("%.*s    <rng model='virtio'>\n"
                           "      <backend model='random'>/dev/urandom</backend>\n"
                           "    </rng>\n%s",
                           (gint)head, xml, dev);
}

   
                                                                              
  
                                                    
                                                
  
                                                           
                                                               
                                                               
                                                             
                                                                
                                                                   
                                                                      
                                                         
  
                           
                                                      
   
gchar *
_vm_xml_ensure_q35_hotplug_ports(const gchar *xml)
{
    constexpr guint required_ports = 8;
    if (!xml) return NULL;

    xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), "domain.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return g_strdup(xml);

    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr devices = NULL;
    gboolean has_devices = FALSE;
    gboolean is_q35 = FALSE;
    gboolean has_pcie_root = FALSE;
    guint ports = 0;
    for (xmlNodePtr child = root ? root->children : NULL; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) continue;
        if (xmlStrcmp(child->name, BAD_CAST "os") == 0) {
            for (xmlNodePtr os_child = child->children; os_child; os_child = os_child->next) {
                if (os_child->type != XML_ELEMENT_NODE ||
                    xmlStrcmp(os_child->name, BAD_CAST "type") != 0) continue;
                xmlChar *machine = xmlGetProp(os_child, BAD_CAST "machine");
                if (machine && strstr((const gchar *)machine, "q35")) is_q35 = TRUE;
                if (machine) xmlFree(machine);
            }
        } else if (xmlStrcmp(child->name, BAD_CAST "devices") == 0) {
            devices = child;
            has_devices = TRUE;
        }
    }

    if (is_q35 && devices) {
        for (xmlNodePtr child = devices->children; child; child = child->next) {
            if (child->type != XML_ELEMENT_NODE ||
                xmlStrcmp(child->name, BAD_CAST "controller") != 0) continue;
            xmlChar *type = xmlGetProp(child, BAD_CAST "type");
            xmlChar *model = xmlGetProp(child, BAD_CAST "model");
            if (type && model && xmlStrcmp(type, BAD_CAST "pci") == 0 &&
                xmlStrcmp(model, BAD_CAST "pcie-root-port") == 0)
                ports++;
            if (type && model && xmlStrcmp(type, BAD_CAST "pci") == 0 &&
                xmlStrcmp(model, BAD_CAST "pcie-root") == 0)
                has_pcie_root = TRUE;
            if (type) xmlFree(type);
            if (model) xmlFree(model);
        }
    }
    xmlFreeDoc(doc);

    if (!is_q35 || !has_devices || (has_pcie_root && ports >= required_ports))
        return g_strdup(xml);
    const gchar *insert = g_strrstr(xml, "</devices>");
    if (!insert) return g_strdup(xml);

    GString *controllers = g_string_new(NULL);
    if (!has_pcie_root) {
                                                                        
                                                        
        g_string_append(controllers,
            "    <controller type='pci' index='0' model='pcie-root'/>\n");
    }
    for (guint i = ports; i < required_ports; i++) {
        g_string_append(controllers,
            "    <controller type='pci' model='pcie-root-port'/>\n");
    }
    gchar *patched = g_strdup_printf("%.*s%s%s", (gint)(insert - xml), xml,
                                     controllers->str, insert);
    g_string_free(controllers, TRUE);
    return patched;
}

   
                                                                                 
  
                                                         
                                                         
                                            
  
                                                                            
                                                                                       
                                                                         
                                                               
                     
  
                                                  
                                                                       
                                                       
                                                                                     
                                                                 
  
                                                                           
                                                                               
                                                   
  
                           
                                                          
   
gchar *
_vm_xml_apply_cpu_invtsc(const gchar *xml)
{
    if (!xml) return NULL;
    if (strstr(xml, "invtsc")) return g_strdup(xml);                  
    const gchar *p = strstr(xml, "<cpu mode=\"");
    if (!p) return g_strdup(xml);                                 
    const gchar *gt = strchr(p, '>');
    if (!gt || *(gt - 1) != '/') return g_strdup(xml);                                    
    gsize head = (gsize)(gt - 1 - xml);                                  
    return g_strdup_printf(
        "%.*s migratable=\"off\"><feature policy=\"require\" name=\"invtsc\"/></cpu>%s",
        (gint)head, xml, gt + 1);
}
