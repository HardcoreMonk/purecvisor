   
                                
                                                           
  
                           
                                                                         
                                                                             
                                                                           
                                                                           
                                                                            
  
                       
                                                        
                                                          
  
                                                                               
   
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include "pcv_shared_bridge.h"

#define TC_ACT_OK 0
#define TC_ACT_SHOT 2
#define PCV_ETH_ALEN 6
#define PCV_ETH_HLEN 14

char LICENSE[] SEC("license") = "GPL";

struct pcv_ethhdr {
    __u8 dst[PCV_ETH_ALEN];
    __u8 src[PCV_ETH_ALEN];
    __be16 proto;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 128);
    __type(key, __u32);
    __type(value, struct pcv_shared_link_config);
} pcv_sh_links SEC(".maps");

                                                                    
                                                                
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 4096);
    __type(key, struct pcv_shared_guest_key);
    __type(value, struct pcv_shared_guest_value);
} pcv_sh_guests SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, PCV_SHARED_STAT_MAX);
    __type(key, __u32);
    __type(value, __u64);
} pcv_sh_stats SEC(".maps");

static __always_inline void stat_inc(__u32 index)
{
    __u64 *counter = bpf_map_lookup_elem(&pcv_sh_stats, &index);
    if (counter) *counter += 1;
}

static __always_inline int load_eth(struct __sk_buff *skb, struct pcv_ethhdr *eth)
{
    return bpf_skb_load_bytes(skb, 0, eth, PCV_ETH_HLEN);
}

static __always_inline bool mac_equal(const __u8 a[6], const __u8 b[6])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]
        && a[3] == b[3] && a[4] == b[4] && a[5] == b[5];
}

static __always_inline bool mac_invalid_source(const __u8 mac[6])
{
    return (mac[0] & 1U) != 0U
        || (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0U;
}

static __always_inline bool mac_group(const __u8 mac[6])
{
    return (mac[0] & 1U) != 0U;
}

static __always_inline struct pcv_shared_guest_value *
guest_lookup(__u32 physical_ifindex, const __u8 mac[6],
             struct pcv_shared_guest_key *key)
{
    __builtin_memset(key, 0, sizeof(*key));
    key->physical_ifindex = physical_ifindex;
    __builtin_memcpy(key->mac, mac, sizeof(key->mac));
    return bpf_map_lookup_elem(&pcv_sh_guests, key);
}

static __always_inline bool config_active(const struct pcv_shared_link_config *cfg)
{
    return cfg && cfg->revision == PCV_SHARED_BPF_REVISION && cfg->active == 1U
        && cfg->generation != 0U && cfg->peer_ifindex != 0U
        && cfg->physical_ifindex != 0U;
}

                                                              
SEC("tc")
int pcv_phys_ing(struct __sk_buff *skb)
{
    if ((skb->mark & PCV_SHARED_MARK_INTERNAL) != 0U) {
        skb->mark &= ~PCV_SHARED_MARK_INTERNAL;
        stat_inc(PCV_SHARED_STAT_PHYS_PASS);
        return TC_ACT_OK;
    }

    __u32 ifindex = skb->ifindex;
    struct pcv_shared_link_config *cfg = bpf_map_lookup_elem(&pcv_sh_links, &ifindex);
    if (!config_active(cfg) || cfg->role != PCV_SHARED_ROLE_PHYSICAL) {
        stat_inc(PCV_SHARED_STAT_PHYS_PASS);
        return TC_ACT_OK;
    }

    struct pcv_ethhdr eth = {};
    if (load_eth(skb, &eth) != 0) {
        stat_inc(PCV_SHARED_STAT_PHYS_PASS);
        return TC_ACT_OK;
    }
    struct pcv_shared_guest_key key = {};
    struct pcv_shared_guest_value *guest =
        guest_lookup(cfg->physical_ifindex, eth.dst, &key);
    if (mac_group(eth.dst)
        || (guest && guest->generation == cfg->generation
            && guest->portal_ifindex == cfg->peer_ifindex)) {
        if (bpf_clone_redirect(skb, cfg->peer_ifindex, 0) < 0)
            stat_inc(PCV_SHARED_STAT_REDIRECT_ERROR);
        else
            stat_inc(PCV_SHARED_STAT_LAN_TO_GUEST);
    }
    stat_inc(PCV_SHARED_STAT_PHYS_PASS);
    return TC_ACT_OK;
}

                                                                        
                                                                        
SEC("tc")
int pcv_phys_eg(struct __sk_buff *skb)
{
    if ((skb->mark & PCV_SHARED_MARK_INTERNAL) != 0U) {
        skb->mark &= ~PCV_SHARED_MARK_INTERNAL;
        stat_inc(PCV_SHARED_STAT_PHYS_PASS);
        return TC_ACT_OK;
    }

    __u32 ifindex = skb->ifindex;
    struct pcv_shared_link_config *cfg = bpf_map_lookup_elem(&pcv_sh_links, &ifindex);
    if (!config_active(cfg) || cfg->role != PCV_SHARED_ROLE_PHYSICAL) {
        stat_inc(PCV_SHARED_STAT_PHYS_PASS);
        return TC_ACT_OK;
    }
    struct pcv_ethhdr eth = {};
    if (load_eth(skb, &eth) != 0) return TC_ACT_OK;

    struct pcv_shared_guest_key key = {};
    struct pcv_shared_guest_value *guest =
        guest_lookup(cfg->physical_ifindex, eth.dst, &key);
    if (guest && guest->generation == cfg->generation
        && guest->portal_ifindex == cfg->peer_ifindex) {
        int rc = bpf_redirect(cfg->peer_ifindex, 0);
        if (rc < 0) {
            stat_inc(PCV_SHARED_STAT_REDIRECT_ERROR);
            return TC_ACT_OK;
        }
        stat_inc(PCV_SHARED_STAT_HOST_TO_GUEST);
        return rc;
    }
    if (mac_group(eth.dst)) {
        if (bpf_clone_redirect(skb, cfg->peer_ifindex, 0) < 0)
            stat_inc(PCV_SHARED_STAT_REDIRECT_ERROR);
        else
            stat_inc(PCV_SHARED_STAT_HOST_TO_GUEST);
    }
    return TC_ACT_OK;
}

                                                                           
                                                                       
SEC("tc")
int pcv_portal_ing(struct __sk_buff *skb)
{
    __u32 ifindex = skb->ifindex;
    struct pcv_shared_link_config *cfg = bpf_map_lookup_elem(&pcv_sh_links, &ifindex);
    if (!config_active(cfg) || cfg->role != PCV_SHARED_ROLE_PORTAL) {
        stat_inc(PCV_SHARED_STAT_GUEST_DROP);
        return TC_ACT_SHOT;
    }
    struct pcv_ethhdr eth = {};
    if (load_eth(skb, &eth) != 0 || mac_invalid_source(eth.src)
        || mac_equal(eth.src, cfg->host_mac)) {
        stat_inc(PCV_SHARED_STAT_GUEST_DROP);
        return TC_ACT_SHOT;
    }

    struct pcv_shared_guest_key source_key = {};
    source_key.physical_ifindex = cfg->physical_ifindex;
    __builtin_memcpy(source_key.mac, eth.src, sizeof(source_key.mac));
    struct pcv_shared_guest_value source_value = {
        .portal_ifindex = ifindex,
        .generation = cfg->generation,
        .last_seen_ns = bpf_ktime_get_ns(),
    };
    if (bpf_map_update_elem(&pcv_sh_guests, &source_key, &source_value, BPF_ANY) < 0) {
        stat_inc(PCV_SHARED_STAT_GUEST_DROP);
        return TC_ACT_SHOT;
    }

    skb->mark |= PCV_SHARED_MARK_INTERNAL;
    if (bpf_clone_redirect(skb, cfg->physical_ifindex, BPF_F_INGRESS) < 0) {
        stat_inc(PCV_SHARED_STAT_REDIRECT_ERROR);
        return TC_ACT_SHOT;
    }
    stat_inc(PCV_SHARED_STAT_GUEST_TO_HOST);

    struct pcv_shared_guest_key destination_key = {};
    struct pcv_shared_guest_value *destination =
        guest_lookup(cfg->physical_ifindex, eth.dst, &destination_key);
    if (mac_equal(eth.dst, cfg->host_mac)) {
        stat_inc(PCV_SHARED_STAT_GUEST_LOCAL);
        return TC_ACT_SHOT;
    }
    if (destination && destination->generation == cfg->generation
        && destination->portal_ifindex == ifindex) {
        stat_inc(PCV_SHARED_STAT_GUEST_LOCAL);
        return TC_ACT_SHOT;
    }

    int rc = bpf_redirect(cfg->physical_ifindex, 0);
    if (rc < 0) {
        stat_inc(PCV_SHARED_STAT_REDIRECT_ERROR);
        return TC_ACT_SHOT;
    }
    stat_inc(PCV_SHARED_STAT_GUEST_TO_LAN);
    return rc;
}
