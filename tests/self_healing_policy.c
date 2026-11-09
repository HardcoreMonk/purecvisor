                                                                                           
                                                                                   
                                                                          
                               
#include "modules/ai/self_healing.c"

void pcv_test_healing_queue_approval(const gchar *policy, const gchar *action, const gchar *reason);

                                                           
void
pcv_test_healing_queue_approval(const gchar *policy, const gchar *action, const gchar *reason)
{
    HealingPolicy p = {0};
    g_strlcpy(p.name, policy, sizeof(p.name));
    g_strlcpy(p.action, action, sizeof(p.action));
    g_mutex_lock(&G.mu);
    _queue_approval(&p, reason);
    g_mutex_unlock(&G.mu);
}
