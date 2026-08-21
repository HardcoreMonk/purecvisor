   
                      
                                                              
  
                                                                 
                                              
                                                                         
                                                     
                                                        
  
                           
                                                                      
                                                                        
                                                             
                                        
  
                       
                                                  
                                                        
                      
   
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define PCV_VPC_DEFAULT_DB_PATH "/var/lib/purecvisor/vpc.db"

   
                           
                                                                           
                                                                    
  
                                                          
   
gboolean pcv_vpc_init(const gchar *db_path, GError **error);
void pcv_vpc_shutdown(void);
gboolean pcv_vpc_reconcile(GError **error);
gboolean pcv_vpc_bridge_is_managed(const gchar *bridge_name);
gboolean pcv_vpc_mac_is_managed(const gchar *mac_address);
gboolean pcv_vpc_vm_is_attached(const gchar *vm_identifier);
gboolean pcv_vpc_vm_has_publish(const gchar *vm_identifier);
                                                                
gboolean pcv_vpc_security_group_sync_vm(const gchar *vm_name, GError **error);

   
                           
                                                                    
                                                                  
  
                                                        
                                    
   
JsonArray *pcv_vpc_list(const gchar *tenant, GError **error);
JsonObject *pcv_vpc_get(const gchar *id, const gchar *tenant, GError **error);
JsonArray *pcv_vpc_subnet_list(const gchar *vpc_id, const gchar *tenant, GError **error);
JsonArray *pcv_vpc_attachment_list(const gchar *vpc_id, const gchar *tenant, GError **error);
JsonArray *pcv_vpc_service_list(const gchar *vpc_id, const gchar *tenant, GError **error);
JsonObject *pcv_vpc_status(const gchar *tenant, GError **error);
JsonArray *pcv_vpc_backend_list(GError **error);

   
                           
                                                                       
                                                                        
                                                   
  
                                                     
                                            
   
gboolean pcv_vpc_create(const gchar *name,
                        const gchar *tenant,
                        const gchar *egress_mode,
                        const gchar *backend,
                        const gchar *initial_subnet_name,
                        const gchar *initial_subnet_cidr,
                        gint initial_subnet_mtu,
                        JsonObject **result_out,
                        GError **error);
gboolean pcv_vpc_delete(const gchar *id, const gchar *tenant, GError **error);
gboolean pcv_vpc_egress_set(const gchar *id,
                            const gchar *tenant,
                            const gchar *mode,
                            gint64 expected_revision,
                            JsonObject **result_out,
                            GError **error);
gboolean pcv_vpc_subnet_create(const gchar *vpc_id,
                               const gchar *tenant,
                               const gchar *name,
                               const gchar *cidr,
                               gint mtu,
                               gint64 expected_revision,
                               JsonObject **result_out,
                               GError **error);
gboolean pcv_vpc_subnet_delete(const gchar *id,
                               const gchar *tenant,
                               GError **error);
gboolean pcv_vpc_attachment_create(const gchar *subnet_id,
                                   const gchar *tenant,
                                   const gchar *vm,
                                   const gchar *actor,
                                   gboolean actor_is_admin,
                                   const gchar *requested_ip,
                                   JsonObject **result_out,
                                   GError **error);
gboolean pcv_vpc_attachment_delete(const gchar *id,
                                   const gchar *tenant,
                                   const gchar *actor,
                                   gboolean actor_is_admin,
                                   GError **error);
gboolean pcv_vpc_service_publish(const gchar *attachment_id,
                                 const gchar *tenant,
                                 const gchar *protocol,
                                 const gchar *listen_address,
                                 gint listen_port,
                                 gint target_port,
                                 GPtrArray *allowed_sources,
                                 gboolean actor_is_admin,
                                 JsonObject **result_out,
                                 GError **error);
gboolean pcv_vpc_service_unpublish(const gchar *id,
                                   const gchar *tenant,
                                   GError **error);

G_END_DECLS
