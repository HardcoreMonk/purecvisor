#include <glib.h>
#include "modules/security/security_policy.h"

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

void
test_security_policy_register(void)
{
    g_test_add_func("/security/policy/recommend-actions",
                    test_security_policy_recommend_actions);
    g_test_add_func("/security/policy/normalize-runtime-severity",
                    test_security_policy_normalize_runtime_severity);
}
