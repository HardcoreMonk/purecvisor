   
                                                                   
  
                           
                                                   
                                                    
                                        
  
                                                       
                                                        
                                                      
                            
  
                                                           
                                                                       
                                                          
                                                                 
                                                            
                                               
                                                                
                                            
                                                                              
                                                          
                                                                             
                                        
                                                               
                                                                      
                                
                                                              
                                                            
                                                         
                                                           
                                                                            
                                                                       
   
#include "modules/security/security_policy.h"
#include "modules/lxc/lxc_volume_guard.h"                                                              

#include <string.h>

  
                                                                                
                                                                                
                                                   
                                                          
   
                                                         
static gboolean
has_prefix(const gchar *s, const gchar *prefix)
{
    return s && prefix && g_str_has_prefix(s, prefix);
}

                                                
                                                    
static gboolean
contains_purecvisor_control_plane_path(const gchar *target)
{
      
                                                                             
                                                                               
                                                          
                                    
       
                                                       
                                                     
    return has_prefix(target, "/etc/purecvisor")
        || has_prefix(target, "/etc/systemd/system/purecvisor")
        || has_prefix(target, "/lib/systemd/system/purecvisor")
        || has_prefix(target, "/usr/lib/systemd/system/purecvisor")
        || has_prefix(target, "/var/lib/purecvisor/pcv_")
        || has_prefix(target, "/var/lib/purecvisor/rbac.db")
        || has_prefix(target, "/usr/lib/purecvisor/bpf");                  
                                                                        
                                                       
}

  
                                                                                
                                                                       
                                                                    
                                                       
                                                                             
                                                                                     
                                                                                               
                                                                                      
                                                                                  
                                                  
                                                                 
                                                                                    
                                                                         
                                              
   
static const gchar *const k_lsm_tool_dirs[] = { "/usr/sbin", "/usr/bin", "/sbin", "/bin" };
static const gchar *const k_lsm_known_tool_names[] = {
    "bridge", "chmod", "chown", "cp", "dd", "dnsmasq", "dpdk-devbind.py", "firewall-cmd",
    "fuser", "getcap", "guestfish", "ip", "iptables", "iscsiadm", "kill", "kpartx", "ls",
    "lspci", "lxc-attach", "lxc-checkpoint", "lxc-create", "lxc-destroy",
    "lxc-info", "lxc-stop", "lxc-usernsexec", "mdevctl", "mkdir", "modprobe", "nft",
    "nvidia-smi", "ovn-nbctl", "ovn-sbctl", "ovs-appctl", "ovs-vsctl", "ovsdb-client",
    "partprobe", "partx", "pgrep", "pkill", "qemu-img", "retis", "rm", "sha256sum", "ss",
    "ssh-keyscan", "suricata", "sysctl", "systemctl", "tar", "tc", "timeout",
    "udevadm", "ufw", "virsh", "virt-customize", "virt-filesystems", "virt-install",
    "virt-sysprep", "wg", "wipefs", "zfs", "zpool",
};
                                                                                                   
                                                                                                                        
                                                                               
static const gchar *const k_lsm_known_tool_exact[] = { "/usr/local/bin/purecvisorsd", PCV_LXC_VOLUME_GUARD_HOOK };

                                                         
                                                  
static gboolean
is_known_daemon_tool_path(const gchar *target)
{
    for (gsize i = 0; i < G_N_ELEMENTS(k_lsm_known_tool_exact); i++) {
        if (g_strcmp0(target, k_lsm_known_tool_exact[i]) == 0) {
            return TRUE;
        }
    }
    const gchar *slash = target ? strrchr(target, '/') : NULL;
    if (!slash) {
        return FALSE;                                            
    }
    gsize dir_len = (gsize)(slash - target);
    gboolean trusted_dir = FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(k_lsm_tool_dirs); i++) {
        trusted_dir = trusted_dir || (strlen(k_lsm_tool_dirs[i]) == dir_len
                                      && strncmp(target, k_lsm_tool_dirs[i], dir_len) == 0);
    }
    if (!trusted_dir) {
        return FALSE;                                                                     
    }
    for (gsize i = 0; i < G_N_ELEMENTS(k_lsm_known_tool_names); i++) {
        if (strcmp(slash + 1, k_lsm_known_tool_names[i]) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

                                            
                                                       
PcvSecuritySeverity
pcv_security_policy_normalize_runtime_severity(const PcvSecurityEvent *ev)
{
    if (!ev) {
        return PCV_SECURITY_SEVERITY_INFO;                              
    }

                                                  
                                                       
    if (contains_purecvisor_control_plane_path(ev->target)) {
        return PCV_SECURITY_SEVERITY_CRIT;
    }
    return PCV_SECURITY_SEVERITY_WARN;                                        
}

                                  
                                                           
PcvSecuritySeverity
pcv_security_policy_normalize_severity(const PcvSecurityEvent *ev)
{
    if (!ev) {
        return PCV_SECURITY_SEVERITY_INFO;               
    }

                                                            
    if (ev->source == PCV_SECURITY_SOURCE_RUNTIME) {
        return pcv_security_policy_normalize_runtime_severity(ev);
    }
                                                      
                                                       
    if (ev->type == PCV_SECURITY_EVENT_FILE_CHANGED
        && contains_purecvisor_control_plane_path(ev->target)) {
        return PCV_SECURITY_SEVERITY_CRIT;
    }
                                                                   
                                                                          
    if (ev->source == PCV_SECURITY_SOURCE_LSM
        && ev->type == PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS
        && is_known_daemon_tool_path(ev->target)) {
        return PCV_SECURITY_SEVERITY_INFO;
    }
    return ev->severity;                                           
}

                           
                                                                   
const gchar *
pcv_security_policy_recommend_action(const PcvSecurityEvent *ev)
{
    if (!ev) {
        return "manual_runbook";                                
    }

      
                                                                                  
                                                           
                                                                    
       
                                              
      
                                         
                                                             
                                                                     
                                                                   
                                                                         
                                                                     
                                                        
                                                                   
                                    
                                                                        
                                    
      
                                                   
                                          
                                                                 
                                                              
                                                            
                                                       
                                                                    
    if (ev->target_kind == PCV_SECURITY_TARGET_IP
        && ev->type == PCV_SECURITY_EVENT_AUTH_BRUTEFORCE) {
        return "block_ip";
    }
                                                  
    if (ev->target_kind == PCV_SECURITY_TARGET_API_KEY) {
        return "revoke_api_key";
    }
      
                                                                  
                                                                   
                                                           
                                                                     
                                                   
                                                      
                                                           
       
                                                                           
                                                         
    if (ev->type == PCV_SECURITY_EVENT_NETWORK_THREAT && ev->recommended_action[0] != '\0') {
        return ev->recommended_action;
    }
    return "manual_runbook";                                 
}

                            
                                                                
gchar *
pcv_security_policy_coalesce_key(const PcvSecurityEvent *ev)
{
    return pcv_security_event_coalesce_key(ev);                                       
}

                       
                                                          
gboolean
pcv_security_policy_should_audit(const PcvSecurityEvent *ev)
{
                                                          
    PcvSecuritySeverity severity = pcv_security_policy_normalize_severity(ev);
    return severity == PCV_SECURITY_SEVERITY_WARN
        || severity == PCV_SECURITY_SEVERITY_CRIT;
}
