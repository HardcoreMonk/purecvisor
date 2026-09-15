   
                      
                                                                    
  
                           
                                                   
                                                    
                                        
  
          
                                                     
                                                     


  
          
                                                    
                                                      
                                                  

  
             
                                                               
                                         
                                                            
                                                                   
                                                             
  
            
                                    
                                                              
                                       
  
          

                                                                           
                                                                               
                                                            

  
                       
                                    



  
           

  
           
                                                       
   
#include "gpu_manager.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "purecvisor/pcv_validate.h"
#include "modules/virt/virt_conn_pool.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#define GPU_LOG_DOM "gpu_manager"
#define GPU_PCI_SYSFS "/sys/bus/pci/devices"



static GMutex gpu_passthrough_mu;

static gchar *
_gpu_pci_attr_path(const gchar *pci_addr, const gchar *attr)
{
    return attr ? g_build_filename(GPU_PCI_SYSFS, pci_addr, attr, NULL)
                : g_build_filename(GPU_PCI_SYSFS, pci_addr, NULL);
}

static gchar *
_gpu_read_trimmed(const gchar *path)
{
    gchar *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL))
        return NULL;
    g_strstrip(text);
    return text;
}

static gchar *
_gpu_driver_name(const gchar *pci_addr)
{
    gchar *path = _gpu_pci_attr_path(pci_addr, "driver");
    gchar *target = g_file_read_link(path, NULL);
    gchar *driver = target ? g_path_get_basename(target) : g_strdup("none");
    g_free(target);
    g_free(path);
    return driver;
}

static gboolean
_gpu_parse_bdf(const gchar *pci_addr, guint *domain, guint *bus,
               guint *slot, guint *function)
{
    return pcv_validate_pci_addr(pci_addr) &&
           sscanf(pci_addr, "%x:%x:%x.%x", domain, bus, slot, function) == 4;
}

static gint
_gpu_bdf_compare(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(gchar * const *)a, *(gchar * const *)b);
}



static GPtrArray *
_gpu_iommu_slot_group(const gchar *pci_addr, GError **error)
{
    gchar *device_path = _gpu_pci_attr_path(pci_addr, NULL);
    gchar *group_link = g_build_filename(device_path, "iommu_group", NULL);
    gchar *group_target = g_file_read_link(group_link, NULL);
    if (!group_target) {
        g_set_error(error, g_quark_from_static_string("gpu"), 2,
                    "GPU %s has no IOMMU group", pci_addr);
        g_free(group_link);
        g_free(device_path);
        return NULL;
    }

    gchar *group_path = g_canonicalize_filename(group_target, device_path);
    gchar *devices_path = g_build_filename(group_path, "devices", NULL);
    GDir *dir = g_dir_open(devices_path, 0, NULL);
    GPtrArray *members = NULL;
    if (!dir) {
        g_set_error(error, g_quark_from_static_string("gpu"), 2,
                    "Cannot inspect IOMMU group for GPU %s", pci_addr);
        goto out;
    }

    members = g_ptr_array_new_with_free_func(g_free);
    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (!pcv_validate_pci_addr(name))
            continue;

        if (strncmp(name, pci_addr, 10) != 0) {
            g_set_error(error, g_quark_from_static_string("gpu"), 3,
                        "IOMMU group for GPU %s also contains unsafe device %s",
                        pci_addr, name);
            g_ptr_array_unref(members);
            members = NULL;
            break;
        }
        g_ptr_array_add(members, g_strdup(name));
    }
    g_dir_close(dir);

    if (members && members->len == 0) {
        g_set_error(error, g_quark_from_static_string("gpu"), 2,
                    "IOMMU group for GPU %s is empty", pci_addr);
        g_ptr_array_unref(members);
        members = NULL;
    }
    if (members)
        g_ptr_array_sort(members, _gpu_bdf_compare);

out:
    g_free(devices_path);
    g_free(group_path);
    g_free(group_target);
    g_free(group_link);
    g_free(device_path);
    return members;
}

static gboolean
_gpu_is_display_controller(const gchar *pci_addr)
{
    gchar *path = _gpu_pci_attr_path(pci_addr, "class");
    gchar *text = _gpu_read_trimmed(path);
    guint64 class_code = text ? g_ascii_strtoull(text, NULL, 0) : 0;
    g_free(text);
    g_free(path);
    return ((class_code >> 16) & 0xff) == 0x03;
}

static gchar *
_gpu_hostdev_xml(const gchar *pci_addr)
{
    guint domain = 0, bus = 0, slot = 0, function = 0;
    if (!_gpu_parse_bdf(pci_addr, &domain, &bus, &slot, &function))
        return NULL;
    return g_strdup_printf(
        "<hostdev mode='subsystem' type='pci' managed='no'>"
        "<source><address domain='0x%04x' bus='0x%02x' slot='0x%02x' "
        "function='0x%x'/></source></hostdev>",
        domain, bus, slot, function);
}

static xmlNodePtr
_gpu_xml_child(xmlNodePtr parent, const gchar *name)
{
    for (xmlNodePtr node = parent ? parent->children : NULL;
         node; node = node->next) {
        if (node->type == XML_ELEMENT_NODE &&
            xmlStrEqual(node->name, (const xmlChar *)name))
            return node;
    }
    return NULL;
}

static gboolean
_gpu_xml_uint(xmlNodePtr node, const gchar *name, guint max, guint *value)
{
    xmlChar *raw = xmlGetProp(node, (const xmlChar *)name);
    if (!raw)
        return FALSE;
    gchar *end = NULL;
    guint64 parsed = g_ascii_strtoull((const gchar *)raw, &end,
                                      g_str_has_prefix((const gchar *)raw, "0x") ? 16 : 10);
    gboolean ok = end && *end == '\0' && parsed <= max;
    if (ok)
        *value = (guint)parsed;
    xmlFree(raw);
    return ok;
}



static gboolean
_gpu_find_hostdev(const gchar *domain_xml, const gchar *pci_addr,
                  gchar **node_xml_out, gboolean *managed_no_out,
                  GError **error)
{
    *node_xml_out = NULL;
    *managed_no_out = FALSE;
    guint want_domain = 0, want_bus = 0, want_slot = 0, want_function = 0;
    if (!_gpu_parse_bdf(pci_addr, &want_domain, &want_bus,
                        &want_slot, &want_function))
        return FALSE;

    xmlDocPtr doc = xmlReadMemory(domain_xml, (gint)strlen(domain_xml),
                                  "pcv-gpu-domain.xml", NULL,
                                  XML_PARSE_NONET | XML_PARSE_NOERROR |
                                  XML_PARSE_NOWARNING);
    if (!doc) {
        g_set_error(error, g_quark_from_static_string("gpu"), 4,
                    "libvirt returned malformed inactive domain XML");
        return FALSE;
    }

    xmlNodePtr root = xmlDocGetRootElement(doc);
    xmlNodePtr devices = _gpu_xml_child(root, "devices");
    for (xmlNodePtr hostdev = devices ? devices->children : NULL;
         hostdev; hostdev = hostdev->next) {
        if (hostdev->type != XML_ELEMENT_NODE ||
            !xmlStrEqual(hostdev->name, (const xmlChar *)"hostdev"))
            continue;
        xmlChar *type = xmlGetProp(hostdev, (const xmlChar *)"type");
        gboolean pci_type = type && xmlStrEqual(type, (const xmlChar *)"pci");
        xmlFree(type);
        if (!pci_type)
            continue;
        xmlNodePtr source = _gpu_xml_child(hostdev, "source");
        xmlNodePtr address = _gpu_xml_child(source, "address");
        guint domain = 0, bus = 0, slot = 0, function = 0;
        if (!address ||
            !_gpu_xml_uint(address, "domain", 0xffff, &domain) ||
            !_gpu_xml_uint(address, "bus", 0xff, &bus) ||
            !_gpu_xml_uint(address, "slot", 0x1f, &slot) ||
            !_gpu_xml_uint(address, "function", 0x7, &function) ||
            domain != want_domain || bus != want_bus || slot != want_slot ||
            function != want_function)
            continue;

        xmlChar *managed = xmlGetProp(hostdev, (const xmlChar *)"managed");
        *managed_no_out = managed && xmlStrEqual(managed, (const xmlChar *)"no");
        xmlFree(managed);
        xmlBufferPtr buffer = xmlBufferCreate();
        if (!buffer || xmlNodeDump(buffer, doc, hostdev, 0, 0) < 0) {
            if (buffer)
                xmlBufferFree(buffer);
            xmlFreeDoc(doc);
            g_set_error(error, g_quark_from_static_string("gpu"), 4,
                        "Cannot serialize GPU hostdev from domain XML");
            return FALSE;
        }
        *node_xml_out = g_strdup((const gchar *)xmlBufferContent(buffer));
        xmlBufferFree(buffer);
        break;
    }
    xmlFreeDoc(doc);
    return TRUE;
}

static void
_gpu_set_libvirt_error(GError **error, const gchar *operation)
{
    virErrorPtr ve = virGetLastError();
    g_set_error(error, g_quark_from_static_string("gpu"), 5, "%s: %s",
                operation, ve && ve->message ? ve->message : "libvirt failure");
}

   
        
                        
                                          
                    
  
                                      
                                       
  
                        
  
                                                        
                                                  
                                                                
                                                             
                             
                                                                     
                                                 
                                                             
                                                   
                                       
  
                                               
                                                
                              
   
                                     
static gboolean
_run(const gchar *cmd, gchar **out, GError **error)
{
                                               
                                                             
    gchar **parsed = NULL;
    GError *pe = NULL;
    if (!g_shell_parse_argv(cmd, NULL, &parsed, &pe)) {                            
                                                           
        if (pe) { if (error) g_propagate_error(error, pe); else g_error_free(pe); }
        return FALSE;
    }
    gchar *se = NULL;
                                                             
    gboolean ok = pcv_spawn_sync((const gchar * const *)parsed, out, &se, error);
    if (!ok) PCV_LOG_WARN(GPU_LOG_DOM, "cmd failed: %s  err=%s", cmd, se ? se : "");                      
    g_free(se);                                                           
    g_strfreev(parsed);                      
    return ok;
}
                                        
  
                                                           
                                                  
                                                  
                                                                  
                                
                                                       
                                         
   
static gboolean
_run_shell(const gchar *cmd, gchar **out, GError **error)
{
                                                        
    const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
    gchar *se = NULL;
    gboolean ok = pcv_spawn_sync(argv, out, &se, error);
    if (!ok) PCV_LOG_WARN(GPU_LOG_DOM, "cmd failed: %s  err=%s", cmd, se ? se : "");
    g_free(se);
    return ok;
}

                                                           
                                                   
void pcv_gpu_init(void)  { PCV_LOG_INFO(GPU_LOG_DOM, "GPU manager initialized"); }
                                                     
                                              
void pcv_gpu_shutdown(void) {}

   
                
  
                                        

                                                     
  
                                                     
                               
  


                                                                
  

   
JsonArray *pcv_gpu_list(void)
{
    JsonArray *arr = json_array_new();
    GDir *dir = g_dir_open(GPU_PCI_SYSFS, 0, NULL);
    if (!dir)
        return arr;

    GPtrArray *bdfs = g_ptr_array_new_with_free_func(g_free);
    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (pcv_validate_pci_addr(name) && _gpu_is_display_controller(name))
            g_ptr_array_add(bdfs, g_strdup(name));
    }
    g_dir_close(dir);
    g_ptr_array_sort(bdfs, _gpu_bdf_compare);

    for (guint i = 0; i < bdfs->len; i++) {
        const gchar *pci = g_ptr_array_index(bdfs, i);
        const gchar *argv[] = {"lspci", "-Dnnks", pci, NULL};
        gchar *detail = NULL;
        pcv_spawn_sync(argv, &detail, NULL, NULL);
        if (detail) {
            gchar *newline = strchr(detail, '\n');
            if (newline)
                *newline = '\0';
            g_strstrip(detail);
        }

        gchar *driver = _gpu_driver_name(pci);
        gchar *vfs_path = _gpu_pci_attr_path(pci, "sriov_totalvfs");
        gchar *vfs_text = _gpu_read_trimmed(vfs_path);
        gchar *mdev_path = _gpu_pci_attr_path(pci, "mdev_supported_types");
        gchar *group_link = _gpu_pci_attr_path(pci, "iommu_group");
        gchar *group_target = g_file_read_link(group_link, NULL);
        gchar *group = group_target ? g_path_get_basename(group_target) : g_strdup("none");
        const gchar *device = detail && strlen(detail) > 13 ? detail + 13 : (detail ? detail : "unknown");

        JsonObject *gpu = json_object_new();
        json_object_set_string_member(gpu, "pci_addr", pci);
        json_object_set_string_member(gpu, "device", device);
        json_object_set_string_member(gpu, "description", detail ? detail : "unknown");
        json_object_set_string_member(gpu, "driver", driver);
        json_object_set_string_member(gpu, "iommu_group", group);
        json_object_set_int_member(gpu, "sriov_vfs",
            vfs_text ? g_ascii_strtoll(vfs_text, NULL, 10) : 0);
        json_object_set_boolean_member(gpu, "mdev_supported",
            g_file_test(mdev_path, G_FILE_TEST_IS_DIR));
        json_object_set_boolean_member(gpu, "assignment_ready",
            g_strcmp0(driver, "vfio-pci") == 0 && group_target != NULL);
        json_array_add_object_element(arr, gpu);

        g_free(group);
        g_free(group_target);
        g_free(group_link);
        g_free(mdev_path);
        g_free(vfs_text);
        g_free(vfs_path);
        g_free(driver);
        g_free(detail);
    }
    g_ptr_array_unref(bdfs);
    return arr;
}

   
                
                                   
  
                                                  
  
                                              
  
                                                                   
                                                           
                       
  
                                                               
   
JsonObject *pcv_gpu_info(const gchar *pci_addr)
{
    JsonObject *obj = json_object_new();
    if (!pci_addr) return obj;                                         
    json_object_set_string_member(obj, "pci_addr", pci_addr);                               
    gchar *cmd = g_strdup_printf("lspci -v -s %s 2>/dev/null", pci_addr);                                   
    gchar *out = NULL;
    if (_run(cmd, &out, NULL) && out)
        json_object_set_string_member(obj, "detail", out);                         
    g_free(out); g_free(cmd);                          
    return obj;
}

   
                      
                    
  
                                       
                                              
  
                                                       
                                           
                                 
  
                                                
   
JsonArray *pcv_gpu_vgpu_types(const gchar *pci_addr)
{
    JsonArray *arr = json_array_new();
    if (!pci_addr) return arr;                           
                                                               
    gchar *cmd = g_strdup_printf("ls /sys/bus/pci/devices/0000:%s/mdev_supported_types/ 2>/dev/null", pci_addr);
    gchar *out = NULL;
    if (_run(cmd, &out, NULL) && out) {
        gchar **types = g_strsplit(g_strstrip(out), "\n", -1);                            
        for (gint i = 0; types[i] && types[i][0]; i++)                   
            json_array_add_string_element(arr, types[i]);                               
        g_strfreev(types);
    }
    g_free(out); g_free(cmd);
    return arr;
}

   
                       
                            
                                                 
                                                   
                       
  
                                      
                                                 
  
                                                               
  
                                                             
                                                                   
                                                              
                                                           
  
                     
   
gboolean pcv_gpu_vgpu_create(const gchar *pci_addr, const gchar *type,
                              gchar **uuid_out, GError **error)
{
    if (!pci_addr || !type) {                                            
        g_set_error(error, g_quark_from_static_string("gpu"), 1, "pci_addr and type required");
        return FALSE;
    }
                                                                      
    gchar *cmd = g_strdup_printf("mdevctl start --parent 0000:%s --type %s 2>&1", pci_addr, type);
    gchar *out = NULL;
    gboolean ok = _run(cmd, &out, error);
                                                                    
    if (ok && out && uuid_out) *uuid_out = g_strdup(g_strstrip(out));
    g_free(out); g_free(cmd);
    return ok;
}

   
                       
                              
                    
  
                                        
                                                      
  
                                                         
  
                                                         
                                                          
                                                         
                                   
  
                     
   
gboolean pcv_gpu_vgpu_delete(const gchar *uuid, GError **error)
{
    if (!uuid) { g_set_error(error, g_quark_from_static_string("gpu"), 1, "uuid required"); return FALSE; }                    
    gchar *cmd = g_strdup_printf("mdevctl stop --uuid %s 2>&1; true", uuid);                                      
    gboolean ok = _run(cmd, NULL, error);                              
    g_free(cmd);
    return ok;
}

   
                     
  
                                              
                                            
  
                                                  
                                                              
                               
  
                                                               
   
JsonArray *pcv_gpu_vgpu_list(void)
{
    JsonArray *arr = json_array_new();
    gchar *out = NULL;
                                                                      
    if (_run_shell("mdevctl list 2>/dev/null", &out, NULL) && out) {
        gchar **lines = g_strsplit(out, "\n", -1);
        for (gint i = 0; lines[i] && lines[i][0]; i++) {                     
            JsonObject *v = json_object_new();
            json_object_set_string_member(v, "entry", lines[i]);                            
            json_array_add_object_element(arr, v);
        }
        g_strfreev(lines);
    }
    g_free(out);
    return arr;
}

   
                  
                            
                                                     
                       
  

                                                         

                                                    

  




  
                     
   
gboolean pcv_gpu_attach(const gchar *vm_name, const gchar *pci_addr, GError **error)
{
    if (!pcv_validate_vm_name(vm_name) || !pcv_validate_pci_addr(pci_addr)) {
        g_set_error(error, g_quark_from_static_string("gpu"), 1,
                    "valid vm_name and canonical pci_addr are required");
        return FALSE;
    }

    if (!_gpu_is_display_controller(pci_addr)) {
        g_set_error(error, g_quark_from_static_string("gpu"), 2,
                    "PCI device %s is not a display controller", pci_addr);
        return FALSE;
    }
    GPtrArray *members = _gpu_iommu_slot_group(pci_addr, error);
    if (!members)
        return FALSE;
    for (guint i = 0; i < members->len; i++) {
        const gchar *member = g_ptr_array_index(members, i);
        gchar *driver = _gpu_driver_name(member);
        gboolean ready = g_strcmp0(driver, "vfio-pci") == 0;
        if (!ready)
            g_set_error(error, g_quark_from_static_string("gpu"), 3,
                        "IOMMU group member %s uses '%s'; pre-bind the whole GPU group to vfio-pci",
                        member, driver);
        g_free(driver);
        if (!ready) {
            g_ptr_array_unref(members);
            return FALSE;
        }
    }

    gboolean ok = FALSE;
    virConnectPtr conn = NULL;
    virDomainPtr dom = NULL;
    char *domain_xml = NULL;
    GPtrArray *device_xmls = g_ptr_array_new_with_free_func(g_free);
    guint attached = 0;

    g_mutex_lock(&gpu_passthrough_mu);
    conn = virt_conn_pool_acquire();
    if (!conn) {
        g_set_error(error, g_quark_from_static_string("gpu"), 5,
                    "libvirt connection unavailable");
        goto out;
    }
    dom = virDomainLookupByName(conn, vm_name);
    if (!dom) {
        _gpu_set_libvirt_error(error, "GPU attach VM lookup failed");
        goto out;
    }
    int active = virDomainIsActive(dom);
    if (active != 0) {
        if (active < 0)
            _gpu_set_libvirt_error(error, "GPU attach state check failed");
        else
            g_set_error(error, g_quark_from_static_string("gpu"), 6,
                        "GPU assignment requires a shut off VM");
        goto out;
    }
    domain_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    if (!domain_xml) {
        _gpu_set_libvirt_error(error, "GPU attach XML inspection failed");
        goto out;
    }

    guint present = 0;
    for (guint i = 0; i < members->len; i++) {
        const gchar *member = g_ptr_array_index(members, i);
        gchar *existing = NULL;
        gboolean managed_no = FALSE;
        if (!_gpu_find_hostdev(domain_xml, member, &existing, &managed_no, error)) {
            g_free(existing);
            goto out;
        }
        if (existing) {
            present++;
            if (!managed_no) {
                g_set_error(error, g_quark_from_static_string("gpu"), 7,
                            "GPU %s has a legacy managed=yes hostdev; detach it before safe assignment",
                            member);
                g_free(existing);
                goto out;
            }
        }
        g_free(existing);
        g_ptr_array_add(device_xmls, _gpu_hostdev_xml(member));
    }
    if (present == members->len) {
        ok = TRUE;
        goto out;
    }
    if (present != 0) {
        g_set_error(error, g_quark_from_static_string("gpu"), 7,
                    "GPU IOMMU group is only partially assigned; repair the VM definition first");
        goto out;
    }

    for (guint i = 0; i < device_xmls->len; i++) {
        const gchar *xml = g_ptr_array_index(device_xmls, i);
        if (virDomainAttachDeviceFlags(dom, xml, VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            _gpu_set_libvirt_error(error, "GPU persistent attach failed");
            for (guint rollback = 0; rollback < attached; rollback++)
                virDomainDetachDeviceFlags(dom,
                    g_ptr_array_index(device_xmls, rollback),
                    VIR_DOMAIN_AFFECT_CONFIG);
            goto out;
        }
        attached++;
    }
    ok = TRUE;

out:
    free(domain_xml);
    if (dom)
        virDomainFree(dom);
    if (conn)
        virt_conn_pool_release(conn);
    g_mutex_unlock(&gpu_passthrough_mu);
    g_ptr_array_unref(device_xmls);
    g_ptr_array_unref(members);
    if (ok)
        PCV_LOG_INFO(GPU_LOG_DOM, "Assigned GPU group %s to shut off VM '%s' (managed=no)",
                     pci_addr, vm_name);
    return ok;
}

   
                  
                            
                    
                       
  
                                                               

  





  
                     
   
gboolean pcv_gpu_detach(const gchar *vm_name, const gchar *pci_addr, GError **error)
{
    if (!pcv_validate_vm_name(vm_name) || !pcv_validate_pci_addr(pci_addr)) {
        g_set_error(error, g_quark_from_static_string("gpu"), 1,
                    "valid vm_name and canonical pci_addr are required");
        return FALSE;
    }

    GPtrArray *members = _gpu_iommu_slot_group(pci_addr, error);
    if (!members)
        return FALSE;
    gboolean ok = FALSE;
    virConnectPtr conn = NULL;
    virDomainPtr dom = NULL;
    char *domain_xml = NULL;
    GPtrArray *existing_xmls = g_ptr_array_new_with_free_func(g_free);
    guint detached = 0;

    g_mutex_lock(&gpu_passthrough_mu);
    conn = virt_conn_pool_acquire();
    if (!conn) {
        g_set_error(error, g_quark_from_static_string("gpu"), 5,
                    "libvirt connection unavailable");
        goto out;
    }
    dom = virDomainLookupByName(conn, vm_name);
    if (!dom) {
        _gpu_set_libvirt_error(error, "GPU detach VM lookup failed");
        goto out;
    }
    int active = virDomainIsActive(dom);
    if (active != 0) {
        if (active < 0)
            _gpu_set_libvirt_error(error, "GPU detach state check failed");
        else
            g_set_error(error, g_quark_from_static_string("gpu"), 6,
                        "GPU removal requires a shut off VM");
        goto out;
    }
    domain_xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE);
    if (!domain_xml) {
        _gpu_set_libvirt_error(error, "GPU detach XML inspection failed");
        goto out;
    }

    for (guint i = 0; i < members->len; i++) {
        gchar *existing = NULL;
        gboolean managed_no = FALSE;
        if (!_gpu_find_hostdev(domain_xml, g_ptr_array_index(members, i),
                               &existing, &managed_no, error)) {
            g_free(existing);
            goto out;
        }
        if (existing)
            g_ptr_array_add(existing_xmls, existing);
    }
    if (existing_xmls->len == 0) {
        ok = TRUE;
        goto out;
    }

    for (guint i = 0; i < existing_xmls->len; i++) {
        const gchar *xml = g_ptr_array_index(existing_xmls, i);
        if (virDomainDetachDeviceFlags(dom, xml, VIR_DOMAIN_AFFECT_CONFIG) < 0) {
            _gpu_set_libvirt_error(error, "GPU persistent detach failed");
            for (guint rollback = 0; rollback < detached; rollback++)
                virDomainAttachDeviceFlags(dom,
                    g_ptr_array_index(existing_xmls, rollback),
                    VIR_DOMAIN_AFFECT_CONFIG);
            goto out;
        }
        detached++;
    }
    ok = TRUE;

out:
    free(domain_xml);
    if (dom)
        virDomainFree(dom);
    if (conn)
        virt_conn_pool_release(conn);
    g_mutex_unlock(&gpu_passthrough_mu);
    g_ptr_array_unref(existing_xmls);
    g_ptr_array_unref(members);
    if (ok)
        PCV_LOG_INFO(GPU_LOG_DOM, "Removed GPU group %s from shut off VM '%s'",
                     pci_addr, vm_name);
    return ok;
}
