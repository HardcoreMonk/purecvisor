#ifndef PURECVISOR_VM_CONFIG_HISTORY_NUDGE_H
#define PURECVISOR_VM_CONFIG_HISTORY_NUDGE_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

typedef enum {
    PCV_VMCH_NUDGE_UNKNOWN = 0,
    PCV_VMCH_NUDGE_INTENT,
    PCV_VMCH_NUDGE_EXEMPT
} PcvVmCfgNudgeClass;

typedef struct {
    const gchar *method;
    const gchar *dev_param;
    const gchar *size_gb_param;
} PcvVmCfgNudgeEntry;

PcvVmCfgNudgeClass pcv_vm_cfg_nudge_classify(const gchar *method, const PcvVmCfgNudgeEntry **entry);

void pcv_vm_cfg_nudge_authorized_hook(const gchar *method, JsonObject *params, gpointer user_data);

void pcv_vm_cfg_nudge_audit_tap(const gchar *username, const gchar *method, const gchar *target, const gchar *result,
                                gint error_code);

G_END_DECLS

#endif
