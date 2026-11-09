   
                                  
                                       
  
                           
                                                                           
  
                                              
   
#include "snapshot_restart_policy.h"

gboolean pcv_snapshot_rollback_should_restart(PcvRollbackPath path, gboolean was_running,
                                              gboolean rollback_ok)
{
    if (!was_running)
        return FALSE;
    return path == PCV_ROLLBACK_PATH_ZFS ? TRUE : rollback_ok;
}
