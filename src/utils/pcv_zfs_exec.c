                                                                             
                           
                                                                            
  
                           
                                                   
                                                    
                                        
  
                                                                    
                                                                 
                                                           
                                                     
                                                                        
  
                                                           
                                  
                                                                                

#include "pcv_zfs_exec.h"
#include <gio/gio.h>
#include <string.h>
#include "pcv_log.h"
#include "pcv_spawn.h"
#include "pcv_zfs_lock.h"

static gint G_lock_wait_ms = 0;                               

                                                           
                                                        
static gint
_zfs_lock_wait_ms(const gchar *target, gint wait_ms)
{
    if (g_main_context_is_owner(g_main_context_default())) {
        PCV_LOG_WARN("zfs_exec", "zfs mutation on the main loop for '%s': not waiting for the pool lock", target);
        return 0;
    }
    const gint override_ms = g_atomic_int_get(&G_lock_wait_ms);
    return override_ms > 0 ? override_ms : wait_ms;
}

                                                                        
                                                       
static gboolean
_zfs_run_locked(const gchar *op, const gchar * const *argv, const gchar *target, gint wait_ms,
                gchar **stderr_out, GError **error)
{
    if (stderr_out)
        *stderr_out = NULL;
    if (!pcv_zfs_pool_lock(target, op, _zfs_lock_wait_ms(target, wait_ms), error))
        return FALSE;
    g_autofree gchar *captured = NULL;
    const gboolean ok = pcv_spawn_sync(argv, NULL, &captured, error);
    pcv_zfs_pool_unlock(target);
    if (stderr_out)
        *stderr_out = g_steal_pointer(&captured);
    return ok;
}

                                                               
static gboolean
_zfs_create(const gchar * const *options, const gchar *dataset, gint wait_ms,
            gchar **stderr_out, GError **error)
{
    g_return_val_if_fail(dataset && *dataset, FALSE);
    g_autoptr(GPtrArray) argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gpointer)"zfs");
    g_ptr_array_add(argv, (gpointer)"create");
    for (guint i = 0; options && options[i]; i++)
        g_ptr_array_add(argv, (gpointer)options[i]);
    g_ptr_array_add(argv, (gpointer)dataset);
    g_ptr_array_add(argv, NULL);
    return _zfs_run_locked("create", (const gchar * const *)argv->pdata, dataset, wait_ms, stderr_out, error);
}

                                                              
static gboolean
_zfs_destroy(const gchar *flag, const gchar *target, gint wait_ms, gchar **stderr_out, GError **error)
{
    g_return_val_if_fail(target && *target, FALSE);
    const gchar *with_flag[] = {"zfs", "destroy", flag, target, NULL};
    const gchar *no_flag[] = {"zfs", "destroy", target, NULL};
    return _zfs_run_locked("destroy", flag ? with_flag : no_flag, target, wait_ms, stderr_out, error);
}

                                                                            
gboolean
pcv_zfs_create_locked(const gchar * const *options, const gchar *dataset,
                      gchar **stderr_out, GError **error)
{
    return _zfs_create(options, dataset, PCV_ZFS_EXEC_LOCK_WAIT_MS, stderr_out, error);
}

                                                                           
gboolean
pcv_zfs_create_locked_short(const gchar * const *options, const gchar *dataset,
                            gchar **stderr_out, GError **error)
{
    return _zfs_create(options, dataset, PCV_ZFS_EXEC_SHORT_LOCK_WAIT_MS, stderr_out, error);
}

                                                                           
gboolean
pcv_zfs_destroy_locked(const gchar *flag, const gchar *target,
                       gchar **stderr_out, GError **error)
{
    return _zfs_destroy(flag, target, PCV_ZFS_EXEC_LOCK_WAIT_MS, stderr_out, error);
}

                                                                            
gboolean
pcv_zfs_destroy_locked_short(const gchar *flag, const gchar *target,
                             gchar **stderr_out, GError **error)
{
    return _zfs_destroy(flag, target, PCV_ZFS_EXEC_SHORT_LOCK_WAIT_MS, stderr_out, error);
}

                                                                       
gboolean
pcv_zfs_destroy_settling(const gchar *flag, const gchar *target, GError **error)
{
    static const guint retry_ms[] = {200, 400, 800, 1600};
    const gchar *udev_argv[] = {"udevadm", "settle", "--timeout=5", NULL};
    (void)pcv_spawn_sync(udev_argv, NULL, NULL, NULL);
    GError *attempt_error = NULL;
    gboolean removed = pcv_zfs_destroy_locked(flag, target, NULL, &attempt_error);
    for (guint i = 0; !removed && i < G_N_ELEMENTS(retry_ms) &&
                      !g_error_matches(attempt_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) &&
                      attempt_error && strstr(attempt_error->message, "dataset is busy"); i++) {
        g_usleep((gulong)retry_ms[i] * 1000);
        g_clear_error(&attempt_error);
        removed = pcv_zfs_destroy_locked(flag, target, NULL, &attempt_error);
    }
    if (attempt_error)
        g_propagate_error(error, attempt_error);
    return removed;
}

                                                
                                                         
                                 
void
pcv_zfs_exec_set_lock_wait_for_test(gint wait_ms)
{
    g_atomic_int_set(&G_lock_wait_ms, wait_ms);
}
