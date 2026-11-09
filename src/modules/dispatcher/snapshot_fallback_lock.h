                                                                             
                                                  
                                                                       
  
                           
                                                                                                          
                                                                                        
                                                                                
  
                                                                                                    
                                                                                         
                                                                                                     
                                                                                                  
  
                                                                          
                                                                   
                                                                                
#ifndef PURECVISOR_SNAPSHOT_FALLBACK_LOCK_H
#define PURECVISOR_SNAPSHOT_FALLBACK_LOCK_H

#include <glib.h>

#include "modules/core/vm_state.h"

G_BEGIN_DECLS

#define PCV_SNAPSHOT_FALLBACK_RENEW_SEC 60

typedef struct PcvSnapshotFallbackLock PcvSnapshotFallbackLock;

   
                                                                                                     
                                                  
   
PcvSnapshotFallbackLock *pcv_snapshot_fallback_lock_begin(const gchar *vm_id, gchar **err_msg);
                                                                                          
void pcv_snapshot_fallback_lock_end(PcvSnapshotFallbackLock *lock);

                                                                    
void pcv_snapshot_fallback_lock_set_seam_for_test(gint64 (*now_fn)(void), gboolean (*renew_fn)(const PcvVmLock *lock));

typedef struct PcvSnapshotLockKeeper PcvSnapshotLockKeeper;

   
                                                                                     
                                                 
   
PcvSnapshotLockKeeper *pcv_snapshot_lock_keeper_start(PcvVmLock *lock);
                                                                                                         
void pcv_snapshot_lock_keeper_stop_and_release(PcvSnapshotLockKeeper *keeper);
                                                                                                     
void pcv_snapshot_lock_keeper_set_seam_for_test(guint interval_ms, gboolean (*renew_fn)(const PcvVmLock *lock));

G_END_DECLS

#endif                                          
