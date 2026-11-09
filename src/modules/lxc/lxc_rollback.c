                                                                             
                                 
                                                                                                       
  
                           
                                                                                                                          
                                                                                                 
                                                                                            
                                                                                                               
                                                                                            
                                                                                             
                                                
                                                                                
                                                                                                        
  
                                                                     
                          
                                                                                
#include "modules/lxc/lxc_driver.h"
#include "modules/lxc/lxc_volume_guard.h"                                                  
#include "modules/lxc/lxc_volume_mount.h"                                                         
#include "api/drain.h"                                                                 
#include "utils/pcv_spawn.h"

                                                                             

                                                                                                             
                                     
static gboolean
_rb_snapshot_exists(const gchar *snapshot)
{
    const gchar *argv[] = { "zfs", "list", "-H", "-t", "snapshot", "-o", "name", snapshot, NULL };
    g_autofree gchar *out = NULL;
    return pcv_spawn_sync(argv, &out, NULL, NULL);                                
}

                                                                                    
static gboolean
_rb_residue_clear(const gchar *lxc_path, const gchar *name, GError **error)
{
    return pcv_lxc_rootfs_residue_clear(lxc_path, name, NULL, error);
}

                                                                                                        
                                                    
static gboolean
_rb_zfs_rollback(const gchar *snapshot, GError **error)
{
    const gchar *argv[] = { "zfs", "rollback", "-r", snapshot, NULL };
    g_autofree gchar *err = NULL;
    return pcv_spawn_sync(argv, NULL, &err, error);
}

                                                                    
static const PcvLxcRollbackOps G_rb_real_ops = {
    .snapshot_exists = _rb_snapshot_exists,
    .state           = pcv_lxc_state_of,
    .halt            = pcv_lxc_halt,
    .residue_clear   = _rb_residue_clear,
    .zfs_rollback    = _rb_zfs_rollback,
    .forget          = pcv_lxc_volume_guard_forget,
};
static const PcvLxcRollbackOps *G_rb_ops = &G_rb_real_ops;

                                                                                         
                                                     
                                                            
void
pcv_lxc_snapshot_set_rollback_ops_for_test(const PcvLxcRollbackOps *ops)
{
    G_rb_ops = ops ? ops : &G_rb_real_ops;
}

                                                                              

                                                                    
typedef struct { gchar *name; gchar *snap; gboolean restart; } LxcRollbackData;

static void
_rb_data_free(gpointer p)
{
    LxcRollbackData *d = p;
    g_free(d->name);
    g_free(d->snap);
    g_free(d);
}

                                                                                                     
                                                                                            
                                                                                               
                       
                                                                                                               
                                                                                 
                                                                   
static void
_snap_rollback_thread(GTask *task, gpointer src __attribute__((unused)),
                      gpointer td, GCancellable *c __attribute__((unused)))
{
    LxcRollbackData *d = td;
    const PcvLxcRollbackOps *ops = G_rb_ops;
    if (!pcv_lxc_op_lock(d->name)) {                                                      
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY, "Container '%s' has a concurrent operation in progress", d->name);
        return;
    }
    g_autofree gchar *target = g_strdup_printf("%s/%s@%s", PCV_LXC_ZFS_BASE, d->name, d->snap);
    g_autofree gchar *fail = NULL;
    GError *error = nullptr;
    PcvLxcState st;
    if (!ops->snapshot_exists(target)) {                                             
        fail = g_strdup_printf("Snapshot '%s' not found for container '%s' (container left untouched)", d->snap, d->name);
    } else if ((st = ops->state(d->name)) == PCV_LXC_STATE_RUNNING || st == PCV_LXC_STATE_FROZEN) {                                     
        if (!ops->halt(d->name, PCV_LXC_STOP_GRACE_SEC, &error))
            fail = g_strdup_printf("container '%s' could not be stopped for the rollback: %s; nothing was rolled back",
                                   d->name, error ? error->message : "stop failed");
        else if ((st = ops->state(d->name)) != PCV_LXC_STATE_STOPPED)                                  
            fail = g_strdup_printf("container '%s' could not be stopped for the rollback: it is still %s after the stop; "
                                   "nothing was rolled back", d->name, pcv_lxc_state_name(st));
        d->restart = !fail;                                      
    } else if (st != PCV_LXC_STATE_STOPPED) {                                                             
        fail = g_strdup_printf("container '%s' is %s; nothing was rolled back (retry when it is running or stopped)",
                               d->name, pcv_lxc_state_name(st));
    }
    if (!fail && !ops->residue_clear(PCV_LXC_PATH, d->name, &error))                                                
        fail = g_strdup_printf("container '%s' has leftover volume mounts under its rootfs that could not be removed: %s; "
                               "nothing was rolled back", d->name, error ? error->message : "unknown error");
    else if (!fail && !ops->zfs_rollback(target, &error))
        fail = g_strdup_printf("snapshot rollback failed: %s", error ? error->message : "unknown");
    else if (!fail && !ops->forget(PCV_LXC_PATH, d->name))                                               
        fail = g_strdup("snapshot rolled back, but the volume fingerprint could not be cleared: "
                        "stop the container and start it with container.start");
    g_clear_error(&error);
    pcv_lxc_op_unlock(d->name);
    if (fail)
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", fail);
    else
        g_task_return_boolean(task, TRUE);
}

                                                                     
                                                                            
void
pcv_lxc_snapshot_rollback_async(const gchar *name, const gchar *snap_name, GCancellable *c, GAsyncReadyCallback cb,
                                gpointer user_data)
{
    GTask *task = pcv_drain_task_new(NULL, c, cb, user_data);
    LxcRollbackData *d = g_new0(LxcRollbackData, 1);
    d->name = g_strdup(name);
    d->snap = g_strdup(snap_name);
    g_task_set_task_data(task, d, _rb_data_free);
    g_task_run_in_thread(task, _snap_rollback_thread);
    g_object_unref(task);
}

                                                                                                           
                                                                                             
gboolean
pcv_lxc_snapshot_rollback_finish(GAsyncResult *r, gboolean *restart_out, GError **e)
{
    if (restart_out)
        *restart_out = ((LxcRollbackData *)g_task_get_task_data(G_TASK(r)))->restart;
    return g_task_propagate_boolean(G_TASK(r), e);
}
