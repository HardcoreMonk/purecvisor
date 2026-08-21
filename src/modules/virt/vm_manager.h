   
                     
                                                    
  
                           
                                                   
                                                    
                                        
  
                                                        
                                                          
                                                  
                                                       
                                                    
                                                      
                            
  
                                                         
                                                    
                                                               
                                               
                                 
                                                                
  
                   
                                                                   
  
                                                                        
                                                    
  
                       
                                                  
                                                     
                                                
  
                                
                                         
                                         
                                         
                                                 
  
             
                                                            
                                                                        
                                                                      
                                                                             
                                                          
                                                             
   
                                   

#ifndef PURECVISOR_VM_MANAGER_H
#define PURECVISOR_VM_MANAGER_H

#include <glib-object.h>
#include <json-glib/json-glib.h>
#include <libvirt-gobject/libvirt-gobject.h>
#include <libvirt/libvirt.h>
#include "modules/network/pcv_qos.h"                                              

G_BEGIN_DECLS

                                                
#define PURECVISOR_TYPE_VM_MANAGER (purecvisor_vm_manager_get_type())

                                                         
G_DECLARE_FINAL_TYPE(PureCVisorVmManager, purecvisor_vm_manager, PURECVISOR, VM_MANAGER, GObject)

   
                             
                                                        
  
                      
                                          
  
                                                    
                                                        
                                                  
   
                            
PureCVisorVmManager *purecvisor_vm_manager_new(GVirConnection *conn);

                                                                            
                                        
  
                       
                               
                                          
                                                                   
                                   
  
                                              
                                                                               

                                                  

   
                                         
  
                                                    
                                                        
                                                    
                    
  
                                                           
                                                               
                                                             
                                                    
  
                               
                                              
                             
                               
                                            
                                        
                                                                   
                                                  
                                                                   
                                                                               
                                                  
                                                                                       
                                                                        
                                                                              
                                                                              
                                                                  
                                          
                                                                            
                                                                        
                                                                      
                                                               
                                                                              
                                                          
                                                               
                                        
                                   
  
                    
                                                              
                                                     
                                       
                                                                      
                                  
   
                                
void purecvisor_vm_manager_create_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           gint vcpu,
                                           gint ram_mb,
                                           gint disk_size_gb,           
                                           const gchar *iso_path,
                                           const gchar *network_bridge,           
                                           gint         vlan_id,                     
                                           gint         boot_mode,                                          
                                           gboolean     tpm,                      
                                           gint         cpu_mode,                                                                                   
                                           gboolean     hugepages,                       
                                           const gchar *storage_type,                                
                                           const gchar *storage_pool,                                
                                           const gchar *image_dir,                          
                                           const gchar *nic_type,                                       
                                           const gchar *pci_addr,                          
                                           const gchar *base_image,                              
                                           const gchar *owner,                                  
                                           const gchar *network_mode,                                                             
                                           const gchar *tenant,                                               
                                           gboolean     qos_required,                              
                                           const PcvQosSla *qos_sla,                                                    
                                           GCancellable *cancellable,                          
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
   
                                          
                   
                                   
                                  
  
                                                     
                                      
                                                
   
gboolean purecvisor_vm_manager_create_vm_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

   
                                        
                                                               
  
                                           
                                                                        
                                     
                    
  
                                                      
                                              
                                                             
                                                      
   
gchar *purecvisor_vm_resolve_network_bridge(const gchar *requested);

   
                                        
                    
                       
                   
                     
  
                                   
                                             
  
                                            
   
        
void purecvisor_vm_manager_start_vm_async(PureCVisorVmManager *self,
                                          const gchar *name,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);
gboolean purecvisor_vm_manager_start_vm_finish(PureCVisorVmManager *manager,
                                               GAsyncResult *res,
                                               GError **error);

   
                                       
                    
                       
                   
                     
  
                                                              
                                             
  
                                                      
                                
   
       
void purecvisor_vm_manager_stop_vm_async(PureCVisorVmManager *self,
                                         const gchar *name,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);
gboolean purecvisor_vm_manager_stop_vm_finish(PureCVisorVmManager *manager,
                                              GAsyncResult *res,
                                              GError **error);

   
                                         
                    
                       
                   
                     
  
                                                   
                                                   
                                                
                                        
  
         
                                      
                                             
                                                 
                                       
                        
   
         
void purecvisor_vm_manager_delete_vm_async(PureCVisorVmManager *self,
                                           const gchar *name,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);
gboolean purecvisor_vm_manager_delete_vm_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

   
                            
             
  
                     
                                                                
  
                                                      
                                                  
   
const gchar *pcv_vm_delete_status_get(const gchar *vm);

                               
                                                       
void pcv_vm_manager_cleanup(void);

   
                                        
                    
                   
                     
  
                                                
                                                                    
  
                                                    
                                   
  
                                                      
   
       
void purecvisor_vm_manager_list_vms_async(PureCVisorVmManager *self,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);
JsonNode *purecvisor_vm_manager_list_vms_finish(PureCVisorVmManager *manager,
                                                GAsyncResult *res,
                                                GError **error);

                                                                               
                                                                        
                                                                               
                                                         
                                                                         
                                                          
                                                                               
                                                                 
                                                                               

   
                                          
                       
                         
                              
                                 
                      
                       
  
                                                                                
                                             
  
                                                   
                                          
   
                       
void purecvisor_vm_manager_set_memory_async(PureCVisorVmManager *self,
                                            const gchar *name,
                                            guint memory_mb,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);

gboolean purecvisor_vm_manager_set_memory_finish(PureCVisorVmManager *self,
                                                 GAsyncResult *res,
                                                 GError **error);

   
                                        
                      
                        
                         
                                 
                     
                      
  
                                                                               
                              
  
                                                 
                                                   
   
                 
void purecvisor_vm_manager_set_vcpu_async(PureCVisorVmManager *self,
                                          const gchar *name,
                                          guint vcpu_count,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

gboolean purecvisor_vm_manager_set_vcpu_finish(PureCVisorVmManager *self,
                                               GAsyncResult *res,
                                               GError **error);


                                                                            
                          
  
                                         
  
                
                                                 
                                                     
                                                        
                                                   
  
                
                                                 
                                                     
                                                        
                                                  
  
                        
                                                 
                                                           
                                                        
                                                
                                               
                                                                           
                                               
  
        
                                                 
                                                       
                                                                               

   
                             
                         
                              
                                                 
                                                          
                                                                            
                                                              
  
                                                         
                                                 
  
                                                          
                                                           
  
                                     
                                                        
                                              
   
void purecvisor_vm_resize_disk(const gchar *name, gint new_size_gb, const gchar *target,
                                gboolean holds_lock);

                                                                            
                                           
  
                                  
                
                                 
                                          
                                  
              
                                                                               

   
                             
                       
  
                                                   
                                                            
               
  
                               
                               
                                                             
                                       
                                          
                                 
   
void purecvisor_vm_clone_async(const gchar *source_name, const gchar *clone_name,
                                gboolean full_copy, GCancellable *cancellable,
                                GAsyncReadyCallback callback, gpointer user_data);

                                                               
#define PCV_VM_SIGNAL_STARTED          "vm-started"
#define PCV_VM_SIGNAL_STOPPED          "vm-stopped"
#define PCV_VM_SIGNAL_METRICS_UPDATED  "vm-metrics-updated"

   
                                              
                          
                                            
  
                                              
                                                       
  
                                                       
                                                
  
                                                      
                                          
   
void purecvisor_vm_manager_emit_metrics_updated(PureCVisorVmManager *self,
                                                GHashTable          *cache);

                                                                            
                                                             
                               
  
                                                                           
                                                                 
                                                                               

                                                           
                                                                   
#define PCV_OVERLAY_METADATA_URI "urn:purecvisor:overlay:1"

   
                           
                                                    
                                                                  
                                                            
  
                                                      
                                 
  
                                                        
                                                   
   
gboolean _overlay_metadata_parse(const gchar *metadata_xml,
                                 gchar **mode_out, gchar **tenant_out);

   
                             
                                                            
                                                                           
                                           
                                                            
  
                                                     
                                             
  
                                                                               
                                                        
                                            
                                                             
   
gboolean _overlay_live_iface_parse(const gchar *domain_xml,
                                   gchar **tap_out, gchar **mac_out);

   
                                
                                               
  
                                                        
                                                  
  
                                                                       
                                                                 
                                                             
   
gchar *_overlay_gw_cidr_from_subnet(const gchar *subnet_cidr);

                                                                            
                                                         
  
                                                                      
                                                                        
                                                 
                                                          
                                                                               

   
                                                                        
  
                                                         
                                                            
                                                   
                                                        
                        
  
                                                           
                                                   
                                                            
                                             
                                               
  
                                                  
                                                                
                                                                     
                                                         
                                                                     
                                                      
                                                                   
                                                         
                                                      
                                                              
                                                                     
                                                              
                                                  
  
                                                        
                                             
                            
   
typedef enum {
    PCV_QOS_META_OK,
    PCV_QOS_META_ABSENT,
    PCV_QOS_META_INVALID
} PcvQosMetaResult;

   
                             
  
                                                    
                                         
  
                                                          
                                                         
                              
  
                                                             
                                                                
                                                       
                                              
  
                                                        
                                                       
                                                 
          
  
                                           
   
gboolean pcv_vm_qos_metadata_write(virDomainPtr dom, const PcvQosSla *sla, GError **error);

   
                            
  
                                                   
                                   
  
                                                   
                  
                                                                 
                                                          
                                         
  
                                                                 
   
PcvQosMetaResult pcv_vm_qos_metadata_read(virDomainPtr dom, PcvQosSla *out);

   
                             
  
                                                   
                                                      
                                   
  
                                                         
                                                           
                          
                                                                      
                                                                 
                                             
                                                        
                                                  
                                        
                                            
                                                                  
                                                                       
                                        
   
void pcv_qos_backfill_existing(void);

                                                                            
                                                                    
  
                                                               
                                                  
                                              
                       
                                                                               

   
                             
  
                                                   
                                                    
                                       
  
                                                   
                                                     
                                                                 
                                            
                                                             
                                                      
                                                                        
                                           
                                                           
                                                          
                                                              
  
                                                            
                                                           
                                
   
gboolean pcv_vm_qos_derive_context(virDomainPtr dom, const gchar *vm_name,
                                   gchar **tenant_out, gchar **iface_out,
                                   PcvQosSla *sla_out);

   
                                
  
                                                  
                                                          
  
                                                                    
                                                                       
                                                         
  
                                                            
                                                    
   
GPtrArray *pcv_vm_qos_expected_provider(void);

G_END_DECLS

#endif                              
