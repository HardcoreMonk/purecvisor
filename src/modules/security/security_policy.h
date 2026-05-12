#ifndef PURECVISOR_SECURITY_POLICY_H
#define PURECVISOR_SECURITY_POLICY_H

#include "modules/security/security_event.h"

G_BEGIN_DECLS

PcvSecuritySeverity pcv_security_policy_normalize_severity(const PcvSecurityEvent *ev);
PcvSecuritySeverity pcv_security_policy_normalize_runtime_severity(const PcvSecurityEvent *ev);
const gchar *pcv_security_policy_recommend_action(const PcvSecurityEvent *ev);
gchar *pcv_security_policy_coalesce_key(const PcvSecurityEvent *ev);
gboolean pcv_security_policy_should_audit(const PcvSecurityEvent *ev);

G_END_DECLS

#endif
