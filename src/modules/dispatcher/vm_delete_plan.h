   
                         
                                                             
  
                           
                                                                                  
                                                                          
                                                       
  
                                                                             
                           
                                                 
   
#ifndef PURECVISOR_DISPATCHER_VM_DELETE_PLAN_H
#define PURECVISOR_DISPATCHER_VM_DELETE_PLAN_H

#include <glib.h>

G_BEGIN_DECLS

                              
typedef enum {
    PCV_VM_DELETE_STORAGE_NONE = 0,                 
    PCV_VM_DELETE_STORAGE_ZVOL,                               
    PCV_VM_DELETE_STORAGE_FILE,                                           
} PcvVmDeleteStorageKind;

                                                            
typedef struct {
    PcvVmDeleteStorageKind kind;
    gchar     *path;                                     
    gchar     *dataset;                              
    GPtrArray *kept;                                                                             
} PcvVmDeleteStoragePlan;

                                
typedef struct {
    gchar *path;                             
    gchar *format;                                                                                
} PcvVmDeleteDiskRef;

   
                               
                                   
                                             
                                                                        
                         
  
                                                                             
                                                                                
                                                                                                        
                                                                         
                                                                         
   
gboolean pcv_vm_delete_plan_from_xml(const gchar *domain_xml, const gchar *vm_name,
                                     PcvVmDeleteStoragePlan *plan, GError **error);

   
                                        
                                                                         
                                                                    
                         
                                                            
                          
   
gboolean pcv_vm_delete_plan_add_kept_from_xml(const gchar *domain_xml, PcvVmDeleteStoragePlan *plan,
                                              GError **error);

   
                                 
                                                                       
                                                     
   
void pcv_vm_delete_plan_for_absent(const gchar *zvol_pool, const gchar *vm_name, PcvVmDeleteStoragePlan *plan);

                                                                         
void pcv_vm_delete_plan_clear(PcvVmDeleteStoragePlan *plan);

   
                               
                            
                         
  
                                                                          
                                              
                                                                                             
   
GPtrArray *pcv_vm_delete_xml_disk_refs(const gchar *domain_xml, GError **error);

   
                                       
                                                                          
  
                                               
                                             
                                                                
   
GPtrArray *pcv_vm_delete_parse_snapshot_clones(const gchar *zfs_output);

   
                                     
                                                                            
                                           
  
                                                          
                                        
                                                               
   
GPtrArray *pcv_vm_delete_parse_backing_chain(const gchar *qemu_img_json, GError **error);

G_END_DECLS

#endif                                             
