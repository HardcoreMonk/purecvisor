   
                         
                                                                                           
  
                           
                                                                 
                                                                           
                                                               
                                                                       
                                                             
                                                                               
                                                                         
                                                                            
                                                                           
                                                                 
                                                                
                                                                 
                                                                  
                                                                     
                                                         
                                                                     
                                 
  
                                                              
                                                          
                                                           
                                                             
   
#ifndef PURECVISOR_PCV_LSM_MOUNTS_H
#define PURECVISOR_PCV_LSM_MOUNTS_H
#include <glib.h>
G_BEGIN_DECLS

            
                                                       
typedef enum {
    PCV_LSM_CLASS_SUBMIT = 0,                              
    PCV_LSM_CLASS_FOREIGN_FS = 1,                                          
} PcvLsmClass;

                                                           
typedef struct PcvLsmMountSet PcvLsmMountSet;
                                                     
typedef struct PcvLsmMountTracker PcvLsmMountTracker;
                                                                      
typedef gchar *(*PcvLsmMountReader)(gpointer user_data);

                                                                   
                                                                  
                                                                         
                                                               
PcvLsmMountSet *pcv_lsm_mount_set_parse(const char *mountinfo, const char *const *prefixes);
                     
void pcv_lsm_mount_set_free(PcvLsmMountSet *set);
                                                   
                                                     
                                                          
                                          
                                                                         
                                             
PcvLsmClass pcv_lsm_mount_set_classify(const PcvLsmMountSet *set, guint32 hook,
                                       guint32 s_dev, const char *path);

                                                                                       
                                                                   
                                                                                 
PcvLsmMountTracker *pcv_lsm_mount_tracker_open(void);
                                           
void pcv_lsm_mount_tracker_free(PcvLsmMountTracker *t);
                                                                                       
                                                                     
                                                                
                                                                
                                                            
                                                          
int pcv_lsm_mount_tracker_poll(PcvLsmMountTracker *t, int ready_fd, int timeout_ms);
                                                                            
                                                                       
                                                          
                                        
PcvLsmClass pcv_lsm_mount_tracker_classify(PcvLsmMountTracker *t, guint32 hook, guint32 s_dev,
                                           const char *path, gint64 now_us);

                                                                   
#define PCV_LSM_MOUNT_REREAD_MIN_US (G_USEC_PER_SEC)

G_END_DECLS
#endif                                  
