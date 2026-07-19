
#ifndef PURECVISOR_VM_BATCH_POLICY_H
#define PURECVISOR_VM_BATCH_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

gboolean pcv_vm_batch_action_is_whitelisted(const gchar *action);

G_END_DECLS

#endif
