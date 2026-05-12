#include "modules/security/security_policy.h"






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
        || has_prefix(target, "/var/lib/purecvisor/rbac.db");
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
