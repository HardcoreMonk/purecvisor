   
                      
                                  
  
                           
                                                   
                                                    
                                        
  
                                                       
                                                            
                                                   
                                                   
                                                                 
  
                                                                       
       
                                                
                            
                                                      
  
                       
                                                               
                           
  
                 
                             
                                                     
  
               
                                                            
                                                               
                                           
                                                                    
  
                
                                        
                                                 
  
              
                                                       
                                          
  
             
                                                  
                                   
  
          
                                                 
                                                            
                                                          
                                                                   
                                        
                                                       
                                    
                                        
                                                         
                         
  
           
                                                                          
                                                                     
                                                
  
                         
                                                  
                                         
                 
  
           
                                                
                                                     
                                        
                                                                       
   
#ifndef PURECVISOR_OVN_MANAGER_H
#define PURECVISOR_OVN_MANAGER_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

  
                                                  
  
                                  
                                         
                                
  
                             
   

                         
void pcv_ovn_init(void);                                                        
void pcv_ovn_shutdown(void);                                              
gboolean pcv_ovn_is_available(void);                    

                            
                                                       


gboolean    pcv_ovn_switch_create(const gchar *name, GError **error);


                                                          
                                                         
                                                                       
                                                       
                                             
gboolean    pcv_ovn_switch_delete(const gchar *name, GError **error);
                                                      
                                                                        
                                                                              
JsonArray  *pcv_ovn_switch_list(void);
                                                                   
                                              
                                                                 
                                                                            
                                                             
gboolean    pcv_ovn_port_add(const gchar *sw, const gchar *port, const gchar *mac, const gchar *ip, GError **error);
                                                                 
                                                       
                                                         
gboolean    pcv_ovn_port_remove(const gchar *sw, const gchar *port, GError **error);

                                
  
                              
                                                           
                                       
  
            
                                                            
                                                         
gboolean pcv_ovn_acl_add(const gchar *sw, const gchar *direction, gint priority,
                           const gchar *match, const gchar *action, GError **error);
gboolean pcv_ovn_acl_delete(const gchar *sw, const gchar *direction, gint priority,
                              const gchar *match, GError **error);
                                                              
                                                             
                                                             
JsonArray *pcv_ovn_acl_list(const gchar *sw);
                                                  
                                              
                                                              
                                                         
                                              
gboolean pcv_ovn_dhcp_enable_for_switch(const gchar *sw, const gchar *subnet,
                                          const gchar *gw, GError **error);

                            
  
           
                                            
                                      
                         
  
        
                                        
                                         
                                            
gboolean    pcv_ovn_router_create(const gchar *name, GError **error);
                                                
                                                        
                                         
gboolean    pcv_ovn_router_delete(const gchar *name, GError **error);
gboolean    pcv_ovn_router_add_port(const gchar *router, const gchar *sw,
                                     const gchar *mac, const gchar *cidr, GError **error);
                                                              
                                           
                                                         
gboolean    pcv_ovn_router_remove_port(const gchar *router, const gchar *port, GError **error);
                                              
                                                                              
JsonArray  *pcv_ovn_router_list(void);

                   
gboolean pcv_ovn_nat_add(const gchar *router, const gchar *type,
                          const gchar *external_ip, const gchar *logical_ip, GError **error);
gboolean pcv_ovn_nat_delete(const gchar *router, const gchar *type,
                             const gchar *external_ip, const gchar *logical_ip, GError **error);
                                                    
JsonArray *pcv_ovn_nat_list_parse(const gchar *output);

                                                     
                                                             
JsonArray *pcv_ovn_nat_list(const gchar *router);

                         
JsonArray *pcv_ovn_dhcp_list(void);

                        
                                                                      
                                                                                   
                                                                         
                                                                
                                                             
                                                   
                                   
gboolean pcv_ovn_single_prepare_local(GError **error);

                         
  
             
                                                        
                                           
                                                    
                                                 
gboolean pcv_ovn_tenant_create(const gchar *tenant, const gchar *subnet, GError **error);
                                                                 
                                                          
                                                                        
                                                        
                                                           
gboolean pcv_ovn_tenant_delete(const gchar *tenant, GError **error);

                              
  
                
                                                                          
                                                            
                                               
gboolean pcv_ovn_vm_port_setup(const gchar *sw, const gchar *vm_name,
                                const gchar *mac, const gchar *ip,
                                gchar **iface_id_out, GError **error);
                                                             
                                                           
                                              
                                                          
                                            
gboolean pcv_ovn_vm_port_cleanup(const gchar *vm_name, GError **error);

                                         
JsonObject *pcv_ovn_switch_detail(const gchar *name);                       
JsonObject *pcv_ovn_router_detail(const gchar *name);                                  

                      
JsonObject *pcv_ovn_status(void);                                                       

                                                                   
                                                                 
gboolean pcv_ovn_resource_is_local_vpc_owned(const gchar *table,
                                             const gchar *name,
                                             GError **error);

G_END_DECLS

#endif                               
