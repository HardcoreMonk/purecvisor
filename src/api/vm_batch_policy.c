
#include "vm_batch_policy.h"

gboolean pcv_vm_batch_action_is_whitelisted(const gchar *action)
{

    static const gchar *WHITELIST[] = { "start", "stop", NULL };
    if (!action)
        return FALSE;
    for (int i = 0; WHITELIST[i]; i++)
        if (g_strcmp0(action, WHITELIST[i]) == 0)
            return TRUE;
    return FALSE;
}
