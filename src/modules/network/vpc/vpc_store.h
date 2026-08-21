   
                    
                                             
  
                                                                
                                                                
                                                           
  
                                                              
                                                                    
                                                                      
                                           
  
                           
                                                                            
                                                                    
                                                                      
                                                                    
  
                       
                                                       
                                                     
   
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

#include "vpc_policy_nft.h"

G_BEGIN_DECLS

typedef struct _PcvVpcStore PcvVpcStore;

   
                           
                                                                        
                                                               
                    
  
                                                    
   
PcvVpcStore *pcv_vpc_store_open(const gchar *path, GError **error);
void pcv_vpc_store_free(PcvVpcStore *store);

   
                           
                                                                               
                                                                    
                  
  
                                                       
        
   
gboolean pcv_vpc_store_create_vpc(PcvVpcStore *store,
                                   const gchar *name,
                                   const gchar *tenant,
                                   const gchar *egress_mode,
                                   const gchar *backend,
                                   gchar **id_out,
                                   gint64 *revision_out,
                                   GError **error);
gboolean pcv_vpc_store_delete_vpc(PcvVpcStore *store,
                                   const gchar *id,
                                   const gchar *tenant,
                                   GError **error);
gboolean pcv_vpc_store_set_egress(PcvVpcStore *store,
                                  const gchar *id,
                                  const gchar *tenant,
                                  const gchar *egress_mode,
                                  gint64 expected_revision,
                                  gint64 *revision_out,
                                  GError **error);
JsonArray *pcv_vpc_store_list_vpcs(PcvVpcStore *store,
                                   const gchar *tenant,
                                   GError **error);
JsonObject *pcv_vpc_store_get_vpc(PcvVpcStore *store,
                                  const gchar *id,
                                  const gchar *tenant,
                                  GError **error);
JsonArray *pcv_vpc_store_list_subnets(PcvVpcStore *store,
                                      const gchar *vpc_id,
                                      const gchar *tenant,
                                      GError **error);
JsonObject *pcv_vpc_store_get_subnet(PcvVpcStore *store,
                                     const gchar *id,
                                     const gchar *tenant,
                                     GError **error);
JsonArray *pcv_vpc_store_list_attachments(PcvVpcStore *store,
                                          const gchar *vpc_id,
                                          const gchar *tenant,
                                          GError **error);
JsonObject *pcv_vpc_store_get_attachment(PcvVpcStore *store,
                                         const gchar *id,
                                         const gchar *tenant,
                                         GError **error);
JsonArray *pcv_vpc_store_list_publishes(PcvVpcStore *store,
                                        const gchar *vpc_id,
                                        const gchar *tenant,
                                        GError **error);
JsonObject *pcv_vpc_store_get_publish(PcvVpcStore *store,
                                      const gchar *id,
                                      const gchar *tenant,
                                      GError **error);
JsonObject *pcv_vpc_store_ensure_ovn_binding(PcvVpcStore *store,
                                             const gchar *vpc_id,
                                             const gchar *tenant,
                                             const gchar *transit_pool,
                                             GError **error);
JsonObject *pcv_vpc_store_get_backend_binding(PcvVpcStore *store,
                                               const gchar *vpc_id,
                                               const gchar *tenant,
                                               GError **error);

   
                           
                                                                             
                                                                   
                                                     
  
                                                      
                            
   
gboolean pcv_vpc_store_create_subnet(PcvVpcStore *store,
                                      const gchar *vpc_id,
                                      const gchar *tenant,
                                      const gchar *name,
                                      const gchar *cidr,
                                      gint mtu,
                                      gint64 expected_revision,
                                      gchar **id_out,
                                      gchar **bridge_out,
                                      gint64 *revision_out,
                                      GError **error);
gboolean pcv_vpc_store_delete_subnet(PcvVpcStore *store,
                                      const gchar *id,
                                      const gchar *tenant,
                                      GError **error);

gboolean pcv_vpc_store_allocate_attachment(PcvVpcStore *store,
                                            const gchar *subnet_id,
                                            const gchar *tenant,
                                            const gchar *vm_uuid,
                                            const gchar *vm_name,
                                            const gchar *owner_subject,
                                            const gchar *requested_ip,
                                            JsonObject **attachment_out,
                                            GError **error);
gboolean pcv_vpc_store_delete_attachment(PcvVpcStore *store,
                                          const gchar *id,
                                          const gchar *tenant,
                                          GError **error);

gboolean pcv_vpc_store_create_publish(PcvVpcStore *store,
                                       const gchar *attachment_id,
                                       const gchar *tenant,
                                       const gchar *protocol,
                                       const gchar *listen_address,
                                       gint listen_port,
                                       gint target_port,
                                       GPtrArray *allowed_sources,
                                       gchar **id_out,
                                       GError **error);
gboolean pcv_vpc_store_delete_publish(PcvVpcStore *store,
                                       const gchar *id,
                                       const gchar *tenant,
                                       GError **error);

   
                           
                                                                 
                                                                    
                                                      
  
                                                  
                              
   
gboolean pcv_vpc_store_set_resource_state(PcvVpcStore *store,
                                           const gchar *table,
                                           const gchar *id,
                                           const gchar *state,
                                           const gchar *last_error,
                                           GError **error);

   
                           
                                                                         
                                                               
                                                                       
  
                                                           
                                       
   
gboolean pcv_vpc_store_bridge_is_managed(PcvVpcStore *store,
                                          const gchar *bridge_name);
gboolean pcv_vpc_store_mac_is_managed(PcvVpcStore *store,
                                      const gchar *mac_address);
gboolean pcv_vpc_store_vm_is_attached(PcvVpcStore *store,
                                      const gchar *vm_identifier);
gboolean pcv_vpc_store_vm_has_publish(PcvVpcStore *store,
                                      const gchar *vm_identifier);
GPtrArray *pcv_vpc_store_list_managed_bridges(PcvVpcStore *store,
                                               GError **error);
PcvVpcPolicySnapshot *pcv_vpc_store_policy_snapshot(PcvVpcStore *store,
                                                     GError **error);

G_END_DECLS
