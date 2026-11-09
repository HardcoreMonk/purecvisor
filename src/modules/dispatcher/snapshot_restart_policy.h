   
                                  
                                    
  
                           
                                                                    
                                                              
                                                                   
  
                                       
   
#ifndef PURECVISOR_SNAPSHOT_RESTART_POLICY_H
#define PURECVISOR_SNAPSHOT_RESTART_POLICY_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    PCV_ROLLBACK_PATH_LIBVIRT,                                    
    PCV_ROLLBACK_PATH_ZFS                               
} PcvRollbackPath;

   
                                                                        
                                                          
                                 
   
gboolean pcv_snapshot_rollback_should_restart(PcvRollbackPath path, gboolean was_running,
                                              gboolean rollback_ok);

G_END_DECLS

#endif                                           
