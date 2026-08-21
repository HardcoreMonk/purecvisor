   
                                                                          
  
                           
                                                   
                                                    
                                        
  
                                                        
                                                       
                                    
  
                                                                         
                                                  
                                                          
                                                                          
                                                           
                                                           
                                                    
                                                         
                                                             
                                                                   
                                                                     
                                                                        
   
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>                 
#include "pcv_bpf_shared.h"

                                                               
                                                            
char LICENSE[] SEC("license") = "GPL";

                                                          
                                                    
                                                           
                                                          
                                         
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 20);              
} pcv_lsm_events SEC(".maps");

                                              
                                                         
                                        
                                                           
                                                          
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} pcv_daemon_cgroup SEC(".maps");

                                                   
                                                      
                                                                         
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 2);
    __type(key, __u32);
    __type(value, __u64);
} pcv_lsm_counters SEC(".maps");

                                                              
                                                       
                                                               
                                               
static __always_inline __u64 daemon_cgroup_id(void) {
    __u32 k = 0;                                                             
    __u64 *v = bpf_map_lookup_elem(&pcv_daemon_cgroup, &k);
    return v ? *v : 0;                                                              
}

                                                      
                                                
                                                                  
static __always_inline void bump(__u32 idx) {
    __u64 *c = bpf_map_lookup_elem(&pcv_lsm_counters, &idx);
    if (c) __sync_fetch_and_add(c, 1);                                                 
}

                                                            
                                                        
                                                
                                            
                                                               
                                                                    
static __always_inline void emit(__u32 hook, const char *path) {
    struct pcv_lsm_event *e =
        bpf_ringbuf_reserve(&pcv_lsm_events, sizeof(*e), 0);                       
    if (!e) { bump(1); return; }                             
    __builtin_memset(e, 0, sizeof(*e));                                                          
    e->ktime_ns  = bpf_ktime_get_ns();                                          
    e->pid       = bpf_get_current_pid_tgid() >> 32;                                 
    e->cgroup_id = bpf_get_current_cgroup_id();                             
    e->hook      = hook;
    bpf_get_current_comm(&e->comm, sizeof(e->comm));                        
    if (path) bpf_probe_read_kernel_str(&e->path, sizeof(e->path), path);                     
    else      e->path[0] = '\0';                                             
    bpf_ringbuf_submit(e, 0);                                                 
    bump(0);                
}

                                               
                                                          
                                                     
                                    
                                                     
                                   
                                                                   
                                                   
SEC("lsm/bprm_check_security")
int BPF_PROG(pcv_bprm, struct linux_binprm *bprm, int ret_prev)
{
    __u64 dcg = daemon_cgroup_id();                                                      
    if (dcg && bpf_get_current_cgroup_id() == dcg) {                                  
        const char *fn = BPF_CORE_READ(bprm, filename);                                
        emit(PCV_LSM_HOOK_BPRM, fn);
    }
    return ret_prev;                               
}

                                     
                                                
                                               
                                                  
                          
                                                     
                                                      
                                                              
                                                      
SEC("lsm/file_open")
int BPF_PROG(pcv_file_open, struct file *file, int ret_prev)
{
    __u64 dcg = daemon_cgroup_id();
    if (dcg && bpf_get_current_cgroup_id() == dcg)                                 
        return ret_prev;                  

                              
                                                      
                                                          
    char buf[PCV_LSM_PATH_MAX];
    long n = bpf_d_path(&file->f_path, buf, sizeof(buf));
    if (n <= 0) return ret_prev;                                                       

                                                            
                                                                 
                                                              
                                                                  
                                                                  
                                                
                                                                                 
                                                   
                                                       
                                                         
                                           
#define PCV_CRIT(p) (n >= (long)(sizeof(p) - 1) && bpf_strncmp(buf, sizeof(p) - 1, (p)) == 0)                               
    if (PCV_CRIT("/etc/purecvisor") ||                                    
        PCV_CRIT("/var/lib/purecvisor") ||                                    
        PCV_CRIT("/usr/lib/purecvisor/bpf") ||                                     
        PCV_CRIT("/etc/systemd/system/purecvisor") ||                          
        PCV_CRIT("/usr/lib/systemd/system/purecvisor") ||                  
        PCV_CRIT("/lib/systemd/system/purecvisor"))                                   
        emit(PCV_LSM_HOOK_FILE_OPEN, buf);                                    
#undef PCV_CRIT
    return ret_prev;                               
}
