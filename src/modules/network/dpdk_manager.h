   
                       
                                             
  
                           
                                                   
                                                    
                                        
  
                                                   
                                                    
                                                    
                                                       
                             
  
                                                                       
       
                                              
                                          
  
                        
                                                 
                          
  
                   
                     
                                               
                                         
  
               
                                                  
                                                      
  
               
                                                  
                                
                                          
  
                             
                                             
                                           
                                         
  
                 
                                           
                                                        

  
          
                                                  
                                    



  
          
                                                           
                                                          
                                                      
                                                         
                                               
  
          
                             
                                                               
                                      
                                                                         
                                       
  
                         
                    
                              
                                                
                                      
                             
  
       
                                         
                                        
                                                                       
   
#ifndef PURECVISOR_DPDK_MANAGER_H
#define PURECVISOR_DPDK_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define PCV_DPDK_MTU_MIN 68u
#define PCV_DPDK_MTU_MAX 9216u
#define PCV_DPDK_MTU_DEFAULT 1500u

  
                                        
  
                                                
  
                                              
                                                          
   

                         
void     pcv_dpdk_init(void);                                         
void     pcv_dpdk_shutdown(void);                   
gboolean pcv_dpdk_is_available(void);                   

                      
JsonObject *pcv_dpdk_status(void);                                                               
JsonObject *pcv_dpdk_hugepage_info(void);                                     

                        
                                                     
                                                    
gboolean    pcv_dpdk_bind(const gchar *pci_addr, const gchar *driver, GError **error);
                                                  
                                                                  
                                                                          
                                                          
                                                                
                                    
                                                      
                                                         


                                                                        
gboolean    pcv_dpdk_unbind(const gchar *pci_addr, GError **error);
JsonArray  *pcv_dpdk_list(void);                                   

                                                  
                                                            
                                                                 
                                                                         
                                                            
                                                                        
                                                           
                 
gboolean pcv_dpdk_nic_is_protected(const gchar *pci_addr, gchar **reason);
                                                                                
gboolean pcv_dpdk_route_is_default_dev(const gchar *netdev, const gchar *proc_base);

                                        
  
                         
                                 
                                                      
                             
gboolean pcv_dpdk_bridge_create(const gchar *name, const gchar *dpdk_port,
                                guint mtu, GError **error);
                                                                       
                                                                         
                                             



gboolean pcv_dpdk_bridge_delete(const gchar *name, GError **error);

                                                   
   
                                                        
                  



   
gchar *pcv_dpdk_vhost_socket_path(const gchar *vm_name);




typedef enum {
    PCV_DPDK_VHOST_ENDPOINT_CANONICAL,
    PCV_DPDK_VHOST_ENDPOINT_ACTIVE_LEGACY
} PcvDpdkVhostEndpoint;


gboolean pcv_dpdk_vhost_runtime_preflight(GError **error);



gboolean pcv_dpdk_vm_port_ensure(const gchar *bridge_name,
                                 const gchar *vm_name,
                                 GError **error);
gboolean pcv_dpdk_vm_port_ensure_endpoint(const gchar *bridge_name,
                                          const gchar *vm_name,
                                          PcvDpdkVhostEndpoint endpoint,
                                          GError **error);
gboolean pcv_dpdk_vm_port_delete(const gchar *vm_name, GError **error);

G_END_DECLS

#endif                                
