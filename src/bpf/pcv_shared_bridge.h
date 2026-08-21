   
                            
                                                               
  
                           
                                                                        
                                                                         
                                                                    
                                    
  
                       
                                                       
                                                  
   
#ifndef PURECVISOR_PCV_SHARED_BRIDGE_ABI_H
#define PURECVISOR_PCV_SHARED_BRIDGE_ABI_H

#ifndef __VMLINUX_H__
#include <linux/types.h>
#endif

#define PCV_SHARED_BPF_REVISION 1U
#define PCV_SHARED_MARK_INTERNAL (1U << 31)
#define PCV_SHARED_TC_PRIORITY 49152U
#define PCV_SHARED_TC_HANDLE_PHYS_INGRESS 0xC0440001U
#define PCV_SHARED_TC_HANDLE_PHYS_EGRESS  0xC0440002U
#define PCV_SHARED_TC_HANDLE_PORTAL       0xC0440003U

enum pcv_shared_link_role {
    PCV_SHARED_ROLE_PHYSICAL = 1,
    PCV_SHARED_ROLE_PORTAL = 2,
};

struct pcv_shared_link_config {
    __u32 revision;
    __u32 generation;
    __u32 active;
    __u32 role;
    __u32 peer_ifindex;
    __u32 physical_ifindex;
    __u32 mtu;
    __u8 host_mac[6];
    __u8 reserved[2];
};

struct pcv_shared_guest_key {
    __u32 physical_ifindex;
    __u8 mac[6];
    __u8 reserved[2];
};

struct pcv_shared_guest_value {
    __u32 portal_ifindex;
    __u32 generation;
    __u64 last_seen_ns;
};

enum pcv_shared_stat_index {
    PCV_SHARED_STAT_PHYS_PASS = 0,
    PCV_SHARED_STAT_LAN_TO_GUEST,
    PCV_SHARED_STAT_HOST_TO_GUEST,
    PCV_SHARED_STAT_GUEST_TO_LAN,
    PCV_SHARED_STAT_GUEST_TO_HOST,
    PCV_SHARED_STAT_GUEST_LOCAL,
    PCV_SHARED_STAT_GUEST_DROP,
    PCV_SHARED_STAT_REDIRECT_ERROR,
    PCV_SHARED_STAT_MAX,
};

#endif
