   
                         
                                                                         
  
                           
                                                                          
                                                              
                                                 
                                                                       
                                                                       
                                                                             
  
                       
                                                             
                                                          
                                                   
   
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    gchar *ip_address;
    gchar *mac_address;
} PcvVpcPolicyAttachment;

typedef struct {
    gchar *id;
    gchar *backend;
    gchar *bridge_name;
    gchar *cidr;
    gchar *gateway;
    GPtrArray *attachments;                                                           
} PcvVpcPolicySubnet;

typedef struct {
    gchar *id;
    gchar *backend;
    gchar *edge_interface;
    gchar *egress_mode;
    GPtrArray *subnets;                                                       
} PcvVpcPolicyVpc;

typedef struct {
    gchar *protocol;
    gchar *listen_address;                              
    guint16 listen_port;
    gchar *target_ip;
    guint16 target_port;
    gchar *target_bridge;
    GPtrArray *allowed_sources;                                              
} PcvVpcPolicyPublish;

typedef struct {
    GPtrArray *vpcs;                                                         
    GPtrArray *publishes;                                                        
} PcvVpcPolicySnapshot;

                           
                                                                
                                                                               
                                                                                   
                                
  
                       
                                                         
                                                        
PcvVpcPolicySnapshot *pcv_vpc_policy_snapshot_new(void);
void pcv_vpc_policy_snapshot_free(PcvVpcPolicySnapshot *snapshot);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PcvVpcPolicySnapshot, pcv_vpc_policy_snapshot_free)

                           
                                                                     
                                                              
                                                              
                                                                  
  
                       
                                                          
                                                         
           
PcvVpcPolicyVpc *pcv_vpc_policy_vpc_new(const gchar *id, const gchar *egress_mode);
PcvVpcPolicySubnet *pcv_vpc_policy_subnet_new(const gchar *id,
                                               const gchar *bridge_name,
                                               const gchar *cidr,
                                               const gchar *gateway);
PcvVpcPolicyAttachment *pcv_vpc_policy_attachment_new(const gchar *ip,
                                                       const gchar *mac);
PcvVpcPolicyPublish *pcv_vpc_policy_publish_new(const gchar *protocol,
                                                 const gchar *listen_address,
                                                 guint16 listen_port,
                                                 const gchar *target_ip,
                                                 guint16 target_port,
                                                 const gchar *target_bridge);

                           
                                                                             
                                                              
                                                                        
                                                                 
                                                               
  
                       
                                                            
                                                              
                                           
gchar *pcv_vpc_policy_build_script(const PcvVpcPolicySnapshot *snapshot);
gchar *pcv_vpc_policy_build_quarantine_script(GPtrArray *managed_bridges);

G_END_DECLS
