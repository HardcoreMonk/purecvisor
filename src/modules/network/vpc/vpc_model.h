   
                    
                                            
  
                           
                                                   
                                                    
                                        
  
                                                  
                                                  
  
                                                    
                                                           
                                                   
   
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    guint32 network;                            
    guint32 broadcast;                          
    guint8  prefix;
} PcvVpcIpv4Cidr;

typedef enum {
    PCV_VPC_ERROR_INVALID_ARGUMENT,
    PCV_VPC_ERROR_NOT_FOUND,
    PCV_VPC_ERROR_CONFLICT,
    PCV_VPC_ERROR_STALE_REVISION,
    PCV_VPC_ERROR_IO,
    PCV_VPC_ERROR_STATE,
} PcvVpcError;

#define PCV_VPC_ERROR pcv_vpc_error_quark()
GQuark pcv_vpc_error_quark(void);

gboolean pcv_vpc_name_is_valid(const gchar *name);
gboolean pcv_vpc_egress_mode_is_valid(const gchar *mode);
gboolean pcv_vpc_backend_is_valid(const gchar *backend);
gboolean pcv_vpc_protocol_is_valid(const gchar *protocol);
gboolean pcv_vpc_port_is_valid(gint port);

gboolean pcv_vpc_cidr_parse(const gchar *text,
                            PcvVpcIpv4Cidr *out,
                            gchar **canonical_out,
                            GError **error);
gboolean pcv_vpc_subnet_cidr_parse(const gchar *text,
                                   PcvVpcIpv4Cidr *out,
                                   gchar **canonical_out,
                                   GError **error);
gboolean pcv_vpc_cidr_overlaps(const PcvVpcIpv4Cidr *a,
                               const PcvVpcIpv4Cidr *b);
gboolean pcv_vpc_cidr_contains_ip(const PcvVpcIpv4Cidr *cidr,
                                  const gchar *ip);
gboolean pcv_vpc_cidr_usable_range(const PcvVpcIpv4Cidr *cidr,
                                   guint32 *first_out,
                                   guint32 *last_out);
gboolean pcv_vpc_sources_cover_all_ipv4(GPtrArray *sources);
gchar *pcv_vpc_ipv4_to_string(guint32 host_order_ip);

                                                                    
                                                               
gchar *pcv_vpc_dnsmasq_conf_arg(const gchar *conf_path);

                                                                   
                                                           
gchar *pcv_vpc_libvirt_metadata_payload(const gchar *metadata_xml,
                                        GError **error);

                                                                
                                                                  
gchar *pcv_vpc_interface_xml(const gchar *mac_address,
                             const gchar *bridge_name,
                             gint bridge_mtu);
gchar *pcv_vpc_ovn_interface_xml(const gchar *mac_address,
                                 const gchar *iface_id,
                                 gint bridge_mtu);

gchar *pcv_vpc_bridge_name_from_id(const gchar *subnet_id);
gchar *pcv_vpc_ovn_router_name_from_id(const gchar *vpc_id);
gchar *pcv_vpc_ovn_switch_name_from_id(const gchar *subnet_id);
gchar *pcv_vpc_ovn_port_name_from_id(const gchar *attachment_id);
gchar *pcv_vpc_ovn_edge_switch_name_from_id(const gchar *vpc_id);
gchar *pcv_vpc_ovn_edge_iface_name_from_id(const gchar *vpc_id);
gchar *pcv_vpc_mac_from_id(const gchar *attachment_id);

G_END_DECLS
