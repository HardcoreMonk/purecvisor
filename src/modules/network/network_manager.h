   
                          
                                                        
  
                           
                                                   
                                                    
                                        
  
                                                     
                                                    
                                                     
                                                
  
                                                                       
       
                                       
                                                         
                                                        
  
                
                                  
                                                
                                                         
  
                        
                                                  
                                                 
                                              
  
            
                                                  
                               
                                          
  
                     
                      
                                                             
                                                       
                                                             
                                                              
                                                
  
            
                                    
                                                         
                              
                                                      
                                      
  
                 
                                                                
                                                  
                                                     
                                                      
  
                       
                               
                                                  
                                                     
                                                                       
   
#ifndef PURECVISOR_NETWORK_MANAGER_H
#define PURECVISOR_NETWORK_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"

G_BEGIN_DECLS

                                                                   
                                                    
  
                    
                                                  
                                                                    
                                               
                                
                                                                      

                                                        
void handle_network_create_request  (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                       
void handle_network_delete_request  (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                       
                                                                       
void handle_network_list_request    (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                              
void handle_network_info_request    (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                 
                                               
void handle_network_mode_set_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                                   
                    
  
                                  
                                                  
                                                                      

   
                                                                  
                                                               
                                         
                                                                                
                               
                                             
                    
   
gboolean network_bridge_create(const gchar *bridge_name, const gchar *cidr, gint mtu, GError **error);

   
                                                                      
                                            
                                                       
                                                              
   
gint pcv_bridge_mtu_read(const gchar *bridge, const gchar *sysfs_net_root);

   
                                                                    
                                               
   
void pcv_network_meta_save(const gchar *bridge_name, const gchar *mode, const gchar *cidr);

   
                                                                 
                                                      
                                                                    
   
gboolean pcv_network_iface_preflight_dedicated(const gchar *physical_if,
                                               const gchar *sysfs_net_root,
                                               const gchar *proc_root,
                                               gboolean *was_up,
                                               GError **error);

                                                            
gboolean pcv_network_iface_address_facts_dedicated(const gchar *physical_if,
                                                   gboolean inventory_known,
                                                   gboolean has_ipv4,
                                                   gboolean has_non_link_local_ipv6,
                                                   GError **error);

typedef struct {
    gchar mac[18];
    gint mtu;
    gboolean was_up;
    gboolean promisc_was_on;
} PcvSharedIfaceFacts;

                                                                 
gboolean pcv_network_iface_preflight_shared(const gchar *physical_if,
                                            const gchar *sysfs_net_root,
                                            const gchar *proc_root,
                                            PcvSharedIfaceFacts *facts,
                                            GError **error);

                                                                        
gboolean pcv_network_iface_address_facts_shared(const gchar *physical_if,
                                                gboolean inventory_known,
                                                gboolean has_ipv4,
                                                gboolean has_non_link_local_ipv6,
                                                GError **error);

                                                                                      
gboolean pcv_network_physical_state_save_at(const gchar *state_dir,
                                            const gchar *bridge_name,
                                            const gchar *physical_if,
                                            gint mtu,
                                            gboolean physical_was_up,
                                            GError **error);

                                                                              
gboolean pcv_network_physical_bridge_create_at(const gchar *bridge_name,
                                               const gchar *physical_if,
                                               gint mtu,
                                               const gchar *sysfs_net_root,
                                               const gchar *proc_root,
                                               const gchar *state_dir,
                                               GError **error);

                                                             
gboolean pcv_network_shared_bridge_create_at(const gchar *bridge_name,
                                             const gchar *physical_if,
                                             gint mtu,
                                             const gchar *sysfs_net_root,
                                             const gchar *proc_root,
                                             const gchar *state_dir,
                                             GError **error);

                                                              
gboolean pcv_network_bridge_uplink_mode(const gchar *bridge_name,
                                        gchar **uplink_mode_out,
                                        GError **error);

                                                                     
gboolean pcv_network_physical_bridge_delete_at(const gchar *bridge_name,
                                               const gchar *sysfs_net_root,
                                               const gchar *state_dir,
                                               GError **error);

                                                                
gboolean pcv_network_bridge_has_host_uplink_at(const gchar *bridge_name,
                                              const gchar *sysfs_net_root,
                                              gchar **uplink_out,
                                              GError **error);

                                                                             
gboolean pcv_network_bridge_delete_at(const gchar *bridge_name,
                                      const gchar *sysfs_net_root,
                                      GError **error);

                                                                                      
gboolean pcv_network_live_l3_mutation_allowed_at(const gchar *bridge_name,
                                                 const gchar *state_dir,
                                                 const gchar *sysfs_net_root,
                                                 gchar **reason_out);

                                                                        
gboolean pcv_network_reconcile_physical_bridges(GError **error);

                                                                   
gboolean pcv_network_reconcile_physical_bridges_at(const gchar *state_dir,
                                                   const gchar *sysfs_net_root,
                                                   const gchar *proc_root,
                                                   GError **error);

   
                                                
                                                    
                                
                           
                                             
  
                                                                
                                                          
                                 
                    
   
gboolean network_bridge_delete(const gchar *bridge_name, GError **error);

                                                                   
                             
                                                                      

                                          
                                                  
                                                                     
void handle_network_bind_phys_request  (JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                
                                                       
                                           
                                           
void handle_network_dhcp_toggle_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                                   
                                    
  
          
                                              
                                                      
                                                   
  
             
                                          
                                                        
                                        
  
           
                                            
                                                 
                                                                      

                                                                     
void handle_network_ovs_create_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                                         
void handle_network_ovs_delete_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                   
                                                           
void handle_network_ovs_vxlan_add_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                         
                                       
void handle_network_ovs_vxlan_del_request(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                                                   
                                               
  
         
                                                                   
                                 
                                                    
  
            
                                         
                                  
                                                                      

                                                           
                                                                      
                                                        
void handle_network_qos_set(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                              
                                      
void handle_network_qos_get(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                         
                                      
void handle_network_qos_remove(JsonObject *params, const gchar *rpc_id, UdsServer *server, GSocketConnection *connection);

                                         
void pcv_qos_restore(void);

                                                            
                                                                     
                                                                             
                    
                                                                     
                                                                      
void pcv_qos_rehydrate_reconcile(void);
                                                    
                                                      
                                                           
                                                         
                                                       
                                                  
void pcv_qos_reconcile_timer_init(void);
                                                    
                                               
                                                         
void pcv_qos_reconcile_timer_shutdown(void);

                                   
gboolean pcv_bridge_vlan_add(const gchar *bridge, const gchar *iface, gint vlan_id);
                                                                        
                                                                            
                                                                     
                                                          
                                                             
                                                    
gboolean pcv_bridge_vlan_remove(const gchar *bridge, const gchar *iface, gint vlan_id);

G_END_DECLS

#endif                                   
