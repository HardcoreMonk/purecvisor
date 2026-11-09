   
                              
                                                                                            
  
                           
                                                                                            
                                                                                         
                                                                                          
                              
                                                                                     
  
                                                         
   
#ifndef PCV_VM_MANAGER_INTERNAL_H
#define PCV_VM_MANAGER_INTERNAL_H

#include <glib-object.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include "modules/virt/vm_manager.h"

   
                                
  
                                     
                                                       
                                                            
  
                          
                                                    
                                                  
                                                      
                                                                        
                                                               
  
                                     
                                            
                                               
                                                    
                                                     
  
                                                                                                          
   
struct _PureCVisorVmManager {
    GObject parent_instance;
};

   
                     
                                       
   
typedef struct {
    PureCVisorVmManager *manager;
    gchar *name;
} LifecycleTaskData;


                                                     
gchar *
_overlay_metadata_xml(const gchar *network_mode, const gchar *tenant);
gchar *
_overlay_ethernet_iface_xml(void);
gint64
_hugepage_preflight_need_pages(gint ram_mb);
gchar *
_build_memory_backing_xml(gboolean hugepages, gboolean want_shared);
gchar *
_build_bridge_iface_xml(const gchar *safe_bridge,
                        const gchar *virtualport_xml,
                        const gchar *vlan_xml,
                        gint mtu);
gchar *
_build_dpdk_iface_xml(const gchar *vm_name);
gchar *
_vm_xml_ensure_memballoon_stats(const gchar *xml);
gchar *
_vm_xml_insert_virtio_rng(const gchar *xml);
gchar *
_vm_xml_ensure_q35_hotplug_ports(const gchar *xml);
gchar *
_vm_xml_apply_cpu_invtsc(const gchar *xml);
gboolean
purecvisor_vm_sriov_pci_parse(const gchar *pci, guint out[4]);
gboolean
purecvisor_vm_sriov_nic_check(const gchar *nic_type, const gchar *network_bridge,
                              const gchar *network_mode, GError **error);
gchar *
pcv_vm_xml_inject_metadata_child(const gchar *xml, const gchar *child_xml);
gchar *
pcv_vm_xml_with_owner_metadata(const gchar *xml, const gchar *owner);

#endif                                
