                                                                             
                                    
                                                                   
                                                               
  
                           
                                                                                 
                                                                                  
                                                                               
                                                                  
                                                                                 
                                                                         
                                                                       
                                                                                        
                                                               
                                                                            
                                                                                   
                                                                             
                                                               
                           
  
                                                                 
                                                                    
                                                                                
#ifndef PURECVISOR_GUEST_IP_H
#define PURECVISOR_GUEST_IP_H

#include <glib.h>
#include <libvirt/libvirt.h>

G_BEGIN_DECLS

                                           
#define PCV_GUEST_AGENT_TTL_US       ((gint64)60 * G_USEC_PER_SEC)
                    
#define PCV_GUEST_AGENT_BACKOFF_MIN_S 60
                
#define PCV_GUEST_AGENT_BACKOFF_MAX_S 600

                                                        
typedef struct {
    gchar     *mac;
    GPtrArray *ipv4;
    GPtrArray *ipv6;
} PcvGuestIface;

                                           
void pcv_guest_iface_free(gpointer iface);

                                                                                      
GPtrArray *pcv_guest_ip_parse_qga_interfaces(const gchar *qga_json, GError **error);

                                                                                                
                                                                                      
GPtrArray *pcv_guest_ip_from_virt_ifaces(virDomainInterfacePtr *ifaces, int count);

                                                                                
typedef enum {
    PCV_GUEST_IP_ANY_FIRST_IPV4,
    PCV_GUEST_IP_MAC_IPV4_THEN_ANY,
} PcvGuestIpRule;

                                                             
typedef struct {
    const GPtrArray *lease_ifaces;                                                      
    const GPtrArray *agent_ifaces;                                     
    const gchar     *lease_file_text;                                     
    const gchar     *proc_net_arp_text;                         
    gboolean         arp_require_complete;                                    
} PcvGuestIpInputs;

                                                                       
gchar *pcv_guest_ip_resolve(const PcvGuestIpInputs *in, const gchar *mac,
                            PcvGuestIpRule rule, const gchar **source_out);

                                                                      
gchar *pcv_guest_ip_lease_file_lookup(const gchar *lease_text, const gchar *mac);

                                                                 
gboolean pcv_guest_ip_arp_lookup(const gchar *proc_net_arp, const gchar *mac,
                                 gboolean require_complete, gchar *ip_out, gsize ip_out_len);

                                                    
typedef enum {
    PCV_GUEST_AGENT_NONE = 0,
    PCV_GUEST_AGENT_OK,
    PCV_GUEST_AGENT_UNRESPONSIVE,
} PcvGuestAgentState;

                                                      
typedef struct {
    gint64             fetched_us;                  
    gint64             next_try_us;                         
    gint               backoff_s;                          
    PcvGuestAgentState state;
    gboolean           inflight;                   
} PcvGuestAgentSlot;

                            
gboolean pcv_guest_agent_slot_fresh(const PcvGuestAgentSlot *s, gint64 now_us);
                                            
gboolean pcv_guest_agent_slot_should_probe(const PcvGuestAgentSlot *s, gint64 now_us);
                                       
void pcv_guest_agent_slot_on_success(PcvGuestAgentSlot *s, gint64 now_us);
                                                    
gint pcv_guest_agent_slot_on_failure(PcvGuestAgentSlot *s, gint64 now_us);

G_END_DECLS

#endif                            
