                                                                                            
                                                                                                
                                              
                                                        
                       
                               
  
                                                                  
  
                 
                                                                         
                                                                         
             
                                                                      
                                                    
                                                               
                                            
  
                                        
   
#include <glib.h>
#include "modules/security/security_policy.h"
#include "modules/lxc/lxc_volume_guard.h"

static void
test_security_policy_recommend_actions(void)
{
    PcvSecurityEvent ev_for_ip = {0};
    ev_for_ip.type = PCV_SECURITY_EVENT_AUTH_BRUTEFORCE;
    ev_for_ip.target_kind = PCV_SECURITY_TARGET_IP;
    g_strlcpy(ev_for_ip.target, "192.0.2.10", sizeof ev_for_ip.target);
    g_assert_cmpstr(pcv_security_policy_recommend_action(&ev_for_ip), ==, "block_ip");

    PcvSecurityEvent ev_for_api_key = {0};
    ev_for_api_key.target_kind = PCV_SECURITY_TARGET_API_KEY;
    g_strlcpy(ev_for_api_key.target, "ak_test", sizeof ev_for_api_key.target);
    g_assert_cmpstr(pcv_security_policy_recommend_action(&ev_for_api_key), ==, "revoke_api_key");

    PcvSecurityEvent ev_for_process = {0};
    ev_for_process.target_kind = PCV_SECURITY_TARGET_PROCESS;
    g_strlcpy(ev_for_process.target, "pid:123", sizeof ev_for_process.target);
    g_assert_cmpstr(pcv_security_policy_recommend_action(&ev_for_process), ==, "manual_runbook");
}

static void
test_security_policy_normalize_runtime_severity(void)
{
    PcvSecurityEvent ev_for_runtime = {0};
    ev_for_runtime.source = PCV_SECURITY_SOURCE_RUNTIME;
    ev_for_runtime.severity = PCV_SECURITY_SEVERITY_CRIT;
    ev_for_runtime.target_kind = PCV_SECURITY_TARGET_PROCESS;
    g_strlcpy(ev_for_runtime.target, "pid:123", sizeof ev_for_runtime.target);

    g_assert_cmpint(pcv_security_policy_normalize_runtime_severity(&ev_for_runtime),
                    ==, PCV_SECURITY_SEVERITY_WARN);
}

static PcvSecuritySeverity
lsm_exec_severity(PcvSecuritySource source, PcvSecurityEventType type, const gchar *target)
{
    PcvSecurityEvent ev = {0};
    ev.source = source;
    ev.type = type;
    ev.severity = PCV_SECURITY_SEVERITY_WARN;
    ev.target_kind = PCV_SECURITY_TARGET_PROCESS;
    g_strlcpy(ev.target, target, sizeof ev.target);
    PcvSecuritySeverity got = pcv_security_policy_normalize_severity(&ev);

    g_assert_cmpint(pcv_security_policy_should_audit(&ev), ==, got != PCV_SECURITY_SEVERITY_INFO);
    return got;
}

#define LSM_PROC(target) \
    lsm_exec_severity(PCV_SECURITY_SOURCE_LSM, PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS, (target))

static void
test_security_policy_known_tool_paths_info(void)
{

    static const gchar *const dirs[] = { "/usr/sbin", "/usr/bin", "/sbin", "/bin" };
    static const gchar *const tools[] = { "ip", "systemctl", "zpool" };
    for (guint d = 0; d < G_N_ELEMENTS(dirs); d++) {
        for (guint t = 0; t < G_N_ELEMENTS(tools); t++) {
            g_autofree gchar *path = g_strdup_printf("%s/%s", dirs[d], tools[t]);
            g_assert_cmpint(LSM_PROC(path), ==, PCV_SECURITY_SEVERITY_INFO);
        }
    }

    g_assert_cmpint(LSM_PROC("/usr/local/bin/purecvisorsd"), ==, PCV_SECURITY_SEVERITY_INFO);

    g_assert_cmpint(LSM_PROC(PCV_LXC_VOLUME_GUARD_HOOK), ==, PCV_SECURITY_SEVERITY_INFO);
}

static void
test_security_policy_untrusted_or_never_downgrade_stays_warn(void)
{

    g_assert_cmpint(LSM_PROC("/usr/local/bin/ip"), ==, PCV_SECURITY_SEVERITY_WARN);
    g_assert_cmpint(LSM_PROC("/usr/local/sbin/zfs"), ==, PCV_SECURITY_SEVERITY_WARN);

    static const gchar *const never_lower[] = {
        "sh", "bash", "dash", "zsh", "busybox", "curl", "wget", "nc", "ncat", "socat",
        "ssh", "scp", "openssl", "python3", "perl", "nsenter",
    };
    g_assert_cmpuint(G_N_ELEMENTS(never_lower), ==, 16);
    for (guint i = 0; i < G_N_ELEMENTS(never_lower); i++) {
        g_autofree gchar *path = g_strdup_printf("/usr/bin/%s", never_lower[i]);
        g_assert_cmpint(LSM_PROC(path), ==, PCV_SECURITY_SEVERITY_WARN);
    }

    static const gchar *const others[] = {
        "/usr/bin/foo", "ip", "", "/usr/sbin//ip", "/usr/sbin/../sbin/ip", "/tmp/ip",
        "/usr/sbin/ip ", "/usr/sbin/", "/ip", "/usr/local/bin/purecvisorsd.new",
        "/usr/local/bin/pcvctlx", "/usr/local/bin/pcvctl/", "/usr/local/bin/pcvctl ", "/usr/bin/pcvctl",
    };
    for (guint i = 0; i < G_N_ELEMENTS(others); i++)
        g_assert_cmpint(LSM_PROC(others[i]), ==, PCV_SECURITY_SEVERITY_WARN);
}

static void
test_security_policy_known_tool_rule_scoped_to_lsm_process(void)
{

    g_assert_cmpint(lsm_exec_severity(PCV_SECURITY_SOURCE_FILE_INTEGRITY,
                                      PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS, "/usr/sbin/ip"),
                    ==, PCV_SECURITY_SEVERITY_WARN);
    g_assert_cmpint(lsm_exec_severity(PCV_SECURITY_SOURCE_LOG,
                                      PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS, "/usr/bin/systemctl"),
                    ==, PCV_SECURITY_SEVERITY_WARN);
    g_assert_cmpint(lsm_exec_severity(PCV_SECURITY_SOURCE_LSM,
                                      PCV_SECURITY_EVENT_FILE_ACCESS_SENSITIVE, "/usr/sbin/ip"),
                    ==, PCV_SECURITY_SEVERITY_WARN);

    g_assert_cmpint(lsm_exec_severity(PCV_SECURITY_SOURCE_RUNTIME,
                                      PCV_SECURITY_EVENT_PROCESS_SUSPICIOUS, "/usr/sbin/ip"),
                    ==, PCV_SECURITY_SEVERITY_WARN);
}

void
test_security_policy_register(void)
{
    g_test_add_func("/security/policy/recommend-actions",
                    test_security_policy_recommend_actions);
    g_test_add_func("/security/policy/normalize-runtime-severity",
                    test_security_policy_normalize_runtime_severity);
    g_test_add_func("/security/policy/known_tool_paths_info",
                    test_security_policy_known_tool_paths_info);
    g_test_add_func("/security/policy/untrusted_or_never_downgrade_stays_warn",
                    test_security_policy_untrusted_or_never_downgrade_stays_warn);
    g_test_add_func("/security/policy/known_tool_rule_scoped_to_lsm_process",
                    test_security_policy_known_tool_rule_scoped_to_lsm_process);
}
