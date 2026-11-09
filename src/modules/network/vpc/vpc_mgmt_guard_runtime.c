   
                                 
                                                      
  
                           
                                                                    
                                                                           
                                                                                 
                                                                
                                                                                   
                                                                          
              
                                                                   
                                                             
                                                                       
                                     
  
                       
                                                          
                                                           
                                                      
                                                     
   
#include "vpc_mgmt_guard.h"

#include "api/drain.h"
#include "modules/audit/pcv_audit.h"
#include "vpc_manager.h"

                                                                      
#define ARM_MAX_SEC (G_MAXUINT / 1000u)

                                                                
static void
runtime_audit(const gchar *actor, const gchar *method, const gchar *target,
              const gchar *result, gint code)
{
    pcv_audit_log(actor, method, target, result, code, 0, "local");
}

static const PcvVpcMgmtOps s_runtime_ops = {
    .subnet_delete = pcv_vpc_subnet_delete,
    .vpc_delete = pcv_vpc_delete,
    .reconcile = pcv_vpc_reconcile,
    .audit = runtime_audit,
    .arm = pcv_vpc_mgmt_arm_pending,
};

                                                                    
const PcvVpcMgmtOps *
pcv_vpc_mgmt_runtime_ops(void)
{
    return &s_runtime_ops;
}

static gboolean expire_cb(gpointer data);

                                                                            
                        
static void
arm_in(const gchar *subnet_id, gint64 left)
{
    guint secs = left < 1 ? 1u : (left > (gint64)ARM_MAX_SEC ? ARM_MAX_SEC : (guint)left);
    g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, secs, expire_cb, g_strdup(subnet_id), g_free);
}

                                                                     
                                                                     
                                                                
                                                              
                                                            
static void
expire_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
    (void)source;
    (void)cancel;
    const gchar *subnet_id = task_data;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gint64 remaining = 0;
    if (pcv_vpc_mgmt_expiry_due(subnet_id, now, &remaining)) {
        PcvVpcMgmtPending *claimed = pcv_vpc_mgmt_pending_claim_for_revert(subnet_id);
        if (claimed) (void)pcv_vpc_mgmt_revert(&s_runtime_ops, claimed, FALSE, now);
    } else if (remaining > 0) {
        arm_in(subnet_id, remaining);
    }
    g_task_return_boolean(task, TRUE);
}

                                                             
                                                 
static gboolean
expire_cb(gpointer data)
{
    GTask *task = pcv_drain_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, g_strdup(data), g_free);
    g_task_run_in_thread(task, expire_worker);
    g_object_unref(task);
    return G_SOURCE_REMOVE;
}

                                                         
                                           
void
pcv_vpc_mgmt_arm_pending(const PcvVpcMgmtPending *p)
{
    if (!p || !p->subnet_id) return;
    arm_in(p->subnet_id, p->deadline_unix - g_get_real_time() / G_USEC_PER_SEC);
}

                                                                
                                                     
gboolean
pcv_vpc_mgmt_boot_reconcile(GError **error)
{
    return pcv_vpc_mgmt_boot_with(pcv_vpc_mgmt_runtime_ops(), g_get_real_time() / G_USEC_PER_SEC,
                                  error);
}
